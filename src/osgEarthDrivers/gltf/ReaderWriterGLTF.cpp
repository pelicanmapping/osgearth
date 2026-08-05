/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <osg/Notify>

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TINYGLTF_NO_EXTERNAL_IMAGE
#define TINYGLTF_NOEXCEPTION // optional. disable exception handling.

#ifdef OSGEARTH_HAVE_DRACO
#define TINYGLTF_ENABLE_DRACO
#endif
#undef TINYGLTF_USE_RAPIDJSON
//#define TINYGLTF_USE_RAPIDJSON
//#define TINYGLTF_USE_RAPIDJSON_CRTALLOCATOR

#include <cmath>
#include "tiny_gltf.h"
using namespace tinygltf;

#include "GLTFReader.h"
#include "GLTFWriter.h"
#include "B3DMReader.h"
#include "B3DMWriter.h"

#include <osgDB/FileNameUtils>
#include <osgDB/Registry>
using namespace osgEarth;

#undef LC
#define LC "[gltf] "

class GLTFReaderWriter : public osgDB::ReaderWriter
{
private:
    mutable GLTFReader::TextureCache _cache;

public:
    GLTFReaderWriter()
    {
        supportsExtension("gltf", "glTF ascii loader");
        supportsExtension("glb", "glTF binary loader");
        supportsExtension("b3dm", "b3dm loader");
    }

    virtual const char* className() const { return "glTF plugin"; }

    ReadResult readObject(const std::string& location, const osgDB::Options* options) const
    {
        return readNode(location, options);
    }

    ReadResult readNode(const std::string& location, const osgDB::Options* options) const
    {
        std::string ext = osgDB::getFileExtension(location);
        if (!acceptsExtension(ext))
            return ReadResult::FILE_NOT_HANDLED;

        if (ext == "gltf")
        {
            OE_NOTICE << "I be reading it" << std::endl;
            GLTFReader reader;
            reader.setTextureCache(&_cache);
            tinygltf::Model model;
            return reader.read(location, false, options);
        }
        else if (ext == "glb")
        {
            OE_NOTICE << "I be reading it" << std::endl;

            GLTFReader reader;
            reader.setTextureCache(&_cache);
            tinygltf::Model model;
            return reader.read(location, true, options);
        }
        else if (ext == "b3dm")
        {
            std::string data = URI(location).getString(options);
            B3DMReader reader;
            reader.setTextureCache(&_cache);
            return reader.read(location, data, options);
        }
        else return ReadResult::FILE_NOT_HANDLED;
    }

    //! Read from a stream:
    ReadResult readNode(std::istream& inputStream, const osgDB::Options* options) const
    {
        // load entire stream into a buffer
        std::istreambuf_iterator<char> eof;
        std::string buffer(std::istreambuf_iterator<char>(inputStream), eof);

        // Find referrer in the options
        URIContext context(options);

        // Determine format by peeking the magic header:
        std::string magic(buffer, 0, 4);

        if (magic == "b3dm")
        {
            B3DMReader reader;
            reader.setTextureCache(&_cache);
            return reader.read(context.referrer(), buffer, options);
        }
        else
        {
            GLTFReader reader;
            reader.setTextureCache(&_cache);
            return reader.read(context.referrer(), buffer, options);
        }

        return ReadResult::FILE_NOT_HANDLED;
    }

    //! Writes a node to GLTF.
    WriteResult writeNode(const osg::Node& node, const std::string& location, const osgDB::Options* options) const
    {
        std::string ext = osgDB::getLowerCaseFileExtension(location);
        if (!acceptsExtension(ext))
            return WriteResult::FILE_NOT_HANDLED;

        if (ext == "gltf")
        {
            GLTFWriter writer;
            return writer.write(node, location, false, options);
        }
        else if (ext == "glb")
        {
            GLTFWriter writer;
            return writer.write(node, location, true, options);
        }
        else if (ext == "b3dm")
        {
            B3DMWriter writer;
            return writer.write(node, location, true, options);
        }

        return WriteResult::ERROR_IN_WRITING_FILE;
    }

};

REGISTER_OSGPLUGIN(gltf, GLTFReaderWriter)
