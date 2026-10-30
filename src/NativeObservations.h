#pragma once
#include "Format.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace bsc {
struct NativeObservation {
    // The seven network IDs refer to the same process-local block registry
    // exported by blocks.json, not IDs portable across game versions.
    // Order: center, up, down, north, south, west, east.
    std::array<std::uint32_t,7> stateIds{};
    Mesh mesh;
};
struct ActorModelSnapshot {
    std::string name;
    std::string geometryJson;
};
bool installNativeObservations();
void uninstallNativeObservations();
void armNativeObservations(bool enabled);
std::vector<NativeObservation> takeNativeObservations();
std::vector<ActorModelSnapshot> takeActorModels();
void clearNativeObservations();
} // namespace bsc
