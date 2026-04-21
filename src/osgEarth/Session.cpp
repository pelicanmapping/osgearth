/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include <osgEarth/Session>
#include <osgEarth/Script>
#include <osgEarth/ScriptEngine>
#include <osgEarth/FeatureSource>

#include <osgEarth/ResourceCache>
#include <osgEarth/StyleSheet>

#include <osgEarth/Registry>

#define LC "[Session] "

using namespace osgEarth;
using namespace osgEarth::Util;

//---------------------------------------------------------------------------

Session::Session() :
osg::Object(),
_map(0L)
{
    init();
}

Session::Session(const Map* map) :
osg::Object(),
_map(map)
{
    init();
}

Session::Session(const Map* map, StyleSheet* styles) :
osg::Object(),
_map(map),
_styles(styles)
{
    init();
}

Session::Session(const Map* map, StyleSheet* styles, FeatureSource* source, const osgDB::Options* dbOptions) :
osg::Object(),
_map(map),
_styles(styles),
_featureSource(source),
_dbOptions(dbOptions)
{
    init();
}

Session::Session(const Session& rhs, const osg::CopyOp& op) :
osg::Object(rhs, op),
_map(rhs._map.get())
{
    //nop
}


void
Session::init()
{
    setStyles(_styles.get());

    // A new cache to optimize state changes. Since the cache lives in the Session, any
    // geometry created under this session takes advantage of it. That's reasonable since
    // tiles in a particular "layer" will tend to share state.
    _stateSetCache = new StateSetCache();

    _name = "Session (unnamed)";

    if (_map.valid())
    {
        _mapProfile = _map->getProfile();
    }

    if (!_mapProfile.valid())
    {
        _mapProfile = Profile::create(Profile::GLOBAL_GEODETIC);
    }
}

Session::~Session()
{
    //nop
}

const osgDB::Options*
Session::getDBOptions() const
{
    // local options if they were set:
    if (_dbOptions.valid())
        return _dbOptions.get();

    // otherwise get them from the map if possible:
    osg::ref_ptr<const Map> map;
    if (_map.lock(map))
        return map->getReadOptions();

    return 0L;
}

void
Session::setResourceCache(ResourceCache* cache)
{
    _resourceCache = cache;
}

ResourceCache*
Session::getResourceCache()
{
    return _resourceCache.get();
}

osg::ref_ptr<const Map>
Session::getMap() const
{
    osg::ref_ptr<const Map> map;
    _map.lock(map);
    return map;
}

const Profile*
Session::getMapProfile() const
{
    osg::ref_ptr<const Map> map = getMap();
    return map.valid() ? map->getProfile() : NULL;
}

const SpatialReference*
Session::getMapSRS() const
{
    osg::ref_ptr<const Map> map = getMap();
    return map.valid() && map->getProfile()? map->getProfile()->getSRS() : NULL;
}

bool
Session::isMapGeocentric() const
{
    osg::ref_ptr<const Map> map = getMap();
    return map.valid() && map->getProfile()? map->getProfile()->getSRS()->isGeographic() : true;
}

StateSetCache*
Session::getStateSetCache()
{
    return _stateSetCache.get();
}

void
Session::setStyles( StyleSheet* value )
{
    _styles = value ? value : new StyleSheet();
    initScriptEngine();
}

void
Session::initScriptEngine()
{
    _styleScriptEngine = 0L;

    // Create a script engine for the StyleSheet
    if (_styles)
    {
        if (_styles->getScript())
        {
            _styleScriptEngine = ScriptEngineFactory::createWithProfile(
                Script(
                    _styles->getScript()->code,
                    _styles->getScript()->language,
                    _styles->getScript()->name ),
                    _styles->getScript()->profile );
        }
        else
        {
            // If the stylesheet has no script set, create a default JS engine
            // This enables the use of "inline" scripting in Expression values
            _styleScriptEngine = ScriptEngineFactory::create("javascript");
        }
    }
}

ScriptEngine*
Session::getScriptEngine() const
{
    return _styleScriptEngine.get();
}

void
Session::setFeatureSource(FeatureSource* fs)
{
    _featureSource = fs;
}

FeatureSource*
Session::getFeatureSource() const 
{ 
    return _featureSource.get(); 
}
