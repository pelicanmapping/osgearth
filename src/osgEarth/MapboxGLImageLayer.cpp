/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/MapboxGLImageLayer>
#include <osgEarth/Session>

#include <osgEarth/FeatureRasterizer>
#include <osgEarth/ArcGISTilePackage>
#include <osgEarth/XYZFeatureSource>
#include <osgEarth/MVT>
#include <osgEarth/Registry>

#include <osgDB/WriteFile>

using namespace osgEarth;
using namespace MapBoxGL;

#define LC "[MapboxGLImageLayer] " << getName() << ": "


REGISTER_OSGEARTH_LAYER(mapboxglimage, MapBoxGLImageLayer);
OE_LAYER_PROPERTY_IMPL(MapBoxGLImageLayer, URI, URL, url);
OE_LAYER_PROPERTY_IMPL(MapBoxGLImageLayer, std::string, Key, key);

void MapBoxGLImageLayer::setPixelScale(const float& value) {
    setOptionThatRequiresReopen(options().pixelScale(), value);
}

const float& MapBoxGLImageLayer::getPixelScale() const {
    return options().pixelScale().get();
}

void MapBoxGLImageLayer::setDisableText(const bool& value) {
    setOptionThatRequiresReopen(options().disableText(), value);
}

const bool& MapBoxGLImageLayer::getDisableText() const {
    return options().disableText().get();
}


void getIfSet(const Json::Value& object, const std::string& member, PropertyValue<float>& value)
{
    if (object.isMember(member))
    {
        auto jsonValue = object[member];
        if (!jsonValue.isObject())
        {
            value.setConstant((float)jsonValue.asDouble());
        }
        else
        {
            if (jsonValue.isMember("stops"))
            {
                Json::Value &stopsValue = jsonValue["stops"];
                if (stopsValue.isArray())
                {
                    PropertyExpression<float> expression;
                    for (unsigned int i = 0; i < stopsValue.size(); ++i)
                    {
                        float zoom = stopsValue[i][0u].asDouble();
                        float value = stopsValue[i][1u].asDouble();
                        expression.stops().emplace_back(zoom, value);
                    }
                    value.setExpression(expression);
                }
            }
        }
    }
}

void getIfSet(const Json::Value& object, const std::string& member, PropertyValue<Color>& value)
{
    if (object.isMember(member))
    {
        auto jsonValue = object[member];
        // Check for an object value  here as the line-width could be an array which is a series of "stops".  We don't support that yet.
        if (!jsonValue.isObject())
        {
            value.setConstant(Color(jsonValue.asString()));
        }
        else
        {
            if (jsonValue.isMember("stops"))
            {
                Json::Value &stopsValue = jsonValue["stops"];
                if (stopsValue.isArray())
                {
                    PropertyExpression<Color> expression;
                    for (unsigned int i = 0; i < stopsValue.size(); ++i)
                    {
                        float zoom = stopsValue[i][0u].asDouble();
                        Color value = Color(stopsValue[i][1u].asString());
                        expression.stops().emplace_back(zoom, value);
                    }
                    value.setExpression(expression);
                }
            }
        }
    }
}

void getIfSet(const Json::Value& object, const std::string& member, optional<std::string>& value)
{
    if (object.isMember(member))
    {
        const Json::Value& jsonValue = object[member];
        if (!jsonValue.isObject())
        {
            value = jsonValue.asString();
        }
    }
}

void getIfSet(const Json::Value& object, const std::string& member, optional<float>& value)
{
    if (object.isMember(member))
    {
        auto jsonValue = object.get(member, 0.0f);
        // Check for an object value  here as the line-width could be an array which is a series of "stops".  We don't support that yet.
        if (!jsonValue.isObject())
        {
            value = (float)object.get(member, 0.0f).asDouble();
        }
    }
}

void getIfSet(const Json::Value& object, const std::string& member, optional<double>& value)
{
    if (object.isMember(member) && object.isDouble())
    {
        auto jsonValue = object.get(member, 0.0f);
        // Check for an object value  here as the line-width could be an array which is a series of "stops".  We don't support that yet.
        if (!jsonValue.isObject())
        {
            value = object.get(member, 0.0).asDouble();
        }
    }
}

void getIfSet(const Json::Value& object, const std::string& member, optional<unsigned int>& value)
{
    if (object.isMember(member))
    {
        value = object.get(member, 0.0).asUInt();
    }
}

void getIfSet(const Json::Value& object, const std::string& member, optional<int>& value)
{
    if (object.isMember(member))
    {
        value = object.get(member, 0.0).asInt();
    }
}


/*************************/
MapBoxGL::StyleSheet::Source::Source()
{
}

std::string& MapBoxGL::StyleSheet::Source::attribution()
{
    return _attribution;
}

const std::string& MapBoxGL::StyleSheet::Source::attribution() const
{
    return _attribution;
}

std::string& MapBoxGL::StyleSheet::Source::url()
{
    return _url;
}

const std::string& MapBoxGL::StyleSheet::Source::url() const
{
    return _url;
}

std::string& MapBoxGL::StyleSheet::Source::type()
{
    return _type;
}

const std::string& MapBoxGL::StyleSheet::Source::type() const
{
    return _type;
}

std::string& MapBoxGL::StyleSheet::Source::name()
{
    return _name;
}

const std::string& MapBoxGL::StyleSheet::Source::name() const
{
    return _name;
}

const std::vector< std::string >& MapBoxGL::StyleSheet::Source::tiles() const
{
    return _tiles;
}

std::vector< std::string >& MapBoxGL::StyleSheet::Source::tiles()
{
    return _tiles;
}

FeatureSource* MapBoxGL::StyleSheet::Source::featureSource()
{
    return _featureSource.get();
}

const FeatureSource* MapBoxGL::StyleSheet::Source::featureSource() const
{
    return _featureSource.get();
}

void MapBoxGL::StyleSheet::Source::loadFeatureSource(const std::string& styleSheetURI, const osgDB::Options* options)
{
    if (!_featureSource.valid())
    {
        URIContext context(styleSheetURI);

        if (type() == "vector-vtpk")
        {
#ifdef OSGEARTH_HAVE_MVT
            URI uri(url(), context);

            osg::ref_ptr< VTPKFeatureSource > featureSource = new VTPKFeatureSource();
            featureSource->setReadOptions(options);
            featureSource->setURL(uri);
            if (featureSource->open().isError())
            {
                OE_WARN << "[MapBoxGLImageLayer] Failed to open: " << url() << std::endl;
            }
            _featureSource = featureSource.get();
#else
            OE_WARN << "[MapboxGLImageLayer] Cannot process 'vector-vtpk' because osgEarth was not compiled with Protobuf support" << std::endl;
#endif
        }
        else if (type() == "vector-mbtiles")
        {
#if defined(OSGEARTH_HAVE_MVT)
            URI uri(url(), context);

            osg::ref_ptr< MVTFeatureSource > featureSource = new MVTFeatureSource();
            featureSource->setReadOptions(options);
            featureSource->setURL(uri);
            if (featureSource->open().isError())
            {
                OE_WARN << "[MapBoxGLImageLayer] Failed to open: " << url() << std::endl;
            }
            _featureSource = featureSource.get();
#else
            OE_WARN << "[MapboxGLImageLayer] Cannot process 'vector-mbtiles' because osgEarth was not compiled with Protobuf and SQLITE support" << std::endl;
#endif
        }
        else if (type() == "vector")
        {
            std::string profileName = "spherical-mercator";
            bool isESRIGeodetic = false;

            if (!tiles().empty())
            {
                osg::ref_ptr< XYZFeatureSource > featureSource = new XYZFeatureSource;
                URI uri(tiles()[0], context);
                featureSource->setMinLevel(0);
                featureSource->setMaxLevel(22);
                featureSource->setURL(uri);
                featureSource->setFormat("pbf");
                featureSource->setReadOptions(options);
                // It's possible arcgis can specify a non mercator profile right in the source, but I've never seen one of those in the wild so don't know what it might look like.
                featureSource->options().profile() = ProfileOptions(profileName);
                if (featureSource->open().isError())
                {
                    OE_WARN << "[MapBoxGLImageLayer] Failed to open: " << *uri << std::endl;
                }
                _featureSource = featureSource.get();
            }
            else
            {
                // Read a tile json from the given URL
                URI tilesURI(url(), context);

                auto rr = tilesURI.readString(options);
                std::string data = rr.getString();

                Json::Reader reader;
                Json::Value root(Json::objectValue);
                if (reader.parse(data, root, false))
                {
                    if (root.isMember("tileInfo"))
                    {
                        if (root["tileInfo"].isMember("spatialReference"))
                        {
                            unsigned int wkid = root["tileInfo"]["spatialReference"].get("latestWkid", "3857").asUInt();
                            if (wkid == 4326)
                            {
                                // Only ESRI sources have a tileInfo block
                                profileName = "global-geodetic";
                                isESRIGeodetic = true;
                            }
                        }
                    }

                    if (root.isMember("tiles"))
                    {
                        std::string tilesetFull = tilesURI.full();

                        // Make sure the service URL ends with a /
                        std::stringstream buf;
                        buf << tilesetFull;
                        if (!endsWith(tilesetFull, "/")) buf << "/";

                        // We need the context relative to the tile json, not the stylesheet.
                        URIContext localContext(buf.str());

                        osg::ref_ptr< XYZFeatureSource > featureSource = new XYZFeatureSource;
                        URI uri(root["tiles"][0u].asString(), localContext);

                        featureSource->setMinLevel(0);
                        featureSource->setMaxLevel(22);
                        featureSource->setURL(uri);
                        featureSource->setFormat("pbf");
                        featureSource->setEsriGeodetic(isESRIGeodetic);
                        featureSource->setReadOptions(options);
                        featureSource->options().profile() = ProfileOptions(profileName); 
                        if (featureSource->open().isError())
                        {
                            OE_WARN << "[MapBoxGLImageLayer] Failed to open: " << *uri << std::endl;
                        }
                        _featureSource = featureSource.get();
                    }
                }
            }
        }
        else
        {
            OE_WARN << "[MapBoxGLImageLayer] Unexpected type: " << type() << std::endl;
        }
    }
}


/*************************/
Paint::Paint() :
    _fillAntialias(true),
    _visibility("visible")
{
}

PropertyValue<Color>& Paint::backgroundColor()
{
    return _backgroundColor;
}

const PropertyValue<Color>& Paint::backgroundColor() const
{
    return _backgroundColor;
}


PropertyValue<float>& Paint::backgroundOpacity()
{
    return _backgroundOpacity;
}

const PropertyValue<float>& Paint::backgroundOpacity() const
{
    return _backgroundOpacity;
}

PropertyValue<float>& Paint::lineWidth()
{
    return _lineWidth;
}

const optional<std::string>& Paint::textField() const
{
    return _textField;
}

optional<std::string>& Paint::textField()
{
    return _textField;
}

const optional<std::string>& Paint::textFont() const
{
    return _textFont;
}

optional<std::string>& Paint::textFont()
{
    return _textFont;
}

const optional<std::string>& Paint::textAnchor() const
{
    return _textAnchor;
}

optional<std::string>& Paint::textAnchor()
{
    return _textAnchor;
}

const PropertyValue<Color>& Paint::textColor() const
{
    return _textColor;
}

PropertyValue<Color>& Paint::textColor()
{
    return _textColor;
}

const PropertyValue<Color>& Paint::textHaloColor() const
{
    return _textHaloColor;
}

PropertyValue<Color>& Paint::textHaloColor()
{
    return _textHaloColor;
}

const PropertyValue<float>& Paint::textSize() const
{
    return _textSize;
}

PropertyValue<float>& Paint::textSize()
{
    return _textSize;
}

const PropertyValue<Color>& Paint::fillColor() const
{
    return _fillColor;
}

PropertyValue<Color>& Paint::fillColor()
{
    return _fillColor;
}

const optional<std::string>& Paint::fillPattern() const
{
    return _fillPattern;
}

optional<std::string>& Paint::fillPattern()
{
    return _fillPattern;
}

const PropertyValue<Color>& Paint::lineColor() const
{
    return _lineColor;
}

PropertyValue<Color>& Paint::lineColor()
{
    return _lineColor;
}


const PropertyValue<float>& Paint::lineWidth() const
{
    return _lineWidth;
}

const optional<std::string>& Paint::iconImage() const
{
    return _iconImage;
}

optional<std::string>& Paint::iconImage()
{
    return _iconImage;
}

const optional<std::string>& Paint::visibility() const
{
    return _visibility;
}

optional<std::string>& Paint::visibility()
{
    return _visibility;
}


/*************************/


MapBoxGL::StyleSheet::Layer::Layer()
{
}

const std::string& MapBoxGL::StyleSheet::Layer::id() const
{
    return _id;
}

std::string& MapBoxGL::StyleSheet::Layer::id()
{
    return _id;
}

std::string& MapBoxGL::StyleSheet::Layer::source()
{
    return _source;
}

const std::string& MapBoxGL::StyleSheet::Layer::source() const
{
    return _source;
}

const std::string& MapBoxGL::StyleSheet::Layer::sourceLayer() const
{
    return _sourceLayer;
}

std::string& MapBoxGL::StyleSheet::Layer::sourceLayer()
{
    return _sourceLayer;
}

const std::string& MapBoxGL::StyleSheet::Layer::type() const
{
    return _type;
}

std::string& MapBoxGL::StyleSheet::Layer::type()
{
    return _type;
}

const unsigned int& MapBoxGL::StyleSheet::Layer::minZoom() const
{
    return _minZoom;
}

unsigned int& MapBoxGL::StyleSheet::Layer::minZoom()
{
    return _minZoom;
}


unsigned int& MapBoxGL::StyleSheet::Layer::maxZoom()
{
    return _maxZoom;
}

const unsigned int& MapBoxGL::StyleSheet::Layer::maxZoom() const
{
    return _maxZoom;
}

Paint& MapBoxGL::StyleSheet::Layer::paint()
{
    return _paint;
}

const Paint& MapBoxGL::StyleSheet::Layer::paint() const
{
    return _paint;
}

MapBoxGL::StyleSheet::FilterExpression& MapBoxGL::StyleSheet::Layer::filter()
{
    return _filter;
}

const MapBoxGL::StyleSheet::FilterExpression& MapBoxGL::StyleSheet::Layer::filter() const
{
    return _filter;
}
/*************************/


const std::string& MapBoxGL::StyleSheet::version() const
{
    return _version;
}

const std::string& MapBoxGL::StyleSheet::name() const
{
    return _name;
}

const std::vector< MapBoxGL::StyleSheet::Layer >& MapBoxGL::StyleSheet::layers() const
{
    return _layers;
}

std::vector< MapBoxGL::StyleSheet::Layer >& MapBoxGL::StyleSheet::layers()
{
    return _layers;
}

const std::vector< MapBoxGL::StyleSheet::Source >& MapBoxGL::StyleSheet::sources() const
{
    return _sources;
}

std::vector< MapBoxGL::StyleSheet::Source >& MapBoxGL::StyleSheet::sources()
{
    return _sources;
}

const URI& MapBoxGL::StyleSheet::sprite() const
{
    return _sprite;
}

const URI& MapBoxGL::StyleSheet::glyphs() const
{
    return _glyphs;
}

const ResourceLibrary* MapBoxGL::StyleSheet::spriteLibrary() const
{
    return _spriteLibrary.get();
}

MapBoxGL::StyleSheet::StyleSheet()
{
}


MapBoxGL::StyleSheet MapBoxGL::StyleSheet::load(const URI& location, const osgDB::Options* options)
{
    MapBoxGL::StyleSheet styleSheet;

    auto rr = location.readString(options);
    std::string data = rr.getString();

    Json::Reader reader;
    Json::Value root(Json::objectValue);
    if (!reader.parse(data, root, false))
    {
        return styleSheet;
    }

    styleSheet._version = root.get("version", "").asString();
    styleSheet._name = root.get("name", "").asString();
    styleSheet._sprite = URI(root.get("sprite", "").asString(), URIContext(location.full()));
    styleSheet._glyphs = URI(root.get("glyphs", "").asString(), URIContext(location.full()));

    styleSheet._spriteLibrary = loadSpriteLibrary(styleSheet._sprite);

    if (root.isMember("sources"))
    {
        const Json::Value& s = root["sources"];
        for (Json::Value::const_iterator i = s.begin(); i != s.end(); ++i)
        {
            const Json::Value& sourceJson = (*i);

            Source source;
            source.name() = i.key().asString();

            if (sourceJson.isMember("url"))
            {
                source.url() = sourceJson.get("url", "").asString();
            }
            else if (sourceJson.isMember("tiles"))
            {
                const Json::Value& tilesJson = sourceJson["tiles"];
                for (Json::Value::const_iterator tileItr = tilesJson.begin(); tileItr != tilesJson.end(); ++tileItr)
                {
                    source.tiles().push_back((*tileItr).asString());
                }
            }
            source.attribution() = sourceJson.get("attribution", "").asString();
            source.type() = sourceJson.get("type", "").asString();
            source.loadFeatureSource(location.full(), options);
            styleSheet._sources.push_back(std::move(source));
        }
    }

    if (root.isMember("layers"))
    {
        const Json::Value& s = root["layers"];
        for (Json::Value::const_iterator i = s.begin(); i != s.end(); ++i)
        {
            const Json::Value& layerJson = (*i);
            Layer layer;
            layer.id() = layerJson.get("id", "").asString();
            layer.source() = layerJson.get("source", "").asString();
            layer.sourceLayer() = layerJson.get("source-layer", "").asString();
            layer.type() = layerJson.get("type", "").asString();
            layer.minZoom() = layerJson.get("minzoom", 0).asUInt();
            layer.maxZoom() = layerJson.get("maxzoom", 24).asUInt();

            // Parse paint
            if (layerJson.isMember("paint"))
            {
                const Json::Value& paint = layerJson["paint"];
                getIfSet(paint, "background-color", layer.paint().backgroundColor());
                getIfSet(paint, "background-opacity", layer.paint().backgroundOpacity());
                getIfSet(paint, "fill-color", layer.paint().fillColor());
                getIfSet(paint, "fill-pattern", layer.paint().fillPattern());

                getIfSet(paint, "line-color", layer.paint().lineColor());
                getIfSet(paint, "line-width", layer.paint().lineWidth());
                getIfSet(paint, "text-color", layer.paint().textColor());
                getIfSet(paint, "text-halo-color", layer.paint().textHaloColor());
            }

            // Parse layout
            if (layerJson.isMember("layout"))
            {
                const Json::Value& layout = layerJson["layout"];

                // TODO:  This is a layout property, not a paint property
                getIfSet(layout, "text-field", layer.paint().textField());
                getIfSet(layout, "text-size", layer.paint().textSize());
                getIfSet(layout, "text-anchor", layer.paint().textAnchor());
                // Just grab the first font for now from the fonts array.
                if (layout.isMember("text-font"))
                {
                    const Json::Value& jsonValue = layout["text-font"];
                    if (!jsonValue.isArray() && !jsonValue.isObject())
                    {
                        layer.paint().textFont() = jsonValue.asString();
                    }
                    else if (jsonValue.isArray())
                    {
                        layer.paint().textFont() = jsonValue[0u].asString();
                    }
                }

                getIfSet(layout, "icon-image", layer.paint().iconImage());
                getIfSet(layout, "visibility", layer.paint().visibility());
            }

            if (layerJson.isMember("filter"))
            {
               layer.filter()._filter = layerJson["filter"];
            }

            styleSheet._layers.emplace_back(std::move(layer));
        }
    }

    return styleSheet;
}

ResourceLibrary* MapBoxGL::StyleSheet::loadSpriteLibrary(const URI& sprite)
{
    ResourceLibrary* library = nullptr;
    URI uri(Stringify() << sprite.full() << ".json");

    URI imagePath(Stringify() << sprite.full() << ".png");
    osg::ref_ptr< osg::Image > image = imagePath.getImage();
    if (image.valid())
    {
        unsigned int imageWidth = image->s();
        unsigned int imageHeight = image->t();

        // Flip the image and convert the image to BGRA premultiplie alpha for blend2d so it doesn't need to do it each time an icon is rendered.
        image->flipVertical();
        ImageUtils::PixelReader imageReader(image.get());
        ImageUtils::PixelWriter imageWriter(image.get());

        for (int t = 0; t < image->t(); ++t)
        {
            for (int s = 0; s < image->s(); ++s)
            {
                osg::Vec4 color = imageReader(s, t);
                osg::Vec4 pma(color.b() * color.a(),
                    color.g() * color.a(),
                    color.r() * color.a(),
                    color.a());
                imageWriter(pma, s, t);
            }
        }

        auto data = uri.getString();
        Json::Reader reader;
        Json::Value root(Json::objectValue);
        if (reader.parse(data, root, false))
        {
            library = new ResourceLibrary("mapbox", "");
            for (Json::Value::iterator i = root.begin(); i != root.end(); ++i)
            {
                unsigned int x = (*i).get("x", 0).asUInt();
                unsigned int y = (*i).get("y", 0).asUInt();
                unsigned int width = (*i).get("width", 0).asUInt();
                unsigned int height = (*i).get("height", 0).asUInt();

                SkinResource* skin = new SkinResource();
                skin->name() = i.key().asString();
                skin->imageURI() = imagePath;
                skin->image() = image.get();
                skin->imageBiasS() = (float)x / (float)imageWidth;
                skin->imageBiasT() = (float)y / (float)imageHeight;
                skin->imageScaleS() = (float)width / (float)imageWidth;
                skin->imageScaleT() = (float)height / (float)imageHeight;
                library->addResource(skin);
            }
        }
    }
    else
    {
        OE_WARN << "Failed to load sprites from " << sprite.full() << std::endl;
    }

    return library;
}


Config
MapBoxGLImageLayer::Options::getConfig() const
{
    Config conf = ImageLayer::Options::getConfig();
    conf.set("url", _url);
    conf.set("key", _key);
    conf.set("pixel_scale", _pixelScale);
    conf.set("disable_text", _disableText);
    return conf;
}

void
MapBoxGLImageLayer::Options::fromConfig(const Config& conf)
{
    pixelScale().setDefault(1.0);
    disableText().setDefault(false);

    conf.get("url", url());
    conf.get("key", key());
    conf.get("pixel_scale", pixelScale());
    conf.get("disable_text", disableText());
}

void
MapBoxGLImageLayer::init()
{
    ImageLayer::init();

    // Default profile (WGS84) if not set
    if (!getProfile())
    {
        setProfile(Profile::create(Profile::GLOBAL_GEODETIC));
    }
}

Status
MapBoxGLImageLayer::openImplementation()
{
    Status parent = ImageLayer::openImplementation();
    if (parent.isError())
        return parent;

    _styleSheet = MapBoxGL::StyleSheet::load(getURL(), getReadOptions());

    if (!_styleSheet.glyphs().empty())
    {
        _glyphManager = new MapboxGLGlyphManager(_styleSheet.glyphs().full(), getKey(), getReadOptions());
    }
    // Compute the data extents
    if (!_styleSheet.layers().empty())
    {
        unsigned int minZoom = UINT_MAX;
        unsigned int maxZoom = 0;
        for (auto& l : _styleSheet.layers())
        {
            if (l.minZoom() < minZoom)
            {
                minZoom = l.minZoom();
            }

            if (l.maxZoom() > maxZoom)
            {
                maxZoom = l.maxZoom();
            }
        }

        DataExtentList dataExtents;
        dataExtents.push_back(DataExtent(getProfile()->getExtent(), minZoom, maxZoom));
        setDataExtents(dataExtents);        
    }

    return Status::NoError;
}


void
MapBoxGLImageLayer::addedToMap(const Map* map)
{
    ImageLayer::addedToMap(map);

    _map = map;
}

void
MapBoxGLImageLayer::removedFromMap(const Map* map)
{
    ImageLayer::removedFromMap(map);
}

struct LayeredFeatures
{
    std::unordered_map< std::string, FeatureList > features;
};


bool evalFilter(const Json::Value& filter, osgEarth::Feature* feature)
{
    if (!filter.isArray())
    {
        return false;
    }

    auto op = osgEarth::trim(filter[0u].asString());

    // https://docs.mapbox.com/mapbox-gl-js/style-spec/other/#other-filter

    // Comparison filters
    if (op == "all")
    {
        bool finalResult = true;
        for (unsigned int i = 1; i < filter.size(); ++i)
        {
            finalResult &= evalFilter(filter[i], feature);
            // Early out.
            if (!finalResult)
            {
                return false;
            }
        }
        return finalResult;
    }
    else if (op == "any")
    {
        bool finalResult = false;
        for (unsigned int i = 1; i < filter.size(); ++i)
        {
            finalResult |= evalFilter(filter[i], feature);
            // Early out.
            if (finalResult)
            {
                return true;
            }
        }
        return finalResult;
    }
    else if (op == "none")
    {
        bool finalResult = false;
        for (unsigned int i = 1; i < filter.size(); ++i)
        {
            finalResult &= !evalFilter(filter[i], feature);
            // Early out.
            if (finalResult)
            {
                return false;
            }
        }
        return finalResult;
    }
    // Existential filters
    else if (op == "has")
    {
        std::string key = filter[1u].asString();
        return feature->hasAttr(key);
    }
    else if (op == "!has")
    {
        std::string key = filter[1u].asString();
        return !feature->hasAttr(key);
    }

    // Comparison filters
    else if (op == "==")
    {
        std::string key = filter[1u].asString();
        const Json::Value& value = filter[2u];

        if (key != "$type" && !feature->hasAttr(key)) return false;

        if (key == "$type")
        {
            std::string typeString;
            if (feature->getGeometry()->getType() == Geometry::Type::TYPE_LINESTRING)
            {
                typeString = "LineString";
            }
            else if (feature->getGeometry()->getType() == Geometry::Type::TYPE_POLYGON)
            {
                typeString = "Polygon";
            }
            else if (feature->getGeometry()->getType() == Geometry::Type::TYPE_POINT || feature->getGeometry()->getType() == Geometry::Type::TYPE_POINTSET)
            {
                typeString = "Point";
            }
            return typeString == value.asString();
        }
        else if (value.isString())
        {
            return feature->getString(key) == value.asString();
        }
        else if (value.isBool())
        {
            return feature->getBool(key) == value.asBool();
        }
        else if (value.isDouble())
        {
            return feature->getDouble(key) == value.asDouble();
        }
        else if (value.isIntegral())
        {
            return feature->getInt(key) == value.asInt();
        }
        return false;
    }
    else if (op == "!=")
    {
        std::string key = filter[1u].asString();
        const Json::Value& value = filter[2u];

        if (!feature->hasAttr(key)) return true;

        if (value.isString())
        {
            return feature->getString(key) != value.asString();
        }
        else if (value.isBool())
        {
            return feature->getBool(key) != value.asBool();
        }
        else if (value.isDouble())
        {
            return feature->getDouble(key) != value.asDouble();
        }
        else if (value.isIntegral())
        {
            return feature->getInt(key) != value.asInt();
        }
        return false;
    }
    else if (op == ">")
    {
        std::string key = filter[1u].asString();
        const Json::Value& value = filter[2u];

        if (!feature->hasAttr(key)) return false;

        if (value.isString())
        {
            return feature->getString(key) > value.asString();
        }
        else if (value.isBool())
        {
            return feature->getBool(key) > value.asBool();
        }
        else if (value.isDouble())
        {
            return feature->getDouble(key) > value.asDouble();
        }
        else if (value.isIntegral())
        {
            return feature->getInt(key) > value.asInt();
        }
        return false;
    }
    else if (op == ">=")
    {
        std::string key = filter[1u].asString();
        const Json::Value& value = filter[2u];

        if (!feature->hasAttr(key)) return false;

        if (value.isString())
        {
            return feature->getString(key) >= value.asString();
        }
        else if (value.isBool())
        {
            return feature->getBool(key) >= value.asBool();
        }
        else if (value.isDouble())
        {
            return feature->getDouble(key) >= value.asDouble();
        }
        else if (value.isIntegral())
        {
            return feature->getInt(key) >= value.asInt();
        }
        return false;
    }
    else if (op == "<")
    {
        std::string key = filter[1u].asString();
        const Json::Value& value = filter[2u];

        if (!feature->hasAttr(key)) return false;

        if (value.isString())
        {
            return feature->getString(key) < value.asString();
        }
        else if (value.isBool())
        {
            return feature->getBool(key) < value.asBool();
        }
        else if (value.isDouble())
        {
            return feature->getDouble(key) < value.asDouble();
        }
        else if (value.isIntegral())
        {
            return feature->getInt(key) < value.asInt();
        }
        return false;
    }
    else if (op == "<=")
    {
        std::string key = filter[1u].asString();
        const Json::Value& value = filter[2u];

        if (!feature->hasAttr(key)) return false;

        if (value.isString())
        {
            return feature->getString(key) <= value.asString();
        }
        else if (value.isBool())
        {
            return feature->getBool(key) <= value.asBool();
        }
        else if (value.isDouble())
        {
            return feature->getDouble(key) <= value.asDouble();
        }
        else if (value.isIntegral())
        {
            return feature->getInt(key) <= value.asInt();
        }
        return false;
    }
    // Set membership filters
    else if (op == "in")
    {
        std::string key = filter[1u].asString();

        if (!feature->hasAttr(key)) return false;

        for (unsigned int i = 2; i < filter.size(); ++i)
        {
            const Json::Value& value = filter[i];

            if (value.isString())
            {
                if (feature->getString(key) == value.asString())
                {
                    return true;
                }
            }
            else if (value.isBool())
            {
                if (feature->getBool(key) == value.asBool())
                {
                    return true;
                }
            }
            else if (value.isDouble())
            {
                if (feature->getDouble(key) == value.asDouble())
                {
                    return true;
                }
            }
            else if (value.isIntegral())
            {
                if (feature->getInt(key) == value.asInt())
                {
                    return true;
                }
            }
        }
        return false;
    }
    else if (op == "!in")
    {
        std::string key = filter[1u].asString();

        std::string featureValue = feature->getString(key);

        if (!feature->hasAttr(key))
        {
            return true;
        }

        for (unsigned int i = 2; i < filter.size(); ++i)
        {
            const Json::Value& value = filter[i];

            if (value.isString())
            {
                if (feature->getString(key) == value.asString())
                {
                    return false;
                }
            }
            else if (value.isBool())
            {
                if (feature->getBool(key) == value.asBool())
                {
                    return false;
                }
            }
            else if (value.isDouble())
            {
                if (feature->getDouble(key) == value.asDouble())
                {
                    return false;
                }
            }
            else if (value.isIntegral())
            {
                if (feature->getInt(key) == value.asInt())
                {
                    return false;
                }
            }
        }
        return true;
    }

    return true;
}

GeoImage
MapBoxGLImageLayer::createImageImplementation(const TileKey& key, ProgressCallback* progress) const
{
    if (getStatus().isError())
    {
        return GeoImage::INVALID;
    }

    // Find the background layer and get the background color

    Color backgroundColor = Color::Transparent;
    for (auto& layer : _styleSheet.layers())
    {
        if (layer.type() == "background")
        {
            backgroundColor = Color(layer.paint().backgroundColor().evaluate(key.getLOD()));
            backgroundColor.a() = layer.paint().backgroundOpacity().evaluate(key.getLOD());
            break;
        }
    }


    unsigned int numFeaturesRendered = 0;

    FeatureRasterizer featureRasterizer(getTileSize(), getTileSize(), key.getExtent(), backgroundColor);
    featureRasterizer.setGlyphManager(_glyphManager.get());
    featureRasterizer.setPixelScale(getPixelScale());

    osg::ref_ptr< StyleSheet > styleSheet = new StyleSheet;
    if (_styleSheet.spriteLibrary())
    {
        styleSheet->addResourceLibrary(const_cast<ResourceLibrary*>(_styleSheet.spriteLibrary()));
    }
    osg::ref_ptr< Session > session = new Session(_map.get(), styleSheet.get(), nullptr, nullptr);

    std::unordered_map< std::string, LayeredFeatures > sourceToFeatures;

    for (auto& layer : _styleSheet.layers())
    {
        // Skip layers with visibility none
        if (layer.paint().visibility() == "none")
        {
            continue;
        }

        //if (key.getLevelOfDetail() >= layer.minZoom() && key.getLevelOfDetail() <= layer.maxZoom())
        if (key.getLevelOfDetail() >= layer.minZoom() && key.getLevelOfDetail() < layer.maxZoom())
        {
            osg::ref_ptr< FeatureSource > featureSource;

            for (auto& s : _styleSheet.sources())
            {
                if (s.name() == layer.source())
                {
                    featureSource = const_cast<FeatureSource*>(s.featureSource());
                    break;
                }
            }
            if (!featureSource.valid())
            {
                continue;
            }

            FilterContext context(session.get(), featureSource->getFeatureProfile(), key.getExtent());

            // TODO:  Maybe you don't need to zoom up for each layer, only once per feature source.

            LayeredFeatures layeredFeatures;
            // See if we already got the features for this tile for this source
            auto featuresItr = sourceToFeatures.find(layer.source());
            if (featuresItr == sourceToFeatures.end())
            {
                FeatureList allFeatures;
                TileKey queryKey = key;
                while (allFeatures.empty() && queryKey.valid())
                {
                    // If the requested key isn't valid we avoid requesting the neighbor keys for metatiling and just fallback on the parent key
                    // to avoid ending up with empty tiles.
                    bool centerSampleValid = false;

                    {
                        // Get the features for this tile
                        osg::ref_ptr< FeatureCursor > cursor = featureSource->createFeatureCursor(queryKey, {}, nullptr, progress);
                        if (progress && progress->isCanceled())
                        {
                            return GeoImage::INVALID;
                        }

                        if (cursor.valid())
                        {
                            cursor->fill(allFeatures);
                            centerSampleValid = !allFeatures.empty();
                        }
                    }

                    if (centerSampleValid)
                    {
                        unsigned int numWide, numHigh;
                        queryKey.getProfile()->getNumTiles(queryKey.getLevelOfDetail(), numWide, numHigh);

                        for (unsigned x = queryKey.getTileX() - 1; x <= queryKey.getTileX() + 1; ++x)
                        {
                            for (unsigned y = queryKey.getTileY() - 1; y <= queryKey.getTileY() + 1; ++y)
                            {
                                if (x < 0 || x >= numWide || y < 0 || y >= numHigh || (x == queryKey.getTileX() && y == queryKey.getTileY())) continue;

                                TileKey sampleKey(queryKey.getLevelOfDetail(), x, y, queryKey.getProfile());

                                if (key.valid())
                                {
                                    osg::ref_ptr< FeatureCursor > cursor = featureSource->createFeatureCursor(sampleKey, {}, nullptr, progress);
                                    if (progress && progress->isCanceled())
                                    {
                                        return GeoImage::INVALID;
                                    }

                                    if (cursor.valid())
                                    {
                                        cursor->fill(allFeatures);
                                    }
                                }
                            }
                        }
                    }

                    if (allFeatures.empty())
                    {
                        queryKey = queryKey.createParentKey();
                        /*
                        if (queryKey.getLevelOfDetail() < featureSource->getFeatureProfile()->getMaxLevel())
                        {
                            std::cout << "Not underzooming " << key.str() << " to " << queryKey.str() << " with a max level of " << featureSource->getFeatureProfile()->getMaxLevel() << std::endl;
                            break;
                        }
                        */
                    }
                }

                for (auto& f : allFeatures)
                {
                    layeredFeatures.features[f->getString("mvt_layer")].push_back(f.get());
                }

                sourceToFeatures[layer.source()] = layeredFeatures;
            }
            else
            {
                layeredFeatures = featuresItr->second;
            }


            if (layeredFeatures.features.find(layer.sourceLayer()) != layeredFeatures.features.end())
            {
                // Run any filters on the layer.
                FeatureList features;
                if (!layer.filter()._filter.empty())
                {
                    for (auto& f : layeredFeatures.features[layer.sourceLayer()])
                    {
                        if (evalFilter(layer.filter()._filter, f.get()))
                        {
                            features.push_back(f.get());
                        }
                    }
                }
                else
                {
                    features = layeredFeatures.features[layer.sourceLayer()];
                }

                if (features.empty())
                {
                    continue;
                }

                if (layer.type() == "fill")
                {
                    if (layer.paint().fillPattern().isSet())
                    {
                        OE_INFO << LC << "Skipping layer " << layer.id() << " with fill-pattern=" << layer.paint().fillPattern().get() << ".  Fill patterns are not supported" << std::endl;
                        continue;
                    }
                    Style style;
                    style.getOrCreateSymbol<PolygonSymbol>()->fill() = Color(layer.paint().fillColor().evaluate(key.getLOD()));
                    featureRasterizer.render(features, style, context);
                        //features,
                        //style,
                        //featureSource->getFeatureProfile(),
                        //session->styles());
                    numFeaturesRendered += features.size();
                }
                else if (layer.type() == "line")
                {
                    Style style;
                    style.getOrCreateSymbol<LineSymbol>()->stroke().mutable_value().color() = layer.paint().lineColor().evaluate(key.getLOD());
                    style.getOrCreateSymbol<LineSymbol>()->stroke().mutable_value().width() = Distance(layer.paint().lineWidth().evaluate(key.getLOD()), Units::PIXELS);
                    featureRasterizer.render(features, style, context);
                        //features,
                        //style,
                        //featureSource->getFeatureProfile(),
                        //session->styles());
                    numFeaturesRendered += features.size();
                }
                else if (layer.type() == "symbol")
                {
                    Style style;
                    if (!getDisableText())
                    {
                        if (layer.paint().textField().isSet())
                        {
                            style.getOrCreateSymbol<TextSymbol>()->content() = layer.paint().textField().get();
                        }
                        style.getOrCreateSymbol<TextSymbol>()->fill().mutable_value().color() = layer.paint().textColor().evaluate(key.getLOD());
                        style.getOrCreateSymbol<TextSymbol>()->halo().mutable_value().color() = layer.paint().textHaloColor().evaluate(key.getLOD());
                        style.getOrCreateSymbol<TextSymbol>()->size().mutable_value().setLiteral(layer.paint().textSize().evaluate(key.getLOD()));

                        TextSymbol::Alignment alignment = TextSymbol::ALIGN_CENTER_CENTER;
                        if (layer.paint().textAnchor().isSet())
                        {
                            std::string anchor = layer.paint().textAnchor().get();
                            if (anchor == "center") alignment = TextSymbol::ALIGN_CENTER_CENTER;
                            else if (anchor == "left") alignment = TextSymbol::ALIGN_LEFT_CENTER;
                            else if (anchor == "right") alignment = TextSymbol::ALIGN_RIGHT_CENTER;
                            else if (anchor == "top") alignment = TextSymbol::ALIGN_CENTER_TOP;
                            else if (anchor == "bottom") alignment = TextSymbol::ALIGN_CENTER_BOTTOM;
                            else if (anchor == "top-left") alignment = TextSymbol::ALIGN_LEFT_TOP;
                            else if (anchor == "top-right") alignment = TextSymbol::ALIGN_RIGHT_TOP;
                            else if (anchor == "bottom-left") alignment = TextSymbol::ALIGN_LEFT_BOTTOM;
                            else if (anchor == "bottom-right") alignment = TextSymbol::ALIGN_RIGHT_BOTTOM;                        
                        }
                        style.getOrCreateSymbol<TextSymbol>()->alignment() = alignment;

                        if (layer.paint().textFont().isSet())
                        {
                            style.getOrCreateSymbol<TextSymbol>()->font() = layer.paint().textFont().get();
                        }
                    }

                    if (layer.paint().iconImage().isSet())
                    {
                        style.getOrCreateSymbol<SkinSymbol>()->library() = "mapbox";
                        style.getOrCreateSymbol<SkinSymbol>()->name() = StringExpression(layer.paint().iconImage().get());

                    }

                    featureRasterizer.render(features, style, context);
                        //features,
                        //style,
                        //featureSource->getFeatureProfile(),
                        //session->styles());

                    numFeaturesRendered += features.size();
                }
            }
        }
    }

    if (numFeaturesRendered > 0)
    {
        return featureRasterizer.finalize();
    }
    return GeoImage::INVALID;
}
