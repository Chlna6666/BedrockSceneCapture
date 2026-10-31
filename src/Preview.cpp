#include "Preview.h"
#include "MeshUtils.h"
#include "ActorWireframe.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/memory/Hook.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Frustum.h"
#include "mc/deps/core_graphics/TextureSetLayerType.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/minecraft_renderer/renderer/IsMissingTexture.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/minecraft_renderer/framebuilder/dragon/RenderMetadata.h"
#include "mc/deps/renderer/ShaderColor.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include <Windows.h>
#include <exception>
#include <string>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace bsc {
std::atomic_bool gPreviewEnabled{false};
std::atomic_bool gCapturePreviewEnabled{false};
namespace {
using Clock=std::chrono::steady_clock;
using Key=std::pair<int,int>;
constexpr std::size_t MaxLinesPerChunk=60000;
// GPU resident line-segment budget; buffers contain two vertices per line.
constexpr std::size_t MaxResidentGpuSegments=1500000;
constexpr std::size_t MaxVertexMarksPerChunk=2000;
constexpr unsigned MaxChunkMeshes=512;
constexpr unsigned RefreshFrames=120;
// Avoid endlessly re-tessellating unchanged chunks while the player is exploring.
constexpr unsigned RebuildFrames=3600;
bool gOverlayInstalled{};
constexpr std::size_t MaxCaptureSections=384;
constexpr std::size_t MaxCaptureSegments=250000;
// Reserve capacity for BlockEntity previews. The ordinary world tessellator
// can otherwise fill all F8 sections/segments before a renderer is observed.
constexpr std::size_t MaxBlockCaptureSections=MaxCaptureSections-48;
constexpr std::size_t MaxBlockCaptureSegments=MaxCaptureSegments-40000;
constexpr std::size_t MaxCaptureSectionSegments=12000;

struct ChunkWire {
    int x{},z{},minY{},maxY{};
    std::uint32_t cell{};
    std::uint64_t builtFrame{};
    bool queued{}, building{},ready{},truncated{};
    std::vector<std::array<Vec3,2>> lines;
    std::unique_ptr<mce::Mesh> gpu;
    std::size_t gpuSegments{};
};
std::mutex gCaptureMutex;
using CaptureKey=std::tuple<int,int,int>;
std::map<CaptureKey,ChunkWire> gCaptureSections;
std::size_t gCaptureSegments{};
// A position can render multiple legal poses (extended piston head, book
// animation, bell swing). Retain a few distinct poses instead of freezing
// the very first pose forever.
constexpr std::size_t MaxActorPoseVariants=4;
std::map<std::array<int,3>,std::set<std::uint64_t>> gCapturedActorPoses;
std::weak_ptr<mce::BufferResourceService> gCaptureService;
std::mutex gResourceMutex;
std::weak_ptr<mce::BufferResourceService> gResource;
std::weak_ptr<mce::BufferResourceService> gBoundService;
std::unique_ptr<Tessellator> gInputTess;
std::unique_ptr<BlockTessellator> gBlockTess;
std::map<Key,ChunkWire> gChunks;
std::deque<Key> gQueue;
std::optional<Key> gActive;
LocalPlayer* gPlayer{};
BlockSource* gRegion{};
std::uint64_t gFrame{};
unsigned gVertexMarks{};
ll::mod::NativeMod* gPreviewMod{};
Clock::time_point gLastTransformDiagnostic{};
mce::TexturePtr gWhiteTexture{};
bool gWhiteReady{};

using TextureVariant=std::variant<std::monostate,mce::TexturePtr,mce::ClientTexture,mce::ServerTexture>;
TextureVariant white(LevelRenderer& level) {
    if(!gWhiteReady){
        auto const& group=level.mTextureGroup.get();
        if(group){
            auto tex=group->getTexture(
                ResourceLocation{Core::PathView{"textures/ui/white_background"}},
                false,std::nullopt,cg::TextureSetLayerType::Color);
            if(tex.mClientTexture && tex.mClientTexture->mIsMissingTexture!=IsMissingTexture::Yes){
                gWhiteTexture=std::move(tex);
                gWhiteReady=true;
            }
        }
    }
    if(gWhiteReady)return TextureVariant{gWhiteTexture};
    return {};
}
void reset() {
    gChunks.clear();
    gQueue.clear();
    gActive.reset();
    gInputTess.reset();
    gBlockTess.reset();
    gBoundService.reset();
    gPlayer=nullptr;
    gRegion=nullptr;
    gFrame=0;
    gWhiteTexture=mce::TexturePtr{};
    gWhiteReady=false;
}
Vec3 cameraPosition(BaseActorRenderContext const& ctx){
    // Camera::mPosition is (0,0,0) in the BlockEntity render pass on
    // Bedrock 1.26.51.1. Its origin is NOT the world-space player camera.
    // Use the render-context camera (the origin used for world-relative
    // BlockEntity geometry) so F7 lines receive exactly one subtraction.
    auto const& impl=ctx.mImpl.get();
    if(impl){
        auto const& p=impl->mCameraPosition.get();
        if(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)){
            if(auto const* player=ctx.mClientInstance.getLocalPlayer()){
                auto const& q=player->getPosition();
                // Guard against an uninitialized (0,0,0) context on a
                // renderer pass. The fallback is the player eye, not zero.
                float const d=std::abs(p.x-q.x)+std::abs(p.y-q.y)+std::abs(p.z-q.z);
                if(d<128.f)return Vec3{p.x,p.y,p.z};
            }else return Vec3{p.x,p.y,p.z};
        }
    }
    if(auto const* player=ctx.mClientInstance.getLocalPlayer())
        return player->getEyePos();
    return Vec3{};
}
void line(ChunkWire& chunk,Vec3 const& a,Vec3 const& b){
    if(chunk.lines.size()>=MaxLinesPerChunk){
        chunk.truncated=true;
        return;
    }
    chunk.lines.push_back({a,b});
}
void vertexMark(ChunkWire& chunk,Vec3 const& v){
    if(gVertexMarks>=MaxVertexMarksPerChunk)return;
    ++gVertexMarks;
    constexpr float d=0.027f;
    line(chunk,{v.x-d,v.y,v.z},{v.x+d,v.y,v.z});
    line(chunk,{v.x,v.y-d,v.z},{v.x,v.y+d,v.z});
    line(chunk,{v.x,v.y,v.z-d},{v.x,v.y,v.z+d});
}
int floorChunk(int blockCoord){
    auto n=static_cast<std::int64_t>(blockCoord);
    return static_cast<int>(n>=0?n/16:-((-n+15)/16));
}
void consume(ChunkWire& chunk,Mesh const& mesh,BlockPos const& blockPos,bool vertexMarks=true) {
    auto at=[&](std::uint32_t i)->Vec3{
        auto const& v=mesh.vertices[i];
        return {static_cast<float>(blockPos.x-chunk.x*16)+v.x,
                static_cast<float>(blockPos.y-chunk.minY)+v.y,
                static_cast<float>(blockPos.z-chunk.z*16)+v.z};
    };
    auto edge=[&](std::uint32_t a,std::uint32_t b){
        if(a>=mesh.vertices.size()||b>=mesh.vertices.size())return;
        line(chunk,at(a),at(b));
    };
    auto triangle=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c){
        edge(a,b);edge(b,c);edge(c,a);
    };
    auto const& indices=mesh.indices;
    std::size_t const n=indices.empty()?mesh.vertices.size():indices.size();
    auto index=[&](std::size_t i)->std::uint32_t{
        return indices.empty()?static_cast<std::uint32_t>(i):indices[i];
    };
    // QuadList perimeter edges only. Triangulating every quad inserted a
    // redundant diagonal and doubled the GPU line count on dense blocks.
    switch(mesh.primitive){
    case 1:
        for(std::size_t i=0;i+3<n && !chunk.truncated;i+=4){
            auto a=index(i),b=index(i+1),c=index(i+2),d=index(i+3);
            edge(a,b);edge(b,c);edge(c,d);edge(d,a);
        }
        break;
    case 2:
        for(std::size_t i=0;i+2<n && !chunk.truncated;i+=3)
            triangle(index(i),index(i+1),index(i+2));
        break;
    case 3:
        for(std::size_t i=2;i<n && !chunk.truncated;++i){
            if(i&1)triangle(index(i-1),index(i-2),index(i));
            else triangle(index(i-2),index(i-1),index(i));
        }
        break;
    default: break;
    }
    if(vertexMarks)for(std::size_t i=0;i<mesh.vertices.size() &&
         gVertexMarks<MaxVertexMarksPerChunk && !chunk.truncated;++i)
        vertexMark(chunk,at(static_cast<std::uint32_t>(i)));
}
void enqueue(Key key){
    auto it=gChunks.find(key);
    if(it!=gChunks.end()&&!it->second.queued&&!it->second.building){
        it->second.queued=true;
        gQueue.emplace_back(key);
    }
}
void refreshLoaded(BlockSource& region,BlockPos const& center){
    auto& source=region.getChunkSource();
    std::set<Key> loaded;
    auto collect=[&](auto const& storage){
        for(auto const& [pos,weak]:storage)
            if(weak.lock()&&source.getAvailableChunk(pos))
                loaded.emplace(pos.x,pos.z);
    };
    if(auto* map=source.getChunkMap())collect(*map);
    if(loaded.empty())collect(source.getStorage());
    // Bound memory before allocating cache entries: admit the nearest loaded chunks.
    if(loaded.size()>MaxChunkMeshes){
        std::vector<Key> nearest(loaded.begin(),loaded.end());
        std::sort(nearest.begin(),nearest.end(),[&](Key const& a,Key const& b){
            auto distance=[&](Key const& key){
                long long dx=static_cast<long long>(key.first)-floorChunk(center.x);
                long long dz=static_cast<long long>(key.second)-floorChunk(center.z);
                return dx*dx+dz*dz;
            };
            return distance(a)<distance(b);
        });
        nearest.resize(MaxChunkMeshes);
        loaded=std::set<Key>(nearest.begin(),nearest.end());
    }
    for(auto it=gChunks.begin();it!=gChunks.end();){
        if(!loaded.contains(it->first)){
            if(gActive==it->first)gActive.reset();
            it=gChunks.erase(it);
        }else ++it;
    }
    // Existing GPUs remain valid across player movement. Only new or aged chunks queue work.
    for(auto const& key:loaded) {
        auto [it,inserted]=gChunks.try_emplace(key);
        if(inserted){
            it->second.x=key.first;
            it->second.z=key.second;
            it->second.minY=region.getMinHeight();
            it->second.maxY=region.getMaxHeight();
            enqueue(key);
        } else if(!it->second.ready && !it->second.queued && !it->second.building) {
            // Rebuild an evicted GPU mesh when the player comes near it again.
            long long dx=static_cast<long long>(key.first)-floorChunk(center.x);
            long long dz=static_cast<long long>(key.second)-floorChunk(center.z);
            if(dx*dx+dz*dz<64)enqueue(key);
        } else if(it->second.ready && gFrame-it->second.builtFrame>RebuildFrames) {
            auto dx=key.first-floorChunk(center.x);
            auto dz=key.second-floorChunk(center.z);
            if(dx*dx+dz*dz<64)enqueue(key);
        }
    }
    // Remove queued work for chunks that have been evicted or unloaded.
    gQueue.erase(std::remove_if(gQueue.begin(),gQueue.end(),
        [&](Key const& key){return !gChunks.contains(key);}),gQueue.end());
}
void update(BaseActorRenderContext& ctx){
    if(!gPreviewEnabled.load()){
        if(gPlayer||!gChunks.empty())reset();
        return;
    }
    auto* player=ctx.mClientInstance.getLocalPlayer();
    if(!player){reset();return;}
    auto& region=player->getDimensionBlockSource();
    auto const& service=ctx.mScreenContext.tessellator.mBufferResourceService;
    if(service.expired()){reset();return;}
    if(player!=gPlayer || &region!=gRegion ||
       gBoundService.lock()!=service.lock()){
        // Old meshes and white-texture handles belong to the previous GPU
        // resource service and cannot survive renderer/device recreation.
        reset();
        gPlayer=player;
        gRegion=&region;
        gBoundService=service;
        gBlockTess=std::make_unique<BlockTessellator>(&region);
        gBlockTess->mCachedGetBlock.get()=[&region](BlockPos const& p)->Block const&{
            return region.getBlock(p);
        };
        gInputTess=std::make_unique<Tessellator>(ctx.mScreenContext.tessellator.mBufferResourceService);
    }
    ++gFrame;
    auto center=player->getFeetBlockPos();
    if(gFrame==1 || gFrame%RefreshFrames==0)refreshLoaded(region,center);
    if(!gActive){
        // Nearest queued chunk first; no fixed radius or 5x5x5 restriction.
        auto found=gQueue.end();
        long long best=std::numeric_limits<long long>::max();
        for(auto it=gQueue.begin();it!=gQueue.end();++it){
            if(!gChunks.contains(*it))continue;
            long long dx=static_cast<long long>(it->first)-floorChunk(center.x);
            long long dz=static_cast<long long>(it->second)-floorChunk(center.z);
            auto dist=dx*dx+dz*dz;
            if(dist<best){best=dist;found=it;}
        }
        if(found!=gQueue.end()){
            auto key=*found;
            gQueue.erase(found);
            auto& chunk=gChunks.at(key);
            chunk.queued=false;
            chunk.building=true;
            chunk.cell=0;
            chunk.lines.clear();
            chunk.truncated=false;
            gVertexMarks=0;
            gActive=key;
        }else{
            gQueue.clear();
            return;
        }
    }
    auto it=gChunks.find(*gActive);
    if(it==gChunks.end()){gActive.reset();return;}
    auto& chunk=it->second;
    if(!region.getChunkSource().getAvailableChunk(ChunkPos{chunk.x,chunk.z})){
        gChunks.erase(it);gActive.reset();return;
    }
    auto started=Clock::now();
    auto volume=static_cast<std::uint32_t>((chunk.maxY-chunk.minY)*256);
    // Native tessellation is the expensive operation, not scanning an air cell.
    // Retain a strict per-frame time budget while batching more cells/calls;
    // 512 cells and 2 tessellations previously serialized a dense chunk over
    // hundreds of frames before a single visible GPU mesh could be uploaded.
    unsigned nativeCalls=0;
    for(unsigned i=0;i<4096 && chunk.cell<volume &&
        nativeCalls<12 && Clock::now()-started<std::chrono::microseconds(2500);++i){
        auto index=chunk.cell++;
        int x=index%16,z=(index/16)%16,y=chunk.minY+static_cast<int>(index/256);
        BlockPos pos{chunk.x*16+x,y,chunk.z*16+z};
        auto const& block=region.getBlock(pos);
        if(block.isAir()||chunk.truncated)continue;
        ChunkData output;
        captureMesh(region,*gBlockTess,*gInputTess,block,pos,0,output);
        ++nativeCalls;
        for(auto const& mesh:output.meshes)consume(chunk,mesh,pos);
    }
    if(chunk.cell>=volume){
        chunk.building=false;
        chunk.ready=true;
        chunk.builtFrame=gFrame;
        chunk.gpu.reset();
        chunk.gpuSegments=0;
        gActive.reset();
    }
}
bool visible(BaseActorRenderContext& ctx,ChunkWire const& chunk,Vec3 const& camera) {
    // SDK 26.51.9 Frustum::cubeInFrustum is marked unavailable. Use the
    // engine camera basis and a conservative chunk bounding sphere instead.
    auto const& cam=ctx.mScreenContext.camera;
    float const cx=static_cast<float>(chunk.x*16+8)-camera.x;
    float const cy=(static_cast<float>(chunk.minY)+static_cast<float>(chunk.maxY))*0.5f-camera.y;
    float const cz=static_cast<float>(chunk.z*16+8)-camera.z;
    glm::vec3 const delta{cx,cy,cz};
    float const height=static_cast<float>(chunk.maxY-chunk.minY)*0.5f;
    float const radius=std::sqrt(8.0f*8.0f+height*height+8.0f*8.0f);
    auto const forward=cam.mForward.get();
    auto const right=cam.mRight.get();
    auto const up=cam.mUp.get();
    float const depth=glm::dot(delta,forward);
    float const farClip=cam.mZFar;
    // Fall back to drawing if the camera basis has not been initialized.
    if(glm::dot(forward,forward)<0.25f || farClip<=0.f)return true;
    if(depth+radius<0.f || depth-radius>farClip)return false;
    float fov=cam.mFov;
    if(fov>3.2f)fov*=0.01745329252f;
    if(fov<=0.f || fov>=3.14f)return true;
    float const slope=std::tan(fov*0.5f);
    float const aspect=std::max(1.f,cam.mAspectRatio);
    float const width=std::max(0.f,depth)*slope*aspect+radius;
    float const vertical=std::max(0.f,depth)*slope+radius;
    return std::abs(glm::dot(delta,right))<=width &&
           std::abs(glm::dot(delta,up))<=vertical;
}
double chunkCameraDistanceSq(ChunkWire const& chunk,Vec3 const& camera){
    double const dx=static_cast<double>(chunk.x)*16.0+8.0-camera.x;
    double const dz=static_cast<double>(chunk.z)*16.0+8.0-camera.z;
    return dx*dx+dz*dz;
}
void render(BaseActorRenderContext& ctx){
    if(!gPreviewEnabled.load(std::memory_order_relaxed) &&
       !gCapturePreviewEnabled.load(std::memory_order_relaxed))return;
    auto* level=ctx.mClientInstance.getLevelRenderer();
    if(!level||!level->mLevelRendererPlayer)return;
    auto const& material=level->mLevelRendererPlayer->mOutlineSelectionMaterial.get();
    if(!material.mRenderMaterialInfoPtr.get())return;
    auto texture=white(*level);
    auto const camera=cameraPosition(ctx);
    auto& t=ctx.mScreenContext.tessellator;
    auto& shader=ctx.mScreenContext.currentShaderColor;
    auto previous=shader.color.get();
    shader.color=mce::Color{0.14f,0.96f,0.83f,1.f};
    shader.dirty=true;
    using Capture=decltype(dragon::RenderMetadata::mOffscreenCaptureDescription);
    static Capture empty{};
    auto restoreShader=[&](){
        shader.color=previous;
        shader.dirty=true;
    };
    try {
        // Ordered map iteration has no relation to camera distance.
        // Upload the closest visible, not-yet-buffered chunk first.
        ChunkWire* candidate=nullptr;
        double candidateDistance=std::numeric_limits<double>::infinity();
        std::size_t residentSegments=0;
        for(auto& [key,chunk]:gChunks){
            if(chunk.gpu)residentSegments+=chunk.gpuSegments;
            if(!chunk.ready || chunk.gpu || chunk.lines.empty() ||
               !visible(ctx,chunk,camera))continue;
            double const distance=chunkCameraDistanceSq(chunk,camera);
            if(distance<candidateDistance){
                candidate=&chunk;
                candidateDistance=distance;
            }
        }
        if(candidate){
            auto const requested=candidate->lines.size();
            // Evict only buffers farther from the camera than the incoming
            // candidate. This prevents oscillation around the budget.
            while(residentSegments+requested>MaxResidentGpuSegments){
                ChunkWire* victim=nullptr;
                double farthest=candidateDistance;
                for(auto& [key,chunk]:gChunks){
                    if(!chunk.gpu || !chunk.gpuSegments)continue;
                    double const d=chunkCameraDistanceSq(chunk,camera);
                    if(d>farthest){farthest=d;victim=&chunk;}
                }
                if(!victim)break;
                residentSegments-=victim->gpuSegments;
                victim->gpu.reset();
                victim->gpuSegments=0;
                victim->ready=false;
            }
            if(residentSegments+requested<=MaxResidentGpuSegments &&
               requested<=static_cast<std::size_t>(std::numeric_limits<int>::max()/2)){
                t.begin(Tessellator::DebugContextCallback{},mce::PrimitiveMode::LineList,
                        static_cast<int>(requested*2),false);
                t.color(1.f,1.f,1.f,1.f);
                for(auto const& ends:candidate->lines)
                    for(auto const& point:ends){
                        t.tex2({0.5f,0.5f});
                        t.vertex(point.x,point.y,point.z);
                    }
                candidate->gpu=std::make_unique<mce::Mesh>(
                    t.end(Tessellator::UploadMode::Buffered,
                          "BSC_ChunkWire",SupplementaryFieldAutoGenerationMode{0}));
                candidate->gpuSegments=requested;
            }else{
                // Leave it eligible for near-camera rebuild rather than
                // retaining a huge CPU geometry array indefinitely.
                candidate->ready=false;
            }
            // vector.clear() alone leaves up to 60k segments of heap capacity
            // in every chunk even when those lines are on the GPU.
            std::vector<std::array<Vec3,2>>{}.swap(candidate->lines);
        }
        // renderBlockEntities may leave an inherited world-to-camera matrix
        // on the stack. push(false) cloned it, and the previous code then
        // subtracted the camera a second time: the whole F7 world shifted
        // down/away from the player's actual blocks.
        //
        // Begin from a known identity model matrix. The camera *view* stack
        // can be either a world-to-eye matrix or orientation-only depending
        // on the renderer pass. Detect which space it consumes so that the
        // camera translation is applied exactly once.
        auto const& view=ctx.mScreenContext.camera.viewMatrixStack.get().top()._m.get();
        glm::vec3 const cameraWorld{camera.x,camera.y,camera.z};
        glm::vec3 const cameraInView{view*glm::vec4(cameraWorld,1.f)};
        glm::vec3 const originInView{view*glm::vec4(0.f,0.f,0.f,1.f)};
        bool const viewAlreadySubtractsCamera=
            std::isfinite(cameraInView.x)&&std::isfinite(cameraInView.y)&&std::isfinite(cameraInView.z) &&
            std::isfinite(originInView.x)&&std::isfinite(originInView.y)&&std::isfinite(originInView.z) &&
            glm::length(cameraInView)+0.5f<glm::length(originInView);

        if(gPreviewMod && (gLastTransformDiagnostic.time_since_epoch().count()==0 ||
            Clock::now()-gLastTransformDiagnostic>std::chrono::seconds(8))){
            gLastTransformDiagnostic=Clock::now();
            auto const& world=ctx.mScreenContext.camera.worldMatrixStack.get().top()._m.get();
            auto const& projection=ctx.mScreenContext.camera.projectionMatrixStack.get().top()._m.get();
            auto const* player=ctx.mClientInstance.getLocalPlayer();
            auto const playerPos=player?player->getFeetBlockPos():BlockPos{};
            gPreviewMod->getLogger().info(
                "F7 matrix diag: camera=({:.2f},{:.2f},{:.2f}) player=({},{},{}) "
                "view_translation=({:.2f},{:.2f},{:.2f}) "
                "world_translation=({:.2f},{:.2f},{:.2f}) "
                "projection_diag=({:.3f},{:.3f},{:.3f}) "
                "eye_camera=({:.2f},{:.2f},{:.2f}) "
                "eye_origin=({:.2f},{:.2f},{:.2f}) "
                "view_subtracts_camera={} cached_chunks={} camera_struct=({:.2f},{:.2f},{:.2f}) "
                "player_eye=({:.2f},{:.2f},{:.2f}) camera_delta_from_eye=({:.2f},{:.2f},{:.2f})",
                camera.x,camera.y,camera.z,playerPos.x,playerPos.y,playerPos.z,
                view[3][0],view[3][1],view[3][2],
                world[3][0],world[3][1],world[3][2],
                projection[0][0],projection[1][1],projection[2][2],
                cameraInView.x,cameraInView.y,cameraInView.z,
                originInView.x,originInView.y,originInView.z,
                viewAlreadySubtractsCamera,gChunks.size(),
                ctx.mScreenContext.camera.mPosition.get().x,
                ctx.mScreenContext.camera.mPosition.get().y,
                ctx.mScreenContext.camera.mPosition.get().z,
                player?player->getEyePos().x:0.f,
                player?player->getEyePos().y:0.f,
                player?player->getEyePos().z:0.f,
                player?camera.x-player->getEyePos().x:0.f,
                player?camera.y-player->getEyePos().y:0.f,
                player?camera.z-player->getEyePos().z:0.f);
        }
        for(auto& [key,chunk]:gChunks){
            if(!chunk.ready || !chunk.gpu || !chunk.gpu->isValid() ||
               !visible(ctx,chunk,camera))continue;
            // Never inherit the original block-entity model transform.
            auto stack=ctx.mScreenContext.camera.worldMatrixStack.get().pushIdentity(false);
            float tx=static_cast<float>(chunk.x*16);
            float ty=static_cast<float>(chunk.minY);
            float tz=static_cast<float>(chunk.z*16);
            if(!viewAlreadySubtractsCamera){
                tx-=camera.x;
                ty-=camera.y;
                tz-=camera.z;
            }
            stack.mat->translate(tx,ty,tz);
            chunk.gpu->renderMesh(ctx.mScreenContext,material,texture,0,
                chunk.gpu->mVertexCount.get().value_or(0u),empty.get(),nullptr);
        }
        if(gCapturePreviewEnabled.load(std::memory_order_relaxed)){
            std::lock_guard lock(gCaptureMutex);
            auto const service=ctx.mScreenContext.tessellator.mBufferResourceService.lock();
            if(gCaptureService.lock()!=service){
                for(auto& [key,section]:gCaptureSections)section.gpu.reset();
                gCaptureService=service;
            }
            ChunkWire* candidate=nullptr;
            double nearest=std::numeric_limits<double>::infinity();
            for(auto& [key,section]:gCaptureSections){
                if(section.gpu || section.lines.empty() || !visible(ctx,section,camera))continue;
                auto const distance=chunkCameraDistanceSq(section,camera);
                if(distance<nearest){nearest=distance;candidate=&section;}
            }
            if(candidate){
                auto const count=candidate->lines.size();
                t.begin(Tessellator::DebugContextCallback{},mce::PrimitiveMode::LineList,
                        static_cast<int>(count*2),false);
                t.color(1.f,1.f,1.f,1.f);
                for(auto const& segment:candidate->lines)
                    for(auto const& point:segment){
                        t.tex2({0.5f,0.5f});
                        t.vertex(point.x,point.y,point.z);
                    }
                candidate->gpu=std::make_unique<mce::Mesh>(
                    t.end(Tessellator::UploadMode::Buffered,
                          "BSC_F8NativeSample",SupplementaryFieldAutoGenerationMode{0}));
                candidate->gpuSegments=count;
            }
            shader.color=mce::Color{1.f,0.18f,0.22f,1.f};
            shader.dirty=true;
            for(auto& [key,section]:gCaptureSections){
                if(!section.gpu || !section.gpu->isValid() ||
                   !visible(ctx,section,camera))continue;
                auto stack=ctx.mScreenContext.camera.worldMatrixStack.get().pushIdentity(false);
                float tx=static_cast<float>(section.x*16);
                float ty=static_cast<float>(section.minY);
                float tz=static_cast<float>(section.z*16);
                if(!viewAlreadySubtractsCamera){tx-=camera.x;ty-=camera.y;tz-=camera.z;}
                stack.mat->translate(tx,ty,tz);
                section.gpu->renderMesh(ctx.mScreenContext,material,texture,0,
                    section.gpu->mVertexCount.get().value_or(0u),empty.get(),nullptr);
            }
        }
    }catch(...){
        restoreShader();
        throw;
    }
    restoreShader();
}
// F7 dedicated BlockEntity pass. Lines are derived from the live native
// ModelPart::render transform while its original BlockActor renderer runs.
// No collision box, BlockShape proxy or guessed facing is rendered here.
void renderActorWireframe(BaseActorRenderContext& ctx){
    auto segments=takeActorWireframeSegments();
    bool const f7=gPreviewEnabled.load(std::memory_order_relaxed);
    bool const f8=gCapturePreviewEnabled.load(std::memory_order_relaxed);
    if(!f7&&!f8)return;
    static Clock::time_point lastReport{};
    if(gPreviewMod && (lastReport.time_since_epoch().count()==0||
        Clock::now()-lastReport>std::chrono::seconds(8))){
        lastReport=Clock::now();
        auto const stats=actorWireframeStats();
        gPreviewMod->getLogger().info(
            "F7/F8 BlockEntity native wireframe: part_calls={}, with_cpu_geometry={}, "
            "valid_transforms={}, missing_geometry={}, invalid_transforms={}, "
            "total_segments={}, current_pass_segments={}, "
            "actor_data_constructed={}, destroyed={}, buffers_flushed={}, f7={}, f8={}",
            stats.partsCalled,stats.partsWithCpuGeometry,
            stats.partsWithValidTransform,stats.missingCpuGeometry,
            stats.discardedTransforms,stats.emittedSegments,segments.size(),
            stats.actorDataConstructed,stats.actorDataDestroyed,
            stats.actorBuffersFlushed,f7,f8);
    }
    if(segments.empty())return;
    auto* level=ctx.mClientInstance.getLevelRenderer();
    if(!level||!level->mLevelRendererPlayer)return;
    auto const& material=level->mLevelRendererPlayer->mOutlineSelectionMaterial.get();
    if(!material.mRenderMaterialInfoPtr.get())return;
    auto const camera=cameraPosition(ctx);
    auto const& view=ctx.mScreenContext.camera.viewMatrixStack.get().top()._m.get();
    glm::vec3 const eye{camera.x,camera.y,camera.z};
    glm::vec3 const eyeInView{view*glm::vec4(eye,1.f)};
    glm::vec3 const zeroInView{view*glm::vec4(0.f,0.f,0.f,1.f)};
    bool const viewAlreadySubtractsCamera=
        std::isfinite(eyeInView.x)&&std::isfinite(eyeInView.y)&&std::isfinite(eyeInView.z)&&
        std::isfinite(zeroInView.x)&&std::isfinite(zeroInView.y)&&std::isfinite(zeroInView.z)&&
        glm::length(eyeInView)+.5f<glm::length(zeroInView);
    auto& t=ctx.mScreenContext.tessellator;
    if(segments.size()>24000)segments.resize(24000);
    auto& shader=ctx.mScreenContext.currentShaderColor;
    auto const previous=shader.color.get();
    // F8 red = live BlockEntity geometry; F7 amber = same native model
    // visualized independently from the ordinary cyan block wireframe.
    shader.color=f8?mce::Color{1.f,.18f,.24f,1.f}:
                    mce::Color{1.f,.69f,.14f,1.f};
    shader.dirty=true;
    using Capture=decltype(dragon::RenderMetadata::mOffscreenCaptureDescription);
    static Capture empty{};
    try{
        t.begin(Tessellator::DebugContextCallback{},mce::PrimitiveMode::LineList,
                static_cast<int>(segments.size()*2),false);
        t.color(1.f,1.f,1.f,1.f);
        for(auto const& s:segments){
            for(auto const& p:{s.from,s.to}){
                t.tex2({.5f,.5f});
                t.vertex(p[0],p[1],p[2]);
            }
        }
        auto gpu=std::make_unique<mce::Mesh>(
            t.end(Tessellator::UploadMode::Buffered,
                  "BSC_NativeBlockActorWire",SupplementaryFieldAutoGenerationMode{0}));
        if(gpu && gpu->isValid()){
            auto stack=ctx.mScreenContext.camera.worldMatrixStack.get().pushIdentity(false);
            if(!viewAlreadySubtractsCamera)
                stack.mat->translate(-camera.x,-camera.y,-camera.z);
            auto texture=white(*level);
            gpu->renderMesh(ctx.mScreenContext,material,texture,0,
                            gpu->mVertexCount.get().value_or(0u),empty.get(),nullptr);
        }
    }catch(...){
        shader.color=previous;
        shader.dirty=true;
        throw;
    }
    shader.color=previous;
    shader.dirty=true;
}
LL_TYPE_INSTANCE_HOOK(BSCOverlayHook,ll::memory::HookPriority::Normal,
    LevelRendererPlayer,&LevelRendererPlayer::$renderBlockEntities,
    void,BaseActorRenderContext& ctx,bool alpha) {
    // Native renderers are invoked inside origin(). Reset per-frame data
    // before the opaque pass, then draw the captured actor geometry just
    // after the original native renderer (in both opaque and alpha passes).
    if(!alpha)beginActorWireframeFrame();
    if(gPreviewEnabled.load(std::memory_order_relaxed)||
       gCapturePreviewEnabled.load(std::memory_order_relaxed))
        updateActorWireframeCamera(cameraPosition(ctx));
    origin(ctx,alpha);
    try {
        if(!alpha) {
            {std::lock_guard guard(gResourceMutex);
             gResource=ctx.mScreenContext.tessellator.mBufferResourceService;}
            update(ctx);
        }
        if(gPreviewEnabled.load(std::memory_order_relaxed)||
           gCapturePreviewEnabled.load(std::memory_order_relaxed))
            renderActorWireframe(ctx);
        if(alpha)::bsc::render(ctx);
    }catch(std::exception const& e){
        OutputDebugStringA(("BSC preview disabled after render exception: "
                            +std::string(e.what())+"\n").c_str());
        gPreviewEnabled.store(false);
        reset();
    }
}
} // namespace
void setCapturePreviewEnabled(bool enabled){
    gCapturePreviewEnabled.store(enabled,std::memory_order_release);
    if(!enabled){
        std::lock_guard lock(gCaptureMutex);
        gCaptureSections.clear();
        gCaptureSegments=0;
        gCapturedActorPoses.clear();
        gCaptureService.reset();
    }
}
void addCapturePreview(BlockPos const& pos,Mesh const& mesh){
    if(!gCapturePreviewEnabled.load(std::memory_order_acquire)||mesh.vertices.empty())return;
    std::unique_lock lock(gCaptureMutex,std::try_to_lock);
    if(!lock.owns_lock() || gCaptureSegments>=MaxBlockCaptureSegments)return;
    CaptureKey const key{floorChunk(pos.x),floorChunk(pos.y),floorChunk(pos.z)};
    auto it=gCaptureSections.find(key);
    if(it==gCaptureSections.end()){
        if(gCaptureSections.size()>=MaxBlockCaptureSections)return;
        auto [added,inserted]=gCaptureSections.try_emplace(key);
        it=added;
        it->second.x=std::get<0>(key);
        it->second.z=std::get<2>(key);
        it->second.minY=std::get<1>(key)*16;
        it->second.maxY=it->second.minY+16;
    }
    auto& section=it->second;
    if(section.lines.size()>=MaxCaptureSectionSegments)return;
    auto const before=section.lines.size();
    consume(section,mesh,pos,false);
    section.lines.resize(std::min(section.lines.size(),
                          before+std::min(MaxCaptureSectionSegments-before,
                            MaxBlockCaptureSegments-gCaptureSegments)));
    gCaptureSegments+=section.lines.size()-before;
    if(section.lines.size()!=before){section.gpu.reset();section.ready=true;}
}
void addCaptureActorPreview(BlockPos const& pos,
                            std::span<ActorWireSegment const> segments){
    if(!gCapturePreviewEnabled.load(std::memory_order_acquire)||segments.empty())return;
    std::unique_lock lock(gCaptureMutex,std::try_to_lock);
    if(!lock.owns_lock()||gCaptureSegments>=MaxCaptureSegments)return;
    std::array<int,3> const actorKey{pos.x,pos.y,pos.z};
    auto posesIt=gCapturedActorPoses.find(actorKey);
    if(posesIt!=gCapturedActorPoses.end()&&
       posesIt->second.size()>=MaxActorPoseVariants)return;
    // Stable quantized world-position-independent geometry signature.
    // Distinguish actual extensions/rotation from repeated render passes.
    std::uint64_t signature=14695981039346656037ULL;
    auto mix=[&](std::uint64_t value){
        signature^=value;signature*=1099511628211ULL;
    };
    mix(segments.size());
    for(auto const& edge:segments){
        for(auto const& point:{edge.from,edge.to}){
            for(unsigned dimension=0;dimension<3;++dimension){
                auto const relative=point[dimension]-static_cast<float>(actorKey[dimension]);
                if(!std::isfinite(relative)||std::abs(relative)>16.f)return;
                auto const scaled=std::llround(static_cast<double>(relative)*1024.);
                mix(static_cast<std::uint64_t>(scaled));
            }
        }
    }
    if(posesIt!=gCapturedActorPoses.end()&&posesIt->second.contains(signature))return;
    CaptureKey const key{floorChunk(pos.x),floorChunk(pos.y),floorChunk(pos.z)};
    auto it=gCaptureSections.find(key);
    if(it==gCaptureSections.end()){
        if(gCaptureSections.size()>=MaxCaptureSections)return;
        it=gCaptureSections.try_emplace(key).first;
        it->second.x=std::get<0>(key);
        it->second.z=std::get<2>(key);
        it->second.minY=std::get<1>(key)*16;
        it->second.maxY=it->second.minY+16;
    }
    auto& section=it->second;
    auto const room=std::min(MaxCaptureSectionSegments-section.lines.size(),
                              MaxCaptureSegments-gCaptureSegments);
    auto const take=std::min(room,segments.size());
    if(take==0)return;
    std::size_t added{};
    float const baseX=static_cast<float>(section.x*16);
    float const baseY=static_cast<float>(section.minY);
    float const baseZ=static_cast<float>(section.z*16);
    for(std::size_t i=0;i<segments.size() && added<take;++i){
        auto const& wire=segments[i];
        auto local=[](std::array<float,3> const& p,float x,float y,float z){
            return Vec3{p[0]-x,p[1]-y,p[2]-z};
        };
        auto a=local(wire.from,baseX,baseY,baseZ);
        auto b=local(wire.to,baseX,baseY,baseZ);
        if(!std::isfinite(a.x)||!std::isfinite(a.y)||!std::isfinite(a.z)||
           !std::isfinite(b.x)||!std::isfinite(b.y)||!std::isfinite(b.z))continue;
        section.lines.push_back({a,b});
        ++added;
    }
    if(added){
        gCaptureSegments+=added;
        gCapturedActorPoses[actorKey].insert(signature);
        section.gpu.reset();
        section.ready=true;
    }
}
std::weak_ptr<mce::BufferResourceService> renderBufferService(){
    std::lock_guard guard(gResourceMutex);
    return gResource;
}
bool installPreview(ll::mod::NativeMod& mod){
    // Idempotent: F7 and F8 can both request the overlay during a session.
    // Never patch the renderer in LeviLamina's synchronous mod enable path.
    if(gOverlayInstalled)return true;
    gPreviewMod=&mod;
    gLastTransformDiagnostic={};
    gOverlayInstalled=(BSCOverlayHook::hook()==0);
    if(!gOverlayInstalled)gPreviewMod=nullptr;
    return gOverlayInstalled;
}
void uninstallPreview(){
    // installPreview can have been skipped entirely (no F7/F8 requested).
    // Avoid asking the hook framework to undo a hook never installed.
    if(gOverlayInstalled){
        BSCOverlayHook::unhook();
        gOverlayInstalled=false;
    }
    reset();
    gPreviewEnabled.store(false);
    gPreviewMod=nullptr;
}
} // namespace bsc
