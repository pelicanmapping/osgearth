/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/ArcGISServer>
#include <osgEarth/Registry>
#include <osgEarth/JsonUtils>
#include <osgEarth/ImageToHeightFieldConverter>
#include <osgEarth/Notify>

using namespace osgEarth;

#undef LC
#define LC "[ArcGISServer] "

//........................................................................

ArcGISServer::MapServiceLayer::MapServiceLayer(int in_id,
                                               const std::string& in_name) :
id( in_id ),
name( in_name )
{
    //NOP
}

int
ArcGISServer::MapServiceLayer::getId() const
{
    return id;
}

const std::string&
ArcGISServer::MapServiceLayer::getName() const
{
    return name;
}

//........................................................................

ArcGISServer::TileInfo::TileInfo() :
is_valid( false ),
tile_size(0),
min_level(0),
max_level(0),
num_tiles_wide(0),
num_tiles_high(0)
{
    //NOP
}

ArcGISServer::TileInfo::TileInfo( int _tile_size, const std::string& _format, int _min_level, int _max_level, int _num_tiles_wide, int _num_tiles_high ) :
format( _format ),
tile_size( _tile_size ),
min_level( _min_level ),
max_level( _max_level ),
is_valid( true ),
num_tiles_wide(_num_tiles_wide),
num_tiles_high(_num_tiles_high)
{
    //NOP
}

ArcGISServer::TileInfo::TileInfo( const TileInfo& rhs ) :
format( rhs.format ),
tile_size( rhs.tile_size ),
min_level( rhs.min_level ),
max_level( rhs.max_level ),
is_valid( rhs.is_valid ),
num_tiles_wide( rhs.num_tiles_wide ),
num_tiles_high( rhs.num_tiles_high )
{
    //NOP
}

bool
ArcGISServer::TileInfo::isValid() const {
    return is_valid;
}

int
ArcGISServer::TileInfo::getTileSize() const {
    return tile_size;
}

const std::string&
ArcGISServer::TileInfo::getFormat() const {
    return format;
}

int
ArcGISServer::TileInfo::getMinLevel() const {
    return min_level;
}

int
ArcGISServer::TileInfo::getMaxLevel() const {
    return max_level;
}

int
ArcGISServer::TileInfo::getNumTilesWide() const {
    return num_tiles_wide;
}

int
ArcGISServer::TileInfo::getNumTilesHigh() const {
    return num_tiles_high;
}

//........................................................................

ArcGISServer::MapService::MapService() :
is_valid( false ),
tiled( false )
{
    //NOP
}

bool
ArcGISServer::MapService::isValid() const {
    return is_valid;
}

bool
ArcGISServer::MapService::isTiled() const {
    return tiled;
}

const Profile*
ArcGISServer::MapService::getProfile() const {
    return profile.get();
}

const ArcGISServer::TileInfo&
ArcGISServer::MapService::getTileInfo() const {
    return tile_info;
}

const std::string&
ArcGISServer::MapService::getCopyright() const
{
    return _copyright;
}

bool
ArcGISServer::MapService::init( const URI& _uri, const osgDB::ReaderWriter::Options* options )
{
    uri = _uri;
    std::string sep = uri.full().find('?') == std::string::npos ? "?" : "&";
    std::string json_url = uri.full() + sep + std::string("f=pjson");  // request the data in JSON format

    ReadResult r = URI(json_url).readString( options );
    if ( r.failed() )
        return setError( "Unable to read metadata from ArcGIS service" );

    Json::Value doc;
    Json::Reader reader;
//    if ( !reader.parse( response.getPartStream(0), doc ) )
    if ( !reader.parse( r.getString(), doc ) )
	{
        return setError( "Unable to parse metadata; invalid JSON" );
	}

    // Read the profile. We are using "fullExtent"; perhaps an option to use "initialExtent" instead?
	double xmin = 0.0;
	double ymin = 0.0;
	double xmax = 0.0;
	double ymax = 0.0;
	int srs = 0;

    Json::Value fullExtentValue = doc["fullExtent"];
	Json::Value extentValue = doc["extent"];
	std::string srsValue;

	// added a case for "extent" which can be removed if we want to fall back on initialExtent if fullExtent fails
    if ( !fullExtentValue.empty() )
	{
		// if "fullExtent" exists .. use that
		xmin = doc["fullExtent"].get("xmin", 0).asDouble();
		ymin = doc["fullExtent"].get("ymin", 0).asDouble();
		xmax = doc["fullExtent"].get("xmax", 0).asDouble();
		ymax = doc["fullExtent"].get("ymax", 0).asDouble();
		srs = doc["fullExtent"].get("spatialReference", osgEarth::Json::Value::null).get("wkid", 0).asInt();

		srsValue = doc["fullExtent"].get("spatialReference", osgEarth::Json::Value::null).get("wkt", "null").asString();

		OE_DEBUG << LC << "fullExtent discovered: xmin: " << xmin << ", ymin: " << ymin << ", xmax: " << xmax << ", ymax: " << ymax << ", srs: " << srs << std::endl;
	}
	else if( !extentValue.empty() )
	{
		// else if "extent" exists .. use that
		xmin = doc["extent"].get("xmin", 0).asDouble();
		ymin = doc["extent"].get("ymin", 0).asDouble();
		xmax = doc["extent"].get("xmax", 0).asDouble();
		ymax = doc["extent"].get("ymax", 0).asDouble();
		srs = doc["extent"].get("spatialReference", osgEarth::Json::Value::null).get("wkid", 0).asInt();

		srsValue = doc["extent"].get("spatialReference", osgEarth::Json::Value::null).get("wkt", "null").asString();

		OE_DEBUG << LC << "extent discovered: xmin: " << xmin << ", ymin: " << ymin << ", xmax: " << xmax << ", ymax: " << ymax << ", srs: " << srs << std::endl;
	}
	else
	{
		// else "initialExtent" must exist ..
		xmin = doc["initialExtent"].get("xmin", 0).asDouble();
		ymin = doc["initialExtent"].get("ymin", 0).asDouble();
		xmax = doc["initialExtent"].get("xmax", 0).asDouble();
		ymax = doc["initialExtent"].get("ymax", 0).asDouble();
		srs = doc["initialExtent"].get("spatialReference", osgEarth::Json::Value::null).get("wkid", 0).asInt();

		srsValue = doc["initialExtent"].get("spatialReference", osgEarth::Json::Value::null).get("wkt", "null").asString();

		OE_DEBUG << LC << "initialExtent discovered: xmin: " << xmin << ", ymin: " << ymin << ", xmax: " << xmax << ", ymax: " << ymax << ", srs: " << srs << std::endl;
	}

    //Assumes the SRS is going to be an EPSG code
    std::stringstream ss;
    ss << "epsg:" << srs;

	// we can create a valid spatial reference from the WKT/proj4 string .. here just check if x/y/min/max values are set & correct
    if ( ! (xmax > xmin && ymax > ymin /*&& srs != 0*/ ) )
    {
        return setError( "Map service does not define a full extent" );
    }

    // Check that the layers list is not empty
    Json::Value j_layers = doc["layers"];
    if (!j_layers.empty())
    {
        // not required to initialise a layer .. in any case this code does nothing
        for (unsigned int i = 0; i < j_layers.size(); i++)
        {
            Json::Value layer = j_layers[i];
            int id = i; // layer.get("id", -1).asInt();
            std::string name = layer["name"].asString();

            if (id >= 0 && !name.empty())
            {
                layers.push_back(MapServiceLayer(id, name));
            }
        }
    }

    tiled = false;
    std::string format = "png";
    int tile_rows = 256;
    int tile_cols = 256;
    int min_level = 0;
    int max_level = 25;
    int num_tiles_wide = 1;
    int num_tiles_high = 1;

    // Read the tiling schema
    Json::Value j_tileinfo = doc["tileInfo"];
    if ( !j_tileinfo.empty() )
    {
        tiled = true;

     //   return setError( "Map service does not define a tiling schema" );

        // TODO: what do we do if the width <> height?
        tile_rows = j_tileinfo.get( "rows", 0 ).asInt();
        tile_cols = j_tileinfo.get( "cols", 0 ).asInt();
        if ( tile_rows <= 0 && tile_cols <= 0 )
		{
            return setError( "Map service tile size not specified" );
		}

        format = j_tileinfo.get( "format", "" ).asString();
        if ( format.empty() )
		{
            return setError( "Map service tile schema does not specify an image format" );
		}

        Json::Value j_levels = j_tileinfo["lods"];
        if ( j_levels.empty() )
		{
            return setError( "Map service tile schema contains no LODs" );
		}

        min_level = INT_MAX;
        max_level = 0;
        for( unsigned int i=0; i<j_levels.size(); i++ )
        {
            int level = j_levels[i].get( "level", -1 ).asInt();
            if ( level >= 0 && level < min_level )
			{
                min_level = level;
			}
            if ( level >= 0 && level > max_level )
			{
                max_level = level;
			}
        }

        if (j_levels.size() > 0)
        {
            int l = j_levels[0u].get("level", -1).asInt();
            double res = j_levels[0u].get("resolution", 0.0).asDouble();
            num_tiles_wide = (int)osg::round((xmax - xmin) / (res * tile_cols));
            num_tiles_high = (int)osg::round((ymax - ymin) / (res * tile_cols));

            //In case the first level specified isn't level 0, compute the number of tiles at level 0
            for (int i = 0; i < l; i++)
            {
                num_tiles_wide /= 2;
                num_tiles_high /= 2;
            }

            //profile.setNumTilesWideAtLod0(num_tiles_wide);
            //profile.setNumTilesHighAtLod0(num_tiles_high);
        }
    }

	std::string ssStr;
	ssStr = ss.str();

	osg::ref_ptr< SpatialReference > spatialReference;

	 // if srs is in a non integer form .. find out what form it's in & create ..
	if(srs == 0)
	{
		OE_DEBUG << LC << "srsString: " << srsValue << std::endl;

		// create spatial reference from WKT string
		spatialReference = SpatialReference::create( srsValue );
	}
	else
	{
		// create spatial reference from epsg string (WKID)
		spatialReference = SpatialReference::create( ssStr );
	}

	// if spatial reference is valid .. create a profile
	if( spatialReference.valid() )
	{
		// create profile from spatial reference
		if ( spatialReference->isGeographic() )
		{
			// If we have a geographic SRS, just use the geodetic profile
            profile = Profile::create(Profile::GLOBAL_GEODETIC);
		}
		else if ( spatialReference->isMercator() )
		{
			// If we have a mercator SRS, just use the mercator profile
            profile = Profile::create(Profile::SPHERICAL_MERCATOR);
		}
		else
		{
			//It's not geodetic or mercator, so try to use the full extent
			profile = Profile::create(
				spatialReference.get(),
				xmin, ymin, xmax, ymax,
				num_tiles_wide,
				num_tiles_high);
		}
	}
	else
	{
		return setError( "Map service Spatial Reference INVALID" );
	}

	if( !profile.valid() )
	{
		return setError( "Map service could not create a valid profile" );
	}

    _copyright = trim(doc["copyrightText"].asString());

    // now we're good.
    tile_info = ArcGISServer::TileInfo( tile_rows, format, min_level, max_level, num_tiles_wide, num_tiles_high);
    is_valid = true;
    return is_valid;
}

bool
ArcGISServer::MapService::setError( const std::string& msg )
{
    error_msg = msg;
    return false;
}

const std::string&
ArcGISServer::MapService::getError() const
{
    return error_msg;
}

//........................................................................

void
ArcGISServerOptions::readFrom(const Config& conf)
{
    conf.get("url", url());
    conf.get("token", token());
    conf.get("format", format());
    conf.get("layers", layers());
}

void
ArcGISServerOptions::writeTo(Config& conf) const
{
    conf.set("url", url());
    conf.set("token", token());
    conf.set("format", format());
    conf.set("layers", layers());
}


Config
ArcGISServerImageLayer::Options::getConfig() const
{
    Config conf = ImageLayer::Options::getConfig();
    writeTo(conf);
    return conf;
}

void
ArcGISServerImageLayer::Options::fromConfig(const Config& conf)
{
    return readFrom(conf);
}

//........................................................................

REGISTER_OSGEARTH_LAYER(arcgisserverimage, ArcGISServerImageLayer);

OE_LAYER_PROPERTY_IMPL(ArcGISServerImageLayer, URI, URL, url);
OE_LAYER_PROPERTY_IMPL(ArcGISServerImageLayer, std::string, Token, token);
OE_LAYER_PROPERTY_IMPL(ArcGISServerImageLayer, std::string, Format, format);
OE_LAYER_PROPERTY_IMPL(ArcGISServerImageLayer, std::string, Layers, layers);

void
ArcGISServerImageLayer::init()
{
    ImageLayer::init();
}

ArcGISServerImageLayer::~ArcGISServerImageLayer()
{
    //nop
}

Status
ArcGISServerImageLayer::openImplementation()
{
    Status parent = ImageLayer::openImplementation();
    if (parent.isError())
        return parent;

    // Default cache policy to NO_CACHE for public services
    if (options().cachePolicy().isSet() == false &&
        options().url()->full().find("arcgisonline.com") != std::string::npos)
    {
        options().cachePolicy() = CachePolicy::NO_CACHE;
    }

    if (options().url().isSet() == false)
    {
        return Status(Status::ConfigurationError, "Missing required endpoint URL");
    }

    // add the security token to the URL if necessary:
    URI url = options().url().value();

    OE_DEBUG << LC << "Initial URL: " << url.full() << std::endl;

    // append token to url in query string is set
    if (options().token().isSet())
    {
        std::string token = options().token().value();
        if (!token.empty())
        {
            std::string sep = url.full().find('?') == std::string::npos ? "?" : "&";
            url = url.append(sep + std::string("token=") + token);
        }
    }
    else
    {
        OE_DEBUG << LC << "Token not set" << std::endl;
    }

    // append layers to url in query string is set .. format is show:1,2,3
    if (options().layers().isSet())
    {
        std::string layers = options().layers().value();
        OE_DEBUG << LC << "_Layers: " << layers << std::endl;
        if (!layers.empty())
        {
            std::string sep = url.full().find('?') == std::string::npos ? "?" : "&";
            url = url.append(sep + std::string("layers=show:") + layers);
        }
    }
    else
    {
        OE_DEBUG << LC << "Layer options not set" << std::endl;
    }

    OE_DEBUG << LC << "_map_service URL: " << url.full() << std::endl;

    // read map service metadata from the server
    if (!_map_service.init(url, getReadOptions()))
    {
        OE_INFO << LC << "_map_service.init failed: " << _map_service.getError() << std::endl;

        return Status(
            Status::ResourceUnavailable, Stringify()
            << "ArcGIS map service initialization failed: "
            << _map_service.getError());
    }

    // image format to request:
    _format = options().format().getOrUse(_map_service.getTileInfo().getFormat());
    _format = osgEarth::toLower(_format);
    if (_format.length() > 3 && _format.substr(0, 3) == "png")
    {
        _format = "png";
    }
    if (_format == "mixed")
    {
        _format = "";
    }
    if (!_format.empty())
    {
        _dot_format = "." + _format;
    }

    _copyright = _map_service.getCopyright();
    if (options().attribution().isSet() == false)
    {
        setAttribution(_copyright);
    }

    // set the tile size from the map service
    setTileSize(_map_service.getTileInfo().getTileSize());

    // establish a profile if we don't already have one:
    if (!getProfile())
    {
        const Profile* profile = NULL;

        if (_profileConf.isSet())
        {
            profile = Profile::create(_profileConf.get());
        }
        else if (_map_service.getProfile())
        {
            profile = _map_service.getProfile();
        }
        else
        {
            // finally, fall back on lat/long
            profile = Profile::create(Profile::GLOBAL_GEODETIC);
        }
        setProfile(profile);
    }

    addDataExtent(DataExtent(getProfile()->getExtent(), _map_service.getTileInfo().getMinLevel(), _map_service.getTileInfo().getMaxLevel()));

    return Status::NoError;
}

GeoImage
ArcGISServerImageLayer::createImageImplementation(const TileKey& key, ProgressCallback* progress) const
{
    std::stringstream buf;

    int level = key.getLevelOfDetail();

    unsigned int tile_x, tile_y;
    key.getTileXY(tile_x, tile_y);

    if (_map_service.isTiled())
    {
        buf << options().url()->full() << "/tile"
            << "/" << level
            << "/" << tile_y
            << "/" << tile_x << _dot_format;
    }
    else
    {
        const GeoExtent& ex = key.getExtent();

        buf << std::setprecision(16)
            << options().url()->full() << "/export"
            << "?bbox=" << ex.xMin() << "," << ex.yMin() << "," << ex.xMax() << "," << ex.yMax()
            << "&format=" << _format
            << "&size=256,256"
            << "&transparent=true"
            << "&f=image";
    }

    //Add the token if necessary
    if (options().token().isSet())
    {
        std::string token = options().token().value();
        if (!token.empty())
        {
            std::string str;
            str = buf.str();
            std::string sep = str.find('?') == std::string::npos ? "?" : "&";
            buf << sep << "token=" << token;
        }
    }

    //Add the layers if necessary
    if (!options().layers()->empty())
    {
        std::string str;
        str = buf.str();
        std::string sep = str.find('?') == std::string::npos ? "?" : "&";
        buf << sep << "layers=show:" << options().layers().value();
    }

    std::string bufStr;
    bufStr = buf.str();

    osg::Image* image = URI(bufStr, options().url()->context()).getImage(getReadOptions(), progress);

    return GeoImage(image, key.getExtent());
}


//........................................................................

Config
ArcGISServerElevationLayer::Options::getConfig() const
{
    Config conf = ElevationLayer::Options::getConfig();
    writeTo(conf);
    return conf;
}

void
ArcGISServerElevationLayer::Options::fromConfig(const Config& conf)
{
    readFrom(conf);
}

REGISTER_OSGEARTH_LAYER(arcgisserverelevation, ArcGISServerElevationLayer);

OE_LAYER_PROPERTY_IMPL(ArcGISServerElevationLayer, URI, URL, url);
OE_LAYER_PROPERTY_IMPL(ArcGISServerElevationLayer, std::string, Token, token);
OE_LAYER_PROPERTY_IMPL(ArcGISServerElevationLayer, std::string, Format, format);
OE_LAYER_PROPERTY_IMPL(ArcGISServerElevationLayer, std::string, Layers, layers);


void
ArcGISServerElevationLayer::init()
{
    ElevationLayer::init();
}

Status
ArcGISServerElevationLayer::openImplementation()
{
    Status parent = ElevationLayer::openImplementation();
    if (parent.isError())
        return parent;

    _imageLayer = new ArcGISServerImageLayer(options());

    // Initialize and open the image layer
    _imageLayer->setReadOptions(getReadOptions());
    Status status = _imageLayer->open();

    if (status.isError())
        return status;

    setProfile(_imageLayer->getProfile());
    DataExtentList dataExtents;
    _imageLayer->getDataExtents(dataExtents);
    setDataExtents(dataExtents);

    return Status::NoError;
}

Status
ArcGISServerElevationLayer::closeImplementation()
{
    if (_imageLayer.valid())
    {
        _imageLayer->close();
        _imageLayer = NULL;
    }
    return ElevationLayer::closeImplementation();
}

GeoHeightField
ArcGISServerElevationLayer::createHeightFieldImplementation(const TileKey& key, ProgressCallback* progress) const
{
    // Avoid accessing null _imageLayer in case layer was deleted
    if (getStatus().isError() || !_imageLayer)
      return GeoHeightField(getStatus());

    // Make an image, then convert it to a heightfield
    GeoImage image = _imageLayer->createImageImplementation(key, progress);
    if (image.valid())
    {
        ImageToHeightFieldConverter conv;
        osg::HeightField* hf = conv.convert(image.getImage());
        return GeoHeightField(hf, key.getExtent());
    }
    else
    {
        return GeoHeightField(image.getStatus());
    }
}




