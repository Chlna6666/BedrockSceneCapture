#include "Format.h"
#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <type_traits>
namespace bsc {
namespace {
template<class T> void le(std::ostream& stream,T value) {
    using U=std::make_unsigned_t<T>;
    U v=static_cast<U>(value);
    for(std::size_t i=0;i<sizeof(T);++i) stream.put(static_cast<char>((v>>(i*8))&0xffu));
}
void fl(std::ostream& s,float v) {le(s,std::bit_cast<std::uint32_t>(v));}
void str(std::ostream& s,std::string const& v) {
    if(v.size()>std::numeric_limits<std::uint32_t>::max()) throw std::length_error("string too large");
    le(s,static_cast<std::uint32_t>(v.size()));
    s.write(v.data(),static_cast<std::streamsize>(v.size()));
}
void commit(std::ofstream& out,std::filesystem::path const& temp,std::filesystem::path const& dest) {
    out.flush();
    if(!out) throw std::runtime_error("flush failed");
    out.close();
    if(!out) throw std::runtime_error("close failed");
    std::filesystem::rename(temp,dest);
}
}
bool saveChunk(std::filesystem::path const& dir,ChunkData const& c,std::string& error) {
    auto dst=dir/("chunk_"+std::to_string(c.x)+"_"+std::to_string(c.z)+".bsc");
    auto tmp=dst;tmp+=".tmp";
    try {
        std::filesystem::create_directories(dir);
        std::ofstream s(tmp,std::ios::binary|std::ios::trunc);
        if(!s) throw std::runtime_error("failed to create file");
        s.write("BSC1",4);le(s,std::uint32_t{1});
        le(s,std::int32_t(c.x));le(s,std::int32_t(c.z));
        le(s,std::int32_t(c.minY));le(s,std::int32_t(c.maxY));
        le(s,std::uint32_t(c.palette.size()));
        le(s,std::uint32_t(c.cells.size()));
        le(s,std::uint32_t(c.meshes.size()));
        le(s,c.missingMeshes);
        for(auto const& p:c.palette) {
            str(s,p.name);str(s,p.serializedBlockNbt);
            le(s,p.networkId);le(s,p.legacyData);
            for(float f:p.atlasUv) fl(s,f);
        }
        for(auto id:c.cells) le(s,id);
        for(auto const& mesh:c.meshes) {
            le(s,mesh.cell);le(s,mesh.layer);le(s,mesh.primitive);
            le(s,std::uint32_t(mesh.vertices.size()));
            le(s,std::uint32_t(mesh.indices.size()));
            for(auto const& v:mesh.vertices) {
                for(float f:{v.x,v.y,v.z,v.nx,v.ny,v.nz,v.u,v.v}) fl(s,f);
                le(s,v.abgr);
            }
            for(auto i:mesh.indices) le(s,i);
        }
        commit(s,tmp,dst);return true;
    }catch(std::exception const& e) {
        error=e.what();
        std::error_code ec;std::filesystem::remove(tmp,ec);return false;
    }
}
bool saveManifest(FinishData const& f,std::string& error) {
    auto dst=f.directory/"capture.json";auto tmp=dst;tmp+=".tmp";
    try {
        std::filesystem::create_directories(f.directory);
        std::ofstream s(tmp,std::ios::binary|std::ios::trunc);
        if(!s) throw std::runtime_error("manifest create failed");
        s<<"{\n  \"format\": \"BSC1\",\n  \"version\": 1,\n  \"game_version\": \"26.51.x\","
          "\n  \"coordinate_order\": \"y,z,x\","
          "\n  \"uv_space\": \"terrain_atlas\","
          "\n  \"chunks_requested\": "<<f.requested
         <<",\n  \"chunks_queued\": "<<f.queued
         <<",\n  \"chunks_skipped\": "<<f.skipped
         <<",\n  \"cancelled\": "<<(f.cancelled?"true":"false")
         <<"\n}\n";
        commit(s,tmp,dst);return true;
    }catch(std::exception const& e){
        error=e.what();std::error_code ec;std::filesystem::remove(tmp,ec);return false;
    }
}
}