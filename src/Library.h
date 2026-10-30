#pragma once
#include "Format.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bsc {
struct ModelSample {
    std::string neighborhood;
    std::vector<Mesh> meshes;
    std::uint32_t missing{};
};
struct TextureUVVariant {
    std::uint32_t variant{},frame{};
    std::string sourcePath;
    std::array<float,4> atlasUv{};
    std::uint16_t tileWidth{},tileHeight{},sourceWidth{},sourceHeight{};
    std::uint16_t pbrHandle{};
};
struct TextureBinding {
    std::uint32_t slot{};
    std::string defaultName;
    std::string carriedName;
    std::string atlasItem;
    std::string sourcePath;
    std::array<float,4> atlasUv{};
    std::uint16_t tileWidth{},tileHeight{},sourceWidth{},sourceHeight{};
    std::uint16_t pbrHandle{};
    bool additive{};
    std::vector<TextureUVVariant> variants;
    std::string carriedAtlasItem;
    bool carriedAdditive{};
    std::vector<TextureUVVariant> carriedVariants;
};
struct StaticModel {
    std::string name;
    std::string geometryJson;
};
struct BlockResource {
    std::string name;
    std::string stateNbtHex;
    std::uint32_t networkId{};
    std::uint16_t legacyData{};
    std::array<float, 4> atlasUv{};
    bool dynamic{};
    int renderLayer{-1};
    bool blockEntity{};
    int blockActorType{};
    bool air{};
    std::vector<ModelSample> samples;
    std::shared_ptr<std::vector<TextureBinding> const> textures;
    std::vector<TextureBinding> const& textureBindings() const {
        static std::vector<TextureBinding> const empty;
        return textures ? *textures : empty;
    }
};
struct LibraryData {
    std::filesystem::path directory;
    std::vector<BlockResource> blocks;
    std::vector<StaticModel> staticModels;
    std::uint32_t registered{}, visitedChunks{}, skippedChunks{};
    std::uint32_t sampledBlocks{}, failedSamples{};
    bool cancelled{};
    bool definitionOnly{};
};
bool saveLibrary(LibraryData const&, std::string& error);
std::string hexBytes(std::string const&);
bool requiresNeighbors(std::string const&);
bool usesBlockEntityRenderer(std::string const&);
} // namespace bsc
