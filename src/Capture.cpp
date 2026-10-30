#include "Capture.h"
#include "Format.h"
#include "MeshUtils.h"
#include "Preview.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <variant>
namespace bsc {
namespace {
using Clock=std::chrono::steady_clock;
using Job=std::variant<std::pair<std::filesystem::path,ChunkData>,FinishData>;
class Writer {
    std::mutex lock;
    std::condition_variable ready;
    std::deque<Job> jobs;
    std::thread worker;
    bool stopRequested=false;
    std::size_t pendingChunks{};
    void run(){
        for(;;){
            Job task;
            {
                std::unique_lock guard(lock);
                ready.wait(guard,[&]{return stopRequested||!jobs.empty();});
                if(jobs.empty()&&stopRequested)return;
                task=std::move(jobs.front());jobs.pop_front();
            }
            std::string err;
            if(auto* chunk=std::get_if<std::pair<std::filesystem::path,ChunkData>>(&task)){
                if(!saveChunk(chunk->first,chunk->second,err))
                    OutputDebugStringA(("BSC chunk write failed: "+err+"\n").c_str());
                std::lock_guard guard(lock);--pendingChunks;
            }else {
                auto const& end=std::get<FinishData>(task);
                if(!saveManifest(end,err))
                    OutputDebugStringA(("BSC manifest write failed: "+err+"\n").c_str());
            }
        }
    }
public:
    void start(){if(!worker.joinable()){stopRequested=false;worker=std::thread([this]{run();});}}
    bool submit(std::filesystem::path const& dir,ChunkData& c){
        std::lock_guard guard(lock);
        if(pendingChunks>=2)return false;
        jobs.emplace_back(std::make_pair(dir,std::move(c)));
        ++pendingChunks;ready.notify_one();return true;
    }
    void finish(FinishData f){std::lock_guard guard(lock);jobs.emplace_back(std::move(f));ready.notify_one();}
    void shutdown(){
        {std::lock_guard guard(lock);stopRequested=true;}
        ready.notify_all();
        if(worker.joinable())worker.join();
        jobs.clear();pendingChunks=0;
    }
};
ll::mod::NativeMod* gMod{};
Writer gWriter;
struct Session {
    std::filesystem::path path;
    LocalPlayer* player{};
    BlockSource* region{};
    std::vector<ChunkPos> chunks;
    std::size_t next{};
    std::unique_ptr<ChunkData> current;
    std::unique_ptr<BlockTessellator> blockTess;
    std::unique_ptr<Tessellator> tess;
    std::unordered_map<Block const*,std::uint16_t> paletteCache;
    std::uint32_t cell{},queued{},skipped{};
};
std::unique_ptr<Session> gSession;
bool wasF7{},wasF8{},wasF9{};
auto& log(){return gMod->getLogger();}
bool gameFocused(){
    DWORD pid{};
    GetWindowThreadProcessId(GetForegroundWindow(),&pid);
    return pid==GetCurrentProcessId();
}
void finish(bool cancelled){
    if(!gSession)return;
    FinishData result;
    result.directory=gSession->path;
    result.requested=static_cast<std::uint32_t>(gSession->chunks.size());
    result.queued=gSession->queued;
    result.skipped=gSession->skipped+
        static_cast<std::uint32_t>(gSession->chunks.size()-gSession->next);
    result.cancelled=cancelled;
    gSession.reset();gWriter.finish(std::move(result));
    log().info("Capture {}. Output queue will finish in background writer.",cancelled?"cancelled":"finished");
}
void begin(LocalPlayer& player) {
    if(gSession)finish(true);
    auto& region=player.getDimensionBlockSource();
    auto& source=region.getChunkSource();
    std::set<std::pair<int,int>> valid;
    auto collect=[&](auto const& storage){
        for(auto const& [pos,weak]:storage)
            if(weak.lock()&&source.getAvailableChunk(pos))valid.emplace(pos.x,pos.z);
    };
    if(auto* map=source.getChunkMap()) collect(*map);
    if(valid.empty())collect(source.getStorage());
    if(valid.empty()){log().warn("No fully loaded chunks found.");return;}
    auto session=std::make_unique<Session>();
    session->path=gMod->getDataDir()/"captures"/
        ("capture_"+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
    session->player=&player;session->region=&region;
    for(auto const& [x,z]:valid)session->chunks.emplace_back(x,z);
    session->blockTess=std::make_unique<BlockTessellator>(&region);
    session->blockTess->mCachedGetBlock.get()=[&region](BlockPos const& p)->Block const&{
        return region.getBlock(p);
    };
    session->tess=std::make_unique<Tessellator>(std::weak_ptr<mce::BufferResourceService>{});
    gWriter.start();
    log().info("BSC capture {} loaded chunks, directory {}",session->chunks.size(),session->path.string());
    gSession=std::move(session);
}
bool prepare(Session& s){
    auto cp=s.chunks[s.next];
    if(!s.region->getChunkSource().getAvailableChunk(cp))return false;
    auto d=std::make_unique<ChunkData>();
    d->x=cp.x;d->z=cp.z;
    d->minY=s.region->getMinHeight();d->maxY=s.region->getMaxHeight();
    if(d->minY>=d->maxY||d->maxY-d->minY>2048)return false;
    d->cells.reserve(static_cast<std::size_t>(d->maxY-d->minY)*256);
    s.paletteCache.clear();s.cell=0;s.current=std::move(d);
    return true;
}
void blockCell(Session& s,Block const& block,BlockPos const& p,std::uint32_t index){
    auto& chunk=*s.current;
    auto it=s.paletteCache.find(&block);
    if(it==s.paletteCache.end()) {
        if(chunk.palette.size()>=65535)throw std::runtime_error("palette overflow");
        PaletteEntry entry;
        entry.name=block.getTypeName();
        entry.serializedBlockNbt=block.mSerializationId.get().toBinaryNbt(true);
        entry.networkId=block.mNetworkId.get();
        entry.legacyData=block.getData();
        if(auto* gfx=BlockGraphics::getForBlock(block)){
            auto const& uv=gfx->getTexture(0,0);
            entry.atlasUv={uv._u0.get(),uv._v0.get(),uv._u1.get(),uv._v1.get()};
        }
        std::uint16_t id=static_cast<std::uint16_t>(chunk.palette.size());
        chunk.palette.emplace_back(std::move(entry));
        it=s.paletteCache.emplace(&block,id).first;
    }
    chunk.cells.push_back(it->second);
    captureMesh(*s.region,*s.blockTess,*s.tess,block,p,index,chunk);
}
void advance(Session& s){
    if(s.next==s.chunks.size()){finish(false);return;}
    if(!s.current) {
        if(!prepare(s)){++s.skipped;++s.next;return;}
    }
    auto& c=*s.current;
    std::uint32_t volume=static_cast<std::uint32_t>((c.maxY-c.minY)*256);
    auto started=Clock::now();
    for(unsigned i=0;i<512 && s.cell<volume&&Clock::now()-started<std::chrono::milliseconds(3);++i){
        auto idx=s.cell++;
        int x=idx%16,z=(idx/16)%16,y=c.minY+static_cast<int>(idx/256);
        BlockPos p{c.x*16+x,y,c.z*16+z};
        blockCell(s,s.region->getBlock(p),p,idx);
    }
    if(s.cell==volume){
        if(!gWriter.submit(s.path,c))return;
        s.current.reset();++s.next;++s.queued;
        if(s.next%16==0||s.next==s.chunks.size())
            log().info("Captured {}/{} chunks",s.next,s.chunks.size());
        if(s.next==s.chunks.size())finish(false);
    }
}
LL_TYPE_INSTANCE_HOOK(BSCTickHook,ll::memory::HookPriority::Normal,
    LocalPlayer,&LocalPlayer::$tickWorld,void,::Tick const& tick){
    origin(tick);
    onTick(*this);
}
} // namespace
void initialize(ll::mod::NativeMod& mod){gMod=&mod;}
bool installCapture(){return BSCTickHook::hook()==0;}
void uninstallCapture(){
    BSCTickHook::unhook();
    if(gSession)finish(true);
    gWriter.shutdown();
}
void onTick(LocalPlayer& player) {
    if(!gMod)return;
    bool focus=gameFocused();
    bool f7=focus&&(GetAsyncKeyState(VK_F7)&0x8000);
    bool f8=focus&&(GetAsyncKeyState(VK_F8)&0x8000);
    bool f9=focus&&(GetAsyncKeyState(VK_F9)&0x8000);
    bool toggle=f7&&!wasF7,start=f8&&!wasF8,cancel=f9&&!wasF9;
    wasF7=f7;wasF8=f8;wasF9=f9;
    if(toggle) {
        bool on=!gPreviewEnabled.load();
        gPreviewEnabled.store(on);
        log().info("F7 triangle and vertex debug view {}",on?"on":"off");
    }
    try {
        if(cancel&&gSession){finish(true);return;}
        if(start){begin(player);return;}
        if(!gSession)return;
        if(gSession->player!=&player||gSession->region!=&player.getDimensionBlockSource()){
            finish(true);return;
        }
        advance(*gSession);
    }catch(std::exception const& e){
        log().error("Capture failed: {}",e.what());finish(true);
    }
}
}