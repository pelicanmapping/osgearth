/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include <osgEarth/InstanceBuilder>
#include <osgEarth/VirtualProgram>
#include <osgEarth/Shaders>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osg/VertexAttribDivisor>
#include <osg/Geometry>

using namespace osgEarth;

#define POSITION_ATTRIB 9
#define ROTATION_ATTRIB 10
#define SCALE_ATTRIB 11

// OSG in GLCORE mode doesn't set the vertex attribute divisor
// correctly, so here's a big old hack to get around that.
class InstancedGeometry : public osg::Geometry
{
public:
    InstancedGeometry();
    InstancedGeometry(
        const osg::Geometry& geometry,
        const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY);
    InstancedGeometry(const InstancedGeometry& geometry,const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY);

    META_Node(osgEarth, InstancedGeometry);

    void drawImplementation(osg::RenderInfo &renderInfo) const override;

    void setVertexAttribDivisor(unsigned int index, unsigned int divisor)
    {
        if (index >= _divisors.size())
        {
            _divisors.resize(index + 1);
        }
        _divisors[index] = divisor;
        dirtyGLObjects();
    }
    unsigned int getVertexAttribDivisor(unsigned int index)
    {
        if (index < _divisors.size())
        {
            return _divisors[index];
    }
        else
    {
            return 0;
        }
    }
protected:
    std::vector<unsigned int> _divisors;
};

namespace
{
    // Funky manoeuvres for calculating the bounding box
    struct InstanceBoundingBoxCallback : public osg::Geometry::ComputeBoundingBoxCallback
    {
        osg::BoundingBox computeBound(const osg::Drawable& drawable) const
        {
            const osg::Geometry* geom = static_cast<const osg::Geometry*>(&drawable);
            osg::BoundingBox initBB = geom->computeBoundingBox();
            const osg::Vec3Array* pos = dynamic_cast<const osg::Vec3Array*>(geom->getVertexAttribArray(POSITION_ATTRIB));
            const osg::Vec4Array* rot = dynamic_cast<const osg::Vec4Array*>(geom->getVertexAttribArray(ROTATION_ATTRIB));
            const osg::Vec3Array* scale = dynamic_cast<const osg::Vec3Array*>(geom->getVertexAttribArray(SCALE_ATTRIB));
            osg::BoundingBox resultBB;
            // XXX
            int numInstances = 0;
            if (geom->getNumPrimitiveSets() > 0)
            {
                numInstances = geom->getPrimitiveSet(0)->getNumInstances();
            }
            for (int i = 0; i < numInstances; ++i)
            {
                osg::Matrixf instanceTransform;
                if (pos)
                {
                    instanceTransform.makeTranslate((*pos)[i]);
                }
                if (rot)
                {
                    if (rot->getBinding() == osg::Array::BIND_PER_VERTEX)
                    {
                        instanceTransform = osg::Matrixf::rotate(osg::Quat((*rot)[i])) * instanceTransform;
                    }
                    else
                    {
                        instanceTransform = osg::Matrixf::rotate(osg::Quat((*rot)[0])) * instanceTransform;
                    }
                }
                if (scale)
                {
                    if (scale->getBinding() == osg::Array::BIND_PER_VERTEX)
                    {
                        instanceTransform = osg::Matrixf::scale((*scale)[i]) * instanceTransform;
                    }
                    else
                    {
                        instanceTransform = osg::Matrixf::scale((*scale)[0]) * instanceTransform;
                    }
                }
                for (int j = 0; j < 8; ++j)
                {
                    resultBB.expandBy(initBB.corner(j) * instanceTransform);
                }
                
            }
            return resultBB;
        }
    };

    void setPerVertexOrOverall(osg::Geometry*geom, osg::Array* perVertex, osg::Array* overall, unsigned int index)
    {
        if (perVertex)
        {
            geom->setVertexAttribArray(index, perVertex, osg::Array::BIND_PER_VERTEX);
            InstancedGeometry* instancedGeom = dynamic_cast<InstancedGeometry*>(geom);
            if (instancedGeom && instancedGeom->getUseVertexArrayObject())
            {
                instancedGeom->setVertexAttribDivisor(index, 1);
            }
            else
            {
                osg::StateSet* ss = geom->getOrCreateStateSet();
                ss->setAttribute(new osg::VertexAttribDivisor(index, 1));
            }
        }
        else
        {
            geom->setVertexAttribArray(index, overall, osg::Array::BIND_OVERALL);
        }
    }
}

InstancedGeometry::InstancedGeometry()
{
    setUseDisplayList(false);
    setUseVertexBufferObjects(true);
    setUseVertexArrayObject(
        osgEarth::Registry::capabilities().supportsVertexArrayObjects());
}

InstancedGeometry::InstancedGeometry(
    const osg::Geometry& geometry,
    const osg::CopyOp& copyop) :
    osg::Geometry(geometry, copyop)
{
    setUseDisplayList(false);
    setUseVertexBufferObjects(true);
    setUseVertexArrayObject(
        osgEarth::Registry::capabilities().supportsVertexArrayObjects());
}

InstancedGeometry::InstancedGeometry(const InstancedGeometry& geometry,const osg::CopyOp& copyop)
    : Geometry(geometry, copyop),
      _divisors(geometry._divisors.begin(), geometry._divisors.end())
{
    setUseDisplayList(false);
    setUseVertexBufferObjects(true);
    setUseVertexArrayObject(
        osgEarth::Registry::capabilities().supportsVertexArrayObjects());
}

void InstancedGeometry::drawImplementation(osg::RenderInfo &renderInfo) const
{
    osg::State &state = *renderInfo.getState();
    bool useVertexArrayObject = state.useVertexArrayObject(_useVertexArrayObject);
    if (useVertexArrayObject)
    {
        const osg::GLExtensions *extensions = state.get<osg::GLExtensions>();
        if (extensions->glVertexAttribDivisor)
        {
            for (unsigned int i = 0; i < _divisors.size(); ++i)
            {
                extensions->glVertexAttribDivisor(i, _divisors[i]);
            }
        }
    }

    Geometry::drawImplementation(renderInfo);
}

InstanceBuilder::InstanceBuilder()
{
    osg::Vec3 position(0.0f, 0.0f, 0.0f);
    osg::Vec4 rotation(0.0f, 0.0f, 0.0f, 1.0f);
    osg::Vec3 scale(1.0f, 1.0f, 1.0f);

    _position = new osg::Vec3Array(1,&position);
    _rotation = new osg::Vec4Array(1, &rotation);
    _scale = new osg::Vec3Array(1, &scale);
}

osg::Geometry* InstanceBuilder::createGeometry()
{
    return new InstancedGeometry;
}

osg::Geometry* InstanceBuilder::createGeometry(
    const osg::Geometry& source,
    const osg::CopyOp& copyop)
{
    return new InstancedGeometry(source, copyop);
}

void InstanceBuilder::installInstancing(osg::Geometry* geometry) const
{
    // XXX do something more clever
    int numInstances = _positions->getNumElements();
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    // assign the instance parameters
    setPerVertexOrOverall(geometry, _positions.get(), _position.get(), POSITION_ATTRIB);
    setPerVertexOrOverall(geometry, _rotations.get(), _rotation.get(), ROTATION_ATTRIB);
    setPerVertexOrOverall(geometry, _scales.get(), _scale.get(), SCALE_ATTRIB);
    osg::Geometry::PrimitiveSetList& prims = geometry->getPrimitiveSetList();
    for (osg::Geometry::PrimitiveSetList::iterator it = prims.begin(), end = prims.end();
         it != end;
         ++it)
    {
        (*it)->setNumInstances(numInstances);
    }
    VirtualProgram* vp = VirtualProgram::getOrCreate(ss);
    vp->setName("DrawInstancedAttribute");
    osgEarth::Shaders pkg;
    pkg.load(vp, pkg.DrawInstancedAttribute);
    vp->addBindAttribLocation("oe_DrawInstancedAttribute_position", POSITION_ATTRIB);
    vp->addBindAttribLocation("oe_DrawInstancedAttribute_rotation", ROTATION_ATTRIB);
    vp->addBindAttribLocation("oe_DrawInstancedAttribute_scale", SCALE_ATTRIB);
    geometry->setComputeBoundingBoxCallback(new InstanceBoundingBoxCallback);
}
