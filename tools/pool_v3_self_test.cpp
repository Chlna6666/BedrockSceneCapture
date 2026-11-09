#include "PoolV3.h"
#include <cassert>
#include <cstdint>
#include <string>
int main(){
    bsc::PoolV3 pool;
    auto g=pool.intern(bsc::PoolKind::Geometry,std::string({char(0),char(1),char(0)}));
    assert(g==0);
    assert(pool.intern(bsc::PoolKind::Geometry,std::string({char(0),char(1),char(0)}))==g);
    assert(pool.intern(bsc::PoolKind::Appearance,std::string("different"))==0);
    assert(pool.intern(bsc::PoolKind::Material,std::string("m"))==0);
    auto bytes=pool.serialize();
    assert(bytes.substr(0,8)=="BSCPOOL3");
    assert(static_cast<unsigned char>(bytes[8])==3);
    assert(bytes.size()==28+36+3+9+1);
    auto read32=[&](std::size_t at){
        std::uint32_t v=0;
        for(unsigned i=0;i<4;++i)v|=static_cast<std::uint32_t>(
          static_cast<unsigned char>(bytes[at+i]))<<(8*i);
        return v;
    };
    assert(read32(12)==1&&read32(16)==1&&read32(20)==1&&read32(24)==0);
    assert(read32(28+8)==3);
    assert(bytes.substr(28+36,3)==std::string({char(0),char(1),char(0)}));
}
