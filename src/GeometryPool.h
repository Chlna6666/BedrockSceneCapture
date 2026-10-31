#pragma once
#include "Format.h"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bsc {
// A lossless, winding-preserving geometry representation independent of
// texture/color/material. No rotations or approximate float quantization:
// those are not safe equivalences without an explicit instance transform.
struct SharedTriangle {
    std::array<std::uint32_t,3> originalVertices{};
    std::uint32_t originalTriangle{};
};
struct SharedMeshEncoding {
    std::string geometryJson;
    std::vector<SharedTriangle> canonicalTriangles;
};
// Throws on corrupt indices, non-finite coordinates or unsupported primitives.
SharedMeshEncoding encodeSharedGeometry(Mesh const& mesh);
// The two binding arrays use the triangle order of the legacy mesh. They are
// remapped to canonical geometry order and kept separate from geometry bytes.
std::string encodeSharedAppearance(
    Mesh const& mesh, SharedMeshEncoding const& geometry,
    std::string_view geometryHash,
    std::string const& triangleSlotsJson,
    std::string const& triangleVariantsJson
);
} // namespace bsc
