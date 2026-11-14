#include "GeometryResources.h"
#include "mc/client/model/GeometryGroup.h"
#include "mc/client/model/GeometryInfo.h"
#include "mc/client/model/Geometry.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace bsc {
namespace {
constexpr std::size_t MaxResourceModels=2048;
constexpr std::size_t MaxNodesPerResource=256;
constexpr std::size_t MaxBoxesPerResource=4096;
constexpr std::size_t MaxPolyVertices=65536;
constexpr std::size_t MaxPolyFaces=65536;
std::string quoted(std::string_view value){
    std::ostringstream out;
    out<<'"';
    constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:value){
        switch(c){
        case '"':out<<"\\\"";break;
        case '\\':out<<"\\\\";break;
        case '\n':out<<"\\n";break;
        case '\r':out<<"\\r";break;
        case '\t':out<<"\\t";break;
        default:
            if(c<0x20){out<<"\\u00"<<hex[c>>4]<<hex[c&15];}
            else out<<c;
        }
    }
    out<<'"';
    return out.str();
}
template<class T>
bool finite3(T const& v){
    return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);
}
template<class T>
void vec3(std::ostream& out,T const& v){out<<'['<<v.x<<','<<v.y<<','<<v.z<<']';}
template<class T>
void vec2(std::ostream& out,T const& v){out<<'['<<v.x<<','<<v.y<<']';}
std::string serialize(Geometry const& geometry){
    auto const& nodes=geometry.mNodes.get();
    if(nodes.empty()||nodes.size()>MaxNodesPerResource)return {};
    std::size_t boxCount=0;
    for(auto const& [name,node]:nodes){
        if(node.mBoxes.get().size()>MaxBoxesPerResource-boxCount)return {};
        boxCount+=node.mBoxes.get().size();
    }
    auto const& tex=geometry.mTextureDimensions.get();
    bool complete=std::isfinite(tex.x)&&std::isfinite(tex.y)&&tex.x>0.f&&tex.y>0.f;
    std::size_t polygonFaces=0;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out<<std::setprecision(std::numeric_limits<float>::max_digits10);
    out<<"{\"schema\":1,\"source\":\"native_geometry_group\","
         "\"coordinate_space\":\"resource_bone_local\","
         "\"association_verified\":false,\"source_file\":"
       <<quoted(geometry.mSourceFilePathWithExtension.get())
       <<",\"texture_size\":";vec2(out,tex);
    out<<",\"nodes\":[";
    bool firstNode=true;
    for(auto const& [name,node]:nodes){
        if(!firstNode)out<<',';
        firstNode=false;
        out<<"{\"name\":"<<quoted(name)
           <<",\"parent\":"<<quoted(node.mParentName.get())
           <<",\"mirror\":"<<(node.mMirror?"true":"false")
           <<",\"visible\":"<<(node.mNeverRender?"false":"true")
           <<",\"pivot\":";vec3(out,node.mPivot.get());
        out<<",\"rotation\":";vec3(out,node.mRot.get());
        out<<",\"scale\":";vec3(out,node.mScale.get());
        if(!finite3(node.mPivot.get())||!finite3(node.mRot.get())||
           !finite3(node.mScale.get()))complete=false;
        out<<",\"boxes\":[";
        bool firstBox=true;
        for(auto const& box:node.mBoxes.get()){
            auto const& from=box.mFrom.get();
            auto const& size=box.mSize.get();
            auto const& rot=box.mRotation.get();
            auto const& pivot=box.mPivot.get();
            if(!finite3(from)||!finite3(size)||!finite3(rot)||!finite3(pivot) ||
               size.x==0.f||size.y==0.f||size.z==0.f){
                complete=false;continue;
            }
            if(!firstBox)out<<',';
            firstBox=false;
            out<<"{\"from\":";vec3(out,from);
            out<<",\"size\":";vec3(out,size);
            out<<",\"rotation\":";vec3(out,rot);
            out<<",\"pivot\":";vec3(out,pivot);
            out<<",\"mirror\":"<<(box.mMirror?"true":"false")
               <<",\"inflate\":"<<box.mInflate
               <<",\"uses_face_uv\":"<<(box.mUsesFaceUVs?"true":"false")
               <<",\"texture_offset\":["
               <<box.mTex.get().x<<','<<box.mTex.get().y<<']'
               <<",\"faces\":[";
            auto const& faces=box.mFaceUVs.get();
            for(std::size_t i=0;i<faces.size();++i){
                if(i)out<<',';
                auto const& f=faces[i];
                auto const& uv=f.mUV.get();
                auto const& uvSize=f.mUVSize.get();
                if(!std::isfinite(uv.x)||!std::isfinite(uv.y)||
                   !std::isfinite(uvSize.x)||!std::isfinite(uvSize.y))
                    complete=false;
                out<<"{\"valid\":"<<(f.mFaceValid?"true":"false")
                   <<",\"uv\":";vec2(out,uv);
                out<<",\"uv_size\":";vec2(out,uvSize);
                out<<",\"material_index\":"<<f.mMaterialInstanceListIndex
                   <<",\"uv_rotation\":"<<static_cast<int>(f.mUvRotation)<<'}';
            }
            out<<"]}";
        }
        out<<"]";
        auto const& poly=node.mMesh.get();
        auto const& pos=poly.mPositions.get();
        auto const& normals=poly.mNormals.get();
        auto const& uvs=poly.mUVs.get();
        auto const& tris=poly.mTris.get();
        auto const& quads=poly.mQuads.get();
        bool valid=!pos.empty()&&pos.size()<=MaxPolyVertices &&
            normals.size()<=MaxPolyVertices && uvs.size()<=MaxPolyVertices &&
            tris.size()+quads.size()<=MaxPolyFaces;
        if(valid){
            for(auto const& v:pos)if(!finite3(v)){valid=false;break;}
            for(auto const& v:normals)if(!finite3(v)){valid=false;break;}
            for(auto const& v:uvs)
                if(!std::isfinite(v.x)||!std::isfinite(v.y)){valid=false;break;}
        }
        if(valid){
            for(auto const& tri:tris)for(auto const& v:tri)
                if(v.mPositionIndex>=pos.size()||(!uvs.empty()&&v.mUVIndex>=uvs.size())||
                   (!normals.empty()&&v.mNormalIndex>=normals.size()))valid=false;
            for(auto const& quad:quads)for(auto const& v:quad)
                if(v.mPositionIndex>=pos.size()||(!uvs.empty()&&v.mUVIndex>=uvs.size())||
                   (!normals.empty()&&v.mNormalIndex>=normals.size()))valid=false;
        }
        if(!valid && (!tris.empty()||!quads.empty()))complete=false;
        if(valid && (!tris.empty()||!quads.empty())){
            polygonFaces+=tris.size()+quads.size();
            out<<",\"poly_mesh\":{\"positions\":[";
            for(std::size_t i=0;i<pos.size();++i){if(i)out<<',';vec3(out,pos[i]);}
            out<<"],\"normals\":[";
            for(std::size_t i=0;i<normals.size();++i){if(i)out<<',';vec3(out,normals[i]);}
            out<<"],\"uvs\":[";
            for(std::size_t i=0;i<uvs.size();++i){if(i)out<<',';vec2(out,uvs[i]);}
            out<<"],\"uv_normalized\":"<<(poly.mAreUVsNormalized?"true":"false");
            auto faces=[&](auto const& list){
                out<<'[';
                for(std::size_t i=0;i<list.size();++i){
                    if(i)out<<',';
                    out<<'[';
                    for(std::size_t j=0;j<list[i].size();++j){
                        if(j)out<<',';
                        auto const& v=list[i][j];
                        out<<'['<<v.mPositionIndex<<','<<v.mNormalIndex<<','<<v.mUVIndex<<']';
                    }
                    out<<']';
                }
                out<<']';
            };
            out<<",\"triangles\":";faces(tris);
            out<<",\"quads\":";faces(quads);
            out<<'}';
            if(uvs.empty())complete=false;
        }
        if(!node.mTextureMeshes.get().empty())
            complete=false; // 2D texture meshes need a different renderer.
        out<<",\"texture_mesh_count\":"<<node.mTextureMeshes.get().size()<<'}';
    }
    out<<"],\"geometry_complete\":"<<(complete?"true":"false")<<"}\n";
    return boxCount||polygonFaces?out.str():std::string{};
}
}
std::vector<ActorModelSnapshot> extractNativeGeometryResources(GeometryGroup& group){
    // The SDK exposes this typed resource map. Copy owned JSON under the
    // engine's native mutex; never store its GeometryInfo/Geometry pointers.
    std::unique_lock lock(group.mGeometryLock.get(),std::try_to_lock);
    if(!lock.owns_lock())return {};
    auto const& registry=group.mGeometries.get();
    std::vector<ActorModelSnapshot> result;
    result.reserve(std::min(registry.size(),MaxResourceModels));
    std::unordered_set<std::string> names;
    for(auto const& [id,info]:registry){
        if(result.size()>=MaxResourceModels)break;
        if(!info)continue;
        auto const* geo=info->mPtr.get();
        if(!geo)continue;
        auto const& name=geo->mSerializableName.get();
        if(name.empty()||!names.insert(name).second)continue;
        auto json=serialize(*geo);
        if(json.empty())continue;
        result.emplace_back(ActorModelSnapshot{name,std::move(json)});
    }
    return result;
}
} // namespace bsc
