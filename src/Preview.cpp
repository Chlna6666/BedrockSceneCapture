#include "Preview.h"
#include "MeshUtils.h"
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
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace bsc {
std::atomic_bool gPreviewEnabled{false};
namespace {
using Clock=std::chrono::steady_clock;
using Key=std::pair<int,int>;
constexpr std::size_t MaxLinesPerChunk=60000;
constexpr std::size_t MaxVertexMarksPerChunk=2000;
constexpr unsigned MaxChunkMeshes=512;
constexpr unsigned RefreshFrames=120;
constexpr unsigned RebuildFrames=900;

struct ChunkWire {
    int x{},z{},minY{},maxY{};
    std::uint32_t cell{};
    std::uint64_t builtFrame{};
    bool queued{}, building{},ready{},truncated{};
    std::vector<std::array<Vec3,2>> lines;
    std::unique_ptr<mce::Mesh> gpu;
};
std::mutex gResourceMutex;
std::weak_ptr<mce::BufferResourceService> gResource;
std::unique_ptr<Tessellator> gInputTess;
std::unique_ptr<BlockTessellator> gBlockTess;
std::map<Key,ChunkWire> gChunks;
std::deque<Key> gQueue;
std::optional<Key> gActive;
LocalPlayer* gPlayer{};
BlockSource* gRegion{};
std::uint64_t gFrame{};
unsigned gVertexMarks{};
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
    gPlayer=nullptr;
    gRegion=nullptr;
    gFrame=0;
    gWhiteTexture=mce::TexturePtr{};
    gWhiteReady=false;
}
Vec3 cameraPosition(BaseActorRenderContext const& ctx){
    // Existing 26.51 render-context camera location: validate when changing SDK.
    auto const* impl=reinterpret_cast<float const*>(ctx.mImpl.get());
    return impl?Vec3{impl[10],impl[11],impl[12]}:Vec3{};
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
void consume(ChunkWire& chunk,Mesh const& mesh,BlockPos const& blockPos) {
    auto at=[&](std::uint32_t i)->Vec3{
        auto const& v=mesh.vertices[i];
        return {static_cast<float>(blockPos.x-chunk.x*16)+v.x,
                static_cast<float>(blockPos.y-chunk.minY)+v.y,
                static_cast<float>(blockPos.z-chunk.z*16)+v.z};
    };
    auto triangle=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c){
        if(a>=mesh.vertices.size()||b>=mesh.vertices.size()||c>=mesh.vertices.size())return;
        auto va=at(a),vb=at(b),vc=at(c);
        line(chunk,va,vb);line(chunk,vb,vc);line(chunk,vc,va);
    };
    auto const& indices=mesh.indices;
    if(mesh.primitive==1){
        std::size_t n=indices.empty()?mesh.vertices.size():indices.size();
        auto index=[&](std::size_t i)->std::uint32_t{
            return indices.empty()?static_cast<std::uint32_t>(i):indices[i];
        };
        for(std::size_t i=0;i+3<n;i+=4) {
            auto a=index(i),b=index(i+1),c=index(i+2),d=index(i+3);
            triangle(a,b,c);triangle(a,c,d);
        }
    }else if(mesh.primitive==2){
        std::size_t n=indices.empty()?mesh.vertices.size():indices.size();
        auto index=[&](std::size_t i)->std::uint32_t{
            return indices.empty()?static_cast<std::uint32_t>(i):indices[i];
        };
        for(std::size_t i=0;i+2<n;i+=3) triangle(index(i),index(i+1),index(i+2));
    }
    for(std::size_t i=0;i<mesh.vertices.size() &&
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
                long long dx=static_cast<long long>(key.first)-center.x/16;
                long long dz=static_cast<long long>(key.second)-center.z/16;
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
        } else if(it->second.ready && gFrame-it->second.builtFrame>RebuildFrames) {
            auto dx=key.first-center.x/16;
            auto dz=key.second-center.z/16;
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
    if(player!=gPlayer || &region!=gRegion){
        reset();
        gPlayer=player;
        gRegion=&region;
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
            long long dx=static_cast<long long>(it->first)-center.x/16;
            long long dz=static_cast<long long>(it->second)-center.z/16;
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
    unsigned nativeCalls=0;
    for(unsigned i=0;i<512 && chunk.cell<volume &&
        nativeCalls<2 && Clock::now()-started<std::chrono::milliseconds(2);++i){
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
    auto const forward=cam.mForward;
    auto const right=cam.mRight;
    auto const up=cam.mUp;
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
void render(BaseActorRenderContext& ctx){
    if(!gPreviewEnabled.load())return;
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
    bool uploaded=false;
    for(auto& [key,chunk]:gChunks){
        if(!chunk.ready || !visible(ctx,chunk,camera))continue;
        if(!chunk.gpu && !chunk.lines.empty() && !uploaded){
            if(chunk.lines.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()/2))
                continue;
            t.begin(Tessellator::DebugContextCallback{},mce::PrimitiveMode::LineList,
                    static_cast<int>(chunk.lines.size()*2),false);
            t.color(1.f,1.f,1.f,1.f);
            for(auto const& ends:chunk.lines)
                for(auto const& point:ends){t.tex2({0.5f,0.5f});t.vertex(point.x,point.y,point.z);}
            chunk.gpu=std::make_unique<mce::Mesh>(t.end(Tessellator::UploadMode::Buffered,
                "BSC_ChunkWire",SupplementaryFieldAutoGenerationMode{0}));
            chunk.lines.clear();
            uploaded=true;
        }
        if(!chunk.gpu||!chunk.gpu->isValid())continue;
        auto stack=ctx.mScreenContext.camera.worldMatrixStack.get().push(false);
        stack.mat->translate(static_cast<float>(chunk.x*16)-camera.x,
                             static_cast<float>(chunk.minY)-camera.y,
                             static_cast<float>(chunk.z*16)-camera.z);
        chunk.gpu->renderMesh(ctx.mScreenContext,material,texture,0,
            chunk.gpu->mVertexCount.get().value_or(0u),empty.get(),nullptr);
    }
    shader.color=previous;
    shader.dirty=true;
}
LL_TYPE_INSTANCE_HOOK(BSCOverlayHook,ll::memory::HookPriority::Normal,
    LevelRendererPlayer,&LevelRendererPlayer::$renderBlockEntities,
    void,BaseActorRenderContext& ctx,bool alpha) {
    origin(ctx,alpha);
    if(!alpha) {
        {std::lock_guard guard(gResourceMutex);
         gResource=ctx.mScreenContext.tessellator.mBufferResourceService;}
        update(ctx);
    } else ::bsc::render(ctx);
}
} // namespace
std::weak_ptr<mce::BufferResourceService> renderBufferService(){
    std::lock_guard guard(gResourceMutex);
    return gResource;
}
bool installPreview(){return BSCOverlayHook::hook()==0;}
void uninstallPreview(){BSCOverlayHook::unhook();reset();gPreviewEnabled.store(false);}
} // namespace bsc
