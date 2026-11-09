#pragma once
#include "Format.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bsc {
struct SharedTriangle {
    std::array<std::uint32_t,3> originalVertices{};
    std::uint32_t originalTriangle{};
};
struct SharedMeshEncoding {
    // v3 payload: vertex_count u32, triangle_count u32,
    // [xyz float32] * vertex_count, [indices u32x3] * triangle_count.
    std::string geometryBytes;
    std::vector<SharedTriangle> canonicalTriangles;
};
SharedMeshEncoding encodeSharedGeometry(Mesh const& mesh);
// v3 appearance: geometry_id, render_layer, source_kind, flags, triangle_count
// (5*u32); 3 corners/triangle each [normal xyz, uv] f32x5 + ABGR u32;
// then per-triangle material_slot, variant, frame u32 (UINT32_MAX = missing).
std::string encodeSharedAppearance(
    Mesh const& mesh, SharedMeshEncoding const& geometry,
    std::uint32_t geometryId,
    std::string const& triangleSlotsJson,
    std::string const& triangleVariantsJson);
} // namespace bsc
