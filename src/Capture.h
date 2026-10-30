#pragma once
namespace ll::mod {class NativeMod;}
class LocalPlayer;
namespace bsc {
void initialize(ll::mod::NativeMod&);
bool installCapture();
void uninstallCapture();
void onTick(LocalPlayer&);
}