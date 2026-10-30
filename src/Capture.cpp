#include "Capture.h"
#include "Library.h"
#include "MeshUtils.h"
#include "StaticModels.h"
#include "Preview.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/client/renderer/block/TextureItem.h"
#include "mc/client/renderer/block/block_geometry/Model.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/registry/BlockTypeRegistry.h"
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
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bsc {
namespace {
using Clock=std::chrono::steady_clock;
class Writer {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<LibraryData> jobs;
    std::thread thread;
    bool stopping{};
    void run() {
        for (;;) {
            LibraryData task;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock,[&]{return stopping || !jobs.empty();});
                if (jobs.empty() && stopping) return;
                task=std::move(jobs.front());
                jobs.pop_front();
            }
            std::string error;
            if (!saveLibrary(task,error))
                OutputDebugStringA(("BSC model library export failed: "+error+"\n").c_str());
            else
                OutputDebugStringA(("BSC model library written: "+task.directory.string()+"\n").c_str());
        }
    }
public:
    void start() {
        if (!thread.joinable()) {
            stopping=false;
            thread=std::thread([this]{run();});
        }
    }
    bool submit(LibraryData&& task) {
        std::lock_guard lock(mutex);
        if (jobs.size()>=2) return false;
        jobs.emplace_back(std::move(task));
        cv.notify_one();
        return true;
    }
    void shutdown() {
        {
            std::lock_guard lock(mutex);
            stopping=true;
        }
        cv.notify_all();
        if (thread.joinable()) thread.join();
        jobs.clear();
    }
};
ll::mod::NativeMod* gMod{};
Writer gWriter;

struct Session {
    LibraryData library;
    LocalPlayer* player{};
    BlockSource* region{};
    std::vector<Block const*> registrations;
    std::unordered_map<Block const*,std::size_t> lookup;
    std::size_t registerCursor{};
    std::vector<std::pair<std::string,BlockGeometry::Model const*>> geometryModels;
    std::size_t geometryCursor{};
    std::vector<ChunkPos> loadedChunks;
    std::size_t nextChunk{};
    std::uint32_t cell{};
    int minY{},maxY{};
    std::unique_ptr<BlockTessellator> blockTess;
    std::unique_ptr<Tessellator> tess;
};
std::unique_ptr<Session> gSession;
bool wasF7{},wasF8{},wasF9{};
auto& log(){return gMod->getLogger();}
bool gameFocused(){
    DWORD pid{};
    GetWindowThreadProcessId(GetForegroundWindow(),&pid);
    return pid==GetCurrentProcessId();
}
void finish(bool cancelled) {
    if(!gSession)return;
    gSession->library.cancelled=cancelled;
    auto remaining=gSession->loadedChunks.size()-gSession->nextChunk;
    gSession->library.skippedChunks+=static_cast<std::uint32_t>(remaining);
    auto directory=gSession->library.directory;
    auto registered=gSession->library.registered;
    auto sampled=gSession->library.sampledBlocks;
    auto data=std::move(gSession->library);
    gSession.reset();
    if (!gWriter.submit(std::move(data))) {
        log().error("F8 export writer queue full; model library not submitted");
        return;
    }
    log().info("F8 registry states={}, native samples={}, cancelled={}; writing {}",
               registered,sampled,cancelled,directory.string());
}
void begin(LocalPlayer& player) {
    if(gSession) finish(true);
    auto& region=player.getDimensionBlockSource();
    auto& source=region.getChunkSource();
    auto service=renderBufferService();
    if(service.expired()) {
        log().warn("F8 needs an initialized native renderer; enter a world and retry");
        return;
    }
    auto session=std::make_unique<Session>();
    session->library.directory=gMod->getDataDir()/"export"/"1.26.51.1";
    session->player=&player;
    session->region=&region;
    session->minY=region.getMinHeight();
    session->maxY=region.getMaxHeight();
    if (session->minY>=session->maxY || session->maxY-session->minY>2048) {
        log().error("Invalid world build height, F8 cancelled");
        return;
    }
    // Use every registered permutation, not just the states present in loaded chunks.
    auto const& types=BlockTypeRegistry::get().mBlockLookupMap.get();
    for (auto const& [name,type] : types) {
        if(!type)continue;
        type->forEachBlockPermutation([&](Block const& block) -> bool {
            session->registrations.emplace_back(&block);
            return true;
        });
    }
    for(auto const& [name,model]:BlockGraphics::mModels()){
        if(model)session->geometryModels.emplace_back(name,model.get());
    }
    if(session->registrations.empty()){
        log().error("F8: no registered block permutations found");
        return;
    }
    std::set<std::pair<int,int>> positions;
    auto collect=[&](auto const& storage){
        for(auto const& [pos,weak]:storage)
            if(weak.lock()&&source.getAvailableChunk(pos))
                positions.emplace(pos.x,pos.z);
    };
    if(auto* map=source.getChunkMap())collect(*map);
    if(positions.empty())collect(source.getStorage());
    for (auto const& [x,z]:positions)session->loadedChunks.emplace_back(x,z);
    session->blockTess=std::make_unique<BlockTessellator>(&region);
    session->blockTess->mCachedGetBlock.get()=[&region](BlockPos const& p)->Block const&{
        return region.getBlock(p);
    };
    session->tess=std::make_unique<Tessellator>(std::move(service));
    gWriter.start();
    log().info("F8 model index: states={}, sample chunks={}, output={}",
               session->registrations.size(),session->loadedChunks.size(),
               session->library.directory.string());
    gSession=std::move(session);
}
void registerStates(Session& s,Clock::time_point started) {
    while (s.registerCursor<s.registrations.size() &&
           Clock::now()-started<std::chrono::milliseconds(3)) {
        auto const* block=s.registrations[s.registerCursor++];
        if(!block)continue;
        BlockResource entry;
        entry.name=block->getTypeName();
        entry.stateNbtHex=hexBytes(block->mSerializationId.get().toBinaryNbt(true));
        entry.networkId=block->mNetworkId;
        entry.legacyData=block->getData();
        entry.air=block->isAir();
        entry.dynamic=requiresNeighbors(entry.name);
        entry.blockEntity=usesBlockEntityRenderer(entry.name);
        if(auto* gfx=BlockGraphics::getForBlock(*block)){
            entry.renderLayer=static_cast<int>(gfx->mRenderLayer);
            auto const& items=gfx->mTextureItems.get();
            entry.textures.reserve(items.size());
            for(std::size_t slot=0;slot<items.size();++slot){
                TextureBinding tex;
                tex.slot=static_cast<std::uint32_t>(slot);
                tex.defaultName=items[slot].defaultName.get();
                tex.carriedName=items[slot].carriedName.get();
                auto const& atlas=items[slot].defaultItem.get();
                tex.atlasItem=atlas.mName.get();
                // Do not use Block::getData() as an index into the atlas variant
                // array: it is a packed block-state value, not a texture index.
                // Export every native (variant, animation-frame) UV mapping.
                auto const& nativeUVs=atlas.mTextureUVs.get();
                for(std::size_t v=0;v<nativeUVs.size();++v){
                    for(std::size_t frame=0;frame<nativeUVs[v].size();++frame){
                        auto const& uv=nativeUVs[v][frame];
                        TextureUVVariant variant;
                        variant.variant=static_cast<std::uint32_t>(v);
                        variant.frame=static_cast<std::uint32_t>(frame);
                        variant.atlasUv={uv._u0,uv._v0,uv._u1,uv._v1};
                        variant.sourcePath=uv.sourceFileLocation.get().mPath.get().value;
                        variant.tileWidth=uv._texSizeW;
                        variant.tileHeight=uv._texSizeH;
                        variant.sourceWidth=uv._sourceImageWidth;
                        variant.sourceHeight=uv._sourceImageHeight;
                        variant.pbrHandle=uv.mPBRTextureDataHandle;
                        tex.variants.emplace_back(std::move(variant));
                    }
                }
                if(!tex.variants.empty()){
                    auto const& first=tex.variants.front();
                    tex.atlasUv=first.atlasUv;
                    tex.sourcePath=first.sourcePath;
                    tex.tileWidth=first.tileWidth;
                    tex.tileHeight=first.tileHeight;
                    tex.sourceWidth=first.sourceWidth;
                    tex.sourceHeight=first.sourceHeight;
                    tex.pbrHandle=first.pbrHandle;
                }
                if(slot==0)entry.atlasUv=tex.atlasUv;
                entry.textures.emplace_back(std::move(tex));
            }
        }
        auto index=s.library.blocks.size();
        s.lookup.emplace(block,index);
        s.library.blocks.emplace_back(std::move(entry));
    }
    s.library.registered=static_cast<std::uint32_t>(s.library.blocks.size());
}
void registerNativeGeometry(Session& s,Clock::time_point start) {
    while(s.geometryCursor<s.geometryModels.size() &&
          Clock::now()-start<std::chrono::milliseconds(3)) {
        auto const& [name,model]=s.geometryModels[s.geometryCursor++];
        if(!model)continue;
        s.library.staticModels.emplace_back(StaticModel{name,serializeStaticGeometry(name,*model)});
    }
}
std::string neighborhood(BlockSource& region, BlockPos const& p) {
    // 3x3x3 context is a SAMPLE signature, not an offline neighbor rule.
    std::string sig;
    for(int dy=-1;dy<=1;++dy)for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
        if (dx==0 && dy==0 && dz==0) continue;
        auto const& neighbor=region.getBlock(BlockPos{p.x+dx,p.y+dy,p.z+dz});
        sig+=neighbor.getTypeName();
        sig.push_back('#');
        sig+=std::to_string(neighbor.getData());
        sig.push_back(';');
    }
    return sig;
}
bool sampleBlock(Session& s,Block const& block,BlockPos const& pos) {
    if(block.isAir())return false;
    auto it=s.lookup.find(&block);
    if(it==s.lookup.end())return false;
    auto& entry=s.library.blocks[it->second];
    if(entry.air || entry.blockEntity)return false;
    constexpr std::size_t MaxDynamicContexts=8;
    if(entry.samples.size()>=(entry.dynamic?MaxDynamicContexts:1u))return false;
    auto context=entry.dynamic?neighborhood(*s.region,pos):std::string{"live_reference"};
    for (auto const& existing:entry.samples)
        if(existing.neighborhood==context)return false;
    ChunkData tmp;
    captureMesh(*s.region,*s.blockTess,*s.tess,block,pos,0,tmp);
    ModelSample sample;
    sample.neighborhood=std::move(context);
    sample.meshes=std::move(tmp.meshes);
    sample.missing=tmp.missingMeshes;
    if (sample.meshes.empty()) ++s.library.failedSamples;
    entry.samples.emplace_back(std::move(sample));
    ++s.library.sampledBlocks;
    return true;
}
void advance(Session& s) {
    auto tickStart=Clock::now();
    if(s.registerCursor<s.registrations.size()) {
        registerStates(s,tickStart);
        return;
    }
    if(s.geometryCursor<s.geometryModels.size()){
        registerNativeGeometry(s,tickStart);
        return;
    }
    unsigned tessellated=0;
    auto const volume=static_cast<std::uint32_t>((s.maxY-s.minY)*256);
    while(s.nextChunk<s.loadedChunks.size() &&
          Clock::now()-tickStart<std::chrono::milliseconds(3)) {
        auto cp=s.loadedChunks[s.nextChunk];
        if(s.cell==0 && !s.region->getChunkSource().getAvailableChunk(cp)){
            ++s.library.skippedChunks;
            ++s.nextChunk;
            continue;
        }
        for (unsigned i=0; i<512 && s.cell<volume &&
             tessellated<2 && Clock::now()-tickStart<std::chrono::milliseconds(3); ++i) {
            auto index=s.cell++;
            int x=index%16,z=(index/16)%16,y=s.minY+static_cast<int>(index/256);
            BlockPos pos{cp.x*16+x,y,cp.z*16+z};
            auto const& block=s.region->getBlock(pos);
            if(sampleBlock(s,block,pos)) ++tessellated;
        }
        if(s.cell<volume)break;
        s.cell=0;
        ++s.nextChunk;
        ++s.library.visitedChunks;
        if(s.nextChunk%16==0 || s.nextChunk==s.loadedChunks.size())
            log().info("F8 samples: scanned {}/{} chunks",
                       s.nextChunk,s.loadedChunks.size());
    }
    if(s.nextChunk==s.loadedChunks.size()) finish(false);
}
LL_TYPE_INSTANCE_HOOK(BSCTickHook,ll::memory::HookPriority::Normal,
    LocalPlayer,&LocalPlayer::$tickWorld,void,::Tick const& tick) {
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
    static bool tickReported=false;
    if(!tickReported){
        tickReported=true;
        log().info("BSC local-player tick hook active; game focus required for F7/F8/F9");
    }
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
        if(start){log().info("F8 global model resource export requested");begin(player);return;}
        if(!gSession)return;
        if(gSession->player!=&player ||
           gSession->region!=&player.getDimensionBlockSource()){
            finish(true);return;
        }
        advance(*gSession);
    }catch(std::exception const& e){
        log().error("F8 resource enumeration failed: {}",e.what());
        finish(true);
    }
}
} // namespace bsc
