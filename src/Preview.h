#pragma once
#include <atomic>
#include <memory>
#include <span>
namespace mce {struct BufferResourceService;}
namespace ll::mod {class NativeMod;}
class BlockPos;
namespace bsc {
struct Mesh;
struct ActorWireSegment;
extern std::atomic_bool gPreviewEnabled;
std::weak_ptr<mce::BufferResourceService> renderBufferService();
// F8 overlay displays only geometries accepted by native scene observation.
void setCapturePreviewEnabled(bool enabled);
void addCapturePreview(BlockPos const&,Mesh const&);
// Persist one validated BlockEntity CPU-wire sample per world position in
// F8's red overlay. F7 remains a non-persistent live amber wireframe.
void addCaptureActorPreview(BlockPos const&,std::span<ActorWireSegment const>);
bool installPreview(ll::mod::NativeMod&);
void uninstallPreview();
}