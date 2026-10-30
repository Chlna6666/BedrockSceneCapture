#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace bsc {
struct PaletteEntry {
    std::string name;
    std::string serializedBlockNbt;
    std::uint32_t networkId{};
    std::uint16_t legacyData{};
    std::array<float,4> atlasUv{};
};
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
    int x{},z{},minY{},maxY{};
    std::vector<PaletteEntry> palette;
    // y-major; for every Y slice, Z rows then X cells.
    std::vector<std::uint16_t> cells;
    std::vector<Mesh> meshes;
    std::uint32_t missingMeshes{};
};
struct FinishData {
    std::filesystem::path directory;
    std::uint32_t requested{},queued{},skipped{};
    bool cancelled{};
};
bool saveChunk(std::filesystem::path const&, ChunkData const&, std::string&);
bool saveManifest(FinishData const&, std::string&);
}