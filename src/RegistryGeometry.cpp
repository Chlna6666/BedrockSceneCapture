#include "RegistryGeometry.h"
#include "mc/world/level/block/BlockShape.h"
#include "mc/world/phys/AABB.h"
#include <array>
#include <cmath>

namespace bsc {
std::optional<Mesh> makeRegistryVisualBox(
    AABB const& bounds, BlockShape shape, std::uint32_t layer) {
    if(shape!=BlockShape::Block && shape!=BlockShape::SlimeBlock)
        return std::nullopt;
    auto const& lo=bounds.min;
    auto const& hi=bounds.max;
    // Reject uninitialized and non-finite native bounds. Do not invent a
    // one-block cube if the engine did not provide usable geometry.
    for(float v : {lo.x,lo.y,lo.z,hi.x,hi.y,hi.z})
        if(!std::isfinite(v) || v < -16.f || v > 16.f) return std::nullopt;
    if(hi.x<=lo.x || hi.y<=lo.y || hi.z<=lo.z)
        return std::nullopt;
    using Point=std::array<float,3>;
    using Quad=std::array<Point,4>;
    std::array<Quad,6> const quads{{
        {{{lo.x,hi.y,lo.z},{lo.x,hi.y,hi.z},{hi.x,hi.y,hi.z},{hi.x,hi.y,lo.z}}}, // up +Y
        {{{lo.x,lo.y,lo.z},{hi.x,lo.y,lo.z},{hi.x,lo.y,hi.z},{lo.x,lo.y,hi.z}}}, // down -Y
        {{{lo.x,lo.y,lo.z},{lo.x,hi.y,lo.z},{hi.x,hi.y,lo.z},{hi.x,lo.y,lo.z}}}, // north -Z
        {{{lo.x,lo.y,hi.z},{hi.x,lo.y,hi.z},{hi.x,hi.y,hi.z},{lo.x,hi.y,hi.z}}}, // south +Z
        {{{lo.x,lo.y,lo.z},{lo.x,lo.y,hi.z},{lo.x,hi.y,hi.z},{lo.x,hi.y,lo.z}}}, // west -X
        {{{hi.x,lo.y,lo.z},{hi.x,hi.y,lo.z},{hi.x,hi.y,hi.z},{hi.x,lo.y,hi.z}}}  // east +X
    }};
    std::array<Point,6> const normals{{{0,1,0},{0,-1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}}};
    std::array<std::array<float,2>,4> const uv{{{0,0},{0,1},{1,1},{1,0}}};
    Mesh mesh;
    mesh.layer=layer;
    mesh.primitive=2; // mce::PrimitiveMode::TriangleList
    mesh.registryVisualBounds=true;
    mesh.vertices.reserve(24);
    mesh.indices.reserve(36);
    mesh.faceSlots.reserve(12);
    for(std::uint32_t face=0;face<6;++face){
        for(std::uint32_t n=0;n<4;++n){
            auto const& p=quads[face][n];
            auto const& normal=normals[face];
            mesh.vertices.push_back(Vertex{
                p[0],p[1],p[2],
                normal[0],normal[1],normal[2],
                uv[n][0],uv[n][1],0xffffffffu
            });
        }
        std::uint32_t const start=face*4;
        for(std::uint32_t k : {0u,1u,2u,0u,2u,3u})
            mesh.indices.emplace_back(start+k);
        mesh.faceSlots.emplace_back(face);
        mesh.faceSlots.emplace_back(face);
    }
    return mesh;
}
} // namespace bsc
