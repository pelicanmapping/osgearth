/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/IntersectionPicker>
#include <osgEarth/Math>
#include <osgEarth/Utils>
#include <osgEarth/Registry>

#define LC "[Picker] "

using namespace osgEarth;


IntersectionPicker::IntersectionPicker( osgViewer::View* view, osg::Node* root, unsigned travMask, float buffer, Limit limit ) :
_view    ( view ),
_root    ( root ),
_travMask( travMask ),
_buffer  ( buffer ),
_limit   ( limit )
{
    if ( root )
        _path = root->getParentalNodePaths()[0];
}

void
IntersectionPicker::setLimit(const IntersectionPicker::Limit& value)
{
    _limit = value;
}

void
IntersectionPicker::setTraversalMask(unsigned value)
{
    _travMask = value;
}

void
IntersectionPicker::setBuffer(float value)
{
    _buffer = value;
}

bool
IntersectionPicker::pick( float x, float y, Hits& results ) const
{
    float local_x = x, local_y = y;
    const osg::Camera* camera = Util::getCameraUnderMouse(_view, x, y, local_x, local_y);
    if ( !camera )
    {
        results.clear();
        return false;
    }

    double buffer_x = _buffer, buffer_y = _buffer;
    if ( camera->getViewport() )
    {
        double aspectRatio = camera->getViewport()->width()/camera->getViewport()->height();
        buffer_x *= aspectRatio;
        buffer_y /= aspectRatio;
    }

    // Build the pick segment in the coordinate frame where the traversal starts: the root's parent
    // frame, or the world when picking the camera's whole scene. Window z is 0..1 near to far; without
    // a viewport the mouse position is in clip space, where the near plane is at z=-1.
    osg::Matrix windowMatrix;
    double zNear = -1.0;
    if (camera->getViewport())
    {
        windowMatrix = camera->getViewport()->computeWindowMatrix();
        zNear = 0.0;
    }

    osg::Matrix localToWorld;
    if ( _root.valid() )
    {
        osg::NodePath prunedNodePath( _path.begin(), _path.end()-1 );
        localToWorld = osg::computeLocalToWorld(prunedNodePath);
    }

    osg::Matrix modelToWindow = localToWorld * camera->getViewMatrix() * camera->getProjectionMatrix() * windowMatrix;
    osg::Matrix windowToModel;
    windowToModel.invert(modelToWindow);

    osg::Vec3d startModel = osg::Vec3d(local_x, local_y, zNear) * windowToModel;
    osg::Vec3d endModel = osg::Vec3d(local_x, local_y, 1.0) * windowToModel;
    osg::Vec3d bufferModel = osg::Vec3d(local_x + buffer_x, local_y + buffer_y, zNear) * windowToModel;

    // The buffer only widens the test for points and lines (triangles are tested exactly), but it also
    // pads every bounding sphere the visitor tests. OSG node bounds are normally single precision, so in
    // geocentric coordinates a small object's bound can be off by about 0.5m and its radius can round to
    // zero; the minimum keeps such objects from being culled before their geometry is tested.
    double buffer = std::max((bufferModel - startModel).length(), 5.0);  //TODO: Setting a minimum of 5.0 may need revisited

    // Start a perspective pick at the eye, not the near plane. OSG clamps the computed near plane to
    // far * nearFarRatio (e.g. 0.0005 * 300km = 150m when looking toward the horizon), and the
    // logarithmic depth buffer still draws anything in front of it, so a segment that starts at the
    // near plane misses visible objects close to the camera.
    if (ProjectionMatrix::isPerspective(camera->getProjectionMatrix()))
    {
        startModel = camera->getInverseViewMatrix().getTrans() * osg::Matrix::inverse(localToWorld);
    }

    OE_DEBUG
        << "local_x:" << local_x << ", local_y:" << local_y
        << ", buffer_x:" << buffer_x << ", buffer_y:" << buffer_y
        << ", bm.x:" << bufferModel.x() << ", bm.y:" << bufferModel.y()
        << ", bm.z:" << bufferModel.z()
        << ", BUFFER: " << buffer
        << std::endl;

    osg::ref_ptr<osgEarth::PrimitiveIntersector> picker =
        new osgEarth::PrimitiveIntersector(osgUtil::Intersector::MODEL, startModel, endModel, buffer);

    picker->setIntersectionLimit( (osgUtil::Intersector::IntersectionLimit)_limit );
    osgUtil::IntersectionVisitor iv(picker.get());

    //picker->setIntersectionLimit( osgUtil::Intersector::LIMIT_ONE_PER_DRAWABLE );

    // in MODEL mode, we need to window and proj matrixes in order to support some of the 
    // features in osgEarth (like Annotation::GeoPositionNode).
    if ( _root.valid() )
    {
        iv.pushWindowMatrix( new osg::RefMatrix(windowMatrix) );
        iv.pushProjectionMatrix( new osg::RefMatrix(camera->getProjectionMatrix()) );
        iv.pushViewMatrix( new osg::RefMatrix(camera->getViewMatrix()) );
    }

    iv.setTraversalMask( _travMask );

    // Without a root, the camera pushes its own matrices and an identity model matrix, so the
    // segment is in world coordinates.
    if ( _root.valid() )
        _path.back()->accept(iv);
    else
        const_cast<osg::Camera*>(camera)->accept(iv);

    if (picker->containsIntersections())
    {
        results = picker->getIntersections();
        return true;
    }
    else
    {
        results.clear();
        return false;
    }
}

bool
IntersectionPicker::getObjectIDs(const Hits& results, std::set<ObjectID>& out_objectIDs) const
{
    ObjectIndex* index = Registry::objectIndex();

    for(Hits::const_iterator hit = results.begin(); hit != results.end(); ++hit)
    {
        bool found = false;

        // check for the uniform.
        const osg::NodePath& path = hit->nodePath;
        for(osg::NodePath::const_reverse_iterator n = path.rbegin(); n != path.rend(); ++n )
        {
            osg::Node* node = *n;
            if ( node && node->getStateSet() )
            {
                osg::Uniform* u = node->getStateSet()->getUniform( index->getObjectIDUniformName() );
                if ( u )
                {
                    ObjectID oid;
                    if ( u->get(oid) )
                    {
                        out_objectIDs.insert( oid );
                        found = true;
                    }
                }
            }
        }

        if ( !found )
        {
            // check the geometry.
            const osg::Geometry* geom = hit->drawable ? hit->drawable->asGeometry() : 0L;
            if ( geom )
            {
                const ObjectIDArray* ids = dynamic_cast<const ObjectIDArray*>( geom->getVertexAttribArray(index->getObjectIDAttribLocation()) );
                if ( ids )
                {
                    for(unsigned i=0; i < hit->indexList.size(); ++i)
                    {
                        unsigned index = hit->indexList[i];
                        if ( index < ids->size() )
                        {
                            ObjectID oid = (*ids)[index];
                            out_objectIDs.insert( oid );
                        }
                    }
                }
            }
        }
    }

    return !out_objectIDs.empty();
}
