/* osgEarth
* Copyright 2008-2014 Pelican Mapping
* MIT License
*/
#include "TileNodeRegistry"

#include <osgEarth/Metrics>

using namespace osgEarth::REX;
using namespace osgEarth;

#define LC "[TileNodeRegistry] "

#define OE_TEST OE_NULL

#define SENTRY_VALUE nullptr

#define PROFILING_REX_TILES "Live Terrain Tiles"

//----------------------------------------------------------------------------

TileNodeRegistry::TileNodeRegistry() :
    _notifyNeighbors(false)
{
    //nop
}

TileNodeRegistry::~TileNodeRegistry()
{
    releaseAll(nullptr);
}

void
TileNodeRegistry::setNotifyNeighbors(bool value)
{
    _notifyNeighbors = value;
}

void
TileNodeRegistry::setDirty(
    const GeoExtent& extent,
    unsigned minLevel,
    unsigned maxLevel,
    const CreateTileManifest& manifest)
{
    std::lock_guard<std::mutex> lock(_mutex);
    ++_childPreparationRevision;
    
    for( TileTable::iterator i = _tiles.begin(); i != _tiles.end(); ++i )
    {
        const TileKey& key = i->first;

        if (minLevel <= key.getLOD() && 
            maxLevel >= key.getLOD() &&
            (extent.isInvalid() || extent.intersects(key.getExtent())))
        {
            i->second._tile->refreshLayers(manifest);
        }
    }
}

void
TileNodeRegistry::add(TileNode* tile)
{
    std::lock_guard<std::mutex> lock(_mutex);

    auto& entry = _tiles[tile->getKey()];
    bool recyclingOrphan = entry._trackerToken != nullptr;

    // Important: prevent duplicate tracker entries for the same key.
    if (entry._trackerToken != nullptr)
    {
        _tracker.erase(entry._trackerToken);
        entry._trackerToken = nullptr;
    }

    entry._tile = tile;
    entry._trackerToken = _tracker.use(entry._tile, nullptr);

    // Start waiting on our neighbors.
    // (If we're recycling and orphaned record, we need to remove old listeners first)
    if (_notifyNeighbors)
    {
        const TileKey& key = tile->getKey();

        if (recyclingOrphan)
        {
            stopListeningFor(key.createNeighborKey(1, 0), key);
            stopListeningFor(key.createNeighborKey(0, 1), key);
        }

        startListeningFor(key.createNeighborKey(1, 0), tile);
        startListeningFor(key.createNeighborKey(0, 1), tile);

        TileKeyOneToMany::iterator notifier = _notifiers.find(tile->getKey());
        if (notifier != _notifiers.end())
        {
            TileKeySet& listeners = notifier->second;

            for (TileKeySet::iterator listener = listeners.begin(); listener != listeners.end(); ++listener)
            {
                TileTable::iterator i = _tiles.find(*listener);
                if (i != _tiles.end())
                {
                    i->second._tile->notifyOfArrival(tile);
                }
            }
            _notifiers.erase(notifier);
        }

        OE_DEBUG << LC
            << ": tiles=" << _tiles.size()
            << ", notifiers=" << _notifiers.size()
            << std::endl;
    }
}

void
TileNodeRegistry::startListeningFor(
    const TileKey& tileToWaitFor,
    TileNode* waiter)
{
    // ASSUME EXCLUSIVE LOCK

    TileTable::iterator i = _tiles.find(tileToWaitFor);
    if (i != _tiles.end())
    {
        TileNode* tile = i->second._tile.get();

        //OE_DEBUG << LC << waiter->getKey().str() << " listened for " << tileToWaitFor.str()
        //    << ", but it was already in the repo.\n";

        waiter->notifyOfArrival( tile );
    }
    else
    {
        //OE_DEBUG << LC << waiter->getKey().str() << " listened for " << tileToWaitFor.str() << ".\n";
        _notifiers[tileToWaitFor].insert( waiter->getKey() );
    }
}

void
TileNodeRegistry::stopListeningFor(
    const TileKey& tileToWaitFor,
    const TileKey& waiterKey)
{
    // ASSUME EXCLUSIVE LOCK

    TileKeyOneToMany::iterator i = _notifiers.find(tileToWaitFor);
    if (i != _notifiers.end())
    {
        // remove the waiter from this set:
        i->second.erase(waiterKey);

        // if the set is now empty, remove the set entirely
        if (i->second.empty())
        {
            _notifiers.erase(i);
        }
    }
}

void
TileNodeRegistry::releaseAll(osg::State* state)
{
    ++_childPreparationRevision;
    std::lock_guard<std::mutex> lock(_mutex);

    for (auto& tile : _tiles)
    {
        tile.second._tile->releaseGLObjects(state);
    }
    _tiles.clear();

    _tracker.reset();

    _notifiers.clear();

    _tilesToUpdate.clear();

    OE_PROFILING_PLOT(PROFILING_REX_TILES, (float)(_tiles.size()));
}

void
TileNodeRegistry::touch(TileNode* tile, osg::NodeVisitor& nv)
{
    std::lock_guard<std::mutex> lock(_mutex);

    TileTable::iterator i = _tiles.find(tile->getKey());

    OE_SOFT_ASSERT_AND_RETURN(i != _tiles.end(), void());

    _tracker.use(tile, i->second._trackerToken);

    if (tile->updateRequired())
    {
        _tilesToUpdate.push_back(tile->getKey());
    }
}

void
TileNodeRegistry::update(osg::NodeVisitor& nv)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_tilesToUpdate.empty())
    {
        // Sorting these from high to low LOD will reduce the number 
        // of inheritance steps each updated image will have to perform
        // against the tile's children
        std::sort(
            _tilesToUpdate.begin(),
            _tilesToUpdate.end(),
            [](const TileKey& lhs, const TileKey& rhs) {
                return lhs.getLOD() > rhs.getLOD();
            });

        for (auto& key : _tilesToUpdate)
        {
            auto iter = _tiles.find(key);
            if (iter != _tiles.end())
            {
                iter->second._tile->update(nv);
            }
        }

        _tilesToUpdate.clear();
    }
}

void
TileNodeRegistry::collectDormantTiles(
    osg::NodeVisitor& nv,
    double oldestAllowableTime,
    unsigned oldestAllowableFrame,
    float farthestAllowableRange,
    unsigned maxTiles,
    std::vector<osg::ref_ptr<TileNode>>& output)
{
    std::lock_guard<std::mutex> lock(_mutex);

    unsigned count = 0u;

    const auto disposeTile = [&](osg::ref_ptr<TileNode>& tile) -> bool
    {
        OE_SOFT_ASSERT_AND_RETURN(tile, true);

        const TileKey& key = tile->getKey();

        if (tile->getDoNotExpire() == false &&
            tile->getLastTraversalTime() < oldestAllowableTime &&
            tile->getLastTraversalFrame() < (int)oldestAllowableFrame &&
            tile->getLastTraversalRange() > farthestAllowableRange &&
            tile->areSiblingsDormant())
        {
            if (_notifyNeighbors)
            {
                // remove neighbor listeners:
                stopListeningFor(key.createNeighborKey(1, 0), key);
                stopListeningFor(key.createNeighborKey(0, 1), key);
            }

            output.push_back(tile);

            _tiles.erase(key);

            return true; // dispose it
        }
        else
        {
            tile->resetTraversalRange();

            return false; // keep it
        }
    };
    
    _tracker.flush(maxTiles, disposeTile);

    OE_PROFILING_PLOT(PROFILING_REX_TILES, (float)(_tiles.size()));
}
