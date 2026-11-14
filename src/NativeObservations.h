#pragma once
#include "Format.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace ll::mod {class NativeMod;}
class MinecraftGame;
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
struct NativeShapeSample {
    int shape{};
    std::size_t acceptedMeshes{},missingUvMeshes{};
};
struct NativeSceneDiagnostics {
    std::uint64_t tessellatorCalls{};
    std::uint64_t eligibleCalls{};
    std::uint64_t acceptedCalls{};
    std::uint64_t rejectedQuota{},rejectedDuplicate{},rejectedLockBusy{};
    std::size_t distinctBlockStates{};
    std::size_t distinctShapes{};
    std::vector<NativeShapeSample> shapeSamples;
};
NativeSceneDiagnostics nativeSceneDiagnostics();
// Recreate selected built-in CPU model definitions after source-box hooks are active.
// Used only on explicit F6, never during mod startup.
void captureBuiltInModelTemplates(ll::mod::NativeMod& mod);
struct ActorMissingModelProbe {
    std::string name;
    bool failedAtStartup{},failedAtRuntime{};
    std::size_t parts{},nativeCubes{},sourceBoxes{},polygonFaces{};
};
struct ActorCaptureDiagnostics {
    // chest, standing sign, hanging sign, bed, shulker box, bell, piston arm.
    std::array<bool,7> hooksInstalled{};
    bool dispatcherHook{};
    bool explicitDispatcherHook{},implicitDispatcherHook{};
    // Native BlockActorRendererId values 0..27 observed during F6, not a
    // claim that the corresponding UV mesh was successfully exported.
    std::uint32_t observedRendererMask{};
    std::size_t capturedModels{};
    // An exported but incomplete UV model is not a missing model.
    std::size_t completeModels{},incompleteModels{};
    bool sourceAddBoxHook{};
    bool sourceLoadBoxesHook{};
    bool sourceCompileCubesHook{};
    std::size_t retainedSourceParts{};
    std::size_t retainedSourceBoxes{};
    std::vector<std::string> observedWithoutGeometry;
    std::vector<std::string> observedIncompleteGeometry;
    std::vector<ActorMissingModelProbe> missingModelProbes;
};
ActorCaptureDiagnostics actorCaptureDiagnostics();
// Install after LeviLamina enables mods, never inside DllMain. Snapshots are
// created at Bedrock renderer initialization or by safe ready-client fallback.
bool installStartupModelCapture(ll::mod::NativeMod& mod);
void tryStartupModelSnapshot(MinecraftGame& game);
std::uint64_t nativeResourceGeneration();
std::size_t startupModelCount();
bool installNativeObservations(ll::mod::NativeMod& mod);
// F7 installs only actor renderers + live model-part hook, not F6 scene capture.
bool installActorWireframeObservations(ll::mod::NativeMod& mod);
void uninstallNativeObservations();
void armNativeObservations(bool enabled);
std::vector<NativeObservation> takeNativeObservations();
std::vector<ActorModelSnapshot> takeActorModels();
void clearNativeObservations();
} // namespace bsc
