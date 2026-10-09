/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include "Terrain"
#include "TerrainTileNode"
#include "Math"
#include "Utils"
#include <osgViewer/View>

#define LC "[Terrain] "

using namespace osgEarth;

//---------------------------------------------------------------------------

Terrain::onTileUpdateOperation::onTileUpdateOperation(const TileKey& key, osg::Node* node, Terrain* terrain)
    : osg::Operation("onTileUpdate", true),
        _terrain(terrain), _key(key), _node(node), _count(0), _delay(0) { }

void Terrain::onTileUpdateOperation::operator()(osg::Object*)
{
    if ( getKeep() == false )
        return;

    if (_delay-- > 0)
        return;

    ++_count;
    osg::ref_ptr<Terrain>   terrain;
    osg::ref_ptr<osg::Node> node;
    
    if ( _terrain.lock(terrain) && (!_key.valid() || _node.lock(node)) )
    {
        if (_key.valid())
            terrain->fireTileUpdate( _key, node.get() );
        else
            terrain->fireTileUpdate( _key, 0L );

    }
    else
    {
        // nop; tile expired before notification; let it go.
    }

    this->setKeep( false );
}

//---------------------------------------------------------------------------

Terrain::Terrain(osg::Node* graph, const Profile* mapProfile) :
    _graph(graph),
    _profile(mapProfile),
    _callbacksSize(0)
{
    _updateQueue = new osg::OperationQueue();
}

void
Terrain::update()
{
    _updateQueue->runOperations();
}

bool
Terrain::getHeight(osg::Node*              patch,
                   const SpatialReference* srs,
                   double                  x, 
                   double                  y, 
                   double*                 out_hamsl,
                   double*                 out_hae    ) const
{
    if ( !_graph.valid() && !patch )
        return 0L;

    // convert to map coordinates:
    if ( srs && !srs->isHorizEquivalentTo(getSRS()) )
    {
        srs->transform2D(x, y, getSRS(), x, y);
    }

    // trivially reject a point that lies outside the terrain:
    if ( !getProfile()->getExtent().contains(x, y) )
        return 0L;

    if (srs && srs->isGeographic())
    {
        // perturb polar latitudes slightly to prevent intersection anomaly at the poles
        if (getSRS()->isGeographic())
        {
            if (osg::equivalent(y, 90.0))
                y -= 1e-7;
            else if (osg::equivalent(y, -90))
                y += 1e-7;
        }
    }

    const Ellipsoid& em = getSRS()->getEllipsoid();
    double r = std::min( em.getRadiusEquator(), em.getRadiusPolar() );

    // calculate the endpoints for an intersection test:
    osg::Vec3d start(x, y, r);
    osg::Vec3d end  (x, y, -r);

    if ( getSRS()->isGeographic() )
    {
        const SpatialReference* ecef = getSRS()->getGeocentricSRS();
        getSRS()->transform(start, ecef, start);
        getSRS()->transform(end,   ecef, end);
    }

    osgUtil::LineSegmentIntersector* lsi = new osgUtil::LineSegmentIntersector( start, end );
    lsi->setIntersectionLimit(osgUtil::Intersector::LIMIT_NEAREST);

    osgUtil::IntersectionVisitor iv( lsi );
 
    if ( patch )
        patch->accept( iv );
    else
        _graph->accept( iv );

    osgUtil::LineSegmentIntersector::Intersections& results = lsi->getIntersections();
    if ( !results.empty() )
    {
        const osgUtil::LineSegmentIntersector::Intersection& firstHit = *results.begin();
        osg::Vec3d hit = firstHit.getWorldIntersectPoint();

        getSRS()->transformFromWorld(hit, hit, out_hae);
        if ( out_hamsl )
            *out_hamsl = hit.z();

        return true;
    }
    return false;
}


bool
Terrain::getHeight(const SpatialReference* srs,
                   double                  x, 
                   double                  y,
                   double*                 out_hamsl,
                   double*                 out_hae ) const
{
    return getHeight( (osg::Node*)0L, srs, x, y, out_hamsl, out_hae );
}

namespace
{    
    bool
    intersectMouse(
        osg::View* view, float x, float y,
        osg::Node* graph,
        osgUtil::LineSegmentIntersector::Intersection& result)
    {
        osgViewer::View* view2 = dynamic_cast<osgViewer::View*>(view);
        if (!view2 || !graph)
            return false;

        float local_x = x, local_y = y;
        const osg::Camera* camera = Util::getCameraUnderMouse(view2, x, y, local_x, local_y);
        if (!camera)
            return false;

        // Build a matrix that transforms from the terrain/world space
        // to either clip or window space, depending on whether we have
        // a viewport. Is it even possible to not have a viewport? -gw
        osg::Matrixd matrix;

        // compensate for any transforms applied between terrain and camera:
        osg::Matrix terrainRefFrame = osg::computeLocalToWorld(graph->getParentalNodePaths()[0]);
        matrix.postMult(terrainRefFrame);

        osg::Matrixd proj = camera->getProjectionMatrix();
        if (proj(3, 3) == 0) //persp
        {
            // persp camera: adjust the near plane to 1.0
            double V, A, N, F;
            ProjectionMatrix::getPerspective(camera->getProjectionMatrix(), V, A, N, F);
            ProjectionMatrix::setPerspective(proj, V, A, 1.0, F);
            proj.makePerspective(V, A, 1.0, F);
        }

        matrix.postMult(camera->getViewMatrix());
        matrix.postMult(proj);

        double zNear = -1.0;
        double zFar = 1.0;
        if (camera->getViewport())
        {
            matrix.postMult(camera->getViewport()->computeWindowMatrix());
            zNear = 0.0, zFar = 1.0;
        }

        osg::Matrixd inverse;
        inverse.invert(matrix);

        osg::Vec3d startVertex = osg::Vec3d(local_x, local_y, zNear) * inverse;
        osg::Vec3d endVertex = osg::Vec3d(local_x, local_y, zFar) * inverse;

        auto picker = new osgUtil::LineSegmentIntersector(
            osgUtil::Intersector::MODEL,
            startVertex,
            endVertex);

        // Limit it to one intersection; we only care about the nearest.
        picker->setIntersectionLimit(osgUtil::Intersector::LIMIT_NEAREST);

        osgUtil::IntersectionVisitor iv(picker);
        graph->accept(iv);

        bool good = false;
        if (picker->containsIntersections())
        {
            result = *picker->getIntersections().begin();
            return true;
        }
        else return false;
    }
}

bool
Terrain::getWorldCoordsUnderMouse(osg::View* view, float x, float y, osg::Vec3d& out_coords) const
{
    bool good = false;
    osgUtil::LineSegmentIntersector::Intersection result;
    if (_graph.valid() && intersectMouse(view, x, y, _graph.get(), result))
    {
        out_coords = result.getWorldIntersectPoint();
        good = true;
    }
    return good;
}

TerrainTile*
Terrain::getTerrainTileUnderMouse(osg::View* view, float x, float y) const
{
    TerrainTile* tile = nullptr;
    osgUtil::LineSegmentIntersector::Intersection result;
    if (_graph.valid() && intersectMouse(view, x, y, _graph.get(), result))
    {
        for (auto iter = result.nodePath.rbegin();
            iter != result.nodePath.rend() && tile == nullptr;
            ++iter)
        {
            tile = dynamic_cast<TerrainTile*>(*iter);
        }
    }
    return tile;
}

void
Terrain::addTerrainCallback( TerrainCallback* cb )
{
    if ( cb )
    {        
        removeTerrainCallback( cb );

        Threading::ScopedWriteLock exclusiveLock( _callbacksMutex );        
        _callbacks.push_back( cb );
        ++_callbacksSize; // atomic increment
    }
}

void
Terrain::removeTerrainCallback( TerrainCallback* cb )
{
    Threading::ScopedWriteLock exclusiveLock( _callbacksMutex );

    for( CallbackList::iterator i = _callbacks.begin(); i != _callbacks.end(); )
    {        
        if ( i->get() == cb )
        {
            i = _callbacks.erase( i );
            --_callbacksSize;
        }
        else
        {
            ++i;
        }
    }
}

void
Terrain::notifyTileUpdate( const TileKey& key, osg::Node* node )
{
    if ( !node )
    {
        OE_WARN << LC << "notify with a null node!" << std::endl;
    }

    if (_callbacksSize > 0)
    {
        if (!key.valid())
            OE_WARN << LC << "notifyTileUpdate with key = NULL\n";

        _updateQueue->add(new onTileUpdateOperation(key, node, this));
    }
}

void
Terrain::fireTileUpdate( const TileKey& key, osg::Node* node )
{
    Threading::ScopedReadLock sharedLock( _callbacksMutex );

    for( CallbackList::iterator i = _callbacks.begin(); i != _callbacks.end(); )
    {       
        TerrainCallbackContext context( this );
        i->get()->onTileUpdate( key, node, context );

        // if the callback set the "remove" flag, discard the callback.
        if ( context.markedForRemoval() )
            i = _callbacks.erase( i );
        else
            ++i;
    }
}

void
Terrain::notifyMapElevationChanged()
{
    if (_callbacksSize > 0)
    {
        onTileUpdateOperation* op = new onTileUpdateOperation(TileKey::INVALID, 0L, this);
        op->_delay = 1; // let the terrain update before applying this
        _updateQueue->add(op);
    }
}

void
Terrain::fireMapElevationChanged()
{
    // nop
}
void
Terrain::accept( osg::NodeVisitor& nv )
{
    _graph->accept( nv );
}
