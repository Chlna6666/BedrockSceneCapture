#include "StaticModels.h"
#include "mc/client/renderer/block/block_geometry/Model.h"
#include "mc/client/renderer/block/block_geometry/Element.h"
#include "mc/client/renderer/block/block_geometry/ElementBox.h"
#include "mc/client/renderer/block/block_geometry/Face.h"
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace bsc {
namespace {
std::string jsonString(std::string const& input) {
    std::ostringstream o;
    constexpr char hex[]="0123456789abcdef";
    o << '"';
    for (unsigned char c: input) {
        switch(c) {
        case '\\': o << "\\\\"; break;
        case '"': o << "\\\""; break;
        case '\n': o << "\\n"; break;
        case '\t': o << "\\t"; break;
        case '\r': o << "\\r"; break;
        default:
            if(c < 0x20) o << "\\u00" << hex[c>>4] << hex[c&15];
            else o << static_cast<char>(c);
        }
    }
    o << '"';
    return o.str();
}
}
std::string serializeStaticGeometry(std::string const& name,BlockGeometry::Model const& model){
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::setprecision(std::numeric_limits<float>::max_digits10);
    o << "{\"schema\":1,\"source\":\"native_registered_block_geometry\","
      << "\"kind\":\"box_elements\",\"name\":" << jsonString(name)
      << ",\"parent\":" << jsonString(model.mParent.get())
      << ",\"texture_map\":{";
    bool first=true;
    for(auto const& [key,value]:model.mTextureMap.get()){
        if(!first)o<<',';
        first=false;
        o<<jsonString(key)<<':'<<jsonString(value);
    }
    o << "},\"texture_list\":[";
    first=true;
    for(auto const& texture:model.mTextureList.get()){
        if(!first)o<<',';
        first=false;
        o<<jsonString(texture);
    }
    o << "],\"texture_stack\":[";
    first=true;
    for(auto const& texture:model.mTextureStack.get()){
        if(!first)o<<',';
        first=false;
        o<<jsonString(texture);
    }
    o << "],\"elements\":[";
    first=true;
    for(auto const& element:model.mElements.get()){
        if(!first)o<<',';
        first=false;
        auto const& pivot=element.mPivot.get();
        auto const& rotation=element.mRotation.get();
        o << "{\"name\":" << jsonString(element.mName.get())
          << ",\"parent\":" << jsonString(element.mParent.get())
          << ",\"pivot\":[" << pivot.x << ',' << pivot.y << ',' << pivot.z << ']'
          << ",\"rotation\":[" << rotation.x << ',' << rotation.y << ',' << rotation.z << ']'
          << ",\"boxes\":[";
        bool firstBox=true;
        for(auto const& box:element.mBoxes.get()){
            if(!firstBox)o<<',';
            firstBox=false;
            auto const& origin=box.mOrigin.get();
            auto const& size=box.mSize.get();
            o << "{\"origin\":[" << origin.x << ',' << origin.y << ',' << origin.z << ']'
              << ",\"size\":[" << size.x << ',' << size.y << ',' << size.z << ']'
              << ",\"faces\":[";
            bool firstFace=true;
            std::size_t faceIndex=0;
            for(auto const& face:box.mFaces.get()){
                if(!firstFace)o<<',';
                firstFace=false;
                o << "{\"face_index\":" << faceIndex++
                  << ",\"enabled\":" << (face.enabled?"true":"false")
                  << ",\"uv\":[" << face.u0 << ',' << face.v0
                  << ',' << face.u1 << ',' << face.v1 << ']'
                  << ",\"rotation\":" << face.rotation
                  << ",\"texture\":" << jsonString(face.texture.get())
                  << ",\"texture_index\":" << face.textureIndex << '}';
            }
            o << "]}";
        }
        o << "]}";
    }
    o << "]}\n";
    return o.str();
}
} // namespace bsc
