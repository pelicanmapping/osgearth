/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#define TINYBVH_IMPLEMENTATION
#include "TinyBVHShape"

#include <algorithm>
#include <cstring>

using namespace osgEarth::Util;

TinyBVHShape::TinyBVHShape()
{
}

TinyBVHShape::TinyBVHShape(osg::Geometry* geometry, unsigned maxPrimitivesPerLeaf) :
    _maxPrimitivesPerLeaf((std::max)(1u, maxPrimitivesPerLeaf))
{
    build(geometry);
}

TinyBVHShape::TinyBVHShape(const TinyBVHShape& rhs, const osg::CopyOp& copyop) :
    osg::Shape(rhs, copyop),
    _maxPrimitivesPerLeaf(rhs._maxPrimitivesPerLeaf)
{
    if (rhs._vertices.valid() && !rhs._primitiveSets.empty())
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry();
        geometry->setVertexArray(rhs._vertices.get());
        for (const auto& primitiveSet : rhs._primitiveSets)
            geometry->addPrimitiveSet(primitiveSet.get());
        build(geometry.get());
    }
}

TinyBVHShape::~TinyBVHShape()
{
}

bool TinyBVHShape::build(osg::Geometry* geometry)
{
    if (!geometry || geometry->getNumPrimitiveSets() == 0u)
        return false;

    osg::Vec3Array* vertices = dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray());
    if (!vertices)
        return false;

    std::vector<osg::ref_ptr<osg::PrimitiveSet>> primitiveSets;
    std::vector<unsigned> primitiveStarts(1u, 0u);
    primitiveSets.reserve(geometry->getNumPrimitiveSets());
    primitiveStarts.reserve(geometry->getNumPrimitiveSets() + 1u);

    for (unsigned i = 0u; i < geometry->getNumPrimitiveSets(); ++i)
    {
        osg::PrimitiveSet* primitiveSet = geometry->getPrimitiveSet(i);
        if (!primitiveSet || primitiveSet->getNumIndices() == 0u)
            continue;
        if (primitiveSet->getMode() != osg::PrimitiveSet::TRIANGLES ||
            primitiveSet->getNumIndices() < 3u ||
            primitiveSet->getNumIndices() % 3u != 0u)
        {
            return false;
        }

        primitiveSets.emplace_back(primitiveSet);
        primitiveStarts.push_back(
            primitiveStarts.back() + primitiveSet->getNumIndices() / 3u);
    }

    if (primitiveSets.empty())
        return false;

    const unsigned primitiveCount = primitiveStarts.back();

    std::vector<tinybvh::bvhvec4> triangleVertices;
    triangleVertices.resize(static_cast<std::size_t>(primitiveCount) * 3u);

    for (unsigned setIndex = 0u; setIndex < primitiveSets.size(); ++setIndex)
    {
        const osg::PrimitiveSet* primitiveSet = primitiveSets[setIndex].get();
        const unsigned setPrimitiveCount = primitiveStarts[setIndex + 1u] - primitiveStarts[setIndex];
        for (unsigned localPrimitive = 0u; localPrimitive < setPrimitiveCount; ++localPrimitive)
        {
            const unsigned indexOffset = localPrimitive * 3u;
            const unsigned i0 = primitiveSet->index(indexOffset);
            const unsigned i1 = primitiveSet->index(indexOffset + 1u);
            const unsigned i2 = primitiveSet->index(indexOffset + 2u);
            if (i0 >= vertices->size() || i1 >= vertices->size() || i2 >= vertices->size())
                return false;

            const osg::Vec3& v0 = (*vertices)[i0];
            const osg::Vec3& v1 = (*vertices)[i1];
            const osg::Vec3& v2 = (*vertices)[i2];
            const unsigned outputOffset = (primitiveStarts[setIndex] + localPrimitive) * 3u;

            triangleVertices[outputOffset] = tinybvh::bvhvec4(v0.x(), v0.y(), v0.z(), 0.0f);
            triangleVertices[outputOffset + 1u] = tinybvh::bvhvec4(v1.x(), v1.y(), v1.z(), 0.0f);
            triangleVertices[outputOffset + 2u] = tinybvh::bvhvec4(v2.x(), v2.y(), v2.z(), 0.0f);
        }
    }

    _vertices = vertices;
    _primitiveSets = std::move(primitiveSets);
    _primitiveStarts = std::move(primitiveStarts);
    _primitiveCount = primitiveCount;

#ifdef OSGEARTH_TINYBVH_USE_BUILD_QUICK
    _bvh.BuildQuick(triangleVertices.data(), primitiveCount);
#else
    _bvh.BuildAVX(triangleVertices.data(), primitiveCount);
#endif
    collapseLeaves(0u);
    compact();

    // TinyBVH does not own its input vertices. Traversal resolves triangles
    // through the Geometry, so do not retain a pointer to the temporary array.
    _bvh.verts = {};
    releaseBuildData();
    return valid();
}

bool TinyBVHShape::valid() const
{
    return _vertices.valid() &&
        !_primitiveSets.empty() &&
        _primitiveStarts.size() == _primitiveSets.size() + 1u &&
        _primitiveCount > 0u &&
        _bvh.bvhNode != nullptr &&
        _bvh.primIdx != nullptr;
}

std::size_t TinyBVHShape::getNodeCount() const
{
    return _bvh.usedNodes;
}

std::size_t TinyBVHShape::getPrimitiveCount() const
{
    return _primitiveCount;
}

std::size_t TinyBVHShape::getMemoryUsage() const
{
    return _bvh.usedNodes * sizeof(tinybvh::BVH::BVHNode) +
        _bvh.idxCount * sizeof(unsigned);
}

const char* TinyBVHShape::getBuilderName()
{
#ifdef OSGEARTH_TINYBVH_USE_BUILD_QUICK
    return "BuildQuick";
#else
    return "BuildAVX";
#endif
}

bool TinyBVHShape::getTriangle(
    unsigned primitiveIndex,
    osg::Vec3d& v0,
    osg::Vec3d& v1,
    osg::Vec3d& v2,
    unsigned& i0,
    unsigned& i1,
    unsigned& i2) const
{
    if (!valid() || primitiveIndex >= _primitiveCount)
        return false;

    const auto start = std::upper_bound(
        _primitiveStarts.begin(), _primitiveStarts.end(), primitiveIndex);
    if (start == _primitiveStarts.begin() || start == _primitiveStarts.end())
        return false;

    const unsigned setIndex = static_cast<unsigned>(start - _primitiveStarts.begin() - 1u);
    const unsigned offset = (primitiveIndex - _primitiveStarts[setIndex]) * 3u;
    const osg::PrimitiveSet* primitiveSet = _primitiveSets[setIndex].get();
    i0 = primitiveSet->index(offset);
    i1 = primitiveSet->index(offset + 1u);
    i2 = primitiveSet->index(offset + 2u);
    v0 = (*_vertices)[i0];
    v1 = (*_vertices)[i1];
    v2 = (*_vertices)[i2];
    return true;
}

unsigned TinyBVHShape::collapseLeaves(unsigned nodeIndex)
{
    tinybvh::BVH::BVHNode& node = _bvh.bvhNode[nodeIndex];
    if (node.isLeaf())
        return node.triCount;

    tinybvh::BVH::BVHNode& left = _bvh.bvhNode[node.leftFirst];
    tinybvh::BVH::BVHNode& right = _bvh.bvhNode[node.leftFirst + 1u];
    const unsigned leftCount = collapseLeaves(node.leftFirst);
    const unsigned rightCount = collapseLeaves(node.leftFirst + 1u);
    const unsigned primitiveCount = leftCount + rightCount;

    if (primitiveCount <= _maxPrimitivesPerLeaf)
    {
        node.leftFirst = (std::min)(left.leftFirst, right.leftFirst);
        node.triCount = primitiveCount;
    }
    return primitiveCount;
}

void TinyBVHShape::compact()
{
    struct PendingNode
    {
        unsigned source;
        unsigned destination;
    };

    std::vector<unsigned> countStack(1u, 0u);
    unsigned nodeCount = 0u;
    while (!countStack.empty())
    {
        const unsigned nodeIndex = countStack.back();
        countStack.pop_back();
        ++nodeCount;

        const tinybvh::BVH::BVHNode& node = _bvh.bvhNode[nodeIndex];
        if (!node.isLeaf())
        {
            countStack.push_back(node.leftFirst + 1u);
            countStack.push_back(node.leftFirst);
        }
    }

    auto* compactNodes = static_cast<tinybvh::BVH::BVHNode*>(
        _bvh.AlignedAlloc(nodeCount * sizeof(tinybvh::BVH::BVHNode)));

    unsigned nextNode = 1u;
    std::vector<PendingNode> stack(1u, { 0u, 0u });
    while (!stack.empty())
    {
        const PendingNode pending = stack.back();
        stack.pop_back();

        const tinybvh::BVH::BVHNode& source = _bvh.bvhNode[pending.source];
        tinybvh::BVH::BVHNode& destination = compactNodes[pending.destination];
        destination = source;

        if (!source.isLeaf())
        {
            const unsigned firstChild = nextNode;
            nextNode += 2u;
            destination.leftFirst = firstChild;
            stack.push_back({ source.leftFirst + 1u, firstChild + 1u });
            stack.push_back({ source.leftFirst, firstChild });
        }
    }

    _bvh.AlignedFree(_bvh.bvhNode);
    _bvh.bvhNode = compactNodes;
    _bvh.usedNodes = nextNode;
    _bvh.allocatedNodes = nextNode;
}

void TinyBVHShape::releaseBuildData()
{
    if (_bvh.fragment)
    {
        _bvh.AlignedFree(_bvh.fragment);
        _bvh.fragment = nullptr;
    }

    if (_bvh.bvhNode && _bvh.usedNodes > 0u && _bvh.allocatedNodes > _bvh.usedNodes)
    {
        const std::size_t size = _bvh.usedNodes * sizeof(tinybvh::BVH::BVHNode);
        auto* compactNodes = static_cast<tinybvh::BVH::BVHNode*>(_bvh.AlignedAlloc(size));
        std::memcpy(compactNodes, _bvh.bvhNode, size);
        _bvh.AlignedFree(_bvh.bvhNode);
        _bvh.bvhNode = compactNodes;
        _bvh.allocatedNodes = _bvh.usedNodes;
    }

    _bvh.rebuildable = false;
}

TinyBVHBuilder::TinyBVHBuilder(unsigned maxPrimitivesPerLeaf) :
    osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
    _maxPrimitivesPerLeaf((std::max)(1u, maxPrimitivesPerLeaf))
{
}

TinyBVHBuilder::TinyBVHBuilder(const TinyBVHBuilder& rhs) :
    osg::Object(rhs),
    osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
    _maxPrimitivesPerLeaf(rhs._maxPrimitivesPerLeaf)
{
}

void TinyBVHBuilder::apply(osg::Geometry& geometry)
{
    if (dynamic_cast<TinyBVHShape*>(geometry.getShape()))
        return;

    osg::ref_ptr<TinyBVHShape> shape =
        new TinyBVHShape(&geometry, _maxPrimitivesPerLeaf);
    if (shape->valid())
        geometry.setShape(shape.get());
}
