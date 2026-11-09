#include "GeometryPool.h"
#include "PoolV3.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <string_view>
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
std::uint32_t bindingNumber(std::string_view token){
    std::uint32_t value=0;
    auto const result=std::from_chars(token.data(),token.data()+token.size(),value);
    if(result.ec!=std::errc{}||result.ptr!=token.data()+token.size())
        throw std::runtime_error("Geometry Pool v3: invalid material/variant index");
    return value;
}
std::uint32_t optionalIndex(std::string_view token){
    if(token=="null")return UINT32_MAX;
    return bindingNumber(token);
}
std::array<std::uint32_t,2> variantIndex(std::string_view token){
    if(token=="null")return {UINT32_MAX,UINT32_MAX};
    if(token.size()<5||token.front()!='['||token.back()!=']')
        throw std::runtime_error("Geometry Pool v3: invalid variant");
    token.remove_prefix(1);
    token.remove_suffix(1);
    auto const comma=token.find(',');
    if(comma==std::string_view::npos)
        throw std::runtime_error("Geometry Pool v3: invalid variant pair");
    return {optionalIndex(token.substr(0,comma)),
            optionalIndex(token.substr(comma+1))};
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
    auto& out=result.geometryBytes;
    if(positionIndex.size()>UINT32_MAX||triangles.size()>UINT32_MAX)
        throw std::runtime_error("Geometry Pool v3: too many vertices or triangles");
    poolU32(out,static_cast<std::uint32_t>(positionIndex.size()));
    poolU32(out,static_cast<std::uint32_t>(triangles.size()));
    for(auto const& [point,index]:positionIndex){
        for(auto v:point)poolF32(out,v);
    }
    for(auto const& tri:triangles){
        for(auto const& p:tri.positions)poolU32(out,positionIndex.at(p));
        result.canonicalTriangles.emplace_back(tri.origin);
    }
    return result;
}

std::string encodeSharedAppearance(
    Mesh const& mesh, SharedMeshEncoding const& geometry,
    std::uint32_t geometryId,
    std::string const& slotsJson,
    std::string const& variantsJson){
    auto const slots=splitBindingArray(slotsJson);
    auto const variants=splitBindingArray(variantsJson);
    if(slots.size()!=geometry.canonicalTriangles.size()||
       variants.size()!=geometry.canonicalTriangles.size())
        throw std::runtime_error("Geometry Pool v3: triangle binding count mismatch");
    if(geometry.canonicalTriangles.size()>UINT32_MAX)
        throw std::runtime_error("Geometry Pool v3: triangle overflow");
    std::string out;
    poolU32(out,geometryId);
    poolU32(out,mesh.layer);
    poolU32(out,mesh.registryCrossPlanes?1u:(mesh.registryVisualBounds?0u:2u));
    poolU32(out,(mesh.uvValid?1u:0u)|(mesh.normalsReconstructed?2u:0u));
    poolU32(out,static_cast<std::uint32_t>(geometry.canonicalTriangles.size()));
    for(auto const& triangle:geometry.canonicalTriangles)for(auto index:triangle.originalVertices){
        auto const& v=mesh.vertices[index];
        for(float n:{v.nx,v.ny,v.nz,v.u,v.v}){
            if(!std::isfinite(n))throw std::runtime_error("Geometry Pool v3: invalid appearance float");
            poolF32(out,n);
        }
        poolU32(out,v.abgr);
    }
    for(auto const& triangle:geometry.canonicalTriangles){
        auto const n=triangle.originalTriangle;
        poolU32(out,optionalIndex(slots[n]));
        auto const pair=variantIndex(variants[n]);
        poolU32(out,pair[0]);
        poolU32(out,pair[1]);
    }
    return out;
}
} // namespace bsc
