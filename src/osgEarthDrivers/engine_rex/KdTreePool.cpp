/* -*-c++-*- OpenSceneGraph - Copyright (C) 1998-2006 Robert Osfield
 *
 * This library is open source and may be redistributed and/or modified under
 * the terms of the OpenSceneGraph Public License (OSGPL) version 0.0 or
 * (at your option) any later version.  The full license is in LICENSE file
 * included with this distribution, and on the openscenegraph.org website.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * OpenSceneGraph Public License for more details.
*/

// Adapted November 6, 2026 from OSG 3.6.5 KdTree.cpp: reuse output/scratch storage,
// accumulate primitive bounds in the collection pass, and add bounded snapshot recycling.
// The upstream license is included in KdTreePool.LICENSE.txt.
#include "KdTreePool"
#include <osg/TemplatePrimitiveIndexFunctor>
#include <osgEarth/Metrics>
#include <typeinfo>
#include <limits>
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
#include <Superluminal/PerformanceAPI.h>
#endif

using namespace osg;
using namespace osgEarth;
using namespace osgEarth::REX;

namespace
{
    constexpr std::size_t treeBudget = 32u * 1024u * 1024u;
    constexpr std::size_t workspaceBudget = 8u * 1024u * 1024u;

    //! Counts retained vector capacity, excluding small fixed object/allocator overheads.
    std::size_t bytes(const osg::KdTree& tree)
    {
        return tree.getNodes().capacity() * sizeof(osg::KdTree::KdNode) +
            (tree.getPrimitiveIndices().capacity() + tree.getVertexIndices().capacity()) * sizeof(unsigned) +
            (tree.getVertices() ? tree.getVertices()->capacity() * sizeof(osg::Vec3) : 0);
    }

    struct BuildKdTree
    {
        //! Binds one build to exclusively leased output and scratch storage.
        BuildKdTree(KdTree& kdTree, std::vector<unsigned>& axes, std::vector<unsigned>& order,
            std::vector<osg::Vec3>& centers, std::vector<unsigned>& reordered) :
            _kdTree(kdTree), _axisStack(axes), _primitiveIndices(order), _centers(centers), _reordered(reordered) {}

        typedef std::vector< osg::Vec3 >            CenterList;
        typedef std::vector< unsigned int >           Indices;
        typedef std::vector< unsigned int >         AxisStack;

        //! Rebuilds all partitions and primitive membership, retaining vector capacities.
        inline bool build(KdTree::BuildOptions& options, osg::PrimitiveSet* primitives);

        //! Selects split axes using the stock OSG longest-dimension rule.
        inline void computeDivisions(KdTree::BuildOptions& options);

        //! Partitions primitive centers and computes conservative leaf/parent bounds.
        inline int divide(KdTree::BuildOptions& options, osg::BoundingBox& bb, int nodeIndex, unsigned int level);

        KdTree&             _kdTree;

        osg::BoundingBox    _bb;
        AxisStack&          _axisStack;
        Indices&            _primitiveIndices;
        CenterList&         _centers;
        Indices&            _reordered;

    protected:

        BuildKdTree& operator=(const BuildKdTree&) = delete;
    };

    struct PrimitiveIndicesCollector
    {
        //! Starts disconnected; the build installs its workspace before traversal.
        PrimitiveIndicesCollector():
            _buildKdTree(0)
        {
        }

        //! Records original primitive IDs, excluding coincident-vertex degeneracies exactly as OSG does.
        inline void operator () (unsigned int p0)
        {
            const osg::Vec3& v0 = (*(_buildKdTree->_kdTree.getVertices()))[p0];

            osg::BoundingBox bb;
            bb.expandBy(v0);
            // Include degenerate primitives in the source bound, as osg::Geometry does.
            _buildKdTree->_bb.expandBy(bb);

            _buildKdTree->_kdTree.addPoint(p0);

            _buildKdTree->_primitiveIndices.push_back(_buildKdTree->_centers.size());
            _buildKdTree->_centers.push_back(bb.center());
        }

        //! Records original primitive IDs, excluding coincident-vertex degeneracies exactly as OSG does.
        inline void operator () (unsigned int p0, unsigned int p1)
        {
            const osg::Vec3& v0 = (*(_buildKdTree->_kdTree.getVertices()))[p0];
            const osg::Vec3& v1 = (*(_buildKdTree->_kdTree.getVertices()))[p1];

            osg::BoundingBox bb;
            bb.expandBy(v0);
            bb.expandBy(v1);
            // Include degenerate primitives in the source bound, as osg::Geometry does.
            _buildKdTree->_bb.expandBy(bb);

            if (v0==v1)
            {
                _buildKdTree->_kdTree._degenerateCount++;
                return;
            }

            _buildKdTree->_kdTree.addLine(p0,p1);

            _buildKdTree->_primitiveIndices.push_back(_buildKdTree->_centers.size());
            _buildKdTree->_centers.push_back(bb.center());
        }

        //! Records original primitive IDs, excluding coincident-vertex degeneracies exactly as OSG does.
        inline void operator () (unsigned int p0, unsigned int p1, unsigned int p2)
        {
            const osg::Vec3& v0 = (*(_buildKdTree->_kdTree.getVertices()))[p0];
            const osg::Vec3& v1 = (*(_buildKdTree->_kdTree.getVertices()))[p1];
            const osg::Vec3& v2 = (*(_buildKdTree->_kdTree.getVertices()))[p2];

            osg::BoundingBox bb;
            bb.expandBy(v0);
            bb.expandBy(v1);
            bb.expandBy(v2);
            // Include degenerate primitives in the source bound, as osg::Geometry does.
            _buildKdTree->_bb.expandBy(bb);

            if (v0==v1 || v1==v2 || v2==v0)
            {
                _buildKdTree->_kdTree._degenerateCount++;
                return;
            }

            _buildKdTree->_kdTree.addTriangle(p0,p1,p2);

            _buildKdTree->_primitiveIndices.push_back(_buildKdTree->_centers.size());
            _buildKdTree->_centers.push_back(bb.center());
        }

        //! Records original primitive IDs, excluding coincident-vertex degeneracies exactly as OSG does.
        inline void operator () (unsigned int p0, unsigned int p1, unsigned int p2, unsigned int p3)
        {
            const osg::Vec3& v0 = (*(_buildKdTree->_kdTree.getVertices()))[p0];
            const osg::Vec3& v1 = (*(_buildKdTree->_kdTree.getVertices()))[p1];
            const osg::Vec3& v2 = (*(_buildKdTree->_kdTree.getVertices()))[p2];
            const osg::Vec3& v3 = (*(_buildKdTree->_kdTree.getVertices()))[p3];

            osg::BoundingBox bb;
            bb.expandBy(v0);
            bb.expandBy(v1);
            bb.expandBy(v2);
            bb.expandBy(v3);
            // Include degenerate primitives in the source bound, as osg::Geometry does.
            _buildKdTree->_bb.expandBy(bb);

            if (v0==v1 || v1==v2 || v2==v0 || v3==v0 || v3==v1 || v3==v2)
            {
                _buildKdTree->_kdTree._degenerateCount++;
                return;
            }

            _buildKdTree->_kdTree.addQuad(p0,p1,p2,p3);

            _buildKdTree->_primitiveIndices.push_back(_buildKdTree->_centers.size());
            _buildKdTree->_centers.push_back(bb.center());
        }

        BuildKdTree* _buildKdTree;
    };

    bool BuildKdTree::build(KdTree::BuildOptions& options, osg::PrimitiveSet* primitives)
    {
        const osg::Vec3Array* vertices = _kdTree.getVertices();
        if (!vertices) return false;

        if (vertices->size() <= options._targetNumTrianglesPerLeaf) return false;

        _axisStack.clear();
        _primitiveIndices.clear();
        _centers.clear();
        _reordered.clear();
        _kdTree.getNodes().clear();
        _kdTree.getPrimitiveIndices().clear();
        _kdTree.getVertexIndices().clear();
        _kdTree._degenerateCount = 0;
        _bb.init();

        unsigned int estimatedSize = (unsigned int)(2.0*float(vertices->size())/float(options._targetNumTrianglesPerLeaf));

        _kdTree.getNodes().reserve(estimatedSize*5);

        options._numVerticesProcessed += vertices->size();

        unsigned int estimatedNumTriangles = vertices->size()*2;
        _primitiveIndices.reserve(estimatedNumTriangles);
        _centers.reserve(estimatedNumTriangles);

        osg::TemplatePrimitiveIndexFunctor<PrimitiveIndicesCollector> collectIndices;
        collectIndices._buildKdTree = this;
        primitives->accept(collectIndices);
        computeDivisions(options);

        _primitiveIndices.reserve(vertices->size());

        KdTree::KdNode node(-1, _primitiveIndices.size());
        node.bb = _bb;

        int nodeNum = _kdTree.addNode(node);

        osg::BoundingBox bb = _bb;
        nodeNum = divide(options, bb, nodeNum, 0);

        osg::KdTree::Indices& primitiveIndices = _kdTree.getPrimitiveIndices();

        _reordered.reserve(_primitiveIndices.size());
        for(Indices::iterator itr = _primitiveIndices.begin();
            itr != _primitiveIndices.end();
            ++itr)
        {
            _reordered.push_back(primitiveIndices[*itr]);
        }
        primitiveIndices.swap(_reordered);

        return !_kdTree.getNodes().empty();
    }

    void BuildKdTree::computeDivisions(KdTree::BuildOptions& options)
    {
        osg::Vec3 dimensions(_bb.xMax()-_bb.xMin(),
                             _bb.yMax()-_bb.yMin(),
                             _bb.zMax()-_bb.zMin());

        _axisStack.reserve(options._maxNumLevels);

        for(unsigned int level=0; level<options._maxNumLevels; ++level)
        {
            int axis = 0;
            if (dimensions[0]>=dimensions[1])
            {
                if (dimensions[0]>=dimensions[2]) axis = 0;
                else axis = 2;
            }
            else if (dimensions[1]>=dimensions[2]) axis = 1;
            else axis = 2;

            _axisStack.push_back(axis);
            dimensions[axis] /= 2.0f;
        }
    }

    int BuildKdTree::divide(KdTree::BuildOptions& options, osg::BoundingBox& bb, int nodeIndex, unsigned int level)
    {
        KdTree::KdNode& node = _kdTree.getNode(nodeIndex);

        bool needToDivide = level < _axisStack.size() &&
                            (node.first<0 && static_cast<unsigned int>(node.second)>options._targetNumTrianglesPerLeaf);

        if (!needToDivide)
        {
            if (node.first<0)
            {
                int istart = -node.first-1;
                int iend = istart+node.second-1;

                node.bb.init();
                for(int i=istart; i<=iend; ++i)
                {
                    unsigned int primitiveIndex = _kdTree.getPrimitiveIndices()[_primitiveIndices[i]];
                    primitiveIndex++; //skip original Primitive index
                    unsigned int numPoints = _kdTree.getVertexIndices()[primitiveIndex++];

                    for(; numPoints>0; --numPoints)
                    {
                        unsigned int vi = _kdTree.getVertexIndices()[primitiveIndex++];
                        const osg::Vec3& v = (*_kdTree.getVertices())[vi];
                        node.bb.expandBy(v);
                    }
                }

                if (node.bb.valid())
                {
                    float epsilon = 1e-6f;
                    node.bb._min.x() -= epsilon;
                    node.bb._min.y() -= epsilon;
                    node.bb._min.z() -= epsilon;
                    node.bb._max.x() += epsilon;
                    node.bb._max.y() += epsilon;
                    node.bb._max.z() += epsilon;
                }
            }

            return nodeIndex;
        }

        int axis = _axisStack[level];

        if (node.first<0)
        {
            int istart = -node.first-1;
            int iend = istart+node.second-1;

            float original_min = bb._min[axis];
            float original_max = bb._max[axis];

            float mid = (original_min+original_max)*0.5f;

            int originalLeftChildIndex = 0;
            int originalRightChildIndex = 0;
            bool insitueDivision = false;

            {
                int left = istart;
                int right = iend;

                while(left<right)
                {
                    while(left<right && (_centers[_primitiveIndices[left]][axis]<=mid)) { ++left; }

                    while(left<right && (_centers[_primitiveIndices[right]][axis]>mid)) { --right; }

                    if (left<right)
                    {
                        std::swap(_primitiveIndices[left], _primitiveIndices[right]);
                        ++left;
                        --right;
                    }
                }

                if (left==right)
                {
                    if (_centers[_primitiveIndices[left]][axis]<=mid) ++left;
                    else --right;
                }

                KdTree::KdNode leftLeaf(-istart-1, (right-istart)+1);
                KdTree::KdNode rightLeaf(-left-1, (iend-left)+1);

                if (leftLeaf.second<=0)
                {
                    originalLeftChildIndex = 0;
                    originalRightChildIndex = nodeIndex;
                    insitueDivision = true;
                }
                else if (rightLeaf.second<=0)
                {
                    originalLeftChildIndex = nodeIndex;
                    originalRightChildIndex = 0;
                    insitueDivision = true;
                }
                else
                {
                    originalLeftChildIndex = _kdTree.addNode(leftLeaf);
                    originalRightChildIndex = _kdTree.addNode(rightLeaf);
                }
            }

            float restore = bb._max[axis];
            bb._max[axis] = mid;

            int leftChildIndex = originalLeftChildIndex!=0 ? divide(options, bb, originalLeftChildIndex, level+1) : 0;

            bb._max[axis] = restore;

            restore = bb._min[axis];
            bb._min[axis] = mid;

            int rightChildIndex = originalRightChildIndex!=0 ? divide(options, bb, originalRightChildIndex, level+1) : 0;

            bb._min[axis] = restore;

            if (!insitueDivision)
            {
                KdTree::KdNode& newNodeRef = _kdTree.getNode(nodeIndex);

                newNodeRef.first = leftChildIndex;
                newNodeRef.second = rightChildIndex;

                insitueDivision = true;

                newNodeRef.bb.init();
                if (leftChildIndex!=0) newNodeRef.bb.expandBy(_kdTree.getNode(leftChildIndex).bb);
                if (rightChildIndex!=0) newNodeRef.bb.expandBy(_kdTree.getNode(rightChildIndex).bb);
            }
        }

        return nodeIndex;
    }
}

struct KdTreePool::Workspace : osg::Referenced
{
    std::vector<unsigned> axes, order, reordered;
    std::vector<osg::Vec3> centers;

    //! Reports scratch capacity for bounded idle retention.
    std::size_t bytes() const
    {
        return (axes.capacity() + order.capacity() + reordered.capacity()) * sizeof(unsigned) +
            centers.capacity() * sizeof(osg::Vec3);
    }
};

struct KdTreePool::Retirement : osg::Referenced
{
    std::shared_ptr<KdTreePool> pool;
    osg::ref_ptr<osg::Shape> shape;
    //! Retains the pool and outgoing snapshot until the disposal worker receives sole carrier ownership.
    Retirement(std::shared_ptr<KdTreePool> owner, osg::Shape* outgoing) : pool(std::move(owner)), shape(outgoing) { }
    //! Recycles only after detachment; rejected snapshots follow ordinary OSG destruction on this worker.
    ~Retirement() override { pool->recycle(shape); }
};

KdTreePool::KdTreePool(const ObjectReleaser::Client& releaser) : _releaser(releaser) { }
KdTreePool::~KdTreePool() = default;

osg::ref_ptr<osg::KdTree>
KdTreePool::build(const std::vector<osg::Vec3>& mesh, osg::PrimitiveSet* primitives, osg::KdTree::BuildOptions options)
{
    OE_PROFILING_ZONE_NAMED("Build pooled terrain KdTree");
    if (!primitives || options._targetNumTrianglesPerLeaf == 0 || mesh.size() <= options._targetNumTrianglesPerLeaf)
        return nullptr;

    osg::ref_ptr<osg::KdTree> tree;
    osg::ref_ptr<Workspace> workspace;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // Best fit avoids consuming a very large tree for a small tile when a closer match exists.
        std::size_t best = _trees.size(), capacity = std::numeric_limits<std::size_t>::max();
        for (std::size_t i = 0; i < _trees.size(); ++i)
        {
            if (_trees[i] && _trees[i]->getVertices()->capacity() >= mesh.size() &&
                _trees[i]->getVertices()->capacity() < capacity)
            {
                best = i;
                capacity = _trees[i]->getVertices()->capacity();
            }
        }
        if (best != _trees.size())
        {
            _treeBytes -= bytes(*_trees[best]);
            tree.swap(_trees[best]);
        }
        for (auto& slot : _workspaces)
        {
            if (slot)
            {
                _workspaceBytes -= slot->bytes();
                workspace.swap(slot);
                break;
            }
        }
    }
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
    PERFORMANCEAPI_INSTRUMENT_DATA("Build pooled terrain KdTree", tree.valid() ? "reuse" : "miss");
#endif
    if (!tree)
    {
        tree = new osg::KdTree;
        tree->setVertices(new osg::Vec3Array);
    }
    if (!workspace) workspace = new Workspace;
    auto* vertices = const_cast<osg::Vec3Array*>(tree->getVertices());
    vertices->assign(mesh.begin(), mesh.end());
    BuildKdTree builder(*tree, workspace->axes, workspace->order, workspace->centers, workspace->reordered);
    bool built = builder.build(options, primitives);
    recycle(workspace);
    if (!built)
    {
        osg::ref_ptr<osg::Shape> rejected = tree;
        tree = nullptr;
        retire(rejected);
    }
    return tree;
}

void KdTreePool::retire(osg::ref_ptr<osg::Shape>& shape)
{
    if (!shape) return;
    ObjectReleaser::Objects retired{new Retirement(shared_from_this(), shape.get())};
    shape = nullptr;
    _releaser.release(retired);
}

void KdTreePool::recycle(osg::ref_ptr<osg::Shape>& shape)
{
    OE_PROFILING_ZONE_NAMED("Recycle terrain KdTree");
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
    PERFORMANCEAPI_INSTRUMENT("Recycle terrain KdTree");
#endif
    auto* tree = dynamic_cast<osg::KdTree*>(shape.get());
    if (!tree || typeid(*tree) != typeid(osg::KdTree) || tree->referenceCount() != 1 || tree->getObserverSet() ||
        tree->getUserDataContainer() || !tree->getName().empty() || !tree->getVertices() ||
        tree->getVertices()->referenceCount() != 1 || tree->getVertices()->getObserverSet())
        return;
    const auto size = bytes(*tree);
    std::lock_guard<std::mutex> lock(_mutex);
    if (size > treeBudget - _treeBytes) return;
    for (auto& slot : _trees)
    {
        if (!slot)
        {
            slot = tree;
            shape = nullptr;
            _treeBytes += size;
            return;
        }
    }
}

void KdTreePool::recycle(osg::ref_ptr<Workspace>& workspace)
{
    const auto size = workspace->bytes();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (size <= workspaceBudget - _workspaceBytes)
        {
            for (auto& slot : _workspaces)
            {
                if (!slot)
                {
                    slot.swap(workspace);
                    _workspaceBytes += size;
                    return;
                }
            }
        }
    }
    ObjectReleaser::Objects retired{workspace.get()};
    workspace = nullptr;
    _releaser.release(retired);
}
