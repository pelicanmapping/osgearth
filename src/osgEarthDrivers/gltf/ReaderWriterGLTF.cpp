/* osgEarth
* Copyright 2026 Pelican Mapping
* MIT License
*/
#include <osg/Notify>

// cgltf parses and writes; stb_image decodes embedded PNG/JPEG data.
// Both keep their implementation blocks outside their include guards, so the
// implementation macros are cleared right after the first include.
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#undef CGLTF_IMPLEMENTATION

#define CGLTF_WRITE_IMPLEMENTATION
#include <cgltf_write.h>
#undef CGLTF_WRITE_IMPLEMENTATION

// glTF images are PNG or JPEG; leave the other stb decoders out of the
// attack surface for untrusted embedded data.
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#define STBI_NO_GIF
#define STBI_NO_TGA
#define STBI_NO_BMP
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION

#include "GLTFReader.h"
#include "GLTFWriter.h"

#include <osgDB/FileNameUtils>
#include <osgDB/ObjectWrapper>
#include <osgDB/Registry>
#include <cctype>
#include <iterator>
#include <sstream>
using namespace osgEarth;

#undef LC
#define LC "[gltf] "

namespace
{
    // Some tile pipelines deliver zlib-compressed documents. Only attempt
    // inflation when the payload is clearly neither GLB nor JSON.
    bool inflateIfCompressed(std::string& data)
    {
        if (data.size() < 4 || data.compare(0, 4, "glTF") == 0)
            return false;
        for (char c : data)
        {
            if (std::isspace(static_cast<unsigned char>(c))) continue;
            if (c == '{') return false;
            break;
        }
        osg::ref_ptr<osgDB::BaseCompressor> compressor =
            osgDB::Registry::instance()->getObjectWrapperManager()->findCompressor("zlib");
        if (!compressor.valid()) return false;
        std::stringstream in(data);
        std::string out;
        if (!compressor->decompress(in, out)) return false;
        data.swap(out);
        return true;
    }
}

class GLTFReaderWriter : public osgDB::ReaderWriter
{
private:
    mutable GLTFReader::TextureCache _cache;

public:
    GLTFReaderWriter()
    {
        supportsExtension("gltf", "glTF ascii loader");
        supportsExtension("glb", "glTF binary loader");
        supportsOption("gltfZUp", "Content is already Z-up; skip the Y-up to Z-up rotation");
        supportsOption("gltfDefaultSceneOnly", "Load only the default scene");
        supportsOption("gltfParentReversesWinding", "The containing transform mirrors geometry");
        supportsOption("gltfSkipImagery", "Do not load textures");
        supportsOption("gltfSkipPBRTextures", "Load base color textures only");
        supportsOption("gltfSkipNormals", "Do not generate missing normals");
        supportsOption("gltfForceReload", "Bypass the shared material cache");
        supportsOption("gltfDisableExternalAssetInstancing", "Create one ExternalNode per external asset reference");
    }

    const char* className() const override { return "glTF plugin"; }

    ReadResult readObject(const std::string& location, const osgDB::Options* options) const override
    {
        return readNode(location, options);
    }

    ReadResult readNode(const std::string& location, const osgDB::Options* options) const override
    {
        if (!acceptsExtension(osgDB::getLowerCaseFileExtension(location)))
            return ReadResult::FILE_NOT_HANDLED;

        GLTFReader reader;
        reader.setTextureCache(&_cache);
        return reader.read(location, options);
    }

    ReadResult readNode(std::istream& in, const osgDB::Options* options) const override
    {
        std::string buffer((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        inflateIfCompressed(buffer);

        // The referrer identifies the document and resolves its relative URIs.
        GLTFReader reader;
        reader.setTextureCache(&_cache);
        return reader.read(URIContext(options).referrer(), buffer.data(), buffer.size(), options);
    }

    WriteResult writeNode(const osg::Node& node, const std::string& location, const osgDB::Options* options) const override
    {
        const std::string ext = osgDB::getLowerCaseFileExtension(location);
        if (!acceptsExtension(ext))
            return WriteResult::FILE_NOT_HANDLED;
        return GLTFWriter().write(node, location, ext == "glb", options);
    }
};

REGISTER_OSGPLUGIN(gltf, GLTFReaderWriter)
