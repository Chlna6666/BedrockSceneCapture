#include "NativeObservations.h"
#include "ll/api/memory/Hook.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockShape.h"
#include "mc/client/model/models/Model.h"
#include "mc/client/model/geom/ModelPart.h"
#include "mc/client/model/geom/Cube.h"
#include "mc/client/renderer/blockactor/ChestRenderer.h"
#include "mc/client/renderer/blockactor/SignRenderer.h"
#include "mc/client/renderer/blockactor/HangingSignRenderer.h"
#include "mc/client/renderer/blockactor/BedRenderer.h"
#include "mc/client/renderer/blockactor/ShulkerBoxRenderer.h"
#include "mc/client/renderer/blockactor/BellBlockActorRenderer.h"
#include <atomic>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
namespace bsc {
namespace {
std::atomic_bool armed{false};
std::mutex mutex;
struct IdHash {
    std::size_t operator()(std::array<std::uint32_t,7> const& values) const noexcept {
        std::size_t h=0x9e3779b9u;
        for(auto v:values)h ^= static_cast<std::size_t>(v)+0x9e3779b9u+(h<<6)+(h>>2);
        return h;
    }
};
std::unordered_set<std::array<std::uint32_t,7>,IdHash> seen;
std::vector<NativeObservation> pending;
std::unordered_map<std::string,std::string> actorModels;
constexpr std::size_t MaxDistinctScenes=4096;
constexpr std::size_t MaxNativeVertices=8192;
bool requiredWorldShape(BlockShape s){
    switch(s){
    case BlockShape::Stairs:
    case BlockShape::Fence:
    case BlockShape::FenceGate:
    case BlockShape::Wall:
    case BlockShape::MetalBars:
    case BlockShape::DoubleSideFence:
    case BlockShape::Water:
    case BlockShape::BubbleColumn:
    case BlockShape::RedDust:
    case BlockShape::Rail:
    case BlockShape::Tripwire:
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
std::string modelJson(Model const& model){
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o<<std::setprecision(std::numeric_limits<float>::max_digits10);
    auto const& parts=model.mAllParts.get();
    if(parts.empty())return {};
    std::unordered_map<ModelPart const*,std::size_t> indices;
    for(std::size_t i=0;i<parts.size();++i)if(parts[i])indices.emplace(parts[i],i);
    o<<"{\"schema\":1,\"source\":\"native_block_actor_model_parts\","
     "\"coordinate_space\":\"model_local\",\"animation_pose\":\"not_captured\",\"parts\":[";
    bool firstPart=true;
    std::size_t cubes=0;
    for(std::size_t index=0;index<parts.size();++index){
        auto const* part=parts[index];
        if(!part)continue;
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
            for(std::size_t face=0;face<faces.size();++face){
                if(!firstFace)o<<',';
                firstFace=false;
                o<<"{\"face\":"<<face
                 <<",\"valid\":"<<(faces[face].mFaceValid?"true":"false")
                 <<",\"uv\":";vec2(o,faces[face].mUV.get());
                o<<",\"uv_size\":";vec2(o,faces[face].mUVSize.get());
                o<<",\"material_index\":"<<faces[face].mMaterialInstanceListIndex
                 <<",\"uv_rotation\":"<<static_cast<int>(faces[face].mUvRotation)<<'}';
            }
            o<<"]}";
        }
        o<<"]}";
    }
    o<<"]}\n";
    return cubes?o.str():std::string{};
}
void recordActor(char const* label,Model const& model){
    if(!armed.load(std::memory_order_relaxed))return;
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock() || actorModels.contains(label))return;
    // Serialize while holding the bounded lock, once per actor-model variant.
    // These are actual client ModelPart/Cube definitions, not Java geometry.
    auto json=modelJson(model);
    if(!json.empty())actorModels.emplace(label,std::move(json));
}
void captureWorld(BlockTessellator& owner,Tessellator& tess,Block const& block,BlockPos const& pos,
                  std::size_t fromPos,std::size_t fromIdx){
    if(!armed.load(std::memory_order_relaxed))return;
    auto const* gfx=BlockGraphics::getForBlock(block);
    if(!gfx || !requiredWorldShape(gfx->mBlockShape))return;
    auto& raw=tess.mMeshData.get();
    auto const& points=raw.mPositions.get();
    if(fromPos>points.size() || points.size()==fromPos ||
       points.size()-fromPos>MaxNativeVertices)return;
    std::array<std::uint32_t,7> ids{};
    ids[0]=block.mNetworkId;
    auto* region=owner.mRegion;
    if(!region)return;
    // Only the six neighboring palette-state identities are read for the
    // already-rendered block. No chunk enumeration or independent tessellation.
    std::array<BlockPos,6> const neighbors{{
        {pos.x,pos.y+1,pos.z},{pos.x,pos.y-1,pos.z},
        {pos.x,pos.y,pos.z-1},{pos.x,pos.y,pos.z+1},
        {pos.x-1,pos.y,pos.z},{pos.x+1,pos.y,pos.z}
    }};
    for(std::size_t i=0;i<6;++i)ids[i+1]=region->getBlock(neighbors[i]).mNetworkId;
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock()||seen.size()>=MaxDistinctScenes||!seen.emplace(ids).second)return;
    Mesh mesh;
    mesh.primitive=static_cast<std::uint32_t>(raw.mMode);
    mesh.layer=static_cast<std::uint32_t>(gfx->mRenderLayer);
    auto const& normals=raw.mNormals.get();
    auto const& uv=raw.mTextureUVs[0].get();
    auto const& colors=raw.mColors.get();
    mesh.uvValid=(uv.size()==points.size());
    mesh.vertices.reserve(points.size()-fromPos);
    for(auto i=fromPos;i<points.size();++i){
        auto const& p=points[i];
        Vertex v;
        v.x=p.x-static_cast<float>(pos.x);
        v.y=p.y-static_cast<float>(pos.y);
        v.z=p.z-static_cast<float>(pos.z);
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
                seen.erase(ids);
                return;
            }
            mesh.indices.emplace_back(indices[i]-static_cast<std::uint32_t>(fromPos));
        }
    }
    // Only topologies supported by Library::triangulate are safe to publish.
    if(mesh.primitive<1||mesh.primitive>3){
        seen.erase(ids);
        return;
    }
    pending.push_back(NativeObservation{ids,std::move(mesh)});
}
LL_TYPE_INSTANCE_HOOK(BSCNativeBlockHook,ll::memory::HookPriority::Normal,
    BlockTessellator,&BlockTessellator::tessellateInWorld,bool,
    Tessellator& tess,Block const& block,BlockPos const& pos,bool useCache) {
    if(!armed.load(std::memory_order_relaxed))return origin(tess,block,pos,useCache);
    auto const beforePos=tess.mMeshData.get().mPositions.get().size();
    auto const beforeIdx=tess.mMeshData.get().mIndices.get().size();
    bool const result=origin(tess,block,pos,useCache);
    captureWorld(*this,tess,block,pos,beforePos,beforeIdx);
    return result;
}
LL_TYPE_INSTANCE_HOOK(BSCChestModelHook,ll::memory::HookPriority::Normal,
    ChestRenderer,&ChestRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    origin(ctx,data);
    recordActor("chest_normal",this->mChestModel.get());
    recordActor("chest_large",this->mLargeChestModel.get());
}
LL_TYPE_INSTANCE_HOOK(BSCSignModelHook,ll::memory::HookPriority::Normal,
    SignRenderer,&SignRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    origin(ctx,data);
    recordActor("standing_sign",this->mSignModel.get());
}
LL_TYPE_INSTANCE_HOOK(BSCHangingSignModelHook,ll::memory::HookPriority::Normal,
    HangingSignRenderer,&HangingSignRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    origin(ctx,data);
    recordActor("hanging_sign",this->mSignModel.get());
}
LL_TYPE_INSTANCE_HOOK(BSCBedModelHook,ll::memory::HookPriority::Normal,
    BedRenderer,&BedRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    origin(ctx,data);
    recordActor("bed",this->mBed.get());
}
LL_TYPE_INSTANCE_HOOK(BSCShulkerModelHook,ll::memory::HookPriority::Normal,
    ShulkerBoxRenderer,&ShulkerBoxRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    origin(ctx,data);
    recordActor("shulker_box",this->mShulkerBoxModel.get());
}
LL_TYPE_INSTANCE_HOOK(BSCBellModelHook,ll::memory::HookPriority::Normal,
    BellBlockActorRenderer,&BellBlockActorRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    origin(ctx,data);
    recordActor("bell",this->mBell.get());
}
bool installedWorld{},installedChest{},installedSign{},installedHanging{},
     installedBed{},installedShulker{},installedBell{};
}
bool installNativeObservations(){
    installedWorld=BSCNativeBlockHook::hook()==0;
    installedChest=BSCChestModelHook::hook()==0;
    installedSign=BSCSignModelHook::hook()==0;
    installedHanging=BSCHangingSignModelHook::hook()==0;
    installedBed=BSCBedModelHook::hook()==0;
    installedShulker=BSCShulkerModelHook::hook()==0;
    installedBell=BSCBellModelHook::hook()==0;
    return installedWorld;
}
void armNativeObservations(bool enabled){
    armed.store(enabled,std::memory_order_release);
}
void clearNativeObservations(){
    armed.store(false,std::memory_order_release);
    std::lock_guard lock(mutex);
    seen.clear();
    pending.clear();
    actorModels.clear();
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
    actorModels.clear();
    return out;
}
void uninstallNativeObservations(){
    armed.store(false,std::memory_order_release);
    if(installedBell){BSCBellModelHook::unhook();installedBell=false;}
    if(installedShulker){BSCShulkerModelHook::unhook();installedShulker=false;}
    if(installedBed){BSCBedModelHook::unhook();installedBed=false;}
    if(installedHanging){BSCHangingSignModelHook::unhook();installedHanging=false;}
    if(installedSign){BSCSignModelHook::unhook();installedSign=false;}
    if(installedChest){BSCChestModelHook::unhook();installedChest=false;}
    if(installedWorld){BSCNativeBlockHook::unhook();installedWorld=false;}
    clearNativeObservations();
}
} // namespace bsc
