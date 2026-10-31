#include "MeshUtils.h"
#include "NativeCoordinates.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/deps/minecraft_renderer/renderer/MeshData.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
namespace bsc {
void captureMesh(BlockSource& region,BlockTessellator& blocks,Tessellator& tess,
    Block const& block,BlockPos const& pos,std::uint32_t cell,ChunkData& output) {
    if(block.isAir())return;
    tess.begin(Tessellator::DebugContextCallback{},mce::PrimitiveMode::QuadList,64,false);
    blocks.setRegion(region);
    bool ok=blocks.tessellateInWorld(tess,block,pos,true);
    auto& raw=tess.mMeshData.get();
    if(raw.mPositions.get().empty()&&!ok && block.getTypeName()=="minecraft:bubble_column")
        blocks.tessellateBubbleColumnInWorld(tess,block,pos);
    auto const& points=raw.mPositions.get();
    if(!points.empty()) {
        Mesh mesh;mesh.cell=cell;
        mesh.primitive=static_cast<std::uint32_t>(raw.mMode);
        auto const* graphics=BlockGraphics::getForBlock(block);
        mesh.layer=graphics?static_cast<std::uint32_t>(graphics->getRenderLayer(region,pos)):0;
        mesh.vertices.reserve(points.size());
        auto const worldSpace=nativePositionsInWorldSpace(points,pos);
        // Reject a renderer whose coordinate convention cannot be normalized;
        // a wild offset would invalidate the entire F7 line overlay.
        for(auto const& point:points){
            if(!plausibleNativeLocalVertex(point,pos,worldSpace)){
                ++output.missingMeshes;
                (void)tess.end(Tessellator::UploadMode::Never,"BSC_Capture",SupplementaryFieldAutoGenerationMode{0});
                return;
            }
        }
        auto const& normals=raw.mNormals.get();
        auto const& uv=raw.mTextureUVs[0].get();
        mesh.uvValid=(uv.size()==points.size());
        auto const& colors=raw.mColors.get();
        for(std::size_t i=0;i<points.size();++i){
            Vertex v;
            v.x=points[i].x-(worldSpace?static_cast<float>(pos.x):0.f);
            v.y=points[i].y-(worldSpace?static_cast<float>(pos.y):0.f);
            v.z=points[i].z-(worldSpace?static_cast<float>(pos.z):0.f);
            if(normals.size()==points.size()){
                v.nx=normals[i].x;v.ny=normals[i].y;v.nz=normals[i].z;
            }
            if(uv.size()==points.size()){v.u=uv[i].x;v.v=uv[i].y;}
            if(colors.size()==points.size())v.abgr=colors[i];
            mesh.vertices.emplace_back(v);
        }
        mesh.indices.assign(raw.mIndices.get().begin(),raw.mIndices.get().end());
        output.meshes.emplace_back(std::move(mesh));
    }else{++output.missingMeshes;}
    (void)tess.end(Tessellator::UploadMode::Never,"BSC_Capture",SupplementaryFieldAutoGenerationMode{0});
}
}