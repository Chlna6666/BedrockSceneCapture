#include "ModelUv.h"
#include <cassert>
#include <limits>
#include <string_view>
int main(){
    auto net=bsc::expandLegacyBoxUV({0,16},{16,8,16});
    assert(net && net->size()==6);
    auto const& faces=*net;
    assert(std::string_view(faces[0].name)=="up" && faces[0].uv[0]==16 && faces[0].uv[1]==16);
    assert(std::string_view(faces[1].name)=="down" && faces[1].uv[0]==32);
    assert(std::string_view(faces[2].name)=="north" && faces[2].uv[1]==32);
    assert(std::string_view(faces[3].name)=="south" && faces[3].uv[0]==48);
    assert(std::string_view(faces[4].name)=="west" && faces[4].uv[0]==0);
    assert(std::string_view(faces[5].name)=="east" && faces[5].uv[0]==32);
    for(auto const& face:faces)assert(face.uvSize[0]>0 && face.uvSize[1]>0);
    assert(!bsc::expandLegacyBoxUV({0,0},{0,8,8}));
    assert(!bsc::expandLegacyBoxUV({0,0},{16,-1,8}));
    assert(!bsc::expandLegacyBoxUV(
        {std::numeric_limits<float>::quiet_NaN(),0},{16,8,8}));
}
