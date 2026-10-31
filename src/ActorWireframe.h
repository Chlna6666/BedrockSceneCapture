#pragma once
#include <array>
#include <cstdint>
#include <vector>
class BaseActorRenderContext;
class BlockPos;
class Vec3;
struct BlockActorRenderData;
struct Vec3;
namespace ll::mod {class NativeMod;}
namespace bsc {
struct ActorWireSegment {
    std::array<float,3> from{},to{};
};
// An actor's original renderer sets the exact world anchor and animated pose
// context. No world/chunk scanning, no collision-box substitution.
class ActorWireframeScope {
public:
    ActorWireframeScope(BaseActorRenderContext&,BlockActorRenderData const&);
    ActorWireframeScope(BaseActorRenderContext&,BlockPos const&,Vec3 const&);
    ActorWireframeScope(BlockPos const&,Vec3 const&,Vec3 const&);
    ~ActorWireframeScope();
    ActorWireframeScope(ActorWireframeScope const&)=delete;
    ActorWireframeScope& operator=(ActorWireframeScope const&)=delete;
private:
    bool active_{};
};
struct ActorWireframeStats {
    std::uint64_t partsCalled{}, partsWithCpuGeometry{}, partsWithValidTransform{};
    std::uint64_t missingCpuGeometry{}, discardedTransforms{}, emittedSegments{};
    std::uint64_t actorDataConstructed{},actorDataDestroyed{},actorBuffersFlushed{};
};
bool installActorWireframePartHook(ll::mod::NativeMod&);
void updateActorWireframeCamera(Vec3 const&);
// The dispatcher includes renderers beyond the seven built-in F8 model hooks.
void uninstallActorWireframePartHook();
void beginActorWireframeFrame();
std::vector<ActorWireSegment> takeActorWireframeSegments();
ActorWireframeStats actorWireframeStats();
}
