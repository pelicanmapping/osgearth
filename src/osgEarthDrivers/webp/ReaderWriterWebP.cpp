/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include <osg/Image>
#include <osg/Notify>

#include <osg/Geode>

#include <osg/GL>

#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>

#include <string>
#include <sstream>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <iomanip>

#include "webp/encode.h"
#include "webp/decode.h"

#if TARGET_OS_IPHONE
#include "strings.h"
#define stricmp strcasecmp
#endif

using namespace osg;

class ReaderWriterWebP : public osgDB::ReaderWriter
{
public:
  ReaderWriterWebP()
  {
    supportsExtension("webp", "WebP image format");
  }

  ~ReaderWriterWebP()
  {
  }

  virtual const char *className() const { return "Google WebP Image Reader/Writer"; }

  virtual ReadResult readObject(const std::string &file, const osgDB::ReaderWriter::Options *options) const
  {
    return readImage(file, options);
  }

  virtual ReadResult readObject(std::istream &fin, const Options *options) const
  {
    return readImage(fin, options);
  }

  virtual ReadResult readImage(const std::string &file, const osgDB::ReaderWriter::Options *options) const
  {
    std::string ext = osgDB::getFileExtension(file);
    if (!acceptsExtension(ext))
      return ReadResult::FILE_NOT_HANDLED;

    std::string fileName = osgDB::findDataFile(file, options);
    if (fileName.empty())
      return ReadResult::FILE_NOT_FOUND;

    osgDB::ifstream f(file.c_str(), std::ios_base::in | std::ios_base::binary);
    if (!f)
      return ReadResult::FILE_NOT_HANDLED;

    ReadResult rr = readImage(f, options);

    if (rr.validImage())
      rr.getImage()->setFileName(file);
    return rr;
  }

  virtual ReadResult readImage(std::istream &fin, const Options *options) const
  {
    Image *image = NULL;
    unsigned long size_of_vp8_image_data = 0;
    char *vp8_buffer = NULL;

    fin.seekg(0, std::ios::end);
    size_t stream_size = fin.tellg();
    fin.seekg(0, std::ios::beg);

    if (stream_size > 0)
    {

      size_of_vp8_image_data = stream_size;
      vp8_buffer = new char[stream_size];

      int version = WebPGetEncoderVersion();
      size_of_vp8_image_data = fin.read(vp8_buffer, size_of_vp8_image_data).gcount();

      WebPDecoderConfig config;
      WebPInitDecoderConfig(&config);
      int status = WebPGetFeatures((const uint8_t *)vp8_buffer, (uint32_t)size_of_vp8_image_data, &config.input);
      if (status == VP8_STATUS_OK)
      {

        unsigned int pixelFormat;
        unsigned int dataType = GL_UNSIGNED_BYTE;

        if (config.input.has_alpha)
        {
          pixelFormat = GL_RGBA;
          config.output.colorspace = MODE_RGBA;
        }
        else
        {
          /*
            BUG of AMD driver: http://devgurus.amd.com/message/1302663
            pixelFormat = GL_RGB;
            config.output.colorspace = MODE_RGB;
            */
          pixelFormat = GL_RGBA;
          config.output.colorspace = MODE_RGBA;
        }
        int internalFormat = pixelFormat;
        int r = 1;

        image = new Image();
        image->allocateImage(config.input.width, config.input.height, 1, pixelFormat, dataType);

        config.output.u.RGBA.rgba = (uint8_t *)image->data();
        config.output.u.RGBA.stride = image->getRowSizeInBytes();
        config.output.u.RGBA.size = image->getImageSizeInBytes();

        config.options.no_fancy_upsampling = 1;

        config.output.is_external_memory = 1;

        status = WebPDecode((const uint8_t *)vp8_buffer, (uint32_t)size_of_vp8_image_data, &config);
      }
      delete[] vp8_buffer;
    }
    else
    {
      OSG_NOTICE << "read webp image: stream size is zero" << std::endl;
    }

    if (image)
    {
        image->flipVertical();
    }

    return image;
  }

  virtual WriteResult writeObject(const osg::Object &object, const std::string &file, const osgDB::ReaderWriter::Options *options) const
  {
    const osg::Image *image = dynamic_cast<const osg::Image *>(&object);
    if (!image)
      return WriteResult::FILE_NOT_HANDLED;

    return writeImage(*image, file, options);
  }

  virtual WriteResult writeObject(const osg::Object &object, std::ostream &fout, const Options *options) const
  {
    const osg::Image *image = dynamic_cast<const osg::Image *>(&object);
    if (!image)
      return WriteResult::FILE_NOT_HANDLED;

    return writeImage(*image, fout, options);
  }

  virtual WriteResult writeImage(const osg::Image &img, const std::string &fileName, const osgDB::ReaderWriter::Options *options) const
  {
    std::string ext = osgDB::getFileExtension(fileName);
    if (!acceptsExtension(ext))
      return WriteResult::FILE_NOT_HANDLED;

    int internalFormat = osg::Image::computeNumComponents(img.getPixelFormat());

    osgDB::ofstream f(fileName.c_str(), std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
    if (!f)
      return WriteResult::ERROR_IN_WRITING_FILE;

    return writeImage(img, f, options);
  }

  static int ostream_writer(const uint8_t *data, size_t data_size,
                            const WebPPicture *const pic)
  {

    std::ostream *const out = (std::ostream *)pic->custom_ptr;
    return data_size ? (int)out->write((const char *)data, data_size).tellp() : 1;
  }

  WriteResult writeImage(const osg::Image &img, std::ostream &fout, const Options *options) const
  {
      int internalFormat = osg::Image::computeNumComponents(img.getPixelFormat());
      bool flipImage = true;

      WebPConfig config;
      WebPPicture picture;
      WebPPreset preset = WEBP_PRESET_PHOTO;

      if (img.getPixelFormat() == GL_LUMINANCE) {
          preset = WEBP_PRESET_DEFAULT;
      }

      if (!WebPPictureInit(&picture) || !WebPConfigInit(&config)) {
          return WriteResult::ERROR_IN_WRITING_FILE;
      }

      if (!WebPConfigPreset(&config, preset, config.quality)) {
          return WriteResult::ERROR_IN_WRITING_FILE;
      }

      if (options) {
          std::istringstream iss(options->getOptionString());
          std::string opt;
          while (iss >> opt) {
              if (strcmp(opt.c_str(), "WEBP_NO_FLIP") == 0) {
                  flipImage = false;
              }
              if (strcmp(opt.c_str(), "WEBP_LOSSLESS") == 0) {
                  config.lossless = 1;
                  config.quality = 100;
              }
              else if (strcmp(opt.c_str(), "WEBP_HINT") == 0) {
                  std::string v;
                  iss >> v;
                  if (strcmp(v.c_str(), "WEBP_HINT_PICTURE") == 0) {
                      config.image_hint = WEBP_HINT_PICTURE;
                  }
                  else if (strcmp(v.c_str(), "WEBP_HINT_PHOTO") == 0) {
                      config.image_hint = WEBP_HINT_PHOTO;
                  }
                  else if (strcmp(v.c_str(), "WEBP_HINT_GRAPH") == 0) {
                      config.image_hint = WEBP_HINT_GRAPH;
                  }
              }
              else if (strcmp(opt.c_str(), "WEBP_QUALITY") == 0) {
                  float v;
                  iss >> v;
                  if (v >= 0.0 && v <= 100.0) {
                      config.quality = v;
                  }
              }
              else if (strcmp(opt.c_str(), "WEBP_METHOD") == 0) {
                  int v;
                  iss >> v;
                  if (v >= 0 && v <= 6) {
                      config.method = v;
                  }
              }
          }
      }

      if (img.getPixelFormat() == GL_LUMINANCE) {
          config.lossless = 1;
      }

      if (!WebPValidateConfig(&config)) {
          return WriteResult::ERROR_IN_WRITING_FILE;
      }

      picture.width = img.s();
      picture.height = img.t();

      const unsigned char* imageData = img.data();
      osg::ref_ptr< osg::Image > flippedImage;
      if (flipImage) {
          flippedImage = new osg::Image(img);
          flippedImage->flipVertical();
          imageData = flippedImage->data();
      }

      switch (img.getPixelFormat()) {
      case(GL_RGB):
          WebPPictureImportRGB(&picture, imageData, img.getRowSizeInBytes());
          break;
      case(GL_RGBA):
          WebPPictureImportRGBA(&picture, imageData, img.getRowSizeInBytes());
          break;
      case(GL_LUMINANCE): // DEM ?! 
          WebPPictureImportRGBX(&picture, imageData, img.getRowSizeInBytes());
          break;
      default: return WriteResult::ERROR_IN_WRITING_FILE; break;
      }

      picture.writer = ReaderWriterWebP::ostream_writer;
      picture.custom_ptr = (void*)&fout;
      if (!WebPEncode(&config, &picture)) {
          return WriteResult::ERROR_IN_WRITING_FILE;
      }

      WebPPictureFree(&picture);
      return WriteResult::FILE_SAVED;
  }
};

// now register with Registry to instantiate the above
// reader/writer.
REGISTER_OSGPLUGIN(webp, ReaderWriterWebP)
