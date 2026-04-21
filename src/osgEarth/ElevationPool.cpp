
/* osgEarth
 * Copyright 2008-2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/ElevationPool>
#include <osgEarth/Map>
#include <osgEarth/Metrics>
#include <osgEarth/rtree.h>
#include <osgEarth/HeightFieldUtils>
#include <osgEarth/Containers>
#include <osgEarth/Progress>
#include <osgEarth/Notify>

#include <thread>

using namespace osgEarth;

#define LC "[ElevationPool] "

ElevationPool::ElevationPool() :
    _tileSize(257),
    _L2(64u)
{
}

const SpatialReference*
ElevationPool::getMapSRS() const
{
    osg::ref_ptr<const Map> safe_map;
    if (_mapData.map.lock(safe_map))
        return safe_map->getSRS();
    else
        return nullptr;
}

namespace
{
    using MaxLevelIndex = RTree<unsigned, double, 2>;
}

ElevationPool::~ElevationPool()
{
    ScopedWriteLock lock(_mapDataMutex);

    for (auto& itr : _mapData.index)
        if (itr.second)
            delete static_cast<MaxLevelIndex*>(itr.second);
}

void
ElevationPool::setMap(const Map* map)
{
    _L2.clear();
    {
        ScopedWriteLock lock(_globalLUTMutex);
        _globalLUT.clear();
    }

    MapData newData;

    if (map)
    {
        newData.map = map;
        newData.mapRevision = map->getOpenLayers<ElevationLayer>(newData.layers);
        newData.mapProfile = map->getProfile();
        newData.mapProfileNoVDatum = map->getProfileNoVDatum();
        newData.interpolation = map->getElevationInterpolation();
        newData.hash = 0;
        for (auto& layer : newData.layers)
            newData.hash = hash_value_unsigned(newData.hash, layer->getUID(), layer->getRevision());

        double a_min[2], a_max[2];

        for (auto i : newData.layers)
        {
            const ElevationLayer* layer = i.get();
            DataExtentList dataExtents;
            layer->getDataExtents(dataExtents);

            MaxLevelIndex* layerIndex = new MaxLevelIndex();

            for (auto de = dataExtents.begin(); de != dataExtents.end(); ++de)
            {
                GeoExtent extentInMapSRS = map->getProfile()->clampAndTransformExtent(*de);

                // Convert the max level so it's relative to the map profile:
                unsigned maxLevel = std::min(de->maxLevel().get(), layer->getMaxDataLevel());
                maxLevel = map->getProfile()->getEquivalentLOD(layer->getProfile(), maxLevel);

                if (extentInMapSRS.crossesAntimeridian())
                {
                    GeoExtent a, b;
                    extentInMapSRS.splitAcrossAntimeridian(a, b);

                    for (auto& ex : { a, b })
                    {
                        a_min[0] = ex.xMin(), a_min[1] = ex.yMin();
                        a_max[0] = ex.xMax(), a_max[1] = ex.yMax();
                        layerIndex->Insert(a_min, a_max, maxLevel);
                    }
                }
                else
                {
                    a_min[0] = extentInMapSRS.xMin(), a_min[1] = extentInMapSRS.yMin();
                    a_max[0] = extentInMapSRS.xMax(), a_max[1] = extentInMapSRS.yMax();
                    layerIndex->Insert(a_min, a_max, maxLevel);
                }
            }
            newData.index[layer] = layerIndex;
        }
    }

    {
        ScopedWriteLock lk(_mapDataMutex);

        for (auto& itr : _mapData.index)
            if (itr.second)
                delete static_cast<MaxLevelIndex*>(itr.second);

        std::swap(_mapData, newData);
    }
}

int
ElevationPool::getLOD(double x, double y, WorkingSet* ws)
{
    double point[2] = { x, y };
    int maxiestMaxLevel = -1;

    auto& layers =
        (ws && ws->_elevationLayers.size() > 0) ? ws->_elevationLayers :
        _mapData.layers;

    for (auto& layerItr : layers)
    {
        auto itr = _mapData.index.find(layerItr.get());
        if (itr != _mapData.index.end())
        {
            MaxLevelIndex* index = static_cast<MaxLevelIndex*>(itr->second);
            index->Search(point, point, [&](const unsigned& level)
                {
                    maxiestMaxLevel = std::max(maxiestMaxLevel, (int)level);
                    return RTREE_KEEP_SEARCHING;
                });
        }
    }

    return maxiestMaxLevel;
}

ElevationPool::WorkingSet::WorkingSet(unsigned size) :
    _lru(size)
{
    //nop
}

void
ElevationPool::WorkingSet::clear()
{
    _lru.clear();
    // No need to clear the elevation layers; only invalidate the cache.
}

bool
ElevationPool::findExistingRaster(
    const Internal::RevElevationKey& key,
    osg::ref_ptr<ElevationTile>& output,
    bool* fromLUT)
{
    OE_PROFILING_ZONE;

    *fromLUT = false;

    // Next check the system LUT -- see if someone somewhere else
    // already has it (the terrain or another WorkingSet)
    optional<Internal::RevElevationKey> orphanedKey;
    {
        ScopedReadLock lock(_globalLUTMutex);

        auto i = _globalLUT.find(key);
        if (i != _globalLUT.end())
        {
            i->second.lock(output);
            if (output.valid())
            {
                *fromLUT = true;
            }
            else
            {
                // observer was orphaned..remove it
                orphanedKey = key;
            }
        }
    }

    if (orphanedKey.isSet())
    {
        ScopedWriteLock lock(_globalLUTMutex);
        _globalLUT.erase(orphanedKey.get());
    }

    return output.valid();
}

osg::ref_ptr<ElevationTile>
ElevationPool::getOrCreateRaster(MapData& snapshot, const Internal::RevElevationKey& key, bool acceptLowerRes, ProgressCallback* progress)
{
    OE_PROFILING_ZONE;

    // first check for pre-existing data for this key:
    osg::ref_ptr<ElevationTile> result;
    bool fromLUT;

    findExistingRaster(key, result, &fromLUT);

    if (!result.valid())
    {
        // need to build NEW data for this key
        osg::ref_ptr<osg::HeightField> hf = HeightFieldUtils::createReferenceHeightField(
            key._tilekey.getExtent(),
            _tileSize, _tileSize,
            false,      // no border
            true);      // initialize to HAE (0.0) heights

        std::vector<float> resolutions;
        resolutions.assign(_tileSize * _tileSize, FLT_MAX);

        TileKey keyToUse;
        bool populated = false;

        for (keyToUse = key._tilekey; keyToUse.valid(); keyToUse.makeParent())
        {
            populated = snapshot.layers.populateHeightField(
                hf.get(),
                &resolutions,
                keyToUse,
                snapshot.mapProfileNoVDatum.get(),
                snapshot.interpolation,
                progress);

            // Resolve any invalid heights in the output heightfield.
            HeightFieldUtils::resolveInvalidHeights(hf.get(), keyToUse.getExtent(), NO_DATA_VALUE, 0L);

            if ((populated == true) ||
                (acceptLowerRes == false) ||
                (progress && progress->isCanceled()))
            {
                break;
            }
        }

        // check for cancelation/deferral
        if (progress && progress->isCanceled())
        {
            return NULL;
        }

        if (populated)
        {
            result = new ElevationTile(
                keyToUse,
                GeoHeightField(hf.get(), keyToUse.getExtent()),
                std::move(resolutions));
        }
        else
        {
            return NULL;
        }
    }

    else
    {
        // found it ... but if it's a lower res tile and we aren't accepting
        // those, discard it.
        if (acceptLowerRes == false &&
            result->getTileKey() != key._tilekey)
        {
            return NULL;
        }
    }

    // update the L2 cache:
    _L2.insert(key, result);

    // update system weak-LUT:
    if (!fromLUT)
    {
        ScopedWriteLock lock(_globalLUTMutex);
        _globalLUT[key] = result.get();
    }

    return result;
}


bool
ElevationPool::prepareEnvelope(ElevationPool::Envelope& env, const GeoPoint& refPoint,
    const Distance& resolution, WorkingSet* ws)
{
    env._ws = ws;

    env._mapDataSnapshot = snapshotMapData(ws);

    env._pool = this;
    env._map = nullptr;
    env._profile = nullptr;

    if (_mapData.map.lock(env._map) == false || env._map->getProfile() == nullptr)
        return false;

    env._profile = env._map->getProfile();

    env._key._hash = env._mapDataSnapshot.hash;

    env._raster = nullptr;
    env._cache.clear();

    env._pw = env._profile->getExtent().width();
    env._ph = env._profile->getExtent().height();
    env._pxmin = env._profile->getExtent().xMin();
    env._pymin = env._profile->getExtent().yMin();

    auto& units = env._map->getSRS()->getUnits();
    Distance pointRes(0.0, units);

    GeoPoint refPointMap = refPoint.transform(env._map->getSRS());

    double resolutionInMapUnits = env._map->getSRS()->transformDistance(resolution, units, refPointMap.y());

    int maxLOD = env._profile->getLevelOfDetailForHorizResolution(
        resolutionInMapUnits,
        ELEVATION_TILE_SIZE);

    env._lod = std::min(getLOD(refPointMap.x(), refPointMap.y(), ws), (int)maxLOD);

    // This can happen if the elevation data publishes no data extents
    if (env._lod < 0 && !_mapData.layers.empty())
    {
        env._lod = maxLOD;
    }

    env._profile->getNumTiles(env._lod, env._tw, env._th);

    env._ws = ws;

    if (env._ws == nullptr)
        env._ws = &env._default_ws;

    return true;
}

int
ElevationPool::Envelope::sampleMapCoords(std::vector<osg::Vec3d>::iterator begin, std::vector<osg::Vec3d>::iterator end,
    ProgressCallback* progress, float failValue)
{
    OE_PROFILING_ZONE;

    if (begin == end)
        return -1;

    if (_lod < 0)
    {
        for (auto i = begin; i != end; ++i)
            i->z() = failValue;
        return 0;
    }

    double u, v;
    double rx, ry;
    int tx, ty;
    int tx_prev = INT_MAX, ty_prev = INT_MAX;
    float lastRes = -1.0f;
    int lod = _lod;
    int lod_prev = INT_MAX;
    osg::Vec4f elev;
    int count = 0;

    for (auto iter = begin; iter != end; ++iter)
    {
        auto& p = *iter;

        bool tileKeyChanged = false;

        rx = (p.x() - _pxmin) / _pw, ry = (p.y() - _pymin) / _ph;
        tx = osg::clampBelow((unsigned)(rx * (double)_tw), _tw - 1u); // TODO: wrap around for geo
        ty = osg::clampBelow((unsigned)((1.0 - ry) * (double)_th), _th - 1u);

        if (lod != lod_prev || tx != tx_prev || ty != ty_prev)
        {
            _key._tilekey = TileKey(lod, tx, ty, _profile.get());
            lod_prev = lod;
            tx_prev = tx;
            ty_prev = ty;
            tileKeyChanged = true;
        }

        if (_key._tilekey.valid())
        {
            if (tileKeyChanged || !_raster.valid())
            {
                auto iter = _cache.find(_key);

                if (iter == _cache.end())
                {
                    _raster = _pool->getOrCreateRaster(
                        _mapDataSnapshot,
                        _key,   // key to query
                        true,  // fall back on lower resolution data if necessary
                        progress);

                    // bail on cancelation before using the quickcache
                    if (progress && progress->isCanceled())
                    {
                        return -1;
                    }

                    _cache[_key] = _raster.get();

                    if (_ws)
                        _ws->_lru.insert(_key, _raster);
                }
                else
                {
                    _raster = iter->second;
                }
            }

            if (_raster.valid())
            {
                u = (p.x() - _raster->getExtent().xMin()) / _raster->getExtent().width();
                v = (p.y() - _raster->getExtent().yMin()) / _raster->getExtent().height();

                // Note: This can happen on the map edges..
                // TODO: consider looping around for geo and clamping for projected
                u = clamp(u, 0.0, 1.0);
                v = clamp(v, 0.0, 1.0);

                p.z() = _raster->getRawElevationUV(u, v);
            }
            else
            {
                p.z() = failValue;
            }
        }
        else
        {
            p.z() = failValue;
        }

        if (p.z() != failValue)
            ++count;
    }

    return count;
}

int
ElevationPool::sampleMapCoords(
    std::vector<osg::Vec4d>::iterator begin,
    std::vector<osg::Vec4d>::iterator end,
    WorkingSet* ws,
    ProgressCallback* progress,
    float failValue)
{
    OE_PROFILING_ZONE;

    if (begin == end)
        return -1;

    osg::ref_ptr<const Map> map;
    if (_mapData.map.lock(map) == false || map->getProfile() == NULL)
        return -1;

    auto snapshot = snapshotMapData(ws);

    if (snapshot.layers.empty())
    {
        for (auto i = begin; i != end; ++i)
            i->z() = failValue;
        return 0;
    }

    Internal::RevElevationKey key;
    key._hash = snapshot.hash;

    osg::ref_ptr<ElevationTile> raster;
    osg::Vec4 elev;
    double u, v;

    const Profile* profile = map->getProfile();
    double pw = profile->getExtent().width();
    double ph = profile->getExtent().height();
    double pxmin = profile->getExtent().xMin();
    double pymin = profile->getExtent().yMin();

    int count = 0;

    Envelope::QuickCache quickCache;

    unsigned tw, th;
    double rx, ry;
    int tx, ty;
    int tx_prev = INT_MAX, ty_prev = INT_MAX;

    int lod;
    int lod_prev = INT_MAX;
    auto* srs = map->getSRS();
    auto& units = srs->getUnits();
    Distance pointRes(0.0, units);

    for (auto iter = begin; iter != end; ++iter)
    {
        auto& p = *iter;

        if (p.w() == FLT_MAX)
            continue;

        bool tileKeyChanged = false;

        pointRes.set(p.w(), units);

            double resolutionInMapUnits = srs->transformDistance(pointRes, units, p.y());

        lod = profile->getLevelOfDetailForHorizResolution(
            resolutionInMapUnits,
            ELEVATION_TILE_SIZE);

        profile->getNumTiles(lod, tw, th);

        rx = (p.x() - pxmin) / pw, ry = (p.y() - pymin) / ph;
        tx = osg::clampBelow((unsigned)(rx * (double)tw), tw - 1u); // TODO: wrap around for geo
        ty = osg::clampBelow((unsigned)((1.0 - ry) * (double)th), th - 1u);

        if (lod != lod_prev || tx != tx_prev || ty != ty_prev)
        {
            key._tilekey = TileKey(lod, tx, ty, profile);
            lod_prev = lod;
            tx_prev = tx;
            ty_prev = ty;
            tileKeyChanged = true;
        }

        if (key._tilekey.valid())
        {
            if (tileKeyChanged || !raster.valid())
            {
                auto iter = quickCache.find(key);

                if (iter == quickCache.end())
                {
                    const bool fallback_if_necessary = true;

                    raster = getOrCreateRaster(snapshot, key, fallback_if_necessary, progress);

                    // bail on cancelation before using the quickcache
                    if (progress && progress->isCanceled())
                    {
                        return -1;
                    }

                    quickCache[key] = raster.get();
                }
                else
                {
                    raster = iter->second;
                }
            }

            if (raster.valid())
            {
                u = (p.x() - raster->getExtent().xMin()) / raster->getExtent().width();
                v = (p.y() - raster->getExtent().yMin()) / raster->getExtent().height();

                // Note: This can happen on the map edges..
                // TODO: consider looping around for geo and clamping for projected
                u = clamp(u, 0.0, 1.0);
                v = clamp(v, 0.0, 1.0);

                p.z() = raster->getRawElevationUV(u, v);
            }
            else
            {
                p.z() = failValue;
            }
        }
        else
        {
            p.z() = failValue;
        }

        if (p.z() != failValue)
            ++count;
    }

    return count;
}

int
ElevationPool::sampleMapCoords(
    std::vector<osg::Vec3d>::iterator begin,
    std::vector<osg::Vec3d>::iterator end,
    const Distance& resolution,
    WorkingSet* ws,
    ProgressCallback* progress,
    float failValue)
{    
    OE_PROFILING_ZONE;

    if (begin == end)
        return -1;

    osg::ref_ptr<const Map> map;
    if (_mapData.map.lock(map) == false || map->getProfile() == NULL)
        return -1;

    auto snapshot = snapshotMapData(ws);

    if (snapshot.layers.empty())
    {
        for (auto i = begin; i != end; ++i)
            i->z() = failValue;
        return 0;
    }

    Internal::RevElevationKey key;
    key._hash = snapshot.hash;
        

    osg::ref_ptr<ElevationTile> raster;
    double u, v;

    const Profile* profile = map->getProfile();
    double pw = profile->getExtent().width();
    double ph = profile->getExtent().height();
    double pxmin = profile->getExtent().xMin();
    double pymin = profile->getExtent().yMin();

    int count = 0;

    Envelope::QuickCache quickCache;

    unsigned tw, th;
    double rx, ry;
    int tx, ty;
    int tx_prev = INT_MAX, ty_prev = INT_MAX;

    int lod;
    int lod_prev = INT_MAX;
    auto* srs = map->getSRS();
    auto& units = srs->getUnits();

    for (auto iter = begin; iter != end; ++iter)
    {
        auto& p = *iter;

        bool tileKeyChanged = false;

        double resolutionInMapUnits = srs->transformDistance(resolution, units, p.y());

        int computedLOD = profile->getLevelOfDetailForHorizResolution(
            resolutionInMapUnits,
            ELEVATION_TILE_SIZE);

        lod = std::min(getLOD(p.x(), p.y(), ws), (int)computedLOD);

        if (lod < 0)
        {
            p.z() = failValue;
            continue;
        }

        profile->getNumTiles(lod, tw, th);
        rx = (p.x() - pxmin) / pw, ry = (p.y() - pymin) / ph;
        tx = osg::clampBelow((unsigned)(rx * (double)tw), tw - 1u);
        ty = osg::clampBelow((unsigned)((1.0 - ry) * (double)th), th - 1u);

        if (lod != lod_prev || tx != tx_prev || ty != ty_prev)
        {
            key._tilekey = TileKey(lod, tx, ty, profile);
            lod_prev = lod;
            tx_prev = tx;
            ty_prev = ty;
            tileKeyChanged = true;
        }

        if (key._tilekey.valid())
        {
            if (tileKeyChanged || !raster.valid())
            {
                auto iter = quickCache.find(key);

                if (iter == quickCache.end())
                {
                    raster = getOrCreateRaster(snapshot, key, true /* fallback */, progress);

                    // bail on cancelation before using the quickcache
                    if (progress && progress->isCanceled())
                    {
                        return -1;
                    }

                    quickCache[key] = raster.get();
                }
                else
                {
                    raster = iter->second;
                }
            }

            if (raster.valid())
            {
                u = (p.x() - raster->getExtent().xMin()) / raster->getExtent().width();
                v = (p.y() - raster->getExtent().yMin()) / raster->getExtent().height();

                // Note: This can happen on the map edges..
                // TODO: consider looping around for geo and clamping for projected
                u = clamp(u, 0.0, 1.0);
                v = clamp(v, 0.0, 1.0);

                p.z() = raster->getRawElevationUV(u, v);
            }
            else
            {
                p.z() = failValue;
            }
        }
        else
        {
            p.z() = failValue;
        }

        if (p.z() != failValue)
            ++count;
    }

    return count;
}

ElevationSample
ElevationPool::getSample(const GeoPoint& p, unsigned maxLOD, WorkingSet* ws, ProgressCallback* progress)
{
    auto snapshot = snapshotMapData(ws);

    if (snapshot.layers.empty())
        return {};

    Internal::RevElevationKey key;

    // Need to limit maxLOD <= INT_MAX else std::min for lod will return -1 due to cast
    maxLOD = std::min(maxLOD, static_cast<unsigned>(std::numeric_limits<int>::max()));

    // returns the best LOD for the given point, or -1 if there is no data there
    int lod = std::min(getLOD(p.x(), p.y(), ws), (int)maxLOD);

    if (lod >= 0)
    {
        key._tilekey = snapshot.mapProfile->createTileKey(p.x(), p.y(), lod);
        key._hash = snapshot.hash;

        osg::ref_ptr<ElevationTile> raster = getOrCreateRaster(
            snapshot,
            key,   // key to query
            true,  // fall back on lower resolution data if necessary
            progress);

        if (raster.valid())
        {
            if (ws)
                ws->_lru.insert(key, raster);

            return raster->getElevation(p.x(), p.y());
        }
    }
    return ElevationSample();
}

ElevationSample
ElevationPool::getSample(const GeoPoint& p, WorkingSet* ws, ProgressCallback* progress)
{
    if (!p.isValid())
        return {};

    osg::ref_ptr<const Map> map;
    if (_mapData.map.lock(map) == false || map->getProfile() == nullptr)
        return {};

    if (!p.getSRS()->isHorizEquivalentTo(map->getSRS()))
    {
        GeoPoint xp(p);
        xp.transformInPlace(map->getSRS());
        return getSample(xp, ~0, ws, progress);
    }
    else
    {
        return getSample(p, ~0, ws, progress);
    }
}

ElevationSample
ElevationPool::getSample(const GeoPoint& p, const Distance& resolution, WorkingSet* ws, ProgressCallback* progress)
{
    if (!p.isValid())
        return {};

    osg::ref_ptr<const Map> map;
    {
        ScopedReadLock lock(_mapDataMutex);
        if (_mapData.layers.empty())
            return {};
        if (_mapData.map.lock(map) == false || map->getProfile() == nullptr)
            return {};
    }

    // mostly right. :)
    double resolutionInMapUnits = SpatialReference::transformUnits(
        resolution,
        map->getSRS(),
        p.y());

    unsigned maxLOD = map->getProfile()->getLevelOfDetailForHorizResolution(
        resolutionInMapUnits,
        ELEVATION_TILE_SIZE);

    if (!p.getSRS()->isHorizEquivalentTo(map->getSRS()))
    {
        GeoPoint xp(p);
        xp.transformInPlace(map->getSRS());
        return getSample(xp, maxLOD, ws, progress);
    }
    else
    {
        return getSample(p, maxLOD, ws, progress);
    }
}

bool
ElevationPool::getTile(const TileKey& tilekey, bool acceptLowerRes, osg::ref_ptr<ElevationTexture>& out_tex,
    WorkingSet* ws, ProgressCallback* progress)
{
    MapData snapshot = snapshotMapData(ws);

    Internal::RevElevationKey key;
    key._tilekey = tilekey;
    key._hash = snapshot.hash;

    out_tex = getOrCreateRaster(snapshot, key, acceptLowerRes, progress);

    if (ws)
        ws->_lru.insert(key, out_tex);

    return out_tex.valid();
}

ElevationPool::MapData
ElevationPool::snapshotMapData(WorkingSet* ws)
{
    ScopedWriteLock exclusive(_mapDataMutex);

    // check for revision change.
    unsigned hash = 0;
    for (auto& layer : _mapData.layers) {
        hash = hash_value_unsigned(hash, layer->getUID(), layer->getRevision());
    }

    // update if necessary.
    _mapData.hash = hash;

    MapData out = _mapData;
    if (ws && !ws->_elevationLayers.empty())
    {
        out.layers = ws->_elevationLayers;

        // override with the hash of the WS layers:
        hash = 0;
        for(auto& layer : ws->_elevationLayers)
            hash = hash_value_unsigned(hash, layer->getUID(), layer->getRevision());
        out.hash = hash;
    }

    return out;
}

//...................................................................

AsyncElevationSampler::AsyncElevationSampler(const Map* map, unsigned numThreads) :
    _map(map),
    _arena(nullptr)
{
    _arena = jobs::get_pool("oe.asyncelevation");
    _arena->set_can_steal_work(false);
    _arena->set_concurrency(numThreads > 0 ? numThreads : _arena->concurrency());
}

Future<ElevationSample>
AsyncElevationSampler::getSample(const GeoPoint& p)
{
    return getSample(p, Distance(0, p.getXYUnits()));
}

Future<ElevationSample>
AsyncElevationSampler::getSample(const GeoPoint& point, const Distance& resolution)
{
    jobs::context c;
    c.pool = _arena;

    auto task = [=](Cancelable& cancelable)
        {
            ElevationSample sample;
            if (!cancelable.canceled())
            {
                osg::ref_ptr<const Map> map(_map);
                if (map.valid())
                {
                    osg::ref_ptr<ProgressCallback> progress = new ProgressCallback(&cancelable);

                    sample = map->getElevationPool()->getSample(point, resolution, &_ws, progress.get());
                }
            }
            return sample;
        };

    return jobs::dispatch(task, c);
}
