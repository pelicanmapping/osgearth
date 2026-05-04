/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include <osgEarth/SpatialReference>
#include <osgEarth/Registry>
#include <osgEarth/Cube>
#include <osgEarth/LocalTangentPlane>
#include <osgEarth/Math>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

#define LC "[SpatialReference] "

using namespace osgEarth;

namespace
{
    std::string
    getOGRAttrValue( void* _handle, const std::string& name, int child_num = 0, bool lowercase =false)
    {
        const char* val = OSRGetAttrValue( static_cast<OGRSpatialReferenceH>(_handle), name.c_str(), child_num );
        if ( val )
        {
            return lowercase ? toLower(val) : val;
        }
        return "";
    } 

    void geodeticToGeocentric(std::vector<osg::Vec3d>& points, const Ellipsoid& em)
    {
        for( unsigned i=0; i<points.size(); ++i )
        {
            points[i] = em.geodeticToGeocentric(points[i]);
        }
    }

    void geocentricToGeodetic(std::vector<osg::Vec3d>& points, const Ellipsoid& em)
    {
        for( unsigned i=0; i<points.size(); ++i )
        {
            points[i] = em.geocentricToGeodetic(points[i]);
        }
    }

    // Make a MatrixTransform suitable for use with a Locator object based on the given extents.
    // Calling Locator::setTransformAsExtents doesn't work with OSG 2.6 due to the fact that the
    // _inverse member isn't updated properly.  Calling Locator::setTransform works correctly.
    osg::Matrixd
    getTransformFromExtents(double minX, double minY, double maxX, double maxY)
    {
        osg::Matrixd transform;
        transform.set(
            maxX - minX, 0.0, 0.0, 0.0,
            0.0, maxY - minY, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            minX, minY, 0.0, 1.0);
        return transform;
    }

    bool
    transformPointZ(
        osg::Vec3d& point,
        const SpatialReference* inputSRS,
        const SpatialReference* outputSRS,
        bool pointIsLatLong)
    {
        const VerticalDatum* inVDatum = inputSRS->getVerticalDatum();
        const VerticalDatum* outVDatum = outputSRS->getVerticalDatum();

        // same vdatum, no xformation necessary.
        if (inVDatum == outVDatum)
            return true;

        UnitsType inUnits = inVDatum ? inVDatum->getUnits() : Units::METERS;
        UnitsType outUnits = outVDatum ? outVDatum->getUnits() : inUnits;

        osg::Vec3d geoPoint;
        const osg::Vec3d* latLongPoint = &point;

        if (!inputSRS->isGeographic() && !pointIsLatLong)
        {
            geoPoint = point;
            if (!inputSRS->transform(geoPoint, inputSRS->getGeographicSRS(), geoPoint))
                return false;

            latLongPoint = &geoPoint;
        }

        if (inVDatum)
        {
            // to HAE:
            point.z() = inVDatum->msl2hae(latLongPoint->y(), latLongPoint->x(), point.z());
        }

        // do the units conversion:
        point.z() = inUnits.convertTo(outUnits, point.z());

        if (outVDatum)
        {
            // to MSL:
            point.z() = outVDatum->hae2msl(latLongPoint->y(), latLongPoint->x(), point.z());
        }

        return true;
    }

    bool
    transformSphericalMercatorXY(
        osg::Vec3d& point,
        const SpatialReference* inputSRS,
        const SpatialReference* outputSRS)
    {
        const double radius = 6378137.0;

        if (inputSRS->isGeographic() &&
            outputSRS->isSphericalMercator() &&
            inputSRS->isHorizEquivalentTo(outputSRS->getGeographicSRS()))
        {
            double lon_deg = inputSRS->getUnits().convertTo(Units::DEGREES, point.x());
            double lat_deg = inputSRS->getUnits().convertTo(Units::DEGREES, point.y());

            if (lat_deg <= -90.0 || lat_deg >= 90.0)
                return false;

            point.x() = Units::METERS.convertTo(outputSRS->getUnits(), radius * deg2rad(lon_deg));
            point.y() = Units::METERS.convertTo(
                outputSRS->getUnits(),
                radius * log(tan(osg::PI_4 + deg2rad(lat_deg) * 0.5)));

            return true;
        }

        if (inputSRS->isSphericalMercator() &&
            outputSRS->isGeographic() &&
            outputSRS->isHorizEquivalentTo(inputSRS->getGeographicSRS()))
        {
            double x_m = inputSRS->getUnits().convertTo(Units::METERS, point.x());
            double y_m = inputSRS->getUnits().convertTo(Units::METERS, point.y());
            double lon_deg = rad2deg(x_m / radius);
            double lat_deg = rad2deg(2.0 * atan(exp(y_m / radius)) - osg::PI_2);

            point.x() = Units::DEGREES.convertTo(
                outputSRS->getUnits(),
                osg::clampBetween(lon_deg, -180.0, 180.0));
            point.y() = Units::DEGREES.convertTo(
                outputSRS->getUnits(),
                osg::clampBetween(lat_deg, -90.0, 90.0));

            return true;
        }

        return false;
    }
}

//------------------------------------------------------------------------

SpatialReference::ThreadLocal::ThreadLocal() :
    _handle(nullptr),
    _workspace(nullptr),
    _workspaceSize(0u),
    _threadId(std::this_thread::get_id())
{
    //nop
}

SpatialReference::ThreadLocal::~ThreadLocal()
{
    if (_workspace)
        delete [] _workspace;

    // Causing a crash under GDAL3/PROJ6 - comment out until further notice
    // This only happens on program exist anyway
#if GDAL_VERSION_MAJOR < 3
    for(auto& xformEntry : _xformCache)
    {
        optional<TransformInfo>& ti = xformEntry.second;
        if (ti.isSet() && ti->_handle != nullptr)
            OCTDestroyCoordinateTransformation(ti->_handle);
    }
#endif

    if (_handle)
    {
        OSRDestroySpatialReference(static_cast<OGRSpatialReferenceH>(_handle));
    }
}

SpatialReference*
SpatialReference::create( const std::string& horiz, const std::string& vert )
{
    osg::ref_ptr<SpatialReference> srs = Registry::instance()->getOrCreateSRS( Key(horiz, vert) );
    return srs.release();
}

SpatialReference*
SpatialReference::createFromKey(const SpatialReference::Key& key)
{
    osg::ref_ptr<SpatialReference> srs;

    if (key.horizLower == "unified-cube")
        srs = new Contrib::CubeSpatialReference(key);
    else
        srs = new SpatialReference(key);

    return (srs.valid() && srs->valid()) ? srs.release() : nullptr;
}

SpatialReference::SpatialReference(void* handle) :
    _valid(true),
    _initialized(false),
    _domain(GEOGRAPHIC),
    _is_mercator(false),
    _is_north_polar(false),
    _is_south_polar(false),
    _is_cube(false),
    _is_user_defined(false),
    _is_ltp(false),
    _is_spherical_mercator(false),
    _ellipsoidId(0u)
{
    _setup.srcHandle = handle;

    // force handle creation
    init();
}

SpatialReference::SpatialReference(const Key& key) :
    _valid(true),
    _initialized(false),
    _domain(GEOGRAPHIC),
    _is_mercator(false),
    _is_north_polar(false),
    _is_south_polar(false),
    _is_cube(false),
    _is_user_defined(false),
    _is_ltp(false),
    _is_spherical_mercator(false),
    _ellipsoidId(0u)
{
    // shortcut for spherical-mercator:
    // https://wiki.openstreetmap.org/wiki/EPSG:3857
    if (key.horizLower == "spherical-mercator" ||
        key.horizLower == "global-mercator"    ||
        key.horizLower == "web-mercator"       ||
        key.horizLower == "epsg:3857"          ||
        key.horizLower == "epsg:900913"        ||
        key.horizLower == "epsg:102100"        ||
        key.horizLower == "epsg:102113"        ||
        key.horizLower == "epsg:3785"          ||
        key.horizLower == "epsg:3587"          ||
        key.horizLower == "osgeo:41001")
    {
        // note the use of nadgrids=@null (see http://proj.maptools.org/faq.html)
        _setup.name = "Spherical Mercator";
        _setup.type = INIT_PROJ;
#if(GDAL_VERSION_MAJOR >= 3)
        _setup.horiz = "+proj=webmerc +a=6378137 +b=6378137 +lat_ts=0.0 +lon_0=0.0 +x_0=0.0 +y_0=0 +k=1.0 +units=m +nadgrids=@null +towgs84=0,0,0,0,0,0,0 +wktext +no_defs";
#else
        _setup.horiz = "+proj=merc +a=6378137 +b=6378137 +lat_ts=0.0 +lon_0=0.0 +x_0=0.0 +y_0=0 +k=1.0 +units=m +nadgrids=@null +towgs84=0,0,0,0,0,0,0 +wktext +no_defs";
#endif
        _setup.vert = key.vertLower;
    }

    // true ellipsoidal ("world") mercator:
    // https://epsg.io/3395
    // https://gis.stackexchange.com/questions/259121/transformation-functions-for-epsg3395-projection-vs-epsg3857
    else if (
        key.horizLower == "world-mercator" ||
        key.horizLower == "epsg:3395" ||
        key.horizLower == "epsg:54004" ||
        key.horizLower == "epsg:9804" ||
        key.horizLower == "epsg:3832")
    {
        _setup.name = "World Mercator (WGS84)";
        _setup.type = INIT_PROJ;
        _setup.horiz = "+proj=merc +lon_0=0 +k=1 +x_0=0 +y_0=0 +ellps=WGS84 +datum=WGS84 +units=m +no_defs";
        _setup.vert = key.vertLower;
    }

    // common WGS84:
    else if(
        key.horizLower == "epsg:4326" ||
        key.horizLower == "wgs84" ||
        key.horizLower == "wgs_1984")
    {
        _setup.name = "WGS84";
        _setup.type = INIT_PROJ;
        _setup.horiz = "+proj=longlat +ellps=WGS84 +datum=WGS84 +no_defs";
        _setup.vert = key.vertLower;
    }

    // WGS84 Plate Carre:
    else if (
        key.horizLower == "plate-carre" || 
        key.horizLower == "plate-carree")
    {
        // https://proj4.org/operations/projections/eqc.html
        _setup.name = "Plate Carree";
        _setup.type = INIT_PROJ;
        _setup.horiz = "+proj=eqc +lat_ts=0 +lat_0=0 +lon_0=0 +x_0=0 +y_0=0 +units=m +ellps=WGS84 +datum=WGS84 +no_defs";
        _setup.vert = key.vertLower;
    }

    else if (
        key.horizLower == "moon" ||
        key.horizLower == "esri104903")
    {
        _setup.name = "Moon";
        _setup.type = INIT_PROJ;
        _setup.horiz = "+proj=longlat +R=1737400 +no_defs +units=m +type=crs";
        _setup.vert = key.vertLower;
    }

    else if (
        key.horizLower.find("+proj=longlat") == 0 &&
        key.horizLower.find("+R=1737400") != std::string::npos)
    {
        _setup.name = "Moon";
        _setup.type = INIT_PROJ;
        _setup.horiz = key.horiz;
        _setup.vert = key.vertLower;
    }

    // custom srs for the unified cube
    else if (
        key.horizLower == "unified-cube" )
    {
        _setup.name = "Unified Cube";
        _setup.type = INIT_USER;
        _setup.horiz = "+proj=longlat +ellps=WGS84 +datum=WGS84 +no_defs";
        _is_cube = true;
    }

    else if (
        key.horizLower.find( '+' ) == 0 )
    {
        //_setup.name = key.horiz;
        _setup.type = INIT_PROJ;
        _setup.horiz = key.horiz;
    }
    else if (
        key.horizLower.find( "epsg:" )  == 0 ||
        key.horizLower.find( "osgeo:" ) == 0 )
    {
        _setup.name = key.horiz;
        _setup.type = INIT_PROJ;
        _setup.horiz = std::string("+init=") + key.horiz;
    }
    else if (
        key.horizLower.find( "projcs" ) == 0 || 
        key.horizLower.find( "geogcs" ) == 0 )
    {
        //_setup.name = key.horiz;
        _setup.type = INIT_WKT;
        _setup.horiz = key.horiz;
    }
    else
    {
        // Try to set it from the user input.  This will handle things like CRS:84
        // createFromUserInput will actually handle all valid inputs from GDAL,
        // so we might be able to simplify this function and just call createFromUserInput.
        //_setup.name = key.horiz;
        _setup.type = INIT_USER;
        _setup.horiz = key.horiz;
    }

    // next, resolve the vertical SRS:
    if ( !key.vert.empty() && !ciEquals(key.vert, "geodetic") )
    {
        _vdatum = VerticalDatum::get( key.vert );
        if ( !_vdatum.valid() )
        {
            OE_WARN << LC << "Failed to locate vertical datum \"" << key.vert << "\"" << std::endl;
        }
    }

    _key = key;

    // create a handle for this thread and establish validity.
    init();
}

SpatialReference::ThreadLocal&
SpatialReference::getLocal() const
{
    ThreadLocal& local = _local.get();

    if (local._handle == nullptr)
    {
        local._threadId = std::this_thread::get_id();

        if (_setup.srcHandle != nullptr)
        {
            local._handle = OSRClone(static_cast<OGRSpatialReferenceH>(_setup.srcHandle));
            if (!local._handle)
            {
                OE_WARN << LC << "Failed to clone an existing handle" << std::endl;
                _valid = false;
            }
        }

        else
        {
            local._handle = OSRNewSpatialReference(nullptr);
            OGRErr error = OGRERR_INVALID_HANDLE;

            if (_setup.type == INIT_PROJ)
            {
                error = OSRImportFromProj4(static_cast<OGRSpatialReferenceH>(local._handle), _setup.horiz.c_str());
            }
            else if (_setup.type == INIT_WKT)
            {
                char buf[8192];
                char* buf_ptr = &buf[0];
                if (_setup.horiz.length() < 8192)
                {
                    strcpy(buf, _setup.horiz.c_str());

                    error = OSRImportFromWkt(static_cast<OGRSpatialReferenceH>(local._handle), &buf_ptr);

                    if (error == OGRERR_NONE)
                    {
                        //TODO
                        //fixWKT();
                    }
                }
                else
                {
                    OE_WARN << LC << "BUFFER OVERFLOW - INTERNAL ERROR\n";
                    _valid = false;
                }
            }
            else
            {
                error = OSRSetFromUserInput(static_cast<OGRSpatialReferenceH>(local._handle), _setup.horiz.c_str());
            }

            if (error != OGRERR_NONE)
            {
                OE_WARN << LC << "Failed to create SRS from \"" << _setup.horiz << "\"" << std::endl;
                OSRDestroySpatialReference(static_cast<OGRSpatialReferenceH>(local._handle));
                local._handle = nullptr;
                _valid = false;
            }
        }
    }

    if (local._threadId != std::this_thread::get_id())
    {
        OE_WARN << LC << "Thread Safety Violation at line " OE_STRINGIFY(__LINE__) << std::endl;
    }

    return local;
}

bool
SpatialReference::getBounds(Bounds& output) const
{
    output = _bounds;
    return _bounds.valid();
}

void*
SpatialReference::getHandle() const
{
    return getLocal()._handle;
}

SpatialReference*
SpatialReference::createFromHandle(void* ogrHandle)
{
    OE_SOFT_ASSERT_AND_RETURN(ogrHandle!=nullptr, nullptr);

    return new SpatialReference(ogrHandle);
}

/****************************************************************************/

SpatialReference::~SpatialReference()
{
    //nop
}

bool
SpatialReference::isGeographic() const 
{
    return _domain == GEOGRAPHIC;
}

bool
SpatialReference::isGeodetic() const 
{
    return isGeographic() && !_vdatum.valid();
}

bool
SpatialReference::isProjected() const
{
    return _domain == PROJECTED;
}

bool
SpatialReference::isGeocentric() const 
{
    return _domain == GEOCENTRIC;
}

const std::string&
SpatialReference::getName() const
{
    return _name;
}

const Ellipsoid&
SpatialReference::getEllipsoid() const
{
    return _ellipsoid;
}

const std::string&
SpatialReference::getDatumName() const
{
    return _datum;
}

const UnitsType&
SpatialReference::getUnits() const
{
    return _units;
}

double
SpatialReference::getReportedLinearUnits() const
{
    return _reportedLinearUnits;
}

const std::string&
SpatialReference::getWKT() const 
{
    return _wkt;
}

const VerticalDatum*
SpatialReference::getVerticalDatum() const
{
    return _vdatum.get();
}

const SpatialReference::Key&
SpatialReference::getKey() const
{
    return _key;
}

const std::string&
SpatialReference::getHorizInitString() const 
{ 
    return _key.horiz;
}

const std::string&
SpatialReference::getVertInitString() const 
{ 
    return _key.vert;
}

bool
SpatialReference::isEquivalentTo( const SpatialReference* rhs ) const
{
    return _isEquivalentTo( rhs, true );
}

bool
SpatialReference::isHorizEquivalentTo( const SpatialReference* rhs ) const
{
    return _isEquivalentTo( rhs, false );
}

bool
SpatialReference::isVertEquivalentTo( const SpatialReference* rhs ) const
{
    // vertical equivalence means the same vertical datum and the same
    // reference ellipsoid.
    return
        _vdatum.get() == rhs->_vdatum.get() &&
        _ellipsoidId  == rhs->_ellipsoidId;
}

bool
SpatialReference::_isEquivalentTo( const SpatialReference* rhs, bool considerVDatum ) const
{
    if (this == rhs)
        return true;

    if (!valid())
        return false;

    if (rhs == nullptr || !rhs->valid())
        return false;

    if (isGeographic()  != rhs->isGeographic()  ||
        isMercator()    != rhs->isMercator()    ||
        isGeocentric()  != rhs->isGeocentric()  ||
        isSphericalMercator() != rhs->isSphericalMercator() ||
        isNorthPolar()  != rhs->isNorthPolar()  ||
        isSouthPolar()  != rhs->isSouthPolar()  ||
        isUserDefined() != rhs->isUserDefined() ||
        isCube()        != rhs->isCube()        ||
        isLTP()         != rhs->isLTP() )
    {
        return false;
    }

    if (isGeocentric() && rhs->isGeocentric())
        return true;

    if ( considerVDatum && (_vdatum.get() != rhs->_vdatum.get()) )
        return false;

    if (_key.horizLower == rhs->_key.horizLower &&
        (!considerVDatum || (_key.vertLower == rhs->_key.vertLower) ) )
    {
        return true;
    }

    if ( _proj4 == rhs->_proj4 )
        return true;

    if ( _wkt == rhs->_wkt )
        return true;

    if (this->isGeographic() && rhs->isGeographic())
    {
        return
            osg::equivalent( getEllipsoid().getRadiusEquator(), rhs->getEllipsoid().getRadiusEquator() ) &&
            osg::equivalent( getEllipsoid().getRadiusPolar(), rhs->getEllipsoid().getRadiusPolar() );
    }

    // last resort, since it requires the lock
    void* myHandle = getHandle();
    void* rhsHandle = rhs->getHandle();

    return 
        myHandle &&
        rhsHandle &&
        OSRIsSame(OGRSpatialReferenceH(myHandle), OGRSpatialReferenceH(rhsHandle)) == TRUE;
}

const SpatialReference*
SpatialReference::getGeographicSRS() const
{
    if ( isGeographic() )
        return this;

    if ( _is_spherical_mercator )
        return get("wgs84", _key.vertLower);

    if ( !_geo_srs.valid() )
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if ( !_geo_srs.valid() ) // double-check pattern
        {
            // temporary SRS to build the WKT
            void* temp_handle = OSRNewSpatialReference(NULL);
            int err = OSRCopyGeogCSFrom(static_cast<OGRSpatialReferenceH>(temp_handle), static_cast<OGRSpatialReferenceH>(getHandle()));
            if (err == OGRERR_NONE)
            {
                char* wktbuf;
                if (OSRExportToWkt(static_cast<OGRSpatialReferenceH>(temp_handle), &wktbuf) == OGRERR_NONE)
                {
                    Key key(std::string(wktbuf), _key.vertLower);
                    _geo_srs = new SpatialReference(key);
                    CPLFree(wktbuf);
                }
            }
            OSRDestroySpatialReference(static_cast<OGRSpatialReferenceH>(temp_handle));
        }
    }

    return _geo_srs.get();
}

const SpatialReference*
SpatialReference::getGeodeticSRS() const
{
    if ( isGeodetic() )
        return this;

    if ( !_geodetic_srs.valid() )
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if ( !_geodetic_srs.valid() ) // double check pattern
        {
            // temporary SRS to build the WKT
            void* temp_handle = OSRNewSpatialReference(NULL);
            int err = OSRCopyGeogCSFrom(static_cast<OGRSpatialReferenceH>(temp_handle), static_cast<OGRSpatialReferenceH>(getHandle()));
            if (err == OGRERR_NONE)
            {
                char* wktbuf;
                if (OSRExportToWkt(static_cast<OGRSpatialReferenceH>(temp_handle), &wktbuf) == OGRERR_NONE)
                {
                    Key key(std::string(wktbuf), "");
                    _geodetic_srs = new SpatialReference(key);
                    CPLFree(wktbuf);
                }
            }
            OSRDestroySpatialReference(static_cast<OGRSpatialReferenceH>(temp_handle));
        }
    }

    return _geodetic_srs.get();
}

const SpatialReference*
SpatialReference::getGeocentricSRS() const
{
    if ( isGeocentric() )
        return this;

    if ( !_geocentric_srs.valid() )
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if ( !_geocentric_srs.valid() ) // double-check pattern
        {
            // temporary SRS to build the WKT
            void* temp_handle = OSRNewSpatialReference(NULL);
            int err = OSRCopyGeogCSFrom(static_cast<OGRSpatialReferenceH>(temp_handle), static_cast<OGRSpatialReferenceH>(getHandle()));
            if (err == OGRERR_NONE)
            {
                char* wktbuf;
                if (OSRExportToWkt(static_cast<OGRSpatialReferenceH>(temp_handle), &wktbuf) == OGRERR_NONE)
                {
                    Key key(std::string(wktbuf), ""); // to vdatum in ECEF
                    _geocentric_srs = new SpatialReference(key);
                    _geocentric_srs->_domain = GEOCENTRIC;
                    CPLFree(wktbuf);
                }
            }
            OSRDestroySpatialReference(static_cast<OGRSpatialReferenceH>(temp_handle));
        }
    }

    return _geocentric_srs.get();
}

const SpatialReference*
SpatialReference::createTangentPlaneSRS(const osg::Vec3d& origin) const
{
    if (!valid())
        return nullptr;

    osg::Vec3d lla;
    const SpatialReference* srs = getGeographicSRS();
    if ( srs && transform(origin, srs, lla) )
    {
        const Key& key = srs->getKey();
        SpatialReference* ltp = new TangentPlaneSpatialReference(key, lla);
        return ltp;
    }
    else
    {
        OE_WARN << LC << "Unable to create LTP SRS" << std::endl;
        return nullptr;
    }
}

const SpatialReference*
SpatialReference::createTransMercFromLongitude( const Angle& lon ) const
{
    if (!valid())
        return nullptr;

    // note. using tmerc with +lat_0 <> 0 is sloooooow.
    std::string datum = getDatumName();
    std::string horiz = Stringify()
        << "+proj=tmerc +lat_0=0"
        << " +lon_0=" << lon.as(Units::DEGREES)
        << " +datum=" << (datum.empty() ? "WGS84" : datum);

    return SpatialReference::create( horiz, getVertInitString() );
}

const SpatialReference*
SpatialReference::createUTMFromLonLat(const Angle& lon, const Angle& lat) const
{
    if (!valid())
        return nullptr;

    // note. UTM is up to 10% faster than TMERC for the same meridian.
    unsigned zone = 1 + (unsigned)floor((lon.as(Units::DEGREES)+180.0)/6.0);
    std::string datum = getDatumName();
    std::string horiz = Stringify()
        << "+proj=utm +zone=" << zone
        << (lat.as(Units::DEGREES) < 0 ? " +south" : "")
        << " +datum=" << (datum.empty() ? "WGS84" : datum);

    return SpatialReference::create(horiz, getVertInitString());
}

const SpatialReference*
SpatialReference::createEquirectangularSRS() const
{
    if (!valid())
        return nullptr;

    return SpatialReference::create(
        "+proj=eqc +units=m +no_defs", 
        getVertInitString());
}

bool
SpatialReference::isMercator() const
{
    return _is_mercator;
}

bool
SpatialReference::isSphericalMercator() const
{
    return _is_spherical_mercator;
}

bool 
SpatialReference::isNorthPolar() const
{
    return _is_north_polar;
}

bool 
SpatialReference::isSouthPolar() const
{
    return _is_south_polar;
}

bool
SpatialReference::isUserDefined() const
{
    return _is_user_defined;
}

bool
SpatialReference::isCube() const
{
    return _is_cube;
}

bool
SpatialReference::populateCoordinateSystemNode( osg::CoordinateSystemNode* csn ) const
{
    OE_SOFT_ASSERT_AND_RETURN(csn!=nullptr, false);

    if ( !_wkt.empty() )
    {
        csn->setFormat( "WKT" );
        csn->setCoordinateSystem( _wkt );
    }
    else if ( !_proj4.empty() )
    {
        csn->setFormat( "PROJ4" );
        csn->setCoordinateSystem( _proj4 );
    }

    csn->setEllipsoidModel(
        new osg::EllipsoidModel(
            _ellipsoid.getSemiMajorAxis(),
            _ellipsoid.getSemiMinorAxis()));
    
    return true;
}

bool
SpatialReference::createLocalToWorld(const osg::Vec3d& xyz, osg::Matrixd& out_local2world ) const
{
    if (!valid())
        return false;

    if ( isProjected() && !isCube() )
    {
        osg::Vec3d world;
        if ( !transformToWorld( xyz, world ) )
            return false;
        out_local2world = osg::Matrix::translate(world);
    }
    else if ( isGeocentric() )
    {
        out_local2world = _ellipsoid.geocentricToLocalToWorld(xyz);
    }
    else
    {
        // convert to ECEF:
        osg::Vec3d ecef;
        if ( !transform(xyz, getGeocentricSRS(), ecef) )
            return false;

        // and create the matrix.
        out_local2world = _ellipsoid.geocentricToLocalToWorld(ecef);
    }
    return true;
}

bool
SpatialReference::createWorldToLocal(const osg::Vec3d& xyz, osg::Matrixd& out_world2local ) const
{
    osg::Matrixd local2world;
    if ( !createLocalToWorld(xyz, local2world) )
        return false;
    out_world2local.invert(local2world);
    return true;
}


bool
SpatialReference::transform(const osg::Vec3d&       input,
                            const SpatialReference* outputSRS,
                            osg::Vec3d&             output) const
{
    OE_SOFT_ASSERT_AND_RETURN(outputSRS!=nullptr, false);

    if (!valid())
        return false;

    output = input;

    // Cube and LTP transforms have custom vector-based pre/post transforms.
    if (isCube() || isLTP() || outputSRS->isCube() || outputSRS->isLTP())
    {
        std::vector<osg::Vec3d> v(1, input);
        if (transform(v, outputSRS))
        {
            output = v[0];
            return true;
        }
        return false;
    }

    // trivial equivalency:
    if (isEquivalentTo(outputSRS))
        return true;

    if (isGeocentric() && !outputSRS->isGeocentric())
    {
        const SpatialReference* outputGeoSRS = outputSRS->getGeodeticSRS();
        output = outputGeoSRS->getEllipsoid().geocentricToGeodetic(output);
        return outputGeoSRS->transform(output, outputSRS, output);
    }

    else if (!isGeocentric() && outputSRS->isGeocentric())
    {
        const SpatialReference* outputGeoSRS = outputSRS->getGeodeticSRS();
        bool success = transform(output, outputGeoSRS, output);
        if (success)
            output = outputGeoSRS->getEllipsoid().geodeticToGeocentric(output);
        return success;
    }

    // if the point is starting as geographic, do the Z first to avoid an unnecessary
    // transformation in the case of differing vdatums.
    bool z_done = false;
    if (isGeographic())
    {
        z_done = transformPointZ(output, this, outputSRS, true);
    }

    if (transformSphericalMercatorXY(output, this, outputSRS))
    {
        if (!z_done)
        {
            z_done = transformPointZ(output, this, outputSRS, outputSRS->isGeographic());
        }
        return true;
    }

    ThreadLocal& local = getLocal();

    double x = output.x();
    double y = output.y();

    bool success = transformXYPointArrays(local, &x, &y, 1, outputSRS);

    if (success)
    {
        if (isProjected() && outputSRS->isGeographic())
        {
            // special case: when going from projected to geographic, clamp the
            // points to the maximum geographic extent. Sometimes the conversion from
            // a global/projected SRS (like mercator) will result in *slightly* invalid
            // geographic points (like long=180.000003), so this addresses that issue.
            output.x() = osg::clampBetween(x, -180.0, 180.0);
            output.y() = osg::clampBetween(y, -90.0, 90.0);
        }
        else
        {
            output.x() = x;
            output.y() = y;
        }
    }

    if (success && !z_done)
    {
        z_done = transformPointZ(output, this, outputSRS, outputSRS->isGeographic());
    }

    return success;
}


bool
SpatialReference::transform(std::vector<osg::Vec3d>& points,
                            const SpatialReference*  outputSRS) const
{
    OE_SOFT_ASSERT_AND_RETURN(outputSRS!=nullptr, false);

    if (!valid())
        return false;

    // trivial equivalency:
    if ( isEquivalentTo(outputSRS) )
        return true;
    
    bool success = false;

    // do the pre-transformation pass:
    const SpatialReference* inputSRS = preTransform( points );
    if ( !inputSRS )
        return false;
        
    if ( inputSRS->isGeocentric() && !outputSRS->isGeocentric() )
    {
        const SpatialReference* outputGeoSRS = outputSRS->getGeodeticSRS();
        geocentricToGeodetic(points, outputGeoSRS->getEllipsoid());
        return outputGeoSRS->transform(points, outputSRS);
    }

    else if ( !inputSRS->isGeocentric() && outputSRS->isGeocentric() )
    {
        const SpatialReference* outputGeoSRS = outputSRS->getGeodeticSRS();
        success = inputSRS->transform(points, outputGeoSRS);
        geodeticToGeocentric(points, outputGeoSRS->getEllipsoid());
        return success;
    }

    // if the points are starting as geographic, do the Z's first to avoid an unneccesary
    // transformation in the case of differing vdatums.
    bool z_done = false;
    if ( inputSRS->isGeographic() )
    {
        z_done = inputSRS->transformZ( points, outputSRS, true );
    }

    ThreadLocal& local = getLocal();

    // move the xy data into straight arrays that OGR can use
    unsigned count = points.size();

    if (count*2 > local._workspaceSize)
    {
        if (local._workspace)
            delete [] local._workspace;
        local._workspace = new double[count*2];
        local._workspaceSize = count*2;
    }

    double* x = local._workspace;
    double* y = local._workspace + count;

    for( unsigned i=0; i<count; i++ )
    {
        x[i] = points[i].x();
        y[i] = points[i].y();
    }

    success = inputSRS->transformXYPointArrays( local, x, y, count, outputSRS );

    if ( success )
    {
        if ( inputSRS->isProjected() && outputSRS->isGeographic() )
        {
            // special case: when going from projected to geographic, clamp the 
            // points to the maximum geographic extent. Sometimes the conversion from
            // a global/projected SRS (like mercator) will result in *slightly* invalid
            // geographic points (like long=180.000003), so this addresses that issue.
            for( unsigned i=0; i<count; i++ )
            {
                points[i].x() = osg::clampBetween( x[i], -180.0, 180.0 );
                points[i].y() = osg::clampBetween( y[i],  -90.0,  90.0 );
            }
        }
        else
        {
            for( unsigned i=0; i<count; i++ )
            {
                points[i].x() = x[i];
                points[i].y() = y[i];
            }
        }
    }

    if (success)
    {
        // calculate the Zs if we haven't already done so
        if ( !z_done )
        {
            z_done = inputSRS->transformZ( points, outputSRS, outputSRS->isGeographic() );
        }   

        // run the user post-transform code
        outputSRS->postTransform( points );
    }

    return success;
}


bool 
SpatialReference::transform2D(double x, double y,
                              const SpatialReference* outputSRS,
                              double& out_x, double& out_y ) const
{
    OE_SOFT_ASSERT_AND_RETURN(outputSRS!=nullptr, false);

    if (!valid())
        return false;

    osg::Vec3d temp(x,y,0);
    bool ok = transform(temp, outputSRS, temp);
    if ( ok ) {
        out_x = temp.x();
        out_y = temp.y();
    }
    return ok;
}


bool
SpatialReference::transformXYPointArrays(
    ThreadLocal& local,
    double*  x,
    double*  y,
    unsigned count,
    const SpatialReference* out_srs) const
{  
    OE_SOFT_ASSERT_AND_RETURN(out_srs!=nullptr, false);

    if (!valid())
        return false;

    // Transform the X and Y values inside an exclusive GDAL/OGR lock
    optional<TransformInfo>& xform = local._xformCache[out_srs->getWKT()];
    if (!xform.isSet())
    {
        xform.mutable_value()._handle = OCTNewCoordinateTransformation(static_cast<OGRSpatialReferenceH>(local._handle), static_cast<OGRSpatialReferenceH>(out_srs->getHandle()));

        if ( xform.mutable_value()._handle == nullptr )
        {
            OE_WARN << LC
                << "SRS xform not possible:" << std::endl
                << "    From => " << getName() << std::endl
                << "    To   => " << out_srs->getName() << std::endl;

            OE_WARN << LC << "INPUT: " << getWKT() << std::endl
                << "OUTPUT: " << out_srs->getWKT() << std::endl;

            const char* errmsg = CPLGetLastErrorMsg();
            OE_WARN << LC << "ERROR: " << (errmsg? errmsg : "do not know") << std::endl;

            xform.mutable_value()._handle = nullptr;
            xform.mutable_value()._failed = true;

            return false;
        }
    }

    if (xform->_failed)
    {
        return false;
    }

    return OCTTransform(static_cast<OGRCoordinateTransformationH>(xform->_handle), count, x, y, 0L) > 0;
}


bool
SpatialReference::transformZ(std::vector<osg::Vec3d>& points,
                             const SpatialReference*  outputSRS,
                             bool                     pointsAreLatLong) const
{
    OE_SOFT_ASSERT_AND_RETURN(outputSRS!=nullptr, false);

    if (!valid())
        return false;

    const VerticalDatum* outVDatum = outputSRS->getVerticalDatum();

    // same vdatum, no xformation necessary.
    if ( _vdatum.get() == outVDatum )
        return true;

    UnitsType inUnits = _vdatum.valid() ? _vdatum->getUnits() : Units::METERS;
    UnitsType outUnits = outVDatum ? outVDatum->getUnits() : inUnits;

    if ( isGeographic() || pointsAreLatLong )
    {
        for( unsigned i=0; i<points.size(); ++i )
        {
            if ( _vdatum.valid() )
            {
                // to HAE:
                points[i].z() = _vdatum->msl2hae( points[i].y(), points[i].x(), points[i].z() );
            }

            // do the units conversion:
            points[i].z() = inUnits.convertTo(outUnits, points[i].z());

            if ( outVDatum )
            {
                // to MSL:
                points[i].z() = outVDatum->hae2msl( points[i].y(), points[i].x(), points[i].z() );
            }
        }
    }

    else // need to xform input points
    {
        // copy the points and convert them to geographic coordinates (lat/long with the same Z):
        std::vector<osg::Vec3d> geopoints(points);
        transform( geopoints, getGeographicSRS() );

        for( unsigned i=0; i<geopoints.size(); ++i )
        {
            if ( _vdatum.valid() )
            {
                // to HAE:
                points[i].z() = _vdatum->msl2hae( geopoints[i].y(), geopoints[i].x(), points[i].z() );
            }

            // do the units conversion:
            points[i].z() = inUnits.convertTo(outUnits, points[i].z());

            if ( outVDatum )
            {
                // to MSL:
                points[i].z() = outVDatum->hae2msl( geopoints[i].y(), geopoints[i].x(), points[i].z() );
            }
        }
    }

    return true;
}

bool 
SpatialReference::transformToWorld(const osg::Vec3d& input,
                                   osg::Vec3d&       output ) const
{
    if (!valid())
        return false;

    if ( isGeographic() || isCube() )
    {
        return transform(input, getGeocentricSRS(), output);
    }
    else // isProjected
    {
        output = input;
        if ( _vdatum.valid() )
        {
            osg::Vec3d geo(input);
            if ( !transform(input, getGeographicSRS(), geo) )
                return false;

            output.z() = _vdatum->msl2hae( geo.y(), geo.x(), input.z() );
        }
        return true;
    }
}

bool 
SpatialReference::transformFromWorld(const osg::Vec3d& world,
                                     osg::Vec3d&       output,
                                     double*           out_haeZ ) const
{
    if ( isGeographic() || isCube() )
    {
        bool ok = getGeocentricSRS()->transform(world, this, output);
        if ( ok && out_haeZ )
        {
            if ( _vdatum.valid() )
                *out_haeZ = _vdatum->msl2hae(output.y(), output.x(), output.z());
            else
                *out_haeZ = output.z();
        }
        return ok;
    }
    else // isProjected
    {
        output = world;

        if (out_haeZ)
            *out_haeZ = world.z();

        if ( _vdatum.valid() )
        {
            // get the geographic coords by converting x/y/hae -> lat/long/msl:
            osg::Vec3d lla;
            if (!transform(world, getGeographicSRS(), lla) )
                return false;

            output.z() = lla.z();
        }

        return true;
    }
}

double
SpatialReference::transformUnits(double                  input,
                                 const SpatialReference* outSRS,
                                 double                  latitude) const
{
    OE_SOFT_ASSERT_AND_RETURN(outSRS!=nullptr, input);

    if ( this->isProjected() && outSRS->isGeographic() )
    {
        return Units::DEGREES.convertTo(
            outSRS->getUnits(),
            outSRS->getEllipsoid().metersToLongitudinalDegrees(
                getUnits().convertTo(Units::METERS, input),
                latitude));
    }
    else if ( this->isGeocentric() && outSRS->isGeographic() )
    {
        return Units::DEGREES.convertTo(
            outSRS->getUnits(),
            outSRS->getEllipsoid().metersToLongitudinalDegrees(input, latitude));
    }
    else if ( this->isGeographic() && outSRS->isProjected() )
    {
        return Units::METERS.convertTo(
            outSRS->getUnits(),
            outSRS->getEllipsoid().longitudinalDegreesToMeters(
                getUnits().convertTo(Units::DEGREES, input),
                latitude));
    }
    else if ( this->isGeographic() && outSRS->isGeocentric() )
    {
        return outSRS->getEllipsoid().longitudinalDegreesToMeters(
            getUnits().convertTo(Units::DEGREES, input),
            latitude);
    }
    else // both projected or both geographic.
    {
        return getUnits().convertTo( outSRS->getUnits(), input );
    }
}

double
SpatialReference::transformUnits(const Distance&         distance,
                                 const SpatialReference* outSRS,
                                 double                  latitude)
{
    OE_SOFT_ASSERT_AND_RETURN(outSRS!=nullptr, distance.getValue());

    if ( distance.getUnits().isLinear() && outSRS->isGeographic() )
    {
        return Units::DEGREES.convertTo(
            outSRS->getUnits(),
            outSRS->getEllipsoid().metersToLongitudinalDegrees(
                distance.as(Units::METERS),
                latitude));
    }
    else if ( distance.getUnits().isAngular() && outSRS->isProjected() )
    {
        return Units::METERS.convertTo(
            outSRS->getUnits(),
            outSRS->getEllipsoid().longitudinalDegreesToMeters(
                distance.as(Units::DEGREES),
                latitude));
    }
    else // both projected or both geographic.
    {
        return distance.as( outSRS->getUnits() );
    }
}

double
SpatialReference::transformDistance(const Distance& input, const UnitsType& outputUnits, double referenceLatitude) const
{
    auto inputUnits = input.getUnits();

    if (inputUnits.isAngle() && outputUnits.isLinear())
    {
        auto meters = getEllipsoid().longitudinalDegreesToMeters(input.as(Units::DEGREES), referenceLatitude);
        return Units::convert(Units::METERS, outputUnits, meters);
    }
    else if (inputUnits.isLinear() && outputUnits.isAngle())
    {
        auto degrees = getEllipsoid().metersToLongitudinalDegrees(input.as(Units::METERS), referenceLatitude);
        return Units::convert(Units::DEGREES, outputUnits, degrees);
    }
    else
    {
        return input.as(outputUnits);
    }
}

bool
SpatialReference::clampExtentToLegalBounds(
    const SpatialReference* target_srs,
    double& in_out_xmin,
    double& in_out_ymin,
    double& in_out_xmax,
    double& in_out_ymax) const
{
    OE_SOFT_ASSERT_AND_RETURN(target_srs, false);

    Bounds rhs_bounds;
    if (!target_srs->getBounds(rhs_bounds) || !rhs_bounds.valid())
        return false;


    Bounds rhs_bounds_geo = rhs_bounds;
    if (!target_srs->isGeographic())
    {
        auto* geo_srs = target_srs->getGeographicSRS();
        target_srs->transform(rhs_bounds._min, geo_srs, rhs_bounds_geo._min);
        target_srs->transform(rhs_bounds._max, geo_srs, rhs_bounds_geo._max);
    }

    Bounds lhs_bounds(in_out_xmin, in_out_ymin, 0.0, in_out_xmax, in_out_ymax, 0.0);
    Bounds lhs_bounds_geo = lhs_bounds;
    if (!this->isGeographic())
    {
        auto* geo_srs = this->getGeographicSRS();
        transform(lhs_bounds._min, geo_srs, lhs_bounds_geo._min);
        transform(lhs_bounds._max, geo_srs, lhs_bounds_geo._max);
    }
    
    lhs_bounds_geo = intersectionOf(lhs_bounds_geo, rhs_bounds_geo);

    if (!this->isGeographic())
    {
        auto* geo_srs = this->getGeographicSRS();
        geo_srs->transform(lhs_bounds_geo._min, this, lhs_bounds._min);
        geo_srs->transform(lhs_bounds_geo._max, this, lhs_bounds._max);
        in_out_xmin = lhs_bounds.xMin();
        in_out_ymin = lhs_bounds.yMin();
        in_out_xmax = lhs_bounds.xMax();
        in_out_ymax = lhs_bounds.yMax();
    }
    else
    {
        in_out_xmin = lhs_bounds_geo.xMin();
        in_out_ymin = lhs_bounds_geo.yMin();
        in_out_xmax = lhs_bounds_geo.xMax();
        in_out_ymax = lhs_bounds_geo.yMax();
    }

    return true;
}

bool
SpatialReference::transformExtentToMBR(
    const SpatialReference* to_srs,
    double& in_out_xmin,
    double& in_out_ymin,
    double& in_out_xmax,
    double& in_out_ymax) const
{
    OE_SOFT_ASSERT_AND_RETURN(to_srs != nullptr, false);

    if (!valid())
        return false;

    // Same SRS? no work to do.
    if (isHorizEquivalentTo(to_srs))
        return true;

    // Transform all points and take the maximum bounding rectangle the resulting points
    std::vector<osg::Vec3d> v;

    // Start by clamping to the out_srs' legal bounds, if possible.
    // TODO: rethink this to be more generic.
    if (isGeographic() && (to_srs->isMercator() || to_srs->isSphericalMercator()))
    {
        osg::ref_ptr<const Profile> merc = Profile::create(Profile::SPHERICAL_MERCATOR);
        in_out_ymin = clamp(in_out_ymin, merc->getLatLongExtent().yMin(), merc->getLatLongExtent().yMax());
        in_out_ymax = clamp(in_out_ymax, merc->getLatLongExtent().yMin(), merc->getLatLongExtent().yMax());
    }

    double height = in_out_ymax - in_out_ymin;
    double width = in_out_xmax - in_out_xmin;
    unsigned int numSamples = 5;

    v.reserve(5 + numSamples * 4);

    // first point is a centroid. This we will use to make sure none of the corner points
    // wraps around if the target SRS is geographic.
    v.push_back(osg::Vec3d(in_out_xmin + width * 0.5, in_out_ymin + height * 0.5, 0)); // centroid.

    // add the four corners
    v.push_back( osg::Vec3d(in_out_xmin, in_out_ymin, 0) ); // ll
    v.push_back( osg::Vec3d(in_out_xmin, in_out_ymax, 0) ); // ul
    v.push_back( osg::Vec3d(in_out_xmax, in_out_ymax, 0) ); // ur
    v.push_back( osg::Vec3d(in_out_xmax, in_out_ymin, 0) ); // lr

    //We also sample along the edges of the bounding box and include them in the 
    //MBR computation in case you are dealing with a projection that will cause the edges
    //of the bounding box to be expanded.  This was first noticed when dealing with converting
    //Hotline Oblique Mercator to WGS84
   
    //Sample the edges
    double dWidth  = width / (numSamples - 1);
    double dHeight = height / (numSamples - 1);
    
    //Left edge
    for (unsigned int i = 0; i < numSamples; i++)
    {
        v.push_back( osg::Vec3d(in_out_xmin, in_out_ymin + dHeight * (double)i, 0) );
    }

    //Right edge
    for (unsigned int i = 0; i < numSamples; i++)
    {
        v.push_back( osg::Vec3d(in_out_xmax, in_out_ymin + dHeight * (double)i, 0) );
    }

    //Top edge
    for (unsigned int i = 0; i < numSamples; i++)
    {
        v.push_back( osg::Vec3d(in_out_xmin + dWidth * (double)i, in_out_ymax, 0) );
    }

    //Bottom edge
    for (unsigned int i = 0; i < numSamples; i++)
    {
        v.push_back( osg::Vec3d(in_out_xmin + dWidth * (double)i, in_out_ymin, 0) );
    }
    
    if ( transform(v, to_srs) )
    {
        in_out_xmin = DBL_MAX;
        in_out_ymin = DBL_MAX;
        in_out_xmax = -DBL_MAX;
        in_out_ymax = -DBL_MAX;

        // For a geographic target, make sure the new extents contain the centroid
        // because they might have wrapped around or run into a precision failure.
        // v[0]=centroid, v[1]=LL, v[2]=UL, v[3]=UR, v[4]=LR
        if (to_srs->isGeographic())
        {
            if (v[1].x() > v[0].x() || v[2].x() > v[0].x()) in_out_xmin = -180.0;
            if (v[3].x() < v[0].x() || v[4].x() < v[0].x()) in_out_xmax = 180.0;
        }

        // enforce an MBR:
        for (unsigned int i = 0; i < v.size(); i++)
        {
            in_out_xmin = std::min( v[i].x(), in_out_xmin );
            in_out_ymin = std::min( v[i].y(), in_out_ymin );
            in_out_xmax = std::max( v[i].x(), in_out_xmax );
            in_out_ymax = std::max( v[i].y(), in_out_ymax );
        }

        return true;
    }

    return false;
}

bool 
SpatialReference::transformGrid(
    const SpatialReference* to_srs,
    double in_xmin, double in_ymin,
    double in_xmax, double in_ymax,
    double* x, double* y,
    unsigned int numx, unsigned int numy ) const
{
    OE_SOFT_ASSERT_AND_RETURN(to_srs!=nullptr, false);

    if (!valid())
        return false;

    std::vector<osg::Vec3d> points;

    const double dx = (in_xmax - in_xmin) / (numx - 1);
    const double dy = (in_ymax - in_ymin) / (numy - 1);

    unsigned int pixel = 0;
    double fc = 0.0;
    for (unsigned int c = 0; c < numx; ++c, ++fc)
    {
        const double dest_x = in_xmin + fc * dx;
        double fr = 0.0;
        for (unsigned int r = 0; r < numy; ++r, ++fr)
        {
            const double dest_y = in_ymin + fr * dy;

            points.push_back(osg::Vec3d(dest_x, dest_y, 0));
            pixel++;     
        }
    }

    if ( transform( points, to_srs ) )
    {
        for( unsigned i=0; i<points.size(); ++i )
        {
            x[i] = points[i].x();
            y[i] = points[i].y();
        }
        return true;
    }
    return false;
}

void
SpatialReference::init()
{
    OGRSpatialReferenceH handle = static_cast<OGRSpatialReferenceH>(getHandle());
    
    if (!handle)
    {
        _valid = false;
        return;
    }

    if (_is_ltp)
    {
        _is_user_defined = true;
        _domain = PROJECTED;
    }
    else if (_is_cube)
    {
        _domain = PROJECTED;
    }
    else if (!isGeocentric())
    {
        _domain = OSRIsGeographic(handle) != 0 ?
            GEOGRAPHIC : PROJECTED;
    }

    // Give the SRS a name if it doesn't have one:
    if ( !_setup.name.empty())
    {
        _name = _setup.name;
    }

    // extract the ellipsoid parameters:
    int err;

    _ellipsoid.setSemiMajorAxis(OSRGetSemiMajor(handle, &err));
    _ellipsoid.setSemiMinorAxis(OSRGetSemiMinor(handle, &err));

    // unique ID for comparing ellipsoids quickly:
    _ellipsoidId = hashString( Stringify() 
        << std::fixed << std::setprecision(10) 
        << _ellipsoid.getSemiMajorAxis() << ";" << _ellipsoid.getSemiMinorAxis() );

    // try to get an ellipsoid name:
    _ellipsoid.setName( getOGRAttrValue(handle, "SPHEROID") );

    // extract the projection:
    if ( _name.empty() || _name == "unnamed" || _name == "unknown" )
    {
        if (isGeographic())
        {
            _name = getOGRAttrValue(handle, "GEOGCS", 0);
            if (_name.empty()) _name = getOGRAttrValue(handle, "GEOGCRS");
        }
        else
        {
            _name = getOGRAttrValue(handle, "PROJCS");
            if (_name.empty()) _name = getOGRAttrValue(handle, "PROJCRS");
        }
    }
    std::string projection = getOGRAttrValue( handle, "PROJECTION" );
    std::string projection_lc = Strings::toLower(projection);

    // check for the Mercator projection:
    _is_mercator = !projection_lc.empty() && projection_lc.find("mercator")==0;

    // check for spherical mercator (a special case)
    _is_spherical_mercator = _is_mercator && osg::equivalent(
        _ellipsoid.getSemiMajorAxis(),
        _ellipsoid.getSemiMinorAxis());

    // check for the Polar projection:
    if ( !projection_lc.empty() && projection_lc.find("polar_stereographic") != std::string::npos )
    {
        double lat = as<double>( getOGRAttrValue( handle, "latitude_of_origin", 0, true ), -90.0 );
        _is_north_polar = lat > 0.0;
        _is_south_polar = lat < 0.0;
    }
    else
    {
        _is_north_polar = false;
        _is_south_polar = false;
    }

    // Try to extract the horizontal datum
    _datum = getOGRAttrValue( handle, "DATUM", 0, true );

    // Fix bad return values..
    if (_datum == "wgs_1984" || _datum == "WGS_1984")
        _datum = "WGS84";
    else if (_datum == "nad_1983" || _datum == "NAD_1983")
        _datum = "NAD83";

    // Extract the base units:
    std::string units_name = getOGRAttrValue( handle, "UNIT", 0, true );
    double unitMultiplier = osgEarth::Util::as<double>( getOGRAttrValue( handle, "UNIT", 1, true ), 1.0 );
    if ( isGeographic() )
        _units = UnitsType(units_name.c_str(), units_name.c_str(), Units::Domain::ANGLE, unitMultiplier);
    else
        _units = UnitsType(units_name.c_str(), units_name.c_str(), Units::Domain::DISTANCE, unitMultiplier);

    _reportedLinearUnits = OSRGetLinearUnits(handle, nullptr);
    
    // Try to extract the PROJ4 initialization string:
    char* proj4buf;
    if ( OSRExportToProj4( handle, &proj4buf ) == OGRERR_NONE )
    {
        _proj4 = proj4buf;
        CPLFree( proj4buf );
    }

    // Try to extract the OGC well-known-text (WKT) string:
    char* wktbuf;
    if ( OSRExportToWkt( handle, &wktbuf ) == OGRERR_NONE )
    {
        _wkt = wktbuf;
        CPLFree( wktbuf );
    }

    if ( _name == "unnamed" || _name == "unknown" || _name.empty() )
    {
        StringTable proj4_tok;

        auto kvps = StringTokenizer()
            .whitespaceDelims()
            .standardQuotes()
            .tokenize(_proj4);

        StringTokenizer tokenize_kvp;
        tokenize_kvp.delim("=");
        for (auto& kvp : kvps) {
            auto tokens = tokenize_kvp(kvp);
            if (tokens.size() == 2)
                proj4_tok[tokens[0]] = tokens[1];
        }

        if (proj4_tok["+proj"] == "utm")
        {
            _name = Stringify() << "UTM " << proj4_tok["+zone"];
        }
        else
        {
            _name =
                isGeographic() && !_datum.empty() ? _datum :
                isGeographic() && !_ellipsoid.getName().empty() ? _ellipsoid.getName() :
                isGeographic() ? "Geographic" :
                isGeocentric() ? "Geocentric" :
                isCube() ? "Unified Cube" :
                isLTP() ? "Tangent Plane" :
                !projection.empty() ? projection :
                _is_spherical_mercator ? "Spherical Mercator" :
                _is_mercator ? "Mercator" :
                (!_proj4.empty() ? _proj4 : 
                    "Projected");
        }
    }

    // Build a 'normalized' initialization key.
    if ( !_proj4.empty() )
    {
        _key.horiz = _proj4;
        _key.horizLower = toLower(_key.horiz);
    }
    else if ( !_wkt.empty() )
    {
        _key.horiz = _wkt;
        _key.horizLower = toLower(_key.horiz);
    }
    if ( _vdatum.valid() )
    {
        _key.vert = _vdatum->getInitString();
        _key.vertLower = toLower(_key.vert);
    }

    // Guess the appropriate bounds for this SRS.
    int isNorth;
    // Initialize the bounds to be invalid.
    _bounds.init();

#if 0 // this always returns false as of GDAL 3.1.3, so omit until later
#if GDAL_VERSION_MAJOR >= 3
    double wlong, elong, slat, nlat;
    if (OSRGetAreaOfUse(handle, &wlong, &slat, &elong, &nlat, nullptr) &&
        wlong > -1000.0)
    {
        osg::ref_ptr<const SpatialReference> geo = getGeographicSRS();
        geo->transform2D(wlong, slat, this, _bounds.xMin(), _bounds.yMin());
        geo->transform2D(elong, nlat, this, _bounds.xMax(), _bounds.yMax());
        OE_INFO << LC << "Bounds: " << _bounds.toString() << std::endl;
    }
#endif
#endif

    if (!_bounds.valid())
    {
        if (isGeographic() || isGeocentric())
        {
            _bounds.set(-180.0, -90.0, 0.0, 180.0, 90.0, 0.0);
        }
        else if (isMercator() || isSphericalMercator())
        {
            _bounds.set(MERC_MINX, MERC_MINY, 0.0, MERC_MAXX, MERC_MAXY, 0.0);
        }
        else if (OSRGetUTMZone(handle, &isNorth))
        {
            if (isNorth)
                _bounds.set(166000, 0, 0.0, 834000, 9330000, 0.0);
            else
                _bounds.set(166000, 1116915, 0.0, 834000, 10000000, 0.0);
        }
        else if (projection_lc == "equirectangular") // plate carre
        {
            _bounds.set(MERC_MINX, MERC_MINY * 0.5, 0.0, MERC_MAXX, MERC_MAXY * 0.5, 0.0);
        }
    }
}

