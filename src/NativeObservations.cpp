#include "NativeObservations.h"
#include "NativeCoordinates.h"
#include "Preview.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockShape.h"
#include "mc/client/model/models/Model.h"
#include "mc/client/model/Geometry.h"
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
#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <vector>
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
    std::size_t operator()(std::array<std::uint32_t,27> const& values) const noexcept {
        std::size_t h=0x9e3779b9u;
        for(auto v:values)h ^= static_cast<std::size_t>(v)+0x9e3779b9u+(h<<6)+(h>>2);
        return h;
    }
};
std::unordered_set<std::array<std::uint32_t,27>,IdHash> seen;
std::vector<NativeObservation> pending;
std::unordered_map<std::string,std::string> actorModels;
// Capture native box inputs at model construction, before their CPU Cube
// definitions can be compacted into GPU meshes. These are not synthetic AABBs.
struct SourceBoxes {bool fromGeometryResource{};std::vector<std::string> boxes;};
std::mutex sourceBoxesMutex;
std::unordered_map<ModelPart const*,SourceBoxes> sourceBoxes;
constexpr std::size_t MaxSourceParts=8192;
constexpr std::size_t MaxSourceBoxesPerPart=256;
constexpr std::size_t MaxDistinctScenes=8192;
constexpr std::size_t MaxDistinctPerShape=768;
std::unordered_map<int,std::size_t> shapesObserved;
std::unordered_set<std::string> recordedActorLabels;
std::unordered_map<std::string,std::chrono::steady_clock::time_point> actorLastAttempt;
constexpr std::size_t MaxTotalVertices=2'000'000;
std::size_t recordedVertices{};
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
std::string modelJson(Model const& model,
                      std::initializer_list<ModelPart const*> explicitParts){
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
     "\"coordinate_space\":\"model_local\",\"animation_pose\":\"not_captured\",\"parts\":[";
    bool firstPart=true;
    std::size_t cubes=0,polyFaces=0,sourceBoxCount=0,unresolvedCubes=0;
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
            auto const& size=cube.mSize.get();
            // Some 1.26.51.1 renderers hold compiled Cube polygons but expose
            // zero mSize through this SDK. Never serialize them as real boxes.
            if(!std::isfinite(size.x)||!std::isfinite(size.y)||!std::isfinite(size.z) ||
               size.x==0.f||size.y==0.f||size.z==0.f){
                ++unresolvedCubes;
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
                }
                o<<']';
                sourceBoxCount+=it->second.boxes.size();
            }
        }
        // Some data-driven BlockActors use an indexed polygon mesh rather
        // than procedural Cubes. Preserve the original vertex/normal/UV
        // indices and topology instead of approximating it with a box.
        auto const& poly=part->mPolyMesh.get();
        auto const& positions=poly.mPositions.get();
        auto const& normals=poly.mNormals.get();
        auto const& uvs=poly.mUVs.get();
        auto const& tris=poly.mTris.get();
        auto const& quads=poly.mQuads.get();
        bool valid=!positions.empty() && positions.size()<=65536 &&
                   normals.size()<=65536 && uvs.size()<=65536 &&
                   tris.size()+quads.size()<=65536;
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
            o<<"],\"uv_normalized\":"<<(poly.mAreUVsNormalized?"true":"false");
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
     <<",\"geometry_complete\":"<<(unresolvedCubes==0?"true":"false")<<"}\n";
    return cubes||polyFaces||sourceBoxCount?o.str():std::string{};
}
void recordActor(char const* label,Model const& model,
                 std::initializer_list<ModelPart const*> explicitParts){
    if(!armed.load(std::memory_order_relaxed))return;
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock() || recordedActorLabels.contains(label))return;
    // An empty model may become valid after resource initialization.
    // Retry at most once a second instead of permanently blacklisting it.
    auto const now=std::chrono::steady_clock::now();
    auto const it=actorLastAttempt.find(label);
    if(it!=actorLastAttempt.end() && now-it->second<std::chrono::seconds(1))return;
    actorLastAttempt[std::string(label)]=now;
    auto json=modelJson(model,explicitParts);
    if(json.empty())return;
    actorModels.emplace(label,std::move(json));
    recordedActorLabels.emplace(label);
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
    auto const* gfx=BlockGraphics::getForBlock(block);
    if(!gfx || !requiredWorldShape(gfx->mBlockShape))return;
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
    std::unique_lock lock(mutex,std::try_to_lock);
    auto const shape=static_cast<int>(gfx->mBlockShape);
    if(!lock.owns_lock()||seen.size()>=MaxDistinctScenes||
       shapesObserved[shape]>=MaxDistinctPerShape||
       recordedVertices+points.size()-fromPos>MaxTotalVertices||
       !seen.emplace(ids).second)return;
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
    reconstructMissingNormals(mesh);
    recordedVertices+=mesh.vertices.size();
    ++shapesObserved[shape];
    addCapturePreview(pos,mesh);
    pending.push_back(NativeObservation{ids,std::move(mesh)});
}
// Capture at construction time: numerous BlockActor models retain only a
// compiled render mesh by the time the F8 observation begins.
LL_TYPE_INSTANCE_HOOK(BSCPartAddBoxHook,ll::memory::HookPriority::Normal,
    ModelPart,&ModelPart::addBox,void,
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
    if(dest.fromGeometryResource || dest.boxes.size()>=MaxSourceBoxesPerPart)return;
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
LL_TYPE_INSTANCE_HOOK(BSCPartLoadBoxesHook,ll::memory::HookPriority::Normal,
    ModelPart,&ModelPart::loadBoxes,void,
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
        auto const& faces=box.mFaceUVs.get();
        if(hasFaceUVs)for(std::size_t i=0;i<faces.size();++i){
            auto const& uv=faces[i].mUV.get();
            auto const& uvSize=faces[i].mUVSize.get();
            if(!finite(uv.x)||!finite(uv.y)||!finite(uvSize.x)||!finite(uvSize.y)){
                completeFaceUVs=false;
                continue;
            }
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
        out<<"],\"legacy_texture_offset\":["<<tex.x<<','<<tex.y<<']'
           <<",\"uv_complete\":"<<(completeFaceUVs?"true":"false")<<'}';
        dest.boxes.emplace_back(out.str());
    }
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
    auto const& small=this->mChestModel.get();
    auto const& large=this->mLargeChestModel.get();
    recordActor("chest_normal",small,
        {&small.mLid.get(),&small.mBottom.get(),&small.mLock.get()});
    recordActor("chest_large",large,
        {&large.mLid.get(),&large.mBottom.get(),&large.mLock.get()});
}
LL_TYPE_INSTANCE_HOOK(BSCSignModelHook,ll::memory::HookPriority::Normal,
    SignRenderer,&SignRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    origin(ctx,data);
    auto const& model=this->mSignModel.get();
    recordActor("standing_sign",model,{&model.mCube.get(),&model.mCube2.get()});
}
LL_TYPE_INSTANCE_HOOK(BSCHangingSignModelHook,ll::memory::HookPriority::Normal,
    HangingSignRenderer,&HangingSignRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    origin(ctx,data);
    auto const& model=this->mHangingSignModel.get();
    recordActor("hanging_sign",model,
        {&model.mPlank.get(),&model.mBoard.get(),&model.mVChain.get(),
         &model.mChainL1.get(),&model.mChainL2.get(),
         &model.mChainR1.get(),&model.mChainR2.get()});
}
LL_TYPE_INSTANCE_HOOK(BSCBedModelHook,ll::memory::HookPriority::Normal,
    BedRenderer,&BedRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    origin(ctx,data);
    auto const& model=this->mBed.get();
    recordActor("bed",model,{&model.mBody.get(),&model.mLeg0.get(),
        &model.mLeg1.get(),&model.mLeg2.get(),&model.mLeg3.get()});
}
LL_TYPE_INSTANCE_HOOK(BSCShulkerModelHook,ll::memory::HookPriority::Normal,
    ShulkerBoxRenderer,&ShulkerBoxRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    origin(ctx,data);
    auto const& model=this->mShulkerBoxModel.get();
    recordActor("shulker_box",model,{&model.mLid.get(),&model.mBase.get()});
}
LL_TYPE_INSTANCE_HOOK(BSCBellModelHook,ll::memory::HookPriority::Normal,
    BellBlockActorRenderer,&BellBlockActorRenderer::$render,void,
    BaseActorRenderContext& ctx,BlockActorRenderData& data) {
    origin(ctx,data);
    auto const& model=this->mBell.get();
    recordActor("bell",model,{&model.mBellBody.get(),&model.mBellBase.get()});
}
bool installedPartAdd{},installedPartLoad{};
bool installedWorld{},installedChest{},installedSign{},installedHanging{},
     installedBed{},installedShulker{},installedBell{};
}
bool installNativeObservations(ll::mod::NativeMod& mod){
    // Run only on an explicit in-world F8 request, never under mod enable.
    // Log BEFORE each symbol resolution / patch so a stuck hook can be
    // identified from the last durable message.
    if(installedWorld)return true;
    auto& logger=mod.getLogger();
    auto install=[&](char const* label,bool& installed,auto patch){
        logger.info("F8 native hook install BEGIN: {}",label);
        installed=(patch()==0);
        if(installed)logger.info("F8 native hook install OK: {}",label);
        else logger.warn("F8 native hook install FAILED: {}",label);
    };
    // Core tessellation hook is installed first. Optional ModelPart hooks
    // are last: they are the most invasive and were previously installed
    // synchronously in LeviLamina's startup path.
    install("BlockTessellator::tessellateInWorld",installedWorld,
        []{return BSCNativeBlockHook::hook();});
    install("ChestRenderer::render",installedChest,
        []{return BSCChestModelHook::hook();});
    install("SignRenderer::render",installedSign,
        []{return BSCSignModelHook::hook();});
    install("HangingSignRenderer::render",installedHanging,
        []{return BSCHangingSignModelHook::hook();});
    install("BedRenderer::render",installedBed,
        []{return BSCBedModelHook::hook();});
    install("ShulkerBoxRenderer::render",installedShulker,
        []{return BSCShulkerModelHook::hook();});
    install("BellBlockActorRenderer::render",installedBell,
        []{return BSCBellModelHook::hook();});
    install("ModelPart::addBox",installedPartAdd,
        []{return BSCPartAddBoxHook::hook();});
    install("ModelPart::loadBoxes",installedPartLoad,
        []{return BSCPartLoadBoxesHook::hook();});
    logger.info("F8 native hooks initialized: world={}, actors={}/6, source_box_hooks={}/2",
        installedWorld,
        static_cast<int>(installedChest)+static_cast<int>(installedSign)+
        static_cast<int>(installedHanging)+static_cast<int>(installedBed)+
        static_cast<int>(installedShulker)+static_cast<int>(installedBell),
        static_cast<int>(installedPartAdd)+static_cast<int>(installedPartLoad));
    return installedWorld;
}
ActorCaptureDiagnostics actorCaptureDiagnostics(){
    ActorCaptureDiagnostics diagnostics;
    diagnostics.hooksInstalled={installedChest,installedSign,installedHanging,
                                installedBed,installedShulker,installedBell};
    std::lock_guard lock(mutex);
    diagnostics.capturedModels=recordedActorLabels.size();
    diagnostics.sourceAddBoxHook=installedPartAdd;
    diagnostics.sourceLoadBoxesHook=installedPartLoad;
    {
        std::lock_guard sourceLock(sourceBoxesMutex);
        diagnostics.retainedSourceParts=sourceBoxes.size();
        for(auto const& [part,boxes]:sourceBoxes)
            diagnostics.retainedSourceBoxes+=boxes.boxes.size();
    }
    for(auto const& [label,lastAttempt]:actorLastAttempt){
        if(!recordedActorLabels.contains(label))
            diagnostics.observedWithoutGeometry.push_back(label);
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
    recordedVertices=0;
    shapesObserved.clear();
    recordedActorLabels.clear();
    actorLastAttempt.clear();
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
    if(installedPartLoad){BSCPartLoadBoxesHook::unhook();installedPartLoad=false;}
    if(installedPartAdd){BSCPartAddBoxHook::unhook();installedPartAdd=false;}
    {std::lock_guard lock(sourceBoxesMutex);sourceBoxes.clear();}
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
