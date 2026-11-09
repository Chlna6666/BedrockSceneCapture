#pragma once
#include <array>
#include <cmath>
#include <optional>
namespace bsc {
struct LegacyBoxFaceUV {
    char const* name;
    std::array<float,2> uv;
    std::array<float,2> uvSize;
};
// Box UV coordinates are source texels, not runtime terrain-atlas UVs.
inline std::optional<std::array<LegacyBoxFaceUV,6>> expandLegacyBoxUV(
    std::array<float,2> offset,std::array<float,3> size){
    for(float v:offset)if(!std::isfinite(v))return std::nullopt;
    for(float v:size)if(!std::isfinite(v)||v<=0.f)return std::nullopt;
    auto const [u,v]=offset;
    auto const [w,h,d]=size;
    return std::array<LegacyBoxFaceUV,6>{{
        {"up",{u+d,v},{w,d}},{"down",{u+d+w,v},{w,d}},
        {"north",{u+d,v+d},{w,h}},{"south",{u+2*d+w,v+d},{w,h}},
        {"west",{u,v+d},{d,h}},{"east",{u+d+w,v+d},{d,h}}
    }};
}
}
