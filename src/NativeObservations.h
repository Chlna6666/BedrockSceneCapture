#pragma once
#include "Format.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace ll::mod {class NativeMod;}
namespace bsc {
struct NativeObservation {
    // The 27 network IDs refer to the same process-local block registry
    // exported by blocks.json, not IDs portable across game versions.
    // Order: center, then dy/dz/dx ascending (-1..1), excluding center.
    std::array<std::uint32_t,27> stateIds{};
    Mesh mesh;
    std::array<int,3> observedBlockPosition{};
    bool positionVariantCandidate{};
};
struct ActorModelSnapshot {
    std::string name;
    std::string geometryJson;
};
struct NativeSceneDiagnostics {
    std::uint64_t tessellatorCalls{};
    std::uint64_t eligibleCalls{};
    std::uint64_t acceptedCalls{};
    std::size_t distinctBlockStates{};
    std::size_t distinctShapes{};
};
NativeSceneDiagnostics nativeSceneDiagnostics();
// Recreate selected built-in CPU model definitions after source-box hooks are active.
// Used only on explicit F8, never during mod startup.
void captureBuiltInModelTemplates(ll::mod::NativeMod& mod);
struct ActorCaptureDiagnostics {
    // chest, standing sign, hanging sign, bed, shulker box, bell, piston arm.
    std::array<bool,7> hooksInstalled{};
    std::size_t capturedModels{};
    bool sourceAddBoxHook{};
    bool sourceLoadBoxesHook{};
    bool sourceCompileCubesHook{};
    std::size_t retainedSourceParts{};
    std::size_t retainedSourceBoxes{};
    std::vector<std::string> observedWithoutGeometry;
};
ActorCaptureDiagnostics actorCaptureDiagnostics();
bool installNativeObservations(ll::mod::NativeMod& mod);
// F7 installs only actor renderers + live model-part hook, not F8 scene capture.
bool installActorWireframeObservations(ll::mod::NativeMod& mod);
void uninstallNativeObservations();
void armNativeObservations(bool enabled);
std::vector<NativeObservation> takeNativeObservations();
std::vector<ActorModelSnapshot> takeActorModels();
void clearNativeObservations();
} // namespace bsc
