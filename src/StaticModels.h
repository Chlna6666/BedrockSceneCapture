#pragma once
#include <string>
namespace BlockGeometry { struct Model; }
namespace bsc {
std::string serializeStaticGeometry(std::string const& name, BlockGeometry::Model const& model);
}
