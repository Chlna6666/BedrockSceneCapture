#include "Capture.h"
#include "Library.h"
#include "MeshUtils.h"
#include "RegistryGeometry.h"
#include "NativeObservations.h"
#include "StaticModels.h"
#include "Preview.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/client/renderer/block/TextureItem.h"
#include "mc/client/renderer/block/block_geometry/Model.h"
#include "mc/world/phys/AABB.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActorType.h"
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
#include <array>
#include <cmath>
#include <map>
#include <tuple>
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
ll::mod::NativeMod* gMod{};
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
            bool const saved=saveLibrary(task,error);
            if(gMod){
                if(!saved)
                    gMod->getLogger().error("F8 model library write failed: {}",error);
                else
                    gMod->getLogger().info(
                        "F8 model library saved: states={}, models={}, samples={}, directory={}",
                        task.registered,task.staticModels.size(),task.sampledBlocks,
                        task.directory.string());
            }
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
        if(stopping)return false;
        // Only the latest pending export matters. A writing job is never
        // interrupted (atomic manifest publication), and the new task runs
        // immediately after it on the same writer thread.
        jobs.clear();
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
Writer gWriter;

struct Session {
    LibraryData library;
    LocalPlayer* player{};
    std::vector<BlockType const*> registrationTypes;
    std::size_t typeCursor{},permutationCursor{};
    std::unordered_map<std::uint32_t,std::size_t> idToIndex;
    bool monitoringReported{};
    std::map<std::tuple<int,std::array<float,6>,std::uint32_t>,std::shared_ptr<Mesh const>> registryBoxCache;
    std::unordered_map<BlockGraphics const*,std::shared_ptr<std::vector<TextureBinding> const>> textureCache;
    std::vector<std::string> geometryModels;
    std::size_t geometryCursor{};
};
std::unique_ptr<Session> gSession;
bool wasF7{},wasF8{},wasF9{},wasMenuF8{},wasMenuF9{};
auto& log(){return gMod->getLogger();}
bool gameFocused(){
    DWORD pid{};
    GetWindowThreadProcessId(GetForegroundWindow(),&pid);
    return pid==GetCurrentProcessId();
}
void collectNativeSamples(Session& s) {
    for(auto& event:takeNativeObservations()){
        auto it=s.idToIndex.find(event.stateIds[0]);
        if(it==s.idToIndex.end())continue;
        auto& block=s.library.blocks[it->second];
        ModelSample sample;
        sample.neighborhood="native_scene";
        sample.nativeObserved=true;
        sample.neighborStateIds=event.stateIds;
        sample.meshes.emplace_back(std::make_shared<Mesh const>(std::move(event.mesh)));
        block.samples.emplace_back(std::move(sample));
        ++s.library.sampledBlocks;
    }
    for(auto& actor:takeActorModels())
        s.library.actorModels.emplace_back(StaticModel{std::move(actor.name),std::move(actor.geometryJson)});
}
void finish(bool cancelled) {
    if(!gSession)return;
    armNativeObservations(false);
    collectNativeSamples(*gSession);
    gSession->library.cancelled=cancelled;
    auto directory=gSession->library.directory;
    auto registered=gSession->library.registered;
    auto sampled=gSession->library.sampledBlocks;
    auto data=std::move(gSession->library);
    gSession.reset();
    if (!gWriter.submit(std::move(data))) {
        log().error("F8 export writer queue full; model library not submitted");
        return;
    }
    log().info("F8 registry states={}, generated geometry states={}, cancelled={}; writing {}",
               registered,sampled,cancelled,directory.string());
}
void begin(LocalPlayer* player) {
    if(gSession) finish(true);
    clearNativeObservations();
    if(!BlockGraphics::mInitialized()){
        log().warn("F8: client graphics resources are not initialized");
        return;
    }
    auto session=std::make_unique<Session>();
    session->library.directory=gMod->getDataDir()/"export"/"1.26.51.1";
    // Full native state/geometry enumeration works both in the menu and in
    // game. Never inspect ChunkSource or require loaded chunks.
    session->library.definitionOnly=true;
    session->player=player;
    // Use every registered permutation, not just the states present in loaded chunks.
    auto const& types=BlockTypeRegistry::get().mBlockLookupMap.get();
    // Expand registered permutations incrementally rather than blocking F8 startup.
    for(auto const& [name,type]:types)
        if(type)session->registrationTypes.emplace_back(type.get());
    {
        // mBlockModelAccess() is declared in SDK 26.51.9 but is not exported
        // by bedrock_runtime_api.lib (LNK2019). Enumerate only on the game
        // thread and keep model names, never pointers, across updates.
        for(auto const& [name,model]:BlockGraphics::mModels())
            if(model)session->geometryModels.emplace_back(name);
    }
    if(session->registrationTypes.empty()){
        log().error("F8: no registered block permutations found");
        return;
    }
    gWriter.start();
    log().info("F8 registry-only model capture: block types={}, no chunk I/O, output={}",
               session->registrationTypes.size(),session->library.directory.string());
    gSession=std::move(session);
    armNativeObservations(player!=nullptr);
}
void registerStates(Session& s,Clock::time_point started) {
    while(Clock::now()-started<std::chrono::milliseconds(3)){
        if(s.typeCursor>=s.registrationTypes.size())break;
        auto const* type=s.registrationTypes[s.typeCursor];
        if(!type){++s.typeCursor;s.permutationCursor=0;continue;}
        auto const& permutations=type->mBlockPermutations.get();
        if(s.permutationCursor>=permutations.size()){
            ++s.typeCursor;
            s.permutationCursor=0;
            continue;
        }
        // One permutation per budget check: a single large native type must
        // not enumerate its entire state vector on the game thread at once.
        auto const* block=permutations[s.permutationCursor++].get();
        if(!block)continue;
        BlockResource entry;
        entry.name=block->getTypeName();
        entry.stateNbtHex=hexBytes(block->mSerializationId.get().toBinaryNbt(true));
        entry.networkId=block->mNetworkId;
        entry.legacyData=block->getData();
        entry.air=block->isAir();
        entry.dynamic=requiresNeighbors(entry.name);
        auto kind=block->getBlockType().mBlockEntityType;
        entry.blockActorType=static_cast<int>(kind);
        // A block actor does not automatically mean the entire visible block
        // is rendered by a special renderer (e.g. furnaces still have a
        // standard block mesh). Only model-owning types need extra capture.
        switch(kind){
        case BlockActorType::Chest:
        case BlockActorType::EnderChest:
        case BlockActorType::Sign:
        case BlockActorType::HangingSign:
        case BlockActorType::Skull:
        case BlockActorType::Bed:
        case BlockActorType::Banner:
        case BlockActorType::ShulkerBox:
        case BlockActorType::Conduit:
        case BlockActorType::Bell:
        case BlockActorType::DecoratedPot:
        case BlockActorType::ItemFrame:
        case BlockActorType::GlowItemFrame:
            entry.blockEntity=true;
            break;
        default:
            entry.blockEntity=false;
            break;
        }
        if(auto const* gfx=BlockGraphics::getForBlock(*block)){
            entry.renderLayer=static_cast<int>(gfx->mRenderLayer);
            auto cached=s.textureCache.find(gfx);
            if(cached==s.textureCache.end()){
                // Native atlas tables belong to BlockGraphics, not to individual
                // block-state permutations. Share one immutable snapshot.
                // Capture only source atlas RECTANGLE metadata (no texture pixels).
                // Native triangle UVs are expressed in the current runtime atlas;
                // the rectangles map them back to local resource-pack texture aliases.
                auto snapshot=std::make_shared<std::vector<TextureBinding>>();
                auto const& items=gfx->mTextureItems.get();
                snapshot->reserve(items.size());
                for(std::size_t slot=0;slot<items.size();++slot){
                    TextureBinding tex;
                    tex.slot=static_cast<std::uint32_t>(slot);
                    tex.defaultName=items[slot].defaultName.get();
                    tex.carriedName=items[slot].carriedName.get();
                    auto const& atlas=items[slot].defaultItem.get();
                    tex.atlasItem=atlas.mName.get();
                    tex.additive=atlas.mIsAdditive;
                    auto const& nativeUVs=atlas.mTextureUVs.get();
                    for(std::size_t variantIndex=0;variantIndex<nativeUVs.size();++variantIndex){
                        for(std::size_t frameIndex=0;frameIndex<nativeUVs[variantIndex].size();++frameIndex){
                            auto const& uv=nativeUVs[variantIndex][frameIndex];
                            TextureUVVariant variant;
                            variant.variant=static_cast<std::uint32_t>(variantIndex);
                            variant.frame=static_cast<std::uint32_t>(frameIndex);
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
                    auto const& carried=items[slot].carriedItem.get();
                    tex.carriedAtlasItem=carried.mName.get();
                    tex.carriedAdditive=carried.mIsAdditive;
                    auto const& carriedUVs=carried.mTextureUVs.get();
                    for(std::size_t v=0;v<carriedUVs.size();++v){
                        for(std::size_t f=0;f<carriedUVs[v].size();++f){
                            auto const& uv=carriedUVs[v][f];
                            TextureUVVariant mapped;
                            mapped.variant=static_cast<std::uint32_t>(v);
                            mapped.frame=static_cast<std::uint32_t>(f);
                            mapped.atlasUv={uv._u0,uv._v0,uv._u1,uv._v1};
                            mapped.sourcePath=uv.sourceFileLocation.get().mPath.get().value;
                            mapped.tileWidth=uv._texSizeW;
                            mapped.tileHeight=uv._texSizeH;
                            mapped.sourceWidth=uv._sourceImageWidth;
                            mapped.sourceHeight=uv._sourceImageHeight;
                            mapped.pbrHandle=uv.mPBRTextureDataHandle;
                            tex.carriedVariants.emplace_back(std::move(mapped));
                        }
                    }
                    snapshot->emplace_back(std::move(tex));
                }
                cached=s.textureCache.emplace(gfx,std::move(snapshot)).first;
            }
            entry.textures=cached->second;
            entry.nativeShape=static_cast<int>(gfx->mBlockShape);
            switch(gfx->mBlockShape){
            case BlockShape::Water:
            case BlockShape::BubbleColumn:
                entry.geometryContext="fluid_neighbors";break;
            case BlockShape::Stairs:
                entry.geometryContext="stairs_corner_neighbors";break;
            case BlockShape::Fence:
            case BlockShape::FenceGate:
            case BlockShape::Wall:
            case BlockShape::MetalBars:
            case BlockShape::DoubleSideFence:
                entry.geometryContext="horizontal_connections";break;
            case BlockShape::Rail:
            case BlockShape::RedDust:
            case BlockShape::Tripwire:
                entry.geometryContext="neighbor_signals";break;
            default:
                if(entry.dynamic)entry.geometryContext="neighbor_dependent";break;
            }
            if(entry.blockEntity)entry.geometryContext="block_entity_nbt";
            if(entry.geometryContext!="none")entry.dynamic=true;
            auto const& visual=gfx->mVisualShape.get();
            entry.visualBounds={visual.min.x,visual.min.y,visual.min.z,
                                visual.max.x,visual.max.y,visual.max.z};
            // Never emit non-finite native bounds into JSON.
            for(auto& coordinate:entry.visualBounds)
                if(!std::isfinite(coordinate)) coordinate=0.f;
            // Preserve shape information even when no safe standalone mesh
            // recipe exists. Models without a verified box remain unresolved.
            auto proxy=makeRegistryVisualBox(
                visual,gfx->mBlockShape,
                static_cast<std::uint32_t>(gfx->mRenderLayer));
            if(!proxy)proxy=makeRegistryCrossPlanes(
                visual,gfx->mBlockShape,
                static_cast<std::uint32_t>(gfx->mRenderLayer));
            if(proxy && !entry.air && !entry.blockEntity){
                entry.visualBoundsAvailable=true;
                auto key=std::make_tuple(entry.nativeShape,entry.visualBounds,proxy->layer);
                auto cachedMesh=s.registryBoxCache.find(key);
                if(cachedMesh==s.registryBoxCache.end())
                    cachedMesh=s.registryBoxCache.emplace(
                        key,std::make_shared<Mesh const>(std::move(*proxy))).first;
                ModelSample sample;
                sample.neighborhood="registry_visual_bounds";
                sample.meshes.emplace_back(cachedMesh->second);
                entry.samples.emplace_back(std::move(sample));
                ++s.library.sampledBlocks;
            }
        }
        auto const index=s.library.blocks.size();
        s.idToIndex.emplace(entry.networkId,index);
        s.library.blocks.emplace_back(std::move(entry));
    }
    s.library.registered=static_cast<std::uint32_t>(s.library.blocks.size());
}
void registerNativeGeometry(Session& s,Clock::time_point start) {
    while(s.geometryCursor<s.geometryModels.size() &&
          Clock::now()-start<std::chrono::milliseconds(3)){
        // The game-thread update is the only supported place to read the
        // native registry: the SDK's model mutex symbol is not linkable.
        if(!BlockGraphics::mInitialized())return;
        auto const& name=s.geometryModels[s.geometryCursor++];
        auto const& models=BlockGraphics::mModels();
        auto it=models.find(name);
        if(it==models.end()||!it->second)continue;
        s.library.staticModels.emplace_back(StaticModel{name,serializeStaticGeometry(name,*it->second)});
    }
}
void advance(Session& s) {
    // Renderer reload can invalidate BlockGraphics and atlas pointers during
    // a multi-tick export. Abort before touching those pointers.
    if(!BlockGraphics::mInitialized()){
        log().warn("F8: native block graphics unloaded during export");
        finish(true);
        return;
    }
    auto tickStart=Clock::now();
    if(s.typeCursor<s.registrationTypes.size()) {
        registerStates(s,tickStart);
        return;
    }
    if(s.geometryCursor<s.geometryModels.size()){
        registerNativeGeometry(s,tickStart);
        return;
    }
    if(s.player){
        // Capture only native tessellation observed by Minecraft's renderer.
        // No synthetic BlockSource or chunk sweep is involved.
        collectNativeSamples(s);
        if(!s.monitoringReported){
            s.monitoringReported=true;
            log().info("F8: registered states ready; visit dynamic blocks, press F8 again to save, F9 to cancel");
        }
    }else finish(false);
}
LL_TYPE_INSTANCE_HOOK(BSCTickHook,ll::memory::HookPriority::Normal,
    LocalPlayer,&LocalPlayer::$tickWorld,void,::Tick const& tick) {
    origin(tick);
    onTick(*this);
}
LL_TYPE_INSTANCE_HOOK(BSCMenuHook,ll::memory::HookPriority::Normal,
    MinecraftGame,&MinecraftGame::$update,void) {
    origin();
    onMenuUpdate(*this);
}
} // namespace

void initialize(ll::mod::NativeMod& mod){gMod=&mod;}
bool installCapture(){return BSCTickHook::hook()==0;}
bool installMenuCapture(){return BSCMenuHook::hook()==0;}
void uninstallMenuCapture(){BSCMenuHook::unhook();}
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
        if(start){
            if(gSession && gSession->player==&player){
                log().info("F8 native scene capture stopped; committing snapshot");
                finish(false);
            }else{
                log().info("F8 registry export and native scene capture started");
                begin(&player);
            }
            return;
        }
        if(!gSession)return;
        if(gSession->player!=&player){
            finish(true);return;
        }
        advance(*gSession);
    }catch(std::exception const& e){
        log().error("F8 resource enumeration failed: {}",e.what());
        finish(true);
    }
}
void onMenuUpdate(MinecraftGame& game){
    if(!gMod)return;
    if(game.isPrimaryClientInstanceReady()){
        // gsl::not_null wraps a Bedrock::NonOwnerPointer. Its first ->
        // targets NonOwnerPointer itself, not IClientInstance.
        auto* client=game.getPrimaryClientInstance().get().get();
        if(client && client->getLocalPlayer()){
            wasMenuF8=false;wasMenuF9=false;
            return; // preserve the in-world F8 session while player is alive
        }
    }
    if(gSession && gSession->player){
        // Leaving the world stops LocalPlayer ticks. Release the old world
        // session only after confirming that there is no active local player.
        finish(true);
    }
    bool focus=gameFocused();
    bool f8=focus&&(GetAsyncKeyState(VK_F8)&0x8000);
    bool f9=focus&&(GetAsyncKeyState(VK_F9)&0x8000);
    bool start=f8&&!wasMenuF8, cancel=f9&&!wasMenuF9;
    wasMenuF8=f8;wasMenuF9=f9;
    try{
        if(cancel && gSession && gSession->player==nullptr){finish(true);return;}
        if(start){log().info("F8: definition-only export from game menu");begin(nullptr);return;}
        if(gSession && gSession->player==nullptr)advance(*gSession);
    }catch(std::exception const& e){
        log().error("F8 menu export failed: {}",e.what());
        if(gSession && gSession->player==nullptr)finish(true);
    }
}
} // namespace bsc
