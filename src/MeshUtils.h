#pragma once
#include "Format.h"
class Block;
class BlockPos;
class BlockSource;
class BlockTessellator;
class Tessellator;
namespace bsc {
void captureMesh(BlockSource&,BlockTessellator&,Tessellator&,Block const&,BlockPos const&,std::uint32_t,ChunkData&);
}