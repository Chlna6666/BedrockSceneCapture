#include "Capture.h"
#include "Preview.h"
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
        if(!installCapture()) {
            mod.getLogger().error("Could not install tick hook");return false;
        }
        if(!installPreview()){
            uninstallCapture();mod.getLogger().error("Could not install render hook");return false;
        }
        if(!installMenuCapture())
            mod.getLogger().warn("Menu hook unavailable: in-world F8 remains usable");
        mod.getLogger().info("BSC enabled: F7 wireframe, F8 registry/model export, F9 cancel");
        mod.getLogger().info("BSC export directory: {}",(mod.getDataDir()/"export"/"1.26.51.1").string());
        return true;
    }
    bool disable(){
        mod.getLogger().info("BSC disabling: unhooking render and tick");
        uninstallMenuCapture();uninstallPreview();uninstallCapture();return true;
    }
    bool unload(){return true;}
};
}
LL_REGISTER_MOD(bsc::BedrockSceneCapture,bsc::BedrockSceneCapture::getInstance());
