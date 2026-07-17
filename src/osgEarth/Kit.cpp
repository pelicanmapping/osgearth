/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/Kit>
#include <osgEarth/Config>
#include <osgEarth/InstanceBuilder>
#include <osgEarth/Notify>
#include <osgEarth/Registry>
#include <osgEarth/URI>

#include <osg/ComputeBoundsVisitor>
#include <osg/Geode>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgUtil/Optimizer>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
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

    class InstallInstances : public osg::NodeVisitor
    {
    public:
        InstallInstances(
            const InstanceArrays& instances,
            float minRange,
            float maxRange) :
            osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            builder.setPositions(instances.positions.get());
            builder.setRotations(instances.rotations.get());
            builder.setScales(instances.scales.get());
            builder.setRange(osg::Vec2f(minRange, maxRange));
        }

        void apply(osg::Geode& geode) override
        {
            for (unsigned i = 0u; i < geode.getNumDrawables(); ++i)
            {
                osg::Geometry* source = geode.getDrawable(i)->asGeometry();
                if (!source)
                    continue;

                osg::ref_ptr<osg::Geometry> geometry = InstanceBuilder::createGeometry(*source);
                geometry->setUseDisplayList(false);
                geometry->setUseVertexBufferObjects(true);
                builder.installInstancing(geometry.get());
                geode.setDrawable(i, geometry.get());
                ++drawables;
            }
            traverse(geode);
        }

        InstanceBuilder builder;
        unsigned drawables = 0u;
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
        osgUtil::Optimizer::REMOVE_REDUNDANT_NODES |
        osgUtil::Optimizer::STATIC_OBJECT_DETECTION);

    Registry::shaderGenerator().run(prepared.get());
    prepared->setName(name);
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
    osg::ref_ptr<osg::Group> result = new osg::Group();
    result->setName("Kit instanced batches");

    if (!source)
    {
        if (outStats) *outStats = stats;
        return result.release();
    }

    GatherInstances gather(_instanceChunkSize);
    source->accept(gather);

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

        osg::ref_ptr<osg::Node> batch = osg::clone(
            model->second.get(),
            osg::CopyOp::DEEP_COPY_NODES |
            osg::CopyOp::DEEP_COPY_DRAWABLES |
            osg::CopyOp::DEEP_COPY_PRIMITIVES |
            osg::CopyOp::DEEP_COPY_STATESETS);

        osg::ComputeBoundsVisitor modelBounds;
        batch->accept(modelBounds);
        InstallInstances install(
            entry.second, entry.first.minRange, entry.first.maxRange);
        install.builder.compressInstanceAttributes();

        // The builder now owns the compact arrays. Drop this batch's float
        // gather arrays immediately instead of retaining every 40-byte source
        // transform until the complete tile has finished compiling. This
        // keeps peak construction memory close to the larger of the gathered
        // and packed representations instead of their sum, and spreads the
        // temporary-array cleanup across batch construction.
        entry.second.positions = nullptr;
        entry.second.rotations = nullptr;
        entry.second.scales = nullptr;

        install.builder.setBaseBoundingBox(modelBounds.getBoundingBox());
        batch->accept(install);
        if (install.drawables == 0u)
        {
            OE_WARN << LC << "Model '" << entry.first.model << "' has no osg::Geometry" << std::endl;
            continue;
        }

        const std::string name = "Kit batch: " + entry.first.model;
        batch->setName(name);
        if (isAlwaysVisible(entry.first.minRange, entry.first.maxRange) ||
            _instanceChunkSize <= 0.0f)
        {
            result->addChild(batch.get());
        }
        else
        {
            const float radius = batch->getBound().radius();
            const float minRange =
                entry.first.minRange > radius ? entry.first.minRange - radius : 0.0f;
            const float maxRange =
                entry.first.maxRange < std::numeric_limits<float>::max() - radius ?
                entry.first.maxRange + radius : std::numeric_limits<float>::max();
            osg::ref_ptr<osg::LOD> lod = new osg::LOD();
            lod->setName(name + " LOD");
            lod->setRangeMode(osg::LOD::DISTANCE_FROM_EYE_POINT);
            lod->addChild(batch.get(), minRange, maxRange);
            result->addChild(lod.get());
        }
        ++stats.batches;
        stats.drawables += install.drawables;
    }

    if (outStats) *outStats = stats;
    return result.release();
}
