/* osgEarth
* Copyright 2026 Pelican Mapping
* MIT License
*/
#pragma once

/**
 * glTF 2.0 / 2.1 reader built on cgltf.
 *
 * Output scene graph:
 *
 *   MatrixTransform (root, Y-up to Z-up unless "gltfZUp"; CullFace BACK;
 *     |              the shared standard PBR program from PBRTexture)
 *     +- MatrixTransform | Group (one per glTF node, named after the node)
 *     |    +- Geode (one per mesh, shared by every node that references the mesh)
 *     |         +- Geometry (one per primitive; PBRTexture bound with install():
 *     |                      descriptor on unit 0, maps on the standard units)
 *     +- InstancedExternalNode (one per distinct external asset file and
 *                              winding parity, glTF 2.1 externalAssets)
 *
 * No shaders are generated per model: every material uses the one program
 * that PBRTexture::installProgram() shares, and the ShaderGenerator is told
 * to ignore the graph.
 *
 * Buffers are read once and used in place: GLB payloads, external buffers
 * and decoded EXT_meshopt_compression views are never copied into
 * intermediate containers. Vertex arrays are built directly from accessor
 * memory and cached per accessor so shared attributes stay shared.
 *
 * Images are decoded from memory (PNG/JPEG via stb_image, WebP via libwebp,
 * KTX2 via the Basis plugin) with the first row on top, which is glTF's
 * texture-coordinate convention. Base color textures sample as sRGB.
 *
 * Supported extensions: KHR_mesh_quantization, KHR_texture_basisu,
 * EXT_texture_webp, EXT_meshopt_compression, EXT_mesh_gpu_instancing,
 * KHR_draco_mesh_compression (when built with Draco), and the glTF 2.1 core
 * externalAssets / files properties. Skins, animations, morph targets,
 * cameras and lights are ignored.
 *
 * Known limitations: every PBR map samples TEXCOORD_0 (texCoord indices and
 * KHR_texture_transform are ignored); normal maps are sampled as authored
 * (no normalTexture.scale); metallic/roughness factors written with their
 * default value of 1 behave as if absent; external asset files must be
 * separate glTF/GLB files (no bufferView-embedded files or aliases); and a
 * document loaded as a mirrored external asset uses one ExternalNode per
 * nested external reference instead of instanced batches.
 *
 * Read options (whitespace separated tokens in osgDB::Options::getOptionString):
 *   gltfZUp                            content is already Z-up; no root rotation
 *   gltfDefaultSceneOnly               load only the default scene
 *   gltfParentReversesWinding          the containing transform mirrors geometry
 *   gltfSkipImagery                    do not load any textures
 *   gltfSkipPBRTextures                load base color textures only
 *   gltfSkipNormals                    do not generate missing normals
 *   gltfForceReload                    bypass the shared material cache
 *   gltfDisableExternalAssetInstancing one ExternalNode per external reference
 */

#include <cgltf.h>
#include <stb_image.h>

#ifdef OSGEARTH_GLTF_HAVE_MESHOPT
#include <meshoptimizer.h>
#endif
#ifdef OSGEARTH_GLTF_HAVE_DRACO
#include <draco/compression/decode.h>
#endif
#ifdef OSGEARTH_GLTF_HAVE_WEBP
#include <webp/decode.h>
#endif

#include <osg/CullFace>
#include <osg/FrontFace>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osg/observer_ptr>
#include <osgDB/FileNameUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgUtil/SmoothingVisitor>

#include <osgEarth/ExternalNode>
#include <osgEarth/InstanceBuilder>
#include <osgEarth/InstancedExternalNode>
#include <osgEarth/Notify>
#include <osgEarth/PBRMaterial>
#include <osgEarth/Registry>
#include <osgEarth/ShaderGenerator>
#include <osgEarth/ShaderUtils>
#include <osgEarth/Threading>
#include <osgEarth/URI>
#include <osgEarth/VertexCompression>

#include <array>
#include <cstring>
#include <memory>
#include <sstream>
#include <streambuf>
#include <string>
#include <unordered_map>
#include <vector>

// These extension tokens are absent from some OSG GL headers.
#ifndef GL_COMPRESSED_SRGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT 0x8C4C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif

#undef LC
#define LC "[gltf] "

class GLTFReader
{
public:
    //! Materials whose images all come from external URIs are shared here.
    using TextureCache = osgEarth::Threading::Mutexed<
        std::unordered_map<std::string, osg::observer_ptr<osgEarth::PBRTexture>>>;

    //! Parsed read options.
    struct Flags
    {
        bool zUp = false;
        bool defaultSceneOnly = false;
        bool parentReversesWinding = false;
        bool skipImagery = false;
        bool skipPBRTextures = false;
        bool skipNormals = false;
        bool forceReload = false;
        bool instanceExternalAssets = true;

        static Flags parse(const osgDB::Options* options)
        {
            Flags flags;
            if (!options) return flags;
            std::istringstream in(options->getOptionString());
            std::string token;
            while (in >> token)
            {
                if (token == "gltfZUp") flags.zUp = true;
                else if (token == "gltfDefaultSceneOnly") flags.defaultSceneOnly = true;
                else if (token == "gltfParentReversesWinding") flags.parentReversesWinding = true;
                else if (token == "gltfSkipImagery") flags.skipImagery = true;
                else if (token == "gltfSkipPBRTextures") flags.skipPBRTextures = true;
                else if (token == "gltfSkipNormals") flags.skipNormals = true;
                else if (token == "gltfForceReload") flags.forceReload = true;
                else if (token == "gltfDisableExternalAssetInstancing") flags.instanceExternalAssets = false;
            }
            return flags;
        }
    };

    GLTFReader() = default;

    void setTextureCache(TextureCache* cache) { _textureCache = cache; }

    //! Reads a .gltf or .glb from a file or URL.
    osgDB::ReaderWriter::ReadResult read(const std::string& location, const osgDB::Options* options) const
    {
        osgEarth::ReadResult rr = osgEarth::URI(location).readString(options);
        if (rr.failed())
            return osgDB::ReaderWriter::ReadResult::FILE_NOT_FOUND;
        const std::string& bytes = rr.getString();
        return read(location, bytes.data(), bytes.size(), options);
    }

    /**
     * Reads a .gltf or .glb that is already in memory. "location" is the
     * document's own URI, used to resolve relative references. The memory
     * must stay valid for the duration of the call; nothing is copied.
     */
    osgDB::ReaderWriter::ReadResult read(const std::string& location, const void* bytes, std::size_t size, const osgDB::Options* options) const
    {
        try
        {
            Document document;
            if (!document.load(bytes, size, location, options))
            {
                OE_WARN << LC << "Error loading " << location << ": " << document.error << std::endl;
                return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
            }

            Builder builder(document, location, options, _textureCache);
            osg::ref_ptr<osg::Node> node = builder.build();
            if (!node.valid())
            {
                OE_WARN << LC << "Error loading " << location << ": " << builder.error << std::endl;
                return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
            }
            return osgDB::ReaderWriter::ReadResult(node.release());
        }
        catch (const std::exception& e)
        {
            OE_WARN << LC << "Error loading " << location << ": " << e.what() << std::endl;
            return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
        }
    }

    //! True when the whitespace-separated option string contains the token.
    static bool hasOption(const osgDB::Options* options, const char* token)
    {
        if (!options) return false;
        std::istringstream in(options->getOptionString());
        std::string current;
        while (in >> current)
            if (current == token) return true;
        return false;
    }

    static void appendOption(osgDB::Options* options, const char* token)
    {
        if (hasOption(options, token)) return;
        std::string value = options->getOptionString();
        if (!value.empty() && !std::isspace(static_cast<unsigned char>(value.back()))) value += ' ';
        value += token;
        options->setOptionString(value);
    }

    static void removeOption(osgDB::Options* options, const char* token)
    {
        std::istringstream in(options->getOptionString());
        std::string current, result;
        while (in >> current)
        {
            if (current == token) continue;
            if (!result.empty()) result += ' ';
            result += current;
        }
        options->setOptionString(result);
    }

    /**
     * Resolves a glTF URI reference against the document location. Local
     * references are percent-decoded once so the filesystem sees a native
     * path; remote URLs (including escaped paths and queries) stay intact.
     */
    static std::string resolveURI(const std::string& reference, const std::string& referrer)
    {
        const osgEarth::URIContext context(referrer);
        osgEarth::URI resolved(reference, context);
        if (resolved.isRemote())
            return resolved.full();
        return osgEarth::URI(osgEarth::URI::decodePathEscapes(reference), context).full();
    }

    static bool isDataURI(const char* uri)
    {
        return uri && std::strncmp(uri, "data:", 5) == 0;
    }

    //! Decodes the payload of a base64 data URI.
    static bool decodeDataURI(const char* uri, std::string& out)
    {
        const char* comma = std::strchr(uri, ',');
        if (!comma || comma - uri < 7 || std::strncmp(comma - 7, ";base64", 7) != 0)
            return false;
        const char* b64 = comma + 1;
        const std::size_t length = std::strlen(b64);
        std::size_t padding = 0;
        while (padding < 2 && padding < length && b64[length - 1 - padding] == '=') ++padding;
        const std::size_t size = length * 3 / 4;
        if (size < padding) return false;
        out.resize(size - padding);
        unsigned buffer = 0, bits = 0;
        std::size_t o = 0;
        for (const char* p = b64; o < out.size(); ++p)
        {
            const char ch = *p;
            const int v =
                (ch >= 'A' && ch <= 'Z') ? ch - 'A' :
                (ch >= 'a' && ch <= 'z') ? ch - 'a' + 26 :
                (ch >= '0' && ch <= '9') ? ch - '0' + 52 :
                ch == '+' ? 62 : ch == '/' ? 63 : -1;
            if (v < 0) return false;
            buffer = (buffer << 6) | static_cast<unsigned>(v);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out[o++] = static_cast<char>((buffer >> bits) & 0xFFu);
            }
        }
        return true;
    }

    static const char* resultString(cgltf_result result)
    {
        switch (result)
        {
        case cgltf_result_success: return "success";
        case cgltf_result_data_too_short: return "data too short";
        case cgltf_result_unknown_format: return "unknown format";
        case cgltf_result_invalid_json: return "invalid JSON";
        case cgltf_result_invalid_gltf: return "invalid glTF";
        case cgltf_result_invalid_options: return "invalid options";
        case cgltf_result_file_not_found: return "file not found";
        case cgltf_result_io_error: return "I/O error";
        case cgltf_result_out_of_memory: return "out of memory";
        case cgltf_result_legacy_gltf: return "legacy glTF 1.0 is not supported";
        default: return "unknown error";
        }
    }

    /**
     * A parsed document with all of its buffers resolved and every
     * EXT_meshopt_compression buffer view decoded.
     */
    struct Document
    {
        cgltf_data* data = nullptr;
        std::string error;

        // Externally read buffers, used in place by cgltf until the document dies.
        std::vector<osg::ref_ptr<osg::Object>> storage;

        Document() = default;
        Document(const Document&) = delete;
        Document& operator=(const Document&) = delete;
        ~Document() { if (data) cgltf_free(data); }

        bool fail(const std::string& message) { error = message; return false; }

        bool load(const void* bytes, std::size_t size, const std::string& location, const osgDB::Options* options)
        {
            cgltf_options parseOptions = {};
            const cgltf_result result = cgltf_parse(&parseOptions, bytes, size, &data);
            if (result != cgltf_result_success)
                return fail(std::string("cgltf: ") + resultString(result));

            for (cgltf_size i = 0; i < data->extensions_required_count; ++i)
            {
                const char* name = data->extensions_required[i];
                if (std::strcmp(name, "EXT_meshopt_compression") == 0)
                {
#ifndef OSGEARTH_GLTF_HAVE_MESHOPT
                    return fail("EXT_meshopt_compression is required, but osgEarth was built without meshoptimizer support");
#endif
                    continue;
                }
                if (std::strcmp(name, "KHR_draco_mesh_compression") == 0)
                {
#ifndef OSGEARTH_GLTF_HAVE_DRACO
                    return fail("KHR_draco_mesh_compression is required, but osgEarth was built without Draco support");
#endif
                    continue;
                }
                if (std::strcmp(name, "KHR_mesh_quantization") == 0 || std::strcmp(name, "KHR_texture_basisu") == 0 ||
                    std::strcmp(name, "EXT_texture_webp") == 0 || std::strcmp(name, "EXT_mesh_gpu_instancing") == 0)
                    continue;
                OE_WARN << LC << location << " requires the unsupported extension " << name << "; loading it anyway" << std::endl;
            }

            return loadBuffers(location, options) && validate() && decodeMeshopt() && validateSparseIndices();
        }

        /**
         * Bounds checks for the data this reader touches. cgltf_validate is
         * not used because it also rejects documents for problems in parts
         * that are ignored here (animations, skins, morph targets).
         */
        bool validate()
        {
            for (cgltf_size i = 0; i < data->buffer_views_count; ++i)
            {
                const cgltf_buffer_view& view = data->buffer_views[i];
                const std::string label = "bufferView " + std::to_string(i);
                if (view.size > view.buffer->size || view.offset > view.buffer->size - view.size)
                    return fail(label + " exceeds its buffer");
                if (!view.has_meshopt_compression) continue;

                const cgltf_meshopt_compression& mc = view.meshopt_compression;
                if (mc.count == 0 || mc.stride == 0 || mc.count > std::numeric_limits<std::size_t>::max() / mc.stride ||
                    mc.count * mc.stride < view.size)
                    return fail(label + " has an invalid EXT_meshopt_compression layout");
                if (mc.size > mc.buffer->size || mc.offset > mc.buffer->size - mc.size)
                    return fail(label + " has an EXT_meshopt_compression range outside its buffer");
                const bool attributes = mc.mode == cgltf_meshopt_compression_mode_attributes;
                if (mc.mode == cgltf_meshopt_compression_mode_invalid ||
                    (attributes && (mc.stride % 4 != 0 || mc.stride > 256)) ||
                    (!attributes && mc.stride != 2 && mc.stride != 4) ||
                    (mc.mode == cgltf_meshopt_compression_mode_triangles && mc.count % 3 != 0))
                    return fail(label + " has an unsupported EXT_meshopt_compression mode");
            }

            for (cgltf_size i = 0; i < data->accessors_count; ++i)
            {
                const cgltf_accessor& accessor = data->accessors[i];
                const std::string label = "accessor " + std::to_string(i);
                const std::size_t elementSize = cgltf_calc_size(accessor.type, accessor.component_type);
                if (elementSize == 0)
                    return fail(label + " has an invalid type");
                if (accessor.count > std::numeric_limits<unsigned>::max())
                    return fail(label + " is too large for an OSG array");
                if (accessor.buffer_view && accessor.count > 0)
                {
                    const std::size_t span = accessor.offset + elementSize;
                    if (accessor.count - 1 > (std::numeric_limits<std::size_t>::max() - span) / std::max<std::size_t>(accessor.stride, 1) ||
                        span + accessor.stride * (accessor.count - 1) > accessor.buffer_view->size)
                        return fail(label + " exceeds its bufferView");
                }
                if (accessor.is_sparse)
                {
                    const cgltf_accessor_sparse& sparse = accessor.sparse;
                    const std::size_t indexSize = cgltf_component_size(sparse.indices_component_type);
                    if (indexSize == 0 || sparse.indices_component_type == cgltf_component_type_r_8 ||
                        sparse.indices_component_type == cgltf_component_type_r_16 || sparse.indices_component_type == cgltf_component_type_r_32f ||
                        sparse.count > sparse.indices_buffer_view->size / indexSize ||
                        sparse.indices_byte_offset > sparse.indices_buffer_view->size - sparse.count * indexSize ||
                        sparse.count > sparse.values_buffer_view->size / elementSize ||
                        sparse.values_byte_offset > sparse.values_buffer_view->size - sparse.count * elementSize)
                        return fail(label + " has invalid sparse storage");
                }
            }
            return true;
        }

        //! Sparse indices address the accessor's own elements; cgltf writes through them unchecked.
        bool validateSparseIndices()
        {
            for (cgltf_size i = 0; i < data->accessors_count; ++i)
            {
                const cgltf_accessor& accessor = data->accessors[i];
                if (!accessor.is_sparse) continue;
                const cgltf_accessor_sparse& sparse = accessor.sparse;
                const uint8_t* indices = cgltf_buffer_view_data(sparse.indices_buffer_view);
                if (!indices)
                    return fail("accessor " + std::to_string(i) + " has no sparse index data");
                indices += sparse.indices_byte_offset;
                const std::size_t indexSize = cgltf_component_size(sparse.indices_component_type);
                for (cgltf_size k = 0; k < sparse.count; ++k)
                {
                    std::size_t index = 0;
                    if (indexSize == 1) index = indices[k];
                    else if (indexSize == 2) { std::uint16_t v; std::memcpy(&v, indices + k * 2, 2); index = v; }
                    else { std::uint32_t v; std::memcpy(&v, indices + k * 4, 4); index = v; }
                    if (index >= accessor.count)
                        return fail("accessor " + std::to_string(i) + " has a sparse index outside the accessor");
                }
            }
            return true;
        }

        bool loadBuffers(const std::string& location, const osgDB::Options* options)
        {
            for (cgltf_size i = 0; i < data->buffers_count; ++i)
            {
                cgltf_buffer& buffer = data->buffers[i];
                if (buffer.data) continue;

                if (!buffer.uri)
                {
                    // The GLB binary chunk backs the first URI-less buffer. Any other
                    // URI-less buffer is an EXT_meshopt_compression fallback and stays empty.
                    if (i == 0 && data->bin)
                    {
                        if (data->bin_size < buffer.size)
                            return fail("GLB binary chunk is shorter than buffer 0");
                        buffer.data = const_cast<void*>(data->bin);
                        buffer.data_free_method = cgltf_data_free_method_none;
                    }
                    continue;
                }

                if (isDataURI(buffer.uri))
                {
                    std::string payload;
                    if (!decodeDataURI(buffer.uri, payload) || payload.size() < buffer.size)
                        return fail("buffer " + std::to_string(i) + " has an invalid data URI");
                    osg::ref_ptr<osgEarth::StringObject> decoded = new osgEarth::StringObject(std::move(payload));
                    buffer.data = const_cast<char*>(decoded->getString().data());
                    buffer.data_free_method = cgltf_data_free_method_none;
                    storage.emplace_back(decoded);
                    continue;
                }

                const std::string uri = resolveURI(buffer.uri, location);
                osgEarth::ReadResult rr = osgEarth::URI(uri).readString(options);
                if (rr.failed())
                    return fail("cannot read buffer " + uri + " (" + osgEarth::ReadResult::getResultCodeString(rr.code()) + ")");
                if (rr.getString().size() < buffer.size)
                    return fail("buffer " + uri + " is shorter than declared");
                buffer.data = const_cast<char*>(rr.getString().data());
                buffer.data_free_method = cgltf_data_free_method_none;
                storage.emplace_back(rr.getObject());
            }
            return true;
        }

        bool decodeMeshopt()
        {
            for (cgltf_size i = 0; i < data->buffer_views_count; ++i)
            {
                cgltf_buffer_view& view = data->buffer_views[i];
                if (!view.has_meshopt_compression) continue;
                const cgltf_meshopt_compression& mc = view.meshopt_compression;
#ifdef OSGEARTH_GLTF_HAVE_MESHOPT
                if (!mc.buffer->data)
                    return fail("EXT_meshopt_compression source buffer of bufferView " + std::to_string(i) + " is not loaded");
                const std::size_t decodedSize = mc.count * mc.stride;
                void* decoded = data->memory.alloc_func(data->memory.user_data, decodedSize);
                if (!decoded)
                    return fail("out of memory decoding bufferView " + std::to_string(i));
                const unsigned char* source = static_cast<const unsigned char*>(mc.buffer->data) + mc.offset;
                int rc = -1;
                switch (mc.mode)
                {
                case cgltf_meshopt_compression_mode_attributes:
                    rc = meshopt_decodeVertexBuffer(decoded, mc.count, mc.stride, source, mc.size); break;
                case cgltf_meshopt_compression_mode_triangles:
                    rc = meshopt_decodeIndexBuffer(decoded, mc.count, mc.stride, source, mc.size); break;
                case cgltf_meshopt_compression_mode_indices:
                    rc = meshopt_decodeIndexSequence(decoded, mc.count, mc.stride, source, mc.size); break;
                default: break;
                }
                if (rc != 0)
                {
                    data->memory.free_func(data->memory.user_data, decoded);
                    return fail("meshoptimizer failed to decode bufferView " + std::to_string(i));
                }
                switch (mc.filter)
                {
                case cgltf_meshopt_compression_filter_octahedral: meshopt_decodeFilterOct(decoded, mc.count, mc.stride); break;
                case cgltf_meshopt_compression_filter_quaternion: meshopt_decodeFilterQuat(decoded, mc.count, mc.stride); break;
                case cgltf_meshopt_compression_filter_exponential: meshopt_decodeFilterExp(decoded, mc.count, mc.stride); break;
                case cgltf_meshopt_compression_filter_color:
#if defined(MESHOPTIMIZER_VERSION) && MESHOPTIMIZER_VERSION >= 230
                    meshopt_decodeFilterColor(decoded, mc.count, mc.stride);
                    break;
#else
                    data->memory.free_func(data->memory.user_data, decoded);
                    return fail("bufferView " + std::to_string(i) + " uses the EXT_meshopt_compression COLOR filter, which needs meshoptimizer 0.23 or newer");
#endif
                default: break;
                }
                view.data = decoded; // freed by cgltf_free
#else
                // Without meshoptimizer the view must carry its uncompressed fallback data.
                if (!view.buffer->data)
                    return fail("bufferView " + std::to_string(i) + " requires EXT_meshopt_compression, which is unavailable in this build");
#endif
            }
            return true;
        }
    };

    //! Read-only std::streambuf over memory for OSG image plugins.
    struct MemoryStreamBuffer : public std::streambuf
    {
        MemoryStreamBuffer(const void* data, std::size_t size)
        {
            char* begin = const_cast<char*>(static_cast<const char*>(data));
            setg(begin, begin, begin + size);
        }
        pos_type seekoff(off_type offset, std::ios_base::seekdir dir, std::ios_base::openmode which) override
        {
            if (!(which & std::ios_base::in)) return pos_type(off_type(-1));
            char* target = dir == std::ios_base::beg ? eback() + offset :
                dir == std::ios_base::cur ? gptr() + offset : egptr() + offset;
            if (target < eback() || target > egptr()) return pos_type(off_type(-1));
            setg(eback(), target, egptr());
            return pos_type(target - eback());
        }
        pos_type seekpos(pos_type position, std::ios_base::openmode which) override
        {
            return seekoff(off_type(position), std::ios_base::beg, which);
        }
    };

    // OSG 3.6 cannot size sRGB S3TC formats when uploading Texture2D images.
    // Keep the linear BC pixel format for block sizing and the sRGB internal
    // format for GPU sampling. TextureArena handles its own uploads separately.
    struct CompressedSRGBUpload : osg::Texture2D::SubloadCallback
    {
        static unsigned mipLevels(const osg::Texture2D& texture)
        {
            const auto* image = texture.getImage();
            const auto filter = texture.getFilter(osg::Texture::MIN_FILTER);
            return !image->isMipmap() && filter != osg::Texture::LINEAR && filter != osg::Texture::NEAREST ?
                osg::Image::computeNumberOfMipmapLevels(image->s(), image->t()) : image->getNumMipmapLevels();
        }

        bool textureObjectValid(const osg::Texture2D& texture, osg::State& state) const override
        {
            const auto* image = texture.getImage();
            auto* object = texture.getTextureObject(state.getContextID());
            return image && object && object->match(GL_TEXTURE_2D, mipLevels(texture),
                texture.getInternalFormat(), image->s(), image->t(), 1, 0);
        }

        void load(const osg::Texture2D& texture, osg::State& state) const override
        {
            const auto* image = texture.getImage();
            if (!image || !image->data()) return;
            auto* ext = state.get<osg::GLExtensions>();
            if (!ext->glCompressedTexImage2D) return;
            state.unbindPixelBufferObject();
            int width = image->s(), height = image->t();
            for (unsigned level = 0; level < image->getNumMipmapLevels(); ++level)
            {
                GLint blockSize, size;
                osg::Texture::getCompressedSize(image->getPixelFormat(), width, height, 1, blockSize, size);
                ext->glCompressedTexImage2D(GL_TEXTURE_2D, level, texture.getInternalFormat(),
                    width, height, 0, size, image->getMipmapData(level));
                width = std::max(1, width / 2);
                height = std::max(1, height / 2);
            }
            const auto levels = mipLevels(texture);
            if (levels > image->getNumMipmapLevels() && ext->glGenerateMipmap)
                ext->glGenerateMipmap(GL_TEXTURE_2D);
            texture.setTextureSize(image->s(), image->t());
            texture.setNumMipmapLevels(levels);
            texture.getModifiedCount(state.getContextID()) = image->getModifiedCount();
        }

        void subload(const osg::Texture2D& texture, osg::State& state) const override
        {
            if (texture.getImage() && texture.isDirty(state.getContextID()))
                load(texture, state);
        }
    };

    /**
     * Builds the OSG scene graph for one parsed document.
     */
    class Builder
    {
    public:
        std::string error;

        Builder(const Document& document, const std::string& referrer, const osgDB::Options* options, TextureCache* textureCache) :
            _data(document.data),
            _referrer(referrer),
            _options(options),
            _flags(Flags::parse(options)),
            _textureCache(textureCache)
        {
            _arrays.resize(_data->accessors_count);
            _meshes.resize(_data->meshes_count);
            _materials.resize(_data->materials_count);
            _materialsBuilt.assign(_data->materials_count, 0);
            _images.resize(_data->images_count);
            _imagesTried.assign(_data->images_count, 0);
            _imageURIs.resize(_data->images_count);
            _imageURIsResolved.assign(_data->images_count, 0);
            _fileNames.resize(_data->files_count);
            _fileBatches.assign(_data->files_count, { -1, -1 });
            for (cgltf_size i = 0; i < _data->extensions_required_count; ++i)
                if (std::strcmp(_data->extensions_required[i], "KHR_texture_basisu") == 0)
                    _basisRequired = true;
        }

        osg::ref_ptr<osg::Node> build()
        {
            osg::ref_ptr<osg::MatrixTransform> root = new osg::MatrixTransform();
            root->setName(osgDB::getSimpleFileName(_referrer));
            if (!_flags.zUp)
                root->setMatrix(osg::Matrixd::rotate(osg::Vec3d(0.0, 1.0, 0.0), osg::Vec3d(0.0, 0.0, 1.0)));
            root->getOrCreateStateSet()->setAttributeAndModes(
                new osg::CullFace(osg::CullFace::BACK), osg::StateAttribute::ON);

            // Every material is bound for the one shared PBR program, so
            // nothing is generated per model and later ShaderGenerator passes
            // must leave this graph alone. Colors are linear throughout.
            osgEarth::PBRTexture::installProgram(root->getStateSet());
            osgEarth::ShaderGenerator::setIgnoreHint(root.get(), true);
            root->setUserValue(SHADERGEN_HINT_LINEAR_COLOR, true);

            auto addScene = [&](const cgltf_scene& scene)
            {
                for (cgltf_size i = 0; i < scene.nodes_count && error.empty(); ++i)
                {
                    osg::ref_ptr<osg::Node> node = buildNode(
                        *scene.nodes[i], osg::Matrixd::identity(), _flags.parentReversesWinding, 0u);
                    if (node.valid()) root->addChild(node.get());
                }
            };

            if (_flags.defaultSceneOnly)
            {
                if (_data->scene) addScene(*_data->scene);
                else if (_data->scenes_count > 0) addScene(_data->scenes[0]);
            }
            else
            {
                for (cgltf_size i = 0; i < _data->scenes_count; ++i)
                    addScene(_data->scenes[i]);
            }
            if (!error.empty()) return {};

            for (const ExternalBatch& batch : _batches)
            {
                osg::ref_ptr<osgEarth::InstancedExternalNode> node = new osgEarth::InstancedExternalNode(
                    batch.filename, batch.matrices, externalOptions(batch.reversed));
                node->setName(batch.name);
                if (!node->isLoaded())
                {
                    error = node->getLastError();
                    if (error.empty()) error = "Failed to load external asset " + batch.filename;
                    return {};
                }
                root->addChild(node.get());
            }

            return root;
        }

    private:
        struct ExternalBatch
        {
            std::string filename;
            std::string name;
            bool reversed = false;
            osgEarth::InstancedExternalNode::MatrixList matrices;
        };

        struct PrimitiveArrays
        {
            osg::ref_ptr<osg::Vec3Array> position, normal;
            osg::ref_ptr<osg::Vec2Array> texcoord[2];
            osg::ref_ptr<osg::Array> color; // Vec4ubArray or Vec4Array
            osg::ref_ptr<osg::PrimitiveSet> primitive;
        };

        const cgltf_data* _data;
        const std::string& _referrer;
        const osgDB::Options* _options;
        const Flags _flags;
        TextureCache* _textureCache;
        bool _basisRequired = false;

        std::vector<osg::ref_ptr<osg::Array>> _arrays;            // per accessor
        std::vector<osg::ref_ptr<osg::Node>> _meshes;             // per mesh (non-instanced)
        std::vector<osg::ref_ptr<osgEarth::PBRTexture>> _materials; // per material
        std::vector<char> _materialsBuilt;
        std::vector<osg::ref_ptr<osg::Image>> _images;            // per image
        std::vector<char> _imagesTried;
        std::vector<std::string> _imageURIs;                      // resolved external image URIs
        std::vector<char> _imageURIsResolved;
        std::vector<std::string> _fileNames;                      // resolved external asset files
        std::vector<std::array<int, 2>> _fileBatches;             // per file, per winding parity
        std::vector<ExternalBatch> _batches;
        osg::ref_ptr<const osgDB::Options> _externalOptions[2];
        osg::ref_ptr<osgDB::Options> _basisOptions;
        osg::ref_ptr<osgEarth::PBRTexture> _defaultMaterial;

        // ------------------------------------------------------------ nodes

        static osg::Matrixd localMatrix(const cgltf_node& node)
        {
            cgltf_float m[16];
            cgltf_node_transform_local(&node, m);
            return osg::Matrixd(m);
        }

        static bool reversesWinding(const osg::Matrixd& m)
        {
            const double det =
                m(0, 0) * (m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1)) -
                m(0, 1) * (m(1, 0) * m(2, 2) - m(1, 2) * m(2, 0)) +
                m(0, 2) * (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0));
            return det < 0.0;
        }

        /**
         * Builds the subtree for a node. Returns null when the node produced no
         * OSG content because everything in it went into external asset batches.
         */
        osg::ref_ptr<osg::Node> buildNode(const cgltf_node& node, const osg::Matrixd& parentWorld, bool parentReversed, unsigned depth)
        {
            if (depth > _data->nodes_count)
            {
                error = "node hierarchy contains a cycle";
                return {};
            }
            const osg::Matrixd local = localMatrix(node);
            const bool localReversed = reversesWinding(local);
            const bool reversed = parentReversed != localReversed;
            const osg::Matrixd world = local * parentWorld;

            osg::ref_ptr<osg::Group> group;
            auto container = [&]() -> osg::Group*
            {
                if (!group.valid())
                {
                    group = local.isIdentity() ? new osg::Group() : new osg::MatrixTransform(local);
                    if (localReversed)
                        group->getOrCreateStateSet()->setAttribute(new osg::FrontFace(
                            reversed ? osg::FrontFace::CLOCKWISE : osg::FrontFace::COUNTER_CLOCKWISE));
                }
                return group.get();
            };

            if (node.mesh)
            {
                osg::ref_ptr<osg::Node> mesh = node.has_mesh_gpu_instancing ? buildInstancedMesh(node) : meshNode(*node.mesh);
                if (mesh.valid()) container()->addChild(mesh.get());
            }

            bool consumed = false;
            for (cgltf_size i = 0; i < node.children_count; ++i)
            {
                osg::ref_ptr<osg::Node> child = buildNode(*node.children[i], world, reversed, depth + 1u);
                if (!error.empty()) return {};
                if (child.valid()) container()->addChild(child.get());
                else consumed = true;
            }

            if (node.external_asset)
            {
                if (node.mesh)
                {
                    error = "node \"" + std::string(node.name ? node.name : "") + "\" cannot contain both a mesh and an externalAsset";
                    return {};
                }
                // A document that is itself a mirrored external asset keeps
                // per-reference nodes: InstancedExternalNode derives winding
                // from its own matrices and cannot see the container's mirror.
                if (_flags.instanceExternalAssets && !_flags.parentReversesWinding)
                {
                    if (!addExternalInstance(*node.external_asset, world, reversed)) return {};
                    consumed = true;
                }
                else
                {
                    osg::ref_ptr<osgEarth::ExternalNode> external = externalNode(*node.external_asset, reversed);
                    if (!external.valid()) return {};
                    container()->addChild(external.get());
                }
            }

            if (!group.valid() && consumed)
                return {};

            osg::ref_ptr<osg::Node> result = container();
            result->setName(node.name ? node.name : "");
            return result;
        }

        // -------------------------------------------------- external assets

        const osgDB::Options* externalOptions(bool reversed)
        {
            auto& cached = _externalOptions[reversed ? 1 : 0];
            if (!cached.valid())
            {
                osg::ref_ptr<osgDB::Options> options = osgEarth::Registry::cloneOrCreateOptions(_options);
                // This root supplies the Y-up to Z-up conversion; nested assets
                // contribute only their default scene in glTF space.
                appendOption(options.get(), "gltfZUp");
                appendOption(options.get(), "gltfDefaultSceneOnly");
                removeOption(options.get(), "gltfForceReload");
                // Absolute FrontFace state inside the asset depends on the
                // cumulative parity, which also identifies its shared cache variant.
                if (reversed) appendOption(options.get(), "gltfParentReversesWinding");
                else removeOption(options.get(), "gltfParentReversesWinding");
                cached = options;
            }
            return cached.get();
        }

        //! Resolves and validates the file behind an external asset; empty on error.
        const std::string& externalFileName(const cgltf_external_asset& asset)
        {
            const cgltf_file& file = *asset.file;
            std::string& name = _fileNames[cgltf_file_index(_data, &file)];
            if (!name.empty()) return name;

            const std::string label = "files[" + std::to_string(cgltf_file_index(_data, &file)) + "]";
            if (file.buffer_view || !file.uri)
                error = label + ": embedded external asset files are not supported; a uri is required";
            else if (isDataURI(file.uri))
                error = label + ": data URI external asset files are not supported";
            else if (!file.mime_type ||
                (std::strcmp(file.mime_type, "model/gltf-binary") != 0 && std::strcmp(file.mime_type, "model/gltf+json") != 0))
                error = label + " referenced as an external asset must use a glTF mimeType";
            else
                name = resolveURI(file.uri, _referrer);
            return name;
        }

        bool addExternalInstance(const cgltf_external_asset& asset, const osg::Matrixd& world, bool reversed)
        {
            const std::string& filename = externalFileName(asset);
            if (filename.empty()) return false;

            int& batchIndex = _fileBatches[cgltf_file_index(_data, asset.file)][reversed ? 1 : 0];
            if (batchIndex < 0)
            {
                // Distinct file entries may still name the same asset.
                for (std::size_t i = 0; i < _batches.size() && batchIndex < 0; ++i)
                    if (_batches[i].reversed == reversed && _batches[i].filename == filename)
                        batchIndex = static_cast<int>(i);
                if (batchIndex < 0)
                {
                    batchIndex = static_cast<int>(_batches.size());
                    _batches.emplace_back();
                    _batches.back().filename = filename;
                    _batches.back().name = asset.name ? asset.name : "";
                    _batches.back().reversed = reversed;
                }
            }
            _batches[batchIndex].matrices.emplace_back(world);
            return true;
        }

        osg::ref_ptr<osgEarth::ExternalNode> externalNode(const cgltf_external_asset& asset, bool reversed)
        {
            const std::string& filename = externalFileName(asset);
            if (filename.empty()) return {};
            osg::ref_ptr<osgEarth::ExternalNode> node = new osgEarth::ExternalNode(filename, externalOptions(reversed));
            node->setName(asset.name ? asset.name : "");
            if (!node->isLoaded())
            {
                error = node->getLastError();
                if (error.empty()) error = "Failed to load external asset " + filename;
                return {};
            }
            return node;
        }

        // ------------------------------------------------------------ meshes

        static GLenum primitiveMode(cgltf_primitive_type type)
        {
            switch (type)
            {
            case cgltf_primitive_type_points: return GL_POINTS;
            case cgltf_primitive_type_lines: return GL_LINES;
            case cgltf_primitive_type_line_loop: return GL_LINE_LOOP;
            case cgltf_primitive_type_line_strip: return GL_LINE_STRIP;
            case cgltf_primitive_type_triangles: return GL_TRIANGLES;
            case cgltf_primitive_type_triangle_strip: return GL_TRIANGLE_STRIP;
            case cgltf_primitive_type_triangle_fan: return GL_TRIANGLE_FAN;
            default: return GL_NONE;
            }
        }

        osg::ref_ptr<osg::Node> meshNode(const cgltf_mesh& mesh)
        {
            auto& cached = _meshes[cgltf_mesh_index(_data, &mesh)];
            if (!cached.valid())
            {
                osg::ref_ptr<osg::Geode> geode = new osg::Geode();
                geode->setName(mesh.name ? mesh.name : "");
                for (cgltf_size i = 0; i < mesh.primitives_count; ++i)
                    buildPrimitive(mesh.primitives[i], false, geode.get());
                cached = geode;
            }
            return cached;
        }

        //! EXT_mesh_gpu_instancing: a private geode whose geometries carry instance attributes.
        osg::ref_ptr<osg::Node> buildInstancedMesh(const cgltf_node& node)
        {
            osg::ref_ptr<osg::Vec3Array> positions, scales;
            osg::ref_ptr<osg::Vec4Array> rotations;
            const cgltf_mesh_gpu_instancing& instancing = node.mesh_gpu_instancing;
            for (cgltf_size i = 0; i < instancing.attributes_count; ++i)
            {
                const cgltf_attribute& attribute = instancing.attributes[i];
                if (!attribute.name) continue;
                if (std::strcmp(attribute.name, "TRANSLATION") == 0) positions = floatArray<osg::Vec3Array>(attribute.data, 3);
                else if (std::strcmp(attribute.name, "ROTATION") == 0) rotations = floatArray<osg::Vec4Array>(attribute.data, 4);
                else if (std::strcmp(attribute.name, "SCALE") == 0) scales = floatArray<osg::Vec3Array>(attribute.data, 3);
            }
            const std::size_t count = positions.valid() ? positions->size() :
                rotations.valid() ? rotations->size() : scales.valid() ? scales->size() : 0u;
            if (count == 0)
                return meshNode(*node.mesh);
            if (!positions.valid())
                positions = new osg::Vec3Array(static_cast<unsigned>(count));

            osg::ref_ptr<osg::Geode> geode = new osg::Geode();
            geode->setName(node.mesh->name ? node.mesh->name : "");
            for (cgltf_size i = 0; i < node.mesh->primitives_count; ++i)
                buildPrimitive(node.mesh->primitives[i], true, geode.get());

            osgEarth::InstanceBuilder builder;
            builder.setPositions(positions.get());
            if (rotations.valid()) builder.setRotations(rotations.get());
            if (scales.valid()) builder.setScales(scales.get());
            for (unsigned i = 0; i < geode->getNumDrawables(); ++i)
                if (osg::Geometry* geometry = geode->getDrawable(i)->asGeometry())
                    builder.installInstancing(geometry);
            return geode;
        }

        void buildPrimitive(const cgltf_primitive& primitive, bool instanced, osg::Geode* geode)
        {
            const GLenum mode = primitiveMode(primitive.type);
            if (mode == GL_NONE)
            {
                OE_WARN << LC << "Skipping primitive with unsupported mode" << std::endl;
                return;
            }

            osg::ref_ptr<osg::Geometry> geometry = instanced ? osgEarth::InstanceBuilder::createGeometry() : new osg::Geometry();
            geometry->setName(geode->getName());
            geometry->setUseVertexBufferObjects(true);
            geometry->setUserValue(SHADERGEN_HINT_LINEAR_COLOR, true);

            osg::Vec4 baseColor(1.0f, 1.0f, 1.0f, 1.0f);
            osg::StateSet* stateSet = geometry->getOrCreateStateSet();
            bool bound = false;
            if (primitive.material)
                bound = applyMaterial(*primitive.material, stateSet, baseColor);
            // The shared program always samples an albedo; untextured
            // primitives get a white one.
            if (!bound)
                defaultMaterial()->install(stateSet);

            PrimitiveArrays arrays;
            bool ok = false;
#ifdef OSGEARTH_GLTF_HAVE_DRACO
            if (primitive.has_draco_mesh_compression)
                ok = decodeDraco(primitive, mode, baseColor, arrays);
            else
#endif
                ok = readPrimitive(primitive, mode, baseColor, arrays);
            if (!ok || !arrays.position.valid() || !arrays.primitive.valid())
            {
                OE_WARN << LC << "Skipping primitive without usable geometry in mesh \"" << geode->getName() << "\"" << std::endl;
                return;
            }

            geometry->setVertexArray(arrays.position.get());
            if (arrays.normal.valid()) geometry->setNormalArray(arrays.normal.get(), osg::Array::BIND_PER_VERTEX);
            if (arrays.texcoord[0].valid()) geometry->setTexCoordArray(0, arrays.texcoord[0].get());
            if (arrays.texcoord[1].valid()) geometry->setTexCoordArray(1, arrays.texcoord[1].get());
            if (!arrays.color.valid())
            {
                // A per-vertex color is required by the shader path; carry the base color factor in it.
                osg::ref_ptr<osg::Vec4ubArray> constant = new osg::Vec4ubArray();
                constant->setNormalize(true);
                constant->assign(arrays.position->size(), osgEarth::packColor(baseColor));
                arrays.color = constant;
            }
            geometry->setColorArray(arrays.color.get(), osg::Array::BIND_PER_VERTEX);
            geometry->addPrimitiveSet(arrays.primitive.get());

            const bool triangles = mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN;
            if (!arrays.normal.valid() && triangles && !_flags.skipNormals)
                osgUtil::SmoothingVisitor::smooth(*geometry);

            geode->addDrawable(geometry.get());
        }

        bool readPrimitive(const cgltf_primitive& primitive, GLenum mode, const osg::Vec4& baseColor, PrimitiveArrays& out)
        {
            for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
            {
                const cgltf_attribute& attribute = primitive.attributes[i];
                switch (attribute.type)
                {
                case cgltf_attribute_type_position:
                    out.position = floatArray<osg::Vec3Array>(attribute.data, 3); break;
                case cgltf_attribute_type_normal:
                    out.normal = floatArray<osg::Vec3Array>(attribute.data, 3); break;
                case cgltf_attribute_type_texcoord:
                    if (attribute.index >= 0 && attribute.index < 2)
                        out.texcoord[attribute.index] = floatArray<osg::Vec2Array>(attribute.data, 2);
                    break;
                case cgltf_attribute_type_color:
                    if (attribute.index == 0) out.color = colorArray(attribute.data, baseColor);
                    break;
                default: break;
                }
            }
            if (!out.position.valid()) return false;

            if (primitive.indices)
                out.primitive = drawElements(*primitive.indices, mode, out.position->size());
            else
                out.primitive = new osg::DrawArrays(mode, 0, static_cast<GLsizei>(wholePrimitives(mode, out.position->size())));
            return out.primitive.valid();
        }

        //! Index count rounded down to whole primitives so OSG never reads past the array.
        static std::size_t wholePrimitives(GLenum mode, std::size_t count)
        {
            if (mode == GL_TRIANGLES) return count - count % 3;
            if (mode == GL_LINES) return count - count % 2;
            return count;
        }

        //! Copies indices into a DrawElements set and rejects any that exceed the vertex count.
        template<typename ElementsT>
        osg::ref_ptr<osg::PrimitiveSet> unpackIndices(const cgltf_accessor& accessor, GLenum mode, std::size_t count, std::size_t vertexCount)
        {
            osg::ref_ptr<ElementsT> elements = new ElementsT(mode, static_cast<unsigned>(count));
            if (count == 0) return elements;
            if (cgltf_accessor_unpack_indices(&accessor, &(*elements)[0], sizeof((*elements)[0]), count) != count)
            {
                OE_WARN << LC << "Index accessor has no readable data" << std::endl;
                return {};
            }
            for (const auto index : *elements)
            {
                if (static_cast<std::size_t>(index) >= vertexCount)
                {
                    OE_WARN << LC << "Index " << index << " exceeds the vertex count " << vertexCount << std::endl;
                    return {};
                }
            }
            return elements;
        }

        osg::ref_ptr<osg::PrimitiveSet> drawElements(const cgltf_accessor& accessor, GLenum mode, std::size_t vertexCount)
        {
            const std::size_t count = wholePrimitives(mode, accessor.count);
            switch (accessor.component_type)
            {
            case cgltf_component_type_r_8u: return unpackIndices<osg::DrawElementsUByte>(accessor, mode, count, vertexCount);
            case cgltf_component_type_r_16u: return unpackIndices<osg::DrawElementsUShort>(accessor, mode, count, vertexCount);
            case cgltf_component_type_r_32u: return unpackIndices<osg::DrawElementsUInt>(accessor, mode, count, vertexCount);
            default:
                OE_WARN << LC << "Unsupported index component type" << std::endl;
                return {};
            }
        }

        /**
         * Returns the accessor as a float vector array with the given number of
         * components. Float accessors are copied directly; quantized
         * (KHR_mesh_quantization) and sparse accessors are unpacked. Results are
         * cached per accessor so primitives sharing an accessor share the array.
         */
        template<typename ArrayT>
        osg::ref_ptr<ArrayT> floatArray(const cgltf_accessor* accessor, unsigned components)
        {
            if (!accessor) return {};
            auto& slot = _arrays[cgltf_accessor_index(_data, accessor)];
            if (slot.valid())
            {
                if (auto* typed = dynamic_cast<ArrayT*>(slot.get())) return typed;
            }
            if (cgltf_num_components(accessor->type) != components)
            {
                OE_WARN << LC << "Accessor has " << cgltf_num_components(accessor->type)
                    << " components where " << components << " are expected" << std::endl;
                return {};
            }

            osg::ref_ptr<ArrayT> array = new ArrayT(static_cast<unsigned>(accessor->count));
            const std::size_t floats = accessor->count * components;
            if (floats > 0)
            {
                float* destination = reinterpret_cast<float*>(&(*array)[0]);
                const std::size_t elementSize = components * sizeof(float);
                const uint8_t* source =
                    accessor->component_type == cgltf_component_type_r_32f && !accessor->is_sparse && accessor->buffer_view ?
                    cgltf_buffer_view_data(accessor->buffer_view) : nullptr;
                if (source)
                {
                    source += accessor->offset;
                    if (accessor->stride == elementSize)
                        std::memcpy(destination, source, floats * sizeof(float));
                    else
                        for (cgltf_size i = 0; i < accessor->count; ++i)
                            std::memcpy(destination + i * components, source + i * accessor->stride, elementSize);
                }
                else if (cgltf_accessor_unpack_floats(accessor, destination, floats) != floats)
                {
                    OE_WARN << LC << "Accessor has no readable data" << std::endl;
                    return {};
                }
                else if (accessor->normalized &&
                    (accessor->component_type == cgltf_component_type_r_8 || accessor->component_type == cgltf_component_type_r_16))
                {
                    // glTF maps the most negative signed integer to -1 as well.
                    for (std::size_t i = 0; i < floats; ++i)
                        destination[i] = std::max(destination[i], -1.0f);
                }
            }
            slot = array;
            return array;
        }

        /**
         * COLOR_0 multiplied by the material base color factor. Byte sources stay
         * packed as normalized bytes; float and short sources keep float precision
         * since vertex colors are linear and the shader encodes them later.
         */
        osg::ref_ptr<osg::Array> colorArray(const cgltf_accessor* accessor, const osg::Vec4& factor)
        {
            if (!accessor) return {};
            const unsigned components = static_cast<unsigned>(cgltf_num_components(accessor->type));
            if (components != 3 && components != 4) return {};
            const bool identity = factor == osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
            const bool bytes = accessor->component_type == cgltf_component_type_r_8u;
            auto& slot = _arrays[cgltf_accessor_index(_data, accessor)];
            if (identity && slot.valid())
            {
                if (bytes ? dynamic_cast<osg::Vec4ubArray*>(slot.get()) != nullptr : dynamic_cast<osg::Vec4Array*>(slot.get()) != nullptr)
                    return slot;
            }

            const unsigned count = static_cast<unsigned>(accessor->count);
            osg::ref_ptr<osg::Array> result;
            if (bytes)
            {
                osg::ref_ptr<osg::Vec4ubArray> array = new osg::Vec4ubArray(count);
                array->setNormalize(true);
                const uint8_t* source = identity && components == 4 && accessor->normalized && !accessor->is_sparse && accessor->buffer_view ?
                    cgltf_buffer_view_data(accessor->buffer_view) : nullptr;
                if (source && count > 0)
                {
                    source += accessor->offset;
                    if (accessor->stride == 4)
                        std::memcpy(&(*array)[0], source, count * 4u);
                    else
                        for (unsigned i = 0; i < count; ++i)
                            std::memcpy(&(*array)[i], source + i * accessor->stride, 4);
                }
                else if (count > 0)
                {
                    std::vector<float> values(count * components);
                    if (cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) != values.size())
                        return {};
                    for (unsigned i = 0; i < count; ++i)
                    {
                        const float* v = values.data() + i * components;
                        (*array)[i] = osgEarth::packColor(osg::Vec4(v[0] * factor.r(), v[1] * factor.g(), v[2] * factor.b(),
                            (components == 4 ? v[3] : 1.0f) * factor.a()));
                    }
                }
                result = array;
            }
            else
            {
                osg::ref_ptr<osg::Vec4Array> array = new osg::Vec4Array(count);
                if (count > 0)
                {
                    std::vector<float> values(count * components);
                    if (cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) != values.size())
                        return {};
                    for (unsigned i = 0; i < count; ++i)
                    {
                        const float* v = values.data() + i * components;
                        (*array)[i].set(v[0] * factor.r(), v[1] * factor.g(), v[2] * factor.b(),
                            (components == 4 ? v[3] : 1.0f) * factor.a());
                    }
                }
                result = array;
            }
            if (identity) slot = result;
            return result;
        }

#ifdef OSGEARTH_GLTF_HAVE_DRACO
        /**
         * Draco converts attributes flagged as normalized to [0,1] / [-1,1]
         * floats itself. Some encoders only record normalization on the glTF
         * accessor, so that flag is honored as a fallback.
         */
        template<typename ArrayT, int N>
        static osg::ref_ptr<ArrayT> dracoFloatArray(const draco::Mesh& mesh, const draco::PointAttribute& attribute, const cgltf_accessor* accessor)
        {
            float scale = 1.0f;
            if (!attribute.normalized() && accessor && accessor->normalized)
            {
                switch (attribute.data_type())
                {
                case draco::DT_INT8: scale = 1.0f / 127.0f; break;
                case draco::DT_UINT8: scale = 1.0f / 255.0f; break;
                case draco::DT_INT16: scale = 1.0f / 32767.0f; break;
                case draco::DT_UINT16: scale = 1.0f / 65535.0f; break;
                default: break;
                }
            }
            osg::ref_ptr<ArrayT> array = new ArrayT(mesh.num_points());
            for (draco::PointIndex i(0); i < mesh.num_points(); ++i)
            {
                float* value = reinterpret_cast<float*>(&(*array)[i.value()]);
                attribute.ConvertValue<float, N>(attribute.mapped_index(i), value);
                if (scale != 1.0f)
                    for (int c = 0; c < N; ++c) value[c] = std::max(value[c] * scale, -1.0f);
            }
            return array;
        }

        bool decodeDraco(const cgltf_primitive& primitive, GLenum mode, const osg::Vec4& baseColor, PrimitiveArrays& out)
        {
            const cgltf_draco_mesh_compression& draco = primitive.draco_mesh_compression;
            const uint8_t* bytes = cgltf_buffer_view_data(draco.buffer_view);
            if (!bytes)
            {
                OE_WARN << LC << "Draco buffer view has no data" << std::endl;
                return false;
            }
            draco::DecoderBuffer buffer;
            buffer.Init(reinterpret_cast<const char*>(bytes), draco.buffer_view->size);
            draco::Decoder decoder;
            auto decoded = decoder.DecodeMeshFromBuffer(&buffer);
            if (!decoded.ok())
            {
                OE_WARN << LC << "Draco: " << decoded.status().error_msg() << std::endl;
                return false;
            }
            const std::unique_ptr<draco::Mesh> mesh = std::move(decoded).value();

            for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
            {
                const cgltf_attribute& attribute = primitive.attributes[i];
                const draco::PointAttribute* dracoAttribute = nullptr;
                for (cgltf_size k = 0; k < draco.attributes_count && !dracoAttribute; ++k)
                {
                    if (!attribute.name || !draco.attributes[k].name || std::strcmp(attribute.name, draco.attributes[k].name) != 0)
                        continue;
                    // The vendored cgltf leaves Draco attribute values as unique ids encoded as (id + 1).
                    const cgltf_size encoded = reinterpret_cast<cgltf_size>(draco.attributes[k].data);
                    if (encoded > 0)
                        dracoAttribute = mesh->GetAttributeByUniqueId(static_cast<uint32_t>(encoded - 1));
                }
                switch (attribute.type)
                {
                case cgltf_attribute_type_position:
                    out.position = dracoAttribute ? dracoFloatArray<osg::Vec3Array, 3>(*mesh, *dracoAttribute, attribute.data) : floatArray<osg::Vec3Array>(attribute.data, 3);
                    break;
                case cgltf_attribute_type_normal:
                    out.normal = dracoAttribute ? dracoFloatArray<osg::Vec3Array, 3>(*mesh, *dracoAttribute, attribute.data) : floatArray<osg::Vec3Array>(attribute.data, 3);
                    break;
                case cgltf_attribute_type_texcoord:
                    if (attribute.index >= 0 && attribute.index < 2)
                        out.texcoord[attribute.index] = dracoAttribute ? dracoFloatArray<osg::Vec2Array, 2>(*mesh, *dracoAttribute, attribute.data) : floatArray<osg::Vec2Array>(attribute.data, 2);
                    break;
                case cgltf_attribute_type_color:
                    if (attribute.index != 0) break;
                    if (!dracoAttribute) { out.color = colorArray(attribute.data, baseColor); break; }
                    {
                        osg::ref_ptr<osg::Vec4Array> values = dracoFloatArray<osg::Vec4Array, 4>(*mesh, *dracoAttribute, attribute.data);
                        for (auto& v : *values)
                        {
                            if (dracoAttribute->num_components() < 4) v.a() = 1.0f;
                            v = osg::componentMultiply(v, baseColor);
                        }
                        out.color = values;
                    }
                    break;
                default: break;
                }
            }
            if (!out.position.valid()) return false;

            const std::size_t indexCount = static_cast<std::size_t>(mesh->num_faces()) * 3u;
            if (mesh->num_points() <= 65535u)
            {
                osg::ref_ptr<osg::DrawElementsUShort> elements = new osg::DrawElementsUShort(GL_TRIANGLES, static_cast<unsigned>(indexCount));
                for (draco::FaceIndex f(0); f < mesh->num_faces(); ++f)
                    for (int c = 0; c < 3; ++c)
                        (*elements)[f.value() * 3 + c] = static_cast<GLushort>(mesh->face(f)[c].value());
                out.primitive = elements;
            }
            else
            {
                osg::ref_ptr<osg::DrawElementsUInt> elements = new osg::DrawElementsUInt(GL_TRIANGLES, static_cast<unsigned>(indexCount));
                for (draco::FaceIndex f(0); f < mesh->num_faces(); ++f)
                    for (int c = 0; c < 3; ++c)
                        (*elements)[f.value() * 3 + c] = mesh->face(f)[c].value();
                out.primitive = elements;
            }
            return true;
        }
#endif

        // --------------------------------------------------------- materials

        //! Applies the material to a primitive stateset; true when a PBRTexture was bound.
        bool applyMaterial(const cgltf_material& material, osg::StateSet* stateSet, osg::Vec4& baseColor)
        {
            if (material.double_sided)
                stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);

            if (material.has_pbr_metallic_roughness)
            {
                const cgltf_float* factor = material.pbr_metallic_roughness.base_color_factor;
                baseColor.set(factor[0], factor[1], factor[2], factor[3]);
            }

            bool bound = false;
            if (!_flags.skipImagery)
            {
                osg::ref_ptr<osgEarth::PBRTexture> textures = pbrTexture(material);
                if (textures.valid())
                {
                    textures->install(stateSet);
                    bound = true;
                }
            }

            if (material.alpha_mode == cgltf_alpha_mode_blend)
            {
                stateSet->setMode(GL_BLEND, osg::StateAttribute::ON);
                stateSet->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                osgEarth::Util::DiscardAlphaFragments().install(stateSet, 0.15f);
            }
            else if (material.alpha_mode == cgltf_alpha_mode_mask)
            {
                // Alpha test only: masked surfaces stay opaque, so they need no
                // sorting and remain eligible for hardware instancing.
                osgEarth::Util::DiscardAlphaFragments().install(stateSet, material.alpha_cutoff);
            }
            return bound;
        }

        //! A white material for untextured primitives; the shared program always samples an albedo.
        osgEarth::PBRTexture* defaultMaterial()
        {
            if (!_defaultMaterial.valid())
            {
                osgEarth::PBRMaterial description;
                description.colorImage = new osg::Image();
                description.colorImage->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
                description.colorImage->setColor(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f), 0, 0);
                _defaultMaterial = new osgEarth::PBRTexture();
                _defaultMaterial->load(description, _options);
                _defaultMaterial->normal = nullptr;
                _defaultMaterial->pbr = nullptr;
                configureTexture(_defaultMaterial->albedo.get(), nullptr, true);
            }
            return _defaultMaterial.get();
        }

        //! The metallic-roughness and occlusion maps can share one sample only when bound identically.
        static bool sharesORM(const cgltf_material& material)
        {
            const cgltf_texture_view& mr = material.pbr_metallic_roughness.metallic_roughness_texture;
            const cgltf_texture_view& ao = material.occlusion_texture;
            if (!mr.texture || mr.texture != ao.texture || mr.texcoord != ao.texcoord || mr.has_transform != ao.has_transform)
                return false;
            return !mr.has_transform || std::memcmp(&mr.transform, &ao.transform, sizeof(mr.transform)) == 0;
        }

        //! Factors explicitly differing from the glTF defaults enable the PBR factor path.
        static bool hasExplicitFactors(const cgltf_material& material)
        {
            return material.has_pbr_metallic_roughness &&
                (material.pbr_metallic_roughness.metallic_factor != 1.0f ||
                 material.pbr_metallic_roughness.roughness_factor != 1.0f);
        }

        //! Resolved URI of an externally referenced image; empty for embedded images.
        const std::string& imageURI(const cgltf_image& image)
        {
            const cgltf_size index = cgltf_image_index(_data, &image);
            if (!_imageURIsResolved[index])
            {
                _imageURIsResolved[index] = 1;
                if (!image.buffer_view && image.uri && !isDataURI(image.uri))
                    _imageURIs[index] = resolveURI(image.uri, _referrer);
            }
            return _imageURIs[index];
        }

        /**
         * Key identifying materials that can be shared across documents. Every
         * candidate image, sampler setting and factor takes part. Embedded
         * images are document-local, so those materials return an empty key.
         */
        std::string materialKey(const cgltf_material& material)
        {
            std::ostringstream key;
            key.precision(9);
            key << "gltf-pbr-v2:" << !_flags.skipPBRTextures << ':' << _basisRequired << ':';
            if (_options) key << _options->getPluginStringData("BASIS_FORMAT");
            const cgltf_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;
            const cgltf_texture* textures[] = {
                material.has_pbr_metallic_roughness ? pbr.base_color_texture.texture : nullptr,
                _flags.skipPBRTextures ? nullptr : material.normal_texture.texture,
                _flags.skipPBRTextures || !material.has_pbr_metallic_roughness ? nullptr : pbr.metallic_roughness_texture.texture,
                _flags.skipPBRTextures ? nullptr : material.occlusion_texture.texture };
            for (const cgltf_texture* texture : textures)
            {
                key << '|';
                if (!texture) { key << "none"; continue; }
                key << texture->has_basisu << ':';
                for (const cgltf_image* image : { texture->has_basisu ? texture->basisu_image : nullptr,
                                                  texture->has_webp ? texture->webp_image : nullptr, texture->image })
                {
                    if (!image) { key << "none:"; continue; }
                    const std::string& uri = imageURI(*image);
                    if (uri.empty()) return {};
                    key << uri.size() << ':' << uri;
                }
                if (texture->sampler)
                    key << ':' << texture->sampler->wrap_s << ',' << texture->sampler->wrap_t << ','
                        << texture->sampler->min_filter << ',' << texture->sampler->mag_filter;
                else key << ":default";
            }
            key << '|' << material.normal_texture.scale << '|' << pbr.roughness_factor << '|' << pbr.metallic_factor
                << '|' << material.occlusion_texture.scale << '|' << hasExplicitFactors(material) << '|' << sharesORM(material);
            return key.str();
        }

        osg::ref_ptr<osgEarth::PBRTexture> pbrTexture(const cgltf_material& material)
        {
            const cgltf_size index = cgltf_material_index(_data, &material);
            if (_materialsBuilt[index]) return _materials[index];
            _materialsBuilt[index] = 1;

            const std::string key = _textureCache ? materialKey(material) : std::string();
            const bool shared = !key.empty();
            osg::ref_ptr<osgEarth::PBRTexture> result;
            if (shared && !_flags.forceReload)
            {
                std::lock_guard<std::mutex> lock(_textureCache->mutex());
                auto found = _textureCache->find(key);
                if (found != _textureCache->end() && !found->second.lock(result))
                    _textureCache->erase(found);
            }
            if (!result.valid())
            {
                result = buildPBRTexture(material);
                if (result.valid() && shared)
                {
                    std::lock_guard<std::mutex> lock(_textureCache->mutex());
                    auto& entry = (*_textureCache)[key];
                    osg::ref_ptr<osgEarth::PBRTexture> existing;
                    if (!_flags.forceReload && entry.lock(existing)) result = existing;
                    else entry = result.get();
                }
            }
            return _materials[index] = result;
        }

        osg::ref_ptr<osgEarth::PBRTexture> buildPBRTexture(const cgltf_material& material)
        {
            const cgltf_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;
            const cgltf_texture* colorTexture = material.has_pbr_metallic_roughness ? pbr.base_color_texture.texture : nullptr;
            const cgltf_texture* mrTexture = material.has_pbr_metallic_roughness ? pbr.metallic_roughness_texture.texture : nullptr;

            osgEarth::PBRMaterial description;
            description.name() = material.name ? material.name : "";
            description.colorImage = textureImage(colorTexture);
            if (!description.colorImage.valid())
            {
                description.colorImage = new osg::Image();
                description.colorImage->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
                description.colorImage->setColor(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f), 0, 0);
            }

            osg::ref_ptr<osg::Image> normal, mr, ao;
            if (!_flags.skipPBRTextures)
            {
                normal = textureImage(material.normal_texture.texture);
                mr = textureImage(mrTexture);
                ao = textureImage(material.occlusion_texture.texture);
            }
            if (!error.empty()) return {};

            const bool factors = !_flags.skipPBRTextures && (mr.valid() || ao.valid() || hasExplicitFactors(material));
            description.normalImage = normal;
            if (factors)
            {
                const bool sharedAO = mr.valid() && ao.valid() && sharesORM(material);
                description.layout() = sharedAO ? osgEarth::PBRMaterial::ORM : osgEarth::PBRMaterial::RM;
                description.packedImage = mr;
                if (!sharedAO) description.aoImage = ao;
                description.roughnessFactor() = material.has_pbr_metallic_roughness ? pbr.roughness_factor : 1.0f;
                description.metallicFactor() = material.has_pbr_metallic_roughness ? pbr.metallic_factor : 1.0f;
                description.occlusionStrength() = material.occlusion_texture.texture ? material.occlusion_texture.scale : 1.0f;
            }

            osg::ref_ptr<osgEarth::PBRTexture> result = new osgEarth::PBRTexture();
            if (!result->load(description, _options).isOK())
            {
                OE_WARN << LC << "Failed to load material \"" << description.name() << "\"" << std::endl;
                return {};
            }
            // Only sample maps the material actually provides.
            if (!normal.valid()) result->normal = nullptr;
            if (!factors) result->pbr = nullptr;

            configureTexture(result->albedo.get(), colorTexture, true);
            configureTexture(result->normal.get(), material.normal_texture.texture, false);
            configureTexture(result->pbr.get(), mrTexture, false);
            configureTexture(result->occlusion.get(), material.occlusion_texture.texture, false);
            return result;
        }

        //! Applies the glTF sampler and color space to a texture created by PBRTexture.
        static void configureTexture(osg::Texture* texture, const cgltf_texture* source, bool srgb)
        {
            if (!texture) return;
            if (srgb)
            {
                const auto* image = texture->getImage(0);
                const GLenum format = image ? image->getPixelFormat() : GL_RGBA;
                texture->setInternalFormat(
                    format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ? GL_COMPRESSED_SRGB_S3TC_DXT1_EXT :
                    format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_SRGB8_ALPHA8);
                // Color and PBR textures may share an image; keep color space on the texture.
                if (format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT)
                    if (auto* texture2D = dynamic_cast<osg::Texture2D*>(texture))
                        texture2D->setSubloadCallback(new CompressedSRGBUpload());
            }
            texture->setResizeNonPowerOfTwoHint(false);
            texture->setDataVariance(osg::Object::STATIC);
            texture->setMaxAnisotropy(16.0f);

            const cgltf_sampler* sampler = source ? source->sampler : nullptr;
            texture->setWrap(osg::Texture::WRAP_S, sampler ? static_cast<osg::Texture::WrapMode>(sampler->wrap_s) : osg::Texture::REPEAT);
            texture->setWrap(osg::Texture::WRAP_T, sampler ? static_cast<osg::Texture::WrapMode>(sampler->wrap_t) : osg::Texture::REPEAT);
            texture->setFilter(osg::Texture::MIN_FILTER, sampler && sampler->min_filter != cgltf_filter_type_undefined ?
                static_cast<osg::Texture::FilterMode>(sampler->min_filter) : osg::Texture::LINEAR_MIPMAP_LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, sampler && sampler->mag_filter != cgltf_filter_type_undefined ?
                static_cast<osg::Texture::FilterMode>(sampler->mag_filter) : osg::Texture::LINEAR);
        }

        // ------------------------------------------------------------ images

        //! Prefer Basis, then WebP, then the core PNG/JPEG image.
        osg::ref_ptr<osg::Image> textureImage(const cgltf_texture* texture)
        {
            if (!texture) return {};
            osg::ref_ptr<osg::Image> image;
            if (texture->has_basisu)
            {
                image = decodedImage(texture->basisu_image);
                if (!image.valid() && _basisRequired)
                {
                    error = "Cannot load required KHR_texture_basisu texture (check KTX2 data and the Basis plugin)";
                    return {};
                }
            }
            if (!image.valid() && texture->has_webp) image = decodedImage(texture->webp_image);
            if (!image.valid()) image = decodedImage(texture->image);
            if (!image.valid() && texture->has_basisu)
                error = "Cannot load KHR_texture_basisu texture or its fallback";
            return image;
        }

        //! Decodes an image once per document; rows are stored top first.
        osg::ref_ptr<osg::Image> decodedImage(const cgltf_image* image)
        {
            if (!image) return {};
            const cgltf_size index = cgltf_image_index(_data, image);
            if (_imagesTried[index]) return _images[index];
            _imagesTried[index] = 1;

            const void* bytes = nullptr;
            std::size_t size = 0;
            std::string payload;
            osg::ref_ptr<osg::Object> holder;
            std::string name = image->name ? image->name : "";
            if (image->buffer_view)
            {
                bytes = cgltf_buffer_view_data(image->buffer_view);
                size = image->buffer_view->size;
            }
            else if (isDataURI(image->uri))
            {
                if (!decodeDataURI(image->uri, payload))
                {
                    OE_WARN << LC << "Image " << index << " has an invalid data URI" << std::endl;
                    return {};
                }
                bytes = payload.data();
                size = payload.size();
            }
            else if (image->uri)
            {
                const std::string& uri = imageURI(*image);
                osgEarth::ReadResult rr = osgEarth::URI(uri).readString(_options);
                if (rr.failed())
                {
                    OE_WARN << LC << "Failed to read image " << uri << std::endl;
                    return {};
                }
                holder = rr.getObject();
                bytes = rr.getString().data();
                size = rr.getString().size();
                if (name.empty()) name = uri;
            }
            if (!bytes || size == 0) return {};

            osg::ref_ptr<osg::Image> result = decodeImage(bytes, size, image->mime_type, name);
            if (result.valid())
            {
                if (result->getFileName().empty()) result->setFileName(name);
                // Rows are stored top first; the writer uses this to avoid flipping twice.
                result->setOrigin(osg::Image::TOP_LEFT);
                _images[index] = result;
            }
            return result;
        }

        static bool isKTX2(const void* bytes, std::size_t size)
        {
            return size >= 12 && std::memcmp(bytes, "\xABKTX 20\xBB\r\n\x1A\n", 12) == 0;
        }

        static bool isWebP(const void* bytes, std::size_t size)
        {
            const char* p = static_cast<const char*>(bytes);
            return size >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0;
        }

        osg::ref_ptr<osg::Image> decodeImage(const void* bytes, std::size_t size, const char* mimeType, const std::string& name)
        {
            if (isKTX2(bytes, size))
                return decodeWithPlugin(bytes, size, "basis", basisOptions(), false);

            if (isWebP(bytes, size))
            {
#ifdef OSGEARTH_GLTF_HAVE_WEBP
                int width = 0, height = 0;
                if (!WebPGetInfo(static_cast<const uint8_t*>(bytes), size, &width, &height)) return {};
                osg::ref_ptr<osg::Image> image = new osg::Image();
                image->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);
                image->setInternalTextureFormat(GL_RGBA8);
                if (!WebPDecodeRGBAInto(static_cast<const uint8_t*>(bytes), size, image->data(), image->getTotalSizeInBytes(), width * 4))
                {
                    OE_WARN << LC << "Failed to decode WebP image " << name << std::endl;
                    return {};
                }
                return image;
#else
                return decodeWithPlugin(bytes, size, "webp", _options, true);
#endif
            }

            int width = 0, height = 0, channels = 0;
            if (size <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
                stbi_info_from_memory(static_cast<const stbi_uc*>(bytes), static_cast<int>(size), &width, &height, &channels))
            {
                // Refuse absurd dimensions before stb allocates for them.
                if (width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > (std::uint64_t(1) << 28))
                {
                    OE_WARN << LC << "Image " << name << " is too large (" << width << "x" << height << ")" << std::endl;
                    return {};
                }
                stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(bytes), static_cast<int>(size), &width, &height, &channels, 4);
                if (pixels)
                {
                    osg::ref_ptr<osg::Image> image = new osg::Image();
                    image->setImage(width, height, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, pixels, osg::Image::USE_MALLOC_FREE);
                    return image;
                }
            }

            // Anything else goes to whatever OSG plugin claims the format.
            std::string extension = osgDB::getLowerCaseFileExtension(name);
            if (mimeType)
            {
                const char* slash = std::strrchr(mimeType, '/');
                if (slash && *(slash + 1)) extension = slash + 1;
            }
            if (extension.empty() || !osgDB::Registry::instance()->getReaderWriterForExtension(extension))
            {
                OE_WARN << LC << "Cannot decode image " << name << ": " << stbi_failure_reason() << std::endl;
                return {};
            }
            return decodeWithPlugin(bytes, size, extension, _options, true);
        }

        //! Reads an image through an OSG plugin from memory; plugins return rows bottom first.
        static osg::ref_ptr<osg::Image> decodeWithPlugin(const void* bytes, std::size_t size, const std::string& extension, const osgDB::Options* options, bool flip)
        {
            osgDB::ReaderWriter* plugin = osgDB::Registry::instance()->getReaderWriterForExtension(extension);
            if (!plugin)
            {
                OE_WARN << LC << "No image plugin for \"" << extension << "\"" << std::endl;
                return {};
            }
            MemoryStreamBuffer streamBuffer(bytes, size);
            std::istream in(&streamBuffer);
            osgDB::ReaderWriter::ReadResult rr = plugin->readImage(in, options);
            if (!rr.validImage())
            {
                OE_WARN << LC << "Image plugin \"" << extension << "\" failed: " << rr.message() << std::endl;
                return {};
            }
            osg::ref_ptr<osg::Image> image = rr.takeImage();
            if (flip) image->flipVertical();
            return image;
        }

        const osgDB::Options* basisOptions()
        {
            if (!_basisOptions.valid())
            {
                _basisOptions = osgEarth::Registry::cloneOrCreateOptions(_options);
                _basisOptions->setPluginStringData("BASIS_ORIGIN", "top_left");
            }
            return _basisOptions.get();
        }
    };

private:
    TextureCache* _textureCache = nullptr;
};
