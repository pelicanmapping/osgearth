/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include <osgEarth/InstanceBuilder>
#include <osgEarth/Registry>
#include <osgEarth/VirtualProgram>
#include <osgEarth/Shaders>
#include <osg/VertexAttribDivisor>
#include <osg/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace osgEarth;

#define POSITION_ATTRIB 9
#define ROTATION_ATTRIB 10
#define SCALE_ATTRIB 11
#define RANGE_ATTRIB 12

// OSG in GLCORE mode doesn't reliably carry VertexAttribDivisor state into a
// Geometry's VAO. Use the non-VAO VBO path and bracket each draw with explicit
// divisors so an instance transform cannot accidentally advance per vertex.
class InstancedGeometry : public osg::Geometry
{
public:
    InstancedGeometry();
    InstancedGeometry(const InstancedGeometry& geometry,const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY);
    InstancedGeometry(const osg::Geometry& geometry,const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY);

    META_Node(osgEarth, InstancedGeometry);
    
    void drawImplementation(osg::RenderInfo& renderInfo) const override;
    osg::BoundingBox computeBoundingBox() const override
    {
        return _instancedBoundingBox.valid() ?
            _instancedBoundingBox : Geometry::computeBoundingBox();
    }
    void setInstancedBoundingBox(const osg::BoundingBox& value)
    {
        _instancedBoundingBox = value;
        dirtyBound();
    }
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
    osg::BoundingBox _instancedBoundingBox;
};

namespace
{
    void setPerVertexOrOverall(osg::Geometry*geom, osg::Array* perVertex, osg::Array* overall, unsigned int index)
    {
        if (perVertex)
        {
            geom->setVertexAttribArray(index, perVertex, osg::Array::BIND_PER_VERTEX);
            InstancedGeometry* instancedGeom = dynamic_cast<InstancedGeometry*>(geom);
            if (instancedGeom)
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
    setUseVertexArrayObject(false);
}

InstancedGeometry::InstancedGeometry(const InstancedGeometry& geometry,const osg::CopyOp& copyop)
    : Geometry(geometry, copyop),
      _divisors(geometry._divisors.begin(), geometry._divisors.end()),
      _instancedBoundingBox(geometry._instancedBoundingBox)
{
    setUseVertexArrayObject(false);
}

InstancedGeometry::InstancedGeometry(const osg::Geometry& geometry,const osg::CopyOp& copyop)
    : Geometry(geometry, copyop)
{
    setUseVertexArrayObject(false);
}

void InstancedGeometry::drawImplementation(osg::RenderInfo& renderInfo) const
{
    osg::State& state = *renderInfo.getState();
    const osg::GLExtensions* extensions = state.get<osg::GLExtensions>();
    if (extensions->glVertexAttribDivisor)
    {
        for (unsigned i = 0u; i < _divisors.size(); ++i)
        {
            if (_divisors[i] != 0u)
                extensions->glVertexAttribDivisor(i, _divisors[i]);
        }
    }

    Geometry::drawImplementation(renderInfo);

    // Divisors live in GL vertex-array state. Reset them so the following
    // non-instanced drawable cannot inherit this geometry's instance cadence.
    if (extensions->glVertexAttribDivisor)
    {
        for (unsigned i = 0u; i < _divisors.size(); ++i)
        {
            if (_divisors[i] != 0u)
                extensions->glVertexAttribDivisor(i, 0u);
        }
    }
}

InstanceBuilder::InstanceBuilder() :
    _instanceVBO(new osg::VertexBufferObject())
{
    osg::Vec3 position(0.0f, 0.0f, 0.0f);
    osg::Vec4 rotation(0.0f, 0.0f, 0.0f, 1.0f);
    osg::Vec3 scale(1.0f, 1.0f, 1.0f);
    osg::Vec2 range(0.0f, std::numeric_limits<float>::max());

    _position = new osg::Vec3Array(1,&position);
    _rotation = new osg::Vec4Array(1, &rotation);
    _scale = new osg::Vec3Array(1, &scale);
    _range = new osg::Vec2Array(1, &range);
    _positionOffsetUniform = new osg::Uniform(
        "oe_DrawInstancedAttribute_positionOffset", _positionOffset);
    _positionScaleUniform = new osg::Uniform(
        "oe_DrawInstancedAttribute_positionScale", _positionScale);
    _scaleOffsetUniform = new osg::Uniform(
        "oe_DrawInstancedAttribute_scaleOffset", _scaleOffset);
    _scaleScaleUniform = new osg::Uniform(
        "oe_DrawInstancedAttribute_scaleScale", _scaleScale);
}

void InstanceBuilder::setPositions(osg::Vec3Array* positions)
{
    _instancedBoundingBox.init();
    _packedPositions = nullptr;
    _positionOffset.set(0.0f, 0.0f, 0.0f);
    _positionScale.set(1.0f, 1.0f, 1.0f);
    _positionOffsetUniform->set(_positionOffset);
    _positionScaleUniform->set(_positionScale);
    _positions = positions;
    if (positions && !positions->getVertexBufferObject())
        positions->setVertexBufferObject(_instanceVBO.get());
}

void InstanceBuilder::setRotations(osg::Vec4Array* rotations)
{
    _instancedBoundingBox.init();
    _packedRotations = nullptr;
    _rotations = rotations;
    if (rotations && !rotations->getVertexBufferObject())
        rotations->setVertexBufferObject(_instanceVBO.get());
}

void InstanceBuilder::setScales(osg::Vec3Array* scales)
{
    _instancedBoundingBox.init();
    _packedScales = nullptr;
    _scaleOffset.set(0.0f, 0.0f, 0.0f);
    _scaleScale.set(1.0f, 1.0f, 1.0f);
    _scaleOffsetUniform->set(_scaleOffset);
    _scaleScaleUniform->set(_scaleScale);
    _scales = scales;
    if (scales && !scales->getVertexBufferObject())
        scales->setVertexBufferObject(_instanceVBO.get());
}

bool InstanceBuilder::compressInstanceAttributes()
{
    if (!_positions.valid() || !_rotations.valid() || !_scales.valid() ||
        _positions->empty() ||
        _rotations->size() != _positions->size() ||
        _scales->size() != _positions->size())
    {
        return false;
    }

    auto quantization = [](const osg::Vec3Array& values, osg::Vec3f& offset, osg::Vec3f& step)
    {
        offset = values.front();
        osg::Vec3f maximum = values.front();
        for (const auto& value : values)
        {
            for (unsigned component = 0u; component < 3u; ++component)
            {
                offset[component] = std::min(offset[component], value[component]);
                maximum[component] = std::max(maximum[component], value[component]);
            }
        }
        step = (maximum - offset) * (1.0f / 65535.0f);
    };
    auto encode = [](float value, float offset, float step)
    {
        if (step <= 0.0f)
            return static_cast<unsigned short>(0u);
        const float quantized = std::round((value - offset) / step);
        return static_cast<unsigned short>(std::max(0.0f, std::min(65535.0f, quantized)));
    };

    quantization(*_positions, _positionOffset, _positionScale);
    quantization(*_scales, _scaleOffset, _scaleScale);
    _positionOffsetUniform->set(_positionOffset);
    _positionScaleUniform->set(_positionScale);
    _scaleOffsetUniform->set(_scaleOffset);
    _scaleScaleUniform->set(_scaleScale);

    const std::size_t count = _positions->size();
    _packedPositions = new osg::Vec3usArray();
    _packedRotations = new osg::Vec4sArray();
    _packedScales = new osg::Vec3usArray();
    _packedPositions->reserve(count);
    _packedRotations->reserve(count);
    _packedScales->reserve(count);
    _packedRotations->setNormalize(true);
    for (std::size_t i = 0u; i < count; ++i)
    {
        const osg::Vec3f& position = (*_positions)[i];
        _packedPositions->push_back(osg::Vec3us(
            encode(position.x(), _positionOffset.x(), _positionScale.x()),
            encode(position.y(), _positionOffset.y(), _positionScale.y()),
            encode(position.z(), _positionOffset.z(), _positionScale.z())));

        const osg::Vec4f& rotation = (*_rotations)[i];
        auto encodeRotation = [](float value)
        {
            const float quantized = std::round(
                std::max(-1.0f, std::min(1.0f, value)) * 32767.0f);
            return static_cast<short>(quantized);
        };
        _packedRotations->push_back(osg::Vec4s(
            encodeRotation(rotation.x()), encodeRotation(rotation.y()),
            encodeRotation(rotation.z()), encodeRotation(rotation.w())));

        const osg::Vec3f& scale = (*_scales)[i];
        _packedScales->push_back(osg::Vec3us(
            encode(scale.x(), _scaleOffset.x(), _scaleScale.x()),
            encode(scale.y(), _scaleOffset.y(), _scaleScale.y()),
            encode(scale.z(), _scaleOffset.z(), _scaleScale.z())));
    }

    _packedPositions->setVertexBufferObject(_instanceVBO.get());
    _packedRotations->setVertexBufferObject(_instanceVBO.get());
    _packedScales->setVertexBufferObject(_instanceVBO.get());
    _positions = nullptr;
    _rotations = nullptr;
    _scales = nullptr;
    _instancedBoundingBox.init();
    return true;
}

void InstanceBuilder::setBaseBoundingBox(const osg::BoundingBox& bounds) const
{
    _instancedBoundingBox.init();
    const std::size_t count = _packedPositions.valid() ?
        _packedPositions->size() : (_positions.valid() ? _positions->size() : 0u);
    if (!bounds.valid() || count == 0u)
        return;

    const osg::Vec3f center = bounds.center();
    const osg::Vec3f extent(
        0.5f * (bounds.xMax() - bounds.xMin()),
        0.5f * (bounds.yMax() - bounds.yMin()),
        0.5f * (bounds.zMax() - bounds.zMin()));

    const osg::Vec4f defaultRotation = (*_rotation)[0];
    const osg::Vec3f defaultScale = (*_scale)[0];
    for (std::size_t i = 0u; i < count; ++i)
    {
        osg::Vec3f position;
        if (_packedPositions.valid())
        {
            const osg::Vec3us& packed = (*_packedPositions)[i];
            position.set(
                _positionOffset.x() + packed.x() * _positionScale.x(),
                _positionOffset.y() + packed.y() * _positionScale.y(),
                _positionOffset.z() + packed.z() * _positionScale.z());
        }
        else
        {
            position = (*_positions)[i];
        }

        osg::Vec4f rotation = defaultRotation;
        if (_packedRotations.valid())
        {
            const osg::Vec4s& packed = (*_packedRotations)[i];
            rotation.set(
                packed.x() * (1.0f / 32767.0f),
                packed.y() * (1.0f / 32767.0f),
                packed.z() * (1.0f / 32767.0f),
                packed.w() * (1.0f / 32767.0f));
        }
        else if (_rotations.valid() && i < _rotations->size())
        {
            rotation = (*_rotations)[i];
        }

        osg::Vec3f scale = defaultScale;
        if (_packedScales.valid())
        {
            const osg::Vec3us& packed = (*_packedScales)[i];
            scale.set(
                _scaleOffset.x() + packed.x() * _scaleScale.x(),
                _scaleOffset.y() + packed.y() * _scaleScale.y(),
                _scaleOffset.z() + packed.z() * _scaleScale.z());
        }
        else if (_scales.valid() && i < _scales->size())
        {
            scale = (*_scales)[i];
        }
        const osg::Matrixf linear =
            osg::Matrixf::scale(scale) *
            osg::Matrixf::rotate(osg::Quat(rotation));
        const osg::Vec3f transformedCenter = center * linear + position;
        const osg::Vec3f transformedExtent(
            std::abs(linear(0, 0)) * extent.x() +
                std::abs(linear(1, 0)) * extent.y() +
                std::abs(linear(2, 0)) * extent.z(),
            std::abs(linear(0, 1)) * extent.x() +
                std::abs(linear(1, 1)) * extent.y() +
                std::abs(linear(2, 1)) * extent.z(),
            std::abs(linear(0, 2)) * extent.x() +
                std::abs(linear(1, 2)) * extent.y() +
                std::abs(linear(2, 2)) * extent.z());
        _instancedBoundingBox.expandBy(transformedCenter - transformedExtent);
        _instancedBoundingBox.expandBy(transformedCenter + transformedExtent);
    }
}

osg::Geometry* InstanceBuilder::createGeometry()
{
    return new InstancedGeometry;
}

osg::Geometry* InstanceBuilder::createGeometry(
    const osg::Geometry& geometry,
    const osg::CopyOp& copyop)
{
    return new InstancedGeometry(geometry, copyop);
}

void InstanceBuilder::installInstancing(osg::Geometry* geometry) const
{
    osg::Array* positionAttribute = _packedPositions.valid() ?
        static_cast<osg::Array*>(_packedPositions.get()) : _positions.get();
    osg::Array* rotationAttribute = _packedRotations.valid() ?
        static_cast<osg::Array*>(_packedRotations.get()) : _rotations.get();
    osg::Array* scaleAttribute = _packedScales.valid() ?
        static_cast<osg::Array*>(_packedScales.get()) : _scales.get();
    if (!geometry || !positionAttribute)
        return;

    int numInstances = positionAttribute->getNumElements();
    const osg::BoundingBox baseBounds = geometry->getBoundingBox();
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    // assign the instance parameters
    setPerVertexOrOverall(geometry, positionAttribute, _position.get(), POSITION_ATTRIB);
    setPerVertexOrOverall(geometry, rotationAttribute, _rotation.get(), ROTATION_ATTRIB);
    setPerVertexOrOverall(geometry, scaleAttribute, _scale.get(), SCALE_ATTRIB);
    setPerVertexOrOverall(geometry, nullptr, _range.get(), RANGE_ATTRIB);
    osg::Geometry::PrimitiveSetList& prims = geometry->getPrimitiveSetList();
    for (osg::Geometry::PrimitiveSetList::iterator it = prims.begin(), end = prims.end();
         it != end;
        ++it)
    {
        (*it)->setNumInstances(numInstances);
    }
    auto vp = Registry::instance()->getOrCreate<VirtualProgram>(
        "vp.InstanceBuilder.DrawInstancedAttribute", []()
        {
            VirtualProgram* result = new VirtualProgram();
            result->setInheritShaders(true);
            result->setName("DrawInstancedAttribute");
            osgEarth::Shaders pkg;
            pkg.load(result, pkg.DrawInstancedAttribute);
            result->addBindAttribLocation("oe_DrawInstancedAttribute_position", POSITION_ATTRIB);
            result->addBindAttribLocation("oe_DrawInstancedAttribute_rotation", ROTATION_ATTRIB);
            result->addBindAttribLocation("oe_DrawInstancedAttribute_scale", SCALE_ATTRIB);
            result->addBindAttribLocation("oe_DrawInstancedAttribute_range", RANGE_ATTRIB);
            return result;
        });
    ss->setAttribute(vp);
    ss->addUniform(_positionOffsetUniform.get());
    ss->addUniform(_positionScaleUniform.get());
    ss->addUniform(_scaleOffsetUniform.get());
    ss->addUniform(_scaleScaleUniform.get());

    if (!_instancedBoundingBox.valid())
        setBaseBoundingBox(baseBounds);
    if (auto* instanced = dynamic_cast<InstancedGeometry*>(geometry))
        instanced->setInstancedBoundingBox(_instancedBoundingBox);
    else
        geometry->setInitialBound(_instancedBoundingBox);

    // Prime Drawable's cached box and sphere now, on the construction/pager
    // thread. Cull traversal will only read these cached values.
    geometry->getBoundingBox();
}
