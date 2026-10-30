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
    bool load(){initialize(mod);return true;}
    bool enable(){
        if(!installCapture()) {
            mod.getLogger().error("Could not install tick hook");return false;
        }
        if(!installPreview()){
            uninstallCapture();mod.getLogger().error("Could not install render hook");return false;
        }
        mod.getLogger().info("BSC ready: F7 wireframe, F8 capture loaded chunks, F9 cancel");
        return true;
    }
    bool disable(){uninstallPreview();uninstallCapture();return true;}
    bool unload(){return true;}
};
}
LL_REGISTER_MOD(bsc::BedrockSceneCapture,bsc::BedrockSceneCapture::getInstance());
