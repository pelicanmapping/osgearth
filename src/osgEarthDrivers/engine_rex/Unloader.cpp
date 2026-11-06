/* osgEarth
 * Copyright 2008-2014 Pelican Mapping
 * MIT License
 */
#include "Unloader"
#include "TileNode"
#include "TileNodeRegistry"

#include <osgEarth/Metrics>
#include <osgEarth/NodeUtils>
#include <osgEarth/Registry>
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
#include <Superluminal/PerformanceAPI.h>
#endif

#undef  LC
#define LC "[UnloaderGroup] "

using namespace osgEarth::REX;


UnloaderGroup::UnloaderGroup(TileNodeRegistry* tiles, const TerrainOptionsAPI& api) :
    _options(api),
    _tiles(tiles),
    _releaser(Registry::instance()->getObjectReleaser()->createClient("REX terrain")),
    //_minResidentTiles(0u),
    //_maxAge(0.1),
    //_minRange(0.0f),
    //_maxTilesToUnloadPerFrame(~0),
    _frameLastUpdated(0u)
{
    ADJUST_UPDATE_TRAV_COUNT(this, +1);
}

void
UnloaderGroup::traverse(osg::NodeVisitor& nv)
{
    if ( nv.getVisitorType() == nv.UPDATE_VISITOR )
    {
        unsigned frame = _clock->getFrame();
        bool runUpdate = (_frameLastUpdated < frame);

        if (runUpdate && _releaser.available() && _tiles->size() > _options.getMinResidentTiles())
        {
            _frameLastUpdated = frame;

            OE_PROFILING_ZONE_NAMED("Expire Tiles");
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
            PERFORMANCEAPI_INSTRUMENT("Retire expired tiles");
#endif

            double now = _clock->getTime();

            // Have to enforce both the time delay AND a frame delay since the frames can
            // stop while the time rolls on (e.g., if you are dragging the window)
            double oldestAllowableTime = now - _options.getMinExpiryTime();
            unsigned oldestAllowableFrame = osg::maximum(frame, 3u) - 3u;

            // Remove them from the registry:
            _tiles->collectDormantTiles(
                nv,
                oldestAllowableTime,
                oldestAllowableFrame,
                _options.getMinExpiryRange(),
                _options.getMaxTilesToUnloadPerFrame(),
                _deadpool);

            // Keep every collected tile (including registry-only orphans) and detached sibling
            // alive until all update-thread temporary references have gone out of scope.
            osg::NodeList retired;
            retired.reserve(_deadpool.size() * 2);
            for (auto& tile : _deadpool)
            {
                retired.push_back(tile.get());
                osg::ref_ptr<TileNode> parent;
                tile->getParentTile(parent);
                if (parent.valid())
                {
                    // Keep temporary parent ownership in the same handoff as its children.
                    retired.push_back(parent.get());
                    if (parent->getNumChildren() != 0)
                        parent->removeSubTiles(retired);
                }
            }

            _deadpool.clear();
            _releaser.release(retired);
        }
    }

    osg::Group::traverse( nv );
}

