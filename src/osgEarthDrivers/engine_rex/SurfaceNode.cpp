/* osgEarth
* Copyright 2008-2014 Pelican Mapping
* MIT License
*/
#include "SurfaceNode"
#include "GeometryPool"
#include "TileDrawable"

#include <osgEarth/TileKey>
#include <osgEarth/Registry>
#include <osgEarth/Horizon>
#include <osgEarth/ImageUtils>
#include <osgEarth/LineDrawable>

#include <osg/CullStack>
#include <osg/Geode>

#include <osg/Geometry>
#include <osg/TriangleFunctor>
#include <osgText/Text>
#include <osg/CullStack>
#include <osgUtil/CullVisitor>

#include <numeric>

using namespace osgEarth::REX;
using namespace osgEarth;

#define LC "[SurfaceNode] "

//..............................................................

namespace
{    
    osg::Node* makeBBox(const osg::BoundingBox& bbox)
    {
        LineDrawable* lines = nullptr;

        if ( bbox.valid() )
        {
            static const int index[24] = {
                0,1, 1,3, 3,2, 2,0,
                0,4, 1,5, 2,6, 3,7,
                4,5, 5,7, 7,6, 6,4
            };

            lines = new LineDrawable(GL_LINES);
            for(int i=0; i<24; i+=2)
            {
                lines->pushVertex(bbox.corner(index[i]));
                lines->pushVertex(bbox.corner(index[i+1]));
            }
            lines->setColor(osg::Vec4(1,0,0,1));
            lines->finish();
        }

        return lines;
    }
}


const bool SurfaceNode::_enableDebugNodes = ::getenv("OSGEARTH_REX_DEBUG") != 0L;

SurfaceNode::SurfaceNode(const TileKey& tilekey, TileDrawable* drawable) :
    _drawable(drawable),
    _ellipsoid(tilekey.getProfile()->getSRS()->getEllipsoid())
{
    // Create the final node.
    addChild(_drawable);

    // Establish a local reference frame for the tile:
    GeoPoint centroid = tilekey.getExtent().getCentroid();
    osg::Matrix local2world;
    centroid.createLocalToWorld(local2world);
    setMatrix(local2world);

    // Initialize the cached bounding box.
    updateCullingData();
}

osg::BoundingSphere
SurfaceNode::computeBound() const
{
    osg::Matrix l2w;
    computeLocalToWorldMatrix(l2w, 0L);
    osg::BoundingSphere bs;
    osg::BoundingBox box = _drawable->getBoundingBox();
    for (unsigned i=0; i<8; ++i)
        bs.expandBy(box.corner(i)*l2w);

    return bs;
}

void
SurfaceNode::setElevationRaster(Texture::Ptr raster, const osg::Matrixf& scaleBias)
{
    if ( !_drawable.valid() )
        return;

    // communicate the raster to the drawable:
    _drawable->setElevationRaster( raster, scaleBias );

    updateCullingData();
}

void
SurfaceNode::updateCullingData()
{
    if (!_drawable.valid())
        return;

    // next compute the bounding box in local space:
    const osg::BoundingBox& box = _drawable->getBoundingBox();

    // Compute the medians of each potential child node:

    osg::Vec3 minZMedians[4];
    osg::Vec3 maxZMedians[4];

    minZMedians[0] = (box.corner(0)+box.corner(1))*0.5;
    minZMedians[1] = (box.corner(1)+box.corner(3))*0.5;
    minZMedians[2] = (box.corner(3)+box.corner(2))*0.5;
    minZMedians[3] = (box.corner(0)+box.corner(2))*0.5;
                                  
    maxZMedians[0] = (box.corner(4)+box.corner(5))*0.5;
    maxZMedians[1] = (box.corner(5)+box.corner(7))*0.5;
    maxZMedians[2] = (box.corner(7)+box.corner(6))*0.5;
    maxZMedians[3] = (box.corner(4)+box.corner(6))*0.5;
                                  
    // Child 0 corners
    _childrenCorners[0][0] =  box.corner(0);
    _childrenCorners[0][1] =  minZMedians[0];
    _childrenCorners[0][2] =  minZMedians[3];
    _childrenCorners[0][3] = (minZMedians[0]+minZMedians[2])*0.5;

    _childrenCorners[0][4] =  box.corner(4);
    _childrenCorners[0][5] =  maxZMedians[0];
    _childrenCorners[0][6] =  maxZMedians[3];
    _childrenCorners[0][7] = (maxZMedians[0]+maxZMedians[2])*0.5;

    // Child 1 corners
    _childrenCorners[1][0] =  minZMedians[0];
    _childrenCorners[1][1] =  box.corner(1);
    _childrenCorners[1][2] = (minZMedians[0]+minZMedians[2])*0.5;
    _childrenCorners[1][3] =  minZMedians[1];
                     
    _childrenCorners[1][4] =  maxZMedians[0];
    _childrenCorners[1][5] =  box.corner(5);
    _childrenCorners[1][6] = (maxZMedians[0]+maxZMedians[2])*0.5;
    _childrenCorners[1][7] =  maxZMedians[1];

    // Child 2 corners
    _childrenCorners[2][0] =  minZMedians[3];
    _childrenCorners[2][1] = (minZMedians[0]+minZMedians[2])*0.5;
    _childrenCorners[2][2] =  box.corner(2);
    _childrenCorners[2][3] =  minZMedians[2];
                     
    _childrenCorners[2][4] =  maxZMedians[3];
    _childrenCorners[2][5] = (maxZMedians[0]+maxZMedians[2])*0.5;
    _childrenCorners[2][6] =  box.corner(6);
    _childrenCorners[2][7] =  maxZMedians[2]; 

    // Child 3 corners
    _childrenCorners[3][0] = (minZMedians[0]+minZMedians[2])*0.5;
    _childrenCorners[3][1] =  minZMedians[1];
    _childrenCorners[3][2] =  minZMedians[2];
    _childrenCorners[3][3] =  box.corner(3);
                     
    _childrenCorners[3][4] = (maxZMedians[0]+maxZMedians[2])*0.5;
    _childrenCorners[3][5] =  maxZMedians[1];
    _childrenCorners[3][6] =  maxZMedians[2];
    _childrenCorners[3][7] =  box.corner(7);

    // Transform the child corners to world space    
    const osg::Matrix& local2world = getMatrix();
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 8; ++j)
        {
            auto& corner = _childrenCorners[i][j];
            corner = corner * local2world;
        }
    }

    // Visible bounding box?
    if( _enableDebugNodes )
    {
        removeDebugNode();
        addDebugNode(box);
    }

    // Update the horizon culling point. If this point is visible over the horizon,
    // that means the tile must pass cull.
    std::vector<osg::Vec3d> mesh_world;
    mesh_world.reserve(_drawable->_mesh.size());
    for(auto& p : _drawable->_mesh)
        mesh_world.push_back(p * local2world);

    auto& ellipsoid = 
    _horizonCullingPoint = _ellipsoid.calculateHorizonCullingPoint(mesh_world);
    _horizonCullingPointSet = (_horizonCullingPoint != osg::Vec3d());

    dirtyBound();
}

void
SurfaceNode::addDebugNode(const osg::BoundingBox& box)
{
    _debugNode = makeBBox(box);
    addChild( _debugNode.get() );
}

void
SurfaceNode::removeDebugNode(void)
{
    if ( _debugNode.valid() )
    {
        removeChild( _debugNode.get() );
    }
}
