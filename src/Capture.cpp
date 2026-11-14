#include "Capture.h"
#include "TargetVersion.h"
#include "Library.h"
#include "MeshUtils.h"
#include "RegistryGeometry.h"
#include "NativeObservations.h"
#include "RendererAudit.h"
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
#include <string_view>
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
#include <type_traits>
#include <vector>

namespace bsc {
namespace {
using Clock=std::chrono::steady_clock;
// Newer BedrockData headers can declare accessors which no longer have
// import-library exports. Resolve them at F6 time instead of hard-linking.
// Absent symbol => do not dereference unverified graphics state.
bool graphicsReady(){
    using Accessor=bool& (*)();
    static auto access=reinterpret_cast<Accessor>(
        ll::memory::SymbolView{"?mInitialized@BlockGraphics@@SAAEA_NXZ"}.resolve(true));
    // Bedrock 26.60.x can omit the exported static accessor while its
    // renderer is already live. The active render pass supplies a stronger,
    // lifetime-checked signal than a missing export.
    return (access && access()) || !renderBufferService().expired();
}
auto graphicsModels() ->
    std::remove_reference_t<decltype(BlockGraphics::mModels())> const* {
    using ModelMap=std::remove_reference_t<decltype(BlockGraphics::mModels())>;
    using Accessor=ModelMap& (*)();
    static auto access=reinterpret_cast<Accessor>(
        ll::memory::SymbolView{"?mModels@BlockGraphics@@SAAEAV?$map@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V?$unique_ptr@UModel@BlockGeometry@@U?$default_delete@UModel@BlockGeometry@@@std@@@2@U?$less@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@2@V?$allocator@U?$pair@$$CBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V?$unique_ptr@UModel@BlockGeometry@@U?$default_delete@UModel@BlockGeometry@@@std@@@2@@std@@@2@@std@@XZ"}.resolve(true));
    return access?&access():nullptr;
}
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
                    gMod->getLogger().error("F6 model library write failed: {}",error);
                else
                    gMod->getLogger().info(
                        "F6 model library saved: states={}, static_models={}, actor_models={}, geometry_samples={}, directory={}",
                        task.registered,task.staticModels.size(),task.actorModels.size(),
                        task.sampledBlocks,task.directory.string());
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
    // Strict, opt-in visual equivalence: note pitch/instrument/trigger state
    // changes audio behavior but does not change the block's geometry.
    using NoteModelKey=std::tuple<std::string,int,int,std::array<float,6>,std::vector<std::string>>;
    std::map<NoteModelKey,std::uint32_t> noteCanonicalModels;
    std::uint32_t audioOnlyStatesAliased{};
    bool audioAliasesReported{};
    bool monitoringReported{};
    bool saveRequested{};
    std::map<std::tuple<int,std::array<float,6>,std::uint32_t>,std::shared_ptr<Mesh const>> registryBoxCache;
    std::unordered_map<BlockGraphics const*,std::shared_ptr<std::vector<TextureBinding> const>> textureCache;
    std::uint64_t resourceGeneration{};
    std::vector<std::string> geometryModels;
    std::size_t geometryCursor{};
};
std::unique_ptr<Session> gSession;
// A single F6 press can precede the first renderBlockEntities pass. Once
// the overlay observes a live GPU resource service, resume automatically.
bool gPendingWorldStart{};
bool wasF7{},wasF6{},wasF9{},wasMenuF6{},wasMenuF9{};
auto& log(){return gMod->getLogger();}
bool gameFocused(){
    DWORD pid{};
    GetWindowThreadProcessId(GetForegroundWindow(),&pid);
    return pid==GetCurrentProcessId();
}

enum class ControlCommand { None, Start, Save, Cancel, Status, Preview };
/// External one-shot triggers for RDP, Moonlight, RustDesk and any host that
/// intercepts function keys. Flags contain no executable text.
ControlCommand pollControlCommand(){
    static auto lastCheck=Clock::time_point{};
    auto const now=Clock::now();
    if(now-lastCheck<std::chrono::milliseconds(150))
        return ControlCommand::None;
    lastCheck=now;
    auto const root=gMod->getDataDir()/"control";
    // One command per poll. Priority is deliberate: cancel always beats save.
    constexpr std::array<std::pair<std::string_view,ControlCommand>,5> commands{{
        {"cancel.flag",ControlCommand::Cancel},
        {"save.flag",ControlCommand::Save},
        {"start.flag",ControlCommand::Start},
        {"status.flag",ControlCommand::Status},
        {"preview.flag",ControlCommand::Preview}
    }};
    for(auto const& [name,kind]:commands){
        std::error_code error;
        auto const path=root/std::filesystem::path(name);
        if(!std::filesystem::is_regular_file(path,error))continue;
        if(error)continue;
        if(!std::filesystem::remove(path,error)){
            if(error)log().warn("BSC control command cannot be consumed: {}: {}",path.string(),error.message());
            continue;
        }
        return kind;
    }
    return ControlCommand::None;
}
void reportControlStatus(std::string_view mode){
    log().info("BSC status: mode={}, game_focused={}, F6_key_down={}, graphics_ready={}, F6_active={}, "
               "export={}, control={}, pending_world_start={}",
               mode,gameFocused(),(GetAsyncKeyState(VK_F6)&0x8000)!=0,
               graphicsReady(),gSession!=nullptr,
               (gMod->getDataDir()/"export"/TargetGameVersion).string(),
               (gMod->getDataDir()/"control").string(),gPendingWorldStart);
}
void collectNativeSamples(Session& s) {
    for(auto& event:takeNativeObservations()){
        auto it=s.idToIndex.find(event.stateIds[0]);
        if(it==s.idToIndex.end())continue;
        auto& block=s.library.blocks[it->second];
        // For fixed blocks, valid native tessellation supersedes the
        // registry's approximate bounding box and its generic full-face UV.
        // Keep proxies for dynamic blocks: a single neighbor observation
        // cannot describe every state.
        if(event.mesh.uvValid && !event.mesh.vertices.empty() &&
           !event.positionVariantCandidate &&
           !block.dynamic && !block.blockEntity){
            std::erase_if(block.samples,[](ModelSample const& old){
                return old.neighborhood=="registry_visual_bounds";
            });
        }
        ModelSample sample;
        sample.neighborhood="native_scene";
        sample.nativeObserved=true;
        sample.neighborStateIds=event.stateIds;
        sample.observedBlockPosition=event.observedBlockPosition;
        sample.positionVariantCandidate=event.positionVariantCandidate;
        sample.meshes.emplace_back(std::make_shared<Mesh const>(std::move(event.mesh)));
        block.samples.emplace_back(std::move(sample));
        ++s.library.sampledBlocks;
    }
    for(auto& actor:takeActorModels()){
        auto exists=std::find_if(s.library.actorModels.begin(),s.library.actorModels.end(),
            [&](StaticModel const& prev){return prev.name==actor.name;});
        if(exists==s.library.actorModels.end())
            s.library.actorModels.emplace_back(StaticModel{std::move(actor.name),std::move(actor.geometryJson)});
    }
}
void finish(bool cancelled) {
    if(!gSession)return;
    armNativeObservations(false);
    setCapturePreviewEnabled(false);
    collectNativeSamples(*gSession);
    gSession->library.cancelled=cancelled;
    auto const sceneStatus=nativeSceneDiagnostics();
    log().info("F6 native-scene coverage: tessellator_calls={}, eligible={}, accepted={}, distinct_block_states={}, distinct_shapes={}, rejected_quota={}, rejected_duplicate={}, rejected_lock_busy={}",
               sceneStatus.tessellatorCalls,sceneStatus.eligibleCalls,
               sceneStatus.acceptedCalls,sceneStatus.distinctBlockStates,
               sceneStatus.distinctShapes,sceneStatus.rejectedQuota,
               sceneStatus.rejectedDuplicate,sceneStatus.rejectedLockBusy);
    gSession->library.nativeCounters={
        sceneStatus.tessellatorCalls,sceneStatus.eligibleCalls,
        sceneStatus.acceptedCalls,sceneStatus.rejectedQuota,
        sceneStatus.rejectedDuplicate,sceneStatus.rejectedLockBusy,
        sceneStatus.distinctBlockStates,sceneStatus.distinctShapes
    };
    gSession->library.nativeShapeCoverage.reserve(sceneStatus.shapeSamples.size());
    for(auto const& sample:sceneStatus.shapeSamples){
        gSession->library.nativeShapeCoverage.push_back(NativeShapeCoverage{
            sample.shape,sample.acceptedMeshes,sample.missingUvMeshes
        });
        auto const audit=auditBlockShape(sample.shape);
        if(audit.name!="other")
            log().info("F6 shape {} {}: native_meshes={}, missing_uv={}, path={}",
                       sample.shape,audit.name,sample.acceptedMeshes,
                       sample.missingUvMeshes,audit.renderPath);
    }
    auto actorStatus=actorCaptureDiagnostics();
    log().info("F6 BlockActor pre-cache: models={}, renderer_generation={}",
               startupModelCount(),nativeResourceGeneration());
    log().info("F6 BlockActor direct hooks: dispatcher={}, chest={}, sign={}, hanging={}, bed={}, shulker={}, bell={}, piston={}; captured_models={}, complete_models={}, incomplete_models={}, observed_without_geometry={}, observed_incomplete_geometry={}",
               actorStatus.dispatcherHook,actorStatus.hooksInstalled[0],actorStatus.hooksInstalled[1],
               actorStatus.hooksInstalled[2],actorStatus.hooksInstalled[3],
               actorStatus.hooksInstalled[4],actorStatus.hooksInstalled[5],
               actorStatus.hooksInstalled[6],
               actorStatus.capturedModels,actorStatus.completeModels,actorStatus.incompleteModels,
               actorStatus.observedWithoutGeometry.size(),actorStatus.observedIncompleteGeometry.size());
    gSession->library.observedActorRendererMask=actorStatus.observedRendererMask;
    log().info("F6 renderer dispatcher hooks: explicit={}, implicit={}",
               actorStatus.explicitDispatcherHook,actorStatus.implicitDispatcherHook);
    log().info("F6 native dispatcher observed mask: 0x{:08x} (28 renderer IDs; observed != captured UV geometry)",
                actorStatus.observedRendererMask);
    for(std::size_t index=0;index<RendererAudit.size();++index){
        if(!(actorStatus.observedRendererMask & (std::uint32_t{1}<<index)))continue;
        auto const& entry=RendererAudit[index];
        if(entry.capturePath=="runtime_only")
            log().warn("F6 renderer observed: id={} {}, path={}; live rendering only, no complete CPU UV mesh",
                       index,entry.name,entry.capturePath);
        else
            log().info("F6 renderer observed: id={} {}, path={}; UV completeness remains per-model",
                       index,entry.name,entry.capturePath);
    }
    log().info("F6 native box construction hooks: add_box={}, load_boxes={}, compile_cubes={}, retained_parts={}, source_boxes={}",
               actorStatus.sourceAddBoxHook,actorStatus.sourceLoadBoxesHook,
               actorStatus.sourceCompileCubesHook,actorStatus.retainedSourceParts,actorStatus.retainedSourceBoxes);
    for(auto const& label:actorStatus.observedWithoutGeometry)
        log().warn("F6 BlockActor renderer observed, but no native model snapshot exists: {}",label);
    for(auto const& label:actorStatus.observedIncompleteGeometry)
        log().warn("F6 BlockActor model exists, but geometry or UV data is incomplete: {}",label);
    for(auto const& probe:actorStatus.missingModelProbes)
        log().warn("F6 BlockActor empty native model: {} startup={} runtime={} parts={} descendant_parts={} texture_meshes={} model_geometries={} cubes={} source_boxes={} poly_faces={}",
                   probe.name,probe.failedAtStartup,probe.failedAtRuntime,
                   probe.parts,probe.childParts,probe.textureMeshes,probe.modelGeometries,
                   probe.nativeCubes,probe.sourceBoxes,probe.polygonFaces);
    auto directory=gSession->library.directory;
    auto registered=gSession->library.registered;
    auto sampled=gSession->library.sampledBlocks;
    auto staticCount=gSession->library.staticModels.size();
    auto actorCount=gSession->library.actorModels.size();
    auto data=std::move(gSession->library);
    gSession.reset();
    if (!gWriter.submit(std::move(data))) {
        log().error("F6 export writer queue full; model library not submitted");
        return;
    }
    log().info("F6 registry states={}, geometry samples={}, static models={}, actor models={}, cancelled={}; writing {}",
               registered,sampled,staticCount,actorCount,cancelled,directory.string());
}
void begin(LocalPlayer* player) {
    // The initialization accessor is not exported by all Bedrock client
    // builds. Do not mistake a missing symbol for uninitialized graphics.
    // Observe a real render pass before accessing BlockGraphics or the atlas.
    if(player && !graphicsReady()){
        if(!installPreview(*gMod)){
            gPendingWorldStart=false;
            log().warn("F6: graphics readiness unavailable; render hook installation failed");
            return;
        }
        if(!graphicsReady()){
            if(!gPendingWorldStart)
                log().info("F6: waiting for first in-world render pass; capture will start automatically");
            gPendingWorldStart=true;
            return;
        }
    }
    gPendingWorldStart=false;
    if(!graphicsReady()){
        log().warn("F6: graphics resources not yet confirmed; enter a world to export native geometry");
        return;
    }
    if(gSession) finish(true);
    clearNativeObservations();
    setCapturePreviewEnabled(false);
    if(player){
        log().info("F6: installing red native-scene overlay on demand");
        if(!installPreview(*gMod))
            log().warn("F6 red overlay unavailable; model export continues");
        log().info("F6: initializing native hooks on demand (not during LeviLamina startup)");
        if(!installNativeObservations(*gMod))
            log().warn("F6: BlockTessellator hook unavailable; registry capture can still run");
    }
    auto session=std::make_unique<Session>();
    session->library.directory=gMod->getDataDir()/"export"/TargetGameVersion;
    // Full native state/geometry enumeration works both in the menu and in
    // game. Never inspect ChunkSource or require loaded chunks.
    // Menu exports are definition-only. In-world exports can also contain
    // real native scene and BlockActor model samples.
    session->library.definitionOnly=(player==nullptr);
    session->player=player;
    session->resourceGeneration=nativeResourceGeneration();
    // Use every registered permutation, not just the states present in loaded chunks.
    auto const& types=BlockTypeRegistry::get().mBlockLookupMap.get();
    // Expand registered permutations incrementally rather than blocking F6 startup.
    for(auto const& [name,type]:types)
        if(type)session->registrationTypes.emplace_back(type.get());
    // mBlockModelAccess() was declared but not exported in SDK 26.51.9.
    // Keep model names, never native pointers, across renderer updates.
    auto const* models=graphicsModels();
    if(models)
        for(auto const& [name,model]:*models)
            if(model)session->geometryModels.emplace_back(name);
    log().info("F6 native registry snapshot: block_types={}, static_geometry_definitions={}",
               session->registrationTypes.size(),session->geometryModels.size());
    if(!models)
        log().warn("F6: BlockGraphics::mModels accessor symbol unavailable; static geometry registry cannot be inspected on this game build");
    else if(session->geometryModels.empty())
        log().warn("F6: BlockGraphics model registry is accessible but contains no populated static models");
    if(session->registrationTypes.empty()){
        log().error("F6: no registered block permutations found");
        return;
    }
    gWriter.start();
    log().info("F6 registry-only model capture: block types={}, no chunk I/O, output={}",
               session->registrationTypes.size(),session->library.directory.string());
    gSession=std::move(session);
    armNativeObservations(player!=nullptr);
    setCapturePreviewEnabled(player!=nullptr);
    if(player){
        log().info("F6: reconstructing built-in CPU model definitions for missing BlockEntity geometries");
        captureBuiltInModelTemplates(*gMod);
    }
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
        case BlockActorType::Chalkboard:
        case BlockActorType::PistonArm:
        case BlockActorType::MovingBlock:
        case BlockActorType::Bed:
        case BlockActorType::Banner:
        case BlockActorType::ShulkerBox:
        case BlockActorType::Conduit:
        case BlockActorType::Bell:
        case BlockActorType::DecoratedPot:
        case BlockActorType::EnchantingTable:
        case BlockActorType::Lectern:
        case BlockActorType::Beacon:
        case BlockActorType::Cauldron:
        case BlockActorType::BrewingStand:
        case BlockActorType::CopperGolemStatue:
        case BlockActorType::ItemFrame:
        case BlockActorType::GlowItemFrame:
            entry.blockEntity=true;
            break;
        default:
            entry.blockEntity=false;
            break;
        }
        // Keep deterministic links between block-state metadata and the
        // dedicated native renderer model families. An absent capture is not
        // silently treated as a complete model.
        switch(kind){
        case BlockActorType::Chest:
        case BlockActorType::EnderChest:
            entry.actorModelKeys={"chest_normal","chest_large"};break;
        case BlockActorType::Sign:
            entry.actorModelKeys={"standing_sign"};break;
        case BlockActorType::HangingSign:
            entry.actorModelKeys={"hanging_sign"};break;
        case BlockActorType::Skull:
            // The actual skull type / player skin is held in BlockActor NBT.
            entry.actorModelKeys={"skull_skeleton","skull_wither","skull_zombie",
                "skull_player","skull_creeper","skull_dragon","skull_piglin"};
            entry.geometryContext="skull_type_and_skin_nbt";entry.dynamic=true;break;
        case BlockActorType::Banner:
            entry.actorModelKeys={"banner_base"};
            entry.geometryContext="banner_pattern_layers_nbt";entry.dynamic=true;break;
        case BlockActorType::Chalkboard:
            entry.actorModelKeys={"chalkboard_1x1","chalkboard_2x1","chalkboard_3x2"};
            entry.geometryContext="chalkboard_size_and_text_nbt";entry.dynamic=true;break;
        case BlockActorType::DecoratedPot:
            entry.actorModelKeys={"decorated_pot_base","decorated_pot_front",
                "decorated_pot_back","decorated_pot_left","decorated_pot_right"};
            entry.geometryContext="decorated_pot_sherds_nbt";entry.dynamic=true;break;
        case BlockActorType::Bed:
            entry.actorModelKeys={"bed"};break;
        case BlockActorType::ShulkerBox:
            entry.actorModelKeys={"shulker_box"};break;
        case BlockActorType::Bell:
            entry.actorModelKeys={"bell"};break;
        case BlockActorType::PistonArm:
            entry.actorModelKeys={"piston_arm"};break;
        case BlockActorType::EnchantingTable:
            entry.actorModelKeys={"enchanting_book"};
            entry.geometryContext="animated_book";entry.dynamic=true;break;
        case BlockActorType::Lectern:
            entry.actorModelKeys={"lectern_book"};
            entry.geometryContext="lectern_book";entry.dynamic=true;break;
        case BlockActorType::Beacon:
            entry.geometryContext="beacon_beam";entry.dynamic=true;break;
        case BlockActorType::Cauldron:
            entry.geometryContext="cauldron_liquid_level_and_color";
            entry.dynamic=true;break;
        case BlockActorType::BrewingStand:
            entry.geometryContext="brewing_contents";
            entry.dynamic=true;break;
        case BlockActorType::CopperGolemStatue:
            entry.actorModelKeys={"copper_golem_statue"};
            entry.geometryContext="serialized_actor_pose_and_oxidation";
            entry.dynamic=true;break;
        default:break;
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
            if(gfx->mBlockShape==BlockShape::Piston &&
               entry.actorModelKeys.empty())
                entry.actorModelKeys={"piston_arm"};
            switch(gfx->mBlockShape){
            case BlockShape::Torch:
                entry.geometryContext="torch_mount_and_native_uv";break;
            case BlockShape::Lantern:
                entry.geometryContext="lantern_hanging_and_native_uv";break;
            case BlockShape::EndRod:
                entry.geometryContext="end_rod_axis_and_native_uv";break;
            case BlockShape::LightningRod:
                entry.geometryContext="lightning_rod_axis_and_native_uv";break;
            case BlockShape::Candle:
                entry.geometryContext="candle_count_and_flame_native_uv";break;
            case BlockShape::Piston:
                entry.geometryContext="piston_state_head_and_moving_arm";break;
            case BlockShape::BlockActorRendererOnly:
                entry.geometryContext="block_actor_renderer_only";break;
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
            case BlockShape::CrossTexture:
            case BlockShape::DoublesidedCrossTexture:
            case BlockShape::DoublesidedWideCrossTexture:
            case BlockShape::Rows:
            case BlockShape::DoublesidedRows:
            case BlockShape::DoublePlant:
            case BlockShape::DoublesidedDoublePlant:
            case BlockShape::Stem:
            case BlockShape::Seagrass:
            case BlockShape::Kelp:
            case BlockShape::SweetBerryBush:
            case BlockShape::PitcherCrop:
            case BlockShape::PitcherPlant:
            case BlockShape::FlowerBed:
            case BlockShape::CaveVine:
            case BlockShape::Eyeblossom:
            case BlockShape::FireflyBush:
                // Exact growth state is still preserved in NBT; additional
                // per-position visual displacement must not be silently
                // collapsed into one supposedly complete static model.
                entry.geometryContext="plant_state_or_position";break;
            default:
                if(entry.dynamic && entry.geometryContext=="none")
                    entry.geometryContext="neighbor_dependent";
                break;
            }
            // Preserve the actual source of dynamic geometry when known:
            // cauldron liquid level, brewing contents, animated books, etc.
            // A generic BlockEntity label would hide these requirements and
            // incorrectly allow fixed-geometry coverage to be reported.
            if(entry.blockEntity && entry.geometryContext=="none")
                entry.geometryContext="block_entity_nbt";
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
        // Store every registered state for accurate NBT/network-ID lookups,
        // but only store one geometry recipe for sound-only note-block states.
        // Never alias stairs, walls, fluids or any neighbor-dependent mesh:
        // their geometry may depend on properties not present in this key.
        bool const audioOnlyNoteBlock=
            entry.name=="minecraft:noteblock" || entry.name=="minecraft:note_block" ||
            entry.name=="noteblock" || entry.name=="note_block";
        if(audioOnlyNoteBlock && entry.visualBoundsAvailable &&
           !entry.blockEntity && entry.geometryContext=="none" &&
           entry.samples.size()==1 &&
           entry.samples.front().neighborhood=="registry_visual_bounds"){
            std::vector<std::string> faceTextures;
            faceTextures.reserve(entry.textureBindings().size());
            for(auto const& item:entry.textureBindings())
                faceTextures.emplace_back(item.defaultName);
            auto const key=std::make_tuple(
                entry.name,entry.nativeShape,entry.renderLayer,
                entry.visualBounds,std::move(faceTextures));
            auto const [it,inserted]=s.noteCanonicalModels.try_emplace(key,entry.networkId);
            if(!inserted){
                entry.geometryAliasNetworkId=it->second;
                entry.hasGeometryAlias=true;
                entry.samples.clear();
                --s.library.sampledBlocks;
                ++s.audioOnlyStatesAliased;
            }
        }
        auto const index=s.library.blocks.size();
        s.idToIndex.emplace(entry.networkId,index);
        s.library.blocks.emplace_back(std::move(entry));
    }
    s.library.registered=static_cast<std::uint32_t>(s.library.blocks.size());
    // Summarize once after all registry states have been enumerated.
    if(s.typeCursor>=s.registrationTypes.size() && !s.audioAliasesReported){
        s.audioAliasesReported=true;
        if(s.audioOnlyStatesAliased)
            log().info("F6: {} audio-only note states share canonical visual meshes",s.audioOnlyStatesAliased);
    }
}
void registerNativeGeometry(Session& s,Clock::time_point start) {
    while(s.geometryCursor<s.geometryModels.size() &&
          Clock::now()-start<std::chrono::milliseconds(3)){
        // The game-thread update is the only supported place to read the
        // native registry: the SDK's model mutex symbol is not linkable.
        if(!graphicsReady())return;
        auto const& name=s.geometryModels[s.geometryCursor++];
        auto const* models=graphicsModels();
        if(!models)continue;
        auto it=models->find(name);
        if(it==models->end()||!it->second)continue;
        s.library.staticModels.emplace_back(StaticModel{name,serializeStaticGeometry(name,*it->second)});
    }
}
void advance(Session& s) {
    if(s.resourceGeneration!=nativeResourceGeneration()){
        log().warn("F6: renderer resource generation changed during export; cancelling stale model capture");
        finish(true);
        return;
    }
    // Renderer reload can invalidate BlockGraphics and atlas pointers during
    // a multi-tick export. Abort before touching those pointers.
    if(!graphicsReady()){
        log().warn("F6: native block graphics unloaded during export");
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
    if(!s.geometryModels.empty() && !s.monitoringReported)
        log().info("F6 static geometry catalog: definitions={}, serialized={}",
                   s.geometryModels.size(),s.library.staticModels.size());
    if(s.player){
        if(s.saveRequested){finish(false);return;}
        // Capture only native tessellation observed by Minecraft's renderer.
        // No synthetic BlockSource or chunk sweep is involved.
        collectNativeSamples(s);
        if(!s.monitoringReported){
            s.monitoringReported=true;
            log().info("F6: registered states ready; visit dynamic blocks, press F6 again to save, F9 to cancel");
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
    gPendingWorldStart=false;
    if(gSession)finish(true);
    gWriter.shutdown();
}
void onTick(LocalPlayer& player) {
    if(!gMod)return;
    static bool tickReported=false;
    if(!tickReported){
        tickReported=true;
        log().info("BSC local-player tick hook active; F7/F6/F9 need game focus. "
                   "Remote fallback: create start.flag/save.flag/cancel.flag/status.flag under {}",
                   (gMod->getDataDir()/"control").string());
    }
    auto const command=pollControlCommand();
    bool focus=gameFocused();
    bool f7=focus&&(GetAsyncKeyState(VK_F7)&0x8000);
    bool f6=focus&&(GetAsyncKeyState(VK_F6)&0x8000);
    bool f9=focus&&(GetAsyncKeyState(VK_F9)&0x8000);
    bool toggle=(f7&&!wasF7)||command==ControlCommand::Preview;
    bool start=(f6&&!wasF6)||command==ControlCommand::Start;
    bool save=command==ControlCommand::Save;
    bool cancel=(f9&&!wasF9)||command==ControlCommand::Cancel;
    wasF7=f7;wasF6=f6;wasF9=f9;
    if(toggle){
        bool const on=!gPreviewEnabled.load();
        if(on){
            log().info("F7: installing overlay render hook on demand");
            if(!installPreview(*gMod))
                log().error("F7: overlay render hook installation failed");
            else {
                gPreviewEnabled.store(true);
                if(!installActorWireframeObservations(*gMod))
                    log().warn("F7: BlockEntity model-part hook unavailable; ordinary block wireframe remains usable");
            }
        }else gPreviewEnabled.store(false);
        log().info("F7 triangle and vertex debug view {}",
                   gPreviewEnabled.load()?"on":"off");
    }
    try{
        if(command==ControlCommand::Status)reportControlStatus("world");
        if(cancel){
            if(gPendingWorldStart){
                gPendingWorldStart=false;
                log().info("F6: pending capture cancelled");
            }
            if(gSession)finish(true);
            else log().info("BSC cancel requested, no active F6 capture");
            return;
        }
        if(start||save){
            if(gSession && gSession->player==&player){
                if(gSession->typeCursor<gSession->registrationTypes.size() ||
                   gSession->geometryCursor<gSession->geometryModels.size()){
                    gSession->saveRequested=true;
                    log().info("F6: save requested; finishing full registry enumeration before writing");
                }else{
                    log().info("F6: native scene observation finished, committing captured models");
                    finish(false);
                }
            }else if(start){
                log().info("F6: registry and native-scene model capture started ({})",
                           command==ControlCommand::Start?"external control":"hotkey");
                begin(&player);
            }else{
                log().warn("BSC save requested without an active F6 capture");
            }
            return;
        }
        if(gPendingWorldStart && graphicsReady()){
            log().info("F6: live renderer resources observed; starting deferred capture");
            begin(&player);
        }
        if(!gSession)return;
        if(gSession->player!=&player){finish(true);return;}
        advance(*gSession);
    }catch(std::exception const& e){
        log().error("F6 resource enumeration failed: {}",e.what());
        finish(true);
    }
}
void onMenuUpdate(MinecraftGame& game){
    if(!gMod)return;
    // Also recovers models initialized before NativeMod::enable installed
    // the public renderer initializer hook.
    tryStartupModelSnapshot(game);
    if(game.isPrimaryClientInstanceReady()){
        // gsl::not_null wraps a Bedrock::NonOwnerPointer.
        auto* client=game.getPrimaryClientInstance().get().get();
        if(client && client->getLocalPlayer()){
            wasMenuF6=false;wasMenuF9=false;
            return; // World controls are polled from LocalPlayer tick.
        }
    }
    if(gSession && gSession->player)
        finish(true); // World closed.
    if(gPendingWorldStart){
        gPendingWorldStart=false;
        log().info("F6: pending in-world capture cancelled (world closed)");
    }
    auto const command=pollControlCommand();
    bool focus=gameFocused();
    bool f6=focus&&(GetAsyncKeyState(VK_F6)&0x8000);
    bool f9=focus&&(GetAsyncKeyState(VK_F9)&0x8000);
    bool start=(f6&&!wasMenuF6)||command==ControlCommand::Start;
    bool save=command==ControlCommand::Save;
    bool cancel=(f9&&!wasMenuF9)||command==ControlCommand::Cancel;
    wasMenuF6=f6;wasMenuF9=f9;
    try{
        if(command==ControlCommand::Status)reportControlStatus("menu");
        if(command==ControlCommand::Preview)
            log().warn("BSC preview requires a loaded world");
        if(cancel){
            if(gSession && !gSession->player)finish(true);
            return;
        }
        if(start){
            log().info("F6: definition-only export from menu ({})",
                       command==ControlCommand::Start?"external control":"hotkey");
            begin(nullptr);
            return;
        }
        if(save){
            if(gSession && !gSession->player){
                log().info("F6 menu export save requested; finish current registry scan first");
                gSession->saveRequested=true;
            }else log().warn("BSC save requested without an active menu export");
        }
        if(gSession && !gSession->player)advance(*gSession);
    }catch(std::exception const& e){
        log().error("F6 menu export failed: {}",e.what());
        if(gSession && !gSession->player)finish(true);
    }
}

} // namespace bsc
