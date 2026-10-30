#pragma once
#include "Format.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bsc {
struct ModelSample {
    std::string neighborhood;
    std::vector<Mesh> meshes;
    std::uint32_t missing{};
};
struct BlockResource {
    std::string name;
    std::string stateNbtHex;
    std::uint32_t networkId{};
    std::uint16_t legacyData{};
    std::array<float, 4> atlasUv{};
    bool dynamic{};
    bool blockEntity{};
    bool air{};
    std::vector<ModelSample> samples;
};
struct LibraryData {
    std::filesystem::path directory;
    std::vector<BlockResource> blocks;
    std::uint32_t registered{}, visitedChunks{}, skippedChunks{};
    std::uint32_t sampledBlocks{}, failedSamples{};
    bool cancelled{};
};
bool saveLibrary(LibraryData const&, std::string& error);
std::string hexBytes(std::string const&);
bool requiresNeighbors(std::string const&);
bool usesBlockEntityRenderer(std::string const&);
} // namespace bsc
