#include "ActorWireframe.h"
#include "Preview.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/model/geom/ModelPart.h"
#include "mc/client/model/geom/Cube.h"
#include "mc/client/model/Geometry.h"
#include "mc/client/model/models/Model.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/blockactor/BlockActorRenderData.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/BlockPos.h"
#include "mc/deps/minecraft_renderer/framebuilder/dragon/RenderMetadata.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec3.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>
#include <iterator>
#include <memory>
#include <mutex>

namespace bsc {
namespace {
constexpr std::size_t MaxActorSegments=24000;
constexpr std::size_t MaxPartCubes=256;
constexpr std::size_t MaxPolyFaces=4096;
constexpr float MaxActorDistance=160.f;
struct ActorContext{
    glm::vec3 world{},relative{},camera{};
    bool valid{};
};
thread_local std::vector<ActorContext> contexts;
thread_local std::vector<ActorWireSegment> lines;
// BlockActor renderers can run on more than one thread. Do not assume that
// the overlay's thread_local line buffer is the same as the renderer's.
std::mutex pendingMutex;
std::vector<ActorWireSegment> pendingSegments;
std::atomic<float> activeCameraX{},activeCameraY{},activeCameraZ{};
std::atomic_bool activeCameraValid{false};
std::atomic<std::uint64_t> actorDataConstructed{},actorDataDestroyed{},actorBuffersFlushed{};
void flushActorSegments(){
    if(lines.empty())return;
    {
        std::lock_guard lock(pendingMutex);
        std::size_t const room= pendingSegments.size()<MaxActorSegments
            ?MaxActorSegments-pendingSegments.size():0;
        auto const count=std::min(room,lines.size());
        pendingSegments.insert(pendingSegments.end(),
            std::make_move_iterator(lines.begin()),
            std::make_move_iterator(lines.begin()+count));
    }
    lines.clear();
    actorBuffersFlushed.fetch_add(1,std::memory_order_relaxed);
}

std::atomic<std::uint64_t> partsCalled{},partsWithCpuGeometry{},partsWithValidTransform{},
    missingCpuGeometry{},discardedTransforms{},emittedSegments{};
bool installed{};
bool installedAtPosition{};

bool finite(glm::vec3 const& v){
    return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);
}
bool closeToActor(glm::vec3 const& point,glm::vec3 const& anchor,float maxDistance=5.f){
    return finite(point)&&glm::distance(point,anchor)<=maxDistance;
}
std::optional<glm::vec3> worldPoint(glm::vec3 const& point,
                                   ActorContext const& actor,
                                   glm::vec3 const& offset){
    auto result=point+offset;
    if(!closeToActor(result,actor.world))return std::nullopt;
    return result;
}
// Distinguish matrices that are world, camera-relative, render-relative, or
// actor-local. Only accept a single origin translation if it places the
// transformed model geometry next to the observed BlockActor world position.
std::optional<glm::vec3> inferWorldOffset(glm::vec3 const& center,
                                         ActorContext const& ctx){
    std::array<glm::vec3,4> const offsets{{
        glm::vec3{0.f},
        ctx.camera,
        ctx.world-ctx.relative,
        ctx.world
    }};
    float best=std::numeric_limits<float>::max();
    std::optional<glm::vec3> result;
    for(auto const& offset:offsets){
        auto const candidate=center+offset;
        if(!finite(candidate))continue;
        float const d=glm::distance(candidate,ctx.world+glm::vec3{.5f});
        if(d<best){best=d;result=offset;}
    }
    if(best>3.f)return std::nullopt;
    return result;
}
bool appendEdge(glm::vec3 const& a,glm::vec3 const& b){
    if(lines.size()>=MaxActorSegments)return false;
    if(!finite(a)||!finite(b))return false;
    auto const length=glm::length(a-b);
    if(!std::isfinite(length)||length<1.e-6f||length>4.f)return false;
    lines.push_back({{a.x,a.y,a.z},{b.x,b.y,b.z}});
    emittedSegments.fetch_add(1,std::memory_order_relaxed);
    return true;
}
constexpr std::array<std::array<int,2>,12> cubeEdges{{
    {{0,1}},{{1,2}},{{2,3}},{{3,0}},
    {{4,5}},{{5,6}},{{6,7}},{{7,4}},
    {{0,4}},{{1,5}},{{2,6}},{{3,7}}
}};
struct LocalGeometry{
    std::vector<std::array<glm::vec3,2>> edges;
    glm::vec3 center{};
    std::size_t count{};
    void add(glm::vec3 const& a,glm::vec3 const& b){
        if(!finite(a)||!finite(b)||edges.size()>=MaxPartCubes*12+MaxPolyFaces*4)return;
        edges.push_back({a,b});
        center+=a+b;
        count+=2;
    }
};
// Part-local boxes and polygons are read from native model geometry, never
// from BlockShape collision/selection AABB. Bone and renderer transforms are
// applied later at the actual ModelPart::render call.
LocalGeometry collectLocal(ModelPart const& part){
    LocalGeometry out;
    auto const& cubes=part.mCubes.get();
    if(cubes.size()<=MaxPartCubes)for(auto const& cube:cubes){
        auto const& o=cube.mOrigin.get();
        auto const& s=cube.mSize.get();
        glm::vec3 origin{o.x,o.y,o.z},size{s.x,s.y,s.z};
        if(!finite(origin)||!finite(size)||size.x==0.f||size.y==0.f||size.z==0.f ||
           glm::length(size)>128.f)continue;
        std::array<glm::vec3,8> vertices{{
            origin,origin+glm::vec3{size.x,0,0},
            origin+glm::vec3{size.x,size.y,0},origin+glm::vec3{0,size.y,0},
            origin+glm::vec3{0,0,size.z},origin+glm::vec3{size.x,0,size.z},
            origin+size,origin+glm::vec3{0,size.y,size.z}
        }};
        // Built-in Cube rotations are part of the cube definition, not part
        // of the bone matrix. Keep them rather than collapsing to an AABB.
        auto const& pivot=cube.mCubePivot.get();
        auto const& rotation=cube.mRotation.get();
        glm::vec3 p{pivot.x,pivot.y,pivot.z};
        glm::vec3 r{rotation.x,rotation.y,rotation.z};
        if(!finite(p)||!finite(r))continue;
        glm::mat4 transform{1.f};
        transform=glm::translate(transform,p);
        transform=glm::rotate(transform,r.z,glm::vec3{0,0,1});
        transform=glm::rotate(transform,r.y,glm::vec3{0,1,0});
        transform=glm::rotate(transform,r.x,glm::vec3{1,0,0});
        transform=glm::translate(transform,-p);
        for(auto& vertex:vertices)vertex=glm::vec3(transform*glm::vec4(vertex,1.f));
        for(auto const& edge:cubeEdges)out.add(vertices[edge[0]],vertices[edge[1]]);
    }
    auto const& poly=part.mPolyMesh.get();
    auto const& positions=poly.mPositions.get();
    auto const& tris=poly.mTris.get();
    auto const& quads=poly.mQuads.get();
    if(positions.size()<=65536 && tris.size()+quads.size()<=MaxPolyFaces){
        auto emit=[&](auto const& face){
            std::array<glm::vec3,4> vertices{};
            for(std::size_t j=0;j<face.size();++j){
                if(face[j].mPositionIndex>=positions.size())return;
                auto const& v=positions[face[j].mPositionIndex];
                vertices[j]={v.x,v.y,v.z};
                if(!finite(vertices[j]))return;
            }
            for(std::size_t j=0;j<face.size();++j)
                out.add(vertices[j],vertices[(j+1)%face.size()]);
        };
        for(auto const& tri:tris)emit(tri);
        for(auto const& quad:quads)emit(quad);
    }
    if(out.count)out.center/=static_cast<float>(out.count);
    return out;
}
void observePart(ModelPart const& part,ScreenContext& screen,float scale,
                 Matrix const& boneMatrix){
    if(!(gPreviewEnabled.load(std::memory_order_relaxed)||
         gCapturePreviewEnabled.load(std::memory_order_relaxed))||contexts.empty()||
       !contexts.back().valid||!part.mVisible||part.mNeverRender||
       lines.size()>=MaxActorSegments)return;
    partsCalled.fetch_add(1,std::memory_order_relaxed);
    auto const& actor=contexts.back();
    if(glm::length(actor.world-actor.camera)>MaxActorDistance)return;
    auto geometry=collectLocal(part);
    if(geometry.edges.empty()){
        missingCpuGeometry.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    partsWithCpuGeometry.fetch_add(1,std::memory_order_relaxed);
    auto const& world=screen.camera.worldMatrixStack.get().top()._m.get();
    auto const& bone=boneMatrix._m.get();
    if(!finite(glm::vec3(world[3]))||!finite(glm::vec3(bone[3]))){
        discardedTransforms.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    // The native model may use 1/16 geometry units; never assume that
    // blindly. Select a uniform scale only when geometry fits the actual
    // BlockActor position and 4-block safety bound.
    for(float candidateScale:{scale,scale/16.f,1.f/16.f,1.f}){
        if(!std::isfinite(candidateScale)||candidateScale<=0.f||
           candidateScale>1.f)continue;
        auto const transform=world*bone*
            glm::scale(glm::mat4{1.f},glm::vec3(candidateScale));
        auto const center=glm::vec3(transform*glm::vec4(geometry.center,1.f));
        if(!finite(center))continue;
        auto offset=inferWorldOffset(center,actor);
        if(!offset)continue;
        // Reject the whole part if native transforms would put any edge far
        // outside this BlockActor. A wrong red/cyan ghost is worse than a
        // clearly reported unsupported native CPU mesh.
        std::vector<std::array<glm::vec3,2>> transformed;
        transformed.reserve(geometry.edges.size());
        bool valid=true;
        for(auto const& e:geometry.edges){
            auto a=glm::vec3(transform*glm::vec4(e[0],1.f))+*offset;
            auto b=glm::vec3(transform*glm::vec4(e[1],1.f))+*offset;
            if(!closeToActor(a,actor.world,5.f)||!closeToActor(b,actor.world,5.f)||
               glm::length(a-b)>4.f){valid=false;break;}
            transformed.push_back({a,b});
        }
        if(!valid)continue;
        for(auto const& e:transformed)if(!appendEdge(e[0],e[1]))break;
        partsWithValidTransform.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    discardedTransforms.fetch_add(1,std::memory_order_relaxed);
}
thread_local unsigned gPartRenderAtPositionDepth{};
struct PartPositionScope {
    PartPositionScope(){++gPartRenderAtPositionDepth;}
    ~PartPositionScope(){--gPartRenderAtPositionDepth;}
};
LL_TYPE_INSTANCE_HOOK(BSCActorPartWireHook,ll::memory::HookPriority::Normal,
    ModelPart,&ModelPart::render,void,
    ScreenContext& screen,Model& model,float scale,bool hideParentBodyPart,
    uint count,Matrix boneToEntitySpaceMatrix){
    // The renderer has already configured the bone pose. Capture while the
    // parent model matrix is still on the stack; the original call may pop it.
    if(gPartRenderAtPositionDepth==0)
        observePart(*this,screen,scale,boneToEntitySpaceMatrix);
    origin(screen,model,scale,hideParentBodyPart,count,boneToEntitySpaceMatrix);
}
// Some native BlockEntity models route directly through renderAtPosition,
// never calling ModelPart::render. Observe the same native part while its
// caller's world matrix is still valid. Validation discards bad transforms.
LL_TYPE_INSTANCE_HOOK(BSCActorPartAtPositionWireHook,ll::memory::HookPriority::Normal,
    ModelPart,&ModelPart::renderAtPosition,void,
    ScreenContext& screen,Model& model,Vec3 const& position,float scale,
    bool hideParentBodyPart,uint count,Matrix boneToEntitySpaceMatrix){
    observePart(*this,screen,scale,boneToEntitySpaceMatrix);
    PartPositionScope scope;
    origin(screen,model,position,scale,hideParentBodyPart,count,boneToEntitySpaceMatrix);
}
} // namespace
ActorWireframeScope::ActorWireframeScope(BaseActorRenderContext& ctx,
                                         BlockActorRenderData const& data)
    :ActorWireframeScope(ctx,data.worldPosition.get(),data.renderPosition){}
ActorWireframeScope::ActorWireframeScope(BaseActorRenderContext& ctx,
                                         BlockPos const& position,Vec3 const& relative)
    :ActorWireframeScope(position,relative,[&]{
        auto const& impl=ctx.mImpl.get();
        Vec3 camera{};
        if(impl)camera=impl->mCameraPosition.get();
        if(auto const* player=ctx.mClientInstance.getLocalPlayer()){
            auto eye=player->getEyePos();
            glm::vec3 const raw{camera.x,camera.y,camera.z};
            glm::vec3 const eyePos{eye.x,eye.y,eye.z};
            if(!finite(raw)||glm::distance(raw,eyePos)>128.f)camera=eye;
        }
        return camera;
    }()){}
ActorWireframeScope::ActorWireframeScope(BlockPos const& position,
                                         Vec3 const& relative,Vec3 const& camera){
    if(!(gPreviewEnabled.load(std::memory_order_relaxed)||
         gCapturePreviewEnabled.load(std::memory_order_relaxed)))return;
    ActorContext actor;
    actor.world={static_cast<float>(position.x),
                 static_cast<float>(position.y),static_cast<float>(position.z)};
    actor.relative={relative.x,relative.y,relative.z};
    actor.camera={camera.x,camera.y,camera.z};
    actor.valid=finite(actor.world)&&finite(actor.relative)&&finite(actor.camera);
    if(!actor.valid)return;
    contexts.push_back(actor);
    actorDataConstructed.fetch_add(1,std::memory_order_relaxed);
    active_=true;
}
ActorWireframeScope::~ActorWireframeScope(){
    if(active_ && !contexts.empty()){
        auto const world=contexts.back().world;
        contexts.pop_back();
        actorDataDestroyed.fetch_add(1,std::memory_order_relaxed);
        if(contexts.empty()){
            if(gCapturePreviewEnabled.load(std::memory_order_acquire)&&!lines.empty()){
                BlockPos const pos{static_cast<int>(world.x),
                                   static_cast<int>(world.y),
                                   static_cast<int>(world.z)};
                addCaptureActorPreview(pos,lines);
            }
            flushActorSegments();
        }
    }
}
void updateActorWireframeCamera(Vec3 const& camera){
    bool const ok=std::isfinite(camera.x)&&std::isfinite(camera.y)&&std::isfinite(camera.z);
    if(ok){
        activeCameraX.store(camera.x,std::memory_order_relaxed);
        activeCameraY.store(camera.y,std::memory_order_relaxed);
        activeCameraZ.store(camera.z,std::memory_order_relaxed);
    }
    activeCameraValid.store(ok,std::memory_order_release);
}
// The lifetime of BlockActorRenderData spans the native renderer call on
// vanilla's dispatcher. Unlike an explicit list of renderer hooks, this
// includes every BlockEntity family that uses BlockActorRenderData.
// Keep the hook scoped; do not store raw BlockActorRenderData pointers.
bool installActorWireframePartHook(ll::mod::NativeMod& mod){
    if(installed && installedAtPosition)return true;
    if(!installed){
        mod.getLogger().info("F7/F8 BlockEntity ModelPart::render hook BEGIN");
        installed=(BSCActorPartWireHook::hook()==0);
        mod.getLogger().info("F7/F8 BlockEntity ModelPart::render hook {}",installed?"OK":"FAILED");
    }
    if(!installedAtPosition){
        mod.getLogger().info("F7/F8 BlockEntity ModelPart::renderAtPosition hook BEGIN");
        installedAtPosition=(BSCActorPartAtPositionWireHook::hook()==0);
        mod.getLogger().info("F7/F8 BlockEntity ModelPart::renderAtPosition hook {}",
                             installedAtPosition?"OK":"FAILED");
    }
    return installed || installedAtPosition;
}
void uninstallActorWireframePartHook(){
    if(installed){BSCActorPartWireHook::unhook();installed=false;}
    if(installedAtPosition){
        BSCActorPartAtPositionWireHook::unhook();
        installedAtPosition=false;
    }
    lines.clear();
    contexts.clear();
    {
        std::lock_guard lock(pendingMutex);
        pendingSegments.clear();
    }
}
void beginActorWireframeFrame(){
    // Clear only the *completed frame* buffer, not scopes still rendering.
    std::lock_guard lock(pendingMutex);
    pendingSegments.clear();
}
std::vector<ActorWireSegment> takeActorWireframeSegments(){
    // A detached renderer thread's completed scopes are moved to the shared
    // frame buffer by their BlockActorRenderData destructor.
    std::vector<ActorWireSegment> output;
    {
        std::lock_guard lock(pendingMutex);
        output.swap(pendingSegments);
    }
    if(!lines.empty()){
        auto const room=output.size()<MaxActorSegments
            ?MaxActorSegments-output.size():0;
        auto const count=std::min(room,lines.size());
        output.insert(output.end(),
            std::make_move_iterator(lines.begin()),
            std::make_move_iterator(lines.begin()+count));
        lines.clear();
    }
    return output;
}
ActorWireframeStats actorWireframeStats(){
    return {partsCalled.load(std::memory_order_relaxed),
            partsWithCpuGeometry.load(std::memory_order_relaxed),
            partsWithValidTransform.load(std::memory_order_relaxed),
            missingCpuGeometry.load(std::memory_order_relaxed),
            discardedTransforms.load(std::memory_order_relaxed),
            emittedSegments.load(std::memory_order_relaxed),
            actorDataConstructed.load(std::memory_order_relaxed),
            actorDataDestroyed.load(std::memory_order_relaxed),
            actorBuffersFlushed.load(std::memory_order_relaxed)};
}
} // namespace bsc
