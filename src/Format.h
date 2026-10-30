#pragma once
#include <cstdint>
#include <vector>
namespace bsc {
struct Vertex {
    float x{},y{},z{},nx{},ny{},nz{},u{},v{};
    std::uint32_t abgr{0xffffffffu};
};
struct Mesh {
    std::uint32_t cell{}, layer{}, primitive{};
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};
struct ChunkData {
    std::vector<Mesh> meshes;
    std::uint32_t missingMeshes{};
};
}
