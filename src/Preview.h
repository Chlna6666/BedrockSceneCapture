#pragma once
#include <atomic>
namespace ll::mod {class NativeMod;}
namespace bsc {
extern std::atomic_bool gPreviewEnabled;
bool installPreview();
void uninstallPreview();
}