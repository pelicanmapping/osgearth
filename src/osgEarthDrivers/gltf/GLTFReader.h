/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#ifndef OSGEARTH_GLTF_READER_H
#define OSGEARTH_GLTF_READER_H

#include <osg/Node>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/observer_ptr>
#include <osg/Texture2D>
#include <osg/CullFace>
#include <osg/FrontFace>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/ObjectWrapper>
#include <osgDB/Registry>
#include <osgUtil/SmoothingVisitor>
#include <osgEarth/Notify>
#include <osgEarth/NodeUtils>
#include <osgEarth/URI>
#include <osgEarth/Containers>
#include <osgEarth/Registry>
#include <osgEarth/ShaderUtils>
#include <osgEarth/ShaderGenerator>
#include <osgEarth/ShaderLoader>
#include <osgEarth/VirtualProgram>
#include <osgEarth/StringUtils>
#include <osgEarth/InstanceBuilder>
#include <osgEarth/StateTransition>
#include <osgEarth/JsonUtils>
#include <osgEarth/BuildConfig>
#include <osgEarth/ExternalNode>
#include <osgEarth/InstancedExternalNode>
#include <osgEarth/VertexCompression>
#include <osgEarth/Chonk>
#ifdef OSGEARTH_HAVE_MESH_OPTIMIZER
#include <meshoptimizer.h>
#endif
#include <limits>
#include <cctype>
#include <map>
#include <sstream>
#include <utility>

using namespace osgEarth;
using namespace osgEarth::Util;

#undef LC
#define LC "[GLTFWriter] "

class GLTFReader
{
public:
    using TextureCache = osgEarth::Threading::Mutexed<
        std::unordered_map<std::string, osg::observer_ptr<osg::Texture2D>> >;

    struct NodeBuilder;

    static const char* meshoptFallbackURI()
    {
        return "__osgearth_meshopt_fallback__.bin";
    }

    static bool isMeshoptFallbackURI(const std::string& uri)
    {
        return uri.find(meshoptFallbackURI()) != std::string::npos;
    }

    static const char* externalAssetExtension()
    {
        // TinyGLTF does not yet expose the glTF 2.1 core properties. The raw
        // JSON preparation step below carries them through its existing
        // extension Value mechanism under this private implementation key.
        return "OE_external_asset";
    }

    static bool hasOption(
        const osgDB::Options* options,
        const std::string& option)
    {
        if (!options)
            return false;

        const std::string& value = options->getOptionString();
        std::string::size_type pos = 0;
        while ((pos = value.find(option, pos)) != std::string::npos)
        {
            const bool startsToken =
                pos == 0 || std::isspace(static_cast<unsigned char>(value[pos - 1]));
            const std::string::size_type end = pos + option.size();
            const bool endsToken =
                end == value.size() ||
                std::isspace(static_cast<unsigned char>(value[end]));
            if (startsToken && endsToken)
                return true;
            pos = end;
        }
        return false;
    }

    static void appendOption(
        osgDB::Options* options,
        const std::string& option)
    {
        if (!options || hasOption(options, option))
            return;

        std::string value = options->getOptionString();
        if (!value.empty() &&
            !std::isspace(static_cast<unsigned char>(value.back())))
        {
            value += ' ';
        }
        value += option;
        options->setOptionString(value);
    }

    static void removeOption(
        osgDB::Options* options,
        const std::string& option)
    {
        if (!options)
            return;

        std::istringstream input(options->getOptionString());
        std::ostringstream output;
        std::string token;
        bool first = true;
        while (input >> token)
        {
            if (token == option)
                continue;
            if (!first)
                output << ' ';
            output << token;
            first = false;
        }
        options->setOptionString(output.str());
    }

    static std::string resolveResourceURI(
        const std::string& reference,
        const std::string& referrer)
    {
        const URIContext context(referrer);
        URI resolved(reference, context);

        // URI escaping belongs to the glTF document, whereas osgDB expects a
        // native filename for local reads. First resolve the encoded spelling
        // only to determine whether it is remote. For a local resource, decode
        // the reference exactly once and then combine it with the unchanged
        // referrer. This preserves a real parent directory containing "%20"
        // while still mapping a glTF "%20" escape to a filesystem space.
        // Remote URLs (including escaped paths and queries) remain intact.
        if (resolved.isRemote())
            return resolved.full();

        return URI(
            URI::decodePathEscapes(reference), context).full();
    }

    static std::string ExpandFilePath(const std::string &filepath, void * userData)
    {
        if (isMeshoptFallbackURI(filepath))
            return filepath;

        const std::string& referrer = *(const std::string*)userData;
        std::string path = resolveResourceURI(filepath, referrer);
        OSG_NOTICE << "ExpandFilePath: expanded " << filepath << " to " << path << std::endl;
        return path;
    }

    static bool ReadWholeFile(std::vector<unsigned char> *out, std::string *err,
        const std::string &filepath, void *)
    {
        if (isMeshoptFallbackURI(filepath))
        {
            out->assign(1, 0u);
            return true;
        }

        auto result = URI(filepath).readString();
        if (result.failed())
        {
            return false;
        }

        std::string str = result.getString();
        out->resize(str.size());
        memcpy(out->data(), str.c_str(), str.size());
        return true;
    }

    static bool LoadImageData(tinygltf::Image* image, const int imageIndex,
        std::string* err, std::string* warn, int requestedWidth,
        int requestedHeight, const unsigned char* bytes, int size, void* userData)
    {
        // stb_image does not support WebP. Keep embedded WebP data encoded so
        // makeTextureFromModel can pass it through osgEarth's WebP plugin.
        const bool isWebP =
            image->mimeType == "image/webp" ||
            (size >= 12 &&
             memcmp(bytes, "RIFF", 4) == 0 &&
             memcmp(bytes + 8, "WEBP", 4) == 0);

        if (isWebP)
        {
            image->mimeType = "image/webp";
            image->image.assign(bytes, bytes + size);
            image->as_is = true;
            return true;
        }

        return tinygltf::LoadImageData(image, imageIndex, err, warn,
            requestedWidth, requestedHeight, bytes, size, userData);
    }

    static bool FileExists(const std::string &abs_filename, void *)
    {
        if (isMeshoptFallbackURI(abs_filename))
            return true;

        if (osgDB::containsServerAddress(abs_filename))
        {
            return true;
        }
        return osgDB::fileExists(abs_filename);
    }

    static uint32_t readUInt32LE(const char* ptr)
    {
        const unsigned char* bytes =
            reinterpret_cast<const unsigned char*>(ptr);
        return static_cast<uint32_t>(bytes[0]) |
            (static_cast<uint32_t>(bytes[1]) << 8u) |
            (static_cast<uint32_t>(bytes[2]) << 16u) |
            (static_cast<uint32_t>(bytes[3]) << 24u);
    }

    static void writeUInt32LE(std::string& data, size_t offset, uint32_t value)
    {
        data[offset + 0] = static_cast<char>(value & 0xffu);
        data[offset + 1] = static_cast<char>((value >> 8u) & 0xffu);
        data[offset + 2] = static_cast<char>((value >> 16u) & 0xffu);
        data[offset + 3] = static_cast<char>((value >> 24u) & 0xffu);
    }

    static bool prepareMeshoptFallbackBuffers(
        std::string& data, bool binary, std::string& err)
    {
        static const uint32_t GLB_MAGIC = 0x46546c67u;
        static const uint32_t GLB_JSON_CHUNK = 0x4e4f534au;

        std::string jsonText;
        size_t jsonDataEnd = 0;
        uint32_t glbLength = 0;

        if (binary)
        {
            if (data.size() < 20 || readUInt32LE(data.data()) != GLB_MAGIC)
            {
                err += "Invalid GLB header while preparing meshopt buffers.\n";
                return false;
            }

            glbLength = readUInt32LE(data.data() + 8);
            const uint32_t jsonLength = readUInt32LE(data.data() + 12);
            const uint32_t chunkType = readUInt32LE(data.data() + 16);
            jsonDataEnd = 20u + static_cast<size_t>(jsonLength);
            if (glbLength > data.size() || jsonDataEnd > glbLength ||
                chunkType != GLB_JSON_CHUNK)
            {
                err += "Invalid GLB JSON chunk while preparing meshopt buffers.\n";
                return false;
            }

            jsonText.assign(data.data() + 20, jsonLength);
            while (!jsonText.empty() && jsonText.back() == '\0')
                jsonText.pop_back();
        }
        else
        {
            jsonText = data;
        }

        const bool hasMeshopt =
            jsonText.find("EXT_meshopt_compression") != std::string::npos;
        const bool mayHaveExternalAssets =
            jsonText.find("\"externalAssets\"") != std::string::npos ||
            jsonText.find("\"externalAsset\"") != std::string::npos;

        if (!hasMeshopt && !mayHaveExternalAssets)
            return true;

        osgEarth::Util::Json::Reader jsonReader;
        osgEarth::Util::Json::Value root;
        if (!jsonReader.parse(jsonText, root, false))
        {
            err += "Failed to parse glTF JSON while preparing reader input:\n";
            err += jsonReader.getFormatedErrorMessages();
            return false;
        }

        bool changed = false;

        osgEarth::Util::Json::Value& nodes = root["nodes"];
        bool hasExternalReferences = false;
        if (nodes.isArray())
        {
            for (unsigned int i = 0u; i < nodes.size(); ++i)
            {
                if (nodes[i].isObject() && nodes[i].isMember("externalAsset"))
                {
                    hasExternalReferences = true;
                    break;
                }
            }
        }

        if (hasExternalReferences)
        {
            const osgEarth::Util::Json::Value& files = root["files"];
            const osgEarth::Util::Json::Value& externalAssets =
                root["externalAssets"];

            if (!files.isArray() || !externalAssets.isArray() ||
                !nodes.isArray())
            {
                err += "glTF external assets require files, externalAssets, "
                    "and nodes arrays.\n";
                return false;
            }

            auto readIndex = [](
                const osgEarth::Util::Json::Value& value,
                unsigned int& index) -> bool
            {
                if (value.isUInt())
                {
                    index = value.asUInt();
                    return true;
                }
                if (value.isInt() && value.asInt() >= 0)
                {
                    index = static_cast<unsigned int>(value.asInt());
                    return true;
                }
                return false;
            };

            for (unsigned int nodeIndex = 0u;
                 nodeIndex < nodes.size(); ++nodeIndex)
            {
                osgEarth::Util::Json::Value& node = nodes[nodeIndex];
                const osgEarth::Util::Json::Value& constNode = node;
                if (!constNode.isMember("externalAsset"))
                    continue;
                const osgEarth::Util::Json::Value& externalAssetValue =
                    constNode["externalAsset"];

                unsigned int externalAssetIndex = 0u;
                if (!readIndex(externalAssetValue, externalAssetIndex) ||
                    externalAssetIndex >= externalAssets.size())
                {
                    err += Stringify() << "node[" << nodeIndex <<
                        "].externalAsset is out of range.\n";
                    return false;
                }

                if (constNode.isMember("mesh"))
                {
                    err += Stringify() << "node[" << nodeIndex <<
                        "] cannot contain both mesh and externalAsset.\n";
                    return false;
                }

                const osgEarth::Util::Json::Value& externalAsset =
                    externalAssets[externalAssetIndex];
                unsigned int fileIndex = 0u;
                if (!externalAsset.isObject() ||
                    !readIndex(externalAsset["file"], fileIndex) ||
                    fileIndex >= files.size())
                {
                    err += Stringify() << "externalAssets[" <<
                        externalAssetIndex << "].file is out of range.\n";
                    return false;
                }

                const osgEarth::Util::Json::Value& file = files[fileIndex];
                if (!file.isObject())
                {
                    err += Stringify() << "files[" << fileIndex <<
                        "] must be an object.\n";
                    return false;
                }

                const bool uriDefined = file.isMember("uri");
                const bool bufferViewDefined = file.isMember("bufferView");
                if (uriDefined == bufferViewDefined)
                {
                    err += Stringify() << "files[" << fileIndex <<
                        "] must contain exactly one of uri or bufferView.\n";
                    return false;
                }

                const osgEarth::Util::Json::Value& uriValue = file["uri"];
                const osgEarth::Util::Json::Value& bufferViewValue =
                    file["bufferView"];
                unsigned int unusedBufferView = 0u;
                if (uriDefined && !uriValue.isString())
                {
                    err += Stringify() << "files[" << fileIndex <<
                        "].uri must be a string.\n";
                    return false;
                }
                if (bufferViewDefined &&
                    !readIndex(bufferViewValue, unusedBufferView))
                {
                    err += Stringify() << "files[" << fileIndex <<
                        "].bufferView must be a non-negative index.\n";
                    return false;
                }

                const osgEarth::Util::Json::Value& mimeTypeValue =
                    file["mimeType"];
                if (!mimeTypeValue.isString() ||
                    (mimeTypeValue.asString() != "model/gltf+json" &&
                     mimeTypeValue.asString() != "model/gltf-binary"))
                {
                    err += Stringify() << "files[" << fileIndex <<
                        "] referenced as an external asset must use a glTF "
                        "MIME type.\n";
                    return false;
                }

                if (file.isMember("aliases") &&
                    (!file["aliases"].isArray() ||
                     file["aliases"].size() > 0u))
                {
                    err += Stringify() << "External asset file aliases in "
                        "files[" << fileIndex << "] are not supported.\n";
                    return false;
                }

                if (bufferViewDefined)
                {
                    err += Stringify() << "Embedded bufferView external asset "
                        "files[" << fileIndex << "] is not supported.\n";
                    return false;
                }

                const std::string uri = uriValue.asString();
                const bool isDataURI =
                    uri.size() >= 5u &&
                    std::tolower(static_cast<unsigned char>(uri[0])) == 'd' &&
                    std::tolower(static_cast<unsigned char>(uri[1])) == 'a' &&
                    std::tolower(static_cast<unsigned char>(uri[2])) == 't' &&
                    std::tolower(static_cast<unsigned char>(uri[3])) == 'a' &&
                    uri[4] == ':';
                if (isDataURI)
                {
                    err += Stringify() << "Embedded data URI external asset "
                        "files[" << fileIndex << "] is not supported.\n";
                    return false;
                }

                osgEarth::Util::Json::Value carried;
                carried["uri"] = uri;
                carried["mimeType"] = mimeTypeValue.asString();
                if (externalAsset["name"].isString())
                    carried["name"] = externalAsset["name"].asString();
                node["extensions"][externalAssetExtension()] = carried;
                changed = true;
            }
        }

        bool meshoptRequired = false;
        const osgEarth::Util::Json::Value& required =
            root["extensionsRequired"];
        if (hasMeshopt && required.isArray())
        {
            for (unsigned int i = 0; i < required.size(); ++i)
            {
                if (required[i].isString() &&
                    required[i].asString() == "EXT_meshopt_compression")
                {
                    meshoptRequired = true;
                    break;
                }
            }
        }

#ifndef OSGEARTH_HAVE_MESH_OPTIMIZER
        if (hasMeshopt && meshoptRequired)
        {
            err += "EXT_meshopt_compression is required, but osgEarth was "
                "built without meshoptimizer support.\n";
            return false;
        }

        // An optional meshopt extension retains its ordinary uncompressed
        // fallback data. External-asset injection, if any, still continues.
#else
        osgEarth::Util::Json::Value& buffers = root["buffers"];
        if (hasMeshopt && buffers.isArray())
        {
            for (unsigned int i = 0; i < buffers.size(); ++i)
            {
                osgEarth::Util::Json::Value& buffer = buffers[i];
                const osgEarth::Util::Json::Value& constBuffer = buffer;
                const osgEarth::Util::Json::Value& meshopt =
                    constBuffer["extensions"]["EXT_meshopt_compression"];
                const bool taggedFallback =
                    meshopt.isObject() && meshopt["fallback"].isBool() &&
                    meshopt["fallback"].asBool();
                const bool hasURI =
                    constBuffer["uri"].isString() &&
                    !constBuffer["uri"].asString().empty();
                const bool uriLessFallback =
                    meshoptRequired && !hasURI && (!binary || i > 0u);

                if (taggedFallback || uriLessFallback)
                {
                    // TinyGLTF insists on loading every buffer before extension
                    // processing. Supply a minimal placeholder; all referencing
                    // bufferViews are replaced with decoded storage below.
                    buffer["byteLength"] = 1u;
                    buffer["uri"] = meshoptFallbackURI();
                    changed = true;
                }
            }
        }
#endif

        if (!changed)
            return true;

        osgEarth::Util::Json::FastWriter jsonWriter;
        std::string preparedJSON = jsonWriter.write(root);
        if (!binary)
        {
            data.swap(preparedJSON);
            return true;
        }

        while ((preparedJSON.size() & 3u) != 0u)
            preparedJSON.push_back(' ');

        const size_t remainingSize = glbLength - jsonDataEnd;
        const size_t rebuiltSize = 20u + preparedJSON.size() + remainingSize;
        if (preparedJSON.size() > std::numeric_limits<uint32_t>::max() ||
            rebuiltSize > std::numeric_limits<uint32_t>::max())
        {
            err += "GLB is too large after preparing meshopt buffers.\n";
            return false;
        }

        std::string rebuilt;
        rebuilt.reserve(rebuiltSize);
        rebuilt.append(data.data(), 12);
        rebuilt.resize(20);
        writeUInt32LE(rebuilt, 12, static_cast<uint32_t>(preparedJSON.size()));
        writeUInt32LE(rebuilt, 16, GLB_JSON_CHUNK);
        rebuilt.append(preparedJSON);
        rebuilt.append(data.data() + jsonDataEnd, remainingSize);
        writeUInt32LE(rebuilt, 8, static_cast<uint32_t>(rebuilt.size()));
        data.swap(rebuilt);
        return true;
    }

    struct Env
    {
        Env(const std::string& loc, const osgDB::Options* opt) : referrer(loc), readOptions(opt) { }
        const std::string referrer;
        const osgDB::Options* readOptions;
    };

public:
    mutable TextureCache* _texCache;

    GLTFReader() : _texCache(NULL)
    {
        //NOP
    }

    void setTextureCache(TextureCache* cache) const
    {
        _texCache = cache;
    }

    osgDB::ReaderWriter::ReadResult read(const std::string& location,
                                         bool isBinary,
                                         const osgDB::Options* readOptions) const
    {
        std::string err, warn;
        tinygltf::Model model;
        tinygltf::TinyGLTF loader;

        tinygltf::FsCallbacks fs;
        fs.FileExists = &GLTFReader::FileExists;
        fs.ExpandFilePath = &GLTFReader::ExpandFilePath;
        fs.ReadWholeFile = &GLTFReader::ReadWholeFile;
        fs.WriteWholeFile = &tinygltf::WriteWholeFile;
        fs.user_data = (void*)&location;
        loader.SetFsCallbacks(fs);
        loader.SetImageLoader(&GLTFReader::LoadImageData, nullptr);

        tinygltf::Options opt;
        opt.skip_imagery = readOptions && readOptions->getOptionString().find("gltfSkipImagery") != std::string::npos;

        osgEarth::ReadResult rr = osgEarth::URI(location).readString(readOptions);
        if (rr.failed())
        {
            return osgDB::ReaderWriter::ReadResult::FILE_NOT_FOUND;
        }

        std::string mem = rr.getString();
        if (!prepareMeshoptFallbackBuffers(mem, isBinary, err))
        {
            OE_WARN << LC << "gltf Error loading " << location << std::endl;
            OE_WARN << LC << err << std::endl;
            return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
        }

        const std::string baseDir = osgDB::getFilePath(location);
        bool loaded = isBinary ?
            loader.LoadBinaryFromMemory(
                &model, &err, &warn,
                reinterpret_cast<const unsigned char*>(mem.data()), mem.size(),
                baseDir, REQUIRE_VERSION, &opt) :
            loader.LoadASCIIFromString(
                &model, &err, &warn, mem.data(), mem.size(),
                baseDir, REQUIRE_VERSION, &opt);

        if (!loaded || !err.empty()) {
            OE_WARN << LC << "gltf Error loading " << location << std::endl;
            OE_WARN << LC << err << std::endl;
            return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
        }

        if (!decodeMeshoptCompression(model, err))
        {
            OE_WARN << LC << "gltf Error loading " << location << std::endl;
            OE_WARN << LC << err << std::endl;
            return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
        }

        Env env(location, readOptions);
        osg::Node* result = makeNodeFromModel(model, env);
        return result ? osgDB::ReaderWriter::ReadResult(result) :
            osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
    }

    osg::Node* read(const std::string& location, const std::string& inputStream, const osgDB::Options* readOptions) const
    {
        std::string err, warn;
        tinygltf::Model model;
        tinygltf::TinyGLTF loader;

        tinygltf::FsCallbacks fs;
        fs.FileExists = &GLTFReader::FileExists;
        fs.ExpandFilePath = &GLTFReader::ExpandFilePath;
        fs.ReadWholeFile = &GLTFReader::ReadWholeFile;
        fs.WriteWholeFile = &tinygltf::WriteWholeFile;
        fs.user_data = (void*)&location;
        loader.SetFsCallbacks(fs);
        loader.SetImageLoader(&GLTFReader::LoadImageData, nullptr);

        tinygltf::Options opt;
        opt.skip_imagery = readOptions && readOptions->getOptionString().find("gltfSkipImagery") != std::string::npos;

        std::string decompressedData;
        const std::string* data = &inputStream;

        osg::ref_ptr<osgDB::BaseCompressor> compressor = osgDB::Registry::instance()->getObjectWrapperManager()->findCompressor("zlib");
        if (compressor.valid())
        {
            std::stringstream in_data(inputStream);
            if (compressor->decompress(in_data, decompressedData))
            {
                data = &decompressedData;
            }
        }

        std::string preparedData = *data;
        const bool binary = preparedData.compare(0, 4, "glTF") == 0;
        if (!prepareMeshoptFallbackBuffers(preparedData, binary, err))
        {
            OE_WARN << LC << "gltf Error loading " << location << std::endl;
            OE_WARN << LC << err << std::endl;
            return 0;
        }

        bool loaded = binary ?
            loader.LoadBinaryFromMemory(
                &model, &err, &warn,
                reinterpret_cast<const unsigned char*>(preparedData.data()),
                preparedData.size(), "", REQUIRE_VERSION, &opt) :
            loader.LoadASCIIFromString(
                &model, &err, &warn, preparedData.data(), preparedData.size(),
                "", REQUIRE_VERSION, &opt);

        if (!loaded || !err.empty()) {
            OE_WARN << LC << "gltf Error loading " << location << std::endl;
            OE_WARN << LC << err << std::endl;
            return 0;
        }

        if (!decodeMeshoptCompression(model, err))
        {
            OE_WARN << LC << "gltf Error loading " << location << std::endl;
            OE_WARN << LC << err << std::endl;
            return 0;
        }

        Env env(location, readOptions);
        return makeNodeFromModel(model, env);
    }

    static bool decodeMeshoptCompression(tinygltf::Model& model, std::string& err)
    {
#ifndef OSGEARTH_HAVE_MESH_OPTIMIZER
        for (const auto& extension : model.extensionsRequired)
        {
            if (extension == "EXT_meshopt_compression")
            {
                err += "EXT_meshopt_compression is required, but osgEarth was "
                    "built without meshoptimizer support.\n";
                return false;
            }
        }
        return true;
#else
        const char* extensionName = "EXT_meshopt_compression";

        for (size_t viewIndex = 0; viewIndex < model.bufferViews.size(); ++viewIndex)
        {
            tinygltf::BufferView& bufferView = model.bufferViews[viewIndex];
            auto extensionIt = bufferView.extensions.find(extensionName);
            if (extensionIt == bufferView.extensions.end())
                continue;

            const tinygltf::Value& extension = extensionIt->second;
            auto fail = [&](const std::string& message)
            {
                err += std::string(extensionName) + " bufferView[" +
                    std::to_string(viewIndex) + "]: " + message + "\n";
                return false;
            };

            if (!extension.IsObject())
                return fail("extension value is not an object");

            auto readSize = [&](const char* name, size_t& value, bool required, size_t defaultValue = 0)
            {
                const tinygltf::Value& property = extension.Get(name);
                if (property.Type() == tinygltf::NULL_TYPE)
                {
                    value = defaultValue;
                    return !required;
                }
                if (!property.IsInt() || property.Get<int>() < 0)
                    return false;
                value = static_cast<size_t>(property.Get<int>());
                return true;
            };

            size_t sourceBufferIndex = 0;
            size_t sourceOffset = 0;
            size_t sourceLength = 0;
            size_t stride = 0;
            size_t count = 0;
            if (!readSize("buffer", sourceBufferIndex, true) ||
                !readSize("byteOffset", sourceOffset, false) ||
                !readSize("byteLength", sourceLength, true) ||
                !readSize("byteStride", stride, true) ||
                !readSize("count", count, true))
            {
                return fail("missing or invalid integer property");
            }

            const tinygltf::Value& modeValue = extension.Get("mode");
            if (!modeValue.IsString())
                return fail("missing or invalid mode");
            const std::string& mode = modeValue.Get<std::string>();

            std::string filter = "NONE";
            const tinygltf::Value& filterValue = extension.Get("filter");
            if (filterValue.Type() != tinygltf::NULL_TYPE)
            {
                if (!filterValue.IsString())
                    return fail("invalid filter");
                filter = filterValue.Get<std::string>();
            }

            if (count == 0 || stride == 0 || sourceLength == 0)
                return fail("count, byteStride, and byteLength must be nonzero");
            if (count > std::numeric_limits<size_t>::max() / stride)
                return fail("decoded byte length overflows size_t");

            const size_t decodedLength = count * stride;
            if (decodedLength != bufferView.byteLength)
                return fail("decoded byte length does not match the parent bufferView");
            if (bufferView.byteStride != 0 && bufferView.byteStride != stride)
                return fail("byteStride does not match the parent bufferView");

            if (mode == "ATTRIBUTES")
            {
                if ((stride % 4) != 0 || stride > 256)
                    return fail("ATTRIBUTES byteStride must be a multiple of 4 and at most 256");
            }
            else if (mode == "TRIANGLES")
            {
                if ((count % 3) != 0 || (stride != 2 && stride != 4))
                    return fail("TRIANGLES requires a count divisible by 3 and a byteStride of 2 or 4");
            }
            else if (mode == "INDICES")
            {
                if (stride != 2 && stride != 4)
                    return fail("INDICES byteStride must be 2 or 4");
            }
            else
            {
                return fail("unsupported mode " + mode);
            }

            if (filter == "OCTAHEDRAL")
            {
                if (mode != "ATTRIBUTES" || (stride != 4 && stride != 8))
                    return fail("OCTAHEDRAL filter requires ATTRIBUTES mode and a byteStride of 4 or 8");
            }
            else if (filter == "QUATERNION")
            {
                if (mode != "ATTRIBUTES" || stride != 8)
                    return fail("QUATERNION filter requires ATTRIBUTES mode and a byteStride of 8");
            }
            else if (filter == "EXPONENTIAL")
            {
                if (mode != "ATTRIBUTES" || (stride % 4) != 0)
                    return fail("EXPONENTIAL filter requires ATTRIBUTES mode and a byteStride divisible by 4");
            }
            else if (filter != "NONE")
            {
                return fail("unsupported filter " + filter);
            }

            if (sourceBufferIndex >= model.buffers.size())
                return fail("compressed buffer index is out of range");
            const std::vector<unsigned char>& source = model.buffers[sourceBufferIndex].data;
            if (sourceOffset > source.size() || sourceLength > source.size() - sourceOffset)
                return fail("compressed byte range is out of bounds");

            std::vector<unsigned char> decoded(decodedLength);
            const unsigned char* compressed = source.data() + sourceOffset;
            int result = -1;
            if (mode == "ATTRIBUTES")
                result = meshopt_decodeVertexBuffer(decoded.data(), count, stride, compressed, sourceLength);
            else if (mode == "TRIANGLES")
                result = meshopt_decodeIndexBuffer(decoded.data(), count, stride, compressed, sourceLength);
            else
                result = meshopt_decodeIndexSequence(decoded.data(), count, stride, compressed, sourceLength);

            if (result != 0)
                return fail("meshoptimizer failed to decode the compressed data");

            if (filter == "OCTAHEDRAL")
                meshopt_decodeFilterOct(decoded.data(), count, stride);
            else if (filter == "QUATERNION")
                meshopt_decodeFilterQuat(decoded.data(), count, stride);
            else if (filter == "EXPONENTIAL")
                meshopt_decodeFilterExp(decoded.data(), count, stride);

            tinygltf::Buffer decodedBuffer;
            decodedBuffer.name = bufferView.name + " (meshopt decoded)";
            decodedBuffer.data.swap(decoded);

            bufferView.buffer = static_cast<int>(model.buffers.size());
            bufferView.byteOffset = 0;
            bufferView.byteLength = decodedLength;
            bufferView.byteStride = stride;
            bufferView.extensions.erase(extensionIt);
            model.buffers.emplace_back(std::move(decodedBuffer));
        }

        for (size_t bufferIndex = 0; bufferIndex < model.buffers.size(); ++bufferIndex)
        {
            tinygltf::Buffer& buffer = model.buffers[bufferIndex];
            if (!isMeshoptFallbackURI(buffer.uri))
                continue;

            for (size_t viewIndex = 0; viewIndex < model.bufferViews.size(); ++viewIndex)
            {
                if (model.bufferViews[viewIndex].buffer ==
                    static_cast<int>(bufferIndex))
                {
                    err += std::string(extensionName) + " fallback buffer[" +
                        std::to_string(bufferIndex) +
                        "] is referenced by undecoded bufferView[" +
                        std::to_string(viewIndex) + "]\n";
                    return false;
                }
            }

            buffer.data.clear();
            buffer.uri.clear();
        }

        return true;
#endif
    }


    /**
     * Node to support the OWT_State extension.
     */
    class StateTransitionNode : public osg::Group, public StateTransition
    {
    public:

        virtual std::vector< std::string > getStates()
        {
            std::vector< std::string > states;
            for (auto& s : _stateToNode)
            {
                states.push_back(s.first);
            }
            return states;
        }

        virtual void transitionToState(const std::string& state)
        {
            auto itr = _stateToNode.find(state);
            if (itr != _stateToNode.end())
            {
                osg::ref_ptr< osg::Node > node;
                itr->second.lock(node);
                if (node.valid())
                {
                    // Turn the destination node on.
                    node->setNodeMask(~0);

                    // Turn this node off
                    setNodeMask(0);
                }
            }
        }

        typedef std::map< std::string, osg::observer_ptr< osg::Node > > StateToNodeMap;
        typedef std::map< std::string, std::string > StateToNodeName;

        StateToNodeMap _stateToNode;
        StateToNodeName _stateToNodeName;
    };

    osg::Node* makeNodeFromModel(const tinygltf::Model &model, const Env& env) const
    {
        NodeBuilder builder(this, model, env);
        bool zUp = env.readOptions && env.readOptions->getOptionString().find("gltfZUp") != std::string::npos;

        // Rotate y-up to z-up if necessary
        osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform;
        if (!zUp)
        {
            transform->setMatrix(osg::Matrixd::rotate(osg::Vec3d(0.0, 1.0, 0.0), osg::Vec3d(0.0, 0.0, 1.0)));
        }

        std::vector<int> sceneIndices;
        const bool parentReversesWinding =
            hasOption(env.readOptions, "gltfParentReversesWinding");
        if (hasOption(env.readOptions, "gltfDefaultSceneOnly"))
        {
            if (model.defaultScene >= 0 &&
                static_cast<size_t>(model.defaultScene) < model.scenes.size())
            {
                sceneIndices.push_back(model.defaultScene);
            }
        }
        else
        {
            sceneIndices.reserve(model.scenes.size());
            for (unsigned int i = 0u; i < model.scenes.size(); ++i)
                sceneIndices.push_back(static_cast<int>(i));
        }

        // Plan batches from glTF data before allocating any per-reference OSG
        // nodes. The outer Y-up-to-Z-up transform remains common to all batches.
        builder.prepareExternalAssetInstancing(sceneIndices, parentReversesWinding);

        for (int sceneIndex : sceneIndices)
        {
            const tinygltf::Scene& scene = model.scenes[sceneIndex];

            for (size_t j = 0; j < scene.nodes.size(); j++)
            {
                const int nodeIndex = scene.nodes[j];
                if (nodeIndex < 0 ||
                    static_cast<size_t>(nodeIndex) >= model.nodes.size())
                {
                    continue;
                }

                osg::Node* node =
                    builder.createNode(
                        model.nodes[nodeIndex], parentReversesWinding);
                if (node)
                {
                    transform->addChild(node);
                }
            }
        }

        for (const auto& entry : builder.externalBatches)
        {
            if (entry.second.isInstanced())
                transform->addChild(entry.second.node.get());
        }

        // Enable backface culling on the nodes
        transform->getOrCreateStateSet()->setAttributeAndModes(new osg::CullFace(osg::CullFace::BACK), osg::StateAttribute::ON);

        // Find all the StateTransitionNodes that were created and try to establishs links between the nodes.
        osgEarth::FindNodesVisitor<StateTransitionNode> findStateTransitions;
        if (!builder.error.empty())
        {
            OE_WARN << LC << builder.error << std::endl;
            return nullptr;
        }

        transform->accept(findStateTransitions);

        for (auto& st : findStateTransitions._results)
        {
            for (auto& stateToNodeName : st->_stateToNodeName)
            {
                std::string state = stateToNodeName.first;
                std::string name = stateToNodeName.second;

                // Find the named node
                osg::Node* node = findNamedNode(transform.get(), name);
                if (node)
                {
                    st->_stateToNode[state] = node;
                }
                else
                {
                    OE_WARN << LC << "Failed to find transition state node " << state << "=" << name << std::endl;
                }
            }
        }

        return transform.release();
    }


    struct NodeBuilder
    {
        const GLTFReader* reader;
        const tinygltf::Model &model;
        const Env& env;
        std::vector< osg::ref_ptr< osg::Array > > arrays;
        mutable std::string error;

        struct ExternalReference;

        struct ExternalInstanceBatch
        {
            const ExternalReference* reference = nullptr;
            osgEarth::InstancedExternalNode::MatrixList matrices;
            osg::ref_ptr<osgEarth::InstancedExternalNode> node;

            bool isInstanced() const
            {
                return node.valid() && node->isUsingHardwareInstancing();
            }
        };

        struct ExternalReference
        {
            std::string filename;
            std::string externalName;
            osg::ref_ptr<osgDB::Options> options;
            ExternalInstanceBatch* batch = nullptr;
        };

        // std::map keeps reference/batch addresses stable while collecting.
        mutable std::map<std::pair<std::size_t, bool>, ExternalReference> externalReferences;
        std::map<std::string, ExternalInstanceBatch> externalBatches;
        bool externalAssetInstancing = true;

        // Texture image units for the original glTF material maps.
        enum : unsigned
        {
            ALBEDO_UNIT = 0u,
            NORMAL_UNIT = 1u,
            METALLIC_ROUGHNESS_UNIT = 2u,
            OCCLUSION_UNIT = 3u,
            HEIGHT_UNIT = 4u
        };

        // Whether to load the normal, metallic-roughness, occlusion and height maps
        // in addition to the base color. Disable with the "gltfSkipPBRTextures"
        // read option.
        bool loadPBRTextures = true;

        // Textures already created for this model, so that primitives that
        // share a material also share the same osg::Texture2D objects.
        mutable std::unordered_map<std::string, osg::ref_ptr<osg::Texture2D>> localTextures;

        NodeBuilder(const GLTFReader* reader_, const tinygltf::Model &model_, const Env& env_)
            : reader(reader_), model(model_), env(env_)
        {
            loadPBRTextures =
                !env.readOptions ||
                env.readOptions->getOptionString().find("gltfSkipPBRTextures") == std::string::npos;

            // Static batching is the default. Successful batches create one
            // root-level matrix list. Callers needing per-reference masks,
            // callbacks, or edits can request ordinary ExternalNodes with
            // the gltfDisableExternalAssetInstancing read option.
            externalAssetInstancing =
                !GLTFReader::hasOption(
                    env.readOptions,
                    "gltfDisableExternalAssetInstancing");

            extractArrays(arrays);
        }

        //! glTF colors are linear until after texture modulation. The rest of
        //! osgEarth's coloring/lighting pipeline expects sRGB color values.
        static void installBaseColor(osg::Geometry* geom)
        {
            geom->setUserValue(CHONK_HINT_LINEAR_COLOR, true);
            ShaderLoader::load(VirtualProgram::getOrCreate(geom->getOrCreateStateSet()), R"(
#pragma vp_function oe_gltf_color_fs, fragment_coloring, 0.55
void oe_gltf_color_fs(inout vec4 color)
{
    vec3 c = clamp(color.rgb, 0.0, 1.0);
    color.rgb = mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
                    12.92 * c, lessThanEqual(c, vec3(0.0031308)));
}
)");
        }

        //! Shader that samples the original glTF material maps directly. It
        //! performs the glTF channel selection, normal scaling, and material
        //! factor application that would otherwise require converted images.
        static const char* pbrMaterialShaderSource()
        {
            return R"(
#pragma vp_function oe_gltf_pbr_vs, vertex_view, 1.0

out vec3 oe_gltf_pbr_pos_view;
out vec2 oe_gltf_pbr_uv;

void oe_gltf_pbr_vs(inout vec4 vertex_view)
{
    oe_gltf_pbr_pos_view = vertex_view.xyz / vertex_view.w;
    oe_gltf_pbr_uv = gl_MultiTexCoord0.st;
}

[break]
#pragma vp_function oe_gltf_parallax_fs, fragment_coloring, 0.4
#pragma import_defines(OE_GLTF_HAS_HEIGHT_MAP)
#pragma import_defines(OE_IS_SHADOW_CAMERA)
#pragma import_defines(OE_IS_DEPTH_CAMERA)

in vec3 vp_Normal;
in vec3 oe_gltf_pbr_pos_view;
in vec2 oe_gltf_pbr_uv;
vec2 oe_gltf_material_uv;

#ifdef OE_GLTF_HAS_HEIGHT_MAP
uniform sampler2D oe_gltf_height_tex;
uniform sampler2D oe_gltf_albedo_tex;
uniform float oe_pbr_displacement_scale = 0.01;
uniform float oe_gltf_parallax_max_distance = 50.0;
uniform mat4 osg_ProjectionMatrix;
float oe_gltf_relief_scale;
float oe_gltf_height_lod;
mat3 oe_gltf_pbr_tbn(vec3 N, vec3 p, vec2 uv);

vec2 oe_gltf_parallax_uv(vec2 uv, vec3 viewTS, vec2 dx, vec2 dy)
{
    float facing = abs(viewTS.z);
    // White is the top of the relief and midgray lies on the mesh plane.
    // Start above that plane and trace into the heightfield along the view ray,
    // accepting the first intersection (including raised edges).
    vec2 ray = viewTS.xy / max(facing, 0.25) *
        oe_gltf_relief_scale * smoothstep(0.05, 0.25, facing);

    // Convert the UV ray to pixels. Subpixel offsets cannot show useful
    // occlusion; fade those out before doing any height fetches.
    float det = dx.x * dy.y - dx.y * dy.x;
    if (abs(det) < 1e-20)
        return uv;
    vec2 rayPixels = vec2(dy.y * ray.x - dy.x * ray.y,
        dx.x * ray.y - dx.y * ray.x) / det;
    float pixels = length(rayPixels);
    if (pixels <= 0.5)
        return uv;
    ray *= smoothstep(0.5, 1.0, pixels);

    // Count texels at the filtered mip level, not the source resolution.
    // Explicit LOD keeps the height search isotropic: anisotropic filtering
    // can otherwise multiply the texture work at every step of the ray.
    float texels = length(ray * vec2(textureSize(oe_gltf_height_tex, 0))) *
        exp2(-oe_gltf_height_lod);
    float layers = ceil(clamp(texels, 8.0, 64.0));
    float stepDepth = 1.0 / layers;
    vec2 topUV = uv + 0.5 * ray;
    float depth = 0.0;
    float surfaceDepth = 1.0 - textureLod(oe_gltf_height_tex, topUV, oe_gltf_height_lod).r;
    if (surfaceDepth <= 0.0)
        return topUV;

    float previousDepth = 0.0;
    float previousError = surfaceDepth;
    float error = surfaceDepth;
    for (int i = 0; i < 64; ++i)
    {
        previousDepth = depth;
        previousError = error;
        depth = min(depth + stepDepth, 1.0);
        surfaceDepth = 1.0 - textureLod(oe_gltf_height_tex, topUV - ray * depth, oe_gltf_height_lod).r;
        error = surfaceDepth - depth;
        if (error <= 0.0)
            break;
    }

    // Interpolate the two samples bracketing the first intersection. This
    // avoids five additional height fetches for binary refinement.
    float weight = previousError / max(previousError - error, 1e-6);
    return topUV - ray * mix(previousDepth, depth, clamp(weight, 0.0, 1.0));
}
#endif

void oe_gltf_parallax_fs(inout vec4 color)
{
    oe_gltf_material_uv = oe_gltf_pbr_uv;
#ifdef OE_GLTF_HAS_HEIGHT_MAP
    vec2 dx = dFdx(oe_gltf_pbr_uv);
    vec2 dy = dFdy(oe_gltf_pbr_uv);
    oe_gltf_relief_scale = 0.0;
#if !defined(OE_IS_SHADOW_CAMERA) && !defined(OE_IS_DEPTH_CAMERA)
    // Use interpolated position, not interpolated vertex distances: large
    // wall triangles may be close even when all their vertices are far away.
    float distance2 = dot(oe_gltf_pbr_pos_view, oe_gltf_pbr_pos_view);
    float limit2 = max(oe_gltf_parallax_max_distance * oe_gltf_parallax_max_distance, 1e-6);
    oe_gltf_relief_scale = oe_gltf_parallax_max_distance > 0.0 ?
        max(oe_pbr_displacement_scale, 0.0) *
        (1.0 - smoothstep(0.16 * limit2, limit2, distance2)) : 0.0;

    // Evaluate derivatives before the distance branch. They are undefined
    // in nonuniform control flow, including at the edge of the distance fade.
    vec3 N = normalize(vp_Normal);
    mat3 tbn = oe_gltf_pbr_tbn(N, oe_gltf_pbr_pos_view, oe_gltf_pbr_uv);
    if (oe_gltf_relief_scale > 0.0)
    {
        vec2 size = vec2(textureSize(oe_gltf_height_tex, 0));
        vec2 tx = dx * size, ty = dy * size;
        oe_gltf_height_lod = 0.5 * log2(max(1.0, max(dot(tx, tx), dot(ty, ty))));
        // Orthographic rays are parallel to the view axis.
        vec3 V = osg_ProjectionMatrix[3][3] == 0.0 ?
            normalize(-oe_gltf_pbr_pos_view) : vec3(0.0, 0.0, 1.0);
        oe_gltf_material_uv = oe_gltf_parallax_uv(oe_gltf_pbr_uv,
            transpose(tbn) * V, dx, dy);
    }
#endif
    // Height-enabled albedo bypasses ShaderGenerator so all maps sample the
    // same offset UV. Keep alpha sampling in shadow/depth passes as well.
    color *= textureGrad(oe_gltf_albedo_tex, oe_gltf_material_uv, dx, dy);
#endif
}

[break]
#pragma vp_function oe_gltf_pbr_fs, fragment_coloring, 0.6
#pragma import_defines(OE_GLTF_HAS_HEIGHT_MAP)
#pragma import_defines(OE_IS_SHADOW_CAMERA)
#pragma import_defines(OE_IS_DEPTH_CAMERA)

struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;

in vec3 vp_Normal;
in vec3 oe_gltf_pbr_pos_view;
in vec2 oe_gltf_pbr_uv;
vec2 oe_gltf_material_uv;

uniform sampler2D oe_gltf_normal_tex;
uniform sampler2D oe_gltf_metallic_roughness_tex;
uniform sampler2D oe_gltf_occlusion_tex;
#ifdef OE_GLTF_HAS_HEIGHT_MAP
uniform sampler2D oe_gltf_height_tex;
float oe_gltf_relief_scale;
float oe_gltf_height_lod;
#endif

uniform vec4 oe_gltf_pbr_flags;
uniform vec4 oe_gltf_pbr_factors; // normal scale, roughness, metallic, AO strength

// Cotangent frame from screen-space derivatives. The bitangent follows
// increasing V, so the sampled glTF normal's Y component is inverted below.
mat3 oe_gltf_pbr_tbn(vec3 N, vec3 p, vec2 uv)
{
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float det = max(dot(T, T), dot(B, B));
    float invmax = det > 0.0 ? inversesqrt(det) : 0.0;
    return mat3(T * invmax, B * invmax, N);
}

void oe_gltf_pbr_fs(inout vec4 color)
{
#if defined(OE_IS_SHADOW_CAMERA) || defined(OE_IS_DEPTH_CAMERA)
    return;
#endif

    vec3 N = normalize(vp_Normal);
    mat3 tbn = oe_gltf_pbr_tbn(N, oe_gltf_pbr_pos_view, oe_gltf_pbr_uv);
    vec2 dx = dFdx(oe_gltf_pbr_uv);
    vec2 dy = dFdy(oe_gltf_pbr_uv);
    if (oe_gltf_pbr_flags.x > 0.5)
    {
        vec3 n = textureGrad(oe_gltf_normal_tex, oe_gltf_material_uv,
            dx, dy).xyz * 2.0 - 1.0;
        n.xy *= vec2(oe_gltf_pbr_factors.x, -oe_gltf_pbr_factors.x);
        vec3 pn = tbn * n;
        vp_Normal = dot(pn, pn) > 0.0 ? normalize(pn) : N;
    }

#ifdef OE_GLTF_HAS_HEIGHT_MAP
    if (oe_gltf_relief_scale > 0.0)
    {
        // Differentiate the filtered heightfield at the ray hit. The original
        // normal map describes its author's relief scale, which can be much
        // shallower than our parallax depth. Use the same cotangent metric as
        // the ray so the visible sides receive the corresponding lighting.
        vec2 delta = max(vec2(exp2(oe_gltf_height_lod)) /
            vec2(textureSize(oe_gltf_height_tex, 0)), abs(dx) + abs(dy));
        vec2 u = vec2(delta.x, 0.0);
        vec2 v = vec2(0.0, delta.y);
        vec2 slope = vec2(
            textureLod(oe_gltf_height_tex, oe_gltf_material_uv + u, oe_gltf_height_lod).r -
            textureLod(oe_gltf_height_tex, oe_gltf_material_uv - u, oe_gltf_height_lod).r,
            textureLod(oe_gltf_height_tex, oe_gltf_material_uv + v, oe_gltf_height_lod).r -
            textureLod(oe_gltf_height_tex, oe_gltf_material_uv - v, oe_gltf_height_lod).r) / (2.0 * delta);
        // Preserve the normal map's surface detail as an additional slope.
        vec3 detail = vp_Normal / max(dot(vp_Normal, N), 0.05) - N;
        vp_Normal = normalize(N + detail - oe_gltf_relief_scale *
            (tbn[0] * slope.x + tbn[1] * slope.y));
    }
#endif

    if (oe_gltf_pbr_flags.w > 0.5)
    {
        float roughness = oe_gltf_pbr_factors.y;
        float metal = oe_gltf_pbr_factors.z;
        if (oe_gltf_pbr_flags.y > 0.5)
        {
            vec4 metallicRoughness = textureGrad(oe_gltf_metallic_roughness_tex, oe_gltf_material_uv,
                dFdx(oe_gltf_pbr_uv), dFdy(oe_gltf_pbr_uv));
            roughness *= metallicRoughness.g;
            metal *= metallicRoughness.b;
        }
        oe_pbr.roughness *= roughness;
        oe_pbr.metal = clamp(oe_pbr.metal + metal, 0.0, 1.0);
    }

    if (oe_gltf_pbr_flags.z > 0.5)
    {
        float occlusion = textureGrad(oe_gltf_occlusion_tex, oe_gltf_material_uv,
            dFdx(oe_gltf_pbr_uv), dFdy(oe_gltf_pbr_uv)).r;
        oe_pbr.ao *= 1.0 + oe_gltf_pbr_factors.w * (occlusion - 1.0);
    }

#ifdef OE_GLTF_HAS_HEIGHT_MAP
    oe_pbr.displacement = textureGrad(oe_gltf_height_tex, oe_gltf_material_uv,
        dFdx(oe_gltf_pbr_uv), dFdy(oe_gltf_pbr_uv)).r;
#endif
}
)";
        }

        //! Installs the original glTF maps and the shader state that interprets
        //! their channels and factors.
        static void installPBRMaterial(
            osg::StateSet* stateset,
            osg::Texture2D* normalTex,
            osg::Texture2D* metallicRoughnessTex,
            osg::Texture2D* occlusionTex,
            float normalScale,
            float roughnessFactor,
            float metallicFactor,
            float occlusionStrength,
            bool applyPBRFactors,
            osg::Texture2D* heightTex = nullptr)
        {
            if (!stateset || (!normalTex && !metallicRoughnessTex && !occlusionTex && !applyPBRFactors && !heightTex))
                return;

            VirtualProgram* vp = VirtualProgram::getOrCreate(stateset);
            vp->setName("glTF PBR material");
            ShaderLoader::load(vp, pbrMaterialShaderSource());

            if (heightTex)
            {
                ShaderGenerator::setIgnoreHint(heightTex, true);
                stateset->setDefine("OE_GLTF_HAS_HEIGHT_MAP");
                stateset->setTextureAttribute(HEIGHT_UNIT, heightTex);
                stateset->addUniform(new osg::Uniform("oe_gltf_height_tex", (int)HEIGHT_UNIT));
                stateset->addUniform(new osg::Uniform("oe_gltf_albedo_tex", (int)ALBEDO_UNIT));
            }

            stateset->addUniform(new osg::Uniform(
                "oe_gltf_pbr_flags", osg::Vec4f(
                normalTex ? 1.0f : 0.0f,
                metallicRoughnessTex ? 1.0f : 0.0f,
                occlusionTex ? 1.0f : 0.0f,
                applyPBRFactors ? 1.0f : 0.0f)));
            stateset->addUniform(new osg::Uniform(
                "oe_gltf_pbr_factors", osg::Vec4f(
                normalScale,
                roughnessFactor,
                metallicFactor,
                occlusionStrength)));

            if (normalTex)
            {
                // Keep the ShaderGenerator from folding these into the color.
                ShaderGenerator::setIgnoreHint(normalTex, true);
                stateset->setTextureAttribute(NORMAL_UNIT, normalTex);
                stateset->addUniform(new osg::Uniform("oe_gltf_normal_tex", (int)NORMAL_UNIT));
            }

            if (metallicRoughnessTex)
            {
                ShaderGenerator::setIgnoreHint(metallicRoughnessTex, true);
                stateset->setTextureAttribute(METALLIC_ROUGHNESS_UNIT, metallicRoughnessTex);
                stateset->addUniform(new osg::Uniform(
                    "oe_gltf_metallic_roughness_tex", (int)METALLIC_ROUGHNESS_UNIT));
            }

            if (occlusionTex)
            {
                ShaderGenerator::setIgnoreHint(occlusionTex, true);
                stateset->setTextureAttribute(OCCLUSION_UNIT, occlusionTex);
                stateset->addUniform(new osg::Uniform(
                    "oe_gltf_occlusion_tex", (int)OCCLUSION_UNIT));
            }
        }

        static std::string makeExternalBatchKey(
            const std::string& filename,
            const osgDB::Options* options)
        {
            std::ostringstream key;
            key << filename << '\x1f';
            if (options)
            {
                key << options->getOptionString() << '\x1e'
                    << static_cast<const void*>(options->getFindFileCallback())
                    << '\x1e'
                    << static_cast<const void*>(options->getReadFileCallback())
                    << '\x1e'
                    << static_cast<const void*>(options->getAuthenticationMap())
                    << '\x1e'
                    << static_cast<const void*>(URIAliasMap::from(options))
                    << '\x1e'
                    << static_cast<const void*>(
                        URIPostReadCallback::from(options));
            }
            return key.str();
        }

        static osg::Matrixd nodeMatrix(const tinygltf::Node& node)
        {
            osg::Matrixd matrix;
            if (node.matrix.size() == 16)
                matrix.set(node.matrix.data());

            if (matrix.isIdentity())
            {
                osg::Matrixd scale, translation, rotation;
                if (node.scale.size() == 3)
                    scale.makeScale(node.scale[0], node.scale[1], node.scale[2]);
                if (node.rotation.size() == 4)
                    rotation.makeRotate(osg::Quat(
                        node.rotation[0], node.rotation[1],
                        node.rotation[2], node.rotation[3]));
                if (node.translation.size() == 3)
                    translation.makeTranslate(
                        node.translation[0], node.translation[1], node.translation[2]);
                matrix = scale * rotation * translation;
            }
            return matrix;
        }

        static bool matrixReversesWinding(const osg::Matrixd& matrix)
        {
            const double determinant =
                matrix(0, 0) * (matrix(1, 1) * matrix(2, 2) - matrix(1, 2) * matrix(2, 1)) -
                matrix(0, 1) * (matrix(1, 0) * matrix(2, 2) - matrix(1, 2) * matrix(2, 0)) +
                matrix(0, 2) * (matrix(1, 0) * matrix(2, 1) - matrix(1, 1) * matrix(2, 0));
            return determinant < 0.0;
        }

        ExternalReference* externalReference(
            const tinygltf::Node& node, bool reversesWinding) const
        {
            const auto external = node.extensions.find(externalAssetExtension());
            if (external == node.extensions.end() || !external->second.IsObject())
                return nullptr;
            const tinygltf::Value& uriValue = external->second.Get("uri");
            if (!uriValue.IsString())
                return nullptr;

            const auto key = std::make_pair(
                static_cast<std::size_t>(&node - model.nodes.data()),
                reversesWinding);
            auto found = externalReferences.find(key);
            if (found != externalReferences.end())
                return &found->second;

            ExternalReference& reference = externalReferences[key];
            reference.filename = resolveResourceURI(
                uriValue.Get<std::string>(), env.referrer);
            reference.options = Registry::cloneOrCreateOptions(env.readOptions);

            // The containing root supplies the Y-up-to-Z-up conversion.
            // Nested assets contribute only their default scene, in glTF space.
            appendOption(reference.options.get(), "gltfZUp");
            appendOption(reference.options.get(), "gltfDefaultSceneOnly");
            removeOption(reference.options.get(), "gltfForceReload");
            removeOption(reference.options.get(), "gltfExternalAssetInstancing");

            // Absolute FrontFace state inside the asset depends on the
            // cumulative parity, which also identifies its shared cache variant.
            if (reversesWinding)
                appendOption(reference.options.get(), "gltfParentReversesWinding");
            else
                removeOption(reference.options.get(), "gltfParentReversesWinding");

            const tinygltf::Value& nameValue = external->second.Get("name");
            if (nameValue.IsString())
                reference.externalName = nameValue.Get<std::string>();
            return &reference;
        }

        void collectExternalAssetInstances(
            const tinygltf::Node& node,
            const osg::Matrixd& parentMatrix,
            bool parentReversesWinding)
        {
            // State transitions require their original traversable hierarchy.
            if (node.extensions.find("OWT_state") != node.extensions.end())
                return;

            const osg::Matrixd localMatrix = nodeMatrix(node);
            const osg::Matrixd matrix = localMatrix * parentMatrix;
            const bool reversesWinding =
                parentReversesWinding != matrixReversesWinding(localMatrix);
            for (int child : node.children)
                collectExternalAssetInstances(
                    model.nodes[child], matrix, reversesWinding);

            ExternalReference* reference = externalReference(node, reversesWinding);
            if (!reference)
                return;

            if (!reference->batch)
            {
                auto& batch = externalBatches[makeExternalBatchKey(
                    reference->filename, reference->options.get())];
                if (!batch.reference)
                    batch.reference = reference;
                reference->batch = &batch;
            }
            // Accumulate in double precision just as computeLocalToWorld did,
            // then convert once at the instance-array boundary.
            reference->batch->matrices.emplace_back(matrix);
        }

        void prepareExternalAssetInstancing(
            const std::vector<int>& sceneIndices, bool parentReversesWinding)
        {
            if (!externalAssetInstancing)
                return;

            for (int sceneIndex : sceneIndices)
            {
                for (int nodeIndex : model.scenes[sceneIndex].nodes)
                {
                    if (nodeIndex >= 0 &&
                        static_cast<std::size_t>(nodeIndex) < model.nodes.size())
                    {
                        collectExternalAssetInstances(
                            model.nodes[nodeIndex], osg::Matrixd(),
                            parentReversesWinding);
                    }
                }
            }

            for (auto& entry : externalBatches)
            {
                ExternalInstanceBatch& batch = entry.second;
                if (batch.matrices.size() < 2u)
                    continue;

                const ExternalReference& reference = *batch.reference;
                batch.node = new osgEarth::InstancedExternalNode(
                    reference.filename, batch.matrices, reference.options.get());
                batch.node->setName(reference.externalName);
                if (!batch.node->isLoaded() && error.empty())
                {
                    error = batch.node->getLastError();
                    if (error.empty())
                        error = "Failed to load external asset " + reference.filename;
                }

                // Keep unsuccessful hardware candidates alive until ordinary
                // nodes are built, so fallback references reuse their loaded
                // payload even when manager retention is disabled.
            }
        }

        osg::Node* createNode(
            const tinygltf::Node& node,
            bool parentReversesWinding,
            bool canInstanceExternalAssets = true) const
        {
            const osg::Matrixd matrix = nodeMatrix(node);
            const bool localReversesWinding = matrixReversesWinding(matrix);
            const bool reversesWinding =
                parentReversesWinding != localReversesWinding;

            // Allocate the transform only if ordinary content needs it. A
            // subtree consumed entirely by batches never creates OSG nodes.
            osg::ref_ptr<osg::MatrixTransform> mt;
            auto transform = [&]() -> osg::MatrixTransform*
            {
                if (!mt)
                {
                    mt = new osg::MatrixTransform;
                    mt->setMatrix(matrix);
                    if (localReversesWinding)
                    {
                        mt->getOrCreateStateSet()->setAttribute(
                            new osg::FrontFace(reversesWinding ?
                                osg::FrontFace::CLOCKWISE :
                                osg::FrontFace::COUNTER_CLOCKWISE));
                    }
                }
                return mt.get();
            };

            if (node.mesh >= 0)
            {
                osg::Group* meshNode = nullptr;
                if (node.extensions.find("EXT_mesh_gpu_instancing") != node.extensions.end())
                {
                    meshNode = makeMesh(model.meshes[node.mesh], true);
                    makeInstancedMeshNode(node, meshNode);
                }
                else
                {
                    meshNode = makeMesh(model.meshes[node.mesh], false);
                }
                transform()->addChild(meshNode);
            }

            const bool hasStateTransition =
                node.extensions.find("OWT_state") != node.extensions.end();
            const bool childrenCanInstanceExternalAssets =
                canInstanceExternalAssets && !hasStateTransition;
            bool consumedByBatch = false;
            for (int childIndex : node.children)
            {
                osg::Node* child = createNode(
                    model.nodes[childIndex], reversesWinding,
                    childrenCanInstanceExternalAssets);
                if (child)
                    transform()->addChild(child);
                else
                    consumedByBatch = true;
            }

            // External content follows ordinary children, as in the source glTF.
            ExternalReference* reference = externalReference(node, reversesWinding);
            if (reference)
            {
                if (childrenCanInstanceExternalAssets &&
                    reference->batch && reference->batch->isInstanced())
                {
                    consumedByBatch = true;
                }
                else
                {
                    osg::ref_ptr<osgEarth::ExternalNode> externalNode =
                        new osgEarth::ExternalNode(
                            reference->filename, reference->options.get());
                    externalNode->setName(reference->externalName);
                    transform()->addChild(externalNode.get());
                    if (!externalNode->isLoaded() && error.empty())
                    {
                        error = externalNode->getLastError();
                        if (error.empty())
                            error = "Failed to load external asset " + reference->filename;
                    }
                }
            }

            if (!mt && consumedByBatch)
                return nullptr;

            // Preserve originally empty nodes, single references, mixed mesh
            // branches, and hardware-rejected hierarchies exactly as before.
            transform();
            osg::ref_ptr<osg::Node> top = mt.release();
            if (hasStateTransition)
            {
                StateTransitionNode* st = new StateTransitionNode;
                st->addChild(top.get());
                auto ext = node.extensions.find("OWT_state")->second;
                for (auto& key : ext.Keys())
                    st->_stateToNodeName[key] = ext.Get(key).Get<std::string>();
                top = st;
            }
            top->setName(node.name);
            return top.release();
        }

        int getTextureSource(const tinygltf::Texture& texture) const
        {
            auto extensionIt = texture.extensions.find("EXT_texture_webp");
            if (extensionIt != texture.extensions.end() && extensionIt->second.IsObject())
            {
                const tinygltf::Value& source = extensionIt->second.Get("source");
                if (source.IsInt())
                    return source.Get<int>();
            }
            return texture.source;
        }

        osg::Image* makeImageFromModel(int source) const
        {
            if (source < 0 || static_cast<size_t>(source) >= model.images.size())
                return nullptr;

            const tinygltf::Image& image = model.images[source];
            bool imageEmbedded =
                tinygltf::IsDataURI(image.uri) ||
                image.image.size() > 0;

            const std::string imageFilename = imageEmbedded ?
                image.uri : resolveResourceURI(image.uri, env.referrer);
            osgEarth::URI imageURI(imageFilename);

            osg::ref_ptr<osg::Image> img;

            if (image.as_is && image.image.size() > 0)
            {
                osgDB::ReaderWriter* imageReader =
                    osgDB::Registry::instance()->getReaderWriterForMimeType(image.mimeType);
                if (!imageReader && image.mimeType == "image/webp")
                    imageReader = osgDB::Registry::instance()->getReaderWriterForExtension("webp");

                if (imageReader)
                {
                    std::string encoded(
                        reinterpret_cast<const char*>(image.image.data()), image.image.size());
                    std::istringstream stream(encoded, std::ios::in | std::ios::binary);
                    osgDB::ReaderWriter::ReadResult result =
                        imageReader->readImage(stream, env.readOptions);
                    if (result.validImage())
                    {
                        img = result.takeImage();
                        // Image plugins return OSG-oriented data. Embedded
                        // glTF image bytes retain their top-row-first layout.
                        img->flipVertical();
                    }
                }
            }
            else if (image.image.size() > 0)
            {
                GLenum format = GL_RGB, texFormat = GL_RGB8;
                if (image.component == 4) format = GL_RGBA, texFormat = GL_RGBA8;

                img = new osg::Image();
                //OE_NOTICE << "Loading image of size " << image.width << "x" << image.height << " components = " << image.component << " totalSize=" << image.image.size() << std::endl;
                unsigned char *imgData = new unsigned char[image.image.size()];
                memcpy(imgData, &image.image[0], image.image.size());
                img->setImage(image.width, image.height, 1, texFormat, format, GL_UNSIGNED_BYTE, imgData, osg::Image::AllocationMode::USE_NEW_DELETE);
            }

            else if (!imageEmbedded) // load from URI
            {
                // GLTF images are assumed to be flipped so that the top of the image is the first row of pixels. OSG images are assumed to be flipped so that the bottom of the image is the first row of pixels. So we need to flip the image vertically when loading it.
                // We use this .flipvertical psuedoloader to flip the image inside of a loader so the flipped image is cached if
                // the same image is used again.
                imageURI = URI(imageURI.full() + ".flipvertical");
                osgDB::ReaderWriter::ReadResult rr =
                    osgDB::Registry::instance()->readImageImplementation(
                        imageURI.full(), env.readOptions);
                if (rr.validImage())
                    img = rr.takeImage();
            }

            return img.release();
        }

        //! Loads the image referenced by a glTF texture, honoring the
        //! EXT_texture_webp alternative and its optional core fallback.
        osg::ref_ptr<osg::Image> makeImageFromTexture(const tinygltf::Texture& texture) const
        {
            const int source = getTextureSource(texture);
            osg::ref_ptr<osg::Image> img = makeImageFromModel(source);

            // If the WebP alternative cannot be decoded, retain the optional
            // core PNG/JPEG fallback when one is present.
            if (!img.valid() && source != texture.source)
                img = makeImageFromModel(texture.source);

            return img;
        }

        //! Wraps an image in a texture configured from the glTF texture's sampler.
        osg::Texture2D* makeTexture(osg::Image* img, const tinygltf::Texture& texture, bool srgb) const
        {
            if (!img)
                return nullptr;

            if(img->getPixelFormat() == GL_RGB)
                img->setInternalTextureFormat(GL_RGB8);
            else if (img->getPixelFormat() == GL_RGBA)
                img->setInternalTextureFormat(GL_RGBA8);

            osg::ref_ptr<osg::Texture2D> tex = new osg::Texture2D(img);
            // Set the texture format, not the shared image's format: one image
            // can supply both sRGB base color and linear material data.
            if (srgb)
                tex->setInternalFormat(img->getPixelFormat() == GL_RGB ? GL_SRGB8 : GL_SRGB8_ALPHA8);
            //tex->setUnRefImageDataAfterApply(imageEmbedded);
            tex->setResizeNonPowerOfTwoHint(false);
            tex->setDataVariance(osg::Object::STATIC);

            // Preserve texture detail at grazing angles while retaining mipmaps.
            tex->setMaxAnisotropy(16.0f);

            if (texture.sampler >= 0 && texture.sampler < model.samplers.size())
            {
                const tinygltf::Sampler& sampler = model.samplers[texture.sampler];
                //tex->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)sampler.minFilter);
                //tex->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)sampler.magFilter);
                tex->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR_MIPMAP_LINEAR); //sampler.minFilter);
                tex->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR); //sampler.magFilter);
                tex->setWrap(osg::Texture::WRAP_S, (osg::Texture::WrapMode)sampler.wrapS);
                tex->setWrap(osg::Texture::WRAP_T, (osg::Texture::WrapMode)sampler.wrapT);
                tex->setWrap(osg::Texture::WRAP_R, (osg::Texture::WrapMode)sampler.wrapR);
            }
            else
            {
                tex->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR_MIPMAP_LINEAR);
                tex->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR);
                tex->setWrap(osg::Texture::WRAP_S, (osg::Texture::WrapMode)osg::Texture::CLAMP_TO_EDGE);
                tex->setWrap(osg::Texture::WRAP_T, (osg::Texture::WrapMode)osg::Texture::CLAMP_TO_EDGE);
            }

            return tex.release();
        }

        osg::Texture2D* makeTextureFromModel(const tinygltf::Texture& texture, bool srgb) const
        {
            osg::ref_ptr<osg::Image> img = makeImageFromTexture(texture);
            return makeTexture(img.get(), texture, srgb);
        }

        bool validTextureIndex(int index) const
        {
            return index >= 0 && static_cast<size_t>(index) < model.textures.size();
        }

        //! Key under which a texture may be shared across models through the
        //! reader's TextureCache. Embedded images have no stable identity and
        //! return an empty key (i.e., do not share them).
        std::string sharedTextureKey(const tinygltf::Texture& texture, const std::string& usage) const
        {
            const int source = getTextureSource(texture);
            if (source < 0 || static_cast<size_t>(source) >= model.images.size())
                return {};

            const tinygltf::Image& image = model.images[source];
            const bool imageEmbedded =
                tinygltf::IsDataURI(image.uri) ||
                image.image.size() > 0;

            if (imageEmbedded)
                return {};

            return resolveResourceURI(image.uri, env.referrer) + usage;
        }

        //! Looks up a texture in this model's local cache and, when sharedKey
        //! is non-empty, in the reader's cross-model TextureCache; otherwise
        //! creates it with the supplied function and caches the result.
        template<typename CREATE>
        osg::ref_ptr<osg::Texture2D> getOrCreateTexture(
            const std::string& localKey,
            const std::string& sharedKey,
            CREATE&& create) const
        {
            auto local = localTextures.find(localKey);
            if (local != localTextures.end())
                return local->second;

            osg::ref_ptr<osg::Texture2D> tex;
            TextureCache* sharedCache = reader->_texCache;
            const bool useSharedCache =
                sharedCache != nullptr && !sharedKey.empty();
            const bool forceReload =
                hasOption(env.readOptions, "gltfForceReload");

            if (useSharedCache && !forceReload)
            {
                std::lock_guard<std::mutex> lock(sharedCache->mutex());
                auto i = sharedCache->find(sharedKey);
                if (i != sharedCache->end())
                {
                    if (!i->second.lock(tex))
                        sharedCache->erase(i);
                }
            }

            if (!tex.valid())
            {
                tex = create();

                if (tex.valid() && useSharedCache)
                {
                    std::lock_guard<std::mutex> lock(sharedCache->mutex());
                    if (forceReload)
                    {
                        // Publish the freshly loaded texture for subsequent
                        // ordinary reads. Existing graphs retain their old
                        // Texture2D until the shared asset swap completes.
                        (*sharedCache)[sharedKey] = tex.get();
                    }
                    else
                    {
                        auto insResult = sharedCache->insert(
                            TextureCache::value_type(sharedKey, tex.get()));
                        if (!insResult.second)
                        {
                            // Some other loader thread beat us to the cache.
                            // Reclaim an expired weak entry if its graph was
                            // released between lookup and insertion.
                            osg::ref_ptr<osg::Texture2D> existing;
                            if (insResult.first->second.lock(existing))
                                tex = existing;
                            else
                                insResult.first->second = tex.get();
                        }
                    }
                }
            }

            localTextures[localKey] = tex;
            return tex;
        }

        //! Returns a glTF texture without rewriting its source image. Keep the
        //! ShaderGenerator-managed color binding separate from maps interpreted
        //! by our material shader, since the latter carry an ignore hint.
        osg::ref_ptr<osg::Texture2D> getOrCreateSourceTexture(
            int textureIndex,
            bool shaderManaged) const
        {
            if (!validTextureIndex(textureIndex))
                return {};

            const tinygltf::Texture& texture = model.textures[textureIndex];
            const char* usage = shaderManaged ? "material" : "color";
            return getOrCreateTexture(
                Stringify() << usage << ":" << textureIndex,
                sharedTextureKey(texture, Stringify() << "|gltf-" << usage),
                [&]() { return osg::ref_ptr<osg::Texture2D>(makeTextureFromModel(texture, !shaderManaged)); });
        }

        //! Base color (albedo) texture for a material.
        osg::ref_ptr<osg::Texture2D> getOrCreateColorTexture(
            const tinygltf::TextureInfo& info, bool parallax = false) const
        {
            if (!parallax)
                return getOrCreateSourceTexture(info.index, false);
            if (!validTextureIndex(info.index))
                return {};

            // Ignore hints belong to textures, so do not share this binding
            // with a material whose color is still sampled by ShaderGenerator.
            const auto& texture = model.textures[info.index];
            return getOrCreateTexture(
                Stringify() << "parallax-color:" << info.index,
                sharedTextureKey(texture, "|gltf-parallax-color"),
                [&]() {
                    osg::ref_ptr<osg::Texture2D> tex = makeTextureFromModel(texture, true);
                    if (tex.valid()) ShaderGenerator::setIgnoreHint(tex.get(), true);
                    return tex;
                });
        }

        //! Optional local Substance sidecar, e.g. civic_basecolor.png ->
        //! civic_height.png. Plain names also work: brick.png -> brick_height.png.
        osg::ref_ptr<osg::Texture2D> getOrCreateHeightTexture(const tinygltf::TextureInfo& info) const
        {
            if (!validTextureIndex(info.index) || info.texCoord != 0)
                return {};
            const auto& texture = model.textures[info.index];
            int source = getTextureSource(texture);
            // An embedded WebP alternative can still have an external core URI.
            if (source >= 0 && static_cast<size_t>(source) < model.images.size() &&
                (model.images[source].uri.empty() || tinygltf::IsDataURI(model.images[source].uri)))
                source = texture.source;
            if (source < 0 || static_cast<size_t>(source) >= model.images.size())
                return {};
            const auto& image = model.images[source];
            if (image.uri.empty() || tinygltf::IsDataURI(image.uri))
                return {};
            const std::string filename = resolveResourceURI(image.uri, env.referrer);
            // This convention is for local assets; do not probe arbitrary URLs.
            if (URI(filename).isRemote())
                return {};
            std::string stem = osgDB::getNameLessExtension(filename);
            const std::string lowerStem = osgDB::convertToLowerCase(stem);
            for (const std::string suffix : { "_basecolor", "_base_color", "_albedo", "_diffuse" })
            {
                if (stem.size() >= suffix.size() &&
                    lowerStem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0)
                {
                    stem.resize(stem.size() - suffix.size());
                    break;
                }
            }
            const std::string heightFilename = stem + "_height.png";
            // Sampler state is part of the key, since a sidecar may be used
            // with different wrap modes in different models.
            std::ostringstream key;
            key << heightFilename << "|gltf-height";
            if (texture.sampler >= 0 && static_cast<size_t>(texture.sampler) < model.samplers.size())
            {
                const auto& sampler = model.samplers[texture.sampler];
                key << ':' << sampler.wrapS << ':' << sampler.wrapT << ':' << sampler.wrapR;
            }
            //OE_INFO << "Looking for height" << heightFilename  << std::endl;
            return getOrCreateTexture(key.str(), key.str(), [&]() {
                if (!osgDB::fileExists(heightFilename))
                    return osg::ref_ptr<osg::Texture2D>();
                auto rr = osgDB::Registry::instance()->readImageImplementation(
                    heightFilename + ".flipvertical", env.readOptions);
                if (!rr.validImage())
                    return osg::ref_ptr<osg::Texture2D>();

                OE_INFO << "Found height!" << heightFilename << std::endl;
                osg::ref_ptr<osg::Image> heightImage = rr.takeImage();
                // PNG grayscale uses legacy luminance formats. Core OpenGL
                // requires RED/RG for both the upload format and the storage;
                // changing only the texture's internal format is insufficient.
                // Clone before changing metadata since the loader may cache it.
                GLenum format = heightImage->getPixelFormat();
                if (format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA)
                {
                    heightImage = new osg::Image(*heightImage, osg::CopyOp::DEEP_COPY_ALL);
                    format = format == GL_LUMINANCE ? GL_RED : GL_RG;
                    heightImage->setPixelFormat(format);
                }
                const bool sixteenBit = heightImage->getDataType() == GL_UNSIGNED_SHORT;
                if (format == GL_RED)
                    heightImage->setInternalTextureFormat(sixteenBit ? GL_R16 : GL_R8);
                else if (format == GL_RG)
                    heightImage->setInternalTextureFormat(sixteenBit ? GL_RG16 : GL_RG8);
                osg::ref_ptr<osg::Texture2D> tex = makeTexture(heightImage.get(), texture, false);
                tex->setName(heightFilename);
                // Preserve the precision of Substance's 16-bit RGB(A) exports.
                if (sixteenBit)
                {
                    if (heightImage->getPixelFormat() == GL_RGB) tex->setInternalFormat(GL_RGB16);
                    else if (heightImage->getPixelFormat() == GL_RGBA) tex->setInternalFormat(GL_RGBA16);
                }
                ShaderGenerator::setIgnoreHint(tex.get(), true);
                return tex;
            });
        }

        //! tinygltf's legacy values map only contains factors explicitly
        //! present in pbrMetallicRoughness. Preserve the reader's existing
        //! fallback for materials that specify no PBR maps or factors.
        static bool hasExplicitPBRFactors(const tinygltf::Material& material)
        {
            return
                material.values.find("metallicFactor") != material.values.end() ||
                material.values.find("roughnessFactor") != material.values.end();
        }

        template<typename ArrayType>
        static osg::Vec4Array* multiplyVertexColors(
            const ArrayType* source, const osg::Vec4& factor, unsigned components)
        {
            if (!source)
                return nullptr;

            // Work on a new array: different primitives can share COLOR_0
            // while using different material factors. Alpha is always linear.
            auto* result = new osg::Vec4Array(osg::Array::BIND_PER_VERTEX);
            result->reserve(source->size());
            const float scale = source->getNormalize() ?
                1.0f / float(std::numeric_limits<typename ArrayType::ElementDataType::value_type>::max()) : 1.0f;
            for (const auto& value : *source)
            {
                osg::Vec4 color = factor;
                for (unsigned c = 0; c < components; ++c)
                    color[c] *= float(value[c]) * scale;
                result->push_back(color);
            }
            return result;
        }

        osg::Vec4Array* makeColorArray(int index, const osg::Vec4& factor) const
        {
            const osg::Array* source = arrays[index].get();
            if (!source)
                return nullptr;
            switch (source->getType())
            {
            case osg::Array::Vec3ArrayType:
                return multiplyVertexColors(static_cast<const osg::Vec3Array*>(source), factor, 3);
            case osg::Array::Vec4ArrayType:
                return multiplyVertexColors(static_cast<const osg::Vec4Array*>(source), factor, 4);
            case osg::Array::Vec3ubArrayType:
                return multiplyVertexColors(static_cast<const osg::Vec3ubArray*>(source), factor, 3);
            case osg::Array::Vec4ubArrayType:
                return multiplyVertexColors(static_cast<const osg::Vec4ubArray*>(source), factor, 4);
            case osg::Array::Vec3usArrayType:
                return multiplyVertexColors(static_cast<const osg::Vec3usArray*>(source), factor, 3);
            case osg::Array::Vec4usArrayType:
                return multiplyVertexColors(static_cast<const osg::Vec4usArray*>(source), factor, 4);
            default:
                return nullptr;
            }
        }

        template<typename ArrayType>
        static osg::Vec3Array* expandVertexArray(
            const ArrayType* source,
            double normalizationScale,
            bool clampSignedNormalized)
        {
            if (!source)
                return nullptr;

            osg::Vec3Array* result = new osg::Vec3Array;
            result->reserve(source->size());
            for (const auto& value : *source)
            {
                osg::Vec3 vertex(
                    static_cast<float>(value.x() / normalizationScale),
                    static_cast<float>(value.y() / normalizationScale),
                    static_cast<float>(value.z() / normalizationScale));

                // glTF maps the most-negative signed integer to -1 as well.
                if (clampSignedNormalized)
                {
                    vertex.x() = std::max(vertex.x(), -1.0f);
                    vertex.y() = std::max(vertex.y(), -1.0f);
                    vertex.z() = std::max(vertex.z(), -1.0f);
                }
                result->push_back(vertex);
            }
            result->setBinding(source->getBinding());
            return result;
        }

        osg::Array* makeVertexArray(int accessorIndex) const
        {
            if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= arrays.size())
                return nullptr;

            const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
            osg::Array* source = arrays[accessorIndex].get();
            if (!source || accessor.type != TINYGLTF_TYPE_VEC3)
                return nullptr;

            // Geometry's PrimitiveFunctor and bounds computation only support
            // floating-point vertex arrays. KHR_mesh_quantization permits the
            // integer POSITION accessors handled below, so expand them while
            // retaining their glTF normalization semantics.
            switch (accessor.componentType)
            {
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                return expandVertexArray(
                    dynamic_cast<osg::Vec3bArray*>(source),
                    accessor.normalized ? 127.0 : 1.0,
                    accessor.normalized);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return expandVertexArray(
                    dynamic_cast<osg::Vec3ubArray*>(source),
                    accessor.normalized ? 255.0 : 1.0,
                    false);
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                return expandVertexArray(
                    dynamic_cast<osg::Vec3sArray*>(source),
                    accessor.normalized ? 32767.0 : 1.0,
                    accessor.normalized);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                return expandVertexArray(
                    dynamic_cast<osg::Vec3usArray*>(source),
                    accessor.normalized ? 65535.0 : 1.0,
                    false);
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                return source;
            default:
                OE_WARN << LC << "Unsupported POSITION component type "
                    << accessor.componentType << std::endl;
                return nullptr;
            }
        }

        osg::Group* makeMesh(const tinygltf::Mesh& mesh, bool prepInstancing) const
        {
            osg::Group *group = new osg::Group;

            //OE_DEBUG << "Drawing " << mesh.primitives.size() << " primitives in mesh" << std::endl;

            for (size_t i = 0; i < mesh.primitives.size(); i++) {

                //OE_DEBUG << " Processing primitive " << i << std::endl;
                const tinygltf::Primitive &primitive = mesh.primitives[i];
                if (primitive.indices < 0)
                {
                    // Hmm, should delete group here
                    return 0;
                }

                osg::ref_ptr< osg::Geometry > geom;
                if (prepInstancing)
                {
                    geom = osgEarth::InstanceBuilder::createGeometry();
                }
                else
                {
                    geom = new osg::Geometry;
                }
                geom->setName(typeid(*this).name());
                geom->setUseVertexBufferObjects(true);
                installBaseColor(geom);

                osg::Geode* geode = new osg::Geode;
                geode->addDrawable(geom);
                group->addChild(geode);

                // The base color factor of the material
                osg::Vec4 baseColorFactor(1.0f, 1.0f, 1.0f, 1.0f);

                if (primitive.material >= 0 && primitive.material < model.materials.size())
                {
                    const tinygltf::Material& material = model.materials[primitive.material];
                    const tinygltf::PbrMetallicRoughness& pbr = material.pbrMetallicRoughness;

                    if (material.doubleSided)
                    {
                        geom->getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
                    }

                    if (pbr.baseColorFactor.size() == 4)
                    {
                        baseColorFactor.set(
                            static_cast<float>(pbr.baseColorFactor[0]),
                            static_cast<float>(pbr.baseColorFactor[1]),
                            static_cast<float>(pbr.baseColorFactor[2]),
                            static_cast<float>(pbr.baseColorFactor[3]));
                    }

                    // Base color (albedo) map. This is a plain osg::Texture2D on
                    // unit 0, also recognized by the Chonk ripper. With a height
                    // sidecar the material shader takes over color sampling.
                    osg::ref_ptr<osg::Texture2D> heightTex = loadPBRTextures ?
                        getOrCreateHeightTexture(pbr.baseColorTexture) : nullptr;
                    osg::ref_ptr<osg::Texture2D> albedoTex = getOrCreateColorTexture(pbr.baseColorTexture, heightTex.valid());
                    if (!albedoTex.valid())
                        heightTex = nullptr;
                    if (albedoTex.valid())
                    {
                        // Set the mode along with the attribute: on OSG builds with the
                        // fixed-function pipeline available, the ShaderGenerator only
                        // captures texture attributes whose GL_TEXTURE_2D mode is ON,
                        // and it removes the mode again once it has generated the sampler.
                        geom->getOrCreateStateSet()->setTextureAttributeAndModes(ALBEDO_UNIT, albedoTex.get());
                    }

                    // Bind the original glTF maps directly. The material shader
                    // selects their glTF channels and applies the scalar factors.
                    if (loadPBRTextures)
                    {
                        osg::ref_ptr<osg::Texture2D> normalTex =
                            getOrCreateSourceTexture(material.normalTexture.index, true);
                        osg::ref_ptr<osg::Texture2D> metallicRoughnessTex =
                            getOrCreateSourceTexture(pbr.metallicRoughnessTexture.index, true);
                        osg::ref_ptr<osg::Texture2D> occlusionTex =
                            getOrCreateSourceTexture(material.occlusionTexture.index, true);

                        const bool applyPBRFactors =
                            metallicRoughnessTex.valid() ||
                            occlusionTex.valid() ||
                            hasExplicitPBRFactors(material);

                        if (normalTex.valid() || applyPBRFactors || heightTex.valid())
                        {
                            installPBRMaterial(
                                geom->getOrCreateStateSet(),
                                normalTex.get(),
                                metallicRoughnessTex.get(),
                                occlusionTex.get(),
                                static_cast<float>(material.normalTexture.scale),
                                static_cast<float>(pbr.roughnessFactor),
                                static_cast<float>(pbr.metallicFactor),
                                static_cast<float>(material.occlusionTexture.strength),
                                applyPBRFactors,
                                heightTex.get());
                        }

                    }

                    if (material.alphaMode == "BLEND")
                    {
                        osg::StateSet* stateset = geom->getOrCreateStateSet();
                        stateset->setMode(GL_BLEND, osg::StateAttribute::ON);
                        stateset->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                        osgEarth::Util::DiscardAlphaFragments().install(stateset, 0.15);
                    }
                    else if (material.alphaMode == "MASK")
                    {
                        osg::StateSet* stateset = geom->getOrCreateStateSet();
                        stateset->setMode(GL_BLEND, osg::StateAttribute::ON);
                        stateset->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                        osgEarth::Util::DiscardAlphaFragments().install(stateset, material.alphaCutoff);
                    }
                }

                std::map<std::string, int>::const_iterator it(primitive.attributes.begin());
                std::map<std::string, int>::const_iterator itEnd(
                    primitive.attributes.end());

                for (; it != itEnd; it++)
                {
                    const tinygltf::Accessor &accessor = model.accessors[it->second];

                    if (it->first.compare("POSITION") == 0)
                    {
                        geom->setVertexArray(makeVertexArray(it->second));
                    }
                    else if (it->first.compare("NORMAL") == 0)
                    {
                        geom->setNormalArray(arrays[it->second].get());
                    }
                    else if (it->first.compare("TEXCOORD_0") == 0)
                    {
                        geom->setTexCoordArray(0, arrays[it->second].get());
                    }
                    else if (it->first.compare("TEXCOORD_1") == 0)
                    {
                        geom->setTexCoordArray(1, arrays[it->second].get());
                    }
                    else if (it->first.compare("COLOR_0") == 0)
                    {
                        geom->setColorArray(makeColorArray(it->second, baseColorFactor));
                    }
                    else
                    {
                        //OE_DEBUG << "Skipping array " << it->first << std::endl;
                    }
                }

                // If there is no color array just add one that has the base color factor in it.
                if (!geom->getColorArray())
                {
                    osg::Vec4ubArray* colors = new osg::Vec4ubArray();
                    osg::Vec4ub color = packColor(baseColorFactor);
                    colors->setNormalize(true);
                    colors->push_back(color);
                    osg::Array* verts = geom->getVertexArray();
                    if (verts)
                    {
                        colors->assign(verts->getNumElements(), color);
                    }
                    geom->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
                }

                int mode = -1;
                if (primitive.mode == TINYGLTF_MODE_TRIANGLES) {
                    mode = GL_TRIANGLES;
                }
                else if (primitive.mode == TINYGLTF_MODE_TRIANGLE_STRIP) {
                    mode = GL_TRIANGLE_STRIP;
                }
                else if (primitive.mode == TINYGLTF_MODE_TRIANGLE_FAN) {
                    mode = GL_TRIANGLE_FAN;
                }
                else if (primitive.mode == TINYGLTF_MODE_POINTS) {
                    mode = GL_POINTS;
                }
                else if (primitive.mode == TINYGLTF_MODE_LINE) {
                    mode = GL_LINES;
                }
                else if (primitive.mode == TINYGLTF_MODE_LINE_LOOP) {
                    mode = GL_LINE_LOOP;
                }

                if (primitive.indices < 0)
                {
                    osg::Array* vertices = geom->getVertexArray();
                    if (vertices)
                    {
                        osg::DrawArrays *drawArrays
                            = new osg::DrawArrays(mode, 0, vertices->getNumElements());
                        geom->addPrimitiveSet(drawArrays);
                    }
                    // Otherwise we can't draw anything!
                }
                else
                {
                    const tinygltf::Accessor &indexAccessor = model.accessors[primitive.indices];

                    if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                    {
                        osg::UShortArray* indices = static_cast<osg::UShortArray*>(arrays[primitive.indices].get());
                        osg::DrawElementsUShort* drawElements
                            = new osg::DrawElementsUShort(mode, indices->begin(), indices->end());
                        geom->addPrimitiveSet(drawElements);
                    }
                    else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
                    {
                        osg::UIntArray* indices = static_cast<osg::UIntArray*>(arrays[primitive.indices].get());
                        osg::DrawElementsUInt* drawElements
                            = new osg::DrawElementsUInt(mode, indices->begin(), indices->end());
                        geom->addPrimitiveSet(drawElements);
                    }
                    else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
                    {
                        osg::UByteArray* indices = static_cast<osg::UByteArray*>(arrays[primitive.indices].get());
                        // Sigh, DrawElementsUByte doesn't have the constructor with iterator arguments.
                        osg::DrawElementsUByte* drawElements = new osg::DrawElementsUByte(mode, indexAccessor.count);
                        std::copy(indices->begin(), indices->end(), drawElements->begin());
                        geom->addPrimitiveSet(drawElements);
                    }
                    else
                    {
                        OE_WARN << LC << "primitive indices are not unsigned.\n";
                    }
                }

                if (!env.readOptions || env.readOptions->getOptionString().find("gltfSkipNormals") == std::string::npos)
                {
                    // Generate normals automatically if we're not given any in the file itself.
                    if (!geom->getNormalArray())
                    {
                        osgUtil::SmoothingVisitor sv;
                        geode->accept(sv);
                    }
                }

                osgEarth::Registry::shaderGenerator().run(geom.get());
            }

            return group;
        }

        // Parameterize the creation of OSG arrays from glTF
        // accessors. It's a bit gratuitous to make ComponentType and
        // AccessorType template parameters. The thought was that the
        // memcpy could be optimized if these were constants in the
        // copyData() function, but that's debatable.

        template<typename OSGArray, int ComponentType, int AccessorType>
        class ArrayBuilder
        {
        public:
            static OSGArray* makeArray(unsigned int size)
            {
                return new OSGArray(size);
            }
            static void copyData(OSGArray* dest, const unsigned char* src, size_t viewOffset,
                                 size_t byteStride,  size_t accessorOffset, size_t count)
            {
                int32_t componentSize = tinygltf::GetComponentSizeInBytes(ComponentType);
                int32_t numComponents = tinygltf::GetNumComponentsInType(AccessorType);
                if (byteStride == 0)
                {
                    memcpy(&(*dest)[0], src + accessorOffset + viewOffset, componentSize * numComponents * count);
                }
                else
                {
                    const unsigned char* ptr = src + accessorOffset + viewOffset;
                    for (int i = 0; i < count; ++i, ptr += byteStride)
                    {
                        memcpy(&(*dest)[i], ptr, componentSize * numComponents);
                    }
                }
            }
            static void copyData(OSGArray* dest, const tinygltf::Buffer& buffer, const tinygltf::BufferView& bufferView,
                                 const tinygltf::Accessor& accessor)
            {
                copyData(dest, &buffer.data.at(0), bufferView.byteOffset,
                         bufferView.byteStride, accessor.byteOffset, accessor.count);
            }
            static OSGArray* makeArray(const tinygltf::Buffer& buffer, const tinygltf::BufferView& bufferView,
                                       const tinygltf::Accessor& accessor)
            {
                OSGArray* result = new OSGArray(accessor.count);
                copyData(result, buffer, bufferView, accessor);
                return result;
            }
        };

        // Take all of the accessors and turn them into arrays
        void extractArrays(std::vector<osg::ref_ptr<osg::Array>> &arrays) const
        {
            for (unsigned int i = 0; i < model.accessors.size(); i++)
            {
                const tinygltf::Accessor& accessor = model.accessors[i];
                const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
                const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
                osg::ref_ptr< osg::Array > osgArray;

                switch (accessor.componentType)
                {
                case TINYGLTF_COMPONENT_TYPE_BYTE:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::ByteArray,
                                                TINYGLTF_COMPONENT_TYPE_BYTE,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2bArray,
                                                TINYGLTF_COMPONENT_TYPE_BYTE,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3bArray,
                                                TINYGLTF_COMPONENT_TYPE_BYTE,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4bArray,
                                                TINYGLTF_COMPONENT_TYPE_BYTE,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::UByteArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2ubArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3ubArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4ubArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_SHORT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::ShortArray,
                                                TINYGLTF_COMPONENT_TYPE_SHORT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2sArray,
                                                TINYGLTF_COMPONENT_TYPE_SHORT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3sArray,
                                                TINYGLTF_COMPONENT_TYPE_SHORT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4sArray,
                                                TINYGLTF_COMPONENT_TYPE_SHORT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::UShortArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2usArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3usArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4usArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_INT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::IntArray,
                                                TINYGLTF_COMPONENT_TYPE_INT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2uiArray,
                                                TINYGLTF_COMPONENT_TYPE_INT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3uiArray,
                                                TINYGLTF_COMPONENT_TYPE_INT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4uiArray,
                                                TINYGLTF_COMPONENT_TYPE_INT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::UIntArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2iArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3iArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4iArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_FLOAT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::FloatArray,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2Array,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3Array,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4Array,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                default:
                    break;
                }
                if (osgArray.valid())
                {
                    osgArray->setBinding(osg::Array::BIND_PER_VERTEX);
                    osgArray->setNormalize(accessor.normalized);
                }
                else
                {
                    OSG_DEBUG << "Adding null array for " << i << std::endl;
                }
                arrays.push_back(osgArray);
            }
        }

        static bool null(const tinygltf::Value& val)
        {
            return val.Type() == tinygltf::NULL_TYPE;
        }

        void makeInstancedMeshNode(const tinygltf::Node& node, osg::Group* meshGroup) const
        {
            auto itr = node.extensions.find("EXT_mesh_gpu_instancing");
            if (itr == node.extensions.end() || !itr->second.IsObject())
                return;
            auto& extObj = itr->second;
            auto& attributes = extObj.Get("attributes");
            if (null(attributes))
                return;
            osgEarth::InstanceBuilder builder;
            auto& translations = attributes.Get("TRANSLATION");
            auto& rotations = attributes.Get("ROTATION");
            auto& scales = attributes.Get("SCALE");
            if (!null(translations) && translations.IsInt())
            {
                osg::Vec3Array* array = dynamic_cast<osg::Vec3Array*>(arrays[translations.Get<int>()].get());
                if (array)
                {
                    builder.setPositions(array);
                }
            }
            if (!null(rotations) && rotations.IsInt())
            {
                osg::Vec4Array* array = dynamic_cast<osg::Vec4Array*>(arrays[rotations.Get<int>()].get());
                if (array)
                {
                    builder.setRotations(array);
                }
            }
            if (!null(scales) && scales.IsInt())
            {
                osg::Vec3Array* array = dynamic_cast<osg::Vec3Array*>(arrays[scales.Get<int>()].get());
                if (array)
                {
                    builder.setScales(array);
                }
            }
            for (unsigned int i = 0; i < meshGroup->getNumChildren(); ++i)
            {
                osg::Geode* geode = meshGroup->getChild(i)->asGeode();
                if (!geode)
                    continue;

                for (unsigned int j = 0; j < geode->getNumDrawables(); ++j)
                {
                    osg::Geometry* geom = geode->getDrawable(j)->asGeometry();
                    if (geom)
                    {
                        builder.installInstancing(geom);
                    }
                }
            }

        }
    };
};

#endif // OSGEARTH_GLTF_READER_H
