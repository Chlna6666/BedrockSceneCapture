#include "Capture.h"
#include "Preview.h"
#include "NativeObservations.h"
#include <filesystem>
#include <system_error>
#include "ll/api/mod/NativeMod.h"
#include "ll/api/mod/RegisterHelper.h"
namespace bsc {
class BedrockSceneCapture {
    ll::mod::NativeMod& mod;
public:
    static BedrockSceneCapture& getInstance(){
        static BedrockSceneCapture v;return v;
    }
    BedrockSceneCapture():mod(*ll::mod::NativeMod::current()){}
    bool load(){
        initialize(mod);
        mod.getLogger().info("BedrockSceneCapture 0.2.0: 1.26.51.1 client build, mod directory: {}",mod.getModDir().string());
        return true;
    }
    bool enable(){
        // Do not resolve or patch every native renderer symbol while
        // LeviLamina is still synchronously enabling mods. Hook resolution
        // can block the game startup on some client builds.
        mod.getLogger().info("BSC enable [1/4]: installing player tick hook");
        if(!installCapture()) {
            mod.getLogger().error("BSC enable failed at player tick hook");return false;
        }
        mod.getLogger().info("BSC enable [2/4]: installing overlay render hook");
        if(!installPreview(mod)){
            uninstallCapture();mod.getLogger().error("BSC enable failed at overlay render hook");return false;
        }
        mod.getLogger().info("BSC enable [3/4]: installing menu update hook");
        if(!installMenuCapture())
            mod.getLogger().warn("BSC menu hook unavailable: in-world F8 remains usable");
        // Heavy native scene and BlockActor hooks are installed only when F8
        // is requested in-world, not under the plugin enable path.
        mod.getLogger().info("BSC enable [4/4]: initializing control directory; native scene hooks deferred until F8");
        auto const controls=mod.getDataDir()/"control";
        std::error_code controlError;
        std::filesystem::create_directories(controls,controlError);
        if(controlError)
            mod.getLogger().warn("BSC control directory unavailable: {}: {}",controls.string(),controlError.message());
        mod.getLogger().info("BSC enabled: F7 wireframe, F8 registry/model export (native hooks installed on F8), F9 cancel");
        mod.getLogger().info("BSC remote control directory: {}",controls.string());
        mod.getLogger().info("BSC export directory: {}",(mod.getDataDir()/"export"/"1.26.51.1").string());
        return true;
    }
    bool disable(){
        mod.getLogger().info("BSC disabling: unhooking render and tick");
        uninstallMenuCapture();uninstallNativeObservations();uninstallPreview();uninstallCapture();return true;
    }
    bool unload(){return true;}
};
}
LL_REGISTER_MOD(bsc::BedrockSceneCapture,bsc::BedrockSceneCapture::getInstance());
