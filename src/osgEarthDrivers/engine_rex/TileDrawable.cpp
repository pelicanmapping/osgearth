/* osgEarth
* Copyright 2008-2014 Pelican Mapping
* MIT License
*/
#include "TileDrawable"
#include "EngineContext"

#include <osgEarth/Registry>
#include <osgEarth/ImageUtils>
#include <osgEarth/Math>
#include <osg/KdTree>

using namespace osg;
using namespace osgEarth::REX;
using namespace osgEarth;

#define LC "[TileDrawable] "

//........................................................................

ModifyBoundingBoxCallback::ModifyBoundingBoxCallback(EngineContext* context) :
    _context(context)
{
    //nop
}

void
ModifyBoundingBoxCallback::operator()(const TileKey& key, osg::BoundingBox& bbox)
{
    osg::ref_ptr<TerrainEngineNode> engine = _context->getEngine();
    if (engine.valid())
    {
        engine->fireModifyTileBoundingBoxCallbacks(key, bbox);

        osg::ref_ptr<const Map> map = _context->getMap();
        if (map.valid())
        {
            LayerVector layers;
            map->getLayers(layers);

            for (LayerVector::const_iterator layer = layers.begin(); layer != layers.end(); ++layer)
            {
                if (layer->valid())
                {
                    layer->get()->modifyTileBoundingBox(key, bbox);
                }
            }
        }
    }
}

//........................................................................

TileDrawable::TileDrawable(
    const TileKey& key,
    SharedGeometry* geometry,
    int tileSize) :
    osg::Drawable(),
    _key(key),
    _geom(geometry),
    _tileSize(tileSize),
    _bboxRadius(1.0),
    _bboxCB(NULL)
{
    // builds the initial mesh.
    setElevationRaster(nullptr, osg::Matrixf::identity());
}

TileDrawable::~TileDrawable()
{
    //nop
}

void
TileDrawable::setElevationRaster(Texture::Ptr image, const osg::Matrixf& scaleBias)
{
    _elevationRaster = image;
    _elevationScaleBias = scaleBias;

    if (equivalent(0.0f, _elevationScaleBias(0,0)) ||
        equivalent(0.0f, _elevationScaleBias(1,1)))
    {
        OE_WARN << "("<<_key.str()<<") precision error\n";
    }

    const osg::Vec3Array& verts = *static_cast<osg::Vec3Array*>(_geom->getVertexArray());
    const osg::DrawElements* de = dynamic_cast<osg::DrawElements*>(_geom->getDrawElements());

    OE_SOFT_ASSERT_AND_RETURN(de != nullptr, void());

    if (_mesh.size() < verts.size())
    {
        _mesh.resize(verts.size());
    }

    if ( _elevationRaster)
    {
        const osg::Vec3Array& normals = *static_cast<osg::Vec3Array*>(_geom->getNormalArray());
        const osg::Vec3Array& units = *static_cast<osg::Vec3Array*>(_geom->getTexCoordArray());

        ImageUtils::PixelReader readElevation(_elevationRaster->osgTexture()->getImage(0));
        readElevation.setBilinear(true);
        osg::Vec4f sample;

        auto encoding = ElevationTile::encodingFor(_elevationRaster->internalFormat().value());
        //bool decode16bitHeight = _elevationRaster->minValue().isSet();
        float minh = _elevationRaster->minValue().value();
        float maxh = _elevationRaster->maxValue().value();
        bool decode16bitHeight = _elevationRaster->minValue().isSet() && minh <= maxh;

        float
            scaleU = _elevationScaleBias(0,0),
            scaleV = _elevationScaleBias(1,1),
            biasU  = _elevationScaleBias(3,0),
            biasV  = _elevationScaleBias(3,1);

        if ( osg::equivalent(scaleU, 0.0f) || osg::equivalent(scaleV, 0.0f) )
        {
            OE_WARN << LC << "Precision loss in tile " << _key.str() << "\n";
        }

        for (int i = 0; i < verts.size(); ++i)
        {
            if ( ((int)units[i].z() & VERTEX_HAS_ELEVATION) == 0)
            {
                sample = readElevation(
                    clamp(units[i].x()*scaleU + biasU, 0.0f, 1.0f),
                    clamp(units[i].y()*scaleV + biasV, 0.0f, 1.0f));

                auto h = ElevationTile::decodeElevation(sample, encoding, minh, maxh);
                _mesh[i] = verts[i] + normals[i] * h;
            }
            else
            {
                _mesh[i] = verts[i];
            }
        }
    }

    else
    {
        std::copy(verts.begin(), verts.end(), _mesh.begin());
    }


    // Make a temporary geometry to build kdtrees on and copy the shape over
    if (_geom->getDrawElements()->getMode() != GL_PATCHES)
    {
        osg::ref_ptr<osg::Geometry> tempGeom = new osg::Geometry;
        osg::Vec3Array* tempVerts = new osg::Vec3Array;
        tempVerts->reserve(_mesh.size());
        for (unsigned int i = 0; i < _mesh.size(); i++)
        {
            tempVerts->push_back(_mesh[i]);
        }
        tempGeom->setVertexArray(tempVerts);
        tempGeom->addPrimitiveSet(_geom->getDrawElements());

        osg::ref_ptr<osg::KdTreeBuilder> kdTreeBuilder = osgDB::Registry::instance()->getKdTreeBuilder();
        if (kdTreeBuilder)
        {            
            kdTreeBuilder = kdTreeBuilder->clone();
            tempGeom->accept(*kdTreeBuilder);
            if (tempGeom->getShape())
            {
                setShape(tempGeom->getShape());
            }
        }
    }

    dirtyBound();
}

// Functor supplies triangles to things like IntersectionVisitor, ComputeBoundsVisitor, etc.
void
TileDrawable::accept(osg::PrimitiveFunctor& f) const
{
    f.setVertexArray(_mesh.size(), _mesh.data());

    f.drawElements(
        GL_TRIANGLES,
        _geom->getDrawElements()->getNumIndices(),
        static_cast<const GLushort*>(_geom->getDrawElements()->getDataPointer()));
}

osg::BoundingSphere
TileDrawable::computeBound() const
{
    return osg::BoundingSphere(getBoundingBox());
}

osg::BoundingBox
TileDrawable::computeBoundingBox() const
{
    osg::BoundingBox box;

    // core bbox created from the mesh:
    for(auto& vert : _mesh)
    {
        box.expandBy(vert);
    }

    // finally see if any of the layers request a bbox change:
    if (_bboxCB)
    {
        (*_bboxCB)(_key, box);
    }

    _bboxRadius = box.radius();
    _bboxHalfWidth = 0.5 * (box.xMax() - box.xMin());
    _bboxHalfHeight = 0.5 * (box.yMax() - box.yMin());

    return box;
}

void TileDrawable::resizeGLObjectBuffers(unsigned maxsize)
{
    if (_geom)
    {
        _geom->resizeGLObjectBuffers(maxsize);
    }
}

void TileDrawable::releaseGLObjects(osg::State* state) const
{
    if (_geom)
    {
        _geom->releaseGLObjects(state);
    }
}
