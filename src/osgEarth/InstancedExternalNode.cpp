/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */

#include <osgEarth/InstancedExternalNode>
#include <osgEarth/Capabilities>
#include <osgEarth/ExternalNode>
#include <osgEarth/InstanceBuilder>
#include <osgEarth/Registry>
#include <osgEarth/StateTransition>
#include <osgEarth/VirtualProgram>

#include <osg/Drawable>
#include <osg/BufferObject>
#include <osg/FrontFace>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/NodeCallback>
#include <osg/Program>
#include <osg/Transform>
#include <osgDB/ObjectWrapper>
#include <osgDB/Registry>
#include <osgUtil/Optimizer>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <typeinfo>
#include <vector>

using namespace osgEarth;
using namespace osgEarth::Util;

#define LC "[InstancedExternalNode] "

namespace
{
    enum : unsigned
    {
        INSTANCE_POSITION_ATTRIBUTE = 9u,
        INSTANCE_ROTATION_ATTRIBUTE = 10u,
        INSTANCE_SCALE_ATTRIBUTE = 11u
    };

    bool optionPresent(
        const std::string& options,
        const std::string& option)
    {
        std::string::size_type pos = 0u;
        while ((pos = options.find(option, pos)) != std::string::npos)
        {
            const bool startsToken =
                pos == 0u ||
                std::isspace(static_cast<unsigned char>(options[pos - 1u]));
            const std::string::size_type end = pos + option.size();
            const bool endsToken =
                end == options.size() ||
                std::isspace(static_cast<unsigned char>(options[end]));
            if (startsToken && endsToken)
                return true;
            pos = end;
        }
        return false;
    }

    bool reversesWinding(const osg::Matrixf& matrix)
    {
        const float determinant =
            matrix(0, 0) *
                (matrix(1, 1) * matrix(2, 2) -
                 matrix(1, 2) * matrix(2, 1)) -
            matrix(0, 1) *
                (matrix(1, 0) * matrix(2, 2) -
                 matrix(1, 2) * matrix(2, 0)) +
            matrix(0, 2) *
                (matrix(1, 0) * matrix(2, 1) -
                 matrix(1, 1) * matrix(2, 0));
        return determinant < 0.0f;
    }

    bool nearlyEqual(float lhs, float rhs)
    {
        const float scale = std::max(
            1.0f,
            std::max(std::abs(lhs), std::abs(rhs)));
        return std::abs(lhs - rhs) <= 1.0e-4f * scale;
    }

    bool hasNormalSafeLinearPart(const osg::Matrixd& matrix)
    {
        // OSG's static-transform flattener applies the forward 3x3 to normal
        // arrays. That is directionally correct only for an orthogonal,
        // uniform-scale linear transform (including reflections).
        // Projective matrices cannot be flattened this way at all. OSG uses
        // row-vector matrices, so an affine matrix has a zero final column
        // above an m(3, 3) value of one.
        const double affineTolerance = 1.0e-12;
        if (std::abs(matrix(0, 3)) > affineTolerance ||
            std::abs(matrix(1, 3)) > affineTolerance ||
            std::abs(matrix(2, 3)) > affineTolerance ||
            std::abs(matrix(3, 3) - 1.0) > affineTolerance)
        {
            return false;
        }

        const osg::Vec3d axis[3] = {
            osg::Vec3d(matrix(0, 0), matrix(0, 1), matrix(0, 2)),
            osg::Vec3d(matrix(1, 0), matrix(1, 1), matrix(1, 2)),
            osg::Vec3d(matrix(2, 0), matrix(2, 1), matrix(2, 2))
        };
        const double length2[3] = {
            axis[0].length2(), axis[1].length2(), axis[2].length2()
        };
        const double scale = std::max(
            1.0,
            std::max(length2[0], std::max(length2[1], length2[2])));
        const double tolerance = 1.0e-8 * scale;

        if (length2[0] <= tolerance ||
            length2[1] <= tolerance ||
            length2[2] <= tolerance)
        {
            return false;
        }
        if (std::abs(length2[0] - length2[1]) > tolerance ||
            std::abs(length2[0] - length2[2]) > tolerance)
        {
            return false;
        }
        if (std::abs(axis[0] * axis[1]) > tolerance ||
            std::abs(axis[0] * axis[2]) > tolerance ||
            std::abs(axis[1] * axis[2]) > tolerance)
        {
            return false;
        }
        return true;
    }

    struct InstanceArrays
    {
        InstanceArrays() :
            vbo(new osg::VertexBufferObject()),
            positions(new osg::Vec3Array()),
            rotations(new osg::Vec4Array()),
            scales(new osg::Vec3Array())
        {
            vbo->setUsage(GL_STATIC_DRAW);
            vbo->setDataVariance(osg::Object::STATIC);
            // Keep per-batch attributes out of the immutable model VBO. OSG
            // otherwise adopts the first VBO already present on the Geometry,
            // which is the shared static model VBO in this path.
            positions->setVertexBufferObject(vbo.get());
            rotations->setVertexBufferObject(vbo.get());
            scales->setVertexBufferObject(vbo.get());
            positions->setDataVariance(osg::Object::STATIC);
            rotations->setDataVariance(osg::Object::STATIC);
            scales->setDataVariance(osg::Object::STATIC);
        }

        bool append(const osg::Matrixf& matrix)
        {
            osg::Vec3f translation;
            osg::Quat rotation;
            osg::Vec3f scale;
            osg::Quat scaleOrientation;
            matrix.decompose(
                translation,
                rotation,
                scale,
                scaleOrientation);
            if (std::abs(scale.x()) <= 1.0e-7f ||
                std::abs(scale.y()) <= 1.0e-7f ||
                std::abs(scale.z()) <= 1.0e-7f)
            {
                // The attribute shader uses inverse scale for normals.
                return false;
            }
            // InstanceBuilder follows the glTF TRS order. Validate the
            // decomposition by reconstructing it; a non-identity scale
            // orientation (affine shear) will fail this check and use the
            // exact shared-node fallback.
            const osg::Matrixf reconstructed =
                osg::Matrixf::scale(scale) *
                osg::Matrixf::rotate(rotation) *
                osg::Matrixf::translate(translation);
            for (unsigned row = 0u; row < 4u; ++row)
            {
                for (unsigned column = 0u; column < 4u; ++column)
                {
                    if (!nearlyEqual(
                            matrix(row, column),
                            reconstructed(row, column)))
                    {
                        return false;
                    }
                }
            }

            positions->push_back(translation);
            rotations->push_back(osg::Vec4f(
                static_cast<float>(rotation.x()),
                static_cast<float>(rotation.y()),
                static_cast<float>(rotation.z()),
                static_cast<float>(rotation.w())));
            scales->push_back(scale);
            return true;
        }

        osg::ref_ptr<osg::VertexBufferObject> vbo;
        osg::ref_ptr<osg::Vec3Array> positions;
        osg::ref_ptr<osg::Vec4Array> rotations;
        osg::ref_ptr<osg::Vec3Array> scales;
    };

    /** Reject graphs whose semantics static transform flattening would
     * change. Fallback still shares the canonical payload and renders it once
     * per transform. */
    struct HardwareEligibilityVisitor : public osg::NodeVisitor
    {
        HardwareEligibilityVisitor() :
            osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            setNodeMaskOverride(~0u);
        }

        void reject(const std::string& value)
        {
            if (error.empty())
                error = value;
        }

        void inspectStateSet(const osg::StateSet* stateSet)
        {
            if (!stateSet || !error.empty())
                return;

            if (isDynamic(stateSet))
            {
                reject("dynamic StateSets require shared-node fallback");
                return;
            }

            for (const auto& entry : stateSet->getAttributeList())
            {
                if (isDynamic(entry.second.first.get()))
                {
                    reject(
                        "dynamic state attributes require shared-node "
                        "fallback");
                    return;
                }
            }
            for (const auto& unit : stateSet->getTextureAttributeList())
            {
                for (const auto& entry : unit)
                {
                    if (isDynamic(entry.second.first.get()))
                    {
                        reject(
                            "dynamic texture state requires shared-node "
                            "fallback");
                        return;
                    }
                }
            }
            for (const auto& entry : stateSet->getUniformList())
            {
                if (isDynamic(entry.second.first.get()))
                {
                    reject(
                        "dynamic uniforms require shared-node fallback");
                    return;
                }
            }

            if (stateSet->requiresUpdateTraversal() ||
                stateSet->requiresEventTraversal())
            {
                reject("state callbacks require shared-node fallback");
                return;
            }

            if ((stateSet->getMode(GL_BLEND) &
                    osg::StateAttribute::ON) != 0u ||
                stateSet->getRenderingHint() ==
                    osg::StateSet::TRANSPARENT_BIN)
            {
                reject(
                    "transparent graphs require shared-node fallback for "
                    "per-reference depth sorting");
                return;
            }

            const osg::StateAttribute* program = stateSet->getAttribute(
                osg::StateAttribute::PROGRAM);
            if (program &&
                dynamic_cast<const VirtualProgram*>(program) == nullptr)
            {
                reject(
                    "raw osg::Program state is incompatible with the "
                    "InstanceBuilder shader composition path");
            }
        }

        bool isDynamic(const osg::Object* object) const
        {
            return object &&
                object->getDataVariance() == osg::Object::DYNAMIC;
        }

        bool hasDynamicArrays(const osg::Geometry& geometry) const
        {
            if (isDynamic(geometry.getVertexArray()) ||
                isDynamic(geometry.getNormalArray()) ||
                isDynamic(geometry.getColorArray()) ||
                isDynamic(geometry.getSecondaryColorArray()) ||
                isDynamic(geometry.getFogCoordArray()))
            {
                return true;
            }

            for (const auto& array : geometry.getTexCoordArrayList())
            {
                if (isDynamic(array.get()))
                    return true;
            }
            for (const auto& array : geometry.getVertexAttribArrayList())
            {
                if (isDynamic(array.get()))
                    return true;
            }
            return false;
        }

        void inspect(osg::Node& node)
        {
            inspectStateSet(node.getStateSet());
            if (dynamic_cast<ExternalNode*>(&node) != nullptr ||
                dynamic_cast<InstancedExternalNode*>(&node) != nullptr)
            {
                reject("nested external references require shared-node fallback");
            }
            else if (dynamic_cast<StateTransition*>(&node) != nullptr)
            {
                reject("state-transition graphs require shared-node fallback");
            }
            else if (node.getUpdateCallback() || node.getEventCallback() ||
                     node.getCullCallback())
            {
                reject("callback-driven graphs require shared-node fallback");
            }
            else if (isDynamic(&node) && node.asDrawable() == nullptr)
            {
                reject("dynamic scene graph nodes require shared-node fallback");
            }
            else if (auto* transform = dynamic_cast<osg::Transform*>(&node))
            {
                if (typeid(*transform) != typeid(osg::MatrixTransform))
                {
                    reject(
                        "specialized transforms require shared-node fallback");
                }
                else if (transform->getReferenceFrame() !=
                         osg::Transform::RELATIVE_RF)
                {
                    reject(
                        "absolute transforms require shared-node fallback");
                }
                else if (!hasNormalSafeLinearPart(
                             static_cast<osg::MatrixTransform*>(transform)->
                                 getMatrix()))
                {
                    reject(
                        "non-uniform or sheared internal transforms require "
                        "shared-node fallback to preserve normals");
                }
            }
            else if (typeid(node) != typeid(osg::Node) &&
                     typeid(node) != typeid(osg::Group) &&
                     typeid(node) != typeid(osg::Geode) &&
                     typeid(node) != typeid(osg::Geometry))
            {
                // Cameras, lights, billboards, paging nodes, and other
                // specialized nodes have per-traversal semantics that cannot
                // be represented by one geometry instance list.
                reject(
                    "specialized scene graph nodes require shared-node fallback");
            }
        }

        void apply(osg::Node& node) override
        {
            inspect(node);
            if (error.empty())
                traverse(node);
        }

        void apply(osg::LOD& node) override
        {
            inspect(node);
            reject("LOD graphs require shared-node fallback");
        }

        void apply(osg::Drawable& drawable) override
        {
            inspect(drawable);
            if (!error.empty())
                return;
            osg::Geometry* geometry = drawable.asGeometry();
            if (!geometry)
            {
                reject("non-Geometry drawables require shared-node fallback");
                return;
            }
            if (typeid(*geometry) != typeid(osg::Geometry))
            {
                reject(
                    "specialized Geometry subclasses require shared-node "
                    "fallback");
                return;
            }
            if (geometry->getNumParents() == 0u)
            {
                reject(
                    "Geometry outside an osg::Geode requires shared-node "
                    "fallback");
                return;
            }
            for (unsigned int i = 0u; i < geometry->getNumParents(); ++i)
            {
                if (typeid(*geometry->getParent(i)) != typeid(osg::Geode))
                {
                    reject(
                        "Geometry outside an osg::Geode requires shared-node "
                        "fallback");
                    return;
                }
            }
            if (drawable.getDrawCallback())
            {
                reject("custom drawable callbacks require shared-node fallback");
                return;
            }
            if (isDynamic(geometry) || hasDynamicArrays(*geometry))
            {
                reject("dynamic geometry arrays require shared-node fallback");
                return;
            }
            if (geometry->getNumPrimitiveSets() == 0u)
            {
                reject("geometry without primitive sets cannot be instanced");
                return;
            }
            if (geometry->getVertexAttribArray(
                    INSTANCE_POSITION_ATTRIBUTE) ||
                geometry->getVertexAttribArray(
                    INSTANCE_ROTATION_ATTRIBUTE) ||
                geometry->getVertexAttribArray(
                    INSTANCE_SCALE_ATTRIBUTE))
            {
                reject("graphs that already use instance attributes require shared-node fallback");
                return;
            }
            for (unsigned int i = 0u;
                 i < geometry->getNumPrimitiveSets(); ++i)
            {
                if (isDynamic(geometry->getPrimitiveSet(i)))
                {
                    reject("dynamic primitive sets require shared-node fallback");
                    return;
                }
                if (geometry->getPrimitiveSet(i)->getNumInstances() > 1u)
                {
                    reject("nested instanced draws require shared-node fallback");
                    return;
                }
            }
            ++geometryCount;
        }

        std::string error;
        unsigned geometryCount = 0u;
    };

    struct MakeTransformsStaticVisitor : public osg::NodeVisitor
    {
        MakeTransformsStaticVisitor() :
            osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            setNodeMaskOverride(~0u);
        }

        void apply(osg::Transform& transform) override
        {
            transform.setDataVariance(osg::Object::STATIC);
            traverse(transform);
        }
    };

    struct AssignStaticVertexBufferObjectVisitor : public osg::NodeVisitor
    {
        AssignStaticVertexBufferObjectVisitor() :
            osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
            vbo(new osg::VertexBufferObject())
        {
            setNodeMaskOverride(~0u);
            vbo->setUsage(GL_STATIC_DRAW);
            vbo->setDataVariance(osg::Object::STATIC);
        }

        void assign(osg::Array* array)
        {
            if (array &&
                (array->getBinding() == osg::Array::BIND_PER_VERTEX ||
                 array->getBinding() == osg::Array::BIND_UNDEFINED))
            {
                array->setVertexBufferObject(vbo.get());
            }
        }

        void apply(osg::Geometry& geometry) override
        {
            assign(geometry.getVertexArray());
            assign(geometry.getNormalArray());
            assign(geometry.getColorArray());
            assign(geometry.getSecondaryColorArray());
            assign(geometry.getFogCoordArray());
            for (auto& array : geometry.getTexCoordArrayList())
                assign(array.get());
            for (auto& array : geometry.getVertexAttribArrayList())
                assign(array.get());
        }

        osg::ref_ptr<osg::VertexBufferObject> vbo;
    };

    bool prepareHardwareTemplate(
        osg::Node* source,
        osg::ref_ptr<osg::Node>& output,
        unsigned& geometryCount,
        std::string& error)
    {
        output = nullptr;
        geometryCount = 0u;
        error.clear();

        HardwareEligibilityVisitor eligibility;
        source->accept(eligibility);
        if (!eligibility.error.empty())
        {
            error = eligibility.error;
            return false;
        }
        if (eligibility.geometryCount == 0u)
        {
            error = "external graph contains no renderable geometry";
            return false;
        }

        const osg::CopyOp copyOp =
            osg::CopyOp::DEEP_COPY_ALL &
            ~osg::CopyOp::DEEP_COPY_IMAGES &
            ~osg::CopyOp::DEEP_COPY_TEXTURES &
            ~osg::CopyOp::DEEP_COPY_SHAPES;
        osg::ref_ptr<osg::Node> snapshot = osg::clone(source, copyOp);
        if (!snapshot.valid())
        {
            error = "failed to clone the external graph";
            return false;
        }

        osg::ref_ptr<osg::Group> prepared = new osg::Group();
        prepared->setName(
            "osgEarth::InstancedExternalNode shared prepared template");
        prepared->addChild(snapshot.get());

        // Bake static internal transforms once per managed asset revision.
        // All batches can then share the resulting immutable vertex arrays.
        MakeTransformsStaticVisitor makeStatic;
        prepared->accept(makeStatic);
        osgUtil::Optimizer::FlattenStaticTransformsDuplicatingSharedSubgraphsVisitor
            flatten;
        prepared->accept(flatten);

        // Make the immutable/static buffer domain explicit. Every batch
        // shallow-shares these arrays and this VBO; its instance attributes
        // are assigned to a different VBO before installation.
        AssignStaticVertexBufferObjectVisitor assignStaticVBO;
        prepared->accept(assignStaticVBO);

        output = prepared;
        geometryCount = eligibility.geometryCount;
        return true;
    }

    struct ToggleFrontFaceVisitor : public osg::NodeVisitor
    {
        ToggleFrontFaceVisitor() :
            osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            setNodeMaskOverride(~0u);
        }

        void inspect(osg::StateSet* stateSet)
        {
            if (!stateSet || !visitedStateSets.insert(stateSet).second)
                return;

            auto* frontFace = dynamic_cast<osg::FrontFace*>(
                stateSet->getAttribute(osg::StateAttribute::FRONTFACE));
            if (!frontFace || !visitedFrontFaces.insert(frontFace).second)
                return;

            frontFace->setMode(
                frontFace->getMode() == osg::FrontFace::CLOCKWISE ?
                    osg::FrontFace::COUNTER_CLOCKWISE :
                    osg::FrontFace::CLOCKWISE);
        }

        void apply(osg::Node& node) override
        {
            inspect(node.getStateSet());
            traverse(node);
        }

        void apply(osg::Drawable& drawable) override
        {
            inspect(drawable.getStateSet());
        }

        std::set<osg::StateSet*> visitedStateSets;
        std::set<osg::FrontFace*> visitedFrontFaces;
    };

    struct InstallAttributeInstancingVisitor : public osg::NodeVisitor
    {
        explicit InstallAttributeInstancingVisitor(
            const InstanceArrays& arrays) :
            osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            builder.setPositions(arrays.positions.get());
            builder.setRotations(arrays.rotations.get());
            builder.setScales(arrays.scales.get());
            setNodeMaskOverride(~0u);
        }

        void apply(osg::Geometry& geometry) override
        {
            osg::ref_ptr<osg::Geometry> instanced =
                InstanceBuilder::createGeometry(
                    geometry,
                    osg::CopyOp::SHALLOW_COPY);
            // The attribute-instancing path intentionally has no expanded
            // intersection proxy. Keep any source KdTree on the canonical
            // ExternalNode graph, but do not retain it on the private render
            // snapshot where it would describe only the uninstanced mesh.
            instanced->setShape(nullptr);
            builder.installInstancing(instanced.get());

            std::vector<osg::ref_ptr<osg::Group>> parents;
            parents.reserve(geometry.getNumParents());
            for (unsigned int i = 0u; i < geometry.getNumParents(); ++i)
                parents.emplace_back(geometry.getParent(i));
            for (auto& parent : parents)
                parent->replaceChild(&geometry, instanced.get());
            ++installedGeometryCount;
        }

        InstanceBuilder builder;
        unsigned installedGeometryCount = 0u;
    };

    struct RefreshCallback : public osg::NodeCallback
    {
        void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
        {
            auto* external = dynamic_cast<InstancedExternalNode*>(node);
            if (external)
                external->refresh();
            traverse(node, visitor);
        }
    };
}

InstancedExternalNode::InstancedExternalNode() :
    _source(new ExternalNode())
{
    _source->setInstancingStats(0u, false, false);
    setDataVariance(osg::Object::DYNAMIC);
    setUpdateCallback(new RefreshCallback());
}

InstancedExternalNode::InstancedExternalNode(
    const std::string& filename,
    const MatrixList& matrices,
    const osgDB::Options* options) :
    InstancedExternalNode()
{
    _matrices = matrices;
    setFileName(filename, options);
}

InstancedExternalNode::InstancedExternalNode(
    const InstancedExternalNode& rhs,
    const osg::CopyOp& copyop) :
    // Children are transient rendering snapshots and are always rebuilt.
    osg::Group(rhs, osg::CopyOp::SHALLOW_COPY),
    _filename(rhs._filename),
    _readOptionsString(rhs._readOptionsString),
    _readOptions(Registry::cloneOrCreateOptions(rhs._readOptions.get())),
    _source(new ExternalNode()),
    _matrices(rhs._matrices)
{
    (void)copyop;
    _source->setInstancingStats(
        static_cast<std::uint64_t>(_matrices.size()), false, false);
    if (getNumChildren() > 0u)
        removeChildren(0u, getNumChildren());
    setDataVariance(osg::Object::DYNAMIC);
    setUpdateCallback(new RefreshCallback());
    if (!_filename.empty())
        _source->setFileName(_filename, _readOptions.get());
    refresh();
}

InstancedExternalNode::~InstancedExternalNode() = default;

bool
InstancedExternalNode::setFileName(
    const std::string& filename,
    const osgDB::Options* options)
{
    _filename = filename;
    _readOptions = Registry::cloneOrCreateOptions(options);
    _readOptionsString = _readOptions.valid() ?
        _readOptions->getOptionString() : std::string();

    const bool result = _source->setFileName(filename, _readOptions.get());
    osg::ref_ptr<osg::Node> current;
    _source->getAssetSnapshot(current, _observedRevision);
    _hasObservedRevision = true;
    // Filename/options changes are identity changes even if a custom reader
    // happens to return the same osg::Node pointer for both requests.
    rebuild(current.get());
    return result;
}

void
InstancedExternalNode::setMatrices(const MatrixList& matrices)
{
    _matrices = matrices;
    rebuild(_source->getExternalNode().get());
}

bool
InstancedExternalNode::load()
{
    const bool result = _source->load();
    refresh();
    return result;
}

bool
InstancedExternalNode::reload()
{
    const bool result = _source->reload();
    refresh();
    return result;
}

void
InstancedExternalNode::unload()
{
    _source->unload();
    refresh();
}

bool
InstancedExternalNode::isLoaded() const
{
    return _source->isLoaded();
}

osg::ref_ptr<osg::Node>
InstancedExternalNode::getExternalNode() const
{
    return _source->getExternalNode();
}

std::string
InstancedExternalNode::getLastError() const
{
    return _source->getLastError();
}

void
InstancedExternalNode::refresh()
{
    osg::ref_ptr<osg::Node> current;
    std::uint64_t revision = 0u;
    _source->getAssetSnapshot(current, revision);
    if (_hasObservedRevision && revision == _observedRevision)
        return;

    _observedRevision = revision;
    _hasObservedRevision = true;
    rebuild(current.get());
}

void
InstancedExternalNode::rebuild(osg::Node* source)
{
    osg::ref_ptr<osg::Group> replacement = new osg::Group();
    replacement->setName("osgEarth::InstancedExternalNode rendering graph");
    _usingHardwareInstancing = false;
    _instancingError.clear();

    if (source && !_matrices.empty())
    {
        _usingHardwareInstancing =
            buildHardwareGraph(source, replacement.get());
        if (!_usingHardwareInstancing)
            buildFallbackGraph(source, replacement.get());
    }

    if (getNumChildren() > 0u)
        removeChildren(0u, getNumChildren());
    if (source && !_matrices.empty())
        addChild(replacement.get());
    _source->setInstancingStats(
        static_cast<std::uint64_t>(_matrices.size()),
        _usingHardwareInstancing,
        source && !_matrices.empty() && !_usingHardwareInstancing);
    dirtyBound();
}

bool
InstancedExternalNode::buildHardwareGraph(
    osg::Node* source,
    osg::Group* output)
{
    if (!Registry::capabilities().supportsDrawInstanced())
    {
        _instancingError = "OpenGL draw instancing is unavailable";
        return false;
    }
    if (!Registry::capabilities().supportsInstancedArrays())
    {
        _instancingError =
            "OpenGL per-attribute instance divisors are unavailable";
        return false;
    }

    bool templateReady = false;
    osg::ref_ptr<osg::Node> preparedTemplate;
    unsigned preparedGeometryCount = 0u;
    std::string preparationError;
    if (!_source->getInstancingTemplate(
            _observedRevision,
            templateReady,
            preparedTemplate,
            preparedGeometryCount,
            preparationError))
    {
        _instancingError =
            "external asset changed while preparing hardware instancing";
        return false;
    }

    if (!templateReady)
    {
        osg::ref_ptr<osg::Node> candidate;
        unsigned candidateGeometryCount = 0u;
        std::string candidateError;
        prepareHardwareTemplate(
            source,
            candidate,
            candidateGeometryCount,
            candidateError);

        if (!_source->installInstancingTemplate(
                _observedRevision,
                candidate.get(),
                candidateGeometryCount,
                candidateError,
                preparedTemplate,
                preparedGeometryCount,
                preparationError))
        {
            _instancingError =
                "external asset changed while installing its shared "
                "instancing template";
            return false;
        }
    }

    if (!preparationError.empty())
    {
        _instancingError = preparationError;
        return false;
    }
    if (!preparedTemplate.valid() || preparedGeometryCount == 0u)
    {
        _instancingError = "external instancing template is empty";
        return false;
    }

    MatrixList partitions[2];
    for (const auto& matrix : _matrices)
        partitions[reversesWinding(matrix) ? 1u : 0u].push_back(matrix);

    // A nested glTF reader may have prepared its explicit FrontFace states
    // for the parent parity encoded in this option. Generic external graphs
    // have an orientation-preserving source context by default.
    const bool sourceParentReversesWinding = optionPresent(
        _readOptionsString,
        "gltfParentReversesWinding");

    osg::ref_ptr<osg::Group> candidate = new osg::Group();
    const osg::CopyOp batchCopyOp =
        osg::CopyOp::DEEP_COPY_NODES |
        osg::CopyOp::DEEP_COPY_DRAWABLES |
        osg::CopyOp::DEEP_COPY_STATESETS |
        osg::CopyOp::DEEP_COPY_STATEATTRIBUTES |
        osg::CopyOp::DEEP_COPY_PRIMITIVES |
        osg::CopyOp::DEEP_COPY_UNIFORMS;

    for (unsigned parity = 0u; parity < 2u; ++parity)
    {
        if (partitions[parity].empty())
            continue;

        InstanceArrays arrays;
        for (const auto& matrix : partitions[parity])
        {
            if (!arrays.append(matrix))
            {
                _instancingError =
                    "instance transforms contain affine shear or singular "
                    "scale that the EXT_mesh_gpu_instancing attribute path "
                    "cannot represent";
                return false;
            }
        }

        // Static arrays retain the prepared template's immutable VBO. The
        // InstanceArrays above already own a separate batch-local VBO, so
        // InstanceBuilder cannot add them to or mutate the static VBO layout.
        osg::ref_ptr<osg::Node> snapshot =
            osg::clone(preparedTemplate.get(), batchCopyOp);
        if (!snapshot.valid())
        {
            _instancingError = "failed to clone the external graph";
            return false;
        }

        // Root partition state covers source paths with no explicit
        // FrontFace. Explicit child states override it, so toggle each of
        // those when this partition's parity differs from the context for
        // which the source graph was prepared.
        if ((parity == 1u) != sourceParentReversesWinding)
        {
            ToggleFrontFaceVisitor toggleFrontFace;
            snapshot->accept(toggleFrontFace);
        }

        osg::ref_ptr<osg::Group> partition = new osg::Group();
        partition->setName(
            "osgEarth::InstanceBuilder external asset partition");
        partition->addChild(snapshot.get());

        InstallAttributeInstancingVisitor install(arrays);
        partition->accept(install);
        if (install.installedGeometryCount != preparedGeometryCount)
        {
            _instancingError =
                "not all external geometry could be converted to instance attributes";
            return false;
        }

        // Front-face orientation cannot vary per gl_InstanceID, so mirrored
        // transforms occupy their own partition/draws.
        partition->getOrCreateStateSet()->setAttribute(
            new osg::FrontFace(parity == 1u ?
                osg::FrontFace::CLOCKWISE :
                osg::FrontFace::COUNTER_CLOCKWISE));
        candidate->addChild(partition.get());
    }

    output->addChild(candidate.get());
    return true;
}

void
InstancedExternalNode::buildFallbackGraph(
    osg::Node* source,
    osg::Group* output)
{
    for (const auto& matrix : _matrices)
    {
        osg::ref_ptr<osg::MatrixTransform> transform =
            new osg::MatrixTransform();
        transform->setMatrix(matrix);
        transform->getOrCreateStateSet()->setAttribute(
            new osg::FrontFace(reversesWinding(matrix) ?
                osg::FrontFace::CLOCKWISE :
                osg::FrontFace::COUNTER_CLOCKWISE));
        transform->addChild(source);
        output->addChild(transform.get());
    }
}

//...........................................................................

#undef LC
#define LC "[InstancedExternalNode Serializer] "

namespace osgEarth { namespace Serializers { namespace InstancedExternalNode
{
    static bool checkExternalInstances(
        const osgEarth::InstancedExternalNode& node)
    {
        return !node.getFileName().empty() ||
            !node.getMatrices().empty();
    }

    static bool readExternalInstances(
        osgDB::InputStream& input,
        osgEarth::InstancedExternalNode& node)
    {
        input >> input.BEGIN_BRACKET;
        std::string filename;
        std::string readOptions;
        input >> input.PROPERTY("FileName");
        input.readWrappedString(filename);
        input >> input.PROPERTY("ReadOptions");
        input.readWrappedString(readOptions);

        input >> input.PROPERTY("Matrices");
        const unsigned int size = input.readSize();
        input >> input.BEGIN_BRACKET;
        osgEarth::InstancedExternalNode::MatrixList matrices;
        matrices.reserve(size);
        for (unsigned int i = 0u; i < size; ++i)
        {
            osg::Matrixf matrix;
            input >> matrix;
            matrices.push_back(matrix);
        }
        input >> input.END_BRACKET;
        input >> input.END_BRACKET;

        osg::ref_ptr<osgDB::Options> options =
            osgEarth::Registry::cloneOrCreateOptions(input.getOptions());
        options->setOptionString(readOptions);
        node.setMatrices(matrices);
        node.setFileName(filename, options.get());
        return true;
    }

    static bool writeExternalInstances(
        osgDB::OutputStream& output,
        const osgEarth::InstancedExternalNode& node)
    {
        output << output.BEGIN_BRACKET << std::endl;
        output << output.PROPERTY("FileName");
        output.writeWrappedString(node.getFileName());
        output << std::endl;
        output << output.PROPERTY("ReadOptions");
        output.writeWrappedString(node.getReadOptionsString());
        output << std::endl;

        output << output.PROPERTY("Matrices");
        output.writeSize(node.getMatrices().size());
        output << output.BEGIN_BRACKET << std::endl;
        for (const auto& matrix : node.getMatrices())
            output << matrix << std::endl;
        output << output.END_BRACKET << std::endl;
        output << output.END_BRACKET << std::endl;
        return true;
    }

    REGISTER_OBJECT_WRAPPER(
        InstancedExternalNode,
        new osgEarth::InstancedExternalNode,
        osgEarth::InstancedExternalNode,
        "osg::Object osg::Node osgEarth::InstancedExternalNode")
    {
        ADD_USER_SERIALIZER(ExternalInstances);
    }
}}}
