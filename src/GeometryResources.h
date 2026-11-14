#pragma once
#include "NativeObservations.h"
#include <vector>
class GeometryGroup;
namespace bsc {
// Returns only owned JSON snapshots. Each model is a native GeometryGroup
// resource, not a verified BlockGraphics model association.
std::vector<ActorModelSnapshot> extractNativeGeometryResources(GeometryGroup& group);
} // namespace bsc
