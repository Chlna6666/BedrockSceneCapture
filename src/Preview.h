#pragma once
#include <atomic>
#include <memory>
namespace mce {struct BufferResourceService;}
namespace ll::mod {class NativeMod;}
class BlockPos;
namespace bsc {
struct Mesh;
extern std::atomic_bool gPreviewEnabled;
std::weak_ptr<mce::BufferResourceService> renderBufferService();
// F8 overlay displays only geometries accepted by native scene observation.
void setCapturePreviewEnabled(bool enabled);
void addCapturePreview(BlockPos const&,Mesh const&);
bool installPreview(ll::mod::NativeMod&);
void uninstallPreview();
}