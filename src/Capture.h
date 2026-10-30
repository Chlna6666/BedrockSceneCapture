#pragma once
namespace ll::mod {class NativeMod;}
class LocalPlayer;
class MinecraftGame;
namespace bsc {
void initialize(ll::mod::NativeMod&);
bool installCapture();
bool installMenuCapture();
void uninstallMenuCapture();
void uninstallCapture();
void onTick(LocalPlayer&);
void onMenuUpdate(MinecraftGame&);
}