/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/XYZFeatureSource>
#include <osgEarth/XYZ>
#include <osgEarth/OgrUtils>
#include <osgEarth/FeatureCursor>
#include <osgEarth/Filter>
#include <osgEarth/MVT>
#include <osgEarth/Metrics>
#include <osgEarth/JsonUtils>
#include <osgDB/FileUtils>
#include <cpl_vsi.h>
#include <gdal.h>
#include <atomic>
#include <cmath>

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
    return conf;
}

void
XYZFeatureSource::Options::fromConfig(const Config& conf)
{
    conf.get("url", _url);
    conf.get("format", _format);
    conf.get("esri_geodetic", _esriGeodetic);
}

//........................................................................

REGISTER_OSGEARTH_LAYER(xyzfeatures, XYZFeatureSource);

OE_LAYER_PROPERTY_IMPL(XYZFeatureSource, URI, URL, url);
OE_LAYER_PROPERTY_IMPL(XYZFeatureSource, std::string, Format, format);
OE_LAYER_PROPERTY_IMPL(XYZFeatureSource, bool, EsriGeodetic, esriGeodetic);

namespace
{
    //! Identifies tile templates without relying on an extension (TileJSON URLs may have none).
    bool hasTileCoordinates(const std::string& url)
    {
        return url.find("{z}") != std::string::npos || url.find("{x}") != std::string::npos ||
            url.find("{y}") != std::string::npos || url.find("{-y}") != std::string::npos;
    }

    //! Reads a TileJSON zoom, using the specification's default for absent or invalid optional values.
    int tileJSONZoom(const Util::Json::Value& value, int fallback)
    {
        return value.isIntegral() && value.asDouble() >= 0.0 && value.asDouble() <= 30.0 ?
            value.asInt() : fallback;
    }

    //! Resolves vector TileJSON during open; retains explicit options and returns errors for unusable metadata.
    //! Derived defaults are not serialized, so reopening can resolve a moving metadata URL again.
    Status readTileJSON(const URI& uri, const osgDB::Options* readOptions, XYZFeatureSource::Options& options,
        std::string& tileTemplate, GeoExtent& bounds)
    {
        const ReadResult result = uri.readString(readOptions);
        if (result.failed())
            return Status(Status::ResourceUnavailable, "Cannot read TileJSON: " + result.getResultCodeString());

        Util::Json::Reader reader;
        Util::Json::Value root;
        if (!reader.parse(result.getString(), root, false) || !root.isObject() ||
            !root["tilejson"].isString() || root["tilejson"].asString().empty())
            return Status(Status::ConfigurationError, "Expected a TileJSON object with a tilejson version");

        const auto& tiles = root["tiles"];
        if (!tiles.isArray() || tiles.empty())
            return Status(Status::ConfigurationError, "TileJSON requires a nonempty tiles array");
        for (unsigned i = 0; i < tiles.size(); ++i)
        {
            if (!tiles[i].isString() || tiles[i].asString().empty())
                return Status(Status::ConfigurationError, "TileJSON tile endpoints must be nonempty strings");
        }

        // Endpoints are equivalent mirrors; use the first, resolving relative URLs against the metadata file.
        tileTemplate = URI(tiles[0u].asString(), URIContext(uri.full())).full();
        if (tileTemplate.find("{z}") == std::string::npos || tileTemplate.find("{x}") == std::string::npos ||
            (tileTemplate.find("{y}") == std::string::npos && tileTemplate.find("{-y}") == std::string::npos))
            return Status(Status::ConfigurationError, "TileJSON endpoint requires {z}, {x}, and {y} or {-y}");
        if (root["scheme"].isString() && root["scheme"].asString() == "tms")
            replaceIn(tileTemplate, "{y}", "{-y}");

        std::string format = "pbf";
        if (root["format"].isString())
            format = root["format"].asString();
        if (format == "mvt")
            format = "pbf";
        format = options.format().getOrUse(format);
        if (format != "pbf" && format != "json" && format != "gml" && format != "fgb")
            return Status(Status::ConfigurationError, "TileJSON feature format must be pbf, json, gml, or fgb");

        const int minLevel = tileJSONZoom(root["minzoom"], 0);
        const int maxLevel = tileJSONZoom(root["maxzoom"], 30);
        if (options.minLevel().getOrUse(minLevel) < 0 ||
            options.maxLevel().getOrUse(maxLevel) > 30 ||
            options.minLevel().getOrUse(minLevel) > options.maxLevel().getOrUse(maxLevel))
            return Status(Status::ConfigurationError, "TileJSON levels must satisfy 0 <= min_level <= max_level <= 30");

        const auto& box = root["bounds"];
        if (box.isArray() && box.size() == 4u)
        {
            bool valid = true;
            for (unsigned i = 0; i < 4u; ++i)
                valid = valid && box[i].isNumeric() && std::isfinite(box[i].asDouble());
            if (valid && box[0u].asDouble() >= -180.0 && box[2u].asDouble() <= 180.0 &&
                box[1u].asDouble() >= -90.0 && box[3u].asDouble() <= 90.0 &&
                box[0u].asDouble() <= box[2u].asDouble() && box[1u].asDouble() <= box[3u].asDouble())
                bounds = GeoExtent(SpatialReference::get("wgs84"), box[0u].asDouble(), box[1u].asDouble(),
                    box[2u].asDouble(), box[3u].asDouble());
        }

        options.format().setDefault(format);
        options.profile().setDefault(ProfileOptions("spherical-mercator"));
        options.minLevel().setDefault(minLevel);
        options.maxLevel().setDefault(maxLevel);
        options.attribution().setDefault(root["attribution"].isString() ? root["attribution"].asString() : "");
        return Status::NoError;
    }
}

Status
XYZFeatureSource::openImplementation()
{
    Status parent = super::openImplementation();
    if (parent.isError())
        return parent;

    if (options().url().get().empty())
        return Status(Status::ConfigurationError, "XYZ driver requires a URL");

    _template = options().url().get().full();
    const bool tileJSON = !hasTileCoordinates(_template);
    GeoExtent bounds;
    if (tileJSON)
    {
        Status status = readTileJSON(options().url().get(), getReadOptions(), options(), _template, bounds);
        if (status.isError())
            return status;
    }

    if (!tileJSON && !options().format().isSet())
    {
        return Status(Status::ConfigurationError, "XYZ driver requires a format (pbf, json, gml, fgb)");
    }

    // if the user didn't expressly set a feature profile, create one from the options
    if (tileJSON || getFeatureProfile() == nullptr)
    {
        FeatureProfile* fp = nullptr;

        // Try to get the results from the settings instead
        if (!tileJSON && !options().profile().isSet())
        {
            return Status(Status::ConfigurationError, "XYZ driver requires an explicit profile");
        }

        if (!tileJSON && (!options().minLevel().isSet() || !options().maxLevel().isSet()))
        {
            return Status(Status::ConfigurationError, "XYZ driver requires a min and max level");
        }

        osg::ref_ptr<const Profile> profile = Profile::create(options().profile().get());
        if (!profile.valid())
            return Status(Status::ConfigurationError, "XYZ driver profile is invalid");

        fp = new FeatureProfile(profile->getExtent());
        if (bounds.isValid())
        {
            // Coverage bounds must not change the global tile grid or its XYZ addressing.
            GeoExtent coverage = bounds.transform(profile->getSRS()).intersectionSameSRS(profile->getExtent());
            if (coverage.isValid())
                fp->setExtent(coverage);
        }
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
    _rotateChoices.clear();
    _rotateString.clear();
    _rotateStart = _template.find('[');
    _rotateEnd = _template.find(']');
    if (_rotateStart != std::string::npos && _rotateEnd != std::string::npos && _rotateEnd - _rotateStart > 1)
    {
        _rotateString = _template.substr(_rotateStart, _rotateEnd - _rotateStart + 1);
        _rotateChoices = _template.substr(_rotateStart + 1, _rotateEnd - _rotateStart - 1);
    }

    return Status::NoError;
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
        // FlatGeobuf has no registered content type; servers report generic binary, which would select MVT.
        if (options().format().value() == "fgb")
            mimeType = "application/flatgeobuf";
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
    if (mimeType == "application/flatgeobuf")
        return getFlatGeobufFeatures(data, features);

    if (mimeType == "application/x-protobuf" || mimeType == "application/vnd.mapbox-vector-tile" ||
        mimeType == "binary/octet-stream" || mimeType == "application/octet-stream")
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


bool
XYZFeatureSource::getFlatGeobufFeatures(const std::string& data, FeatureList& features) const
{
    // OGR reads FlatGeobuf only from a (virtual) file. Each call gets its own name, so concurrent tile
    // reads never share a buffer; the buffer is borrowed, not copied or freed by GDAL.
    static std::atomic_uint serial{ 0u };
    const std::string path = Stringify() << "/vsimem/osgearth_xyz_" << this << "_" << serial++ << ".fgb";
    VSILFILE* file = VSIFileFromMemBuffer(path.c_str(),
        reinterpret_cast<GByte*>(const_cast<char*>(data.data())), data.size(), FALSE);
    if (!file)
        return false;
    VSIFCloseL(file);

    bool ok = false;
    GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (ds)
    {
        OGRLayerH layer = GDALDatasetGetLayer(ds, 0);
        if (layer)
        {
            OGRFeatureFactory factory;
            // A FlatGeobuf carries its own CRS; without one, assume the tiling profile's SRS.
            OGRSpatialReferenceH handle = OGR_L_GetSpatialRef(layer);
            osg::ref_ptr<const SpatialReference> srs;
            if (handle)
                srs = SpatialReference::createFromHandle(handle);
            factory.srs = srs.valid() ? srs.get() : getFeatureProfile()->getSRS();
            factory.interp = getFeatureProfile()->geoInterp();
            factory.rewindPolygons = _options->rewindPolygons().defaultValue();
            // Schema columns exist on every feature; drop unset ones so attribute presence stays meaningful.
            factory.keepNullValues = false;

            const bool reproject = !factory.srs->isHorizEquivalentTo(getFeatureProfile()->getSRS());
            OGR_L_ResetReading(layer);
            OGRFeatureH next;
            while ((next = OGR_L_GetNextFeature(layer)) != nullptr)
            {
                osg::ref_ptr<Feature> f = factory.createFeature(next);
                if (f.valid())
                {
                    if (reproject)
                        f->transform(getFeatureProfile()->getSRS());
                    features.push_back(f.release());
                }
                OGR_F_Destroy(next);
            }
            ok = true;
        }
        GDALClose(ds);
    }
    else
    {
        OE_WARN << LC << "Error reading FlatGeobuf tile" << std::endl;
    }
    VSIUnlink(path.c_str());
    return ok;
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

        return XYZ::makeTileURI(location, options().url()->context(), _rotateString, _rotateChoices, _rotate_iter);
    }
    return URI();
}

bool
XYZFeatureSource::isWritable() const
{
    return options().url().isSet() &&
        (options().url()->isRemote() == false) && hasTileCoordinates(options().url()->full());
}

Status
XYZFeatureSource::insert(const TileKey& key, const FeatureList& features, bool overwrite)
{
    if (!isWritable())
        return Status(Status::ConfigurationError, "XYZ insert requires a local tile URL template");

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
