#include "NativeObservations.h"
#include "ModelUv.h"
#include "RendererAudit.h"
#include "NativeCoordinates.h"
#include "Preview.h"
#include "ActorWireframe.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/block/actor/component/IVanillaRenderBlockActorComponent.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"
#include "mc/deps/minecraft_renderer/framebuilder/dragon/RenderMetadata.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockShape.h"
#include "mc/client/model/models/Model.h"
#include "mc/client/model/models/ChestModel.h"
#include "mc/client/model/models/BellModel.h"
#include "mc/client/model/models/BedModel.h"
#include "mc/client/model/models/PistonArmModel.h"
#include "mc/client/model/models/EnchantingBookModel.h"
#include "mc/client/model/Geometry.h"
#include "mc/client/model/geom/ModelPart.h"
#include "mc/client/model/geom/Cube.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/blockactor/ChestRenderer.h"
#include "mc/client/renderer/blockactor/SignRenderer.h"
#include "mc/client/renderer/blockactor/HangingSignRenderer.h"
#include "mc/client/renderer/blockactor/BedRenderer.h"
#include "mc/client/renderer/blockactor/ShulkerBoxRenderer.h"
#include "mc/client/renderer/blockactor/BellBlockActorRenderer.h"
#include "mc/client/renderer/blockactor/PistonBlockActorRenderer.h"
#include "mc/client/renderer/blockactor/BannerRenderer.h"
#include "mc/client/renderer/blockactor/DecoratedPotRenderer.h"
#include "mc/client/renderer/blockactor/ConduitRenderer.h"
#include "mc/client/renderer/blockactor/LecternRenderer.h"
#include "mc/client/renderer/blockactor/EnchantingTableRenderer.h"
#include "mc/client/renderer/blockactor/BeaconRenderer.h"
#include "mc/client/renderer/blockactor/SerializedActorRenderer.h"
#include "mc/client/renderer/blockactor/SkullBlockRenderer.h"
#include "mc/client/renderer/blockactor/ChalkboardRenderer.h"
#include "mc/client/renderer/blockactor/CopperGolemStatueRenderer.h"
#include "mc/client/model/models/SkeletonHeadModel.h"
#include "mc/client/model/models/DragonHeadModel.h"
#include "mc/client/model/models/PiglinHeadModel.h"
#include "mc/client/model/models/BannerModel.h"
#include "mc/client/model/models/ChalkboardModel.h"
#include "mc/client/model/models/SerializedActorModel.h"
#include <atomic>
#include <array>
#include <bitset>
#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <vector>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
namespace bsc {
namespace {
static_assert(RendererAudit.size()==static_cast<std::size_t>(BlockActorRendererId::Count),
              "Review every BlockActorRendererId after upgrading Bedrock SDK");
static_assert(static_cast<int>(BlockShape::Count)==159 &&
              static_cast<int>(BlockShape::Torch)==2 &&
              static_cast<int>(BlockShape::Lantern)==113,
              "Review BlockShape audit after Bedrock SDK upgrade");
std::atomic_bool armed{false};
// Startup snapshots contain owned JSON only; never retain native model
// pointers across renderer reloads. DllMain and NativeMod::load do no scan.
std::atomic_bool startupCaptureActive{false};
std::atomic_bool startupCachePending{true};
std::atomic_bool rendererInitializing{false};
std::atomic<std::uint64_t> resourceGeneration{0};
std::mutex rendererSnapshotMutex;
// Bit i means the native dispatcher actually rendered renderer-id i during
// this F6 recording. A registered renderer is NOT a captured UV model.
std::atomic<std::uint32_t> observedRendererMask{0};
std::mutex mutex;
struct IdHash {
    std::size_t operator()(std::array<std::uint32_t,30> const& values) const noexcept {
        std::size_t h=0x9e3779b9u;
        for(auto v:values)h ^= static_cast<std::size_t>(v)+0x9e3779b9u+(h<<6)+(h>>2);
        return h;
    }
};
std::unordered_set<std::array<std::uint32_t,30>,IdHash> seen;
std::vector<NativeObservation> pending;
std::unordered_map<std::string,std::string> actorModels;
std::unordered_map<std::string,std::string> startupActorModels;
// Capture native box inputs at model construction, before their CPU Cube
// definitions can be compacted into GPU meshes. These are not synthetic AABBs.
struct SourceBoxes {
    bool fromGeometryResource{};
    bool fromCompiledCubes{};
    std::vector<std::string> boxes;
};
std::mutex sourceBoxesMutex;
std::unordered_map<ModelPart const*,SourceBoxes> sourceBoxes;
constexpr std::size_t MaxSourceParts=8192;
constexpr std::size_t MaxSourceBoxesPerPart=256;
constexpr std::size_t MaxDistinctScenes=12288;
// High frequency variants (walls, slabs) must not consume the whole capture.
// A per-state quota distributes native coverage across more block families.
constexpr std::size_t MaxDistinctPerShape=4096;
constexpr std::size_t MaxDistinctPerState=12;
constexpr std::size_t MaxFullBoundsBlockSamples=768;
std::size_t fullBoundsBlockSamples{};
std::unordered_map<int,std::size_t> shapesObserved;
std::unordered_map<int,std::size_t> shapesMissingUv;
std::unordered_map<std::uint32_t,std::size_t> statesObserved;
std::unordered_map<int,std::size_t> shapesRejected;
std::atomic<std::uint64_t> tessellatorCalls{0};
std::atomic<std::uint64_t> eligibleCalls{0};
std::atomic<std::uint64_t> acceptedCalls{0};
std::atomic<std::uint64_t> rejectedQuota{},rejectedDuplicate{},rejectedLockBusy{};
std::unordered_map<std::string,std::chrono::steady_clock::time_point> actorLastAttempt;
constexpr std::size_t MaxTotalVertices=2'000'000;
std::size_t recordedVertices{};
constexpr std::size_t MaxNativeVertices=8192;
bool fullBoundsCubeCandidate(BlockGraphics const& gfx){
    if(gfx.mBlockShape!=BlockShape::Block)return false;
    auto const& box=gfx.mVisualShape.get();
    return box.min.x==0.f && box.min.y==0.f && box.min.z==0.f &&
           box.max.x==1.f && box.max.y==1.f && box.max.z==1.f;
}
// BlockShape::Block and a 1x1x1 *visual bound* do NOT prove that a block
// tessellates as a complete six-face cube. Keep a separately bounded sample
// budget for those candidates, but do not exclude them from F6 entirely.
bool mayVaryByPosition(BlockShape shape){
    // Crossed plants may have deterministic per-position offsets or varying
    // orientation even with identical block state and 3x3x3 neighborhood.
    switch(shape){
    case BlockShape::CrossTexture:
    case BlockShape::DoublesidedCrossTexture:
    case BlockShape::DoublesidedWideCrossTexture:
    case BlockShape::DoublePlant:
    case BlockShape::DoublesidedDoublePlant:
    case BlockShape::Rows:
    case BlockShape::DoublesidedRows:
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
        return true;
    default:return false;
    }
}
std::string quote(std::string const& s){
    std::ostringstream o;
    o<<'"';
    constexpr char digits[]="0123456789abcdef";
    for(unsigned char c:s){
        if(c=='"'||c=='\\')o<<'\\'<<static_cast<char>(c);
        else if(c=='\n')o<<"\\n";
        else if(c<32)o<<"\\u00"<<digits[c>>4]<<digits[c&15];
        else o<<static_cast<char>(c);
    }
    return o.str()+'"';
}
void vec3(std::ostringstream& o,Vec3 const& v){
    o<<'['<<v.x<<','<<v.y<<','<<v.z<<']';
}
void vec2(std::ostringstream& o,Vec2 const& v){
    o<<'['<<v.x<<','<<v.y<<']';
}
using NativeTextureResources=std::vector<std::pair<std::string,std::string>>;
std::string actorColorPath(ActorTextureInfo const& texture){
    return texture.mResourceLocations.get().mColorLocation.get().mPath.get().value;
}
void addActorColor(NativeTextureResources& out,
                   std::string const& variant,ActorTextureInfo const& texture){
    auto const path=actorColorPath(texture);
    if(!path.empty() && path.size()<=1024)out.emplace_back(variant,path);
}
std::string modelJson(Model const& model,
                      std::vector<ModelPart const*> const& explicitParts,
                      NativeTextureResources const& resources){
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o<<std::setprecision(std::numeric_limits<float>::max_digits10);
    // Built-in block-actor models may own inline ModelParts absent from mAllParts.
    std::vector<ModelPart const*> parts;
    auto const& registered=model.mAllParts.get();
    parts.reserve(registered.size()+explicitParts.size());
    for(auto const* part:registered)if(part)parts.push_back(part);
    for(auto const* part:explicitParts)
        if(part && std::find(parts.begin(),parts.end(),part)==parts.end())
            parts.push_back(part);
    if(parts.empty())return {};
    std::unordered_map<ModelPart const*,std::size_t> indices;
    for(std::size_t i=0;i<parts.size();++i)if(parts[i])indices.emplace(parts[i],i);
    o<<"{\"schema\":1,\"source\":\"native_block_actor_model_parts\","
     "\"coordinate_space\":\"model_local\",\"animation_pose\":\"not_captured\","
     "\"texture_mapping\":\"native_variants_unresolved_per_face\","
     "\"native_texture_resources\":[";
    bool firstResource=true;
    for(auto const& [variant,resource]:resources){
        if(!firstResource)o<<',';
        firstResource=false;
        o<<"{\"variant\":"<<quote(variant)
         <<",\"color_resource\":"<<quote(resource)<<'}';
    }
    o<<"],\"parts\":[";
    bool firstPart=true;
    std::size_t cubes=0,polyFaces=0,sourceBoxCount=0,unresolvedCubes=0;
    std::size_t incompleteSourceUVs=0,incompleteNativeUVs=0;
    std::size_t invalidPolygonMeshes=0;
    for(std::size_t index=0;index<parts.size();++index){
        auto const* part=parts[index];
        if(!part)continue;
        std::size_t missingPartUVs=0,completeSourceUVs=0;
        std::size_t completeNativeUVs=0,incompleteSourcePartUVs=0,unresolvedPartCubes=0;
        if(!firstPart)o<<',';
        firstPart=false;
        auto const parent=indices.find(part->mParent);
        o<<"{\"index\":"<<index<<",\"parent_index\":";
        if(parent!=indices.end())o<<parent->second;else o<<"null";
        o<<",\"position\":";vec3(o,part->mPos.get());
        o<<",\"offset\":";vec3(o,part->mOffset.get());
        o<<",\"rotation\":";vec3(o,part->mRot.get());
        o<<",\"scale\":";vec3(o,part->mScale.get());
        o<<",\"texture_size\":";vec2(o,part->mTexSize.get());
        o<<",\"visible\":"<<(part->mVisible?"true":"false")
         <<",\"cubes\":[";
        bool firstCube=true;
        for(auto const& cube:part->mCubes.get()){
            auto const& size=cube.mSize.get();
            // Some 26.51-era renderers held compiled Cube polygons but exposed
            // zero mSize through this SDK. Never serialize them as real boxes.
            if(!std::isfinite(size.x)||!std::isfinite(size.y)||!std::isfinite(size.z) ||
               size.x==0.f||size.y==0.f||size.z==0.f){
                ++unresolvedPartCubes;
                continue;
            }
            if(!firstCube)o<<',';
            firstCube=false;
            ++cubes;
            o<<"{\"origin\":";vec3(o,cube.mOrigin.get());
            o<<",\"size\":";vec3(o,cube.mSize.get());
            o<<",\"rotation\":";vec3(o,cube.mRotation.get());
            o<<",\"pivot\":";vec3(o,cube.mCubePivot.get());
            o<<",\"mirrored\":"<<(cube.mMirrored?"true":"false")<<",\"faces\":[";
            bool firstFace=true;
            auto const& faces=cube.mFaceData.get();
            bool cubeUVComplete=!faces.empty();
            std::size_t validFaces=0;
            for(std::size_t face=0;face<faces.size();++face){
                auto const& uv=faces[face].mUV.get();
                auto const& sz=faces[face].mUVSize.get();
                if(!std::isfinite(uv.x)||!std::isfinite(uv.y)||
                   !std::isfinite(sz.x)||!std::isfinite(sz.y)||
                   (faces[face].mFaceValid && (sz.x==0.f||sz.y==0.f))){
                    if(faces[face].mFaceValid)cubeUVComplete=false;
                    continue;
                }
                if(faces[face].mFaceValid)++validFaces;
                if(!firstFace)o<<',';
                firstFace=false;
                o<<"{\"face\":"<<face
                 <<",\"valid\":"<<(faces[face].mFaceValid?"true":"false")
                 <<",\"uv\":";vec2(o,faces[face].mUV.get());
                o<<",\"uv_size\":";vec2(o,faces[face].mUVSize.get());
                o<<",\"material_index\":"<<faces[face].mMaterialInstanceListIndex
                 <<",\"uv_rotation\":"<<static_cast<int>(faces[face].mUvRotation)<<'}';
            }
            cubeUVComplete=cubeUVComplete && validFaces>0;
            if(!cubeUVComplete)++missingPartUVs;
            else ++completeNativeUVs;
            o<<"],\"uv_complete\":"<<(cubeUVComplete?"true":"false")<<'}';
        }
        o<<"]";
        // Read the exact model-construction box parameters if compiled GPU
        // geometry no longer exposes Cube.mSize at observation time.
        // This can recover built-in model dimensions and resource face UVs.
        {
            std::lock_guard sourceLock(sourceBoxesMutex);
            auto const it=sourceBoxes.find(part);
            if(it!=sourceBoxes.end() && !it->second.boxes.empty()){
                o<<",\"native_source_boxes\":[";
                for(std::size_t i=0;i<it->second.boxes.size();++i){
                    if(i)o<<',';
                    o<<it->second.boxes[i];
                    if(it->second.boxes[i].find("\"uv_complete\":true")!=std::string::npos)
                        ++completeSourceUVs;
                    else ++incompleteSourcePartUVs;
                }
                o<<']';
                sourceBoxCount+=it->second.boxes.size();
            }
        }
        // CPU Cubes and source boxes may represent the same boxes. A complete
        // source face layout can recover missing live Cube UV data.
        unresolvedCubes+=unresolvedPartCubes>completeSourceUVs?
            unresolvedPartCubes-completeSourceUVs:0;
        if(missingPartUVs>completeSourceUVs)
            incompleteNativeUVs+=missingPartUVs-completeSourceUVs;
        if(incompleteSourcePartUVs>completeNativeUVs)
            incompleteSourceUVs+=incompleteSourcePartUVs-completeNativeUVs;
        // Some data-driven BlockActors use an indexed polygon mesh rather
        // than procedural Cubes. Preserve the original vertex/normal/UV
        // indices and topology instead of approximating it with a box.
        auto const& poly=part->mPolyMesh.get();
        auto const& positions=poly.mPositions.get();
        auto const& normals=poly.mNormals.get();
        auto const& uvs=poly.mUVs.get();
        auto const& tris=poly.mTris.get();
        auto const& quads=poly.mQuads.get();
        bool const polygonUVComplete=!uvs.empty() &&
            std::all_of(uvs.begin(),uvs.end(),[](auto const& uv){
                return std::isfinite(uv.x)&&std::isfinite(uv.y);
            });
        if((!tris.empty()||!quads.empty()) && !polygonUVComplete)
            ++incompleteNativeUVs;
        // Never silently skip invalid polygons and still report complete
        // geometry. Non-finite native values would also corrupt JSON.
        bool valid=!positions.empty() && positions.size()<=65536 &&
                   normals.size()<=65536 && uvs.size()<=65536 &&
                   tris.size()+quads.size()<=65536 &&
                   std::all_of(positions.begin(),positions.end(),[](auto const& p){
                       return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);
                   }) &&
                   std::all_of(normals.begin(),normals.end(),[](auto const& p){
                       return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);
                   }) &&
                   std::all_of(uvs.begin(),uvs.end(),[](auto const& uv){
                       return std::isfinite(uv.x)&&std::isfinite(uv.y);
                   });
        auto validVertex=[&](Geometry::NodeVertex const& v){
            return v.mPositionIndex<positions.size() &&
                   (normals.empty()||v.mNormalIndex<normals.size()) &&
                   (uvs.empty()||v.mUVIndex<uvs.size());
        };
        if(valid){
            for(auto const& tri:tris)for(auto const& v:tri)
                if(!validVertex(v))valid=false;
            for(auto const& quad:quads)for(auto const& v:quad)
                if(!validVertex(v))valid=false;
        }
        if((!tris.empty()||!quads.empty()) && !valid)
            ++invalidPolygonMeshes;
        if(valid && (!tris.empty()||!quads.empty())){
            polyFaces+=tris.size()+quads.size();
            o<<",\"poly_mesh\":{\"positions\":[";
            for(std::size_t i=0;i<positions.size();++i){
                if(i)o<<',';
                vec3(o,positions[i]);
            }
            o<<"],\"normals\":[";
            for(std::size_t i=0;i<normals.size();++i){
                if(i)o<<',';
                vec3(o,normals[i]);
            }
            o<<"],\"uvs\":[";
            for(std::size_t i=0;i<uvs.size();++i){
                if(i)o<<',';
                vec2(o,uvs[i]);
            }
            o<<"],\"uv_complete\":"<<(polygonUVComplete?"true":"false")
             <<",\"uv_normalized\":"<<(poly.mAreUVsNormalized?"true":"false");
            auto writeFaces=[&](auto const& faces){
                o<<'[';
                for(std::size_t i=0;i<faces.size();++i){
                    if(i)o<<',';
                    o<<'[';
                    for(std::size_t j=0;j<faces[i].size();++j){
                        if(j)o<<',';
                        auto const& v=faces[i][j];
                        o<<'['<<v.mPositionIndex<<','
                         <<v.mNormalIndex<<','<<v.mUVIndex<<']';
                    }
                    o<<']';
                }
                o<<']';
            };
            o<<",\"triangles\":";writeFaces(tris);
            o<<",\"quads\":";writeFaces(quads);
            o<<'}';
        }
        o<<"}";
    }
    o<<"],\"unresolved_compiled_cubes\":"<<unresolvedCubes
     <<",\"incomplete_source_uv_boxes\":"<<incompleteSourceUVs
     <<",\"incomplete_native_uv\":"<<incompleteNativeUVs
     <<",\"invalid_polygon_meshes\":"<<invalidPolygonMeshes
     <<",\"geometry_complete\":"
     <<(unresolvedCubes==0 && incompleteSourceUVs==0 &&
        incompleteNativeUVs==0 && invalidPolygonMeshes==0?"true":"false")
     <<"}\n";
    return cubes||polyFaces||sourceBoxCount?o.str():std::string{};
}
void recordActor(char const* label,Model const& model,
                 std::vector<ModelPart const*> const& explicitParts,
                 NativeTextureResources const& resources={}){
    bool const prewarming=startupCaptureActive.load(std::memory_order_acquire);
    if(!prewarming && !armed.load(std::memory_order_relaxed))return;
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock())return;
    auto& models=prewarming?startupActorModels:actorModels;
    auto existing=models.find(label);
    auto const complete=[](std::string const& json){
        return json.find("\"geometry_complete\":true")!=std::string::npos;
    };
    if(existing!=models.end() && complete(existing->second))return;
    if(!prewarming){
        // Do not replace a complete startup model with an incomplete runtime
        // sample produced after native Cube buffers have been compacted.
        auto const cached=startupActorModels.find(label);
        if(cached!=startupActorModels.end() && complete(cached->second))return;
        auto const now=std::chrono::steady_clock::now();
        auto const it=actorLastAttempt.find(label);
        if(it!=actorLastAttempt.end() && now-it->second<std::chrono::seconds(1))return;
        actorLastAttempt[std::string(label)]=now;
    }
    auto json=modelJson(model,explicitParts,resources);
    if(json.empty())return;
    if(existing!=models.end()){
        if(!complete(json))return;
        existing->second=std::move(json);
    }else{
        models.emplace(label,std::move(json));
    }
}

void recordRendererModel(BlockActorRenderDispatcher& dispatcher,
                         BlockActorRendererId id,bool runtimeObserved){
    bool const prewarming=startupCaptureActive.load(std::memory_order_relaxed);
    if(!prewarming && !armed.load(std::memory_order_relaxed))return;
    // RendererId indexes the 28-entry native DenseEnumMap. It is not a
    // BlockActorType, and is validated before accessing the renderer.
    auto const index=static_cast<std::size_t>(id);
    auto const& renderers=dispatcher.mRenderers.get();
    if(index>=renderers.size())return;
    if(runtimeObserved)
        observedRendererMask.fetch_or(std::uint32_t{1}<<static_cast<unsigned>(id),
                                     std::memory_order_relaxed);
    auto const& renderer=renderers[id];
    if(!renderer)return;
    // Throttle the expensive model inspection per renderer family, rather
    // than per BlockActor instance. A renderer often draws hundreds of
    // instances each frame; incomplete captures retry on later seconds.
    static std::array<std::atomic<std::int64_t>,28> sampledSecond{};
    auto const second=std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto& last=sampledSecond[index];
    auto prev=last.load(std::memory_order_relaxed);
    if(!prewarming && (prev==second || !last.compare_exchange_strong(
        prev,second,std::memory_order_relaxed)))return;
    switch(id){
    case BlockActorRendererId::Skull: {
        auto const* skull=static_cast<SkullBlockRenderer const*>(renderer.get());
        // The slot order is SkullBlock::SkullType (0 skeleton, 1 wither,
        // 2 zombie, 3 char/player, 4 creeper, 5 dragon, 6 piglin).
        // Export the base geometry; custom skin/texture NBT is separate.
        constexpr std::array<char const*,7> names{
            "skull_skeleton","skull_wither","skull_zombie",
            "skull_player","skull_creeper","skull_dragon","skull_piglin"};
        auto const& generic=skull->mSkullGeometry.get();
        auto const& nativeTextures=skull->mSkullTextures;
        for(std::size_t i=0;i<generic.size();++i){
            if((i==5 && skull->mDragonHeadModel.get()) ||
               (i==6 && skull->mPiglinHeadModel.get()))continue;
            if(auto const& model=generic[i];model){
                NativeTextureResources resources;
                addActorColor(resources,"skull_type_"+std::to_string(i),
                              nativeTextures[i].get());
                recordActor(names[i],*model,{&model->mHead.get()},resources);
            }
        }
        if(auto const& dragon=skull->mDragonHeadModel.get();dragon){
            NativeTextureResources resources;
            addActorColor(resources,"skull_type_5",nativeTextures[5].get());
            recordActor("skull_dragon",*dragon,{&dragon->mHead.get(),
                &dragon->mJaw.get(),&dragon->mSnout.get()},resources);
        }
        if(auto const& piglin=skull->mPiglinHeadModel.get();piglin){
            NativeTextureResources resources;
            addActorColor(resources,"skull_type_6",nativeTextures[6].get());
            recordActor("skull_piglin",*piglin,{&piglin->mHead.get(),
                &piglin->mLeftEar.get(),&piglin->mRightEar.get()},resources);
        }
        break;
    }
    case BlockActorRendererId::Banner: {
        auto const& model=static_cast<BannerRenderer const*>(renderer.get())->mBannerModel.get();
        if(model)recordActor("banner_base",*model,{&model->mFlag.get(),
            &model->mPole.get(),&model->mLongPole.get(),&model->mBar.get()});
        break;
    }
    case BlockActorRendererId::Chalkboard: {
        auto const* chalk=static_cast<ChalkboardRenderer const*>(renderer.get());
        auto save=[&](char const* name,ChalkboardModel const& m){
            recordActor(name,m,{&m.mBoard.get(),&m.mLeftLeg.get(),&m.mRightLeg.get()});
        };
        save("chalkboard_1x1",chalk->mOneByOne.get());
        save("chalkboard_2x1",chalk->mTwoByOne.get());
        save("chalkboard_3x2",chalk->mThreeByTwo.get());
        break;
    }
    case BlockActorRendererId::DecoratedPot: {
        auto const* pot=static_cast<DecoratedPotRenderer const*>(renderer.get());
        auto const& base=pot->mPotBase.get();
        recordActor("decorated_pot_base",base,{&base.mNeckOuter.get(),
            &base.mNeckInner.get(),&base.mBottom.get(),&base.mTop.get()});
        auto face=[&](char const* name,DecoratedPotFace const& m){
            recordActor(name,m,{&m.mModel.get()});
        };
        face("decorated_pot_front",pot->mFrontFace.get());
        face("decorated_pot_back",pot->mBackFace.get());
        face("decorated_pot_left",pot->mLeftFace.get());
        face("decorated_pot_right",pot->mRightFace.get());
        break;
    }
    case BlockActorRendererId::CopperGolemStatue: {
        auto const& models=static_cast<CopperGolemStatueRenderer const*>(
            renderer.get())->mModels.get();
        // Native variants may encode different poses and oxidization states.
        // Do not collapse them into a single geometry or fabricate a mapping.
        for(std::size_t i=0;i<std::min(models.size(),std::size_t{32});++i){
            auto const& model=models[i];
            auto const& nativeParts=model.mParts.get();
            std::vector<ModelPart const*> parts;
            parts.reserve(nativeParts.size());
            for(auto const& part:nativeParts)parts.push_back(&part);
            auto const name=i==0?std::string("copper_golem_statue"):
                "copper_golem_statue_"+std::to_string(i);
            recordActor(name.c_str(),model,parts);
        }
        break;
    }
    case BlockActorRendererId::Bed: {
        auto const* bed=static_cast<BedRenderer const*>(renderer.get());
        auto const& model=bed->mBed.get();
        NativeTextureResources resources;
        for(std::size_t i=0;i<16;++i)
            addActorColor(resources,"bed_color_"+std::to_string(i),
                          bed->mTextures[i].get());
        recordActor("bed",model,{&model.mBody.get(),&model.mLeg0.get(),
            &model.mLeg1.get(),&model.mLeg2.get(),&model.mLeg3.get()},resources);
        break;
    }
    case BlockActorRendererId::ShulkerBox: {
        auto const* shulker=static_cast<ShulkerBoxRenderer const*>(renderer.get());
        auto const& model=shulker->mShulkerBoxModel.get();
        NativeTextureResources resources;
        std::size_t variant=0;
        for(auto const& [key,texture]:shulker->mTextures.get()){
            (void)key;
            addActorColor(resources,"shulker_texture_"+std::to_string(variant++),texture);
        }
        recordActor("shulker_box",model,{&model.mLid.get(),&model.mBase.get()},resources);
        break;
    }
    case BlockActorRendererId::Bell: {
        auto const* bell=static_cast<BellBlockActorRenderer const*>(renderer.get());
        auto const& model=bell->mBell.get();
        NativeTextureResources resources;
        addActorColor(resources,"bell",bell->mBellTexture.get());
        recordActor("bell",model,{&model.mBellBody.get(),&model.mBellBase.get()},resources);
        break;
    }
    case BlockActorRendererId::Enchanter: {
        auto const& model=static_cast<EnchantingTableRenderer const*>(renderer.get())->mBook.get();
        auto const& parts=model.mBook;
        recordActor("enchanting_book",model,{&parts[0].get(),&parts[1].get(),&parts[2].get(),
            &parts[3].get(),&parts[4].get(),&parts[5].get(),&parts[6].get()});
        break;
    }
    case BlockActorRendererId::Lectern: {
        auto const& model=static_cast<LecternRenderer const*>(renderer.get())->mBook.get();
        auto const& parts=model.mBook;
        recordActor("lectern_book",model,{&parts[0].get(),&parts[1].get(),&parts[2].get(),
            &parts[3].get(),&parts[4].get(),&parts[5].get(),&parts[6].get()});
        break;
    }
    default:break;
    }
}
void recordDispatchedActor(BlockActorRenderDispatcher& dispatcher,
                           IVanillaRenderBlockActorComponent& component){
    recordRendererModel(dispatcher,component.getRendererId(),true);
}
void captureExistingRendererModels(BlockActorRenderDispatcher& dispatcher){
    // Called only after the native initialization method returned or when
    // the client reports render readiness. No pointers escape this function.
    auto const& renderers=dispatcher.mRenderers.get();
    std::size_t available=0;
    for(std::size_t i=0;i<RendererAudit.size() && i<renderers.size();++i)
        if(renderers[static_cast<BlockActorRendererId>(i)])++available;
    if(available==0)return;
    startupCaptureActive.store(true,std::memory_order_release);
    for(std::size_t i=0;i<RendererAudit.size() && i<renderers.size();++i){
        if(!renderers[static_cast<BlockActorRendererId>(i)])continue;
        recordRendererModel(dispatcher,static_cast<BlockActorRendererId>(i),false);
    }
    startupCaptureActive.store(false,std::memory_order_release);
    startupCachePending.store(false,std::memory_order_release);
}

void reconstructMissingNormals(Mesh& mesh){
    if(mesh.vertices.empty())return;
    bool missing=false;
    for(auto const& v:mesh.vertices){
        float n2=v.nx*v.nx+v.ny*v.ny+v.nz*v.nz;
        if(!std::isfinite(n2)||n2<1.0e-8f){missing=true;break;}
    }
    if(!missing)return;
    struct Normal { double x{},y{},z{}; };
    std::vector<Normal> sums(mesh.vertices.size());
    auto addTriangle=[&](std::uint32_t ia,std::uint32_t ib,std::uint32_t ic){
        if(ia>=mesh.vertices.size()||ib>=mesh.vertices.size()||ic>=mesh.vertices.size())
            return;
        auto const& a=mesh.vertices[ia];
        auto const& b=mesh.vertices[ib];
        auto const& c=mesh.vertices[ic];
        double const ux=b.x-a.x,uy=b.y-a.y,uz=b.z-a.z;
        double const vx=c.x-a.x,vy=c.y-a.y,vz=c.z-a.z;
        Normal const cross{uy*vz-uz*vy,uz*vx-ux*vz,ux*vy-uy*vx};
        double const len2=cross.x*cross.x+cross.y*cross.y+cross.z*cross.z;
        if(!std::isfinite(len2)||len2<1.e-16)return;
        // Face-area weighting avoids a bias from unequal triangle areas.
        for(auto i:{ia,ib,ic}){
            sums[i].x+=cross.x;
            sums[i].y+=cross.y;
            sums[i].z+=cross.z;
        }
    };
    std::vector<std::uint32_t> sequence=mesh.indices;
    if(sequence.empty()){
        sequence.resize(mesh.vertices.size());
        for(std::size_t i=0;i<sequence.size();++i)
            sequence[i]=static_cast<std::uint32_t>(i);
    }
    switch(mesh.primitive){
    case 1:
        for(std::size_t i=0;i+3<sequence.size();i+=4){
            addTriangle(sequence[i],sequence[i+1],sequence[i+2]);
            addTriangle(sequence[i],sequence[i+2],sequence[i+3]);
        }
        break;
    case 2:
        for(std::size_t i=0;i+2<sequence.size();i+=3)
            addTriangle(sequence[i],sequence[i+1],sequence[i+2]);
        break;
    case 3:
        for(std::size_t i=2;i<sequence.size();++i)
            if(i&1)addTriangle(sequence[i-1],sequence[i-2],sequence[i]);
            else addTriangle(sequence[i-2],sequence[i-1],sequence[i]);
        break;
    default:return;
    }
    bool reconstructed=false;
    for(std::size_t i=0;i<mesh.vertices.size();++i){
        auto& v=mesh.vertices[i];
        auto const& n=sums[i];
        float const old2=v.nx*v.nx+v.ny*v.ny+v.nz*v.nz;
        if(std::isfinite(old2)&&old2>=1.e-8f)continue;
        auto const len=std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
        if(len<=1.e-8||!std::isfinite(len))continue;
        v.nx=static_cast<float>(n.x/len);
        v.ny=static_cast<float>(n.y/len);
        v.nz=static_cast<float>(n.z/len);
        reconstructed=true;
    }
    mesh.normalsReconstructed=reconstructed;
}

void captureWorld(BlockTessellator& owner,Tessellator& tess,Block const& block,BlockPos const& pos,
                  std::size_t fromPos,std::size_t fromIdx){
    if(!armed.load(std::memory_order_relaxed))return;
    tessellatorCalls.fetch_add(1,std::memory_order_relaxed);
    auto const* gfx=BlockGraphics::getForBlock(block);
    if(!gfx)return;
    bool const fullBoundsCandidate=fullBoundsCubeCandidate(*gfx);
    eligibleCalls.fetch_add(1,std::memory_order_relaxed);
    auto const shape=static_cast<int>(gfx->mBlockShape);
    // Most of the renderer's repeat calls target already-saturated states.
    // Check quotas before reading 26 neighbors and copying native vertices.
    // Never trust this preflight as authoritative: quotas are rechecked
    // under the final capture lock after the full neighborhood is read.
    {
        std::unique_lock quick(mutex,std::try_to_lock);
        if(quick.owns_lock()){
            auto const stateIt=statesObserved.find(block.mNetworkId);
            auto const shapeIt=shapesObserved.find(shape);
            if((stateIt!=statesObserved.end() &&
                stateIt->second>=(fullBoundsCandidate?4:MaxDistinctPerState)) ||
               (shapeIt!=shapesObserved.end() &&
                shapeIt->second>=MaxDistinctPerShape) ||
               (fullBoundsCandidate && fullBoundsBlockSamples>=MaxFullBoundsBlockSamples)||
               seen.size()>=MaxDistinctScenes||
               recordedVertices>=MaxTotalVertices){
                rejectedQuota.fetch_add(1,std::memory_order_relaxed);
                return;
            }
        }
    }
    auto& raw=tess.mMeshData.get();
    auto const& points=raw.mPositions.get();
    if(fromPos>points.size() || points.size()==fromPos ||
       points.size()-fromPos>MaxNativeVertices)return;
    auto const worldSpace=nativePositionsInWorldSpace(points,pos,fromPos);
    for(auto i=fromPos;i<points.size();++i)
        if(!plausibleNativeLocalVertex(points[i],pos,worldSpace))return;
    std::array<std::uint32_t,27> ids{};
    ids[0]=block.mNetworkId;
    auto* region=owner.mRegion;
    if(!region)return;
    // Diagonal and vertical neighbors affect stair corners, wall joins,
    // fluid corner heights and overlay visibility. Preserve a 3x3x3
    // signature, never assume the six cardinal blocks are sufficient.
    std::size_t next=1;
    for(int dy=-1;dy<=1;++dy)
        for(int dz=-1;dz<=1;++dz)
            for(int dx=-1;dx<=1;++dx){
                if(dx==0 && dy==0 && dz==0)continue;
                ids[next++]=region->getBlock(BlockPos{
                    pos.x+dx,pos.y+dy,pos.z+dz}).mNetworkId;
            }
    std::array<std::uint32_t,30> sceneKey{};
    std::copy(ids.begin(),ids.end(),sceneKey.begin());
    bool const positionVariant=mayVaryByPosition(gfx->mBlockShape);
    if(positionVariant){
        sceneKey[27]=static_cast<std::uint32_t>(pos.x);
        sceneKey[28]=static_cast<std::uint32_t>(pos.y);
        sceneKey[29]=static_cast<std::uint32_t>(pos.z);
    }
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock()){
        rejectedLockBusy.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    if(seen.size()>=MaxDistinctScenes||
       shapesObserved[shape]>=MaxDistinctPerShape||
       statesObserved[block.mNetworkId]>=(fullBoundsCandidate?4:MaxDistinctPerState)||
       (fullBoundsCandidate && fullBoundsBlockSamples>=MaxFullBoundsBlockSamples)||
       recordedVertices+points.size()-fromPos>MaxTotalVertices){
        rejectedQuota.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    if(!seen.emplace(sceneKey).second){
        rejectedDuplicate.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    Mesh mesh;
    mesh.primitive=static_cast<std::uint32_t>(raw.mMode);
    mesh.layer=static_cast<std::uint32_t>(gfx->mRenderLayer);
    auto const& normals=raw.mNormals.get();
    auto const& uv=raw.mTextureUVs[0].get();
    auto const& colors=raw.mColors.get();
    mesh.uvValid=uv.size()==points.size() &&
        std::all_of(uv.begin()+fromPos,uv.end(),[](auto const& value){
            return std::isfinite(value.x)&&std::isfinite(value.y);
        });
    mesh.vertices.reserve(points.size()-fromPos);
    for(auto i=fromPos;i<points.size();++i){
        auto const& p=points[i];
        Vertex v;
        v.x=p.x-(worldSpace?static_cast<float>(pos.x):0.f);
        v.y=p.y-(worldSpace?static_cast<float>(pos.y):0.f);
        v.z=p.z-(worldSpace?static_cast<float>(pos.z):0.f);
        if(normals.size()==points.size()){
            v.nx=normals[i].x;v.ny=normals[i].y;v.nz=normals[i].z;
        }
        if(mesh.uvValid){v.u=uv[i].x;v.v=uv[i].y;}
        if(colors.size()==points.size())v.abgr=colors[i];
        mesh.vertices.emplace_back(v);
    }
    auto const& indices=raw.mIndices.get();
    if(fromIdx<indices.size()){
        mesh.indices.reserve(indices.size()-fromIdx);
        for(auto i=fromIdx;i<indices.size();++i){
            if(indices[i]<fromPos || indices[i]>=points.size()){
                seen.erase(sceneKey);
                return;
            }
            mesh.indices.emplace_back(indices[i]-static_cast<std::uint32_t>(fromPos));
        }
    }
    // Only topologies supported by Library::triangulate are safe to publish.
    if(mesh.primitive<1||mesh.primitive>3){
        seen.erase(sceneKey);
        return;
    }
    reconstructMissingNormals(mesh);
    recordedVertices+=mesh.vertices.size();
    ++shapesObserved[shape];
    if(!mesh.uvValid)++shapesMissingUv[shape];
    ++statesObserved[block.mNetworkId];
    if(fullBoundsCandidate)++fullBoundsBlockSamples;
    acceptedCalls.fetch_add(1,std::memory_order_relaxed);
    addCapturePreview(pos,mesh);
    pending.push_back(NativeObservation{
        ids,std::move(mesh),{pos.x,pos.y,pos.z},positionVariant
    });
}
// Capture at construction time: numerous BlockActor models retain only a
// compiled render mesh by the time the F6 observation begins.
LL_TYPE_INSTANCE_HOOK(BSCPartAddBoxHook,ll::memory::HookPriority::Normal,
    ModelPart,ll::memory::SymbolView{"?addBox@ModelPart@@QEAAXAEBVVec3@@0_NMAEBVColor@mce@@@Z"},void,
    Vec3 const& originPos,Vec3 const& extents,bool mirror,float inflate,
    mce::Color const& color) {
    origin(originPos,extents,mirror,inflate,color);
    auto finite=[](float x){return std::isfinite(x)&&std::abs(x)<1024.f;};
    if(!finite(originPos.x)||!finite(originPos.y)||!finite(originPos.z)||
       !finite(extents.x)||!finite(extents.y)||!finite(extents.z)||
       !finite(inflate)||extents.x==0.f||extents.y==0.f||extents.z==0.f)return;
    std::unique_lock lock(sourceBoxesMutex,std::try_to_lock);
    if(!lock.owns_lock())return;
    auto it=sourceBoxes.find(this);
    if(it==sourceBoxes.end()){
        if(sourceBoxes.size()>=MaxSourceParts)return;
        it=sourceBoxes.try_emplace(this).first;
    }
    auto& dest=it->second;
    if(dest.fromGeometryResource || dest.fromCompiledCubes ||
       dest.boxes.size()>=MaxSourceBoxesPerPart)return;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out<<std::setprecision(std::numeric_limits<float>::max_digits10);
    out<<"{\"source\":\"native_modelpart_addbox\",\"origin\":";
    vec3(out,originPos);
    out<<",\"size\":";vec3(out,extents);
    out<<",\"mirror\":"<<(mirror?"true":"false")
       <<",\"inflate\":"<<inflate<<",\"uv_complete\":false}";
    dest.boxes.emplace_back(out.str());
}
// Preserve the full native Cube definition just before GPU compilation can
// discard CPU-side geometry. Unlike addBox parameters, this includes UV per
// face and the final cube pivot, mirror and rotation.
LL_TYPE_INSTANCE_HOOK(BSCPartCompileCubesHook,ll::memory::HookPriority::Normal,
    ModelPart,&ModelPart::compileCubes,void,Tessellator& tess) {
    if(armed.load(std::memory_order_relaxed) ||
       startupCaptureActive.load(std::memory_order_relaxed)){
        auto const& cubes=this->mCubes.get();
        if(!cubes.empty() && cubes.size()<=MaxSourceBoxesPerPart){
            std::unique_lock lock(sourceBoxesMutex,std::try_to_lock);
            if(lock.owns_lock()){
                auto it=sourceBoxes.find(this);
                if(it==sourceBoxes.end()){
                    if(sourceBoxes.size()<MaxSourceParts)
                        it=sourceBoxes.try_emplace(this).first;
                }
                if(it!=sourceBoxes.end() && !it->second.fromGeometryResource){
                    std::vector<std::string> compiled;
                    compiled.reserve(cubes.size());
                    auto finite=[](float v){return std::isfinite(v)&&std::abs(v)<=1024.f;};
                    for(auto const& cube:cubes){
                        auto const& originPos=cube.mOrigin.get();
                        auto const& size=cube.mSize.get();
                        auto const& rotation=cube.mRotation.get();
                        auto const& pivot=cube.mCubePivot.get();
                        if(!finite(originPos.x)||!finite(originPos.y)||!finite(originPos.z)||
                           !finite(size.x)||!finite(size.y)||!finite(size.z)||
                           !finite(rotation.x)||!finite(rotation.y)||!finite(rotation.z)||
                           !finite(pivot.x)||!finite(pivot.y)||!finite(pivot.z)||
                           size.x==0.f||size.y==0.f||size.z==0.f)continue;
                        std::ostringstream o;
                        o.imbue(std::locale::classic());
                        o<<std::setprecision(std::numeric_limits<float>::max_digits10);
                        o<<"{\"source\":\"native_modelpart_compile_cubes\",\"origin\":";
                        vec3(o,originPos);
                        o<<",\"size\":";vec3(o,size);
                        o<<",\"rotation\":";vec3(o,rotation);
                        o<<",\"pivot\":";vec3(o,pivot);
                        o<<",\"mirror\":"<<(cube.mMirrored?"true":"false")<<",\"faces\":[";
                        bool first=true;
                        auto const& faces=cube.mFaceData.get();
                        bool complete=!faces.empty();
                        std::size_t goodFaces=0;
                        for(std::size_t i=0;i<faces.size();++i){
                            auto const& uv=faces[i].mUV.get();
                            auto const& extent=faces[i].mUVSize.get();
                            if(!finite(uv.x)||!finite(uv.y)||
                               !finite(extent.x)||!finite(extent.y)||
                               (faces[i].mFaceValid &&
                                (extent.x==0.f||extent.y==0.f))){
                                complete=false;
                                continue;
                            }
                            if(faces[i].mFaceValid)++goodFaces;
                            if(!first)o<<',';
                            first=false;
                            o<<"{\"face\":"<<i<<",\"valid\":"
                             <<(faces[i].mFaceValid?"true":"false")
                             <<",\"uv\":";vec2(o,uv);
                            o<<",\"uv_size\":";vec2(o,extent);
                            o<<",\"material_index\":"<<faces[i].mMaterialInstanceListIndex
                             <<",\"uv_rotation\":"<<static_cast<int>(faces[i].mUvRotation)<<'}';
                        }
                        complete=complete && goodFaces>0;
                        o<<"],\"uv_complete\":"<<(complete?"true":"false")<<'}';
                        compiled.push_back(o.str());
                    }
                    if(!compiled.empty()){
                        it->second.fromCompiledCubes=true;
                        it->second.boxes=std::move(compiled);
                    }
                }
            }
        }
    }
    origin(tess);
}
LL_TYPE_INSTANCE_HOOK(BSCPartLoadBoxesHook,ll::memory::HookPriority::Normal,
    ModelPart,ll::memory::SymbolView{"?loadBoxes@ModelPart@@QEAAXAEBVVec3@@0AEBV?$vector@UBox@Geometry@@V?$allocator@UBox@Geometry@@@std@@@std@@0@Z"},void,
    Vec3 const& pivot,Vec3 const& offset,
    std::vector<Geometry::Box> const& boxes,Vec3 const& bindRotation) {
    origin(pivot,offset,boxes,bindRotation);
    if(boxes.empty()||boxes.size()>MaxSourceBoxesPerPart)return;
    std::unique_lock lock(sourceBoxesMutex,std::try_to_lock);
    if(!lock.owns_lock())return;
    auto it=sourceBoxes.find(this);
    if(it==sourceBoxes.end()){
        if(sourceBoxes.size()>=MaxSourceParts)return;
        it=sourceBoxes.try_emplace(this).first;
    }
    auto& dest=it->second;
    dest.fromGeometryResource=true;
    dest.boxes.clear();
    for(auto const& box:boxes){
        auto const& size=box.mSize.get();
        auto const& from=box.mFrom.get();
        auto const& rotation=box.mRotation.get();
        auto const& pivotPos=box.mPivot.get();
        auto finite=[](float n){return std::isfinite(n)&&std::abs(n)<1024.f;};
        if(!finite(size.x)||!finite(size.y)||!finite(size.z)||
           size.x==0.f||size.y==0.f||size.z==0.f||
           !finite(from.x)||!finite(from.y)||!finite(from.z)||
           !finite(rotation.x)||!finite(rotation.y)||!finite(rotation.z)||
           !finite(pivotPos.x)||!finite(pivotPos.y)||!finite(pivotPos.z))continue;
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out<<std::setprecision(std::numeric_limits<float>::max_digits10);
        out<<"{\"source\":\"native_geometry_resource_box\",\"origin\":";
        vec3(out,from);
        out<<",\"size\":";vec3(out,size);
        out<<",\"rotation\":";vec3(out,rotation);
        out<<",\"pivot\":";vec3(out,pivotPos);
        out<<",\"mirror\":"<<(box.mMirror?"true":"false")<<",\"faces\":[";
        bool const hasFaceUVs=box.mUsesFaceUVs;
        bool completeFaceUVs=hasFaceUVs;
        bool firstValidFace=true;
        std::size_t goodFaces=0;
        auto const& faces=box.mFaceUVs.get();
        if(hasFaceUVs)for(std::size_t i=0;i<faces.size();++i){
            auto const& uv=faces[i].mUV.get();
            auto const& uvSize=faces[i].mUVSize.get();
            if(!finite(uv.x)||!finite(uv.y)||!finite(uvSize.x)||!finite(uvSize.y)||
               (faces[i].mFaceValid && (uvSize.x==0.f||uvSize.y==0.f))){
                completeFaceUVs=false;
                continue;
            }
            if(faces[i].mFaceValid)++goodFaces;
            if(!firstValidFace)out<<',';
            firstValidFace=false;
            out<<"{\"face\":"<<i<<",\"valid\":"
               <<(faces[i].mFaceValid?"true":"false")<<",\"uv\":";
            vec2(out,uv);
            out<<",\"uv_size\":";vec2(out,uvSize);
            out<<",\"material_index\":"<<faces[i].mMaterialInstanceListIndex
               <<",\"uv_rotation\":"<<static_cast<int>(faces[i].mUvRotation)<<'}';
        }
        auto const& tex=box.mTex.get();
        if(!hasFaceUVs){
            auto const uvNet=expandLegacyBoxUV(
                {static_cast<float>(tex.x),static_cast<float>(tex.y)},
                {static_cast<float>(size.x),static_cast<float>(size.y),static_cast<float>(size.z)});
            if(uvNet){
                for(auto const& face:*uvNet){
                    if(!firstValidFace)out<<',';
                    firstValidFace=false;
                    out<<"{\"name\":\""<<face.name<<"\",\"valid\":true,\"uv\":["
                       <<face.uv[0]<<','<<face.uv[1]<<"],\"uv_size\":["
                       <<face.uvSize[0]<<','<<face.uvSize[1]<<"]}";
                }
                completeFaceUVs=true;
            }
        }else completeFaceUVs=completeFaceUVs && goodFaces>0;
        out<<"],\"uv_mapping\":\""
           <<(hasFaceUVs?"native_per_face":"legacy_box_unfold")
           <<"\",\"uv_space\":\"texture_pixels\",\"legacy_texture_offset\":";
        if(finite(tex.x)&&finite(tex.y))out<<'['<<tex.x<<','<<tex.y<<']';
        else out<<"null";
        out<<",\"uv_complete\":"<<(completeFaceUVs?"true":"false")<<'}';
        dest.boxes.emplace_back(out.str());
    }
}
thread_local unsigned gWorldTessellationDepth{};
struct WorldTessellationScope {
    WorldTessellationScope(){++gWorldTessellationDepth;}
    ~WorldTessellationScope(){--gWorldTessellationDepth;}
};
LL_TYPE_INSTANCE_HOOK(BSCNativeBlockHook,ll::memory::HookPriority::Normal,
    BlockTessellator,&BlockTessellator::tessellateInWorld,bool,
    Tessellator& tess,Block const& block,BlockPos const& pos,bool useCache) {
    if(!armed.load(std::memory_order_relaxed))return origin(tess,block,pos,useCache);
    auto const beforePos=tess.mMeshData.get().mPositions.get().size();
    auto const beforeIdx=tess.mMeshData.get().mIndices.get().size();
    WorldTessellationScope scope;
    bool const result=origin(tess,block,pos,useCache);
    // A nested fallback can tessellate the same block. Only take the
    // outermost completed mesh to preserve all faces and texture layers.
    if(gWorldTessellationDepth==1)
        captureWorld(*this,tess,block,pos,beforePos,beforeIdx);
    return result;
}
// The SDK also exports tessellateBlockInWorld with a face mask. Some
// render paths call it directly without going through tessellateInWorld.
// Capture that path only when it is not nested in the normal world call.
LL_TYPE_INSTANCE_HOOK(BSCNativeMaskedBlockHook,ll::memory::HookPriority::Normal,
    BlockTessellator,&BlockTessellator::tessellateBlockInWorld,bool,
    Tessellator& tess,Block const& block,BlockPos const& pos,
    std::bitset<6> const faces,AirAndSimpleBlockBits const* air){
    if(!armed.load(std::memory_order_relaxed) || gWorldTessellationDepth!=0)
        return origin(tess,block,pos,faces,air);
    auto const beforePos=tess.mMeshData.get().mPositions.get().size();
    auto const beforeIdx=tess.mMeshData.get().mIndices.get().size();
    WorldTessellationScope scope;
    bool const result=origin(tess,block,pos,faces,air);
    if(gWorldTessellationDepth==1)
        captureWorld(*this,tess,block,pos,beforePos,beforeIdx);
    return result;
}
LL_TYPE_INSTANCE_HOOK(BSCChestModelHook,ll::memory::HookPriority::Normal,
    ChestRenderer,&ChestRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    ActorWireframeScope wireScope{ctx,data};
    origin(ctx,data);
    auto const& small=this->mChestModel.get();
    auto const& large=this->mLargeChestModel.get();
    NativeTextureResources smallTextures,largeTextures;
    addActorColor(smallTextures,"normal",this->normalTex.get());
    addActorColor(smallTextures,"trapped",this->trappedTex.get());
    addActorColor(smallTextures,"ender",this->enderTex.get());
    addActorColor(largeTextures,"large",this->largeTex.get());
    addActorColor(largeTextures,"trapped_large",this->trappedLargeTex.get());
    for(std::size_t i=0;i<4;++i){
        auto const type=static_cast<CopperType>(i);
        addActorColor(smallTextures,"copper_"+std::to_string(i),
                      this->mCopperTextures.get()[type]);
        addActorColor(largeTextures,"copper_large_"+std::to_string(i),
                      this->mLargeCopperTextures.get()[type]);
    }
    recordActor("chest_normal",small,
        {&small.mLid.get(),&small.mBottom.get(),&small.mLock.get()},smallTextures);
    recordActor("chest_large",large,
        {&large.mLid.get(),&large.mBottom.get(),&large.mLock.get()},largeTextures);
}
LL_TYPE_INSTANCE_HOOK(BSCSignModelHook,ll::memory::HookPriority::Normal,
    SignRenderer,&SignRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    ActorWireframeScope wireScope{ctx,data};
    origin(ctx,data);
    auto const& model=this->mSignModel.get();
    NativeTextureResources resources;
    addActorColor(resources,"default",this->mSignTex.get());
    for(auto const& [type,texture]:this->mTextureMap.get())
        addActorColor(resources,"sign_type_"+std::to_string(static_cast<int>(type)),texture);
    recordActor("standing_sign",model,{&model.mCube.get(),&model.mCube2.get()},resources);
}
LL_TYPE_INSTANCE_HOOK(BSCHangingSignModelHook,ll::memory::HookPriority::Normal,
    HangingSignRenderer,&HangingSignRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    ActorWireframeScope wireScope{ctx,data};
    origin(ctx,data);
    auto const& model=this->mHangingSignModel.get();
    NativeTextureResources resources;
    addActorColor(resources,"default",this->mSignTex.get());
    for(auto const& [type,texture]:this->mTextureMap.get())
        addActorColor(resources,"hanging_sign_type_"+std::to_string(static_cast<int>(type)),texture);
    recordActor("hanging_sign",model,
        {&model.mPlank.get(),&model.mBoard.get(),&model.mVChain.get(),
         &model.mChainL1.get(),&model.mChainL2.get(),
         &model.mChainR1.get(),&model.mChainR2.get()},resources);
}
LL_TYPE_INSTANCE_HOOK(BSCPistonArmModelHook,ll::memory::HookPriority::Normal,
    PistonBlockActorRenderer,&PistonBlockActorRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    ActorWireframeScope wireScope{ctx,data};
    origin(ctx,data);
    auto const& model=this->mPistonArm.get();
    recordActor("piston_arm",model,{
        &model.mArmBase.get(),&model.mArm.get(),&model.mFront.get()
    });
}
// Observe the exported dispatcher instead of non-exported MCNAPI renderers.
LL_TYPE_INSTANCE_HOOK(BSCPotWireHook,ll::memory::HookPriority::Normal,
    DecoratedPotRenderer,&DecoratedPotRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data){
    ActorWireframeScope scope{ctx,data};
    origin(ctx,data);
}
// Covers all serialized actor-like BlockEntity models (including copper
// golem statues) via the shared base renderer, not by assuming their visual
// model can be recovered from the ordinary BlockTessellator.

LL_TYPE_INSTANCE_HOOK(BSCBlockActorInitializerHook,ll::memory::HookPriority::Normal,
    BlockActorRenderDispatcher,&BlockActorRenderDispatcher::initializeBlockEntityRenderers,void,
    Bedrock::NotNullNonOwnerPtr<GeometryGroup> const& geometryGroup,
    std::shared_ptr<mce::TextureGroup> textureGroup,
    BlockTessellator& blockTessellator,
    Bedrock::NotNullNonOwnerPtr<ActorResourceDefinitionGroup const> const& actorDefinitions,
    ResourcePackManager& resourcePacks,
    Bedrock::NotNullNonOwnerPtr<ResourceLoadManager> resourceLoader,
    BaseGameVersion const& baseGameVersion,
    Experiments const& experiments){
    // The hook is installed from NativeMod::enable, not in DllMain.
    // This function is the Bedrock renderer's own resource-init boundary.
    std::lock_guard rebuild(rendererSnapshotMutex);
    rendererInitializing.store(true,std::memory_order_release);
    resourceGeneration.fetch_add(1,std::memory_order_acq_rel);
    startupCachePending.store(true,std::memory_order_release);
    {
        std::lock_guard lock(mutex);
        startupActorModels.clear();
    }
    {
        std::lock_guard lock(sourceBoxesMutex);
        sourceBoxes.clear();
    }
    startupCaptureActive.store(true,std::memory_order_release);
    origin(geometryGroup,std::move(textureGroup),blockTessellator,
           actorDefinitions,resourcePacks,resourceLoader,baseGameVersion,experiments);
    // The renderer table is initialized here; snapshot its CPU models before
    // any following world render, then drop transient ModelPart address keys.
    captureExistingRendererModels(*this);
    startupCaptureActive.store(false,std::memory_order_release);
    {
        std::lock_guard lock(sourceBoxesMutex);
        sourceBoxes.clear();
    }
    rendererInitializing.store(false,std::memory_order_release);
}
using ExplicitActorRenderFn=void(BlockActorRenderDispatcher::*)(
    BaseActorRenderContext&,BlockSource&,IVanillaRenderBlockActorComponent&,
    Block const&,Vec3 const&,BlockPos const&,bool,mce::MaterialPtr const&,
    mce::ClientTexture const*,int,std::optional<dragon::RenderMetadata>);
inline const ExplicitActorRenderFn kExplicitActorRender=
    static_cast<ExplicitActorRenderFn>(&BlockActorRenderDispatcher::render);
using ImplicitActorRenderFn=void(BlockActorRenderDispatcher::*)(
    BaseActorRenderContext&,BlockSource&,IVanillaRenderBlockActorComponent&,
    Block const&,bool,mce::MaterialPtr const&,mce::ClientTexture const*,
    int,std::optional<dragon::RenderMetadata>);
inline const ImplicitActorRenderFn kImplicitActorRender=
    static_cast<ImplicitActorRenderFn>(&BlockActorRenderDispatcher::render);
// Both overloads are MCAPI. Some 26.60.x paths call the implicit overload
// without forwarding to the explicit-position overload. Hooking both avoids
// silently losing BlockActors; per-family capture is throttled and deduped.
LL_TYPE_INSTANCE_HOOK(BSCDispatcherImplicitRenderHook,ll::memory::HookPriority::Normal,
    BlockActorRenderDispatcher,kImplicitActorRender,void,
    BaseActorRenderContext& ctx,BlockSource& source,
    IVanillaRenderBlockActorComponent& component,Block const& block,bool alpha,
    mce::MaterialPtr const& material,mce::ClientTexture const* texture,
    int breaking,std::optional<dragon::RenderMetadata> metadata){
    origin(ctx,source,component,block,alpha,material,texture,breaking,metadata);
    recordDispatchedActor(*this,component);
}
LL_TYPE_INSTANCE_HOOK(BSCDispatcherRenderHook,ll::memory::HookPriority::Normal,
    BlockActorRenderDispatcher,kExplicitActorRender,void,
    BaseActorRenderContext& ctx,BlockSource& source,
    IVanillaRenderBlockActorComponent& component,Block const& block,
    Vec3 const& relative,BlockPos const& world,bool alpha,
    mce::MaterialPtr const& material,mce::ClientTexture const* texture,
    int breaking,std::optional<dragon::RenderMetadata> metadata){
    ActorWireframeScope scope{ctx,world,relative};
    origin(ctx,source,component,block,relative,world,alpha,material,texture,
           breaking,metadata);
    // Some BlockActors only render their visible model in the alpha pass.
    // Attempt capture on either pass; the per-family throttle deduplicates.
    recordDispatchedActor(*this,component);
}
bool installedPartAdd{},installedPartLoad{},installedPartCompile{};
bool installedStartupInitializer{};
bool installedWorld{},installedWorldMasked{},installedDispatch{},installedImplicitDispatch{},installedChest{},installedSign{},
     installedHanging{},installedPiston{},installedPot{};
}
void installActorRenderHooks(ll::mod::NativeMod& mod){
    auto& logger=mod.getLogger();
    auto install=[&](char const* label,bool& installed,auto patch){
        if(installed)return;
        logger.info("BlockEntity hook BEGIN: {}",label);
        auto const status=patch();
        installed=(status==0);
        if(installed)logger.info("BlockEntity hook OK: {}",label);
        else logger.warn("BlockEntity hook FAILED: {}, hook_status={}",label,status);
    };
    install("BlockActorRenderDispatcher::render(explicit position)",installedDispatch,
        []{return BSCDispatcherRenderHook::hook();});
    install("BlockActorRenderDispatcher::render(implicit position)",installedImplicitDispatch,
        []{return BSCDispatcherImplicitRenderHook::hook();});
    install("ChestRenderer::render",installedChest,
        []{return BSCChestModelHook::hook();});
    install("SignRenderer::render",installedSign,
        []{return BSCSignModelHook::hook();});
    install("HangingSignRenderer::render",installedHanging,
        []{return BSCHangingSignModelHook::hook();});



    install("PistonBlockActorRenderer::render",installedPiston,
        []{return BSCPistonArmModelHook::hook();});

    install("DecoratedPotRenderer::render",installedPot,
        []{return BSCPotWireHook::hook();});





}
bool installActorWireframeObservations(ll::mod::NativeMod& mod){
    // The old BlockActorRenderData constructor hook executed millions of
    // times while its destructor hook never fired on SDK 26.51.9. This leaked
    // nested context scopes indefinitely. Only use tightly paired RAII
    // renderer hooks; do not hook native constructor/destructor lifetimes.
    installActorRenderHooks(mod);
    bool const partHook=installActorWireframePartHook(mod);
    if(!partHook && (installedDispatch || installedImplicitDispatch))
        mod.getLogger().warn("F7/F6: ModelPart render hooks unavailable; dispatcher capture still active");
    return installedDispatch || installedImplicitDispatch || partHook;
}
bool installStartupModelCapture(ll::mod::NativeMod& mod){
    if(!installedStartupInitializer){
        auto const status=BSCBlockActorInitializerHook::hook();
        installedStartupInitializer=(status==0);
        if(installedStartupInitializer)
            mod.getLogger().info("BSC startup: BlockActor renderer initialization observer active");
        else
            mod.getLogger().warn("BSC startup: native renderer initialization observer unavailable, hook_status={}; ready-client fallback remains available",status);
    }
    // This is the only exported ModelPart construction path verified on 26.60.5.
    // Install it before models are compiled, not when F6 is first pressed.
    if(!installedPartCompile){
        auto const status=BSCPartCompileCubesHook::hook();
        installedPartCompile=(status==0);
        if(!installedPartCompile)
            mod.getLogger().warn("BSC startup: ModelPart::compileCubes hook unavailable, hook_status={}",status);
    }
    return installedStartupInitializer || installedPartCompile;
}
std::uint64_t nativeResourceGeneration(){
    return resourceGeneration.load(std::memory_order_acquire);
}
std::size_t startupModelCount(){
    std::lock_guard lock(mutex);
    return startupActorModels.size();
}
void tryStartupModelSnapshot(MinecraftGame& game){
    if(!startupCachePending.load(std::memory_order_acquire) ||
       rendererInitializing.load(std::memory_order_acquire))return;
    static auto lastAttempt=std::chrono::steady_clock::time_point{};
    auto const now=std::chrono::steady_clock::now();
    if(now-lastAttempt<std::chrono::seconds(1))return;
    lastAttempt=now;
    if(!game.isPrimaryClientInstanceReady())return;
    auto* client=game.getPrimaryClientInstance().get().get();
    if(!client || !client->isReadyToRender())return;
    std::unique_lock rebuild(rendererSnapshotMutex,std::try_to_lock);
    if(!rebuild.owns_lock() ||
       !startupCachePending.load(std::memory_order_acquire) ||
       rendererInitializing.load(std::memory_order_acquire))return;
    // Virtual IClientInstance interface: no hard link to an MCNAPI thunk.
    captureExistingRendererModels(client->getBlockEntityRenderDispatcher());
    if(!startupCachePending.load(std::memory_order_acquire)){
        std::lock_guard lock(sourceBoxesMutex);
        sourceBoxes.clear();
    }
}
bool installNativeObservations(ll::mod::NativeMod& mod){
    if(installedWorld && installedWorldMasked)return true;
    auto& logger=mod.getLogger();
    auto install=[&](char const* label,bool& installed,auto patch){
        if(installed)return;
        logger.info("F6 native hook install BEGIN: {}",label);
        auto const status=patch();
        installed=(status==0);
        if(installed)logger.info("F6 native hook install OK: {}",label);
        else logger.warn("F6 native hook install FAILED: {}, hook_status={}",label,status);
    };
    install("BlockTessellator::tessellateInWorld",installedWorld,
        []{return BSCNativeBlockHook::hook();});
    install("BlockTessellator::tessellateBlockInWorld(face-mask)",installedWorldMasked,
        []{return BSCNativeMaskedBlockHook::hook();});
    // F6 red preview and F7 amber preview consume the same live CPU geometry.
    // Hooking only renderer-specific model collectors could never produce a
    // complete F6 red BlockEntity preview.
    installActorWireframePartHook(mod);
    installActorRenderHooks(mod);
    install("ModelPart::addBox",installedPartAdd,
        []{return BSCPartAddBoxHook::hook();});
    install("ModelPart::loadBoxes",installedPartLoad,
        []{return BSCPartLoadBoxesHook::hook();});
    // This hook is already installed during NativeMod::enable. Retry only
    // if the early hook failed; never install it twice.
    install("ModelPart::compileCubes",installedPartCompile,
        []{return BSCPartCompileCubesHook::hook();});
    logger.info("F6 native hooks initialized: world={}, masked_world={}, dispatcher_explicit={}, dispatcher_implicit={}, direct_actor_hooks={}/5, source_box_hooks={}/3",
        installedWorld,installedWorldMasked,installedDispatch,installedImplicitDispatch,
        static_cast<int>(installedChest)+static_cast<int>(installedSign)+
        static_cast<int>(installedHanging)+static_cast<int>(installedPiston)+
        static_cast<int>(installedPot),
        static_cast<int>(installedPartAdd)+static_cast<int>(installedPartLoad)+
        static_cast<int>(installedPartCompile));
    return installedWorld || installedWorldMasked;
}
void captureBuiltInModelTemplates(ll::mod::NativeMod& mod){
    // The 26.60.5 client SDK has no imported native constructors for
    // ChestModel/BellModel/PistonArmModel/BedModel/EnchantingBookModel.
    // Instantiating them would also require unresolved Model vtables and
    // AppPlatformListener methods. Capture the live renderer and its
    // ModelPart hooks instead; never generate fake models from AABBs.
    mod.getLogger().info("F6: native built-in model constructors unavailable on 26.60.5; relying on live renderer observation");
}

NativeSceneDiagnostics nativeSceneDiagnostics(){
    NativeSceneDiagnostics out;
    out.tessellatorCalls=tessellatorCalls.load(std::memory_order_relaxed);
    out.eligibleCalls=eligibleCalls.load(std::memory_order_relaxed);
    out.acceptedCalls=acceptedCalls.load(std::memory_order_relaxed);
    out.rejectedQuota=rejectedQuota.load(std::memory_order_relaxed);
    out.rejectedDuplicate=rejectedDuplicate.load(std::memory_order_relaxed);
    out.rejectedLockBusy=rejectedLockBusy.load(std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    out.distinctBlockStates=statesObserved.size();
    out.distinctShapes=shapesObserved.size();
    out.shapeSamples.reserve(shapesObserved.size());
    for(auto const& [shape,count]:shapesObserved)
        out.shapeSamples.push_back(NativeShapeSample{
            shape,count,shapesMissingUv.contains(shape)?shapesMissingUv.at(shape):0
        });
    std::sort(out.shapeSamples.begin(),out.shapeSamples.end(),
              [](auto const& a,auto const& b){return a.shape<b.shape;});
    return out;
}
ActorCaptureDiagnostics actorCaptureDiagnostics(){
    ActorCaptureDiagnostics diagnostics;
    diagnostics.hooksInstalled={installedChest,installedSign,installedHanging,
                                false,false,false,installedPiston};
    diagnostics.dispatcherHook=installedDispatch || installedImplicitDispatch;
    diagnostics.explicitDispatcherHook=installedDispatch;
    diagnostics.implicitDispatcherHook=installedImplicitDispatch;
    diagnostics.observedRendererMask=observedRendererMask.load(std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    // A startup snapshot remains a captured model even if a later live
    // inspection fails to recover its CPU geometry or UV data.
    std::unordered_set<std::string> available;
    available.reserve(actorModels.size()+startupActorModels.size());
    for(auto const& [name,json]:startupActorModels)available.insert(name);
    for(auto const& [name,json]:actorModels)available.insert(name);
    diagnostics.capturedModels=available.size();
    auto complete=[](std::string const& json){
        return json.find("\"geometry_complete\":true")!=std::string::npos;
    };
    for(auto const& name:available){
        auto const runtime=actorModels.find(name);
        auto const startup=startupActorModels.find(name);
        bool const valid=(runtime!=actorModels.end() && complete(runtime->second)) ||
            (startup!=startupActorModels.end() && complete(startup->second));
        if(valid)++diagnostics.completeModels;
        else {
            ++diagnostics.incompleteModels;
            if(actorLastAttempt.contains(name))
                diagnostics.observedIncompleteGeometry.push_back(name);
        }
    }
    for(auto const& [label,lastAttempt]:actorLastAttempt)
        if(!available.contains(label))
            diagnostics.observedWithoutGeometry.push_back(label);
    std::sort(diagnostics.observedWithoutGeometry.begin(),diagnostics.observedWithoutGeometry.end());
    std::sort(diagnostics.observedIncompleteGeometry.begin(),diagnostics.observedIncompleteGeometry.end());
    diagnostics.sourceAddBoxHook=installedPartAdd;
    diagnostics.sourceLoadBoxesHook=installedPartLoad;
    diagnostics.sourceCompileCubesHook=installedPartCompile;
    {
        std::lock_guard sourceLock(sourceBoxesMutex);
        diagnostics.retainedSourceParts=sourceBoxes.size();
        for(auto const& [part,boxes]:sourceBoxes)
            diagnostics.retainedSourceBoxes+=boxes.boxes.size();
    }
    return diagnostics;
}
void armNativeObservations(bool enabled){
    armed.store(enabled,std::memory_order_release);
}
void clearNativeObservations(){
    armed.store(false,std::memory_order_release);
    std::lock_guard lock(mutex);
    seen.clear();
    observedRendererMask.store(0,std::memory_order_relaxed);
    recordedVertices=0;
    fullBoundsBlockSamples=0;
    shapesObserved.clear();
    shapesMissingUv.clear();
    statesObserved.clear();
    shapesRejected.clear();
    tessellatorCalls.store(0,std::memory_order_relaxed);
    eligibleCalls.store(0,std::memory_order_relaxed);
    acceptedCalls.store(0,std::memory_order_relaxed);
    rejectedQuota.store(0,std::memory_order_relaxed);
    rejectedDuplicate.store(0,std::memory_order_relaxed);
    rejectedLockBusy.store(0,std::memory_order_relaxed);
    actorLastAttempt.clear();
    pending.clear();
    actorModels.clear();
    // Source-box keys are raw ModelPart addresses. Never carry an address
    // from a previous F6 session into a newly constructed model instance.
    // Startup snapshots are owned JSON and survive F6 resets. Only raw
    // construction-time address keys are discarded here.
    {
        std::lock_guard sourceLock(sourceBoxesMutex);
        sourceBoxes.clear();
    }
}
std::vector<NativeObservation> takeNativeObservations(){
    std::lock_guard lock(mutex);
    std::vector<NativeObservation> out;
    out.swap(pending);
    return out;
}
std::vector<ActorModelSnapshot> takeActorModels(){
    std::lock_guard lock(mutex);
    std::vector<ActorModelSnapshot> out;
    out.reserve(actorModels.size());
    for(auto& [name,geometry]:actorModels)
        out.push_back(ActorModelSnapshot{name,std::move(geometry)});
    for(auto const& [name,geometry]:startupActorModels)
        if(!actorModels.contains(name))
            out.push_back(ActorModelSnapshot{name,geometry});
    actorModels.clear();
    return out;
}
void uninstallNativeObservations(){
    uninstallActorWireframePartHook();
    if(installedStartupInitializer){
        BSCBlockActorInitializerHook::unhook();
        installedStartupInitializer=false;
    }
    startupCaptureActive.store(false,std::memory_order_release);
    startupCachePending.store(false,std::memory_order_release);
    {
        std::lock_guard lock(mutex);
        startupActorModels.clear();
    }
    armed.store(false,std::memory_order_release);
    if(installedPartCompile){BSCPartCompileCubesHook::unhook();installedPartCompile=false;}
    if(installedPartLoad){BSCPartLoadBoxesHook::unhook();installedPartLoad=false;}
    if(installedPartAdd){BSCPartAddBoxHook::unhook();installedPartAdd=false;}
    {std::lock_guard lock(sourceBoxesMutex);sourceBoxes.clear();}
    if(installedPot){BSCPotWireHook::unhook();installedPot=false;}
    if(installedDispatch){BSCDispatcherRenderHook::unhook();installedDispatch=false;}
    if(installedImplicitDispatch){
        BSCDispatcherImplicitRenderHook::unhook();installedImplicitDispatch=false;
    }
    if(installedPiston){BSCPistonArmModelHook::unhook();installedPiston=false;}
    if(installedHanging){BSCHangingSignModelHook::unhook();installedHanging=false;}
    if(installedSign){BSCSignModelHook::unhook();installedSign=false;}
    if(installedChest){BSCChestModelHook::unhook();installedChest=false;}
    if(installedWorldMasked){
        BSCNativeMaskedBlockHook::unhook();installedWorldMasked=false;
    }
    if(installedWorld){BSCNativeBlockHook::unhook();installedWorld=false;}
    clearNativeObservations();
}
} // namespace bsc
