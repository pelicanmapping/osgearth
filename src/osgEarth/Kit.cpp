/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/Kit>
#include <osgEarth/Config>
#include <osgEarth/GLUtils>
#include <osgEarth/InstanceBuilder>
#include <osgEarth/Notify>
#include <osgEarth/Registry>
#include <osgEarth/Shaders>
#include <osgEarth/URI>
#include <osgEarth/VirtualProgram>

#include <osg/BufferObject>
#include <osg/Camera>
#include <osg/ComputeBoundsVisitor>
#include <osg/Geode>
#include <osg/GLExtensions>
#include <osg/MatrixTransform>
#include <osg/PrimitiveSet>
#include <osg/RenderInfo>
#include <osg/VertexArrayState>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgUtil/Optimizer>
#include <osgUtil/CullVisitor>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>

using namespace osgEarth;

#define LC "[Kit] "

namespace
{
    struct BatchKey
    {
        std::string model;
        float minRange = 0.0f;
        float maxRange = std::numeric_limits<float>::max();
        int chunkX = 0;
        int chunkY = 0;

        bool operator<(const BatchKey& rhs) const
        {
            return std::tie(model, minRange, maxRange, chunkX, chunkY) <
                std::tie(rhs.model, rhs.minRange, rhs.maxRange, rhs.chunkX, rhs.chunkY);
        }
    };

    struct InstanceArrays
    {
        InstanceArrays() :
            positions(new osg::Vec3Array()),
            rotations(new osg::Vec4Array()),
            scales(new osg::Vec3Array()) { }

        void append(
            const osg::Vec3f& position,
            const osg::Quat& rotation,
            const osg::Vec3f& scale)
        {
            positions->push_back(position);
            rotations->push_back(osg::Vec4f(
                static_cast<float>(rotation.x()),
                static_cast<float>(rotation.y()),
                static_cast<float>(rotation.z()),
                static_cast<float>(rotation.w())));
            scales->push_back(scale);
        }

        void append(
            const std::vector<osg::Vec3f>& sourcePositions,
            const osg::Quat& rotation,
            const osg::Vec3f& scale)
        {
            const std::size_t count = sourcePositions.size();
            positions->reserve(positions->size() + count);
            rotations->reserve(rotations->size() + count);
            scales->reserve(scales->size() + count);
            positions->insert(positions->end(), sourcePositions.begin(), sourcePositions.end());
            rotations->insert(rotations->end(), count, osg::Vec4f(
                static_cast<float>(rotation.x()),
                static_cast<float>(rotation.y()),
                static_cast<float>(rotation.z()),
                static_cast<float>(rotation.w())));
            scales->insert(scales->end(), count, scale);
        }

        osg::ref_ptr<osg::Vec3Array> positions;
        osg::ref_ptr<osg::Vec4Array> rotations;
        osg::ref_ptr<osg::Vec3Array> scales;
    };

    using InstanceMap = std::map<BatchKey, InstanceArrays>;

    bool validRange(float minRange, float maxRange)
    {
        return std::isfinite(minRange) && std::isfinite(maxRange) &&
            minRange >= 0.0f && maxRange > minRange;
    }

    bool isAlwaysVisible(float minRange, float maxRange)
    {
        return minRange <= 0.0f &&
            maxRange >= std::numeric_limits<float>::max() * 0.5f;
    }

    KitNode::Instance normalizeInstanceRange(KitNode::Instance value)
    {
        if (!validRange(value.minRange, value.maxRange))
        {
            OE_WARN << LC << "Invalid instance range on model '" << value.model
                << "'; using always-visible range" << std::endl;
            value.minRange = 0.0f;
            value.maxRange = std::numeric_limits<float>::max();
        }
        return value;
    }

    osg::Matrixd compose(const KitNode::Instance& instance)
    {
        return
            osg::Matrixd::scale(instance.scale) *
            osg::Matrixd::rotate(instance.rotation) *
            osg::Matrixd::translate(instance.position);
    }

    class GatherInstances : public osg::NodeVisitor
    {
    public:
        GatherInstances() :
            osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            setNodeMaskOverride(~0u);
        }

        explicit GatherInstances(float chunkSize) :
            osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
            _chunkSize(chunkSize)
        {
            setNodeMaskOverride(~0u);
        }

        BatchKey keyFor(
            const std::string& model,
            float minRange,
            float maxRange,
            const osg::Vec3f& position) const
        {
            BatchKey key = { model, minRange, maxRange };
            if (_chunkSize > 0.0f)
            {
                key.chunkX = static_cast<int>(std::floor(position.x() / _chunkSize));
                key.chunkY = static_cast<int>(std::floor(position.y() / _chunkSize));
            }
            return key;
        }

        InstanceArrays& destination(
            const std::string& model,
            float minRange,
            float maxRange,
            const osg::Vec3f& position)
        {
            return instances[keyFor(model, minRange, maxRange, position)];
        }

        void append(const KitNode::Instance& instance)
        {
            destination(
                instance.model,
                instance.minRange,
                instance.maxRange,
                instance.position).append(
                    instance.position, instance.rotation, instance.scale);
        }

        static std::uint64_t chunkCode(int x, int y)
        {
            return
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32u) |
                static_cast<std::uint32_t>(y);
        }

        void appendCompactBatch(
            const KitNode::InstanceBatch& batch,
            const osg::Matrixd& parent)
        {
            if (!parent.isIdentity())
            {
                for (const auto& position : batch.positions)
                {
                    KitNode::Instance flattened(
                        batch.model, position, batch.rotation, batch.scale,
                        batch.minRange, batch.maxRange);
                    const osg::Matrixd matrix = compose(flattened) * parent;

                    osg::Vec3d translation;
                    osg::Vec3d scale;
                    osg::Quat rotation;
                    osg::Quat scaleOrientation;
                    matrix.decompose(translation, rotation, scale, scaleOrientation);
                    flattened.position.set(
                        static_cast<float>(translation.x()),
                        static_cast<float>(translation.y()),
                        static_cast<float>(translation.z()));
                    flattened.rotation = rotation;
                    flattened.scale.set(
                        static_cast<float>(scale.x()),
                        static_cast<float>(scale.y()),
                        static_cast<float>(scale.z()));
                    append(flattened);
                }
                return;
            }

            if (batch.positions.empty())
                return;

            if (_chunkSize <= 0.0f)
            {
                destination(
                    batch.model, batch.minRange, batch.maxRange,
                    batch.positions.front()).append(
                        batch.positions, batch.rotation, batch.scale);
                return;
            }

            // A binary source batch has one model/rotation/scale/range tuple.
            // Cache its small set of spatial destinations so the hot loop only
            // performs integer-key lookups instead of copying and comparing the
            // model string for every instance.
            std::unordered_map<std::uint64_t, InstanceArrays*> destinations;
            destinations.reserve(32u);
            for (const auto& position : batch.positions)
            {
                const int chunkX = static_cast<int>(std::floor(position.x() / _chunkSize));
                const int chunkY = static_cast<int>(std::floor(position.y() / _chunkSize));
                const std::uint64_t code = chunkCode(chunkX, chunkY);
                auto found = destinations.find(code);
                InstanceArrays* arrays = nullptr;
                if (found == destinations.end())
                {
                    arrays = &destination(
                        batch.model, batch.minRange, batch.maxRange, position);
                    destinations.emplace(code, arrays);
                }
                else
                {
                    arrays = found->second;
                }
                arrays->append(position, batch.rotation, batch.scale);
            }
        }

        void apply(osg::Group& group) override
        {
            KitNode* kitNode = dynamic_cast<KitNode*>(&group);
            if (kitNode)
            {
                const osg::Matrixd parent = osg::computeLocalToWorld(getNodePath());
                const auto& compactBatches = kitNode->getInstanceBatches();
                if (!compactBatches.empty())
                {
                    for (const auto& batch : compactBatches)
                        appendCompactBatch(batch, parent);
                }
                else if (parent.isIdentity())
                {
                    for (const auto& instance : kitNode->getInstances())
                        append(instance);
                }
                else
                {
                    for (const auto& instance : kitNode->getInstances())
                    {
                        KitNode::Instance flattened = instance;
                        const osg::Matrixd matrix = compose(instance) * parent;

                        osg::Vec3d translation;
                        osg::Vec3d scale;
                        osg::Quat rotation;
                        osg::Quat scaleOrientation;
                        matrix.decompose(translation, rotation, scale, scaleOrientation);

                        flattened.position.set(
                            static_cast<float>(translation.x()),
                            static_cast<float>(translation.y()),
                            static_cast<float>(translation.z()));
                        flattened.rotation = rotation;
                        flattened.scale.set(
                            static_cast<float>(scale.x()),
                            static_cast<float>(scale.y()),
                            static_cast<float>(scale.z()));

                        append(flattened);
                    }
                }
            }
            traverse(group);
        }

        InstanceMap instances;

    private:
        float _chunkSize = 0.0f;
    };

    constexpr unsigned KIT_POSITION_ATTRIB = 9u;
    constexpr unsigned KIT_ROTATION_ATTRIB = 10u;
    constexpr unsigned KIT_SCALE_ATTRIB = 11u;
    constexpr unsigned KIT_BATCH_SSBO_BINDING = 28u;
    constexpr std::size_t PACKED_INSTANCE_SIZE = 20u;
    constexpr std::size_t INITIAL_VISIBLE_BATCH_CAPACITY = 128u;
    constexpr std::size_t INSTANCE_RING_SLOTS = 3u;

#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif

    // Some OSG build configurations deliberately avoid exporting the GLsync
    // typedef after declaring GLExtensions. Recover the opaque handle type
    // from the extension function signature instead of depending on GL 3.2
    // header visibility here.
    using GLSyncHandle = decltype(
        std::declval<decltype(std::declval<osg::GLExtensions>().glFenceSync)>()(
            std::declval<GLenum>(), std::declval<GLbitfield>()));

    // This is byte-for-byte compatible with InstanceBuilder's compressed
    // record. The user-provided no-op constructor is intentional: arena growth
    // must expose writable storage without first zeroing records that allocate()
    // immediately overwrites with a contiguous memcpy.
    struct StagedInstance
    {
        std::uint16_t position[3];
        std::int16_t rotation[4];
        std::uint16_t scale[3];

        StagedInstance() noexcept { }
    };

    static_assert(sizeof(StagedInstance) == PACKED_INSTANCE_SIZE,
        "Unexpected collected Kit instance layout");
    static_assert(offsetof(StagedInstance, position) == 0u, "Unexpected position offset");
    static_assert(offsetof(StagedInstance, rotation) == 6u, "Unexpected rotation offset");
    static_assert(offsetof(StagedInstance, scale) == 14u, "Unexpected scale offset");

    struct BatchDescriptor
    {
        osg::Vec4f values[12];
    };

    static_assert(sizeof(BatchDescriptor) == sizeof(float) * 4u * 12u,
        "Kit batch descriptors must match the std430 vec4 array");

    template<typename Value>
    class VectorBufferData : public osg::BufferData
    {
    public:
        VectorBufferData() = default;
        VectorBufferData(const VectorBufferData& rhs, const osg::CopyOp& copyop) :
            osg::BufferData(rhs, copyop), values(rhs.values) { }

        osg::Object* cloneType() const override { return new VectorBufferData(); }
        osg::Object* clone(const osg::CopyOp& copyop) const override
        {
            return new VectorBufferData(*this, copyop);
        }
        bool isSameKindAs(const osg::Object* object) const override
        {
            return dynamic_cast<const VectorBufferData*>(object) != nullptr;
        }
        const char* libraryName() const override { return "osgEarth"; }
        const char* className() const override { return "KitVectorBufferData"; }
        const GLvoid* getDataPointer() const override
        {
            return values.empty() ? nullptr : values.data();
        }
        unsigned getTotalDataSize() const override
        {
            return static_cast<unsigned>(values.size() * sizeof(Value));
        }

        std::vector<Value> values;

    protected:
        ~VectorBufferData() override = default;
    };

    using StagedInstanceBuffer = VectorBufferData<StagedInstance>;
    using BatchDescriptorBuffer = VectorBufferData<BatchDescriptor>;

    struct DrawSpan
    {
        std::uint32_t firstInstance = 0u;
        std::uint32_t instanceCount = 0u;
    };

    // Match the OpenGL Draw*IndirectCommand layouts exactly. The no-op
    // constructors avoid clearing command storage when a frame grows it; every
    // member is overwritten before upload.
    struct DrawArraysCommand
    {
        std::uint32_t count;
        std::uint32_t instanceCount;
        std::uint32_t first;
        std::uint32_t baseInstance;

        DrawArraysCommand() noexcept { }
    };

    struct DrawElementsCommand
    {
        std::uint32_t count;
        std::uint32_t instanceCount;
        std::uint32_t firstIndex;
        std::uint32_t baseVertex;
        std::uint32_t baseInstance;

        DrawElementsCommand() noexcept { }
    };

    static_assert(sizeof(DrawArraysCommand) == 16u,
        "Unexpected DrawArraysIndirect command layout");
    static_assert(sizeof(DrawElementsCommand) == 20u,
        "Unexpected DrawElementsIndirect command layout");

    using DrawArraysCommandBuffer = VectorBufferData<DrawArraysCommand>;
    using DrawElementsCommandBuffer = VectorBufferData<DrawElementsCommand>;

    struct CompactBatch
    {
        osg::ref_ptr<osg::BufferData> buffer;
        std::size_t offset = 0u;
        std::size_t count = 0u;
        osg::Vec3f positionOffset;
        osg::Vec3f positionScale;
        osg::Vec3f scaleOffset;
        osg::Vec3f scaleScale;
        osg::Vec2f range;
        osg::BoundingBox bounds;
    };

    class ModelCollector;

    class CollectedGeometry : public osg::Geometry
    {
    public:
        CollectedGeometry()
        {
            setUseDisplayList(false);
            setUseVertexBufferObjects(true);
            setUseVertexArrayObject(false);
            setCullingActive(false);
            initializeCommandBuffers();
        }

        CollectedGeometry(const osg::Geometry& rhs, ModelCollector* collector) :
            osg::Geometry(rhs, osg::CopyOp::SHALLOW_COPY),
            _collector(collector)
        {
            setUseDisplayList(false);
            setUseVertexBufferObjects(true);
            setUseVertexArrayObject(false);
            setCullingActive(false);
            for (auto& primitive : getPrimitiveSetList())
                primitive->setNumInstances(0);
            initializeCommandBuffers();
        }

        CollectedGeometry(
            const CollectedGeometry& rhs,
            const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY) :
            osg::Geometry(rhs, copyop),
            _collector(rhs._collector)
        {
            setUseVertexArrayObject(false);
            initializeCommandBuffers();
        }

        META_Node(osgEarth, CollectedGeometry);

        void drawImplementation(osg::RenderInfo& renderInfo) const override;
        void releaseGLObjects(osg::State* state) const override;

        // The persistent renderer is at world origin and shader-places every
        // instance. Its CPU-side model box would otherwise pollute near/far
        // computation at the center of the earth.
        osg::BoundingBox computeBoundingBox() const override
        {
            return osg::BoundingBox();
        }

    private:
        void initializeCommandBuffers()
        {
            _arrayCommands = new DrawArraysCommandBuffer();
            osg::ref_ptr<osg::DrawIndirectBufferObject> arrays =
                new osg::DrawIndirectBufferObject();
            arrays->setUsage(GL_STREAM_DRAW);
            _arrayCommands->setBufferObject(arrays.get());
            _arrayCommands->values.reserve(INITIAL_VISIBLE_BATCH_CAPACITY);

            _elementCommands = new DrawElementsCommandBuffer();
            osg::ref_ptr<osg::DrawIndirectBufferObject> elements =
                new osg::DrawIndirectBufferObject();
            elements->setUsage(GL_STREAM_DRAW);
            _elementCommands->setBufferObject(elements.get());
            _elementCommands->values.reserve(INITIAL_VISIBLE_BATCH_CAPACITY);
        }

        osg::ref_ptr<ModelCollector> _collector;
        mutable osg::ref_ptr<DrawArraysCommandBuffer> _arrayCommands;
        mutable osg::ref_ptr<DrawElementsCommandBuffer> _elementCommands;
    };

    class InstanceBudget : public osg::Referenced
    {
    public:
        std::size_t reserve(
            const osg::Camera* camera,
            std::uint64_t frameNumber,
            std::size_t requested)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            Frame& frame = _frames[camera];
            if (frame.frameNumber != frameNumber)
            {
                frame.frameNumber = frameNumber;
                frame.submitted = 0u;
                frame.dropped = 0u;
            }

            const std::size_t room = _maximum == 0u ? requested :
                (frame.submitted < _maximum ? _maximum - frame.submitted : 0u);
            const std::size_t accepted = std::min(requested, room);
            frame.submitted += accepted;
            frame.dropped += requested - accepted;
            if (accepted != requested && !_warned)
            {
                OE_WARN << LC << "Visible instance budget " << _maximum
                    << " reached; dropping instances. Increase the Kit budget or use 0 "
                    << "for an uncapped transient ring." << std::endl;
                _warned = true;
            }
            return accepted;
        }

        void setMaximum(std::size_t value)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _maximum = value;
            _warned = false;
        }

        std::size_t getMaximum() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return _maximum;
        }

        std::size_t getDropped(const osg::Camera* camera) const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            const auto found = _frames.find(camera);
            return found == _frames.end() ? 0u : found->second.dropped;
        }

        std::size_t getDropped() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            std::size_t result = 0u;
            for (const auto& entry : _frames)
                result += entry.second.dropped;
            return result;
        }

    private:
        struct Frame
        {
            std::uint64_t frameNumber = std::numeric_limits<std::uint64_t>::max();
            std::size_t submitted = 0u;
            std::size_t dropped = 0u;
        };

        mutable std::mutex _mutex;
        //! Zero means dynamically sized and uncapped.
        std::size_t _maximum = 0u;
        bool _warned = false;
        std::unordered_map<const osg::Camera*, Frame> _frames;
    };

    class ModelCollector : public osg::Referenced
    {
    public:
        struct SourceSpan
        {
            osg::ref_ptr<osg::BufferData> buffer;
            std::size_t offset = 0u;
            std::uint32_t count = 0u;
        };

        struct FrameData
        {
            FrameData() :
                descriptors(new BatchDescriptorBuffer())
            {
                osg::ref_ptr<osg::ShaderStorageBufferObject> ssbo =
                    new osg::ShaderStorageBufferObject();
                ssbo->setUsage(GL_STREAM_DRAW);
                descriptors->setBufferObject(ssbo.get());
                descriptors->values.reserve(INITIAL_VISIBLE_BATCH_CAPACITY);
                spans.reserve(INITIAL_VISIBLE_BATCH_CAPACITY);
                sources.reserve(INITIAL_VISIBLE_BATCH_CAPACITY);
            }

            void reset(std::uint64_t value)
            {
                frameNumber = value;
                descriptors->values.clear();
                spans.clear();
                sources.clear();
                instanceCount = 0u;
                dirty = false;
            }

            std::uint64_t frameNumber = std::numeric_limits<std::uint64_t>::max();
            osg::ref_ptr<BatchDescriptorBuffer> descriptors;
            std::vector<DrawSpan> spans;
            std::vector<SourceSpan> sources;
            std::size_t instanceCount = 0u;
            bool dirty = false;
        };

        struct CameraFrames
        {
            FrameData& acquire(std::uint64_t frameNumber)
            {
                std::unique_ptr<FrameData>& slot =
                    slots[frameNumber % INSTANCE_RING_SLOTS];
                if (!slot)
                    slot = std::make_unique<FrameData>();
                if (slot->frameNumber != frameNumber)
                    slot->reset(frameNumber);
                latestFrameNumber = frameNumber;
                return *slot;
            }

            FrameData* find(std::uint64_t frameNumber) const
            {
                const std::unique_ptr<FrameData>& slot =
                    slots[frameNumber % INSTANCE_RING_SLOTS];
                return slot && slot->frameNumber == frameNumber ? slot.get() : nullptr;
            }

            FrameData* latest() const
            {
                return latestFrameNumber == std::numeric_limits<std::uint64_t>::max() ?
                    nullptr : find(latestFrameNumber);
            }

            std::array<std::unique_ptr<FrameData>, INSTANCE_RING_SLOTS> slots;
            std::uint64_t latestFrameNumber =
                std::numeric_limits<std::uint64_t>::max();
        };

        explicit ModelCollector(
            const osg::BoundingBox& bounds,
            InstanceBudget* budget) :
            _modelBounds(bounds),
            _budget(budget)
        {
        }

        void submit(
            const CompactBatch& batch,
            const osg::Matrixd& localToView,
            const osg::Camera* camera,
            std::uint64_t frameNumber)
        {
            if (!batch.buffer.valid() || batch.count == 0u ||
                !batch.buffer->getDataPointer() ||
                batch.count > std::numeric_limits<std::uint32_t>::max())
            {
                return;
            }

            const std::size_t availableRecords =
                batch.buffer->getTotalDataSize() / PACKED_INSTANCE_SIZE;
            if (batch.offset > availableRecords ||
                batch.count > availableRecords - batch.offset)
            {
                return;
            }

            const std::size_t accepted = _budget.valid() ?
                _budget->reserve(camera, frameNumber, batch.count) : batch.count;
            if (accepted == 0u)
                return;

            std::lock_guard<std::mutex> lock(_mutex);
            for (auto i = _frames.begin(); i != _frames.end(); )
            {
                if (i->first != camera &&
                    frameNumber > i->second.latestFrameNumber + INSTANCE_RING_SLOTS)
                {
                    i = _frames.erase(i);
                }
                else
                {
                    ++i;
                }
            }

            FrameData& frame = _frames[camera].acquire(frameNumber);
            if (frame.descriptors->values.size() >=
                static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()) ||
                frame.instanceCount >
                    std::numeric_limits<std::uint32_t>::max() - accepted)
            {
                return;
            }

            BatchDescriptor descriptor;
            descriptor.values[0].set(
                batch.positionOffset.x(), batch.positionOffset.y(), batch.positionOffset.z(),
                batch.range.x());
            descriptor.values[1].set(
                batch.positionScale.x(), batch.positionScale.y(), batch.positionScale.z(),
                batch.range.y());
            descriptor.values[2].set(
                batch.scaleOffset.x(), batch.scaleOffset.y(), batch.scaleOffset.z(), 0.0f);
            descriptor.values[3].set(
                batch.scaleScale.x(), batch.scaleScale.y(), batch.scaleScale.z(), 0.0f);

            descriptor.values[4].set(
                static_cast<float>(localToView(0, 0)),
                static_cast<float>(localToView(0, 1)),
                static_cast<float>(localToView(0, 2)), 0.0f);
            descriptor.values[5].set(
                static_cast<float>(localToView(1, 0)),
                static_cast<float>(localToView(1, 1)),
                static_cast<float>(localToView(1, 2)), 0.0f);
            descriptor.values[6].set(
                static_cast<float>(localToView(2, 0)),
                static_cast<float>(localToView(2, 1)),
                static_cast<float>(localToView(2, 2)), 0.0f);

            const osg::Vec3f translationHigh(
                static_cast<float>(localToView(3, 0)),
                static_cast<float>(localToView(3, 1)),
                static_cast<float>(localToView(3, 2)));
            descriptor.values[7].set(
                translationHigh.x(), translationHigh.y(), translationHigh.z(), 0.0f);
            descriptor.values[8].set(
                static_cast<float>(localToView(3, 0) -
                    static_cast<double>(translationHigh.x())),
                static_cast<float>(localToView(3, 1) -
                    static_cast<double>(translationHigh.y())),
                static_cast<float>(localToView(3, 2) -
                    static_cast<double>(translationHigh.z())),
                0.0f);

            osg::Matrixd inverse;
            if (!inverse.invert(localToView))
                inverse.makeIdentity();
            descriptor.values[9].set(
                static_cast<float>(inverse(0, 0)),
                static_cast<float>(inverse(1, 0)),
                static_cast<float>(inverse(2, 0)), 0.0f);
            descriptor.values[10].set(
                static_cast<float>(inverse(0, 1)),
                static_cast<float>(inverse(1, 1)),
                static_cast<float>(inverse(2, 1)), 0.0f);
            descriptor.values[11].set(
                static_cast<float>(inverse(0, 2)),
                static_cast<float>(inverse(1, 2)),
                static_cast<float>(inverse(2, 2)), 0.0f);

            frame.descriptors->values.push_back(descriptor);
            frame.spans.push_back({
                static_cast<std::uint32_t>(frame.instanceCount),
                static_cast<std::uint32_t>(accepted) });
            frame.sources.push_back({
                batch.buffer, batch.offset,
                static_cast<std::uint32_t>(accepted) });
            frame.instanceCount += accepted;
            frame.dirty = true;
        }

        const osg::BoundingBox& getModelBounds() const { return _modelBounds; }
        void setDrawableCount(unsigned value) { _drawables = value; }
        unsigned getDrawableCount() const { return _drawables; }

        std::size_t getCollectedInstanceCount(const osg::Camera* camera) const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            const auto found = _frames.find(camera);
            if (found == _frames.end())
                return 0u;
            const FrameData* frame = found->second.latest();
            return frame ? frame->instanceCount : 0u;
        }

        std::size_t getCollectedInstanceCount() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            std::size_t result = 0u;
            for (const auto& entry : _frames)
            {
                const FrameData* frame = entry.second.latest();
                if (frame)
                    result += frame->instanceCount;
            }
            return result;
        }

        void getStats(
            const osg::Camera* camera,
            std::uint64_t frameNumber,
            Kit::ModelStats& output) const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            const auto found = _frames.find(camera);
            const FrameData* frame = found == _frames.end() ?
                nullptr : found->second.find(frameNumber);
            output.instances = frame ? frame->instanceCount : 0u;
            output.batches = frame ? frame->spans.size() : 0u;
            output.visibleBytes = output.instances * PACKED_INSTANCE_SIZE;
            output.drawables = _drawables;
            output.ringBytes = 0u;
            for (const auto& entry : _contextRings)
            {
                output.ringBytes += entry.second->capacity *
                    INSTANCE_RING_SLOTS * PACKED_INSTANCE_SIZE;
            }
            output.ringStalls = _ringStalls;
        }

        std::size_t getRingBytes() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            std::size_t result = 0u;
            for (const auto& entry : _contextRings)
                result += entry.second->capacity * INSTANCE_RING_SLOTS * PACKED_INSTANCE_SIZE;
            return result;
        }

        std::uint64_t getRingStalls() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return _ringStalls;
        }

        void releaseGLObjects(osg::State* state) const
        {
            if (!state)
                return;
            std::lock_guard<std::mutex> lock(_mutex);
            const auto found = _contextRings.find(state->getContextID());
            if (found == _contextRings.end())
                return;
            releaseRing(*found->second, state->get<osg::GLExtensions>(), false);
            _contextRings.erase(found);
        }

    private:
        struct ContextRing
        {
            GLBuffer::Ptr buffer;
            StagedInstance* mapped = nullptr;
            std::vector<StagedInstance> scratch;
            std::array<GLSyncHandle, INSTANCE_RING_SLOTS> fences = {};
            std::size_t capacity = 0u;
            std::size_t currentSlot = 0u;
            const osg::Camera* uploadedCamera = nullptr;
            std::uint64_t uploadedFrame = std::numeric_limits<std::uint64_t>::max();
            bool persistent = false;
        };

        bool waitAndDeleteFence(
            GLSyncHandle& fence,
            const osg::GLExtensions* extensions,
            bool countStall) const
        {
            if (!fence)
                return true;
            GLenum status = extensions->glClientWaitSync(fence, 0u, 0u);
            if (status == GL_TIMEOUT_EXPIRED)
            {
                if (countStall)
                    ++_ringStalls;
                status = extensions->glClientWaitSync(
                    fence, GL_SYNC_FLUSH_COMMANDS_BIT,
                    std::numeric_limits<GLuint64>::max());
            }
            extensions->glDeleteSync(fence);
            fence = nullptr;
            return status != GL_WAIT_FAILED;
        }

        void releaseRing(
            ContextRing& ring,
            const osg::GLExtensions* extensions,
            bool wait) const
        {
            for (GLSyncHandle& fence : ring.fences)
            {
                if (!fence)
                    continue;
                if (wait)
                    waitAndDeleteFence(fence, extensions, false);
                else
                {
                    extensions->glDeleteSync(fence);
                    fence = nullptr;
                }
            }
            if (ring.mapped && ring.buffer)
            {
                ring.buffer->bind();
                ring.buffer->unmap();
                ring.buffer->unbind();
            }
            ring.mapped = nullptr;
            ring.buffer.reset();
            ring.scratch.clear();
            ring.capacity = 0u;
            ring.uploadedCamera = nullptr;
            ring.uploadedFrame = std::numeric_limits<std::uint64_t>::max();
            ring.persistent = false;
        }

        bool ensureRingCapacity(
            ContextRing& ring,
            std::size_t required,
            osg::State& state,
            const osg::GLExtensions* extensions) const
        {
            if (ring.buffer && required <= ring.capacity)
                return true;

            const std::size_t maximumCapacity =
                static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()) /
                (INSTANCE_RING_SLOTS * PACKED_INSTANCE_SIZE);
            if (required == 0u || required > maximumCapacity)
                return false;

            std::size_t capacity = ring.capacity == 0u ? 65536u :
                ring.capacity + ring.capacity / 2u;
            capacity = std::max(capacity, required);
            capacity = std::min(capacity, maximumCapacity);
            releaseRing(ring, extensions, true);

            const GLsizei bytes = static_cast<GLsizei>(
                capacity * INSTANCE_RING_SLOTS * PACKED_INSTANCE_SIZE);
            ring.buffer = GLBuffer::create(GL_ARRAY_BUFFER_ARB, state);
            ring.buffer->bind();

            if (extensions->glBufferStorage && extensions->glMapBufferRange)
            {
                const GLbitfield flags = GL_MAP_WRITE_BIT |
                    GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
                ring.buffer->bufferStorage(bytes, nullptr, flags);
                ring.mapped = static_cast<StagedInstance*>(
                    ring.buffer->mapRange(0, bytes, flags));
                ring.persistent = ring.mapped != nullptr;
            }

            ring.buffer->unbind();
            if (!ring.persistent)
            {
                ring.buffer.reset();
                ring.buffer = GLBuffer::create(GL_ARRAY_BUFFER_ARB, state);
                ring.buffer->bind();
                ring.buffer->bufferData(bytes, nullptr, GL_STREAM_DRAW);
                ring.buffer->unbind();
            }

            ring.scratch.reserve(capacity);
            ring.capacity = capacity;
            return ring.buffer != nullptr;
        }

        bool uploadVisibleInstances(
            ContextRing& ring,
            const FrameData& frame,
            const osg::Camera* camera,
            std::uint64_t frameNumber,
            osg::State& state,
            const osg::GLExtensions* extensions,
            std::uint32_t& baseInstance) const
        {
            if (ring.uploadedCamera == camera &&
                ring.uploadedFrame == frameNumber && ring.buffer)
            {
                baseInstance = static_cast<std::uint32_t>(
                    ring.currentSlot * ring.capacity);
                return true;
            }
            if (!ensureRingCapacity(ring, frame.instanceCount, state, extensions))
                return false;

            const std::size_t slot = (ring.currentSlot + 1u) % INSTANCE_RING_SLOTS;
            if (!waitAndDeleteFence(ring.fences[slot], extensions, true))
                return false;

            const std::size_t slotFirst = slot * ring.capacity;
            std::size_t cursor = 0u;
            // Persistent coherent mappings are commonly write-combined. Many
            // small writes into them are dramatically slower than one linear
            // transfer, so first gather the tile-owned spans into reusable
            // ordinary RAM and then write the mapped slot exactly once.
            ring.scratch.resize(frame.instanceCount);

            for (const SourceSpan& source : frame.sources)
            {
                const unsigned char* sourceBytes =
                    static_cast<const unsigned char*>(source.buffer->getDataPointer()) +
                    source.offset * PACKED_INSTANCE_SIZE;
                std::memcpy(
                    ring.scratch.data() + cursor, sourceBytes,
                    static_cast<std::size_t>(source.count) * PACKED_INSTANCE_SIZE);
                cursor += source.count;
            }
            if (cursor != frame.instanceCount)
                return false;

            if (ring.persistent)
            {
                std::memcpy(
                    ring.mapped + slotFirst, ring.scratch.data(),
                    frame.instanceCount * PACKED_INSTANCE_SIZE);
            }
            else
            {
                ring.buffer->bind();
                ring.buffer->bufferSubData(
                    static_cast<GLintptr>(slotFirst * PACKED_INSTANCE_SIZE),
                    static_cast<GLsizei>(frame.instanceCount * PACKED_INSTANCE_SIZE),
                    ring.scratch.data());
                ring.buffer->unbind();
            }

            ring.currentSlot = slot;
            ring.uploadedCamera = camera;
            ring.uploadedFrame = frameNumber;
            baseInstance = static_cast<std::uint32_t>(slotFirst);
            return true;
        }

        void fenceCurrentSlot(
            ContextRing& ring,
            const osg::GLExtensions* extensions) const
        {
            GLSyncHandle& fence = ring.fences[ring.currentSlot];
            if (fence)
                extensions->glDeleteSync(fence);
            fence = extensions->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0u);
        }

        friend class CollectedGeometry;
        osg::BoundingBox _modelBounds;
        unsigned _drawables = 0u;
        osg::ref_ptr<InstanceBudget> _budget;
        mutable std::mutex _mutex;
        mutable std::unordered_map<const osg::Camera*, CameraFrames> _frames;
        mutable std::unordered_map<unsigned, std::unique_ptr<ContextRing>> _contextRings;
        mutable std::uint64_t _ringStalls = 0u;
    };

    void CollectedGeometry::drawImplementation(osg::RenderInfo& renderInfo) const
    {
        if (!_collector.valid())
            return;

        osg::State& state = *renderInfo.getState();
        const osg::GLExtensions* extensions = state.get<osg::GLExtensions>();
        if (!extensions->glVertexAttribPointer ||
            !extensions->glEnableVertexAttribArray ||
            !extensions->glDisableVertexAttribArray ||
            !extensions->glVertexAttribDivisor ||
            !extensions->glBindBuffer ||
            !extensions->glBindBufferBase ||
            !extensions->glBufferSubData ||
            !extensions->glFenceSync ||
            !extensions->glDeleteSync ||
            !extensions->glClientWaitSync ||
            !extensions->glMultiDrawArraysIndirect ||
            !extensions->glMultiDrawElementsIndirect)
        {
            return;
        }

        const osg::Camera* camera = renderInfo.getCurrentCamera();
        const osg::FrameStamp* stamp = state.getFrameStamp();
        const std::uint64_t frameNumber = stamp ? stamp->getFrameNumber() : 0u;
        std::lock_guard<std::mutex> lock(_collector->_mutex);
        auto found = _collector->_frames.find(camera);
        ModelCollector::FrameData* framePtr = found == _collector->_frames.end() ?
            nullptr : found->second.find(frameNumber);
        if (!framePtr || framePtr->spans.empty() ||
            framePtr->descriptors->values.size() != framePtr->spans.size() ||
            framePtr->sources.size() != framePtr->spans.size())
        {
            return;
        }

        ModelCollector::FrameData& frame = *framePtr;
        if (frame.dirty)
        {
            frame.descriptors->dirty();
            frame.dirty = false;
        }

        std::unique_ptr<ModelCollector::ContextRing>& ringPtr =
            _collector->_contextRings[state.getContextID()];
        if (!ringPtr)
            ringPtr = std::make_unique<ModelCollector::ContextRing>();
        ModelCollector::ContextRing& ring = *ringPtr;
        std::uint32_t ringBaseInstance = 0u;
        if (!_collector->uploadVisibleInstances(
                ring, frame, camera, frameNumber, state, extensions,
                ringBaseInstance))
        {
            return;
        }

        osg::GLBufferObject* descriptorGL =
            frame.descriptors->getOrCreateGLBufferObject(state.getContextID());
        if (!ring.buffer || !descriptorGL)
            return;
        if (descriptorGL->isDirty())
            descriptorGL->compileBuffer();

        extensions->glBindBufferBase(
            GL_SHADER_STORAGE_BUFFER, KIT_BATCH_SSBO_BINDING,
            descriptorGL->getGLObjectID());

        const bool usingVertexBufferObjects = state.useVertexBufferObject(
            _supportsVertexBufferObjects && _useVertexBufferObjects);
        osg::VertexArrayState* vas = state.getCurrentVertexArrayState();
        vas->setVertexBufferObjectSupported(usingVertexBufferObjects);
        drawVertexArraysImplementation(renderInfo);
        extensions->glBindBuffer(GL_ARRAY_BUFFER_ARB, ring.buffer->name());

        const std::uintptr_t base = 0u;
        const GLsizei stride = static_cast<GLsizei>(sizeof(StagedInstance));
        auto pointer = [base](std::size_t offset)
        {
            return reinterpret_cast<const GLvoid*>(base + offset);
        };

        extensions->glEnableVertexAttribArray(KIT_POSITION_ATTRIB);
        extensions->glVertexAttribPointer(
            KIT_POSITION_ATTRIB, 3, GL_UNSIGNED_SHORT, GL_FALSE, stride,
            pointer(offsetof(StagedInstance, position)));
        extensions->glVertexAttribDivisor(KIT_POSITION_ATTRIB, 1u);

        extensions->glEnableVertexAttribArray(KIT_ROTATION_ATTRIB);
        extensions->glVertexAttribPointer(
            KIT_ROTATION_ATTRIB, 4, GL_SHORT, GL_TRUE, stride,
            pointer(offsetof(StagedInstance, rotation)));
        extensions->glVertexAttribDivisor(KIT_ROTATION_ATTRIB, 1u);

        extensions->glEnableVertexAttribArray(KIT_SCALE_ATTRIB);
        extensions->glVertexAttribPointer(
            KIT_SCALE_ATTRIB, 3, GL_UNSIGNED_SHORT, GL_FALSE, stride,
            pointer(offsetof(StagedInstance, scale)));
        extensions->glVertexAttribDivisor(KIT_SCALE_ATTRIB, 1u);

        const GLsizei drawCount = static_cast<GLsizei>(frame.spans.size());
        for (const osg::ref_ptr<osg::PrimitiveSet>& primitive : getPrimitiveSetList())
        {
            if (const osg::DrawArrays* arrays =
                dynamic_cast<const osg::DrawArrays*>(primitive.get()))
            {
                _arrayCommands->values.resize(frame.spans.size());
                for (std::size_t i = 0u; i < frame.spans.size(); ++i)
                {
                    DrawArraysCommand& command = _arrayCommands->values[i];
                    command.count = static_cast<std::uint32_t>(arrays->getCount());
                    command.instanceCount = frame.spans[i].instanceCount;
                    command.first = static_cast<std::uint32_t>(arrays->getFirst());
                    command.baseInstance =
                        ringBaseInstance + frame.spans[i].firstInstance;
                }
                _arrayCommands->dirty();
                osg::GLBufferObject* commandGL =
                    _arrayCommands->getOrCreateGLBufferObject(state.getContextID());
                if (!commandGL)
                    continue;
                state.bindDrawIndirectBufferObject(commandGL);
                const std::uintptr_t commandOffset = static_cast<std::uintptr_t>(
                    commandGL->getOffset(_arrayCommands->getBufferIndex()));
                extensions->glMultiDrawArraysIndirect(
                    arrays->getMode(),
                    reinterpret_cast<const GLvoid*>(commandOffset),
                    drawCount,
                    static_cast<GLsizei>(sizeof(DrawArraysCommand)));
                continue;
            }

            osg::DrawElements* elements = primitive->getDrawElements();
            if (!elements)
                continue;

            osg::GLBufferObject* elementGL =
                primitive->getOrCreateGLBufferObject(state.getContextID());
            if (!elementGL)
                continue;
            if (elementGL->isDirty())
                elementGL->compileBuffer();
            vas->bindElementBufferObject(elementGL);

            std::size_t indexSize = 0u;
            switch (elements->getDataType())
            {
            case GL_UNSIGNED_BYTE: indexSize = sizeof(std::uint8_t); break;
            case GL_UNSIGNED_SHORT: indexSize = sizeof(std::uint16_t); break;
            case GL_UNSIGNED_INT: indexSize = sizeof(std::uint32_t); break;
            default: continue;
            }
            const std::uint32_t firstIndex = static_cast<std::uint32_t>(
                elementGL->getOffset(primitive->getBufferIndex()) / indexSize);

            _elementCommands->values.resize(frame.spans.size());
            for (std::size_t i = 0u; i < frame.spans.size(); ++i)
            {
                DrawElementsCommand& command = _elementCommands->values[i];
                command.count = elements->getNumIndices();
                command.instanceCount = frame.spans[i].instanceCount;
                command.firstIndex = firstIndex;
                command.baseVertex = 0u;
                command.baseInstance =
                    ringBaseInstance + frame.spans[i].firstInstance;
            }
            _elementCommands->dirty();
            osg::GLBufferObject* commandGL =
                _elementCommands->getOrCreateGLBufferObject(state.getContextID());
            if (!commandGL)
                continue;
            state.bindDrawIndirectBufferObject(commandGL);
            const std::uintptr_t commandOffset = static_cast<std::uintptr_t>(
                commandGL->getOffset(_elementCommands->getBufferIndex()));
            extensions->glMultiDrawElementsIndirect(
                elements->getMode(),
                elements->getDataType(),
                reinterpret_cast<const GLvoid*>(commandOffset),
                drawCount,
                static_cast<GLsizei>(sizeof(DrawElementsCommand)));
        }

        state.unbindDrawIndirectBufferObject();
        for (unsigned attribute = KIT_POSITION_ATTRIB;
             attribute <= KIT_SCALE_ATTRIB; ++attribute)
        {
            extensions->glVertexAttribDivisor(attribute, 0u);
            extensions->glDisableVertexAttribArray(attribute);
        }
        extensions->glBindBufferBase(
            GL_SHADER_STORAGE_BUFFER, KIT_BATCH_SSBO_BINDING, 0u);
        vas->unbindVertexBufferObject();
        vas->unbindElementBufferObject();
        _collector->fenceCurrentSlot(ring, extensions);
    }

    void CollectedGeometry::releaseGLObjects(osg::State* state) const
    {
        osg::Geometry::releaseGLObjects(state);
        if (_collector.valid())
            _collector->releaseGLObjects(state);
    }

    class DisableRendererCulling : public osg::NodeVisitor
    {
    public:
        DisableRendererCulling() :
            osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN) { }

        void apply(osg::Node& node) override
        {
            node.setCullingActive(false);
            traverse(node);
        }

        void apply(osg::Geode& geode) override
        {
            geode.setCullingActive(false);
            for (unsigned i = 0u; i < geode.getNumDrawables(); ++i)
                geode.getDrawable(i)->setCullingActive(false);
            traverse(geode);
        }
    };

    class InstallCollectedGeometry : public osg::NodeVisitor
    {
    public:
        explicit InstallCollectedGeometry(ModelCollector* collector) :
            osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
            _collector(collector) { }

        void apply(osg::Geode& geode) override
        {
            for (unsigned i = 0u; i < geode.getNumDrawables(); ++i)
            {
                osg::Geometry* source = geode.getDrawable(i)->asGeometry();
                if (!source)
                    continue;
                geode.setDrawable(i, new CollectedGeometry(*source, _collector.get()));
                ++drawables;
            }
            traverse(geode);
        }

        unsigned drawables = 0u;

    private:
        osg::ref_ptr<ModelCollector> _collector;
    };

    class KitRenderer : public osg::Group
    {
    public:
        KitRenderer()
        {
            _budget = new InstanceBudget();
            setName("Kit scene-wide renderer");
            setCullingActive(false);
            auto vp = Registry::instance()->getOrCreate<VirtualProgram>(
                "vp.Kit.CollectedInstancing", []()
                {
                    osg::ref_ptr<VirtualProgram> result = new VirtualProgram();
                    result->setInheritShaders(true);
                    result->setName("KitCollectedInstancing");
                    Shaders pkg;
                    pkg.load(result.get(), pkg.KitCollectedInstancing);
                    result->addBindAttribLocation("oe_Kit_position", KIT_POSITION_ATTRIB);
                    result->addBindAttribLocation("oe_Kit_rotation", KIT_ROTATION_ATTRIB);
                    result->addBindAttribLocation("oe_Kit_scale", KIT_SCALE_ATTRIB);
                    return result.release();
                });
            getOrCreateStateSet()->setAttribute(vp);
        }

        void setModel(const std::string& name, osg::Node* prepared)
        {
            auto existing = _models.find(name);
            if (existing != _models.end())
            {
                removeChild(existing->second.branch.get());
                _models.erase(existing);
            }

            osg::ComputeBoundsVisitor boundsVisitor;
            prepared->accept(boundsVisitor);
            ModelRecord record;
            record.collector = new ModelCollector(
                boundsVisitor.getBoundingBox(), _budget.get());
            record.branch = osg::clone(
                prepared,
                osg::CopyOp::DEEP_COPY_NODES |
                osg::CopyOp::DEEP_COPY_DRAWABLES |
                osg::CopyOp::DEEP_COPY_PRIMITIVES);
            InstallCollectedGeometry install(record.collector.get());
            record.branch->accept(install);
            record.collector->setDrawableCount(install.drawables);
            DisableRendererCulling disableCulling;
            record.branch->accept(disableCulling);
            addChild(record.branch.get());
            _models.emplace(name, std::move(record));
        }

        ModelCollector* getCollector(const std::string& name) const
        {
            auto found = _models.find(name);
            return found == _models.end() ? nullptr : found->second.collector.get();
        }

        unsigned getDrawableCount() const
        {
            unsigned result = 0u;
            for (const auto& model : _models)
                result += model.second.collector->getDrawableCount();
            return result;
        }

        std::size_t getCollectedInstanceCount(const osg::Camera* camera) const
        {
            std::size_t result = 0u;
            for (const auto& model : _models)
                result += model.second.collector->getCollectedInstanceCount(camera);
            return result;
        }

        std::size_t getCollectedInstanceCount() const
        {
            std::size_t result = 0u;
            for (const auto& model : _models)
                result += model.second.collector->getCollectedInstanceCount();
            return result;
        }

        void getModelStats(
            const osg::Camera* camera,
            std::uint64_t frameNumber,
            std::vector<Kit::ModelStats>& output) const
        {
            output.clear();
            output.reserve(_models.size());
            for (const auto& model : _models)
            {
                Kit::ModelStats stats;
                stats.name = model.first;
                model.second.collector->getStats(camera, frameNumber, stats);
                output.emplace_back(std::move(stats));
            }
        }

        void setMaxVisibleInstances(std::size_t value)
        {
            _budget->setMaximum(value);
        }

        std::size_t getMaxVisibleInstances() const
        {
            return _budget->getMaximum();
        }

        std::size_t getDroppedInstanceCount(const osg::Camera* camera) const
        {
            return _budget->getDropped(camera);
        }

        std::size_t getDroppedInstanceCount() const
        {
            return _budget->getDropped();
        }

        std::size_t getRingBytes() const
        {
            std::size_t result = 0u;
            for (const auto& model : _models)
                result += model.second.collector->getRingBytes();
            return result;
        }

        std::uint64_t getRingStalls() const
        {
            std::uint64_t result = 0u;
            for (const auto& model : _models)
                result += model.second.collector->getRingStalls();
            return result;
        }

    private:
        struct ModelRecord
        {
            osg::ref_ptr<osg::Node> branch;
            osg::ref_ptr<ModelCollector> collector;
        };
        osg::ref_ptr<InstanceBudget> _budget;
        std::map<std::string, ModelRecord> _models;
    };

    // One of these represents an entire compiled city/tile. It owns all the
    // compact instance batches and submits them directly during cull traversal,
    // avoiding a separate osg::Node (and optional osg::LOD) for every batch.
    class InstanceNode : public osg::Group
    {
    public:
        InstanceNode()
        {
            setDataVariance(osg::Object::STATIC);
            setName("Kit instance batches");
        }

        bool addBatch(ModelCollector* collector, const CompactBatch& batch)
        {
            if (!collector || !batch.buffer.valid() || batch.count == 0u)
                return false;

            Submission submission;
            submission.collector = collector;
            submission.batch = batch;
            // The tile owns its one compact aggregate buffer. Cull traversal
            // submits spans into it; the renderer copies only visible spans
            // into its bounded transient ring for the current frame.
            _submissions.emplace_back(std::move(submission));
            if (batch.bounds.valid())
                _bounds.expandBy(batch.bounds);
            dirtyBound();
            return true;
        }

        void traverse(osg::NodeVisitor& visitor) override
        {
            if (visitor.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
            {
                osgUtil::CullVisitor* cull =
                    dynamic_cast<osgUtil::CullVisitor*>(&visitor);
                const osg::FrameStamp* stamp = visitor.getFrameStamp();
                const osg::Matrixd localToView = cull && cull->getModelViewMatrix() ?
                    osg::Matrixd(*cull->getModelViewMatrix()) : osg::Matrixd::identity();
                const osg::Camera* camera = cull ? cull->getCurrentCamera() : nullptr;
                const std::uint64_t frameNumber = stamp ?
                    stamp->getFrameNumber() : visitor.getTraversalNumber();

                for (const Submission& submission : _submissions)
                {
                    const CompactBatch& batch = submission.batch;
                    if (!isAlwaysVisible(batch.range.x(), batch.range.y()) &&
                        batch.bounds.valid())
                    {
                        const float radius = batch.bounds.radius();
                        const float distance = visitor.getDistanceToEyePoint(
                            batch.bounds.center(), true);
                        const float minRange = batch.range.x() > radius ?
                            batch.range.x() - radius : 0.0f;
                        const float maxRange =
                            batch.range.y() < std::numeric_limits<float>::max() - radius ?
                            batch.range.y() + radius : std::numeric_limits<float>::max();
                        if (distance < minRange || distance >= maxRange)
                            continue;
                    }

                    submission.collector->submit(
                        batch, localToView, camera, frameNumber);
                }
            }
            osg::Group::traverse(visitor);
        }

    protected:
        osg::BoundingSphere computeBound() const override
        {
            osg::BoundingSphere result;
            if (_bounds.valid())
                result.expandBy(_bounds);
            result.expandBy(osg::Group::computeBound());
            return result;
        }

    private:
        struct Submission
        {
            osg::ref_ptr<ModelCollector> collector;
            CompactBatch batch;
        };

        std::vector<Submission> _submissions;
        osg::BoundingBox _bounds;
    };

    osg::Matrixd readTransform(std::istringstream& input, bool& ok)
    {
        osg::Vec3d position;
        osg::Quat rotation;
        osg::Vec3d scale;
        ok = static_cast<bool>(input >>
            position.x() >> position.y() >> position.z() >>
            rotation.x() >> rotation.y() >> rotation.z() >> rotation.w() >>
            scale.x() >> scale.y() >> scale.z());
        return osg::Matrixd::scale(scale) *
            osg::Matrixd::rotate(rotation) *
            osg::Matrixd::translate(position);
    }

    template<typename T>
    bool readBinaryValue(std::istream& input, T& value)
    {
        return static_cast<bool>(input.read(
            reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T))));
    }

    osgDB::ReaderWriter::ReadResult readBinaryCity(const std::string& resolved)
    {
        std::ifstream input(resolved, std::ios::binary);
        if (!input)
            return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;

        std::array<char, 8> magic = {};
        std::uint32_t endianMarker = 0u;
        std::uint32_t batchCount = 0u;
        std::uint64_t totalInstanceCount = 0u;
        if (!input.read(magic.data(), static_cast<std::streamsize>(magic.size())) ||
            !readBinaryValue(input, endianMarker) ||
            !readBinaryValue(input, batchCount) ||
            !readBinaryValue(input, totalInstanceCount))
        {
            return osgDB::ReaderWriter::ReadResult("Truncated binary kit city header");
        }

        const std::array<char, 8> version1Magic = { 'O', 'E', 'K', 'I', 'T', 'B', '0', '1' };
        const std::array<char, 8> version2Magic = { 'O', 'E', 'K', 'I', 'T', 'B', '0', '2' };
        const bool hasInstanceRanges = magic == version2Magic;
        if (magic != version1Magic && !hasInstanceRanges)
            return osgDB::ReaderWriter::ReadResult("Invalid binary kit city signature");
        if (endianMarker != 0x01020304u)
            return osgDB::ReaderWriter::ReadResult("Unsupported binary kit city byte order");
        if (batchCount > 1000000u || totalInstanceCount > 100000000ull ||
            totalInstanceCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        {
            return osgDB::ReaderWriter::ReadResult("Unreasonable binary kit city dimensions");
        }

        static_assert(sizeof(osg::Vec3f) == sizeof(float) * 3u,
            "Binary Kit city positions require tightly packed osg::Vec3f values");
        KitNode::InstanceBatches instanceBatches;
        instanceBatches.reserve(batchCount);
        std::uint64_t instancesRead = 0u;

        for (std::uint32_t batch = 0u; batch < batchCount; ++batch)
        {
            std::uint32_t modelLength = 0u;
            if (!readBinaryValue(input, modelLength) || modelLength == 0u || modelLength > 4096u)
                return osgDB::ReaderWriter::ReadResult("Invalid binary kit city model name");

            std::string model(modelLength, '\0');
            if (!input.read(&model[0], static_cast<std::streamsize>(modelLength)))
                return osgDB::ReaderWriter::ReadResult("Truncated binary kit city model name");

            std::array<float, 9> batchValues = {};
            std::uint64_t count = 0u;
            if (!input.read(
                    reinterpret_cast<char*>(batchValues.data()),
                    static_cast<std::streamsize>(sizeof(float) *
                        (hasInstanceRanges ? batchValues.size() : 7u))) ||
                !readBinaryValue(input, count) ||
                count > totalInstanceCount - instancesRead)
            {
                return osgDB::ReaderWriter::ReadResult("Invalid binary kit city batch");
            }

            KitNode::InstanceBatch instanceBatch;
            instanceBatch.model = std::move(model);
            instanceBatch.rotation = osg::Quat(
                batchValues[0], batchValues[1],
                batchValues[2], batchValues[3]);
            instanceBatch.scale = osg::Vec3f(
                batchValues[4], batchValues[5], batchValues[6]);
            instanceBatch.minRange = hasInstanceRanges ? batchValues[7] : 0.0f;
            instanceBatch.maxRange = hasInstanceRanges ? batchValues[8] :
                std::numeric_limits<float>::max();
            if (!validRange(instanceBatch.minRange, instanceBatch.maxRange))
                return osgDB::ReaderWriter::ReadResult("Invalid binary kit city instance range");
            instanceBatch.positions.resize(static_cast<std::size_t>(count));
            const std::streamsize positionBytes = static_cast<std::streamsize>(
                count * sizeof(osg::Vec3f));
            if (positionBytes > 0 && !input.read(
                reinterpret_cast<char*>(instanceBatch.positions.data()),
                positionBytes))
            {
                return osgDB::ReaderWriter::ReadResult("Truncated binary kit city positions");
            }
            instanceBatches.push_back(std::move(instanceBatch));
            instancesRead += count;
        }

        if (instancesRead != totalInstanceCount)
            return osgDB::ReaderWriter::ReadResult("Binary kit city instance count mismatch");

        osg::ref_ptr<KitNode> result = new KitNode();
        result->setName(osgDB::getSimpleFileName(resolved));
        result->setInstanceBatches(std::move(instanceBatches));
        return result.release();
    }

    class KitCityReaderWriter : public osgDB::ReaderWriter
    {
    public:
        KitCityReaderWriter()
        {
            supportsExtension("kitcity", "osgEarth lightweight Kit instance graph");
            supportsExtension("kitcityb", "osgEarth binary lightweight Kit instance graph");
        }

        const char* className() const override
        {
            return "osgEarth Kit city reader";
        }

        ReadResult readObject(const std::string& fileName, const osgDB::Options* options) const override
        {
            return readNode(fileName, options);
        }

        ReadResult readNode(const std::string& fileName, const osgDB::Options* options) const override
        {
            if (!acceptsExtension(osgDB::getLowerCaseFileExtension(fileName)))
                return ReadResult::FILE_NOT_HANDLED;

            const std::string resolved = osgDB::findDataFile(fileName, options);
            if (resolved.empty())
                return ReadResult::FILE_NOT_FOUND;

            if (osgDB::getLowerCaseFileExtension(fileName) == "kitcityb")
                return readBinaryCity(resolved);

            std::ifstream input(resolved);
            if (!input)
                return ReadResult::ERROR_IN_READING_FILE;

            osg::ref_ptr<osg::Group> root = new osg::Group();
            root->setName(osgDB::getSimpleFileName(resolved));

            std::vector<osg::ref_ptr<osg::Group>> stack;
            stack.push_back(root.get());
            osg::ref_ptr<KitNode> current = new KitNode();
            stack.back()->addChild(current.get());

            std::string line;
            unsigned lineNumber = 0u;
            bool sawHeader = false;
            unsigned cityVersion = 0u;
            while (std::getline(input, line))
            {
                ++lineNumber;
                const std::size_t first = line.find_first_not_of(" \t\r");
                if (first == std::string::npos || line[first] == '#')
                    continue;

                std::istringstream tokens(line.substr(first));
                std::string command;
                tokens >> command;

                if (command == "kitcity")
                {
                    tokens >> cityVersion;
                    if (cityVersion != 1u && cityVersion != 2u)
                        return ReadResult("Unsupported kitcity version");
                    sawHeader = true;
                }
                else if (command == "transform")
                {
                    std::string name;
                    tokens >> std::quoted(name);
                    bool ok = false;
                    const osg::Matrixd matrix = readTransform(tokens, ok);
                    if (!ok)
                        return ReadResult("Malformed transform on line " + std::to_string(lineNumber));

                    osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform(matrix);
                    transform->setName(name);
                    stack.back()->addChild(transform.get());
                    stack.push_back(transform.get());
                    current = new KitNode();
                    stack.back()->addChild(current.get());
                }
                else if (command == "end")
                {
                    if (stack.size() <= 1u)
                        return ReadResult("Unmatched end on line " + std::to_string(lineNumber));
                    stack.pop_back();
                    current = new KitNode();
                    stack.back()->addChild(current.get());
                }
                else if (command == "instance")
                {
                    KitNode::Instance value;
                    tokens >> std::quoted(value.model) >>
                        value.position.x() >> value.position.y() >> value.position.z() >>
                        value.rotation.x() >> value.rotation.y() >> value.rotation.z() >> value.rotation.w() >>
                        value.scale.x() >> value.scale.y() >> value.scale.z();
                    if (!tokens || value.model.empty())
                        return ReadResult("Malformed instance on line " + std::to_string(lineNumber));

                    tokens >> std::ws;
                    if (!tokens.eof())
                    {
                        tokens >> value.minRange >> value.maxRange;
                        if (!tokens)
                            return ReadResult("Malformed instance range on line " + std::to_string(lineNumber));
                        tokens >> std::ws;
                        if (!tokens.eof())
                            return ReadResult("Unexpected instance data on line " + std::to_string(lineNumber));
                    }
                    if (!validRange(value.minRange, value.maxRange))
                        return ReadResult("Invalid instance range on line " + std::to_string(lineNumber));
                    current->addInstance(value);
                }
                else
                {
                    return ReadResult("Unknown kitcity command on line " + std::to_string(lineNumber));
                }
            }

            if (!sawHeader)
                return ReadResult("Missing kitcity header");
            if (stack.size() != 1u)
                return ReadResult("Unclosed transform in kitcity file");

            return root.release();
        }
    };

    REGISTER_OSGPLUGIN(kitcity, KitCityReaderWriter);
}

KitNode::KitNode()
{
    setDataVariance(osg::Object::STATIC);
}

KitNode::KitNode(const KitNode& rhs, const osg::CopyOp& copyop) :
    osg::Group(rhs, copyop),
    _instances(rhs._instances),
    _instanceBatches(rhs._instanceBatches)
{
}

void KitNode::addInstance(const Instance& value)
{
    materializeInstances();
    _instances.push_back(normalizeInstanceRange(value));
    dirtyBound();
}

void KitNode::addInstance(
    const std::string& model,
    const osg::Vec3f& position,
    const osg::Quat& rotation,
    const osg::Vec3f& scale,
    float minRange,
    float maxRange)
{
    addInstance(Instance(model, position, rotation, scale, minRange, maxRange));
}

void KitNode::clearInstances()
{
    _instances.clear();
    _instanceBatches.clear();
    dirtyBound();
}

void KitNode::setInstances(Instances&& values)
{
    _instances = std::move(values);
    _instanceBatches.clear();
    for (auto& value : _instances)
        value = normalizeInstanceRange(value);
    dirtyBound();
}

void KitNode::setInstanceBatches(InstanceBatches&& values)
{
    _instances.clear();
    _instanceBatches = std::move(values);
    dirtyBound();
}

void KitNode::materializeInstances() const
{
    if (_instanceBatches.empty())
        return;

    _instances.reserve(getNumInstances());
    for (const auto& batch : _instanceBatches)
    {
        for (const auto& position : batch.positions)
        {
            _instances.emplace_back(
                batch.model, position, batch.rotation, batch.scale,
                batch.minRange, batch.maxRange);
        }
    }
    _instanceBatches.clear();
}

void KitNode::reserveInstances(std::size_t count)
{
    materializeInstances();
    _instances.reserve(count);
}

const KitNode::Instances& KitNode::getInstances() const
{
    materializeInstances();
    return _instances;
}

std::size_t KitNode::getNumInstances() const
{
    std::size_t result = _instances.size();
    for (const auto& batch : _instanceBatches)
        result += batch.positions.size();
    return result;
}

osg::BoundingSphere KitNode::computeBound() const
{
    osg::BoundingSphere result = osg::Group::computeBound();
    for (const auto& instance : _instances)
    {
        const float radius = 0.5f * instance.scale.length();
        result.expandBy(osg::BoundingSphere(instance.position, radius));
    }
    for (const auto& batch : _instanceBatches)
    {
        const float radius = 0.5f * batch.scale.length();
        for (const auto& position : batch.positions)
            result.expandBy(osg::BoundingSphere(position, radius));
    }
    return result;
}

Kit::Kit() :
    _renderNode(new KitRenderer())
{
}

Kit::~Kit() = default;

unsigned Kit::getNumRenderDrawables() const
{
    const KitRenderer* renderer = dynamic_cast<const KitRenderer*>(_renderNode.get());
    return renderer ? renderer->getDrawableCount() : 0u;
}

std::size_t Kit::getNumCollectedInstances(const osg::Camera* camera) const
{
    const KitRenderer* renderer = dynamic_cast<const KitRenderer*>(_renderNode.get());
    return renderer ? renderer->getCollectedInstanceCount(camera) : 0u;
}

std::size_t Kit::getNumCollectedInstances() const
{
    const KitRenderer* renderer = dynamic_cast<const KitRenderer*>(_renderNode.get());
    return renderer ? renderer->getCollectedInstanceCount() : 0u;
}

void Kit::getModelStats(
    const osg::Camera* camera,
    std::uint64_t frameNumber,
    std::vector<ModelStats>& output) const
{
    const KitRenderer* renderer = dynamic_cast<const KitRenderer*>(_renderNode.get());
    if (renderer)
        renderer->getModelStats(camera, frameNumber, output);
    else
        output.clear();
}

void Kit::setMaxVisibleInstances(std::size_t value)
{
    KitRenderer* renderer = dynamic_cast<KitRenderer*>(_renderNode.get());
    if (renderer)
        renderer->setMaxVisibleInstances(value);
}

std::size_t Kit::getMaxVisibleInstances() const
{
    const KitRenderer* renderer = dynamic_cast<const KitRenderer*>(_renderNode.get());
    return renderer ? renderer->getMaxVisibleInstances() : 0u;
}

std::size_t Kit::getNumDroppedInstances(const osg::Camera* camera) const
{
    const KitRenderer* renderer = dynamic_cast<const KitRenderer*>(_renderNode.get());
    return renderer ? renderer->getDroppedInstanceCount(camera) : 0u;
}

std::size_t Kit::getNumDroppedInstances() const
{
    const KitRenderer* renderer = dynamic_cast<const KitRenderer*>(_renderNode.get());
    return renderer ? renderer->getDroppedInstanceCount() : 0u;
}

std::size_t Kit::getInstanceRingBytes() const
{
    const KitRenderer* renderer = dynamic_cast<const KitRenderer*>(_renderNode.get());
    return renderer ? renderer->getRingBytes() : 0u;
}

std::uint64_t Kit::getInstanceRingStallCount() const
{
    const KitRenderer* renderer = dynamic_cast<const KitRenderer*>(_renderNode.get());
    return renderer ? renderer->getRingStalls() : 0u;
}

bool Kit::load(
    const std::string& manifestURI,
    const osgDB::Options* readOptions,
    ProgressCallback* progress)
{
    _lastError.clear();

    Config manifest;
    if (!manifest.fromURI(URI(manifestURI)))
    {
        _lastError = "Failed to read kit manifest: " + manifestURI;
        return false;
    }

    unsigned loaded = 0u;
    for (const auto& modelConfig : manifest.children("model"))
    {
        if (progress && progress->canceled())
        {
            _lastError = "Kit load canceled";
            return false;
        }

        const std::string name = modelConfig.value("name");
        const std::string url = modelConfig.value("url");
        if (name.empty() || url.empty())
        {
            OE_WARN << LC << "Ignoring a model without both name and url" << std::endl;
            continue;
        }

        osg::ref_ptr<osg::Node> node = URI(url, URIContext(modelConfig.referrer())).getNode(readOptions, progress);
        if (!node.valid())
        {
            _lastError = "Failed to load model '" + name + "' from " + url;
            return false;
        }

        if (!addModel(name, node.get()))
            return false;
        ++loaded;
    }

    if (loaded == 0u)
    {
        _lastError = "Kit manifest contains no models: " + manifestURI;
        return false;
    }

    return true;
}

bool Kit::addModel(const std::string& name, osg::Node* model)
{
    if (name.empty() || !model)
    {
        _lastError = "A kit model requires a non-empty name and a valid node";
        return false;
    }

    osg::ref_ptr<osg::Node> prepared = osg::clone(
        model,
        osg::CopyOp::DEEP_COPY_NODES |
        osg::CopyOp::DEEP_COPY_DRAWABLES |
        osg::CopyOp::DEEP_COPY_ARRAYS |
        osg::CopyOp::DEEP_COPY_PRIMITIVES |
        osg::CopyOp::DEEP_COPY_STATESETS);

    osgUtil::Optimizer optimizer;
    optimizer.optimize(
        prepared.get(),
        osgUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS |
        osgUtil::Optimizer::MERGE_GEODES |
        osgUtil::Optimizer::MERGE_GEOMETRY |
        osgUtil::Optimizer::REMOVE_REDUNDANT_NODES |
        osgUtil::Optimizer::STATIC_OBJECT_DETECTION);

    Registry::shaderGenerator().run(prepared.get());
    prepared->setName(name);
    static_cast<KitRenderer*>(_renderNode.get())->setModel(name, prepared.get());
    _models[name] = prepared;
    return true;
}

bool Kit::hasModel(const std::string& name) const
{
    return _models.find(name) != _models.end();
}

osg::Node* Kit::getModel(const std::string& name)
{
    auto i = _models.find(name);
    return i == _models.end() ? nullptr : i->second.get();
}

const osg::Node* Kit::getModel(const std::string& name) const
{
    auto i = _models.find(name);
    return i == _models.end() ? nullptr : i->second.get();
}

osg::Group* Kit::createInstancedNode(osg::Node* source, BuildStats* outStats) const
{
    BuildStats stats;
    osg::ref_ptr<InstanceNode> result = new InstanceNode();

    if (!source)
    {
        if (outStats) *outStats = stats;
        return result.release();
    }

    GatherInstances gather(_instanceChunkSize);
    source->accept(gather);

    std::size_t totalInstances = 0u;
    for (const auto& entry : gather.instances)
        totalInstances += entry.second.positions->size();
    InstanceBuilder instanceBuilder;
    instanceBuilder.reserveInstances(totalInstances);
    KitRenderer* renderer = static_cast<KitRenderer*>(_renderNode.get());
    std::set<ModelCollector*> referencedCollectors;

    for (auto& entry : gather.instances)
    {
        stats.instances += static_cast<unsigned>(entry.second.positions->size());
        auto model = _models.find(entry.first.model);
        if (model == _models.end())
        {
            ++stats.missingModels;
            OE_WARN << LC << "Instance references missing model '" << entry.first.model << "'" << std::endl;
            continue;
        }

        ModelCollector* collector = renderer->getCollector(entry.first.model);
        if (!collector)
        {
            ++stats.missingModels;
            continue;
        }
        instanceBuilder.setPositions(entry.second.positions.get());
        instanceBuilder.setRotations(entry.second.rotations.get());
        instanceBuilder.setScales(entry.second.scales.get());
        instanceBuilder.setRange(osg::Vec2f(
            entry.first.minRange, entry.first.maxRange));
        instanceBuilder.compressInstanceAttributes();

        // The city-level builder now owns this batch's compact records. Drop
        // the temporary float arrays immediately instead of retaining every
        // 40-byte source transform until the complete tile has finished
        // compiling. This keeps peak construction memory close to the larger
        // of the gathered and packed representations instead of their sum,
        // and spreads temporary-array cleanup across batch construction.
        entry.second.positions = nullptr;
        entry.second.rotations = nullptr;
        entry.second.scales = nullptr;

        instanceBuilder.setBaseBoundingBox(collector->getModelBounds());
        if (collector->getDrawableCount() == 0u)
        {
            OE_WARN << LC << "Model '" << entry.first.model << "' has no osg::Geometry" << std::endl;
            continue;
        }

        CompactBatch compact;
        compact.buffer = instanceBuilder.getInstanceBuffer();
        compact.offset = instanceBuilder.getInstanceOffset();
        compact.count = instanceBuilder.getInstanceCount();
        compact.positionOffset = instanceBuilder.getPositionOffset();
        compact.positionScale = instanceBuilder.getPositionScale();
        compact.scaleOffset = instanceBuilder.getScaleOffset();
        compact.scaleScale = instanceBuilder.getScaleScale();
        compact.range.set(entry.first.minRange, entry.first.maxRange);
        compact.bounds = instanceBuilder.getInstancedBoundingBox();

        if (result->addBatch(collector, compact))
        {
            referencedCollectors.insert(collector);
            ++stats.batches;
        }
    }

    for (ModelCollector* collector : referencedCollectors)
        stats.drawables += collector->getDrawableCount();

    if (outStats) *outStats = stats;
    return result.release();
}
