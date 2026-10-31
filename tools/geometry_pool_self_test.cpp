#include "GeometryPool.h"
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

int main(){
    using namespace bsc;
    Mesh a;
    a.primitive=2;
    a.layer=5;
    a.uvValid=true;
    a.vertices={
        {0,0,0,0,0,1,0,0,0xffffffffu},
        {1,0,0,0,0,1,1,0,0xffffffffu},
        {1,1,0,0,0,1,1,1,0xffffffffu},
        {0,1,0,0,0,1,0,1,0xffffffffu}
    };
    a.indices={0,1,2,0,2,3};
    auto const baseline=encodeSharedGeometry(a);
    assert(baseline.canonicalTriangles.size()==2);
    auto const baselineAppearance=encodeSharedAppearance(
        a,baseline,"canonical-geometry","[2,3]","[null,[0,0]]");
    // Triangle order and cyclic starting corner do not affect the shared
    // geometry, appearance stream or the original per-face texture binding.
    auto reordered=a;
    reordered.indices={2,3,0,1,2,0};
    auto const reorderedGeometry=encodeSharedGeometry(reordered);
    auto const reorderedAppearance=encodeSharedAppearance(
        reordered,reorderedGeometry,"canonical-geometry","[3,2]","[[0,0],null]");
    assert(reorderedGeometry.geometryJson==baseline.geometryJson);
    assert(reorderedAppearance==baselineAppearance);

    // Changing only texture coordinates or material layer MUST NOT create a
    // new GPU geometry resource. Appearance data must change independently.
    auto recolored=a;
    recolored.vertices[0].u=.375f;
    recolored.layer=8;
    auto const recoloredGeometry=encodeSharedGeometry(recolored);
    assert(recoloredGeometry.geometryJson==baseline.geometryJson);
    assert(encodeSharedAppearance(recolored,recoloredGeometry,
        "canonical-geometry","[2,3]","[null,[0,0]]")!=baselineAppearance);

    // A changed triangle winding is not an equivalent mesh.
    auto flipped=a;
    flipped.indices={0,2,1,0,3,2};
    assert(encodeSharedGeometry(flipped).geometryJson!=baseline.geometryJson);

    // The exact quad tessellation and its explicit triangles are equivalent.
    auto quads=a;
    quads.primitive=1;
    quads.indices={0,1,2,3};
    assert(encodeSharedGeometry(quads).geometryJson==baseline.geometryJson);

    // Invalid native index positions and non-finite positions are rejected.
    auto invalid=a;
    invalid.indices={0,1,99};
    bool rejected=false;
    try{(void)encodeSharedGeometry(invalid);}
    catch(std::runtime_error const&){rejected=true;}
    assert(rejected);
    invalid=a;
    invalid.vertices[0].x=std::numeric_limits<float>::quiet_NaN();
    rejected=false;
    try{(void)encodeSharedGeometry(invalid);}
    catch(std::runtime_error const&){rejected=true;}
    assert(rejected);

    // An appearance stream must have one binding per original triangle.
    rejected=false;
    try{(void)encodeSharedAppearance(a,baseline,"canonical-geometry","[2]","[null]");}
    catch(std::runtime_error const&){rejected=true;}
    assert(rejected);
}
