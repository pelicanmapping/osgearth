/* osgEarth
* Copyright 2020 Pelican Mapping
* MIT License
*/
#include <osg/Image>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>

#include <basisu/transcoder/basisu_transcoder.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

class ReaderWriterBasis : public osgDB::ReaderWriter
{
public:
    ReaderWriterBasis()
    {
        supportsExtension("basis", "Basis image format");
        supportsExtension("ktx2", "KTX2 Basis Universal image format");
        supportsOption("BASIS_FORMAT", "Plugin string data: auto (default, BC1/BC3), or rgba8");
        supportsOption("BASIS_ORIGIN", "Plugin string data: bottom_left (default), or top_left");
    }

    const char* className() const override { return "Basis Universal Image Reader"; }

    ReadResult readObject(std::istream& fin, const Options* options = nullptr) const override
    {
        return readImage(fin, options);
    }

    ReadResult readObject(const std::string& file, const Options* options = nullptr) const override
    {
        return readImage(file, options);
    }

    ReadResult readImage(std::istream& fin, const Options* options = nullptr) const override
    try
    {
        // Initialize on first read, after the plugin's static objects exist.
        // Initializing from REGISTER_OSGPLUGIN's constructor can precede the
        // construction of Basis's ASTC tables and leave them uninitialized.
        static std::once_flag initialized;
        std::call_once(initialized, []() { basist::basisu_transcoder_init(); });

        // Read from the current position, including streams that cannot seek.
        // Basis uses 32-bit file sizes; never silently truncate a larger input.
        std::vector<char> data;
        char buffer[65536];
        while (fin)
        {
            try
            {
                fin.read(buffer, sizeof(buffer));
            }
            catch (const std::ios_base::failure&)
            {
                // A short final read can throw when the caller enabled stream
                // exceptions. Keep those bytes, but still reject I/O failures.
                if (fin.bad() || !fin.eof())
                    throw;
            }
            const auto count = static_cast<std::size_t>(fin.gcount());
            if (count > (std::numeric_limits<uint32_t>::max)() - data.size())
                return ReadResult("Basis input exceeds the 32-bit file size limit");
            data.insert(data.end(), buffer, buffer + count);
        }
        if (fin.bad() || !fin.eof() || data.empty())
            return ReadResult("Cannot read Basis input");

        const auto length = static_cast<uint32_t>(data.size());
        basist::basisu_transcoder transcoder;
        basist::basisu_file_info fileInfo;
        basist::basisu_image_info imageInfo;
        basist::ktx2_transcoder ktx;
        const bool isKTX2 = length >= 12 &&
            std::memcmp(data.data(), "\xABKTX 20\xBB\r\n\x1A\n", 12) == 0;
        uint32_t imageWidth, imageHeight, levels;
        bool alpha, bottomUp;
        // Preserve the reader's first-image behavior for multi-image files.
        const uint32_t imageIndex = 0;
        if (isKTX2)
        {
            if (!ktx.init(data.data(), length) ||
                ktx.get_header().m_pixel_depth != 0 || ktx.get_faces() != 1 ||
                ktx.get_layers() != 0 || (!ktx.is_etc1s() && !ktx.is_uastc()))
                return ReadResult("Expected a 2D ETC1S or UASTC KTX2 image");
            imageWidth = ktx.get_width();
            imageHeight = ktx.get_height();
            levels = ktx.get_levels();
            alpha = ktx.get_has_alpha() != 0;
            bottomUp = false;
            // Basis guarantees a terminating NUL in returned metadata values.
            if (const auto* orientation = ktx.find_key("KTXorientation"))
            {
                const std::string value(reinterpret_cast<const char*>(orientation->data()));
                if (value != "rd" && value != "ru")
                    return ReadResult("Unsupported KTX2 orientation");
                bottomUp = value == "ru";
            }
            if (const auto* swizzle = ktx.find_key("KTXswizzle"))
            {
                if (std::string(reinterpret_cast<const char*>(swizzle->data())) != "rgba")
                    return ReadResult("Unsupported KTX2 swizzle");
            }
        }
        else
        {
            if (!transcoder.validate_header(data.data(), length) ||
                !transcoder.get_file_info(data.data(), length, fileInfo) ||
                !transcoder.get_image_info(data.data(), length, imageInfo, imageIndex))
                return ReadResult("Invalid Basis image header");
            imageWidth = imageInfo.m_orig_width;
            imageHeight = imageInfo.m_orig_height;
            levels = imageInfo.m_total_levels;
            alpha = imageInfo.m_alpha_flag;
            bottomUp = fileInfo.m_y_flipped;
        }
        if (levels == 0 || levels > 32 || imageWidth == 0 || imageHeight == 0 ||
            imageWidth > (std::numeric_limits<int>::max)() ||
            imageHeight > (std::numeric_limits<int>::max)())
            return ReadResult("Invalid Basis image header");

        const std::string requestedFormat = options ? options->getPluginStringData("BASIS_FORMAT") : "";
        if (!requestedFormat.empty() && requestedFormat != "auto" && requestedFormat != "rgba8")
            return ReadResult("BASIS_FORMAT must be auto or rgba8");
        const std::string requestedOrigin = options ? options->getPluginStringData("BASIS_ORIGIN") : "";
        if (!requestedOrigin.empty() && requestedOrigin != "bottom_left" && requestedOrigin != "top_left")
            return ReadResult("BASIS_ORIGIN must be bottom_left or top_left");
        const bool topLeft = requestedOrigin == "top_left";
        const bool flip = bottomUp == topLeft;

        // OSG's DXT vertical flip only supports power-of-two dimensions.
        // Decode other top-down images to pixels so every mip can be flipped correctly.
        const auto powerOfTwo = [](uint32_t value) { return (value & (value - 1)) == 0; };
        // Without mipmaps, OSG's byte-size calculation also truncates partial
        // DXT blocks. Use pixels in that case so image copies remain safe.
        const bool rgba = requestedFormat == "rgba8" ||
            (levels == 1 && ((imageWidth % 4) != 0 || (imageHeight % 4) != 0)) ||
            (flip && (!powerOfTwo(imageWidth) || !powerOfTwo(imageHeight)));
        const auto format = rgba ? basist::transcoder_texture_format::cTFRGBA32 :
            (alpha ? basist::transcoder_texture_format::cTFBC3_RGBA :
                basist::transcoder_texture_format::cTFBC1_RGB);
        const GLenum pixelFormat = rgba ? GL_RGBA :
            (alpha ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT);

        // This also rejects HDR inputs for our LDR destinations, and is
        // available in both Basis 1.50 and 2.x.
        if (!isKTX2 && !basist::basis_is_format_supported(format, fileInfo.m_tex_format))
            return ReadResult("This Basis encoding cannot be transcoded to the requested format");

        const uint32_t bytesPerUnit = basist::basis_get_bytes_per_block_or_pixel(format);
        osg::Image::MipmapDataType mipmapOffsets;
        std::vector<uint32_t> outputCounts;
        uint64_t totalSize = 0;
        uint32_t width = imageWidth;
        uint32_t height = imageHeight;

        for (uint32_t level = 0; level < levels; ++level)
        {
            basist::basisu_image_level_info levelInfo;
            basist::ktx2_image_level_info ktxLevel;
            const bool valid = isKTX2 ?
                (ktx.get_image_level_info(ktxLevel, level, 0, 0) &&
                    ktxLevel.m_orig_width == width && ktxLevel.m_orig_height == height) :
                (transcoder.get_image_level_info(data.data(), length, levelInfo, imageIndex, level) &&
                    levelInfo.m_orig_width == width && levelInfo.m_orig_height == height);
            if (!valid)
                return ReadResult("Invalid Basis mipmap dimensions");

            // Size the destination blocks, not the source blocks: newer Basis
            // encodings can use block sizes other than BC1/BC3's 4x4.
            const uint64_t count = rgba ? uint64_t(width) * height :
                ((uint64_t(width) + 3) / 4) * ((uint64_t(height) + 3) / 4);
            const uint64_t levelSize = count * bytesPerUnit;
            if (levelSize == 0 || totalSize + levelSize > (std::numeric_limits<unsigned int>::max)())
                return ReadResult("Basis image exceeds the OSG image size limit");

            if (level > 0)
                mipmapOffsets.push_back(static_cast<unsigned int>(totalSize));
            outputCounts.push_back(static_cast<uint32_t>(count));
            totalSize += levelSize;
            if (width == 1 && height == 1 && level + 1 < levels)
                return ReadResult("Too many Basis mipmap levels");
            width = width > 1 ? width / 2 : 1;
            height = height > 1 ? height / 2 : 1;
        }

        if (!(isKTX2 ? ktx.start_transcoding() : transcoder.start_transcoding(data.data(), length)))
            return ReadResult("Cannot initialize Basis transcoding");

        std::unique_ptr<unsigned char[]> decoded(new unsigned char[static_cast<std::size_t>(totalSize)]);
        for (uint32_t level = 0; level < levels; ++level)
        {
            const auto offset = level == 0 ? 0 : mipmapOffsets[level - 1];
            const bool valid = isKTX2 ?
                ktx.transcode_image_level(level, 0, 0, decoded.get() + offset, outputCounts[level], format) :
                transcoder.transcode_image_level(data.data(), length, imageIndex, level,
                    decoded.get() + offset, outputCounts[level], format);
            if (!valid)
                return ReadResult("Cannot transcode Basis mipmap level");
        }

        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->setImage(imageWidth, imageHeight, 1,
            rgba ? GL_RGBA8 : pixelFormat, pixelFormat, GL_UNSIGNED_BYTE,
            decoded.get(), osg::Image::USE_NEW_DELETE);
        decoded.release();
        image->setMipmapLevels(mipmapOffsets);
        if (flip)
            image->flipVertical();
        image->setOrigin(topLeft ? osg::Image::TOP_LEFT : osg::Image::BOTTOM_LEFT);
        return image.release();
    }
    catch (const std::ios_base::failure&)
    {
        return ReadResult("Cannot read Basis input stream");
    }
    catch (const std::bad_alloc&)
    {
        return ReadResult("Cannot allocate Basis image data");
    }

    ReadResult readImage(const std::string& file, const Options* options = nullptr) const override
    {
        if (!acceptsExtension(osgDB::getLowerCaseFileExtension(file)))
            return ReadResult::FILE_NOT_HANDLED;

        const std::string fileName = osgDB::findDataFile(file, options);
        if (fileName.empty()) return ReadResult::FILE_NOT_FOUND;

        osgDB::ifstream stream(fileName.c_str(), std::ios::in | std::ios::binary);
        if (!stream) return ReadResult::ERROR_IN_READING_FILE;

        ReadResult result = readImage(stream, options);
        if (result.validImage())
            result.getImage()->setFileName(fileName);
        return result;
    }
};

REGISTER_OSGPLUGIN(basis, ReaderWriterBasis)
