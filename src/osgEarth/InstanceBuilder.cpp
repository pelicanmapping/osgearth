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
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>

using namespace osgEarth;

#define POSITION_ATTRIB 9
#define ROTATION_ATTRIB 10
#define SCALE_ATTRIB 11

namespace osgEarth
{
    struct PackedInstance
    {
        osg::Vec3us position;
        osg::Vec4s rotation;
        osg::Vec3us scale;
    };

    static_assert(sizeof(PackedInstance) == 20u,
        "Packed Kit instances must remain 20 bytes");
    static_assert(offsetof(PackedInstance, position) == 0u, "Unexpected position packing");
    static_assert(offsetof(PackedInstance, rotation) == 6u, "Unexpected rotation packing");
    static_assert(offsetof(PackedInstance, scale) == 14u, "Unexpected scale packing");

    class PackedInstanceBuffer : public osg::BufferData
    {
    public:
        PackedInstanceBuffer() = default;
        PackedInstanceBuffer(
            const PackedInstanceBuffer& rhs,
            const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY) :
            osg::BufferData(rhs, copyop),
            _instances(rhs._instances) { }

        osg::Object* cloneType() const override { return new PackedInstanceBuffer(); }
        osg::Object* clone(const osg::CopyOp& copyop) const override
        {
            return new PackedInstanceBuffer(*this, copyop);
        }
        bool isSameKindAs(const osg::Object* object) const override
        {
            return dynamic_cast<const PackedInstanceBuffer*>(object) != nullptr;
        }
        const char* libraryName() const override { return "osgEarth"; }
        const char* className() const override { return "PackedInstanceBuffer"; }

        const GLvoid* getDataPointer() const override
        {
            return _instances.empty() ? nullptr : _instances.data();
        }
        unsigned int getTotalDataSize() const override
        {
            return static_cast<unsigned int>(_instances.size() * sizeof(PackedInstance));
        }
        void reserve(std::size_t count) { _instances.reserve(count); }
        std::size_t size() const { return _instances.size(); }
        void append(const PackedInstance& value) { _instances.push_back(value); }
        const PackedInstance& operator[](std::size_t index) const { return _instances[index]; }

    protected:
        ~PackedInstanceBuffer() override = default;

    private:
        std::vector<PackedInstance> _instances;
    };
}

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
    void compileGLObjects(osg::RenderInfo& renderInfo) const override;
    void resizeGLObjectBuffers(unsigned int maxSize) override;
    void releaseGLObjects(osg::State* state = nullptr) const override;
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
    void setInstanceBuffer(
        osg::BufferData* buffer,
        std::size_t offset,
        std::size_t count)
    {
        _instanceBuffer = buffer;
        _instanceOffset = offset;
        _instanceCount = count;
        dirtyGLObjects();
    }
    const osg::BufferData* getInstanceBuffer() const { return _instanceBuffer.get(); }
    std::size_t getInstanceOffset() const { return _instanceOffset; }
    std::size_t getInstanceCount() const { return _instanceCount; }
protected:
    std::vector<unsigned int> _divisors;
    osg::BoundingBox _instancedBoundingBox;
    osg::ref_ptr<osg::BufferData> _instanceBuffer;
    std::size_t _instanceOffset = 0u;
    std::size_t _instanceCount = 0u;
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
      _instancedBoundingBox(geometry._instancedBoundingBox),
      _instanceBuffer(geometry._instanceBuffer),
      _instanceOffset(geometry._instanceOffset),
      _instanceCount(geometry._instanceCount)
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
    if (_instanceBuffer.valid() && _instanceCount > 0u)
    {
        osg::State& state = *renderInfo.getState();
        const osg::GLExtensions* extensions = state.get<osg::GLExtensions>();
        if (!extensions->glVertexAttribPointer ||
            !extensions->glEnableVertexAttribArray ||
            !extensions->glDisableVertexAttribArray ||
            !extensions->glVertexAttribDivisor)
        {
            return;
        }

        osg::GLBufferObject* glBuffer =
            _instanceBuffer->getOrCreateGLBufferObject(state.getContextID());
        if (!glBuffer)
            return;

        const bool usingVertexBufferObjects = state.useVertexBufferObject(
            _supportsVertexBufferObjects && _useVertexBufferObjects);
        osg::VertexArrayState* vas = state.getCurrentVertexArrayState();
        vas->setVertexBufferObjectSupported(usingVertexBufferObjects);

        // Set up only the immutable model arrays through osg::Geometry. Instance
        // transforms live in one interleaved city buffer and are installed below.
        drawVertexArraysImplementation(renderInfo);
        vas->bindVertexBufferObject(glBuffer);

        const std::uintptr_t base =
            static_cast<std::uintptr_t>(glBuffer->getOffset(_instanceBuffer->getBufferIndex())) +
            _instanceOffset * sizeof(osgEarth::PackedInstance);
        const GLsizei stride = static_cast<GLsizei>(sizeof(osgEarth::PackedInstance));
        auto pointer = [base](std::size_t offset)
        {
            return reinterpret_cast<const GLvoid*>(base + offset);
        };

        extensions->glEnableVertexAttribArray(POSITION_ATTRIB);
        extensions->glVertexAttribPointer(
            POSITION_ATTRIB, 3, GL_UNSIGNED_SHORT, GL_FALSE, stride,
            pointer(offsetof(osgEarth::PackedInstance, position)));
        extensions->glVertexAttribDivisor(POSITION_ATTRIB, 1u);

        extensions->glEnableVertexAttribArray(ROTATION_ATTRIB);
        extensions->glVertexAttribPointer(
            ROTATION_ATTRIB, 4, GL_SHORT, GL_TRUE, stride,
            pointer(offsetof(osgEarth::PackedInstance, rotation)));
        extensions->glVertexAttribDivisor(ROTATION_ATTRIB, 1u);

        extensions->glEnableVertexAttribArray(SCALE_ATTRIB);
        extensions->glVertexAttribPointer(
            SCALE_ATTRIB, 3, GL_UNSIGNED_SHORT, GL_FALSE, stride,
            pointer(offsetof(osgEarth::PackedInstance, scale)));
        extensions->glVertexAttribDivisor(SCALE_ATTRIB, 1u);

        drawPrimitivesImplementation(renderInfo);

        for (unsigned attribute = POSITION_ATTRIB; attribute <= SCALE_ATTRIB; ++attribute)
        {
            extensions->glVertexAttribDivisor(attribute, 0u);
            extensions->glDisableVertexAttribArray(attribute);
        }
        vas->unbindVertexBufferObject();
        vas->unbindElementBufferObject();
        return;
    }

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

void InstancedGeometry::compileGLObjects(osg::RenderInfo& renderInfo) const
{
    Geometry::compileGLObjects(renderInfo);
    if (!_instanceBuffer.valid())
        return;

    osg::State& state = *renderInfo.getState();
    osg::GLBufferObject* glBuffer =
        _instanceBuffer->getOrCreateGLBufferObject(state.getContextID());
    if (glBuffer && glBuffer->isDirty())
        glBuffer->compileBuffer();
    state.get<osg::GLExtensions>()->glBindBuffer(GL_ARRAY_BUFFER_ARB, 0u);
}

void InstancedGeometry::resizeGLObjectBuffers(unsigned int maxSize)
{
    Geometry::resizeGLObjectBuffers(maxSize);
    if (_instanceBuffer.valid())
        _instanceBuffer->resizeGLObjectBuffers(maxSize);
}

void InstancedGeometry::releaseGLObjects(osg::State* state) const
{
    Geometry::releaseGLObjects(state);
    if (_instanceBuffer.valid())
        _instanceBuffer->releaseGLObjects(state);
}

InstanceBuilder::InstanceBuilder() :
    _instanceVBO(new osg::VertexBufferObject()),
    _instanceBuffer(new osgEarth::PackedInstanceBuffer())
{
    osg::Vec3 position(0.0f, 0.0f, 0.0f);
    osg::Vec4 rotation(0.0f, 0.0f, 0.0f, 1.0f);
    osg::Vec3 scale(1.0f, 1.0f, 1.0f);

    _position = new osg::Vec3Array(1,&position);
    _rotation = new osg::Vec4Array(1, &rotation);
    _scale = new osg::Vec3Array(1, &scale);
    _instanceBuffer->setBufferObject(_instanceVBO.get());
}

void InstanceBuilder::setPositions(osg::Vec3Array* positions)
{
    _instancedBoundingBox.init();
    _instanceOffset = 0u;
    _instanceCount = 0u;
    _positionOffset.set(0.0f, 0.0f, 0.0f);
    _positionScale.set(1.0f, 1.0f, 1.0f);
    resetBatchUniforms();
    _positions = positions;
    if (positions && !positions->getVertexBufferObject())
        positions->setVertexBufferObject(_instanceVBO.get());
}

void InstanceBuilder::setRotations(osg::Vec4Array* rotations)
{
    _instancedBoundingBox.init();
    _instanceOffset = 0u;
    _instanceCount = 0u;
    resetBatchUniforms();
    _rotations = rotations;
    if (rotations && !rotations->getVertexBufferObject())
        rotations->setVertexBufferObject(_instanceVBO.get());
}

void InstanceBuilder::setScales(osg::Vec3Array* scales)
{
    _instancedBoundingBox.init();
    _instanceOffset = 0u;
    _instanceCount = 0u;
    _scaleOffset.set(0.0f, 0.0f, 0.0f);
    _scaleScale.set(1.0f, 1.0f, 1.0f);
    resetBatchUniforms();
    _scales = scales;
    if (scales && !scales->getVertexBufferObject())
        scales->setVertexBufferObject(_instanceVBO.get());
}

void InstanceBuilder::setTints(osg::Vec3Array* tints)
{
    _instanceOffset = 0u;
    _instanceCount = 0u;
    _tints = tints;
}

void InstanceBuilder::setRange(const osg::Vec2f& range)
{
    _range = range;
    resetBatchUniforms();
}

void InstanceBuilder::reserveInstances(std::size_t count)
{
    static_cast<osgEarth::PackedInstanceBuffer*>(_instanceBuffer.get())->reserve(count);
}

void InstanceBuilder::resetBatchUniforms()
{
    _positionOffsetUniform = nullptr;
    _positionScaleUniform = nullptr;
    _scaleOffsetUniform = nullptr;
    _scaleScaleUniform = nullptr;
    _packedScaleTintUniform = nullptr;
    _rangeUniform = nullptr;
}

void InstanceBuilder::createBatchUniforms() const
{
    if (_positionOffsetUniform.valid())
        return;

    _positionOffsetUniform = new osg::Uniform(
        "oe_DrawInstancedAttribute_positionOffset", _positionOffset);
    _positionScaleUniform = new osg::Uniform(
        "oe_DrawInstancedAttribute_positionScale", _positionScale);
    _scaleOffsetUniform = new osg::Uniform(
        "oe_DrawInstancedAttribute_scaleOffset", _scaleOffset);
    _scaleScaleUniform = new osg::Uniform(
        "oe_DrawInstancedAttribute_scaleScale", _scaleScale);
    _packedScaleTintUniform = new osg::Uniform(
        "oe_DrawInstancedAttribute_packedScaleTint", _instanceCount > 0u);
    _rangeUniform = new osg::Uniform(
        "oe_DrawInstancedAttribute_range", _range);
}

bool InstanceBuilder::compressInstanceAttributes()
{
    if (!_positions.valid() || !_rotations.valid() || !_scales.valid() ||
        _positions->empty() ||
        _rotations->size() != _positions->size() ||
        _scales->size() != _positions->size() ||
        (_tints.valid() && _tints->size() != _positions->size()))
    {
        return false;
    }

    auto quantization = [](
        const osg::Vec3Array& values,
        osg::Vec3f& offset,
        osg::Vec3f& step,
        const osg::Vec3f& levels)
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
        for (unsigned component = 0u; component < 3u; ++component)
            step[component] = (maximum[component] - offset[component]) / levels[component];
    };
    auto encode = [](float value, float offset, float step, float maximum)
    {
        if (step <= 0.0f)
            return static_cast<unsigned short>(0u);
        const float quantized = std::round((value - offset) / step);
        return static_cast<unsigned short>(std::max(0.0f, std::min(maximum, quantized)));
    };
    auto encodeTint = [](float value, unsigned maximum)
    {
        if (!std::isfinite(value))
            value = 1.0f;
        const float quantized = std::round(
            std::max(0.0f, std::min(1.0f, value)) * static_cast<float>(maximum));
        return static_cast<unsigned short>(quantized);
    };

    quantization(
        *_positions, _positionOffset, _positionScale,
        osg::Vec3f(65535.0f, 65535.0f, 65535.0f));
    // The low scale bits carry RGB565 without growing the 20-byte record:
    // X = 11-bit scale + 5-bit red, Y = 10-bit scale + 6-bit green,
    // Z = 11-bit scale + 5-bit blue.
    quantization(
        *_scales, _scaleOffset, _scaleScale,
        osg::Vec3f(2047.0f, 1023.0f, 2047.0f));

    const std::size_t count = _positions->size();
    osgEarth::PackedInstanceBuffer* buffer =
        static_cast<osgEarth::PackedInstanceBuffer*>(_instanceBuffer.get());
    _instanceOffset = buffer->size();
    _instanceCount = count;
    for (std::size_t i = 0u; i < count; ++i)
    {
        const osg::Vec3f& position = (*_positions)[i];
        osgEarth::PackedInstance packed;
        packed.position.set(
            encode(position.x(), _positionOffset.x(), _positionScale.x(), 65535.0f),
            encode(position.y(), _positionOffset.y(), _positionScale.y(), 65535.0f),
            encode(position.z(), _positionOffset.z(), _positionScale.z(), 65535.0f));

        const osg::Vec4f& rotation = (*_rotations)[i];
        auto encodeRotation = [](float value)
        {
            const float quantized = std::round(
                std::max(-1.0f, std::min(1.0f, value)) * 32767.0f);
            return static_cast<short>(quantized);
        };
        packed.rotation.set(
            encodeRotation(rotation.x()), encodeRotation(rotation.y()),
            encodeRotation(rotation.z()), encodeRotation(rotation.w()));

        const osg::Vec3f& scale = (*_scales)[i];
        const osg::Vec3f tint = _tints.valid() ?
            (*_tints)[i] : osg::Vec3f(1.0f, 1.0f, 1.0f);
        packed.scale.set(
            static_cast<unsigned short>(
                (encode(scale.x(), _scaleOffset.x(), _scaleScale.x(), 2047.0f) << 5u) |
                encodeTint(tint.x(), 31u)),
            static_cast<unsigned short>(
                (encode(scale.y(), _scaleOffset.y(), _scaleScale.y(), 1023.0f) << 6u) |
                encodeTint(tint.y(), 63u)),
            static_cast<unsigned short>(
                (encode(scale.z(), _scaleOffset.z(), _scaleScale.z(), 2047.0f) << 5u) |
                encodeTint(tint.z(), 31u)));
        buffer->append(packed);
    }
    buffer->dirty();

    // The compact interleaved buffer supersedes these temporary float arrays.
    // Detach them even if their caller retains a reference so the city VBO
    // contains only the packed records that resident geometry will draw.
    if (_positions->getVertexBufferObject() == _instanceVBO.get())
        _positions->setVertexBufferObject(nullptr);
    if (_rotations->getVertexBufferObject() == _instanceVBO.get())
        _rotations->setVertexBufferObject(nullptr);
    if (_scales->getVertexBufferObject() == _instanceVBO.get())
        _scales->setVertexBufferObject(nullptr);
    if (_tints.valid() && _tints->getVertexBufferObject() == _instanceVBO.get())
        _tints->setVertexBufferObject(nullptr);
    _positions = nullptr;
    _rotations = nullptr;
    _scales = nullptr;
    _tints = nullptr;
    _instancedBoundingBox.init();
    return true;
}

void InstanceBuilder::setBaseBoundingBox(const osg::BoundingBox& bounds) const
{
    _instancedBoundingBox.init();
    const std::size_t count = _instanceCount > 0u ?
        _instanceCount : (_positions.valid() ? _positions->size() : 0u);
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
        const osgEarth::PackedInstance* packed = nullptr;
        if (_instanceCount > 0u)
        {
            packed = &(*static_cast<const osgEarth::PackedInstanceBuffer*>(
                _instanceBuffer.get()))[_instanceOffset + i];
            position.set(
                _positionOffset.x() + packed->position.x() * _positionScale.x(),
                _positionOffset.y() + packed->position.y() * _positionScale.y(),
                _positionOffset.z() + packed->position.z() * _positionScale.z());
        }
        else
        {
            position = (*_positions)[i];
        }

        osg::Vec4f rotation = defaultRotation;
        if (packed)
        {
            rotation.set(
                packed->rotation.x() * (1.0f / 32767.0f),
                packed->rotation.y() * (1.0f / 32767.0f),
                packed->rotation.z() * (1.0f / 32767.0f),
                packed->rotation.w() * (1.0f / 32767.0f));
        }
        else if (_rotations.valid() && i < _rotations->size())
        {
            rotation = (*_rotations)[i];
        }

        osg::Vec3f scale = defaultScale;
        if (packed)
        {
            scale.set(
                _scaleOffset.x() + (packed->scale.x() >> 5u) * _scaleScale.x(),
                _scaleOffset.y() + (packed->scale.y() >> 6u) * _scaleScale.y(),
                _scaleOffset.z() + (packed->scale.z() >> 5u) * _scaleScale.z());
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

const osg::BufferData* InstanceBuilder::getInstanceBuffer(const osg::Geometry* geometry)
{
    const InstancedGeometry* instanced =
        dynamic_cast<const InstancedGeometry*>(geometry);
    return instanced ? instanced->getInstanceBuffer() : nullptr;
}

std::size_t InstanceBuilder::getInstanceOffset(const osg::Geometry* geometry)
{
    const InstancedGeometry* instanced =
        dynamic_cast<const InstancedGeometry*>(geometry);
    return instanced ? instanced->getInstanceOffset() : 0u;
}

std::size_t InstanceBuilder::getInstanceCount(const osg::Geometry* geometry)
{
    const InstancedGeometry* instanced =
        dynamic_cast<const InstancedGeometry*>(geometry);
    return instanced ? instanced->getInstanceCount() : 0u;
}

void InstanceBuilder::installInstancing(osg::Geometry* geometry) const
{
    installInstancing(geometry, nullptr);
}

void InstanceBuilder::installInstancing(
    osg::Geometry* geometry,
    osg::StateSet* shaderStateSet) const
{
    if (!geometry || (_instanceCount == 0u && !_positions.valid()))
        return;

    InstancedGeometry* instanced = dynamic_cast<InstancedGeometry*>(geometry);
    if (_instanceCount > 0u && !instanced)
        return;

    const int numInstances = static_cast<int>(
        _instanceCount > 0u ? _instanceCount : _positions->size());
    const osg::BoundingBox baseBounds = geometry->getBoundingBox();
    osg::StateSet* geometryStateSet = geometry->getOrCreateStateSet();
    if (!shaderStateSet)
        shaderStateSet = geometryStateSet;

    if (_instanceCount > 0u)
    {
        instanced->setInstanceBuffer(
            _instanceBuffer.get(), _instanceOffset, _instanceCount);
    }
    else
    {
        // Retain the original non-interleaved path for callers that do not ask
        // InstanceBuilder to compact their data.
        setPerVertexOrOverall(geometry, _positions.get(), _position.get(), POSITION_ATTRIB);
        setPerVertexOrOverall(geometry, _rotations.get(), _rotation.get(), ROTATION_ATTRIB);
        setPerVertexOrOverall(geometry, _scales.get(), _scale.get(), SCALE_ATTRIB);
    }
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
            return result;
        });
    shaderStateSet->setAttribute(vp);
    createBatchUniforms();
    geometryStateSet->addUniform(_positionOffsetUniform.get());
    geometryStateSet->addUniform(_positionScaleUniform.get());
    geometryStateSet->addUniform(_scaleOffsetUniform.get());
    geometryStateSet->addUniform(_scaleScaleUniform.get());
    geometryStateSet->addUniform(_packedScaleTintUniform.get());
    geometryStateSet->addUniform(_rangeUniform.get());

    if (!_instancedBoundingBox.valid())
        setBaseBoundingBox(baseBounds);
    if (instanced)
        instanced->setInstancedBoundingBox(_instancedBoundingBox);
    else
        geometry->setInitialBound(_instancedBoundingBox);

    // Prime Drawable's cached box and sphere now, on the construction/pager
    // thread. Cull traversal will only read these cached values.
    geometry->getBoundingBox();
}
