#pragma once
#include "mc/world/level/BlockPos.h"
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace bsc {
// Native tessellation has both block-local and world-space producers.
// Do NOT subtract BlockPos from already-local vertices: it moves entire
// meshes hundreds of blocks away from the player (F7 overlay and F8 export).
template<class Positions>
bool nativePositionsInWorldSpace(Positions const& points,BlockPos const& pos,
                                 std::size_t first=0){
    if(first>=points.size())return false;
    double localError=0.,worldError=0.;
    auto const last=std::min(points.size(),first+32);
    for(std::size_t i=first;i<last;++i){
        auto const& p=points[i];
        for(int axis=0;axis<3;++axis){
            double const v=axis==0?p.x:(axis==1?p.y:p.z);
            double const world=axis==0?pos.x:(axis==1?pos.y:pos.z);
            double const a=v-0.5;
            double const b=v-world-0.5;
            localError+=a*a;
            worldError+=b*b;
        }
    }
    return worldError+0.001<localError;
}
template<class Position>
bool plausibleNativeLocalVertex(Position const& p,BlockPos const& pos,bool world){
    auto const x=p.x-(world?pos.x:0);
    auto const y=p.y-(world?pos.y:0);
    auto const z=p.z-(world?pos.z:0);
    return std::isfinite(x)&&std::isfinite(y)&&std::isfinite(z) &&
        x>=-2.f&&x<=3.f&&y>=-2.f&&y<=3.f&&z>=-2.f&&z<=3.f;
}
} // namespace bsc
