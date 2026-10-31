#include "Library.h"
#include "GeometryPool.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
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
// Immutable SHA-256 addressed pool entries can be reused between exports.
// Verify bytes rather than trusting the file name alone, so a partial or
// manually corrupted cache file is repaired on the next export.
bool contentAddressedFile(std::filesystem::path const& path,
                          std::string const& data);
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
        // MSVC's filesystem rename semantics have varied across CRT
        // versions when the destination already exists. Use an explicit
        // atomic replacement for repeated F8 exports on Windows.
        if(!MoveFileExW(tmp.c_str(),path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)){
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "cannot atomically replace "+path.string());
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        throw;
    }
}

bool contentAddressedFile(std::filesystem::path const& path,
                          std::string const& data){
    std::error_code ec;
    if(std::filesystem::is_regular_file(path,ec) && !ec &&
       std::filesystem::file_size(path,ec)==data.size() && !ec){
        std::ifstream stream(path,std::ios::binary);
        if(stream){
            std::array<char,16384> buffer{};
            std::size_t offset=0;
            bool equal=true;
            while(offset<data.size()){
                std::size_t n=std::min(buffer.size(),data.size()-offset);
                stream.read(buffer.data(),static_cast<std::streamsize>(n));
                if(stream.gcount()!=static_cast<std::streamsize>(n)||
                   std::memcmp(buffer.data(),data.data()+offset,n)!=0){
                    equal=false;
                    break;
                }
                offset+=n;
            }
            if(equal && offset==data.size())return false;
        }
    }
    atomicFile(path,data);
    return true;
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
// Geometry identity ignores UV, vertex tint and render layer, but preserves
// topology and winding. This groups stone/wood wall textures and waterlogged
// variants by actual native shape rather than by BlockState name.
std::string geometryShapeHash(Mesh const& mesh){
    auto const triangles=triangulate(mesh);
    using Point=std::array<std::int64_t,3>;
    using Face=std::array<Point,3>;
    std::vector<Face> faces;
    faces.reserve(triangles.size()/3);
    auto quantize=[](float v)->std::int64_t{
        if(!std::isfinite(v)||std::abs(v)>4096.f)
            throw std::runtime_error("native shape has non-finite/out-of-range vertex");
        return static_cast<std::int64_t>(std::llround(static_cast<double>(v)*65536.));
    };
    for(std::size_t i=0;i<triangles.size();i+=3){
        Face face{};
        for(int j=0;j<3;++j){
            auto const& v=mesh.vertices[triangles[i+static_cast<std::size_t>(j)]];
            face[static_cast<std::size_t>(j)]={quantize(v.x),quantize(v.y),quantize(v.z)};
        }
        // Same directed triangle may start at any of its three corners.
        if(face[1]<face[0]&&face[1]<=face[2])
            std::rotate(face.begin(),face.begin()+1,face.end());
        else if(face[2]<face[0]&&face[2]<face[1])
            std::rotate(face.begin(),face.begin()+2,face.end());
        faces.emplace_back(face);
    }
    std::sort(faces.begin(),faces.end());
    std::ostringstream s;
    for(auto const& face:faces)
        for(auto const& p:face)s<<p[0]<<','<<p[1]<<','<<p[2]<<';';
    return sha256(s.str());
}
struct TriangleBindings {
    std::string slots;
    std::string variants;
    std::uint64_t unresolved{};
};
TriangleBindings faceBindingJson(Mesh const& mesh,BlockResource const& block) {
    auto triangles=triangulate(mesh);
    std::ostringstream slots, variants;
    slots << '[';
    variants << '[';
    constexpr float epsilon=0.00003f;
    std::uint64_t unresolved=0;
    if(mesh.registryVisualBounds){
        // Vertex UV is face-local, not an atlas coordinate; BMCBL resolves
        // the named textures using the resource-pack definitions.
        for(std::size_t i=0;i<mesh.faceSlots.size();++i){
            if(i){slots<<',';variants<<',';}
            auto slot=mesh.faceSlots[i];
            auto const& bindings=block.textureBindings();
            if(slot<bindings.size() && !bindings[slot].defaultName.empty()) slots<<slot;
            else if(bindings.size()==1 && !bindings[0].defaultName.empty()) slots<<bindings[0].slot;
            else {slots<<"null";++unresolved;}
            variants<<"null";
        }
        slots<<']';
        variants<<']';
        return {slots.str(),variants.str(),unresolved};
    }
    for(std::size_t i=0;i<triangles.size();i+=3){
        if(!mesh.uvValid){
            if(i){slots<<',';variants<<',';}
            slots<<"null";
            variants<<"null";
            ++unresolved;
            continue;
        }
        if(i){slots<<',';variants<<',';}
        TextureBinding const* selected=nullptr;
        TextureUVVariant const* selectedVariant=nullptr;
        bool ambiguous=false;
        for(auto const& texture:block.textureBindings()){
            for(auto const& candidate:texture.variants){
                auto const& rect=candidate.atlasUv;
                auto loU=std::min(rect[0],rect[2])-epsilon;
                auto hiU=std::max(rect[0],rect[2])+epsilon;
                auto loV=std::min(rect[1],rect[3])-epsilon;
                auto hiV=std::max(rect[1],rect[3])+epsilon;
                bool inside=true;
                for(std::size_t j=0;j<3;++j){
                    auto const& v=mesh.vertices[triangles[i+j]];
                    if(v.u<loU||v.u>hiU||v.v<loV||v.v>hiV){inside=false;break;}
                }
                if(!inside)continue;
                if(!selected){
                    selected=&texture;
                    selectedVariant=&candidate;
                }else if(candidate.sourcePath!=selectedVariant->sourcePath ||
                         candidate.atlasUv!=selectedVariant->atlasUv ||
                         candidate.pbrHandle!=selectedVariant->pbrHandle ||
                         texture.additive!=selected->additive){
                    // Different slots can be perfectly equivalent aliases
                    // for the same native atlas rectangle and material.
                    // Only conflicting texture identities are ambiguous.
                    ambiguous=true;
                }
            }
        }
        if(!selected || ambiguous){
            ++unresolved;
            slots<<"null";
            variants<<"null";
        }else{
            slots<<selected->slot;
            variants<<'['<<selectedVariant->variant<<','<<selectedVariant->frame<<']';
        }
    }
    slots << ']';
    variants << ']';
    return {slots.str(),variants.str(),unresolved};
}
std::string meshJson(Mesh const& mesh) {
    auto triangles=triangulate(mesh);
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::setprecision(std::numeric_limits<float>::max_digits10);
    s << "{\"schema\":1,\"topology\":\"triangles\",\"uv_complete\":"
      << (mesh.uvValid?"true":"false")
      << ",\"geometry_source\":\""
      << (mesh.registryCrossPlanes?"registry_cross_planes":(mesh.registryVisualBounds?"registry_visual_bounds":"native_world_tessellation"))
      << "\",\"uv_space\":\""
      << (mesh.registryVisualBounds?"block_face_local":"runtime_terrain_atlas")
      << "\",\"normals_source\":\""
      << (mesh.normalsReconstructed?"reconstructed_triangle_winding":
          (mesh.registryVisualBounds?"registry_geometry":"native_or_unavailable"))
      << "\",\"material\":\"terrain/layer_"
      << mesh.layer << "\",\"vertices\":[";
    for (std::size_t i=0;i<mesh.vertices.size();++i) {
        if (i) s << ',';
        auto const& v=mesh.vertices[i];
        s << '[' << v.x << ',' << v.y << ',' << v.z
          << ',' << v.nx << ',' << v.ny << ',' << v.nz
          << ',' << v.u << ',' << v.v << ',' << v.abgr << ']';
    }
    s << ']';
    if(mesh.registryVisualBounds){
        s << ",\"geometry_confidence\":\"visual_shape_proxy\"";
        if(!mesh.registryCrossPlanes)
            s << ",\"face_order\":[\"up\",\"down\",\"north\",\"south\",\"west\",\"east\"]";
        s << ",\"face_index_per_triangle\":[";
        for(std::size_t i=0;i<mesh.faceSlots.size();++i){
            if(i)s<<',';
            s<<mesh.faceSlots[i];
        }
        s << ']';
    }
    s << ",\"indices\":[";
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
        std::filesystem::create_directories(data.directory / "geometry_pool" / "models");
        std::filesystem::create_directories(data.directory / "geometry_pool" / "geometry");
        std::filesystem::create_directories(data.directory / "geometry_pool" / "appearance");
        std::filesystem::create_directories(data.directory / "geometry_pool" / "materials");
        // Invalidate any manifest from an earlier run before rewriting the
        // version-scoped indexes. If this export crashes or I/O fails,
        // a stale manifest must not advertise old results as current.
        {
            std::error_code ec;
            std::filesystem::remove(data.directory / "manifest.json",ec);
            if(ec)throw std::system_error(ec,"cannot invalidate previous manifest");
        }
        std::set<std::uint32_t> layers;
        // Registry proxy meshes are shared across many permutations; avoid
        // re-triangulating, re-serializing and re-hashing the same object.
        std::set<std::string> uniqueExactGeometries;
        std::set<std::string> uniqueAppearances;
        std::set<std::string> uniqueMaterials;
        std::map<std::vector<TextureBinding> const*,std::string> materialTableRefs;
        std::map<Mesh const*,SharedMeshEncoding> canonicalGeometryCache;
        std::map<Mesh const*,std::string> canonicalGeometryRefs;
        std::map<std::pair<Mesh const*,std::vector<TextureBinding> const*>,std::string> canonicalAppearanceRefs;
        std::map<std::pair<Mesh const*,std::vector<TextureBinding> const*>,TriangleBindings> memoizedBindings;
        std::map<std::vector<TextureBinding> const*,std::string> memoizedTextureTables;
        std::set<std::string> staticModelHashes;
        std::map<std::string,std::string> staticModelIndex;
        std::map<std::string, std::uint32_t> statuses;
        std::set<std::string> capturedActorModelKeys;
        for(auto const& actor:data.actorModels)
            if(!actor.geometryJson.empty())capturedActorModelKeys.insert(actor.name);
        std::ostringstream blocks, coverage;
        blocks << "{\"schema\":1,\"key_encoding\":\"native_block_state_nbt_le_hex\","
                  "\"uv_space\":\"per_mesh_declared\",\"blocks\":[";
        blocks.imbue(std::locale::classic());
        blocks << std::setprecision(std::numeric_limits<float>::max_digits10);
        coverage << "{\"schema\":1,\"states\":[";
        bool firstBlock = true, firstCoverage = true;
        std::uint64_t samples = 0, meshRefs = 0, writeFailures = 0;
        std::uint64_t unmatchedFaces=0;
        for(auto const& model:data.staticModels){
            try{
                auto hash=sha256(model.geometryJson);
                if(!staticModelHashes.contains(hash)){
                    (void)contentAddressedFile(data.directory/"geometry_pool"/"models"/(hash+".json"),model.geometryJson);
                    staticModelHashes.insert(hash);
                }
                staticModelIndex.emplace(model.name,hash);
            }catch(std::exception const&){
                ++writeFailures;
            }
        }
        for (auto const& block : data.blocks) {
            if(block.renderLayer>=0) layers.insert(static_cast<std::uint32_t>(block.renderLayer));
            std::string stateStatus;
            std::string reason;
            if (block.air) {
                stateStatus="complete"; reason="air requires no geometry";
            } else if (block.blockEntity) {
                bool hasActorGeometry=false;
                for(auto const& key:block.actorModelKeys)
                    if(capturedActorModelKeys.contains(key)){hasActorGeometry=true;break;}
                stateStatus=hasActorGeometry?"partial":"unsupported";
                reason=hasActorGeometry
                    ? "native BlockActor base geometry captured; animation, text, NBT and variant transforms remain unresolved"
                    : "no usable native BlockActor model observed for this block family";
            } else if (block.dynamic) {
                stateStatus="dynamic";
                reason="neighbor-dependent geometry; samples do not constitute a complete offline rule";
            } else {
                stateStatus="partial";
                reason=block.hasGeometryAlias
                    ? "audio-only state refers to canonical visual geometry"
                    : (block.samples.empty()
                        ? "non-box native shape requires geometry rules or engine tessellation"
                        : "native visual bounds triangulated; scene-specific tessellation not verified");
            }
            ++statuses[stateStatus];
            if (!firstBlock) blocks << ',';
            firstBlock = false;
            blocks << "{\"name\":" << quoted(block.name)
                   << ",\"state_nbt_hex\":" << quoted(block.stateNbtHex)
                   << ",\"network_id\":" << block.networkId
                   << ",\"geometry_alias_network_id\":";
            if(block.hasGeometryAlias)blocks << block.geometryAliasNetworkId;
            else blocks << "null";
            blocks << ",\"legacy_data\":" << block.legacyData
                   << ",\"status\":" << quoted(stateStatus)
                   << ",\"native_render_layer\":" << block.renderLayer
                   << ",\"block_actor_type\":" << block.blockActorType
                   << ",\"actor_model_keys\":[";
            bool firstActorKey=true;
            for(auto const& key:block.actorModelKeys){
                if(!firstActorKey)blocks<<',';
                firstActorKey=false;
                blocks<<quoted(key);
            }
            blocks << "],\"captured_actor_model_keys\":[";
            firstActorKey=true;
            for(auto const& key:block.actorModelKeys){
                if(!capturedActorModelKeys.contains(key))continue;
                if(!firstActorKey)blocks<<',';
                firstActorKey=false;
                blocks<<quoted(key);
            }
            blocks << "],\"native_block_shape\":" << block.nativeShape
                   << ",\"geometry_context\":" << quoted(block.geometryContext)
                   << ",\"visual_bounds_mesh_generated\":"
                   << (block.visualBoundsAvailable?"true":"false")
                   << ",\"visual_bounds\":["
                   << block.visualBounds[0] << ',' << block.visualBounds[1] << ',' << block.visualBounds[2]
                   << ',' << block.visualBounds[3] << ',' << block.visualBounds[4] << ',' << block.visualBounds[5]
                   << "],\"atlas_uv\":[" << block.atlasUv[0] << ',' << block.atlasUv[1]
                   << ',' << block.atlasUv[2] << ',' << block.atlasUv[3] << ']'
                   << ",\"shared_material_ref\":";
            if(block.hasGeometryAlias){
                blocks<<"null";
            }else{
                auto key=block.textures.get();
                auto cached=memoizedTextureTables.find(key);
                if(cached==memoizedTextureTables.end()){
                    std::ostringstream textureStream;
                    textureStream.imbue(std::locale::classic());
                    textureStream<<std::setprecision(std::numeric_limits<float>::max_digits10);
                    textureStream<<'[';
            bool firstTexture=true;
            // Audio-only state aliases inherit the canonical model's face
            // texture bindings; repeating atlas tables for every note pitch
            // would waste far more space than the shared mesh itself.
            for(auto const& tex:block.textureBindings()){
                if(!firstTexture)textureStream<<',';
                firstTexture=false;
                textureStream << "{\"slot\":" << tex.slot
                       << ",\"resource_alias\":" << quoted(tex.defaultName)
                       << ",\"carried_alias\":" << quoted(tex.carriedName)
                       << ",\"atlas_item\":" << quoted(tex.atlasItem)
                       << ",\"source_path\":" << quoted(tex.sourcePath)
                       << ",\"atlas_uv\":[" << tex.atlasUv[0] << ',' << tex.atlasUv[1]
                       << ',' << tex.atlasUv[2] << ',' << tex.atlasUv[3] << ']'
                       << ",\"tile_size\":[" << tex.tileWidth << ',' << tex.tileHeight << ']'
                       << ",\"source_size\":[" << tex.sourceWidth << ',' << tex.sourceHeight << ']'
                       << ",\"pbr_handle\":" << tex.pbrHandle
                       << ",\"additive\":" << (tex.additive?"true":"false")
                       << ",\"atlas_variants\":[";
                auto writeVariants=[&](std::vector<TextureUVVariant> const& atlasVariants){
                    bool firstUV=true;
                    for(auto const& variant:atlasVariants){
                        if(!firstUV)textureStream<<',';
                        firstUV=false;
                        textureStream << "{\"variant\":" << variant.variant
                               << ",\"frame\":" << variant.frame
                               << ",\"source_path\":" << quoted(variant.sourcePath)
                               << ",\"atlas_uv\":[" << variant.atlasUv[0] << ',' << variant.atlasUv[1]
                               << ',' << variant.atlasUv[2] << ',' << variant.atlasUv[3] << ']'
                               << ",\"tile_size\":[" << variant.tileWidth << ',' << variant.tileHeight << ']'
                               << ",\"source_size\":[" << variant.sourceWidth << ',' << variant.sourceHeight << ']'
                               << ",\"pbr_handle\":" << variant.pbrHandle << '}';
                    }
                };
                writeVariants(tex.variants);
                textureStream << "],\"carried_atlas_item\":" << quoted(tex.carriedAtlasItem)
                       << ",\"carried_additive\":" << (tex.carriedAdditive?"true":"false")
                       << ",\"carried_atlas_variants\":[";
                writeVariants(tex.carriedVariants);
                textureStream << "]}";
            }
                    textureStream<<']';
                    cached=memoizedTextureTables.emplace(key,textureStream.str()).first;
                }
                auto materialIt=materialTableRefs.find(key);
                if(materialIt==materialTableRefs.end()){
                    std::string content="{\"schema\":2,\"texture_bindings\":"
                                      +cached->second+"}\n";
                    auto hash=sha256(content);
                    if(uniqueMaterials.insert(hash).second)
                        (void)contentAddressedFile(data.directory/"geometry_pool"/"materials"/(hash+".json"),content);
                    materialIt=materialTableRefs.emplace(key,std::move(hash)).first;
                }
                blocks << quoted(materialIt->second);
            }
            blocks << ",\"variants\":[";
            bool firstVariant=true;
            for (auto const& sample : block.samples) {
                ++samples;
                if (!firstVariant) blocks << ',';
                firstVariant=false;
                blocks << "{\"neighborhood\":" << quoted(sample.neighborhood);
                if(sample.nativeObserved){
                    blocks << ",\"capture_source\":\"native_block_tessellator\","
                              "\"neighbor_network_ids\":[";
                    for(std::size_t n=0;n<sample.neighborStateIds.size();++n){
                        if(n)blocks<<',';
                        blocks<<sample.neighborStateIds[n];
                    }
                    blocks<<"],\"observation_position\":["
                          <<sample.observedBlockPosition[0]<<','
                          <<sample.observedBlockPosition[1]<<','
                          <<sample.observedBlockPosition[2]<<']'
                          <<",\"position_variant_candidate\":"
                          <<(sample.positionVariantCandidate?"true":"false");
                }
                std::vector<std::string> perMeshSharedGeometry;
                std::vector<std::string> perMeshSharedAppearance;
                std::uint64_t localUnmatched=0;
                for (auto const& meshRef : sample.meshes) {
                    try {
                        if(!meshRef)continue;
                        Mesh const& mesh=*meshRef;
                        auto bindingKey=std::make_pair(&mesh,block.textures.get());
                        auto bindingIt=memoizedBindings.find(bindingKey);
                        if(bindingIt==memoizedBindings.end())
                            bindingIt=memoizedBindings.emplace(bindingKey,faceBindingJson(mesh,block)).first;
                        auto const& bindings=bindingIt->second;
                        // V2 shared geometry is independent of UV, tint, material
                        // and state. Exact float32 positions and triangle winding
                        // prevent the lossy quantization of legacy geometry hashes.
                        auto encoded=canonicalGeometryCache.find(&mesh);
                        if(encoded==canonicalGeometryCache.end())
                            encoded=canonicalGeometryCache.emplace(
                                &mesh,encodeSharedGeometry(mesh)).first;
                        auto geoRef=canonicalGeometryRefs.find(&mesh);
                        if(geoRef==canonicalGeometryRefs.end()){
                            std::string geoHash=sha256(encoded->second.geometryJson);
                            if(uniqueExactGeometries.insert(geoHash).second)
                                (void)contentAddressedFile(data.directory/"geometry_pool"/"geometry"/(geoHash+".json"),
                                           encoded->second.geometryJson);
                            geoRef=canonicalGeometryRefs.emplace(&mesh,std::move(geoHash)).first;
                        }
                        auto appRef=canonicalAppearanceRefs.find(bindingKey);
                        if(appRef==canonicalAppearanceRefs.end()){
                            auto appearance=encodeSharedAppearance(mesh,encoded->second,geoRef->second,
                                bindings.slots,bindings.variants);
                            auto appHash=sha256(appearance);
                            if(uniqueAppearances.insert(appHash).second)
                                (void)contentAddressedFile(data.directory/"geometry_pool"/"appearance"/(appHash+".json"),
                                           appearance);
                            appRef=canonicalAppearanceRefs.emplace(bindingKey,std::move(appHash)).first;
                        }
                        perMeshSharedGeometry.push_back(geoRef->second);
                        perMeshSharedAppearance.push_back(appRef->second);
                        layers.insert(mesh.layer);
                        localUnmatched+=bindings.unresolved;
                        ++meshRefs;
                    } catch (std::exception const&) {
                        ++writeFailures;
                    }
                }
                blocks << ",\"shared_geometry_refs\":[";

                for(std::size_t i=0;i<perMeshSharedGeometry.size();++i){
                    if(i)blocks<<',';
                    blocks<<quoted(perMeshSharedGeometry[i]);
                }
                blocks << "],\"shared_appearance_refs\":[";
                for(std::size_t i=0;i<perMeshSharedAppearance.size();++i){
                    if(i)blocks<<',';
                    blocks<<quoted(perMeshSharedAppearance[i]);
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
        coverage << "},\"shared_geometry_count\":" << uniqueExactGeometries.size()
                 << ",\"shared_geometry_references\":" << meshRefs
                 << ",\"mesh_write_failures\":" << writeFailures
                 << ",\"limitations\":[\"No complete offline neighbor rules exported\","
                    "\"No fluid surface reconstruction\","
                    "\"No block entity geometry or NBT renderer\","
                    "\"No entity skeletons or animation clips\","
                    "\"No verified local texture mapping or resource pack fingerprint\"]}\n";
        // Immutable v2 geometry, appearance and material streams only.
        std::ostringstream sharedIndex;
        sharedIndex << "{\"schema\":2,\"format\":\"BSCSharedGeometryPool\","
                    "\"geometry_precision\":\"exact_float32\","
                    "\"rotation_equivalence\":false,"
                    "\"neighbor_rules_complete\":false,"
                    "\"legacy_meshes_retained\":false,\"geometry\":[";
        bool firstShared=true;
        for(auto const& hash:uniqueExactGeometries){
            if(!firstShared)sharedIndex<<',';
            firstShared=false;
            sharedIndex<<quoted(hash);
        }
        sharedIndex << "],\"appearance\":[";
        firstShared=true;
        for(auto const& hash:uniqueAppearances){
            if(!firstShared)sharedIndex<<',';
            firstShared=false;
            sharedIndex<<quoted(hash);
        }
        sharedIndex<<"],\"materials\":[";
        firstShared=true;
        for(auto const& hash:uniqueMaterials){
            if(!firstShared)sharedIndex<<',';
            firstShared=false;
            sharedIndex<<quoted(hash);
        }
        sharedIndex<<"]}\n";
        std::ostringstream modelIndex;
        modelIndex << "{\"schema\":1,\"source\":\"native_block_geometry_registry\","
                      "\"associations_verified\":false,\"models\":[";
        bool firstModel=true;
        for(auto const& [name,hash]:staticModelIndex){
            if(!firstModel)modelIndex<<',';
            firstModel=false;
            modelIndex << "{\"name\":" << quoted(name)
                       << ",\"model_ref\":" << quoted(hash) << '}';
        }
        modelIndex << "]}\n";
        std::ostringstream materials;
        materials << "{\"schema\":1,\"texture_source\":\"local_game_installation\","
                     "\"texture_payload_exported\":false,\"materials\":[";
        bool firstLayer=true;
        for (auto layer : layers) {
            if (!firstLayer) materials << ',';
            firstLayer=false;
            std::string_view className="unknown";
            bool alphaTest=false,doubleSided=false;
            switch(layer){
            case 0:className="double_sided";doubleSided=true;break;
            case 1:case 2:className="water";break;
            case 3:className="blend";break;
            case 4:className="blend_to_opaque";break;
            case 5:className="opaque";break;
            case 6:className="light_source";break;
            case 7:className="alpha_test_to_opaque";alphaTest=true;break;
            case 8:className="alpha_test";alphaTest=true;break;
            case 9:className="seasons_opaque";break;
            case 10:className="seasons_alpha_test_to_opaque";alphaTest=true;break;
            case 11:className="alpha_test_single_side";alphaTest=true;break;
            case 12:className="alpha_test_single_side_to_opaque";alphaTest=true;break;
            case 13:className="end_portal";break;
            case 14:className="barrier";break;
            case 15:className="light";break;
            case 16:className="structure_void";break;
            case 17:className="shift_opaque";break;
            case 18:className="shift_alpha_test_to_opaque";alphaTest=true;break;
            case 19:className="shift_alpha_test";alphaTest=true;break;
            case 20:className="shift_alpha_test_single_side";alphaTest=true;break;
            case 21:className="shift_alpha_test_single_side_to_opaque";alphaTest=true;break;
            default:break;
            }
            materials << "{\"id\":\"terrain/layer_" << layer
                      << "\",\"render_layer\":" << layer
                      << ",\"render_class\":" << quoted(std::string(className))
                      << ",\"alpha_test\":" << (alphaTest?"true":"false")
                      << ",\"double_sided\":" << (doubleSided?"true":"false")
                      << ",\"uv_space\":\"terrain_atlas\","
                         "\"atlas_ref\":\"terrain_atlas\","
                         "\"texture_resolution\":\"external\","
                         "\"exact_shader_state\":\"unresolved\"}";
        }
        materials << "],\"atlas_binding\":{"
                        "\"coordinate_space\":\"runtime_atlas_uv\","
                        "\"asset_resolver\":\"local_resource_pack_terrain_texture_json\","
                        "\"face_texture_slot_mapping\":\"geometry_pool.appearance.triangle_texture_slots\","
                         "\"slot_assets\":\"geometry_pool.materials.texture_bindings.atlas_variants\","
                         "\"triangle_variant_lookup\":\"geometry_pool.appearance.triangle_texture_variant_indices\","
                         "\"require_exact_atlas_layout\":false,"
                         "\"remap_via_original_slot_uv_rect\":true,"
                         "\"unresolved_face_policy\":\"report_not_fallback\"}}\n";
        std::ostringstream entities;
        entities << "{\"schema\":1,\"source\":\"native_block_actor_model_parts\",\"model_parts\":[";
        bool firstActor=true;
        for(auto const& actor:data.actorModels){
            try{
                auto hash=sha256(actor.geometryJson);
                (void)contentAddressedFile(data.directory/"geometry_pool"/"models"/(hash+".json"),actor.geometryJson);
                if(!firstActor)entities<<',';
                firstActor=false;
                entities << "{\"name\":"<<quoted(actor.name)<<",\"model_ref\":"<<quoted(hash)<<'}';
            }catch(std::exception const&){++writeFailures;}
        }
        entities << "],\"animations\":[],\"status\":"
                 << (firstActor?"\"unsupported\"":"\"partial\"")
                 << ",\"reason\":\"Native ModelPart/Cube base geometry only; scene transforms, text and animation are not yet captured\"}\n";
        std::ostringstream manifest;
        manifest << "{\"schema\":1,\"format\":\"BedrockSceneCaptureModelLibrary\","
                    "\"target_game_version\":\"1.26.51.1\","
                    "\"levilamina_sdk_version\":\"26.51.9\","
                    "\"resource_pack_fingerprint\":null,"
                    "\"resource_pack_fingerprint_status\":\"not_captured\","
                    "\"textures_exported\":false,"
                    "\"shared_geometry_references\":" << meshRefs
                 << ",\"shared_geometry_pool_schema\":2"
                 << ",\"shared_geometry_count\":" << uniqueExactGeometries.size()
                 << ",\"shared_appearance_count\":" << uniqueAppearances.size()
                 << ",\"shared_material_count\":" << uniqueMaterials.size()
                 << ",\"legacy_meshes_retained\":false"
                 << ",\"static_geometry_models\":" << staticModelIndex.size()
                 << ",\"unresolved_texture_triangles\":" << unmatchedFaces
                 << ",\"sample_contexts\":" << samples
                 << ",\"registered_block_states\":" << data.registered
                 << ",\"loaded_chunks_scanned\":" << data.visitedChunks
                 << ",\"loaded_chunks_skipped\":" << data.skippedChunks
                 << ",\"sampled_blocks\":" << data.sampledBlocks
                 << ",\"failed_samples\":" << data.failedSamples + writeFailures
                 << ",\"registry_only\":true,\"passive_world_observation\":" << (!data.definitionOnly?"true":"false")
                  << ",\"captured_block_actor_models\":" << data.actorModels.size()
                  << ",\"definitions_only\":" << (data.definitionOnly?"true":"false")
                 << ",\"cancelled\":" << (data.cancelled?"true":"false")
                 << ",\"complete\":false}\n";
        // Commit metadata last; an incomplete export is never advertised as complete.
        atomicFile(data.directory/"geometry_pool.json",sharedIndex.str());
        atomicFile(data.directory/"blocks.json",blocks.str());
        atomicFile(data.directory/"geometry_catalog.json",modelIndex.str());
        atomicFile(data.directory/"entities.json",entities.str());
        atomicFile(data.directory/"materials.json",materials.str());
        atomicFile(data.directory/"coverage.json",coverage.str());
        atomicFile(data.directory/"manifest.json",manifest.str());
        // Remove redundant v1 files only after successfully publishing v2.
        {
            auto legacy=data.directory/"meshes";
            std::error_code ec;
            auto status=std::filesystem::symlink_status(legacy,ec);
            if(!ec && std::filesystem::is_directory(status) &&
               !std::filesystem::is_symlink(status))
                std::filesystem::remove_all(legacy,ec);
        }
        return true;
    } catch (std::exception const& e) {
        error=e.what();
        return false;
    }
}
} // namespace bsc
