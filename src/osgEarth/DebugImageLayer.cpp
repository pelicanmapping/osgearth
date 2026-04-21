/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "DebugImageLayer"
#include <osgEarth/Registry>
#include <osgEarth/ImageUtils>
#include <osgEarth/StringUtils>
#include <osgEarth/Geometry>
#include <osgText/Glyph>
#include <osgText/Font>
#include <osg/PolygonMode>
#include <osg/PolygonOffset>
#include <sstream>

// do this once FeatureRasterizer supports all the TextSymbol properties
//#define USE_FEATURE_RASTERIZER

#ifdef USE_FEATURE_RASTERIZER
#include "FeatureRasterizer"
#else
#include "GeometryRasterizer"
#endif

using namespace osgEarth;
using namespace osgEarth::Util;

#undef LC
#define LC "[Debug] "

//........................................................................
namespace osgEarth { namespace Debug
{
    static osg::Vec4 colors[4] = {
        osg::Vec4(1,0,0,1),
        osg::Vec4(0,1,0,1),
        osg::Vec4(0,0,1,1),
        osg::Vec4(1,0,1,1)
    };

    void copySubImageAndColorize( const osg::Image* src, osg::Image* dst, unsigned dx, unsigned dy, const osg::Vec4f& newColor)
    {
        ImageUtils::PixelReader read(src);
        ImageUtils::PixelWriter write(dst);

        // for one-channel images, use the RED channel, otherwise ALPHA channel
        unsigned chan = src->getPixelFormat() == GL_RED || src->getPixelFormat() == GL_LUMINANCE? 0 : 3;

        osg::Vec4f color;

        for( int src_t=0, dst_t=dy; src_t < src->t(); src_t++, dst_t++ )
        {
            for( int src_s=0, dst_s=dx; src_s < src->s(); src_s++, dst_s++ )
            {           
                read(color, src_s, src_t);
                if ( color[chan] > 0.5f )
                    color = newColor;
                write( color, dst_s, dst_t );
            }
        }
    }

} } // namespace osgEarth::Debug

//........................................................................

Config
DebugImageLayer::Options::getMetadata()
{
    return Config::readJSON(R"(
        { "name" : "Debug",
          "properties": [
            { "name": "color_code", "description": "", "type": "string", "default": "" },
            { "name": "invert_y", "description": "Whether to invert the tilekey Y coordinate", "type": "boolean", "default": "false" },
          ]
        } )");
}

Config
DebugImageLayer::Options::getConfig() const
{
    Config conf = ImageLayer::Options::getConfig();
    conf.set("color", _colorCode);
    conf.set("invert_y", _invertY);
    conf.set("show_tessellation", showTessellation());
    return conf;
}

void
DebugImageLayer::Options::fromConfig(const Config& conf)
{
    conf.get("color", _colorCode);
    conf.get("invert_y", _invertY);
    conf.get("show_tessellation", showTessellation());
}

//........................................................................

REGISTER_OSGEARTH_LAYER(debugimage, DebugImageLayer);
REGISTER_OSGEARTH_LAYER(debug, DebugImageLayer);

void
DebugImageLayer::init()
{
    if (!options().morphImagery().isSet())
    {
        options().morphImagery().setDefault(false);
    }

    super::init();

    _geom = new Ring();
    _geom->push_back(osg::Vec3(5, 5, 0));
    _geom->push_back(osg::Vec3(250, 5, 0));
    _geom->push_back(osg::Vec3(250, 250, 0));
    _geom->push_back(osg::Vec3(5, 250, 0));
    _font = Registry::instance()->getDefaultFont();

    // disable caching for the debugging layer.
    layerHints().cachePolicy() = CachePolicy::NO_CACHE;

    // set a default name
    if (getName().empty())
        setName("Debug");

    if (options().showTessellation() == true)
    {
        osg::StateSet* ss = getOrCreateStateSet();
        ss->setAttributeAndModes(new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::LINE), 1);
        ss->setAttributeAndModes(new osg::PolygonOffset(-1, -1), 1);
        _tessImage = ImageUtils::createOnePixelImage(Color(Color::Yellow, 0.75f));
    }
}

void
DebugImageLayer::setProfile(const Profile* profile)
{
    //OE_SOFT_ASSERT_AND_RETURN(!isOpen(), void(), "Cannot set profile after layer is open");

    super::setProfile(profile);
}

Status
DebugImageLayer::openImplementation()
{
    Status parent = ImageLayer::openImplementation();
    if (parent.isError())
        return parent;

    _color = osgEarth::htmlColorToVec4f(options().colorCode().get());

    return Status::NoError;
}

void
DebugImageLayer::addedToMap(const Map* map)
{
    if (!getProfile())
    {
        setProfile(map->getProfile());
    }
    super::addedToMap(map);
}

GeoImage
DebugImageLayer::createImageImplementation(const TileKey& key, ProgressCallback* progress) const
{
    if (options().showTessellation() == true)
    {
        return GeoImage(_tessImage.get(), key.getExtent());
    }

#ifdef USE_FEATURE_RASTERIZER

    FeatureRasterizer ras(256, 256, key.getExtent());

    const GeoExtent& e = key.getExtent();
    double bx = e.width() / 20.0, by = e.height() / 20.0;
    Ring* ring = new Ring();
    ring->push_back(osg::Vec3d(e.xMin() + bx, e.yMin() + by, 0));
    ring->push_back(osg::Vec3d(e.xMax() - bx, e.yMin() + by, 0));
    ring->push_back(osg::Vec3d(e.xMax() - bx, e.yMax() - by, 0));
    ring->push_back(osg::Vec3d(e.xMin() + bx, e.yMax() - by, 0));
    Feature* f = new Feature(ring, e.getSRS());
    FeatureList features{ f };
    Style lineStyle;
    lineStyle.getOrCreate<LineSymbol>()->stroke()->color() = Debug::colors[key.getLOD() % 4];
    ras.render(features, lineStyle);   
    
    // next render the text:
    std::stringstream buf;
    if (options().invertY() == true)
    {
        // TMS format (inverted Y)
        unsigned int tileX, tileY;
        key.getTileXY(tileX, tileY);
        unsigned int numRows, numCols;
        key.getProfile()->getNumTiles(key.getLevelOfDetail(), numCols, numRows);
        tileY = numRows - tileY - 1;
        buf << key.getLevelOfDetail() << "/" << tileX << "/" << tileY;
    }
    else
    {
        buf << key.str();
    }

    buf << std::fixed << std::setprecision(1)
        << "\nh=" << e.height(Units::METERS)
        << "m\nw=" << e.width(Units::METERS)
        << "m";


    PointSet* point = new PointSet();
    point->push_back(osg::Vec3d(e.xMin() + bx * 2, e.yMin() + by * 2, 0));
    f->setGeometry(point);
    Style textStyle;
    TextSymbol* text = textStyle.getOrCreate<TextSymbol>();
    text->content() = StringExpression(buf.str());
    text->fill() = Color::White;
    text->halo() = Color::Gray;
    ras.render(features, textStyle);

    GeoImage result = ras.finalize();

    return result;

#else

    // first draw the colored outline:
    GeometryRasterizer rasterizer(256, 256);
    rasterizer.draw(_geom.get(), Debug::colors[key.getLevelOfDetail() % 4]);
    osg::Image* image = rasterizer.finalize();

    // next render the tile key text:
    std::stringstream buf;
    //Print out a TMS key for the TileKey
    unsigned int tileX, tileY;
    key.getTileXY(tileX, tileY);
    unsigned int numRows, numCols;
    key.getProfile()->getNumTiles(key.getLevelOfDetail(), numCols, numRows);
    auto tmsY = numRows - tileY - 1;
    buf << "\nTMS-Y: " << tmsY << "\nXYZ-Y: " << tileY << "\nX: " << tileX << "\nLOD: " << key.getLOD();

    const GeoExtent& e = key.getExtent();

    buf << std::fixed << std::setprecision(1) 
        << "\nH: " << e.height(Units::METERS)
        << "m\nW: " << e.width(Units::METERS)
        << "m";

    std::string text;
    text = buf.str();

    unsigned x = 10, y = 10;

    int res = 24;
    osgText::FontResolution resolution(res, res);
    for (unsigned i = 0; i < text.length(); ++i)
    {
        if (text[i] == '\n')
        {
            y += res + 10;
            x = 10;
        }
        else if (text[i] == ' ')
        {
            x += res / 2;
        }
        else if (_font.valid())
        {
            //TODO: if SDF is in play, need to 'render' the SDF to a normal image
            // in copySubImageAndColorize -gw
            osgText::Glyph* glyph = _font->getGlyph(resolution, text[i]);
            Debug::copySubImageAndColorize(glyph, image, x, y, _color);
            x += glyph->s() + 1;
        }
    }

    return GeoImage(image, key.getExtent());
#endif
}
