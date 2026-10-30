#pragma once
#include "Format.h"
#include <optional>
#include <cstdint>
class AABB;
enum class BlockShape : int;
namespace bsc {
// CPU only: reconstruct a renderable, face-indexed box using the native
// BlockGraphics visual bounds. Never creates a GPU resource or reads a chunk.
// Non-box shapes must use native geometry definitions/context-specific rules.
std::optional<Mesh> makeRegistryCrossPlanes(
    AABB const& bounds, BlockShape shape, std::uint32_t layer);
std::optional<Mesh> makeRegistryVisualBox(
    AABB const& bounds, BlockShape shape, std::uint32_t layer);
}
