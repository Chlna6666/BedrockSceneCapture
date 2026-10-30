#include "Library.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <iomanip>
#include <fstream>
#include <limits>
#include <locale>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace bsc {
namespace {
std::string quoted(std::string const& s) {
    std::ostringstream o;
    o << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\n': o << "\\n"; break;
        case '\r': o << "\\r"; break;
        case '\t': o << "\\t"; break;
        default:
            if (c < 0x20) {
                o << "\\u00" << hex[c >> 4] << hex[c & 15];
            } else o << static_cast<char>(c);
        }
    }
    return o.str() + '"';
}
std::string sha256(std::string const& content) {
    if (content.size() > std::numeric_limits<ULONG>::max())
        throw std::runtime_error("mesh too large to hash");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 provider unavailable");
    std::array<unsigned char, 32> digest{};
    auto status = BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(content.data())),
        static_cast<ULONG>(content.size()), digest.data(),
        static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("SHA256 failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto b : digest) {
        result.push_back(hex[b >> 4]);
        result.push_back(hex[b & 15]);
    }
    return result;
}
void atomicFile(std::filesystem::path const& path, std::string const& data) {
    auto tmp = path;
    tmp += ".tmp";
    try {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot create " + tmp.string());
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) throw std::runtime_error("cannot write " + tmp.string());
        f.close();
        if (!f) throw std::runtime_error("cannot close " + tmp.string());
        std::filesystem::rename(tmp, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        throw;
    }
}
std::vector<std::uint32_t> triangulate(Mesh const& mesh) {
    std::vector<std::uint32_t> source=mesh.indices;
    if(source.empty()){
        source.resize(mesh.vertices.size());
        std::iota(source.begin(),source.end(),0u);
    }
    std::vector<std::uint32_t> triangles;
    auto tri=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c){
        if(a>=mesh.vertices.size()||b>=mesh.vertices.size()||c>=mesh.vertices.size())
            throw std::runtime_error("native mesh has invalid index");
        triangles.insert(triangles.end(),{a,b,c});
    };
    switch(mesh.primitive){
    case 1:
        if(source.size()%4)throw std::runtime_error("incomplete native quad");
        for(std::size_t i=0;i<source.size();i+=4){
            tri(source[i],source[i+1],source[i+2]);
            tri(source[i],source[i+2],source[i+3]);
        }
        break;
    case 2:
        if(source.size()%3)throw std::runtime_error("incomplete native triangle");
        for(std::size_t i=0;i<source.size();i+=3)
            tri(source[i],source[i+1],source[i+2]);
        break;
    case 3:
        for(std::size_t i=2;i<source.size();++i)
            if(i&1)tri(source[i-1],source[i-2],source[i]);
            else tri(source[i-2],source[i-1],source[i]);
        break;
    default: throw std::runtime_error("unsupported primitive topology");
    }
    return triangles;
}
std::string faceBindingJson(Mesh const& mesh,BlockResource const& block,
                            std::uint64_t& unresolved) {
    auto triangles=triangulate(mesh);
    std::ostringstream o;
    o << '[';
    constexpr float epsilon=0.00003f;
    for(std::size_t i=0;i<triangles.size();i+=3){
        if(i)o<<',';
        TextureBinding const* match=nullptr;
        bool ambiguous=false;
        for(auto const& binding:block.textures){
            auto const& rect=binding.atlasUv;
            float const loU=std::min(rect[0],rect[2])-epsilon;
            float const hiU=std::max(rect[0],rect[2])+epsilon;
            float const loV=std::min(rect[1],rect[3])-epsilon;
            float const hiV=std::max(rect[1],rect[3])+epsilon;
            bool inside=true;
            for(std::size_t j=0;j<3;++j){
                auto const& v=mesh.vertices[triangles[i+j]];
                if(v.u<loU||v.u>hiU||v.v<loV||v.v>hiV){inside=false;break;}
            }
            if(!inside)continue;
            if(!match){match=&binding;continue;}
            if(match->defaultName!=binding.defaultName ||
               match->sourcePath!=binding.sourcePath ||
               match->atlasUv!=binding.atlasUv)ambiguous=true;
        }
        if(!match||ambiguous){
            ++unresolved;
            o<<"null";
        }else o<<match->slot;
    }
    o << ']';
    return o.str();
}
std::string meshJson(Mesh const& mesh) {
    auto triangles=triangulate(mesh);
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::setprecision(std::numeric_limits<float>::max_digits10);
    s << "{\"schema\":1,\"topology\":\"triangles\",\"material\":\"terrain/layer_"
      << mesh.layer << "\",\"vertices\":[";
    for (std::size_t i=0;i<mesh.vertices.size();++i) {
        if (i) s << ',';
        auto const& v=mesh.vertices[i];
        s << '[' << v.x << ',' << v.y << ',' << v.z
          << ',' << v.nx << ',' << v.ny << ',' << v.nz
          << ',' << v.u << ',' << v.v << ',' << v.abgr << ']';
    }
    s << "],\"indices\":[";
    for (std::size_t i=0;i<triangles.size();++i) {
        if (i) s << ',';
        s << triangles[i];
    }
    s << "]}\n";
    return s.str();
}
bool containsAny(std::string const& name, std::initializer_list<std::string_view> terms) {
    for (auto term : terms) if (name.find(term) != std::string::npos) return true;
    return false;
}
} // namespace

std::string hexBytes(std::string const& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size()*2);
    for (unsigned char c : bytes) {
        result.push_back(digits[c>>4]);
        result.push_back(digits[c&15]);
    }
    return result;
}
bool requiresNeighbors(std::string const& name) {
    return containsAny(name, {"stairs","fence","wall","pane","water","lava",
        "fluid","rail","redstone","wire","vine","door","trapdoor","chest",
        "fire","snow_layer","grass","kelp","seagrass","piston","scaffolding",
        "tripwire","candle","chorus","bamboo","mushroom","coral","leaves"});
}
bool usesBlockEntityRenderer(std::string const& name) {
    return containsAny(name, {"sign","banner","bed","chest","shulker_box",
        "bell","skull","head","conduit","beacon","campfire","enchanting_table",
        "lectern","decorated_pot","spawner","end_portal"});
}
bool saveLibrary(LibraryData const& data, std::string& error) {
    try {
        std::filesystem::create_directories(data.directory / "meshes");
        std::set<std::uint32_t> layers;
        std::set<std::string> writtenMeshes;
        std::set<std::string> staticModelHashes;
        std::map<std::string,std::string> staticModelIndex;
        std::map<std::string, std::uint32_t> statuses;
        std::ostringstream blocks, coverage;
        blocks << "{\"schema\":1,\"key_encoding\":\"native_block_state_nbt_le_hex\","
                  "\"uv_space\":\"game_terrain_atlas\",\"blocks\":[";
        coverage << "{\"schema\":1,\"states\":[";
        bool firstBlock = true, firstCoverage = true;
        std::uint64_t samples = 0, meshRefs = 0, writeFailures = 0;
        std::uint64_t unmatchedFaces=0;
        for(auto const& model:data.staticModels){
            try{
                auto hash=sha256(model.geometryJson);
                if(staticModelHashes.insert(hash).second)
                    atomicFile(data.directory/"meshes"/(hash+".json"),model.geometryJson);
                staticModelIndex.emplace(model.name,hash);
            }catch(std::exception const&){
                ++writeFailures;
            }
        }
        for (auto const& block : data.blocks) {
            std::string stateStatus;
            std::string reason;
            if (block.air) {
                stateStatus="complete"; reason="air requires no geometry";
            } else if (block.blockEntity) {
                stateStatus="unsupported";
                reason="dedicated block-entity renderer is not hooked; no fabricated geometry";
            } else if (block.dynamic) {
                stateStatus="dynamic";
                reason="neighbor-dependent geometry; samples do not constitute a complete offline rule";
            } else {
                stateStatus="partial";
                reason=block.samples.empty()
                    ? "registered state has no captured native mesh"
                    : "live native mesh samples captured; neighbor-invariance not verified";
            }
            ++statuses[stateStatus];
            if (!firstBlock) blocks << ',';
            firstBlock = false;
            blocks << "{\"name\":" << quoted(block.name)
                   << ",\"state_nbt_hex\":" << quoted(block.stateNbtHex)
                   << ",\"network_id\":" << block.networkId
                   << ",\"legacy_data\":" << block.legacyData
                   << ",\"status\":" << quoted(stateStatus)
                   << ",\"atlas_uv\":[" << block.atlasUv[0] << ',' << block.atlasUv[1]
                   << ',' << block.atlasUv[2] << ',' << block.atlasUv[3] << ']'
                   << ",\"texture_bindings\":[";
            bool firstTexture=true;
            for(auto const& tex:block.textures){
                if(!firstTexture)blocks<<',';
                firstTexture=false;
                blocks << "{\"slot\":" << tex.slot
                       << ",\"resource_alias\":" << quoted(tex.defaultName)
                       << ",\"carried_alias\":" << quoted(tex.carriedName)
                       << ",\"atlas_item\":" << quoted(tex.atlasItem)
                       << ",\"source_path\":" << quoted(tex.sourcePath)
                       << ",\"atlas_uv\":[" << tex.atlasUv[0] << ',' << tex.atlasUv[1]
                       << ',' << tex.atlasUv[2] << ',' << tex.atlasUv[3] << ']'
                       << ",\"tile_size\":[" << tex.tileWidth << ',' << tex.tileHeight << ']'
                       << ",\"source_size\":[" << tex.sourceWidth << ',' << tex.sourceHeight << ']'
                       << ",\"pbr_handle\":" << tex.pbrHandle << '}';
            }
            blocks << "],\"variants\":[";
            bool firstVariant=true;
            for (auto const& sample : block.samples) {
                ++samples;
                if (!firstVariant) blocks << ',';
                firstVariant=false;
                blocks << "{\"neighborhood\":" << quoted(sample.neighborhood)
                       << ",\"mesh_refs\":[";
                bool firstMesh=true;
                std::vector<std::string> perMeshTextureSlots;
                std::uint64_t localUnmatched=0;
                for (auto const& mesh : sample.meshes) {
                    try {
                        auto content = meshJson(mesh);
                        auto hash = sha256(content);
                        if (writtenMeshes.insert(hash).second)
                            atomicFile(data.directory/"meshes"/(hash+".json"),content);
                        layers.insert(mesh.layer);
                        if (!firstMesh) blocks << ',';
                        firstMesh=false;
                        blocks << quoted(hash);
                        perMeshTextureSlots.push_back(faceBindingJson(mesh,block,localUnmatched));
                        ++meshRefs;
                    } catch (std::exception const&) {
                        ++writeFailures;
                    }
                }
                blocks << "],\"mesh_triangle_texture_slots\":[";
                for(std::size_t i=0;i<perMeshTextureSlots.size();++i){
                    if(i)blocks<<',';
                    blocks<<perMeshTextureSlots[i];
                }
                unmatchedFaces+=localUnmatched;
                blocks << "],\"unresolved_texture_triangles\":" << localUnmatched
                       << ",\"missing_meshes\":" << sample.missing << '}';
            }
            blocks << "]}";
            if (stateStatus!="complete") {
                if (!firstCoverage) coverage << ',';
                firstCoverage=false;
                coverage << "{\"name\":" << quoted(block.name)
                         << ",\"state_nbt_hex\":" << quoted(block.stateNbtHex)
                         << ",\"status\":" << quoted(stateStatus)
                         << ",\"reason\":" << quoted(reason) << '}';
            }
        }
        blocks << "]}\n";
        coverage << "],\"counts\":{";
        for (auto const& key : {"complete","dynamic","partial","unsupported"}) {
            if (std::string_view(key)!="complete") coverage << ',';
            coverage << quoted(key) << ':' << statuses[key];
        }
        coverage << "},\"mesh_write_failures\":" << writeFailures
                 << ",\"limitations\":[\"No complete offline neighbor rules exported\","
                    "\"No fluid surface reconstruction\","
                    "\"No block entity geometry or NBT renderer\","
                    "\"No entity skeletons or animation clips\","
                    "\"No verified local texture mapping or resource pack fingerprint\"]}\n";
        std::ostringstream modelIndex;
        modelIndex << "{\"schema\":1,\"source\":\"native_block_geometry_registry\","
                      "\"associations_verified\":false,\"models\":[";
        bool firstModel=true;
        for(auto const& [name,hash]:staticModelIndex){
            if(!firstModel)modelIndex<<',';
            firstModel=false;
            modelIndex << "{\"name\":" << quoted(name)
                       << ",\"mesh_ref\":" << quoted(hash) << '}';
        }
        modelIndex << "]}\n";
        std::ostringstream materials;
        materials << "{\"schema\":1,\"texture_source\":\"local_game_installation\","
                     "\"texture_payload_exported\":false,\"materials\":[";
        bool firstLayer=true;
        for (auto layer : layers) {
            if (!firstLayer) materials << ',';
            firstLayer=false;
            materials << "{\"id\":\"terrain/layer_" << layer
                      << "\",\"render_layer\":" << layer
                      << ",\"uv_space\":\"terrain_atlas\","
                         "\"atlas_ref\":\"terrain_atlas\","
                         "\"texture_resolution\":\"external\","
                         "\"blend_mode\":\"unresolved\"}";
        }
        materials << "],\"atlas_binding\":{"
                        "\"coordinate_space\":\"runtime_atlas_uv\","
                        "\"asset_resolver\":\"local_resource_pack_terrain_texture_json\","
                        "\"face_texture_slot_mapping\":\"blocks.variants.mesh_triangle_texture_slots\","
                        "\"slot_assets\":\"blocks.texture_bindings\","
                        "\"require_exact_atlas_layout\":false,"
                        "\"remap_via_original_slot_uv_rect\":true,"
                        "\"unresolved_face_policy\":\"report_not_fallback\"}}\n";
        std::ostringstream entities;
        entities << "{\"schema\":1,\"geometry\":[],\"skeletons\":[],\"animations\":[],"
                    "\"status\":\"unsupported\","
                    "\"reason\":\"No actor or block-entity renderer capture hook installed\"}\n";
        std::ostringstream manifest;
        manifest << "{\"schema\":1,\"format\":\"BedrockSceneCaptureModelLibrary\","
                    "\"target_game_version\":\"1.26.51.1\","
                    "\"levilamina_sdk_version\":\"26.51.9\","
                    "\"resource_pack_fingerprint\":null,"
                    "\"resource_pack_fingerprint_status\":\"not_captured\","
                    "\"textures_exported\":false,"
                    "\"native_meshes_unique\":" << writtenMeshes.size()
                 << ",\"native_mesh_refs\":" << meshRefs
                 << ",\"static_geometry_models\":" << staticModelIndex.size()
                 << ",\"unresolved_texture_triangles\":" << unmatchedFaces
                 << ",\"sample_contexts\":" << samples
                 << ",\"registered_block_states\":" << data.registered
                 << ",\"loaded_chunks_scanned\":" << data.visitedChunks
                 << ",\"loaded_chunks_skipped\":" << data.skippedChunks
                 << ",\"sampled_blocks\":" << data.sampledBlocks
                 << ",\"failed_samples\":" << data.failedSamples + writeFailures
                 << ",\"cancelled\":" << (data.cancelled?"true":"false")
                 << ",\"complete\":false}\n";
        // Commit metadata last; an incomplete export is never advertised as complete.
        atomicFile(data.directory/"blocks.json",blocks.str());
        atomicFile(data.directory/"geometry_catalog.json",modelIndex.str());
        atomicFile(data.directory/"entities.json",entities.str());
        atomicFile(data.directory/"materials.json",materials.str());
        atomicFile(data.directory/"coverage.json",coverage.str());
        atomicFile(data.directory/"manifest.json",manifest.str());
        return true;
    } catch (std::exception const& e) {
        error=e.what();
        return false;
    }
}
} // namespace bsc
