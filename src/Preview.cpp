#include "Preview.h"
#include "MeshUtils.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
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
#include "mc/deps/core_graphics/TextureSetLayerType.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/minecraft_renderer/renderer/IsMissingTexture.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/framebuilder/dragon/RenderMetadata.h"
#include "mc/deps/renderer/ShaderColor.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>
#include <variant>
namespace bsc {
std::atomic_bool gPreviewEnabled{false};
namespace {
std::unique_ptr<mce::Mesh> gWireMesh;
std::unique_ptr<Tessellator> gInputTess;
std::unique_ptr<BlockTessellator> gBlockTess;
LocalPlayer* gPlayer{};
BlockSource* gRegion{};
BlockPos gOrigin{};
int gOffset{},gMaxOffset{};
constexpr int R=2; // radius 2 => 5x5x5 blocks; capped at 125 per rebuild
constexpr std::size_t MaxLines=120000;
constexpr std::size_t MaxMarks=5000;
std::vector<std::array<Vec3,2>> gLines;
bool gBuilding{},gReady{};
mce::TexturePtr gWhiteTexture{};
bool gWhiteReady{};
using TextureVariant=std::variant<std::monostate,mce::TexturePtr,mce::ClientTexture,mce::ServerTexture>;
TextureVariant white(LevelRenderer& level) {
    if(!gWhiteReady) {
        auto const& group=level.mTextureGroup.get();
        if(group) {
            auto texture=group->getTexture(
                ResourceLocation{Core::PathView{"textures/ui/white_background"}},
                false,std::nullopt,cg::TextureSetLayerType::Color);
            if(texture.mClientTexture && texture.mClientTexture->mIsMissingTexture!=IsMissingTexture::Yes){
                gWhiteTexture=std::move(texture);gWhiteReady=true;
            }
        }
    }
    if(gWhiteReady)return TextureVariant{gWhiteTexture};
    return {};
}
void reset() {
    gWireMesh.reset();gInputTess.reset();gBlockTess.reset();
    gLines.clear();gBuilding=false;gReady=false;gPlayer=nullptr;gRegion=nullptr;
}
Vec3 cameraPosition(BaseActorRenderContext const& ctx){
    // Proven in LHolo 26.51 render context; check the layout again on SDK upgrades.
    auto const* impl=reinterpret_cast<float const*>(ctx.mImpl.get());
    return impl?Vec3{impl[10],impl[11],impl[12]}:Vec3{};
}
void line(Vec3 const& a,Vec3 const& b) {
    if(gLines.size()<MaxLines)gLines.push_back({a,b});
}
void vertexMark(Vec3 const& v) {
    constexpr float d=0.027f;
    line({v.x-d,v.y,v.z},{v.x+d,v.y,v.z});
    line({v.x,v.y-d,v.z},{v.x,v.y+d,v.z});
    line({v.x,v.y,v.z-d},{v.x,v.y,v.z+d});
}
void consumeMesh(Mesh const& mesh,BlockPos const& p) {
    auto at=[&](std::uint32_t i)->Vec3 {
        auto const& v=mesh.vertices[i];
        return {static_cast<float>(p.x-gOrigin.x)+v.x,
                static_cast<float>(p.y-gOrigin.y)+v.y,
                static_cast<float>(p.z-gOrigin.z)+v.z};
    };
    auto edgeTri=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c){
        if(a>=mesh.vertices.size()||b>=mesh.vertices.size()||c>=mesh.vertices.size())return;
        auto va=at(a),vb=at(b),vc=at(c);
        line(va,vb);line(vb,vc);line(vc,va);
    };
    if(!mesh.indices.empty()){
        // Native MeshData commonly supplies quad topology; preserve each quad's
        // triangulation by adding its diagonal explicitly.
        for(std::size_t i=0;i+3<mesh.indices.size();i+=4){
            auto a=mesh.indices[i],b=mesh.indices[i+1],
                 c=mesh.indices[i+2],d=mesh.indices[i+3];
            edgeTri(a,b,c);edgeTri(a,c,d);
        }
    }else {
        for(std::uint32_t i=0;i+3<mesh.vertices.size();i+=4){
            edgeTri(i,i+1,i+2);edgeTri(i,i+2,i+3);
        }
    }
    std::size_t currentMarks=gLines.size()/3;
    if(currentMarks<MaxMarks)
        for(std::size_t i=0;i<mesh.vertices.size()&&i<MaxMarks-currentMarks;++i)
            vertexMark(at(static_cast<std::uint32_t>(i)));
}
void update(BaseActorRenderContext& ctx) {
    if(!gPreviewEnabled.load()){if(gBuilding||gWireMesh) reset();return;}
    auto* player=ctx.mClientInstance.getLocalPlayer();
    if(!player){reset();return;}
    auto& region=player->getDimensionBlockSource();
    auto center=player->getFeetBlockPos();
    BlockPos origin{center.x-R,center.y-R,center.z-R};
    if(gPlayer!=player||gRegion!=&region||gOrigin!=origin){
        reset();
        gPlayer=player;gRegion=&region;gOrigin=origin;
        gBlockTess=std::make_unique<BlockTessellator>(&region);
        gBlockTess->mCachedGetBlock.get()=[&region](BlockPos const& pos)->Block const&{
            return region.getBlock(pos);
        };
        gInputTess=std::make_unique<Tessellator>(ctx.mScreenContext.tessellator.mBufferResourceService);
        gBuilding=true;gOffset=0;gMaxOffset=(2*R+1)*(2*R+1)*(2*R+1);
    }
    if(!gBuilding) return;
    // Strict bounded per-frame model reads: 8 blocks at a time.
    for(int n=0;n<8&&gOffset<gMaxOffset;++n,++gOffset){
        int localX=gOffset%(2*R+1);
        int localZ=(gOffset/(2*R+1))%(2*R+1);
        int localY=gOffset/((2*R+1)*(2*R+1));
        BlockPos p{gOrigin.x+localX,gOrigin.y+localY,gOrigin.z+localZ};
        if(!region.areChunksFullyLoaded(p,0))continue;
        auto const& block=region.getBlock(p);
        if(block.isAir())continue;
        ChunkData out;
        captureMesh(region,*gBlockTess,*gInputTess,block,p,0,out);
        for(auto const& mesh:out.meshes)consumeMesh(mesh,p);
    }
    if(gOffset>=gMaxOffset) {
        gBuilding=false;gReady=true;
        gBlockTess.reset();gInputTess.reset();
    }
}
void render(BaseActorRenderContext& ctx) {
    if(!gPreviewEnabled.load()||!gReady)return;
    auto* level=ctx.mClientInstance.getLevelRenderer();
    if(!level||!level->mLevelRendererPlayer)return;
    auto const& material=level->mLevelRendererPlayer->mOutlineSelectionMaterial.get();
    if(!material.mRenderMaterialInfoPtr.get())return;
    if(!gWireMesh && !gLines.empty()){
        auto& t=ctx.mScreenContext.tessellator;
        if(gLines.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()/2))return;
        t.begin(Tessellator::DebugContextCallback{},mce::PrimitiveMode::LineList,
                static_cast<int>(gLines.size()*2),false);
        t.color(1.f,1.f,1.f,1.f);
        for(auto const& ends:gLines){
            for(auto const& v:ends){t.tex2({0.5f,0.5f});t.vertex(v.x,v.y,v.z);}
        }
        gWireMesh=std::make_unique<mce::Mesh>(t.end(Tessellator::UploadMode::Buffered,
            "BSC_TriangleVertexWireframe",SupplementaryFieldAutoGenerationMode{0}));
        gLines.clear();
    }
    if(!gWireMesh||!gWireMesh->isValid())return;
    auto texture=white(*level);
    auto const camera=cameraPosition(ctx);
    auto matrix=ctx.mScreenContext.camera.worldMatrixStack.get().push(false);
    matrix.mat->translate(static_cast<float>(gOrigin.x)-camera.x,
                          static_cast<float>(gOrigin.y)-camera.y,
                          static_cast<float>(gOrigin.z)-camera.z);
    auto& shader=ctx.mScreenContext.currentShaderColor;
    auto previous=shader.color.get();
    shader.color=mce::Color{0.14f,0.96f,0.83f,1.f};shader.dirty=true;
    using Capture=decltype(dragon::RenderMetadata::mOffscreenCaptureDescription);
    static Capture empty{};
    gWireMesh->renderMesh(ctx.mScreenContext,material,texture,0,
        gWireMesh->mVertexCount.get().value_or(0u),empty.get(),nullptr);
    shader.color=previous;shader.dirty=true;
}
LL_TYPE_INSTANCE_HOOK(BSCOverlayHook,ll::memory::HookPriority::Normal,
    LevelRendererPlayer,&LevelRendererPlayer::$renderBlockEntities,
    void,BaseActorRenderContext& ctx,bool alpha) {
    origin(ctx,alpha);
    if(!alpha){ update(ctx); }
    else { render(ctx); }
}
}
bool installPreview(){return BSCOverlayHook::hook()==0;}
void uninstallPreview(){BSCOverlayHook::unhook();reset();gPreviewEnabled.store(false);}
}