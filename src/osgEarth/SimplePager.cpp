#include <osgEarth/SimplePager>
#include <osgEarth/TileKey>
#include <osgEarth/CullingUtils>
#include <osgEarth/PagedNode>
#include <osgEarth/ElevationLayer>
#include <osgEarth/ElevationRanges>
#include <osgEarth/NodeUtils>
#include <osgDB/Registry>
#include <osg/ShapeDrawable>
#include <osg/MatrixTransform>

#include <osg/KdTree>

#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
#include <Superluminal/PerformanceAPI.h>
#endif

using namespace osgEarth;
using namespace osgEarth::Util;

#define LC "[SimplerPager] "


SimplePager::SimplePager(const osgEarth::Map* map, const osgEarth::Profile* profile) :
    _map(map),
    _profile(profile)
{
    if (map)
    {
        _mapProfile = map->getProfile();
    }
    else
    {
        _mapProfile = Profile::create(Profile::GLOBAL_GEODETIC);
    }
}

void SimplePager::setEnableCancelation(bool value)
{
    _canCancel = value;
}

bool SimplePager::getEnableCancelation() const
{
    return _canCancel;
}

void SimplePager::setClusterCullingEnabled(bool value)
{
    _clusterCullingEnabled = value;
}

bool SimplePager::getClusterCullingEnabled() const
{
    return _clusterCullingEnabled;
}

void SimplePager::setUsePayloadBoundsForChildren(bool value)
{
    _usePayloadBoundsForChildren = value;
}

bool SimplePager::getUsePayloadBoundsForChildren() const
{
    return _usePayloadBoundsForChildren;
}

void SimplePager::setRangeFactor(float value)
{
    _rangeFactor = value;
    _useRange = true;

    forEachNodeOfType<PagedNode2>(this, [&](auto* node)
        {
            // trick to switch over to range mode
            node->setMaxRange(node->getMaxRange());
        });
}

void SimplePager::setMaxRange(float value)
{
    _maxRange = value;
    _useRange = true;

    forEachNodeOfType<PagedNode2>(this, [&](auto* node)
        {
            node->setMaxRange(value);
        });
}

void SimplePager::setMinPixels(float value)
{
    _minPixels = value;
    _useRange = false;

    forEachNodeOfType<PagedNode2>(this, [&](auto* node)
        {
            node->setMinPixels(value);
        });
}

void SimplePager::setMaxPixels(float value)
{
    _maxPixels = value;
    _useRange = false;

    forEachNodeOfType<PagedNode2>(this, [&](auto* node)
        {
            node->setMaxPixels(value);
        });
}

void SimplePager::setLODMethod(const LODMethod& value)
{
    if (value == LODMethod::CAMERA_DISTANCE && !_useRange)
    {
        setRangeFactor(_rangeFactor);
    }
    else if (value == LODMethod::SCREEN_SPACE && _useRange)
    {
        setMaxPixels(_maxPixels);
    }
}

void SimplePager::setDone()
{
    _done = true;
}

void SimplePager::traverse(osg::NodeVisitor& nv)
{
    if (!_done && nv.getVisitorType() == nv.CULL_VISITOR && getNumChildren() == 0)
    {
        OE_WARN << LC << "Not initialized - did you forget to call build()?" << std::endl;
        setDone();
    }

    if (nv.getVisitorType() == nv.CULL_VISITOR)
    {
        PagedNodeManager::traverse(nv);
    }

    osg::Group::traverse(nv);
}

void SimplePager::build()
{
    addChild( buildRootNode() );
}

osg::BoundingSphered SimplePager::getBounds(const TileKey& key) const
{
    // TODO:  This is very similar to the code in FeatureModelGraph::getBoundInWorldCoords, consolidate it at some point.
    GeoExtent workingExtent;

    workingExtent = _mapProfile->clampAndTransformExtent(key.getExtent());

    GeoPoint center = workingExtent.getCentroid();
    unsigned lod = _mapProfile->getLOD(workingExtent.height());
    float minElevation = -100.0f;
    float maxElevation = 100.0f;

    ElevationLayerVector elevationLayers;
    osg::ref_ptr<const Map> map;
    _map.lock(map);
    if (map.valid())
    {
        map->getLayers<ElevationLayer>(elevationLayers);
        if (!elevationLayers.empty())
        {
            // Get the approximate elevation range if we have elevation data in the map
            lod = osg::clampBetween(lod, 0u, ElevationRanges::getMaxLevel());
            GeoPoint centerWGS84 = center.transform(ElevationRanges::getProfile()->getSRS());
            TileKey rangeKey = ElevationRanges::getProfile()->createTileKey(centerWGS84.x(), centerWGS84.y(), lod);
            short min, max;
            if (!*map->options().disableElevationRanges())
            {
                ElevationRanges::getElevationRange(rangeKey.getLevelOfDetail(), rangeKey.getTileX(), rangeKey.getTileY(), min, max);
            }
            else
            {
                ElevationRanges::getDefaultElevationRange(min, max);
            }
            // Clamp the min value to avoid extreme underwater values.
            minElevation = osg::maximum(min, (short)-500);
            // Add a little bit extra of extra height to account for feature data.
            maxElevation = max + 100.0f;
        }
    }
    return workingExtent.createWorldBoundingSphere(minElevation, maxElevation);

}

osg::ref_ptr<osg::Node> SimplePager::buildRootNode()
{
    osg::ref_ptr<osg::Group> root = new osg::Group();

    std::vector<TileKey> keys;
    _profile->getRootKeys( keys );
    osg::ref_ptr<ProgressCallback> prog = new ObserverProgressCallback(this);
    for (unsigned int i = 0; i < keys.size(); i++)
    {
        osg::ref_ptr<osg::Node> node = createChildNode(keys[i], prog.get());
        if ( node.valid() )
            root->addChild( node );
    }

    return root;
}

osg::ref_ptr<osg::Node>
SimplePager::createNode(const TileKey& key, ProgressCallback* progress)
{
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
    PERFORMANCEAPI_INSTRUMENT_FUNCTION();
    PERFORMANCEAPI_INSTRUMENT_DATA("key", key.str().c_str());
#endif

    if (_createNodeFunction)
    {
        return _createNodeFunction(key, progress);       
    }
    else
    {
        osg::BoundingSphered bounds = getBounds(key);
        if (bounds.valid())
        {
            osg::MatrixTransform* mt = new osg::MatrixTransform;
            mt->setMatrix(osg::Matrixd::translate(bounds.center()));
            osg::Geode* geode = new osg::Geode;
            osg::ShapeDrawable* sd = new osg::ShapeDrawable(new osg::Sphere(osg::Vec3f(0, 0, 0), bounds.radius()));
            sd->setColor(osg::Vec4(1, 0, 0, 1));
            geode->addDrawable(sd);
            mt->addChild(geode);
            return mt;
        }
        else
        {
            return {};
        }
    }
}

osg::ref_ptr<osg::Node>
SimplePager::createChildNode(const TileKey& key, ProgressCallback* progress)
{
    osg::ref_ptr<osg::Node> result;

    osg::BoundingSphered tileBounds = getBounds(key);
    double tileRadius = tileBounds.radius();

    // restrict subdivision to max level:
    bool hasChildren = key.getLOD() < _maxLevel;
    bool mayHavePayload = key.getLOD() >= _minLevel;

    // Create the actual drawable data for this tile.
    osg::ref_ptr<osg::Node> payload;

    // only create real node if we are at least at the min LOD:
    if (mayHavePayload)
    {
        payload = createNode(key, progress);

        if (progress && progress->canceled())
            return nullptr;

        if (payload.valid())
        {
            // Build kdtrees to increase intersection speed.
            if (osgDB::Registry::instance()->getKdTreeBuilder())
            {
                osg::ref_ptr< osg::KdTreeBuilder > kdTreeBuilder = osgDB::Registry::instance()->getKdTreeBuilder()->clone();
                payload->accept(*kdTreeBuilder.get());
            }
        }

#if 0
        // if we comment this out, the pager will continue to subdivide even up to the max level,
        // which MIGHT be desirable for some datasets with sparse data...
        else if (!_additive)
        {
            // If we are in REPLACE mode, and this node's payload did not appear,
            // we have run out of data and will stop here.
            // In ADD mode, we will continue to subdivide because we do not know when data will appear.
            hasChildren = false;
        }
#endif
    }

    if (hasChildren)
    {
        //osg::ref_ptr<PagedNode2> pagedNode = new PagedNode2();
        osg::ref_ptr<PagedNode2> pagedNode;
        if (_createPagedNodeFunction)
            pagedNode = _createPagedNodeFunction(key);
        else
            pagedNode = new PagedNode2();

        // Tell the owner that this pager is its creator and that it should use our
        // traversal data and cull callbacks.
        pagedNode->setOwner(this);

        // Install pre- and post- merge callbacks on the paged node.
        pagedNode->setSceneGraphCallbacks(getSceneGraphCallbacks());

        if (payload.valid())
        {
            // Add a drawable payload to this paged node.
            pagedNode->addChild(payload);

            // and tell people about it.
            onCreateNode.fire(key, payload.get());

            if (_usePayloadBoundsForChildren)
            {
                const auto& bs = payload->getBound();
                if (bs.valid())
                {
                    tileBounds.set(bs.center(), bs.radius());
                    tileRadius = tileBounds.radius();
                }
            }
        }

        // Set up the culling bounding sphere.
        pagedNode->setCenter(tileBounds.center());
        pagedNode->setRadius(tileRadius);

        // Install a cluster-culling callback for geocentric data:
        if (_clusterCullingEnabled && _mapProfile->getSRS()->isGeographic())
        {
            const GeoExtent& ccExtent = key.getExtent();
            if (ccExtent.isValid())
            {
                // if the extent is more than 90 degrees, bail
                GeoExtent geodeticExtent = ccExtent.transform(ccExtent.getSRS()->getGeographicSRS());
                if (geodeticExtent.width() < 90.0 && geodeticExtent.height() < 90.0)
                {
                    // get the geocentric tile center:
                    osg::Vec3d tileCenter;
                    ccExtent.getCentroid(tileCenter.x(), tileCenter.y());
                    osg::Vec3d centerECEF;
                    const SpatialReference* mapSRS = _mapProfile->getSRS();
                    if (mapSRS)
                    {
                        ccExtent.getSRS()->transform(tileCenter, mapSRS->getGeocentricSRS(), centerECEF);
                        osg::NodeCallback* ccc = ClusterCullingFactory::create(geodeticExtent);
                        if (ccc)
                        {
                            pagedNode->addCullCallback(ccc);
                        }
                    }
                }
            }
        }

        float loadRange = FLT_MAX;

        if (getName().empty())
            pagedNode->setName(key.str());
        else
            pagedNode->setName(getName() + " " + key.str());

        pagedNode->setPriorityScale(_priorityScale);

        // Now set up a loader that will load the child data
        osg::observer_ptr<SimplePager> pager_weakptr(this);

        pagedNode->setLoadFunction([pager_weakptr, key](Cancelable* c)
            {
                osg::ref_ptr<osg::Node> result;
                if (c && c->canceled())
                    return result;

                osg::ref_ptr<SimplePager> pager;
                if (pager_weakptr.lock(pager))
                {
                    osg::ref_ptr<ProgressCallback> progress = new ProgressCallback(c);
                    result = pager->createPagedChildrenOf(key, progress.get());
                }
                return result;
            });

        loadRange = (float)(tileRadius * _rangeFactor);

        pagedNode->setRefinePolicy(_additive ? REFINE_ADD : REFINE_REPLACE);

        pagedNode->setMaxRange(std::min(loadRange, _maxRange));

        if (!_useRange)
        {
            pagedNode->setMinPixels(_minPixels);
            pagedNode->setMaxPixels(_maxPixels);
        }

        result = pagedNode;
    }

    else // !hasChildren
    {
        if (payload.valid())
        {
            onCreateNode.fire(key, payload.get());
            result = payload;
        }
    }

    if (result.valid())
    {
        if (getName().empty())
            result->setName(key.str());
        else
            result->setName(getName() + " " + key.str());
    }

    return result;
}

osg::ref_ptr<osg::Node>
SimplePager::createPagedChildrenOf(const TileKey& parentKey, ProgressCallback* progress)
{
    if (_done)
    {
        if (progress) progress->cancel();
        return {};
    }

    osg::ref_ptr< osg::Group >  group = new osg::Group;

    for (unsigned int i = 0; i < 4; i++)
    {
        TileKey childKey = parentKey.createChildKey( i );

        osg::ref_ptr<osg::Node> child = createChildNode(childKey, progress);
        if (child.valid())
        {
            group->addChild(child);
        }
    }
    if (group->getNumChildren() > 0)
    {
        return group;
    }
    return {};
}

const osgEarth::Profile* SimplePager::getProfile() const
{
    return _profile.get();
}
