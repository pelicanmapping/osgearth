/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/XYZFeatureSource>
#include <osgEarth/OgrUtils>
#include <osgEarth/FeatureCursor>
#include <osgEarth/Filter>
#include <osgEarth/MVT>
#include <osgEarth/Metrics>
#include <osgDB/FileUtils>

#define LC "[XYZFeatureSource] " << getName() << " : "

using namespace osgEarth;

#define OE_DEVEL OE_NULL

//........................................................................

Config
XYZFeatureSource::Options::getConfig() const
{
    Config conf = super::Options::getConfig();
    conf.set("url", _url);
    conf.set("format", _format);
    conf.set("esri_geodetic", _esriGeodetic);
    conf.set("auto_fallback", _autoFallback);
    return conf;
}

void
XYZFeatureSource::Options::fromConfig(const Config& conf)
{
    conf.get("url", _url);
    conf.get("format", _format);
    conf.get("esri_geodetic", _esriGeodetic);
    conf.get("auto_fallback", _autoFallback);
}

//........................................................................

REGISTER_OSGEARTH_LAYER(xyzfeatures, XYZFeatureSource);

OE_LAYER_PROPERTY_IMPL(XYZFeatureSource, URI, URL, url);
OE_LAYER_PROPERTY_IMPL(XYZFeatureSource, std::string, Format, format);
OE_LAYER_PROPERTY_IMPL(XYZFeatureSource, bool, EsriGeodetic, esriGeodetic);
OE_LAYER_PROPERTY_IMPL(XYZFeatureSource, bool, AutoFallbackToMaxLevel, autoFallback);

Status
XYZFeatureSource::openImplementation()
{
    if (!options().format().isSet())
    {
        return Status(Status::ConfigurationError, "XYZ driver requires a format (pbf, json, gml)");
    }

    // if the user didn't expressly set a feature profile, create one from the options
    if (getFeatureProfile() == nullptr)
    {
        FeatureProfile* fp = nullptr;

        // Try to get the results from the settings instead
        if (!options().profile().isSet())
        {
            return Status(Status::ConfigurationError, "XYZ driver requires an explicit profile");
        }

        if (!options().minLevel().isSet() || !options().maxLevel().isSet())
        {
            return Status(Status::ConfigurationError, "XYZ driver requires a min and max level");
        }

        osg::ref_ptr<const Profile> profile = Profile::create(options().profile().get());

        fp = new FeatureProfile(profile->getExtent());
        fp->setFirstLevel(options().minLevel().get());
        fp->setMaxLevel(options().maxLevel().get());
        fp->setTilingProfile(profile.get());

        if (options().geoInterp().isSet())
        {
            fp->geoInterp() = options().geoInterp().get();
        }

        setFeatureProfile(fp);
    }

    // set up the URL template
    _template = options().url()->full();
    _rotateStart = _template.find('[');
    _rotateEnd = _template.find(']');
    if (_rotateStart != std::string::npos && _rotateEnd != std::string::npos && _rotateEnd - _rotateStart > 1)
    {
        _rotateString = _template.substr(_rotateStart, _rotateEnd - _rotateStart + 1);
        _rotateChoices = _template.substr(_rotateStart + 1, _rotateEnd - _rotateStart - 1);
    }

    return super::openImplementation();
}

void
XYZFeatureSource::init()
{
    super::init();

    _rotateStart = 0.0;
    _rotateEnd = 0.0;
}


FeatureCursor*
XYZFeatureSource::createFeatureCursorImplementation(const Query& query, ProgressCallback* progress) const
{
    OE_PROFILING_ZONE;

    // Must be a tiled request.
    if (!query.tileKey().isSet())
        return nullptr;

    // Check the parent first
    FeatureCursor* result = nullptr;

    URI uri = createURL(query);
    if (uri.empty())
        return nullptr;

    //OE_DEVEL << LC << uri.full() << std::endl;

    // read the data:
    ReadResult r = uri.readString(getReadOptions(), progress);

    const std::string& data = r.getString();
    const Config&      meta = r.metadata();

    bool dataOK = false;

    FeatureList features;
    if (!data.empty())
    {
        // Get the mime-type from the metadata record if possible
        std::string mimeType = r.metadata().value(IOMetadata::CONTENT_TYPE);
        //If the mimetype is empty then try to set it from the format specification
        if (mimeType.empty())
        {
            if (options().format().value() == "json")
                mimeType = "json";
            else if (options().format().value().compare("gml") == 0)
                mimeType = "text/xml";
            else if (options().format().value().compare("pbf") == 0)
                mimeType = "application/x-protobuf";
        }
        dataOK = getFeatures(data, *query.tileKey(), mimeType, features);
    }

    if (dataOK)
    {
        OE_DEVEL << LC << "Tile " << query.tileKey()->str() << " read " << features.size() << " features" << std::endl;
    }

    result = dataOK ? new FeatureListCursor(std::move(features)) : nullptr;

    return result;
}

bool
XYZFeatureSource::getFeatures(const std::string& data, const TileKey& key, const std::string& mimeType, FeatureList& features) const
{
    if (mimeType == "application/x-protobuf" || mimeType == "binary/octet-stream" || mimeType == "application/octet-stream")
    {
#ifdef OSGEARTH_HAVE_MVT
        return MVT::readTile(data, key, features, options().layers());
#else
        if (getStatus().isOK())
        {
            setStatus(Status::ResourceUnavailable, "osgEarth is not built with MVT/PBF support");
            OE_WARN << LC << getStatus().message() << std::endl;
        }
        return false;
#endif
    }
    else
    {
        bool json = isJSON(mimeType);
        bool gml = isGML(mimeType);

        // find the right driver for the given mime type
        OGRSFDriverH ogrDriver =
            json ? OGRGetDriverByName("GeoJSON") :
            gml ? OGRGetDriverByName("GML") :
            0L;

        // fail if we can't find an appropriate OGR driver:
        if (!ogrDriver)
        {
            OE_WARN << LC << "Error reading response; cannot grok content-type \"" << mimeType << "\""
                << std::endl;
            return false;
        }

        OGRDataSourceH ds = OGROpen(data.c_str(), FALSE, &ogrDriver);

        if (!ds)
        {
            OE_WARN << LC << "Error reading response" << std::endl;
            return false;
        }

        // read the feature data.
        OGRLayerH layer = OGR_DS_GetLayer(ds, 0);
        if (layer)
        {
            bool keepNullValues = true;

            OGRFeatureFactory factory;
            factory.srs = getFeatureProfile()->getSRS();
            factory.interp = getFeatureProfile()->geoInterp();
            factory.rewindPolygons = _options->rewindPolygons().defaultValue();

            if (json)
            {
                // GeoJSON is always WGS84 according to spec
                // https://datatracker.ietf.org/doc/html/rfc7946
                factory.srs = osgEarth::SpatialReference::create("wgs84");

                // for GeoJSON we will discard null value properties since
                // the data doesn't have a schema
                factory.keepNullValues = false;
            }

            OGR_L_ResetReading(layer);
            OGRFeatureH feat_handle;
            while ((feat_handle = OGR_L_GetNextFeature(layer)) != NULL)
            {
                if (feat_handle)
                {
                    osg::ref_ptr<Feature> f = factory.createFeature(feat_handle);

                    if (f.valid())
                    {
                        if (factory.srs != getFeatureProfile()->getSRS())
                        {
                            f->transform(getFeatureProfile()->getSRS());
                        }

                        features.push_back(f.release());
                    }
                    OGR_F_Destroy(feat_handle);
                }
            }
        }

        // Destroy the datasource
        OGR_DS_Destroy(ds);
    }

    return true;
}


std::string
XYZFeatureSource::getExtensionForMimeType(const std::string& mime) const
{
    //OGR is particular sometimes about the extension of files when it's reading them so it's good to have
    //the temp file have an appropriate extension
    if ((mime.compare("text/xml") == 0) ||
        (mime.compare("text/xml; subtype=gml/2.1.2") == 0) ||
        (mime.compare("text/xml; subtype=gml/3.1.1") == 0)
        )
    {
        return ".xml";
    }
    else if ((mime.compare("application/json") == 0) ||
        (mime.compare("json") == 0) ||

        (mime.compare("application/x-javascript") == 0) ||
        (mime.compare("text/javascript") == 0) ||
        (mime.compare("text/x-javascript") == 0) ||
        (mime.compare("text/x-json") == 0)
        )
    {
        return ".json";
    }
    return "";
}

bool
XYZFeatureSource::isGML(const std::string& mime) const
{
    return
        startsWith(mime, "text/xml");
}


bool
XYZFeatureSource::isJSON(const std::string& mime) const
{
    return
        (mime.compare("application/json") == 0) ||
        (mime.compare("json") == 0) ||

        (mime.compare("application/x-javascript") == 0) ||
        (mime.compare("text/javascript") == 0) ||
        (mime.compare("text/x-javascript") == 0) ||
        (mime.compare("text/x-json") == 0);
}

URI
XYZFeatureSource::createURL(const Query& query) const
{
    if (query.tileKey().isSet() && query.tileKey()->valid())
    {
        TileKey key = query.tileKey().get();
        
        if ((int)key.getLOD() > getMaxLevel())
            key = key.createAncestorKey(getMaxLevel());

        unsigned int tileX = key.getTileX();
        unsigned int tileY = key.getTileY();
        unsigned int level = key.getLevelOfDetail();

        // ESRI geodetic sources have an odd geodetic profile which has level 0 as a 360x360 single tile which then splits into a quadtree
        // The tiling scheme matches up exactly with osgEarth's global geodetic profile if you increment the level by 1, treating ESRI's level 1
        // as our level 0.
        if (*options().esriGeodetic())
        {
            level += 1;
        }

        unsigned int numRows, numCols;
        key.getProfile()->getNumTiles(key.getLevelOfDetail(), numCols, numRows);
        unsigned inverted_tileY = numRows - tileY - 1;

        std::string location = _template;

        // support OpenLayers template style:
        replaceIn(location, "${x}", Stringify() << tileX);
        replaceIn(location, "${y}", Stringify() << tileY);
        replaceIn(location, "${-y}", Stringify() << inverted_tileY);
        replaceIn(location, "${z}", Stringify() << level);

        // failing that, legacy osgearth style:
        replaceIn(location, "{x}", Stringify() << tileX);
        replaceIn(location, "{y}", Stringify() << tileY);
        replaceIn(location, "{-y}", Stringify() << inverted_tileY);
        replaceIn(location, "{z}", Stringify() << level);

        std::string cacheKey;

        if (!_rotateChoices.empty())
        {
            cacheKey = location;
            unsigned index = (++_rotate_iter) % _rotateChoices.size();
            replaceIn(location, _rotateString, Stringify() << _rotateChoices[index]);
        }


        URI uri(location, options().url()->context());
        if (!cacheKey.empty())
        {
            uri.setCacheKey(Cache::makeCacheKey(location, "uri"));
        }

        return uri;
    }
    return URI();
}

bool
XYZFeatureSource::isWritable() const
{
    return options().url().isSet() &&
        (options().url()->isRemote() == false);
}

Status
XYZFeatureSource::insert(const TileKey& key, const FeatureList& features, bool overwrite)
{
    OE_SOFT_ASSERT_AND_RETURN(key.valid(), Status::AssertionFailure);
    OE_SOFT_ASSERT_AND_RETURN((int)key.getLOD() >= getMinLevel(), Status::ConfigurationError);
    OE_SOFT_ASSERT_AND_RETURN((int)key.getLOD() <= getMaxLevel(), Status::ConfigurationError);
    OE_SOFT_ASSERT_AND_RETURN(ci_equals(options().format().value(), "json"), Status::ConfigurationError);

    auto url = createURL(Query(key));

    if (isJSON(options().format().value()))
    {
        if (overwrite == false && osgDB::fileExists(url.full()))
        {
            return Status(Status::ConfigurationError, "Tile already exists and overwrite disabled");
        }

        if (osgDB::makeDirectoryForFile(url.full()))
        {
            auto geojson = Feature::featuresToGeoJSON(features);
            std::ofstream out(url.full().c_str());
            out.write(geojson.c_str(), geojson.length());
            out.close();
            return Status::NoError;
        }
        else
        {
            return Status(Status::ResourceUnavailable, "Failed to create directory for file");
        }
    }

    return Status(Status::ConfigurationError, "Bad JSON");
}
