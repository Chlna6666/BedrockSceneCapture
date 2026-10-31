#pragma once
#include <atomic>
#include <memory>
namespace mce {struct BufferResourceService;}
namespace ll::mod {class NativeMod;}
namespace bsc {
extern std::atomic_bool gPreviewEnabled;
std::weak_ptr<mce::BufferResourceService> renderBufferService();
bool installPreview(ll::mod::NativeMod&);
void uninstallPreview();
}