#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bsc {
enum class PoolKind : std::size_t { Geometry=0, Appearance=1, Material=2, Model=3 };
inline void poolU32(std::string& out,std::uint32_t value){
    for(unsigned i=0;i<4;++i)out.push_back(static_cast<char>((value>>(8*i))&0xff));
}
inline void poolU64(std::string& out,std::uint64_t value){
    for(unsigned i=0;i<8;++i)out.push_back(static_cast<char>((value>>(8*i))&0xff));
}
inline void poolF32(std::string& out,float value){
    poolU32(out,std::bit_cast<std::uint32_t>(value==0.f?0.f:value));
}
// Exact bytes are always checked even after an FNV64 fingerprint match.
// No content hash is written to disk or exposed as a resource identifier.
class PoolV3 {
    std::array<std::vector<std::string>,4> entries_{};
    std::array<std::unordered_map<std::uint64_t,std::vector<std::uint32_t>>,4> fingerprints_{};
    static std::uint64_t fingerprint(std::string const& data){
        std::uint64_t result=14695981039346656037ull;
        for(unsigned char byte:data){result^=byte;result*=1099511628211ull;}
        return result;
    }
public:
    std::uint32_t intern(PoolKind kind,std::string data){
        auto const k=static_cast<std::size_t>(kind);
        auto& list=entries_[k];
        auto& candidates=fingerprints_[k][fingerprint(data)];
        for(auto id:candidates)if(list[id]==data)return id;
        if(list.size()>=std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("Geometry Pool v3: ID overflow");
        auto id=static_cast<std::uint32_t>(list.size());
        list.emplace_back(std::move(data));
        candidates.push_back(id);
        return id;
    }
    std::size_t size(PoolKind kind)const{return entries_[static_cast<std::size_t>(kind)].size();}
    // On-disk header: magic[8]="BSCPOOL3"; version u32=3; counts[4] u32.
    // Then each section's (offset u64, byte_length u32) directory records,
    // followed by contiguous payloads. All integers / float32 are LE.
    std::string serialize()const{
        std::string out("BSCPOOL3",8);
        poolU32(out,3);
        std::uint64_t total=0;
        for(auto const& group:entries_){
            if(group.size()>std::numeric_limits<std::uint32_t>::max())
                throw std::runtime_error("Geometry Pool v3: section overflow");
            poolU32(out,static_cast<std::uint32_t>(group.size()));
            total+=group.size();
        }
        if(total>(std::numeric_limits<std::uint64_t>::max()-28)/12)
            throw std::runtime_error("Geometry Pool v3: directory overflow");
        std::uint64_t offset=28+12*total;
        for(auto const& group:entries_)for(auto const& bytes:group){
            if(bytes.size()>std::numeric_limits<std::uint32_t>::max())
                throw std::runtime_error("Geometry Pool v3: payload overflow");
            poolU64(out,offset);
            poolU32(out,static_cast<std::uint32_t>(bytes.size()));
            offset+=bytes.size();
        }
        if(offset>std::numeric_limits<std::size_t>::max())
            throw std::runtime_error("Geometry Pool v3: file overflow");
        out.reserve(static_cast<std::size_t>(offset));
        for(auto const& group:entries_)for(auto const& bytes:group)out.append(bytes);
        return out;
    }
};
} // namespace bsc
