#pragma once
#include <cstdint>
#include <vector>
namespace bsc {
struct Vertex {
    float x{},y{},z{},nx{},ny{},nz{},u{},v{};
    std::uint32_t abgr{0xffffffffu};
    // Runtime MeshData::mPBRTextureIndices, if present for every vertex.
    // Retained only for deterministic UV->material resolution at export.
    std::uint16_t pbrHandle{0xffffu};
};
struct Mesh {
    std::uint32_t cell{}, layer{}, primitive{};
    bool uvValid{true};
    bool registryVisualBounds{};
    bool registryCrossPlanes{};
    bool normalsReconstructed{}; // CPU-generated proxy from registered native BlockGraphics bounds
    std::vector<std::uint32_t> faceSlots; // two triangles per Up/Down/North/South/West/East
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};
struct ChunkData {
    std::vector<Mesh> meshes;
    std::uint32_t missingMeshes{};
};
}
