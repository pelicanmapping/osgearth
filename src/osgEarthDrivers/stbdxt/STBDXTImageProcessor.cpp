/* osgEarth
* Copyright 2008-2013 Pelican Mapping
* MIT License
*/

#define STB_DXT_IMPLEMENTATION
#include <string>
#include <string.h>
#include "stb_dxt.h"

#include <osg/Texture>
#include <osgDB/Registry>
#include <osgEarth/Notify>
#include <osg/GLU>
#include <osgEarth/ImageUtils>
#include <stdlib.h>


using namespace osgEarth;
using namespace osgEarth::Util;

namespace
{
    osg::Image* convertToRG8(const osg::Image* image)
    {
        if (!image) return nullptr;

        int numComponents = 0;
        if (image->getPixelFormat() == GL_RGB) numComponents = 3;
        else if (image->getPixelFormat() == GL_RGBA) numComponents = 4;
        else return nullptr;

        int width = image->s();
        int height = image->t();
        int depth = image->r();

        unsigned char* rgData = new unsigned char[width * height * depth * 2];
        const unsigned char* srcData = image->data();

        for (int i = 0; i < width * height * depth; ++i)
        {
            rgData[i * 2 + 0] = srcData[i * numComponents + 0];
            rgData[i * 2 + 1] = srcData[i * numComponents + 1];
        }

        osg::Image* rgImage = new osg::Image();
        rgImage->setImage(width, height, depth, GL_RG8, GL_RG, GL_UNSIGNED_BYTE,
            rgData, osg::Image::USE_NEW_DELETE);

        return rgImage;
    }

    void padImageToMultipleOf4(osg::Image* input)
    {
        if (input->s() % 4 == 0 && input->t() % 4 == 0)
            return;

        unsigned int newS = (input->s() + 3) & ~3;
        unsigned int newT = (input->t() + 3) & ~3;

        osg::ref_ptr<osg::Image> padded = new osg::Image();
        padded->allocateImage(newS, newT, input->r(), input->getPixelFormat(), input->getDataType());

        ImageUtils::PixelReader read(input);
        ImageUtils::PixelWriter write(padded);

        osg::Vec4 pixel;

        for (unsigned t = 0; t < read.t(); ++t)
        {
            for (unsigned s = 0; s < read.s(); ++s)
            {
                read(pixel, s, t);
                write(pixel, s, t);
            }
            for (unsigned ps = read.s(); ps < newS; ++ps)
            {
                write(pixel, ps, t);
            }
        }

        for (unsigned pt = read.t(); pt < newT; ++pt)
        {
            for (unsigned ps = 0; ps < newS; ++ps)
            {
                read(pixel, ps < (unsigned)read.s() ? ps : (unsigned)read.s() - 1, (unsigned)read.t() - 1);
                write(pixel, ps, pt);
            }
        }

        padded->setAllocationMode(osg::Image::NO_DELETE);

        input->setImage(newS, newT, input->r(), input->getInternalTextureFormat(),
            input->getPixelFormat(), input->getDataType(), padded->data(), osg::Image::USE_NEW_DELETE);
    }

    void scaleImage(osg::Image* image, int new_s, int new_t)
    {
        if (image->s() == new_s && image->t() == new_t)
            return;

        osg::ref_ptr<osg::Image> scaled = new osg::Image();
        scaled->allocateImage(new_s, new_t, image->r(), image->getPixelFormat(), image->getDataType());

        ImageUtils::PixelReader read(image);
        ImageUtils::PixelWriter write(scaled);

        osg::Vec4 pixel;

        for (unsigned t = 0; t < (unsigned)new_t; ++t)
        {
            float v = (float)t / (float)(new_t - 1);
            for (unsigned s = 0; s < (unsigned)new_s; ++s)
            {
                float u = (float)s / (float)(new_s - 1);
                read(pixel, u, v);
                write(pixel, s, t);
            }
        }

        scaled->setAllocationMode(osg::Image::NO_DELETE);
        image->setImage(new_s, new_t, image->r(), image->getInternalTextureFormat(),
            image->getPixelFormat(), image->getDataType(), scaled->data(), osg::Image::USE_NEW_DELETE);
    }

    // Compress a full image using stb_dxt block-by-block.
    // Returns the number of bytes written to 'out'.
    int compressImageSTB(
        const unsigned char* in,
        unsigned char* out,
        int width,
        int height,
        int format,
        int quality)
    {
        int blockSize;
        int mode = (quality > 0) ? STB_DXT_HIGHQUAL : STB_DXT_NORMAL;

        // DXT1 = 8 bytes per 4x4 block, DXT5/BC5 = 16 bytes per 4x4 block
        bool isDXT1 = (format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);
        bool isDXT5 = (format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);
        bool isBC5 = (format == (int)GL_COMPRESSED_RED_GREEN_RGTC2_EXT);

        if (isDXT1) blockSize = 8;
        else blockSize = 16;

        int srcBytesPerPixel = isBC5 ? 2 : 4; // RG8 for BC5, RGBA for DXT
        int srcStride = width * srcBytesPerPixel;

        unsigned char* dst = out;

        for (int y = 0; y < height; y += 4)
        {
            for (int x = 0; x < width; x += 4)
            {
                if (isBC5)
                {
                    // Extract 4x4 block of RG data
                    unsigned char block[32]; // 4x4 pixels * 2 channels
                    for (int by = 0; by < 4; ++by)
                    {
                        const unsigned char* row = in + (y + by) * srcStride + x * 2;
                        memcpy(block + by * 8, row, 8);
                    }
                    stb_compress_bc5_block(dst, block);
                }
                else
                {
                    // Extract 4x4 block of RGBA data
                    unsigned char block[64]; // 4x4 pixels * 4 channels
                    for (int by = 0; by < 4; ++by)
                    {
                        const unsigned char* row = in + (y + by) * srcStride + x * 4;
                        memcpy(block + by * 16, row, 16);
                    }
                    stb_compress_dxt_block(dst, block, isDXT5 ? 1 : 0, mode);
                }

                dst += blockSize;
            }
        }

        return (int)(dst - out);
    }
}

class STBDXTProcessor : public osgDB::ImageProcessor
{
public:
    virtual void compress(
        osg::Image& input,
        osg::Texture::InternalFormatMode compressedFormat,
        bool generateMipMap,
        bool resizeToPowerOfTwo,
        CompressionMethod method,
        CompressionQuality quality)
    {
        if (input.s() < 4 || input.t() < 4)
            return;

        if (input.isCompressed())
            return;

        if (!ImageUtils::isPowerOfTwo(&input))
        {
            unsigned int s = osg::Image::computeNearestPowerOfTwo(input.s());
            unsigned int t = osg::Image::computeNearestPowerOfTwo(input.t());
            scaleImage(&input, s, t);
        }

        GLenum compressedPixelFormat;
        int minLevelSize;
        int highQuality = (quality == ImageProcessor::PRODUCTION || quality == ImageProcessor::HIGHEST) ? 1 : 0;

        switch (compressedFormat)
        {
        case osg::Texture::USE_S3TC_DXT1_COMPRESSION:
            compressedPixelFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
            minLevelSize = 8;
            break;
        case osg::Texture::USE_S3TC_DXT5_COMPRESSION:
            compressedPixelFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            minLevelSize = 16;
            break;
        case osg::Texture::USE_RGTC2_COMPRESSION:
            compressedPixelFormat = GL_COMPRESSED_RED_GREEN_RGTC2_EXT;
            minLevelSize = 16;
            break;
        default:
            OSG_WARN << "STB DXT: Unhandled compressed format " << compressedFormat << std::endl;
            return;
        }

        osg::Image* sourceImage = &input;

        osg::ref_ptr<osg::Image> converted;
        if (compressedFormat == osg::Texture::USE_RGTC2_COMPRESSION)
        {
            if (input.getPixelFormat() != GL_RG)
            {
                if (input.getPixelFormat() == GL_RGB || input.getPixelFormat() == GL_RGBA)
                {
                    converted = convertToRG8(&input);
                    sourceImage = converted.get();
                }
                else
                {
                    OSG_WARN << "STB DXT: BC5 compression requires GL_RG, GL_RGB, or GL_RGBA input" << std::endl;
                    return;
                }
            }
        }
        else
        {
            if (input.getPixelFormat() != GL_RGBA)
            {
                converted = ImageUtils::convertToRGBA8(&input);
                sourceImage = converted.get();
            }
        }

        OE_SOFT_ASSERT_AND_RETURN(sourceImage != nullptr, void());

        padImageToMultipleOf4(sourceImage);

        if (generateMipMap)
        {
            int levelZeroSizeBytes = sourceImage->getTotalSizeInBytes();

            osg::ref_ptr<const osg::Image> mipmapped = ImageUtils::mipmapImage(sourceImage, minLevelSize);

            std::vector<unsigned char*> mipLevels;
            std::vector<unsigned> mipLevelBytes;
            osg::Image::MipmapDataType mipOffsets;
            mipOffsets.reserve(mipmapped->getNumMipmapLevels());

            unsigned totalCompressedBytes = 0u;

            unsigned int numLevels = mipmapped->getNumMipmapLevels();
            for (unsigned level = 0; level < numLevels; ++level)
            {
                int level_s = sourceImage->s() >> level;
                int level_t = sourceImage->t() >> level;
                if (level_s < 4) level_s = 4;
                if (level_t < 4) level_t = 4;

                // Allocate max possible size for this level
                int maxOutputBytes = ((level_s + 3) / 4) * ((level_t + 3) / 4) *
                    (compressedPixelFormat == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8 : 16);

                unsigned char* compressedLevelDataPtr = new unsigned char[maxOutputBytes];
                memset(compressedLevelDataPtr, 0, maxOutputBytes);
                mipLevels.push_back(compressedLevelDataPtr);

                const unsigned char* in = mipmapped->getMipmapData(level);

                if (level != 0)
                {
                    mipOffsets.push_back(totalCompressedBytes);
                }

                int outputBytes = compressImageSTB(
                    in,
                    compressedLevelDataPtr,
                    level_s,
                    level_t,
                    compressedPixelFormat,
                    highQuality);

                mipLevelBytes.push_back(outputBytes);
                totalCompressedBytes += outputBytes;
            }

            unsigned char* data = new unsigned char[totalCompressedBytes];
            unsigned char* ptr = data;
            for (unsigned i = 0; i < mipLevels.size(); ++i)
            {
                memcpy(ptr, mipLevels[i], mipLevelBytes[i]);
                ptr += mipLevelBytes[i];
                delete[] mipLevels[i];
            }

            input.setImage(
                sourceImage->s(),
                sourceImage->t(),
                sourceImage->r(),
                compressedPixelFormat,
                compressedPixelFormat,
                GL_UNSIGNED_BYTE,
                data,
                osg::Image::USE_NEW_DELETE);

            input.setMipmapLevels(mipOffsets);
        }
        else
        {
            int numBlocks = ((sourceImage->s() + 3) / 4) * ((sourceImage->t() + 3) / 4);
            int blockSize = (compressedPixelFormat == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) ? 8 : 16;
            int maxOutputBytes = numBlocks * blockSize;

            unsigned char* out = new unsigned char[maxOutputBytes];
            memset(out, 0, maxOutputBytes);

            int outputBytes = compressImageSTB(
                sourceImage->data(),
                out,
                sourceImage->s(),
                sourceImage->t(),
                compressedPixelFormat,
                highQuality);

            unsigned char* data = new unsigned char[outputBytes];
            memcpy(data, out, outputBytes);
            delete[] out;

            input.setImage(
                input.s(), input.t(), input.r(),
                compressedPixelFormat, compressedPixelFormat, GL_UNSIGNED_BYTE,
                data, osg::Image::USE_NEW_DELETE);
        }
    }

    virtual void generateMipMap(osg::Image& image, bool resizeToPowerOfTwo, CompressionMethod method)
    {
        OSG_WARN << "STB DXT: generateMipMap not implemented" << std::endl;
    }
};

REGISTER_OSGIMAGEPROCESSOR(stbdxt, STBDXTProcessor)
