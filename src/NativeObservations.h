#pragma once
#include "Format.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace bsc {
struct NativeObservation {
    // The 27 network IDs refer to the same process-local block registry
    // exported by blocks.json, not IDs portable across game versions.
    // Order: center, then dy/dz/dx ascending (-1..1), excluding center.
    std::array<std::uint32_t,27> stateIds{};
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
