#include "GeometryPool.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace bsc {
namespace {
using Point=std::array<float,3>;
struct Triangle {
    std::array<Point,3> positions{};
    SharedTriangle origin;
};
std::vector<std::uint32_t> triangleIndices(Mesh const& mesh){
    std::vector<std::uint32_t> src=mesh.indices;
    if(src.empty()){
        src.resize(mesh.vertices.size());
        std::iota(src.begin(),src.end(),0u);
    }
    std::vector<std::uint32_t> dst;
    dst.reserve(src.size()*2);
    auto add=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c){
        if(a>=mesh.vertices.size()||b>=mesh.vertices.size()||c>=mesh.vertices.size())
            throw std::runtime_error("shared geometry: invalid native mesh index");
        dst.insert(dst.end(),{a,b,c});
    };
    switch(mesh.primitive){
    case 1:
        if(src.size()%4)throw std::runtime_error("shared geometry: incomplete quad");
        for(std::size_t i=0;i<src.size();i+=4){
            add(src[i],src[i+1],src[i+2]);
            add(src[i],src[i+2],src[i+3]);
        }
        break;
    case 2:
        if(src.size()%3)throw std::runtime_error("shared geometry: incomplete triangle");
        for(std::size_t i=0;i<src.size();i+=3)add(src[i],src[i+1],src[i+2]);
        break;
    case 3:
        for(std::size_t i=2;i<src.size();++i)
            if(i&1)add(src[i-1],src[i-2],src[i]);
            else add(src[i-2],src[i-1],src[i]);
        break;
    default:throw std::runtime_error("shared geometry: unsupported topology");
    }
    return dst;
}
float checked(float f){
    if(!std::isfinite(f)||std::abs(f)>4096.f)
        throw std::runtime_error("shared geometry: invalid native vertex position");
    // Canonicalize the sign of zero, but retain all other IEEE-754 float32
    // values. Do not merge triangles by rounding their positions.
    return f==0.f?0.f:f;
}
void writePoint(std::ostringstream& out,Point const& p){
    out<<'['<<p[0]<<','<<p[1]<<','<<p[2]<<']';
}
// A strict parser for the only binding values emitted by faceBindingJson:
// arrays of integers/null or pairs of integer/null. Nested brackets are
// supported; strings are forbidden in these arrays.
std::vector<std::string> splitBindingArray(std::string const& json){
    if(json.size()<2||json.front()!='['||json.back()!=']')
        throw std::runtime_error("shared appearance: invalid binding array");
    std::vector<std::string> out;
    std::size_t start=1;
    int depth=0;
    for(std::size_t i=1;i+1<json.size();++i){
        char c=json[i];
        if(c=='[')++depth;
        if(c==']' && --depth<0)
            throw std::runtime_error("shared appearance: unbalanced array");
        if(c==',' && depth==0){
            if(i==start)throw std::runtime_error("shared appearance: empty binding");
            out.emplace_back(json.substr(start,i-start));
            start=i+1;
        }
    }
    if(depth!=0)throw std::runtime_error("shared appearance: unbalanced bindings");
    if(start<json.size()-1)out.emplace_back(json.substr(start,json.size()-start-1));
    return out;
}
} // namespace

SharedMeshEncoding encodeSharedGeometry(Mesh const& mesh){
    auto const indices=triangleIndices(mesh);
    std::vector<Triangle> triangles;
    triangles.reserve(indices.size()/3);
    for(std::size_t i=0;i<indices.size();i+=3){
        Triangle tri;
        tri.origin.originalTriangle=static_cast<std::uint32_t>(i/3);
        for(std::size_t j=0;j<3;++j){
            auto const index=indices[i+j];
            auto const& v=mesh.vertices[index];
            tri.positions[j]={checked(v.x),checked(v.y),checked(v.z)};
            tri.origin.originalVertices[j]=index;
        }
        // Rotate the triangle cyclically, never reverse its winding.
        std::size_t first=0;
        if(tri.positions[1]<tri.positions[first])first=1;
        if(tri.positions[2]<tri.positions[first])first=2;
        std::rotate(tri.positions.begin(),tri.positions.begin()+first,tri.positions.end());
        std::rotate(tri.origin.originalVertices.begin(),
                    tri.origin.originalVertices.begin()+first,
                    tri.origin.originalVertices.end());
        triangles.emplace_back(std::move(tri));
    }
    std::stable_sort(triangles.begin(),triangles.end(),
        [](Triangle const& a,Triangle const& b){return a.positions<b.positions;});
    std::map<Point,std::uint32_t> positionIndex;
    for(auto const& tri:triangles)
        for(auto const& point:tri.positions)
            positionIndex.emplace(point,0);
    std::uint32_t next=0;
    for(auto& [point,index]:positionIndex)index=next++;

    SharedMeshEncoding result;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out<<std::setprecision(std::numeric_limits<float>::max_digits10);
    out<<"{\"schema\":2,\"topology\":\"triangles\","
       "\"coordinate_space\":\"block_local\","
       "\"precision\":\"exact_float32\","
       "\"winding_preserved\":true,\"vertices\":[";
    bool first=true;
    for(auto const& [point,index]:positionIndex){
        if(!first)out<<',';
        first=false;
        writePoint(out,point);
    }
    out<<"],\"triangles\":[";
    first=true;
    for(auto const& tri:triangles){
        if(!first)out<<',';
        first=false;
        out<<'['<<positionIndex.at(tri.positions[0])<<','
           <<positionIndex.at(tri.positions[1])<<','
           <<positionIndex.at(tri.positions[2])<<']';
        result.canonicalTriangles.emplace_back(tri.origin);
    }
    out<<"]}\n";
    result.geometryJson=out.str();
    return result;
}

std::string encodeSharedAppearance(
    Mesh const& mesh, SharedMeshEncoding const& geometry,
    std::string_view geometryHash,
    std::string const& slotsJson,
    std::string const& variantsJson){
    auto const slots=splitBindingArray(slotsJson);
    auto const variants=splitBindingArray(variantsJson);
    if(slots.size()!=geometry.canonicalTriangles.size()||
       variants.size()!=geometry.canonicalTriangles.size())
        throw std::runtime_error("shared appearance: triangle binding count mismatch");
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out<<std::setprecision(std::numeric_limits<float>::max_digits10);
    out<<"{\"schema\":2,\"geometry_ref\":\""<<geometryHash
       <<"\",\"render_layer\":"<<mesh.layer
       <<",\"uv_complete\":"<<(mesh.uvValid?"true":"false")
       <<",\"geometry_source\":\""
       <<(mesh.registryCrossPlanes?"registry_cross_planes":
          (mesh.registryVisualBounds?"registry_visual_bounds":"native_world_tessellation"))
       <<"\",\"geometry_confidence\":\""
       <<(mesh.registryVisualBounds||mesh.registryCrossPlanes?"visual_shape_proxy":"native_observed")
       <<"\",\"uv_space\":\""
       <<(mesh.registryVisualBounds||mesh.registryCrossPlanes?"block_face_local":"runtime_terrain_atlas")
       <<"\",\"normals_reconstructed\":"<<(mesh.normalsReconstructed?"true":"false")
       <<",\"corner_attributes\":[";
    bool first=true;
    for(auto const& triangle:geometry.canonicalTriangles){
        for(auto index:triangle.originalVertices){
            if(!first)out<<',';
            first=false;
            auto const& v=mesh.vertices[index];
            out<<'['<<v.nx<<','<<v.ny<<','<<v.nz<<','
               <<v.u<<','<<v.v<<','<<v.abgr<<']';
        }
    }
    out<<"],\"triangle_texture_slots\":[";
    first=true;
    for(auto const& triangle:geometry.canonicalTriangles){
        if(!first)out<<',';
        first=false;
        out<<slots[triangle.originalTriangle];
    }
    out<<"],\"triangle_texture_variant_indices\":[";
    first=true;
    for(auto const& triangle:geometry.canonicalTriangles){
        if(!first)out<<',';
        first=false;
        out<<variants[triangle.originalTriangle];
    }
    out<<"]}\n";
    return out.str();
}
} // namespace bsc
