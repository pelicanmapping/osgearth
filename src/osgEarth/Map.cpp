/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/Map>
#include <osgEarth/MapModelChange>
#include <osgEarth/Registry>
#include <osgEarth/Notify>

using namespace osgEarth;

#define LC "[Map] "

//...................................................................

#if 0
Map::LayerCB::LayerCB(Map* map) : _map(map) { }

void Map::LayerCB::onOpen(Layer* layer)
{
    osg::ref_ptr<Map> map;
    if (_map.lock(map))
        map->notifyOnLayerOpenOrClose(layer);
}

void Map::LayerCB::onClose(Layer* layer)
{
    osg::ref_ptr<Map> map;
    if (_map.lock(map))
        map->notifyOnLayerOpenOrClose(layer);
}
#endif

namespace
{
    struct UpdateElevationPoolCallback : public MapCallback
    {
        Map* _map;
        UpdateElevationPoolCallback(Map* map) : _map(map) {}
        void onMapModelChanged(const MapModelChange& change) override
        {
            if (change.getElevationLayer())
            {
                if (_map->getElevationPool())
                {
                    _map->getElevationPool()->setMap(_map);
                }
            }
        }
    };
}

//...................................................................

Config
Map::Options::getConfig() const
{
    Config conf = ConfigOptions::getConfig();

    conf.set( "name",         name() );
    conf.set( "profile",      profile() );
    conf.set( "cache",        cache() );
    conf.set( "cache_policy", cachePolicy() );

    conf.set( "elevation_interpolation", "nearest",     elevationInterpolation(), INTERP_NEAREST);
    conf.set( "elevation_interpolation", "average",     elevationInterpolation(), INTERP_AVERAGE);
    conf.set( "elevation_interpolation", "bilinear",    elevationInterpolation(), INTERP_BILINEAR);
    conf.set( "elevation_interpolation", "triangulate", elevationInterpolation(), INTERP_TRIANGULATE);

    conf.set( "profile_layer", profileLayer() );

    conf.set("read_options", osgOptionString());

    conf.set("disable_elevation_ranges", disableElevationRanges());

    return conf;
}

void
Map::Options::fromConfig(const Config& conf)
{   
    conf.get( "name",         name() );
    conf.get( "profile",      profile() );
    conf.get( "cache",        cache() );  
    conf.get( "cache_policy", cachePolicy() );

    // legacy support:
    if ( conf.value<bool>( "cache_only", false ) == true )
        cachePolicy().mutable_value().usage() = CachePolicy::USAGE_CACHE_ONLY;

    if ( conf.value<bool>( "cache_enabled", true ) == false )
        cachePolicy().mutable_value().usage() = CachePolicy::USAGE_NO_CACHE;

    conf.get( "elevation_interpolation", "nearest",     elevationInterpolation(), INTERP_NEAREST);
    conf.get( "elevation_interpolation", "average",     elevationInterpolation(), INTERP_AVERAGE);
    conf.get( "elevation_interpolation", "bilinear",    elevationInterpolation(), INTERP_BILINEAR);
    conf.get( "elevation_interpolation", "triangulate", elevationInterpolation(), INTERP_TRIANGULATE);

    conf.get( "profile_layer", profileLayer() );

    conf.get("read_options", osgOptionString());
    conf.get("osg_options", osgOptionString()); // back compat

    conf.get("disable_elevation_ranges", disableElevationRanges());
}

//...................................................................

Map::Map() :
osg::Object()
{
    init();
}

Map::Map(const osgDB::Options* readOptions) :
    osg::Object(),
    _readOptions(readOptions ? osg::clone(readOptions) : nullptr)
{
    init();
}

Map::Map(const Map::Options& options, const osgDB::Options* readOptions) :
    osg::Object(),
    _optionsConcrete(options),
    _readOptions(readOptions ? osg::clone(readOptions) : nullptr)
{
    init();
}

void
Map::init()
{
    // reset the revision:
    _dataModelRevision = 0;

    // set the object name from the options:
    if (options().name().isSet())
        osg::Object::setName(options().name().get());
    else
        osg::Object::setName("osgEarth.Map");

    // Generate a UID.
    _uid = osgEarth::createUID();

    if (!_readOptions.valid())
    {
        _readOptions = new osgDB::Options();
    }

    // Set up the map's profile
    if (options().profile().isSet())
        setProfile(Profile::create(options().profile().get()));

    if (getProfile() == nullptr)
        setProfile(Profile::create(Profile::GLOBAL_GEODETIC));

    // If the registry doesn't have a default cache policy, but the
    // map options has one, make the map policy the default.
    if (options().cachePolicy().isSet() &&
        !Registry::instance()->defaultCachePolicy().isSet())
    {
        Registry::instance()->setDefaultCachePolicy( options().cachePolicy().get() );
        OE_INFO << LC
            << "Setting default cache policy from map ("
            << options().cachePolicy()->usageString() << ")" << std::endl;
    }

    if (!options().osgOptionString()->empty())
    {
        std::string a = _readOptions->getOptionString();
        a = options().osgOptionString().get() + " " + a;
        _readOptions->setOptionString(a);
    }

    // put the CacheSettings object in there. We will propogate this throughout
    // the data model and the renderer. These will be stored in the readOptions
    // (and ONLY there)
    _cacheSettings = new CacheSettings();

    // Set up a cache if there's one in the options:
    if (options().cache().isSet())
        _cacheSettings->setCache(CacheFactory::create(options().cache().get()));

    // Otherwise use the registry default cache if there is one:
    if (_cacheSettings->getCache() == nullptr)
        _cacheSettings->setCache(Registry::instance()->getDefaultCache());

    // Integrate local cache policy (which can be overridden by the environment)
    _cacheSettings->integrateCachePolicy(options().cachePolicy());

    // store in the options so we can propagate it to layers, etc.
    _cacheSettings->store(_readOptions.get());
    if (_cacheSettings->getCache() && _cacheSettings->isCacheEnabled())
        OE_INFO << LC << _cacheSettings->toString() << std::endl;

    // remember the referrer for relative-path resolution:
    URIContext( options().referrer() ).store( _readOptions.get() );

    // create a callback that the Map will use to detect setEnabled calls
    //_layerCB = new LayerCB(this);

    // elevation sampling
    _elevationPool = new ElevationPool();
    _elevationPool->setMap( this );

    // tell the elevation pool to refresh when layers open or close
    addMapCallback(new UpdateElevationPoolCallback(this));

    _numTerrainPatchLayers = 0;
}

Map::~Map()
{
    //nop
}

ElevationPool*
Map::getElevationPool() const
{
    return _elevationPool.get();
}

void
Map::notifyOnLayerOpenOrClose(Layer* layer)
{
    // bump the revision safely:
    Revision newRevision;
    {
        Threading::ScopedWriteLock lock(_mapDataMutex);
        newRevision = ++_dataModelRevision;
    }

    if (layer->isOpen())
    {
        if (getProfile())
        {
            layer->addedToMap(this);
        }
    }
    else
    {
        layer->removedFromMap(this);
    }

    MapModelChange change(
        layer->isOpen() ? MapModelChange::OPEN_LAYER : MapModelChange::CLOSE_LAYER,
        newRevision,
        layer);

    for (auto cb : _mapCallbacks)
    {
        cb->onMapModelChanged(change);
    }
}

void
Map::setMapName( const std::string& name )
{
    _name = name;
}

Revision
Map::getDataModelRevision() const
{
    return _dataModelRevision;
}

void
Map::setProfile(const Profile* value)
{
    bool notifyLayers = !_profile.valid();

    if (value)
    {
        _profile = value;
        _readOptions->setPluginStringData("osgEarth.mapProfileSignature", value->getHorizSignature());

        // create a "proxy" profile to use when querying elevation layers with a vertical datum
        if (_profile.valid() && _profile->getSRS()->getVerticalDatum() != 0L )
        {
            ProfileOptions po = _profile->toProfileOptions();
            po.vsrsString().unset();
            _profileNoVDatum = Profile::create(po);
        }
        else
        {
            _profileNoVDatum = _profile;
        }

        // finally, fire an event if the profile has been set.
        OE_INFO << LC << "Map profile: " << _profile->toString() << std::endl;
    }

    // If we just set the profile, tell all our layers they are now added
    // to a valid map.
    if (_profile.valid() && notifyLayers)
    {
        for(auto& layer : _layers)
        {
            if (layer->isOpen())
            {
                layer->addedToMap(this);
            }
        }
    }

    // The pool also exists on maps with no elevation layers or a custom WorkingSet.
    if (_elevationPool.valid())
        _elevationPool->setMap(this);
}

const Profile*
Map::getProfile() const
{
    return _profile.get();
}

void
Map::setCachePolicy(const CachePolicy& value)
{
    options().cachePolicy() = value;

    CacheSettings* cacheSettings = CacheSettings::get(_readOptions.get());
    if (cacheSettings)
    {
        cacheSettings->integrateCachePolicy(value);
    }
}

const CachePolicy&
Map::getCachePolicy() const
{
    return options().cachePolicy().get();
}

void
Map::setElevationInterpolation(const RasterInterpolation& value)
{
    if (options().elevationInterpolation().get() == value)
        return;
    options().elevationInterpolation() = value;
    if (_elevationPool.valid())
        _elevationPool->setMap(this);
}

const RasterInterpolation&
Map::getElevationInterpolation() const
{
    return options().elevationInterpolation().get();
}

Cache*
Map::getCache() const
{
    CacheSettings* cacheSettings = CacheSettings::get(_readOptions.get());
    return cacheSettings ? cacheSettings->getCache() : 0L;
}

void
Map::setCache(Cache* cache)
{
    // note- probably unsafe to do this after initializing the terrain. so don't.
    CacheSettings* cacheSettings = CacheSettings::get(_readOptions.get());
    if (cacheSettings && cacheSettings->getCache() != cache)
        cacheSettings->setCache(cache);
}

namespace
{
    void addAttributions(const LayerVector& layers, StringSet& attributions)
    {
        for(auto& layer : layers)
        {
            if (layer->isOpen())
            {
                VisibleLayer* visibleLayer = dynamic_cast<VisibleLayer*>(layer.get());
                if (!visibleLayer || visibleLayer->getVisible())
                {
                    std::string attribution = layer->getAttribution();
                    if (!attribution.empty())
                    {
                        attributions.insert(attribution);
                    }
                }
            }
        }
    }
}

void
Map::getAttributions(StringSet& attributions) const
{
    LayerVector layers;
    getLayers(layers);
    addAttributions(layers, attributions);
}

MapCallback*
Map::addMapCallback(MapCallback* cb) const
{
    if ( cb )
        _mapCallbacks.push_back( cb );
    return cb;
}

void
Map::removeMapCallback(MapCallback* cb) const
{
    MapCallbackList::iterator i = std::find( _mapCallbacks.begin(), _mapCallbacks.end(), cb);
    if (i != _mapCallbacks.end())
    {
        _mapCallbacks.erase( i );
    }
}

void
Map::beginUpdate()
{
    MapModelChange msg( MapModelChange::BEGIN_BATCH_UPDATE, _dataModelRevision );
    for(auto& cb : _mapCallbacks)
    {
        cb->onMapModelChanged( msg );
    }
}

void
Map::endUpdate()
{
    MapModelChange msg( MapModelChange::END_BATCH_UPDATE, _dataModelRevision );
    for (auto& cb : _mapCallbacks)
    {
        cb->onMapModelChanged(msg);
    }
}

void
Map::addLayer(Layer* layer)
{
    if (layer == NULL)
        return;

    // ensure it's not already in the map
    if (getIndexOfLayer(layer) != getNumLayers())
        return;

    // Store in a ref_ptr for scope to ensure callbacks don't accidentally delete while adding
    osg::ref_ptr<Layer> layerRef( layer );

    layer->setReadOptions(getReadOptions());

    if (layer->getOpenAutomatically())
    {
        layer->open();
    }

    // do we need this? Won't the callback to this?
    if (layer->isOpen() && getProfile() != nullptr)
    {
        layer->addedToMap(this);

        // if the layer has sublayers (which is must open manually) add them now.
        for (auto& sublayer : layer->_sublayers)
        {
            if (sublayer->isOpen())
            {
                sublayer->addedToMap(this);
            }
        }
    }

    // Set up callbacks. Do this *after* calling addedToMap (since the callback invokes addedToMap)
    installLayerCallbacks(layer);

    // Add the layer to our stack.
    int newRevision;
    unsigned index = -1;
    {
        Threading::ScopedWriteLock lock( _mapDataMutex );

        _layers.push_back( layer );
        index = _layers.size() - 1;
        newRevision = ++_dataModelRevision;

        if (layer->options().terrainPatch() == true)
            ++_numTerrainPatchLayers;

        // if the layer has sublayers (which is must open manually) add them now.
        for (auto& sublayer : layer->_sublayers)
        {
            if (sublayer->isOpen())
            {
                _layers.push_back(sublayer);
            }
        }
    }

    // a separate block b/c we don't need the mutex
    for(auto& cb : _mapCallbacks)
    {
        cb->onMapModelChanged(MapModelChange(MapModelChange::ADD_LAYER, newRevision, layer, index));
    }
}

void
Map::insertLayer(Layer* layer, unsigned index)
{
    if (layer == nullptr)
        return;

    // ensure it's not already in the map
    if (getIndexOfLayer(layer) != getNumLayers())
        return;

    // Store in a ref_ptr for scope to ensure callbacks don't accidentally delete while adding
    osg::ref_ptr<Layer> layerRef( layer );

    layer->setReadOptions(getReadOptions());

    if (layer->getOpenAutomatically())
    {
        layer->open();
    }

    if (layer->isOpen() && getProfile() != nullptr)
    {
        layer->addedToMap(this);

        // if the layer has sublayers (which is must open manually)...
        for (auto& sublayer : layer->_sublayers)
        {
            if (sublayer->isOpen())
            {
                sublayer->addedToMap(this);
            }
        }
    }

    // Set up callbacks. Do this *after* calling addedToMap (since the callback invokes addedToMap)
    installLayerCallbacks(layer);

    // Add the layer to our stack.
    int newRevision;
    {
        Threading::ScopedWriteLock lock(_mapDataMutex);

        if (index >= _layers.size())
            _layers.push_back(layer);
        else
            _layers.insert(_layers.begin() + index, layer);

        for (auto& sublayer : layer->_sublayers)
        {
            if (sublayer->isOpen())
            {
                _layers.push_back(sublayer);
            }
        }

        newRevision = ++_dataModelRevision;

        if (layer->options().terrainPatch() == true)
            ++_numTerrainPatchLayers;
    }

    // a separate block b/c we don't need the mutex
    for (auto& cb : _mapCallbacks)
    {
        cb->onMapModelChanged(MapModelChange(MapModelChange::ADD_LAYER, newRevision, layer, index));
    }
}

void
Map::removeLayer(Layer* layer)
{
    if (layer == nullptr)
        return;

    // ensure it's in the map
    if (getIndexOfLayer(layer) == getNumLayers())
        return;

    //osgEarth::Registry::instance()->clearBlacklist();
    unsigned int index = -1;

    osg::ref_ptr<Layer> layerToRemove(layer);
    Revision newRevision;

    uninstallLayerCallbacks(layerToRemove.get());

    layer->removedFromMap(this);

    // Close the layer if we opened it.
    if (layer->getOpenAutomatically())
    {
        layer->close();
    }

    if ( layerToRemove.get() )
    {
        Threading::ScopedWriteLock lock( _mapDataMutex );
        index = 0;
        for( LayerVector::iterator i = _layers.begin(); i != _layers.end(); i++, index++ )
        {
            if ( i->get() == layerToRemove.get() )
            {
                _layers.erase( i );
                newRevision = ++_dataModelRevision;

                if (layer->options().terrainPatch() == true)
                    --_numTerrainPatchLayers;

                break;
            }
        }
    }

    // a separate block b/c we don't need the mutex
    if (newRevision >= 0)
    {
        for (auto& cb : _mapCallbacks)
        {
            cb->onMapModelChanged(MapModelChange(MapModelChange::REMOVE_LAYER, newRevision, layerToRemove.get(), index));
        }
    }
}

void
Map::moveLayer(Layer* layer, unsigned newIndex)
{
    unsigned int oldIndex = 0;
    unsigned int actualIndex = 0;
    Revision newRevision;

    if ( layer )
    {
        Threading::ScopedWriteLock lock( _mapDataMutex );

        // preserve the layer with a ref:
        osg::ref_ptr<Layer> layerToMove( layer );

        // find it:
        LayerVector::iterator i_oldIndex = _layers.end();
        for( LayerVector::iterator i = _layers.begin(); i != _layers.end(); i++, actualIndex++ )
        {
            if ( i->get() == layer )
            {
                i_oldIndex = i;
                oldIndex = actualIndex;
                break;
            }
        }

        if ( i_oldIndex == _layers.end() )
            return; // layer not found in list

        // erase the old one and insert the new one.
        _layers.erase( i_oldIndex );
        _layers.insert( _layers.begin() + newIndex, layerToMove.get() );

        newRevision = ++_dataModelRevision;
    }

    // a separate block b/c we don't need the mutex
    if ( layer )
    {
        for (auto& cb : _mapCallbacks)
        {
            cb->onMapModelChanged(MapModelChange(MapModelChange::MOVE_LAYER, newRevision, layer, oldIndex, newIndex));
        }
    }
}

void
Map::addLayers(const LayerVector& layers)
{
    // This differs from addLayer() in a loop because it will
    // (a) call addedToMap only after all the layers are added, and
    // (b) invoke all the MapModelChange callbacks with the same 
    // new revision number.

    for(auto& layer : layers)
    {
        if ( !layer )
            continue;

        layer->setReadOptions(getReadOptions());

        // open, but don't call addedToMap(layer) yet.
        if (layer->getOpenAutomatically())
        {
            layer->open();
        }
    }

    unsigned firstIndex;
    unsigned count = 0;
    int newRevision;

    // Add the layers to the map.
    {
        Threading::ScopedWriteLock lock( _mapDataMutex );

        firstIndex = _layers.size();
        newRevision = ++_dataModelRevision;

        for (auto& layer : layers)
        {
            if (layer.valid())
            {
                _layers.push_back(layer);

                // if the layer has sublayers (which is must open manually) add them now.
                for (auto& sublayer : layer->_sublayers)
                {
                    if (sublayer->isOpen())
                    {
                        _layers.push_back(sublayer);
                    }
                }
            }
        }
    }

    // call addedToMap on each new layer in turn:
    unsigned index = firstIndex;

    for (auto& layer : layers)
    {
        if ( !layer )
            continue;

        if (layer->isOpen() && getProfile() != NULL)
        {
            layer->addedToMap(this);

            for (auto& sublayer : layer->_sublayers)
            {
                sublayer->addedToMap(this);
            }
        }

        // Set up callbacks.
        installLayerCallbacks(layer);

        // a separate block b/c we don't need the mutex
        for (auto& cb : _mapCallbacks)
        {
            cb->onMapModelChanged(MapModelChange(MapModelChange::ADD_LAYER, newRevision, layer, index++));
        }
    }
}

void
Map::installLayerCallbacks(Layer* layer)
{
    // Callback to detect changes in "enabled"
    //layer->addCallback(_layerCB.get());

    osg::observer_ptr<Map> weak_ptr(this);

    _layerOpenCallbacks.emplace(layer, layer->onOpen([weak_ptr](Layer* layer)
        {
            osg::ref_ptr<Map> map;
            if (weak_ptr.lock(map))
                map->notifyOnLayerOpenOrClose(layer);
        }));

    _layerCloseCallbacks.emplace(layer, layer->onClose([weak_ptr](Layer* layer)
        {
            osg::ref_ptr<Map> map;
            if (weak_ptr.lock(map))
                map->notifyOnLayerOpenOrClose(layer);
        }));
}

void
Map::uninstallLayerCallbacks(Layer* layer)
{
    //layer->removeCallback(_layerCB.get());
    layer->onOpen.remove(_layerOpenCallbacks[layer]);
    layer->onClose.remove(_layerCloseCallbacks[layer]);
}

Revision
Map::getLayers(LayerVector& out_list) const
{
    out_list.reserve( _layers.size() );

    Threading::ScopedReadLock lock(_mapDataMutex);
    for(auto& layer : _layers)
    {
        out_list.push_back( layer.get() );
    }

    return _dataModelRevision;
}

unsigned
Map::getNumLayers() const
{
    Threading::ScopedReadLock lock( _mapDataMutex );
    return _layers.size();
}

Layer*
Map::getLayerByName(const std::string& name) const
{
    Threading::ScopedReadLock lock( _mapDataMutex );
    for(auto& layer : _layers)
    {
        if (layer->getName() == name)
            return layer.get();
    }
    return nullptr;
}

Layer*
Map::getLayerByUID(UID layerUID) const
{
    Threading::ScopedReadLock lock( _mapDataMutex );
    for(auto& layer : _layers)
    {
        if (layer->getUID() == layerUID)
            return layer.get();
    }
    return nullptr;
}

Layer*
Map::getLayerAt(unsigned index) const
{
    Threading::ScopedReadLock lock( _mapDataMutex );
    if ( index >= 0 && index < (int)_layers.size() )
        return _layers[index].get();
    else
        return 0L;
}

unsigned
Map::getIndexOfLayer(const Layer* layer) const
{
    Threading::ScopedReadLock lock( _mapDataMutex );
    unsigned index = 0;
    for (; index < _layers.size(); ++index)
    {
        if (_layers[index] == layer)
            break;
    }
    return index;
}


void
Map::clear()
{
    LayerVector layersRemoved;
    Revision newRevision;
    {
        Threading::ScopedWriteLock lock( _mapDataMutex );

        layersRemoved.swap( _layers );

        // calculate a new revision.
        newRevision = ++_dataModelRevision;
    }

    // a separate block b/c we don't need the mutex
    for(auto& cb : _mapCallbacks)
    {
        cb->onBeginUpdate();

        for(auto& layer : layersRemoved)
        {
            cb->onMapModelChanged(MapModelChange(MapModelChange::REMOVE_LAYER, newRevision, layer.get()));
        }

        cb->onEndUpdate();
    }
}

const SpatialReference*
Map::getSRS() const
{
    return _profile.valid() ? _profile->getSRS() : 0L;
}

const SpatialReference*
Map::getWorldSRS() const
{
    return getSRS() && getSRS()->isGeographic() ? getSRS()->getGeocentricSRS() : getSRS();
}

bool
Map::isFast(const TileKey& key, const LayerVector& layers) const
{
    if (getCache() == NULL)
        return false;
    
    for(auto& layer : layers)
    {
        if (!layer)
            continue;

        if (!layer->isOpen())
            continue;

        TileLayer* tilelayer = dynamic_cast<TileLayer*>(layer.get());
        if (tilelayer)
        {
            if (tilelayer->getCacheSettings()->cachePolicy()->isCacheDisabled())
              return false;

            //If no data is available on this tile, we'll be fast
            if (!tilelayer->mayHaveData(key))
                continue;

            if (!tilelayer->isCached(key))
                return false;
        }
    }
    return true;
}

int 
Map::getNumTerrainPatchLayers() const
{
    return _numTerrainPatchLayers;
}
