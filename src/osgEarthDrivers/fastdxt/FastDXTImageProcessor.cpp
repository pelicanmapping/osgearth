/* osgEarth
* Copyright 2008-2013 Pelican Mapping
* MIT License
*/

#include <osg/Texture>
#include <osgDB/Registry>
#include <osgEarth/Notify>
#include <osg/GLU>
#include <osgEarth/ImageUtils>
#include <stdlib.h>
#include "libdxt.h"
#include <string.h>


using namespace osgEarth;
using namespace osgEarth::Util;

// Helper function to convert RGB/RGBA to RG8 for BC5 compression
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
    
    // Create new RG image
    unsigned char* rgData = new unsigned char[width * height * depth * 2];
    const unsigned char* srcData = image->data();
    
    // Extract RG channels
    for (int i = 0; i < width * height * depth; ++i)
    {
        rgData[i * 2 + 0] = srcData[i * numComponents + 0]; // Red
        rgData[i * 2 + 1] = srcData[i * numComponents + 1]; // Green
    }
    
    osg::Image* rgImage = new osg::Image();
    rgImage->setImage(width, height, depth, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 
                      rgData, osg::Image::USE_NEW_DELETE);
    
    return rgImage;
}

void padImageToMultipleOf4(osg::Image* input)
{
    if (input->s() % 4 == 0 && input->t() % 4 == 0)
        return; // Already a multiple of 4

    unsigned int newS = (input->s() + 3) & ~3; // Round up to next multiple of 4
    unsigned int newT = (input->t() + 3) & ~3; // Round up to next multiple of 4

    osg::ref_ptr<osg::Image> padded = new osg::Image();
    padded->allocateImage(newS, newT, input->r(), input->getPixelFormat(), input->getDataType());

    ImageUtils::PixelReader read(input);
    ImageUtils::PixelWriter write(padded);

    osg::Vec4 pixel;

    for (int t = 0; t < read.t(); ++t)
    {
        for (int s = 0; s < read.s(); ++s)
        {
            read(pixel, s, t);
            write(pixel, s, t);
        }

        // pad remaining columns in the output row with the same pixel value:
        for (unsigned ps = read.s(); ps < newS; ++ps)
        {
            write(pixel, ps, t);
        }
    }

    // pad the remaining rows in the output image with the last row's pixel values:
    for (unsigned pt = read.t(); pt < newT; ++pt)
    {
        for (unsigned ps = 0; ps < newS; ++ps)
        {
            // read from the last valid row:
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
        return; // No scaling needed

    // allocate new image:
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

class FastDXTProcessor : public osgDB::ImageProcessor
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
        // Gotta be at least 16x16
        if (input.s() < 16 || input.t() < 16)
            return;

        // Not already compressed?
        if (input.isCompressed())
            return;

        //Resize the image to the nearest power of two
        if (!ImageUtils::isPowerOfTwo(&input))
        {
            unsigned int s = osg::Image::computeNearestPowerOfTwo(input.s());
            unsigned int t = osg::Image::computeNearestPowerOfTwo(input.t());
            //input.scaleImage(s, t, input.r());

            scaleImage(&input, s, t);
        }

        // Determine compression parameters first
        int format;
        GLenum compressedPixelFormat;
        int minLevelSize;

        switch (compressedFormat)
        {
        case osg::Texture::USE_S3TC_DXT1_COMPRESSION:
            format = FORMAT_DXT1;
            compressedPixelFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
            minLevelSize = 8;
            break;
        case osg::Texture::USE_S3TC_DXT5_COMPRESSION:
            format = FORMAT_DXT5;
            compressedPixelFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            minLevelSize = 16;
            break;
        case osg::Texture::USE_RGTC2_COMPRESSION:
            format = FORMAT_BC5;
            compressedPixelFormat = GL_COMPRESSED_RED_GREEN_RGTC2_EXT;
            minLevelSize = 16;
            break;
        default:
            OSG_WARN << "Unhandled compressed format" << compressedFormat << std::endl;
            return;
            break;
        }

        osg::Image* sourceImage = &input;

        //Handle format conversion based on compression target
        osg::ref_ptr< osg::Image > converted;
        if (format == FORMAT_BC5)
        {
            // BC5 needs RG format - for now accept GL_RG8 directly or convert from multi-channel formats
            if (input.getPixelFormat() != GL_RG)
            {
                // Convert to RG8 by extracting first two channels from RGB/RGBA
                if (input.getPixelFormat() == GL_RGB || input.getPixelFormat() == GL_RGBA)
                {
                    converted = convertToRG8(&input);
                    sourceImage = converted.get();
                }
                else
                {
                    OSG_WARN << "BC5 compression requires GL_RG, GL_RGB, or GL_RGBA input format" << std::endl;
                    return;
                }
            }
        }
        else
        {
            //DXT1/DXT5 only work on RGBA imagery so we must convert it
            if (input.getPixelFormat() != GL_RGBA)
            {
                converted = ImageUtils::convertToRGBA8(&input);
                sourceImage = converted.get();
            }
        }

        OE_SOFT_ASSERT_AND_RETURN(sourceImage != nullptr, void());

        if (generateMipMap)
        {
            // size in bytes of the top-level image set (sum of [0..r-1])
            int levelZeroSizeBytes = sourceImage->getTotalSizeInBytes();

            // Mipmap the image
            osg::ref_ptr< const osg::Image > mipmapped = ImageUtils::mipmapImage(sourceImage, minLevelSize);

            // data ptr of each mipmap level [0..r-1]
            std::vector<unsigned char*> mipLevels;

            // length in bytes of each mipmap level [0..r-1]
            std::vector<unsigned> mipLevelBytes;

            // offset vector does not include level 0 (the full-resolution level)
            osg::Image::MipmapDataType mipOffsets;
            mipOffsets.reserve(mipmapped->getNumMipmapLevels());

            // now, populate the image levels.
            osg::PixelStorageModes psm;
            psm.pack_alignment = sourceImage->getPacking();
            psm.pack_row_length = sourceImage->getRowLength();
            psm.unpack_alignment = sourceImage->getPacking();

            const unsigned char* in = nullptr;

            unsigned totalCompressedBytes = 0u;

            // interate over mipmap levels:
            unsigned int numLevels = mipmapped->getNumMipmapLevels();
            for (unsigned level = 0; level < numLevels; ++level)
            {
                int level_s = sourceImage->s() >> level;
                int level_t = sourceImage->t() >> level;

                std::size_t levelAllocatedBytes = sourceImage->r() * (levelZeroSizeBytes); // max possible size
                unsigned char* compressedLevelDataPtr = (unsigned char*)memalign(16, levelAllocatedBytes);
                ::memset(compressedLevelDataPtr, 0, levelAllocatedBytes);
                mipLevels.push_back(compressedLevelDataPtr);

                unsigned levelCompressedBytes = 0u;

                in = mipmapped->getMipmapData(level);

                if (level != 0)
                {
                    // when we start a new mip level that's > 0, record its offset.
                    mipOffsets.push_back(totalCompressedBytes);
                }

                int outputBytes = CompressDXT(
                    in,
                    compressedLevelDataPtr,
                    level_s,
                    level_t,
                    format);

                // advance our counters:
                levelCompressedBytes += outputBytes;
                totalCompressedBytes += outputBytes;

                // advance output pointer:
                compressedLevelDataPtr += outputBytes;

                // record the data offset of this mip level
                mipLevelBytes.push_back(levelCompressedBytes);
            }

            // now combine into a new mipmapped compressed image
            // and delete any workspaces along the way.
            unsigned char* data = new unsigned char[totalCompressedBytes];
            unsigned char* ptr = data;
            for (unsigned i = 0; i < mipLevels.size(); ++i)
            {
                ::memcpy(ptr, mipLevels[i], mipLevelBytes[i]);
                ptr += mipLevelBytes[i];
                memfree(mipLevels[i]);
            }

            input.setImage(
                sourceImage->s(),
                sourceImage->t(),
                sourceImage->r(),
                compressedPixelFormat, // ?
                compressedPixelFormat,
                GL_UNSIGNED_BYTE,
                data,
                osg::Image::USE_NEW_DELETE);

            input.setMipmapLevels(mipOffsets);
        }

        else // no mipmaps, just one level
        {
            //TODO: support r > 1
            //Copy over the source data to an array
            unsigned char* in = 0;
            in = (unsigned char*)memalign(16, sourceImage->getTotalSizeInBytes());
            memcpy(in, sourceImage->data(0, 0), sourceImage->getTotalSizeInBytes());

            //Allocate memory for the output
            unsigned char* out = (unsigned char*)memalign(16, input.s() * input.t() * 4);
            memset(out, 0, input.s() * input.t() * 4);

            int outputBytes = CompressDXT(in, out, sourceImage->s(), sourceImage->t(), format);

            //Allocate and copy over the output data to the correct size array.
            unsigned char* data = (unsigned char*)malloc(outputBytes);
            memcpy(data, out, outputBytes);
            memfree(out);
            memfree(in);
            input.setImage(input.s(), input.t(), input.r(), compressedPixelFormat, compressedPixelFormat, GL_UNSIGNED_BYTE, data, osg::Image::USE_MALLOC_FREE);
        }
    }

    virtual void generateMipMap(osg::Image& image, bool resizeToPowerOfTwo, CompressionMethod method)
    {
        OSG_WARN << "FastDXT: generateMipMap not implemented" << std::endl;
    }
};

REGISTER_OSGIMAGEPROCESSOR(fastdxt, FastDXTProcessor)