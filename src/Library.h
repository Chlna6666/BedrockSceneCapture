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
    std::vector<std::shared_ptr<Mesh const>> meshes;
    std::uint32_t missing{};
    bool nativeObserved{};
    std::array<std::uint32_t,27> neighborStateIds{};
    std::array<int,3> observedBlockPosition{};
    bool positionVariantCandidate{};
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
    // A full BlockState is preserved, but audio-only properties may reference
    // an earlier canonical visual recipe instead of duplicating its mesh.
    std::uint32_t geometryAliasNetworkId{};
    bool hasGeometryAlias{};
    std::uint16_t legacyData{};
    std::array<float, 4> atlasUv{};
    bool dynamic{};
    int renderLayer{-1};
    bool blockEntity{};
    int blockActorType{};
    // Expected native renderer model families. A family is only treated as
    // captured when the corresponding model exists in entities.json.
    std::vector<std::string> actorModelKeys;
    bool air{};
    int nativeShape{-1};
    std::string geometryContext{"none"};
    bool visualBoundsAvailable{};
    std::array<float,6> visualBounds{};
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
    std::vector<StaticModel> actorModels;
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
