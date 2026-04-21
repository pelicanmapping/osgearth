/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgEarth/Notify>
// For internal format definitions.
#include <osg/Texture>

#include <Lerc_c_api.h>
#include <Lerc_types.h>

#define LC "[lerc] "

typedef unsigned char Byte;    // convenience
typedef unsigned int uint32;

class ReaderWriterLERC : public osgDB::ReaderWriter
{
public:
    ReaderWriterLERC()
    {
        supportsExtension("lerc", "ESRI Lerc");
        supportsExtension("lerc1", "ESRI Lerc");
        supportsExtension("lerc2", "ESRI Lerc");
    }

    virtual const char* className() const { return "ESRI Lerc"; }

    virtual ReadResult readObject(std::istream& fin, const osgDB::ReaderWriter::Options* options = NULL) const
    {
        return readImage(fin, options);
    }

    virtual ReadResult readObject(const std::string& file, const osgDB::ReaderWriter::Options* options = NULL) const
    {
        return readImage(file, options);
    }

    virtual ReadResult readImage(std::istream& fin, const osgDB::ReaderWriter::Options* = NULL) const
    {
        // get length of file:
        fin.seekg(0, fin.end);
        int length = fin.tellg();
        fin.seekg(0, fin.beg);

        std::unique_ptr<char[]> data(new char[length]);
        fin.read(data.get(), length);

        uint32 infoArr[8];

        lerc_status hr(0);

        hr = lerc_getBlobInfo((const unsigned char*)(data.get()), length, infoArr, NULL, 8, 0);
        if (hr)
        {
            OE_WARN << LC << "Failed to get blob info error = " << hr << std::endl;
            return ReadResult::ERROR_IN_READING_FILE;
        }

        unsigned int dataType = infoArr[1];
        unsigned int numDims = infoArr[2];
        unsigned int width = infoArr[3];
        unsigned int height = infoArr[4];
        unsigned int numBands = infoArr[5];
        unsigned int nValidPixels = infoArr[6];
        unsigned int blobSize = infoArr[7];

        GLenum glDataType;
        int    sampleSize;
        GLint  internalFormat;

        switch (dataType)
        {
            case (uint32)LercNS::DataType::dt_char:
            {
                sampleSize = sizeof(char);
                glDataType = GL_BYTE;
                internalFormat =
                    numBands == 1 ? GL_R8 :
                    numBands == 2 ? GL_RG8 :
                    numBands == 3 ? GL_RGB8 :
                    GL_RGBA8;
                break;
            }
            case (uint32)LercNS::DataType::dt_uchar:
            {
                sampleSize = sizeof(unsigned char);
                glDataType = GL_UNSIGNED_BYTE;
                internalFormat =
                    numBands == 1 ? GL_R8 :
                    numBands == 2 ? GL_RG8 :
                    numBands == 3 ? GL_RGB8 :
                    GL_RGBA8;
                break;
            }
            case (uint32)LercNS::DataType::dt_short:
            {
                sampleSize = sizeof(short);
                glDataType = GL_SHORT;
                internalFormat =
                    numBands == 1 ? GL_R16 :
                    numBands == 2 ? GL_RG16 :
                    numBands == 3 ? GL_RGB16 :
                    GL_RGBA16;
                break;
            }
            case (uint32)LercNS::DataType::dt_ushort:
            {
                sampleSize = sizeof(unsigned short);
                glDataType = GL_UNSIGNED_SHORT;
                internalFormat =
                    numBands == 1 ? GL_R16 :
                    numBands == 2 ? GL_RG16 :
                    numBands == 3 ? GL_RGB16 :
                    GL_RGBA16;
                break;
            }
            case (uint32)LercNS::DataType::dt_double:
            {
                sampleSize = sizeof(double);
                glDataType = GL_DOUBLE;
                internalFormat = GL_R32F;
                internalFormat =
                    numBands == 1 ? GL_R32F :
                    numBands == 2 ? GL_RG32F :
                    numBands == 3 ? GL_RGB32F_ARB :
                    GL_RGB32F_ARB;
                break;
            }
            default:
            {
                sampleSize = sizeof(float);
                glDataType = GL_FLOAT;
                internalFormat = GL_R32F;
                internalFormat =
                    numBands == 1 ? GL_R32F :
                    numBands == 2 ? GL_RG32F :
                    numBands == 3 ? GL_RGB32F_ARB :
                    GL_RGB32F_ARB;
                break;
            }

        }

        GLenum pixelFormat =
            numBands == 1 ? GL_RED :
            numBands == 2 ? GL_RG :
            numBands == 3 ? GL_RGB :
            GL_RGBA;

        // Allocate enough memory to hold the output image.
        unsigned int totalOutputSize = width * height * sampleSize * numDims * numBands;
        Byte* output = new Byte[totalOutputSize];
        memset(output, 0, totalOutputSize);

        // Decode the image
        unsigned int bandOffset = 0;
        hr = lerc_decode((const unsigned char*)(data.get()), length, 0, numDims, width, height, numBands, dataType, (void*)output);
        if (hr)
        {
            delete[]output;
            OE_WARN << LC << "Failed to decode lerc blob error=" << hr << std::endl;
            return ReadResult::ERROR_IN_READING_FILE;
        }

        // Allocate the final output image
        osg::ref_ptr< osg::Image > image = new osg::Image;

        if (numBands > 1 && numDims == 1)
        {
            // Interleave the bands properly if we have more than one.
            image->allocateImage(width, height, 1, pixelFormat, glDataType);
            memset(image->data(), 0, image->getImageSizeInBytes());

            unsigned int bandSize = width * height * sampleSize * numDims;
            unsigned int rowSize = sampleSize * width;

            for (unsigned int r = 0; r < height; ++r)
            {
                for (unsigned int c = 0; c < width; ++c)
                {
                    for (unsigned int band = 0; band < numBands; ++band)
                    {
                        Byte* bandPtr = output + (band * bandSize);
                        memcpy(image->data(c, r) + band * sampleSize, (const void*)&bandPtr[r * rowSize + c * sampleSize], sampleSize);
                    }
                }
            }
            // Delete the original output array, no longer needed.
            delete[]output;
        }
        else
        {
            // Just set the output array as the image, image takes ownership of the array.
            image->setImage(width, height, 1, internalFormat, pixelFormat, glDataType, (unsigned char*)output, osg::Image::USE_NEW_DELETE);
        }

        image->flipVertical();
        image->setInternalTextureFormat(internalFormat);

        return image;
    }

    virtual ReadResult readImage(const std::string& file, const osgDB::ReaderWriter::Options* options) const
    {
        std::string ext = osgDB::getLowerCaseFileExtension(file);
        if (!acceptsExtension(ext)) return ReadResult::FILE_NOT_HANDLED;

        std::string fileName = osgDB::findDataFile(file, options);
        if (fileName.empty()) return ReadResult::FILE_NOT_FOUND;

        osgDB::ifstream istream(fileName.c_str(), std::ios::in | std::ios::binary);
        if (!istream) return ReadResult::ERROR_IN_READING_FILE;

        return readImage(istream, options);
    }

    virtual WriteResult writeImage(const osg::Image& img, std::ostream& fout, const osgDB::ReaderWriter::Options *options) const
    {
        lerc_status hr(0);

        uint32 numBytesNeeded = 0;
        uint32 numBytesWritten = 0;

        double maxZErrorWanted = 0.1;
        if (options)
        {
            std::istringstream iss(options->getOptionString());
            std::string opt;
            while (iss >> opt)
            {
                if (strcmp(opt.c_str(), "LERC_MAXZERROR") == 0)
                {
                    iss >> maxZErrorWanted;
                }
            }
        }

        double eps = 0.0001;    // safety margin (optional), to account for finite floating point accuracy
        double maxZError = maxZErrorWanted - eps;

        unsigned int width = img.s();
        unsigned int height = img.t();

        osg::ref_ptr< osg::Image > flipped = new osg::Image(img);
        flipped->flipVertical();

        unsigned int numDims = 1;
        unsigned int numBands = 1;
        uint32 dataType;
        unsigned int sampleSize;

        switch (img.getDataType())
        {
        case GL_BYTE:
            dataType = (uint32)LercNS::DataType::dt_char;
            sampleSize = sizeof(char);
            break;
        case GL_UNSIGNED_BYTE:
            dataType = (uint32)LercNS::DataType::dt_uchar;
            sampleSize = sizeof(unsigned char);
            break;
        case GL_SHORT:
            dataType = (uint32)LercNS::DataType::dt_short;
            sampleSize = sizeof(short);
            break;
        case GL_UNSIGNED_SHORT:
            dataType = (uint32)LercNS::DataType::dt_ushort;
            sampleSize = sizeof(unsigned short);
            break;
        case GL_FLOAT:
            dataType = (uint32)LercNS::DataType::dt_float;
            sampleSize = sizeof(float);
            break;
        case GL_DOUBLE:
            dataType = (uint32)LercNS::DataType::dt_double;
            sampleSize = sizeof(double);
            break;
        default:
            break;
        }

        switch (img.getPixelFormat())
        {
        case GL_LUMINANCE:
        case GL_RED:
            numBands = 1;
            break;

        case GL_RG:
            numBands = 2;
            break;

        case GL_RGB:
            numBands = 3;
            break;
        case GL_RGBA:
            numBands = 4;
            break;
        }

        unsigned int totalOutputSize = width * height * sampleSize * numDims * numBands;
        unsigned char* imageData = new unsigned char[totalOutputSize];
        if (numBands > 1)
        {
            unsigned int bandSize = width * height * sampleSize * numDims;
            unsigned int rowSize = sampleSize * width;

            // Write each band into the array separately.
            for (unsigned int band = 0; band < numBands; ++band)
            {
                Byte* bandPtr = imageData + (band * bandSize);
                for (unsigned int r = 0; r < height; ++r)
                {
                    for (unsigned int c = 0; c < width; ++c)
                    {
                        unsigned int destPix = r * rowSize + c * sampleSize;
                        memcpy((void*)(bandPtr + destPix), flipped->data(c, r) + band * sampleSize, sampleSize);
                    }
                }
            }
        }
        else
        {
            memcpy(imageData, flipped->data(), totalOutputSize);
        }



        hr = lerc_computeCompressedSize((void*)imageData,    // raw image data, row by row, band by band
            dataType, numDims, width, height, numBands,
            0,
            maxZError,           // max coding error per pixel, or precision
            &numBytesNeeded);    // size of outgoing Lerc blob
        if (hr)
        {
            OE_WARN << LC << "Failed to compute compressed size of  image error=" << hr << std::endl;
            return WriteResult::ERROR_IN_WRITING_FILE;
        }

        uint32 numBytesBlob = numBytesNeeded;
        Byte* pLercBlob = new Byte[numBytesBlob];

        hr = lerc_encode((void*)imageData,    // raw image data, row by row, band by band
            dataType, numDims, width, height, numBands,
            0,         // can give nullptr if all pixels are valid
            maxZError,           // max coding error per pixel, or precision
            pLercBlob,           // buffer to write to, function will fail if buffer too small
            numBytesBlob,        // buffer size
            &numBytesWritten);   // num bytes written to buffer
        if (hr)
        {
            delete[]pLercBlob;
            OE_WARN << LC << "Failed to encode image error=" << hr << std::endl;
            return WriteResult::ERROR_IN_WRITING_FILE;
        }
        fout.write((const char*)pLercBlob, numBytesWritten);
        delete[]pLercBlob;
        delete[]imageData;

        return WriteResult::FILE_SAVED;
    }

    virtual WriteResult writeImage(const osg::Image &img, const std::string& fileName, const osgDB::ReaderWriter::Options *options) const
    {
        std::string ext = osgDB::getFileExtension(fileName);
        if (!acceptsExtension(ext)) return WriteResult::FILE_NOT_HANDLED;

        osgDB::ofstream fout(fileName.c_str(), std::ios::out | std::ios::binary);
        if (!fout) return WriteResult::ERROR_IN_WRITING_FILE;

        return writeImage(img, fout, options);
    }
};

REGISTER_OSGPLUGIN(lerc, ReaderWriterLERC)