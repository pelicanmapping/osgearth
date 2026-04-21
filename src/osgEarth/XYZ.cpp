/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "XYZ"
#include <osgEarth/Registry>
#include <osgEarth/ImageToHeightFieldConverter>
#include "MetaTile"

#include <osgDB/WriteFile>

using namespace osgEarth;
using namespace osgEarth::XYZ;

#undef LC
#define LC "[XYZ] "

//............................................................................

Status
XYZ::Driver::open(const URI& uri,
                  const std::string& format,
                  const osgDB::Options* readOptions)
{
    if (uri.empty())
    {
        return Status::Error(Status::ConfigurationError, "Valid URL is missing");
    }

    _template = uri.full();

    // Set up a rotating element in the template
    _rotateStart = _template.find('[');
    _rotateEnd = _template.find(']');
    if (_rotateStart != std::string::npos && _rotateEnd != std::string::npos && _rotateEnd - _rotateStart > 1)
    {
        _rotateString = _template.substr(_rotateStart, _rotateEnd - _rotateStart + 1);
        _rotateChoices = _template.substr(_rotateStart + 1, _rotateEnd - _rotateStart - 1);
    }

    _format = !format.empty() ? format : osgDB::getLowerCaseFileExtension(uri.base());

    return STATUS_OK;
}

osgEarth::ReadResult
XYZ::Driver::read(const URI& uri,
                  const TileKey& key, 
                  bool invertY,
                  ProgressCallback* progress,
                  const osgDB::Options* readOptions) const
{
    unsigned x, y;
    key.getTileXY(x, y);
    unsigned cols = 0, rows = 0;
    key.getProfile()->getNumTiles(key.getLevelOfDetail(), cols, rows);
    unsigned inverted_y = rows - y - 1;

    if (invertY == true)
    {
        y = inverted_y;
    }

    std::string location = _template;

    // support OpenLayers template style:
    replaceIn( location, "${x}", Stringify() << x );
    replaceIn( location, "${y}", Stringify() << y );
    replaceIn( location, "${-y}", Stringify() << inverted_y);
    replaceIn( location, "${z}", Stringify() << key.getLevelOfDetail() );


    // failing that, legacy osgearth style:
    replaceIn( location, "{x}", Stringify() << x );
    replaceIn( location, "{y}", Stringify() << y );
    replaceIn( location, "{-y}", Stringify() << inverted_y);
    replaceIn( location, "{z}", Stringify() << key.getLevelOfDetail() );

    std::string cacheKey;

    if ( !_rotateChoices.empty() )
    {
        cacheKey = location;
        unsigned index = (++_rotate_iter) % _rotateChoices.size();
        replaceIn( location, _rotateString, Stringify() << _rotateChoices[index] );
    }


    URI myUri( location, uri.context() );
    if ( !cacheKey.empty() )
    {
        myUri.setCacheKey(Cache::makeCacheKey(location, "uri"));
    }

    return myUri.getImage(readOptions, progress);
}

//........................................................................

Config
XYZImageLayer::Options::getConfig() const
{
    Config conf = super::getConfig();
    conf.set("url", _url);
    conf.set("format", _format);
    conf.set("invert_y", _invertY);
    conf.set("min_level", minLevel());
    conf.set("max_level", maxLevel());
    return conf;
}

void
XYZImageLayer::Options::fromConfig(const Config& conf)
{
    conf.get("url", _url);
    conf.get("format", _format);
    conf.get("invert_y", _invertY);
    conf.get("min_level", minLevel());
    conf.get("max_level", maxLevel());
}

//........................................................................

Config
XYZElevationLayer::Options::getConfig() const
{
    Config conf = super::getConfig();
    conf.set("url", _url);
    conf.set("format", _format);
    conf.set("invert_y", _invertY);
    conf.set("min_level", minLevel());
    conf.set("max_level", maxLevel());
    conf.set("elevation_encoding", _elevationEncoding);
    conf.set("stitch_edges", stitchEdges());
    return conf;
}

void
XYZElevationLayer::Options::fromConfig(const Config& conf)
{
    conf.get("url", _url);
    conf.get("format", _format);
    conf.get("invert_y", _invertY);
    conf.get("min_level", minLevel());
    conf.get("max_level", maxLevel());
    conf.get("elevation_encoding", _elevationEncoding);
    conf.get("interpretation", elevationEncoding()); // compat with QGIS
    conf.get("stitch_edges", stitchEdges());
}

//........................................................................

REGISTER_OSGEARTH_LAYER(xyzimage, XYZImageLayer);

OE_LAYER_PROPERTY_IMPL(XYZImageLayer, URI, URL, url);
OE_LAYER_PROPERTY_IMPL(XYZImageLayer, bool, InvertY, invertY);
OE_LAYER_PROPERTY_IMPL(XYZImageLayer, std::string, Format, format);

void
XYZImageLayer::init()
{
    ImageLayer::init();
}

void
XYZImageLayer::setProfile(const Profile* profile)
{
    ImageLayer::setProfile(profile);

    if (profile)
    {
        // update the options for proper serialization
        options().profile() = profile->toProfileOptions();
    }
}

Status
XYZImageLayer::openImplementation()
{
    Status parent = ImageLayer::openImplementation();
    if (parent.isError())
        return parent;
   
    Status status = _driver.open(
        options().url().get(),
        options().format().get(),        
        getReadOptions());

    if (status.isError())
        return status;

    if (!getProfile())
    {
        OE_INFO << LC << "No profile; assuming spherical-mercator" << std::endl;
        setProfile(Profile::create("spherical-mercator"));
    }

    DataExtentList dataExtents;
    DataExtent e(getProfile()->getExtent());
    // these copy the optional, retaining the set or unset state:
    e.minLevel() = options().minLevel();
    e.maxLevel() = options().maxLevel();
    dataExtents.emplace_back(e);

    setDataExtents(dataExtents);

    return Status::NoError;
}

GeoImage
XYZImageLayer::createImageImplementation(const TileKey& key, ProgressCallback* progress) const
{
    ReadResult r = _driver.read(
        options().url().get(),
        key,
        options().invertY() == true,
        progress,
        getReadOptions());

    if (r.succeeded())
        return GeoImage(r.releaseImage(), key.getExtent());
    else
        return GeoImage(Status(r.errorDetail()));
}

//........................................................................

REGISTER_OSGEARTH_LAYER(xyzelevation, XYZElevationLayer);

OE_LAYER_PROPERTY_IMPL(XYZElevationLayer, URI, URL, url);
OE_LAYER_PROPERTY_IMPL(XYZElevationLayer, bool, InvertY, invertY);
OE_LAYER_PROPERTY_IMPL(XYZElevationLayer, std::string, Format, format);
OE_LAYER_PROPERTY_IMPL(XYZElevationLayer, std::string, ElevationEncoding, elevationEncoding);
OE_LAYER_PROPERTY_IMPL(XYZElevationLayer, bool, StitchEdges, stitchEdges);
OE_LAYER_PROPERTY_IMPL(XYZElevationLayer, RasterInterpolation, Interpolation, interpolation);

void
XYZElevationLayer::init()
{
    ElevationLayer::init();
}

void
XYZElevationLayer::setProfile(const Profile* profile)
{
    // do not use profile after callin this function
    ElevationLayer::setProfile(profile);

    if (getProfile())
    {
        // update the options for proper serialization
        options().profile() = getProfile()->toProfileOptions();
    }
}

Status
XYZElevationLayer::openImplementation()
{
    Status parent = ElevationLayer::openImplementation();
    if (parent.isError())
        return parent;

    // Create an image layer under the hood. TMS fetch is the same for image and
    // elevation; we just convert the resulting image to a heightfield
    _imageLayer = new XYZImageLayer(options());

    // Initialize and open the image layer
    _imageLayer->setReadOptions(getReadOptions());
    Status status = _imageLayer->open();

    if (status.isError())
        return status;

    setProfile(_imageLayer->getProfile());

    if (!options().maxLevel().isSet())
    {
        OE_WARN << LC << "Warning, max_level not set on this layer, so it will default to "
            << options().maxLevel().value() << ". This may lead to poor performance or missing data." << std::endl;
    }

    DataExtentList de;
    _imageLayer->getDataExtents(de);
    setDataExtents(de);

    return Status::NoError;
}

GeoHeightField
XYZElevationLayer::createHeightFieldImplementation(const TileKey& key, ProgressCallback* progress) const
{
    if (!_imageLayer->mayHaveData(key))
    {
        return GeoHeightField::INVALID;
    }

    std::function<float(const osg::Vec4f&)> decode;
    
    if (options().elevationEncoding() == "mapbox")
    {
        decode = [](const osg::Vec4f& p) -> float
            {
                return -10000.0f + ((p.r() * 255.0f * 256.0f * 256.0f + p.g() * 255.0f * 256.0f + p.b() * 255.0f) * 0.1f);
            };
    }
    else if (options().elevationEncoding() == "terrarium")
    {
        decode = [](const osg::Vec4f& p) -> float
            {
                return (p.r() * 255.0f * 256.0f + p.g() * 255.0f + p.b() * 255.0f / 256.0f) - 32768.0f;
            };
    }
    else
    {
        decode = [](const osg::Vec4f& p) -> float
            {
                return p.r();
            };
    }

    if (options().stitchEdges() == true)
    {
        MetaTile<GeoImage> metaImage;
        metaImage.setCreateTileFunction([this](const TileKey& key, ProgressCallback* progress)
            {
                auto result = _stitchingCache.get_or_insert(
                    key,
                    [&](auto& new_value) {
                        GeoImage image = _imageLayer->createImage(key, progress);
                        if (image.valid())
                            new_value = image;
                    });

                if (result.has_value()) return result.value();
                else return GeoImage::INVALID;
            });

        metaImage.setCenterTileKey(key, progress);

        if (!metaImage.getCenterTile().valid() || !metaImage.getScaleBias().isIdentity())
        {
            return {};
        }

        osg::ref_ptr<osg::HeightField> hf = new osg::HeightField();
        hf->allocate(getTileSize(), getTileSize());
        std::fill(hf->getHeightList().begin(), hf->getHeightList().end(), NO_DATA_VALUE);

        double xmin, ymin, xmax, ymax;
        key.getExtent().getBounds(xmin, ymin, xmax, ymax);

        osg::Vec4f pixel;
        for (int c = 0; c < (int)getTileSize(); c++)
        {
            if (progress && progress->isCanceled())
                return {};

            double u = (double)c / (double)(getTileSize() - 1);
            double x = xmin + u * (xmax - xmin);
            for (int r = 0; r < (int)getTileSize(); r++)
            {
                double v = (double)r / (double)(getTileSize() - 1);
                double y = ymin + v * (ymax - ymin);
                if (metaImage.readAtCoord(pixel, x, y))
                {
                    hf->setHeight(c, r, decode(pixel));
                }
            }
        }

        return GeoHeightField(hf.release(), key.getExtent());
    }

    else
    {
        // Make an image, then convert it to a heightfield
        GeoImage geoImage = _imageLayer->createImageImplementation(key, progress);
        if (geoImage.valid())
        {
            const osg::Image* image = geoImage.getImage();

            // Allocate the heightfield.
            osg::HeightField* hf = new osg::HeightField();
            hf->allocate(image->s(), image->t());

            ImageUtils::PixelReader read(image);
            osg::Vec4f pixel;
            read.forEachPixel([&](auto& i)
                {
                    read(pixel, i.s(), i.t());
                    hf->setHeight(i.s(), i.t(), decode(pixel));
                });

            if (key.is(8, 70, 107))
            {
                ImageToHeightFieldConverter c;
                auto out = c.convert(hf, 32);
                osgDB::writeImageFile(*out, "test.tif");
            }

            return GeoHeightField(hf, key.getExtent());
        }

        return GeoHeightField::INVALID;
    }
}
