/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*
* The subdivision algorithm mirrors osg::KdTree::build() from OpenSceneGraph
* (Copyright (C) 1998-2006 Robert Osfield, OSGPL) so that the two builders
* produce identical trees; the implementation here restructures the data
* flow for speed, allocates exact-size memory, and can subdivide large
* geometries on multiple threads while still producing the identical tree.
*/
#include "KdTreeBuilder"

#include <osg/TemplatePrimitiveIndexFunctor>

#include <atomic>
#include <cfloat>
#include <climits>
#include <cstdlib>
#include <thread>
#include <typeinfo>
#include <vector>

using namespace osgEarth;
using namespace osgEarth::Util;

namespace
{
    // Geometries below this many primitives are always built single-threaded.
    const size_t MIN_PRIMS_FOR_PARALLEL_BUILD = 16384;

    // Minimum primitives before the collection phase runs in parallel.
    const size_t MIN_PRIMS_FOR_PARALLEL_COLLECT = 32768;

    //------------------------------------------------------------------
    // Pass 1: count the primitives a geometry decomposes into so every
    // allocation can be made at its exact final size. Never reads vertex
    // data, so it optimizes down to simple arithmetic over the index counts.
    struct PrimitiveCounter
    {
        unsigned int _numPrimitives = 0u;
        size_t _numVertexIndexEntries = 0u;

        inline void operator()(unsigned int)
        {
            ++_numPrimitives;
            _numVertexIndexEntries += 3u; // originalIndex, count, p0
        }
        inline void operator()(unsigned int, unsigned int)
        {
            ++_numPrimitives;
            _numVertexIndexEntries += 4u;
        }
        inline void operator()(unsigned int, unsigned int, unsigned int)
        {
            ++_numPrimitives;
            _numVertexIndexEntries += 5u;
        }
        inline void operator()(unsigned int, unsigned int, unsigned int, unsigned int)
        {
            ++_numPrimitives;
            _numVertexIndexEntries += 6u;
        }
    };

    //------------------------------------------------------------------
    // Per-primitive working record: the primitive's bounding box plus its
    // offset into the kdtree's vertex index array. The kd subdivision
    // partitions these records in place, so both the partition and the
    // leaf-bound computation run over sequential memory - unlike
    // osg::KdTree::build() which partitions an index array pointing into a
    // separate center array and recomputes leaf bounds by re-walking the
    // vertex indices.
    //
    // Bounds use the exact per-component comparison semantics of
    // osg::BoundingBox::expandBy() so the resulting tree matches OSG's
    // bit for bit.
    struct PrimRecord
    {
        float _min[3];
        float _max[3];
        unsigned int _offset;

        PrimRecord() {} // intentionally uninitialized so resize() does not zero-fill

        inline void init()
        {
            _min[0] = _min[1] = _min[2] = FLT_MAX;
            _max[0] = _max[1] = _max[2] = -FLT_MAX;
        }

        inline void expandBy(const osg::Vec3& v)
        {
            if (v.x() < _min[0]) _min[0] = v.x();
            if (v.x() > _max[0]) _max[0] = v.x();
            if (v.y() < _min[1]) _min[1] = v.y();
            if (v.y() > _max[1]) _max[1] = v.y();
            if (v.z() < _min[2]) _min[2] = v.z();
            if (v.z() > _max[2]) _max[2] = v.z();
        }

        inline float center(int axis) const
        {
            return (_min[axis] + _max[axis]) * 0.5f;
        }
    };

    //------------------------------------------------------------------
    // Pass 2 (sequential flavor): emit the packed vertex-index data and the
    // PrimRecords. Degenerate primitive rejection matches
    // osg::KdTree::build() exactly.
    struct PrimitiveCollector
    {
        osg::KdTree::Indices* _vertexIndices = nullptr;
        std::vector<PrimRecord>* _prims = nullptr;
        const osg::Vec3* _verts = nullptr;

        // running count of all primitives processed, including degenerates;
        // this is the "original primitive index" that intersection functors report.
        unsigned int _primitiveIndex = 0u;

        inline void operator()(unsigned int p0)
        {
            PrimRecord r;
            r.init();
            r.expandBy(_verts[p0]);
            r._offset = static_cast<unsigned int>(_vertexIndices->size());
            _vertexIndices->push_back(_primitiveIndex++);
            _vertexIndices->push_back(1u);
            _vertexIndices->push_back(p0);
            _prims->push_back(r);
        }

        inline void operator()(unsigned int p0, unsigned int p1)
        {
            const osg::Vec3& v0 = _verts[p0];
            const osg::Vec3& v1 = _verts[p1];

            if (v0 == v1)
            {
                ++_primitiveIndex;
                return;
            }

            PrimRecord r;
            r.init();
            r.expandBy(v0);
            r.expandBy(v1);
            r._offset = static_cast<unsigned int>(_vertexIndices->size());
            _vertexIndices->push_back(_primitiveIndex++);
            _vertexIndices->push_back(2u);
            _vertexIndices->push_back(p0);
            _vertexIndices->push_back(p1);
            _prims->push_back(r);
        }

        inline void operator()(unsigned int p0, unsigned int p1, unsigned int p2)
        {
            const osg::Vec3& v0 = _verts[p0];
            const osg::Vec3& v1 = _verts[p1];
            const osg::Vec3& v2 = _verts[p2];

            if (v0 == v1 || v1 == v2 || v2 == v0)
            {
                ++_primitiveIndex;
                return;
            }

            PrimRecord r;
            r.init();
            r.expandBy(v0);
            r.expandBy(v1);
            r.expandBy(v2);
            r._offset = static_cast<unsigned int>(_vertexIndices->size());
            _vertexIndices->push_back(_primitiveIndex++);
            _vertexIndices->push_back(3u);
            _vertexIndices->push_back(p0);
            _vertexIndices->push_back(p1);
            _vertexIndices->push_back(p2);
            _prims->push_back(r);
        }

        inline void operator()(unsigned int p0, unsigned int p1, unsigned int p2, unsigned int p3)
        {
            const osg::Vec3& v0 = _verts[p0];
            const osg::Vec3& v1 = _verts[p1];
            const osg::Vec3& v2 = _verts[p2];
            const osg::Vec3& v3 = _verts[p3];

            if (v0 == v1 || v1 == v2 || v2 == v0 || v3 == v0 || v3 == v1 || v3 == v2)
            {
                ++_primitiveIndex;
                return;
            }

            PrimRecord r;
            r.init();
            r.expandBy(v0);
            r.expandBy(v1);
            r.expandBy(v2);
            r.expandBy(v3);
            r._offset = static_cast<unsigned int>(_vertexIndices->size());
            _vertexIndices->push_back(_primitiveIndex++);
            _vertexIndices->push_back(4u);
            _vertexIndices->push_back(p0);
            _vertexIndices->push_back(p1);
            _vertexIndices->push_back(p2);
            _vertexIndices->push_back(p3);
            _prims->push_back(r);
        }
    };

    //------------------------------------------------------------------
    // Pass 2 (parallel flavor): decompose primitive-set ranges directly so
    // independent chunks can be emitted concurrently at precomputed offsets.
    // The decomposition mirrors osg::TemplatePrimitiveIndexFunctor. Only
    // valid when the geometry has no degenerate primitives (each primitive's
    // output offset must be computable from its ordinal alone); a degenerate
    // aborts the parallel pass and the sequential collector runs instead.

    // arity of one packed primitive in the kdtree's vertex index array
    inline int entriesForMode(GLenum mode)
    {
        switch (mode)
        {
        case GL_POINTS: return 3;
        case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP: return 4;
        case GL_TRIANGLES: case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN: case GL_POLYGON: return 5;
        case GL_QUADS: case GL_QUAD_STRIP: return 6;
        default: return 0;
        }
    }

    inline int primCountForMode(GLenum mode, int count)
    {
        switch (mode)
        {
        case GL_POINTS: return count;
        case GL_LINES: return count >= 2 ? count / 2 : 0;
        case GL_LINE_STRIP: return count >= 2 ? count - 1 : 0;
        case GL_LINE_LOOP: return count >= 2 ? count : 0;
        case GL_TRIANGLES: return count >= 3 ? count / 3 : 0;
        case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN: case GL_POLYGON: return count >= 3 ? count - 2 : 0;
        case GL_QUADS: return count >= 4 ? count / 4 : 0;
        case GL_QUAD_STRIP: return count >= 4 ? (count - 2) / 2 : 0;
        default: return 0;
        }
    }

    // source vertex indices of primitive k. idx == nullptr means drawArrays
    // (identity mapping offset by "first").
    template<typename Index>
    inline int primSourceIndices(GLenum mode, int first, int count, const Index* idx, int k, unsigned int out[4])
    {
        auto fetch = [&](int i) -> unsigned int {
            return idx ? (unsigned int)idx[i] : (unsigned int)(first + i);
        };
        switch (mode)
        {
        case GL_POINTS:
            out[0] = fetch(k);
            return 1;
        case GL_LINES:
            out[0] = fetch(2 * k); out[1] = fetch(2 * k + 1);
            return 2;
        case GL_LINE_STRIP:
            out[0] = fetch(k); out[1] = fetch(k + 1);
            return 2;
        case GL_LINE_LOOP:
            if (k == count - 1) { out[0] = fetch(count - 1); out[1] = fetch(0); }
            else { out[0] = fetch(k); out[1] = fetch(k + 1); }
            return 2;
        case GL_TRIANGLES:
            out[0] = fetch(3 * k); out[1] = fetch(3 * k + 1); out[2] = fetch(3 * k + 2);
            return 3;
        case GL_TRIANGLE_STRIP:
            // odd primitives flip the winding, mirroring osg's functor
            if (k % 2 != 0) { out[0] = fetch(k); out[1] = fetch(k + 2); out[2] = fetch(k + 1); }
            else { out[0] = fetch(k); out[1] = fetch(k + 1); out[2] = fetch(k + 2); }
            return 3;
        case GL_TRIANGLE_FAN:
        case GL_POLYGON:
            out[0] = fetch(0); out[1] = fetch(k + 1); out[2] = fetch(k + 2);
            return 3;
        case GL_QUADS:
            out[0] = fetch(4 * k); out[1] = fetch(4 * k + 1); out[2] = fetch(4 * k + 2); out[3] = fetch(4 * k + 3);
            return 4;
        case GL_QUAD_STRIP:
            out[0] = fetch(2 * k); out[1] = fetch(2 * k + 1); out[2] = fetch(2 * k + 2); out[3] = fetch(2 * k + 3);
            return 4;
        default:
            return 0;
        }
    }

    struct SetInfo
    {
        GLenum mode;
        int first;               // drawArrays only
        int count;               // index count (elements) or vertex count (arrays)
        const void* indices;     // nullptr for drawArrays
        int indexSize;           // 1, 2, 4 bytes
        int numPrims;
        int entriesPerPrim;
        // running totals at the start of this set:
        unsigned int ordinalBase;
        size_t vtxBase;
        size_t primBase;
    };

    // returns false if the geometry contains primitive-set types the range
    // decomposer does not handle; the caller uses the sequential path then.
    bool analyzePrimitiveSets(osg::Geometry* geometry, std::vector<SetInfo>& sets,
        size_t& totalVtx, size_t& totalPrims)
    {
        unsigned int totalOrdinals = 0;
        totalVtx = 0;
        totalPrims = 0;

        for (unsigned int s = 0; s < geometry->getNumPrimitiveSets(); ++s)
        {
            const osg::PrimitiveSet* ps = geometry->getPrimitiveSet(s);
            if (!ps) continue;

            SetInfo info;
            info.mode = ps->getMode();
            info.entriesPerPrim = entriesForMode(info.mode);

            switch (ps->getType())
            {
            case osg::PrimitiveSet::DrawArraysPrimitiveType:
            {
                const osg::DrawArrays* da = static_cast<const osg::DrawArrays*>(ps);
                info.first = da->getFirst();
                info.count = da->getCount();
                info.indices = nullptr;
                info.indexSize = 0;
                break;
            }
            case osg::PrimitiveSet::DrawElementsUBytePrimitiveType:
            {
                const osg::DrawElementsUByte* de = static_cast<const osg::DrawElementsUByte*>(ps);
                info.first = 0;
                info.count = de->empty() ? 0 : (int)de->size();
                info.indices = de->empty() ? nullptr : &de->front();
                info.indexSize = 1;
                break;
            }
            case osg::PrimitiveSet::DrawElementsUShortPrimitiveType:
            {
                const osg::DrawElementsUShort* de = static_cast<const osg::DrawElementsUShort*>(ps);
                info.first = 0;
                info.count = de->empty() ? 0 : (int)de->size();
                info.indices = de->empty() ? nullptr : &de->front();
                info.indexSize = 2;
                break;
            }
            case osg::PrimitiveSet::DrawElementsUIntPrimitiveType:
            {
                const osg::DrawElementsUInt* de = static_cast<const osg::DrawElementsUInt*>(ps);
                info.first = 0;
                info.count = de->empty() ? 0 : (int)de->size();
                info.indices = de->empty() ? nullptr : &de->front();
                info.indexSize = 4;
                break;
            }
            default:
                return false; // DrawArrayLengths etc: use the sequential path
            }

            // unsupported modes contribute nothing, same as osg's functor
            info.numPrims = info.entriesPerPrim > 0 ? primCountForMode(info.mode, info.count) : 0;

            info.ordinalBase = totalOrdinals;
            info.vtxBase = totalVtx;
            info.primBase = totalPrims;
            totalOrdinals += info.numPrims;
            totalVtx += (size_t)info.numPrims * info.entriesPerPrim;
            totalPrims += info.numPrims;
            sets.push_back(info);
        }
        return true;
    }

    // emit primitives [k0,k1) of one set. returns false as soon as a
    // degenerate primitive is found.
    bool emitRange(const SetInfo& si, const osg::Vec3* verts, int k0, int k1,
        unsigned int* vtxOut, PrimRecord* primOut)
    {
        unsigned int* vp = vtxOut + (size_t)k0 * si.entriesPerPrim;
        PrimRecord* pp = primOut + k0;
        unsigned int src[4];

        for (int k = k0; k < k1; ++k)
        {
            int nv;
            switch (si.indexSize)
            {
            case 0: nv = primSourceIndices<unsigned int>(si.mode, si.first, si.count, nullptr, k, src); break;
            case 1: nv = primSourceIndices(si.mode, si.first, si.count, (const GLubyte*)si.indices, k, src); break;
            case 2: nv = primSourceIndices(si.mode, si.first, si.count, (const GLushort*)si.indices, k, src); break;
            default: nv = primSourceIndices(si.mode, si.first, si.count, (const GLuint*)si.indices, k, src); break;
            }

            PrimRecord r;
            r.init();
            bool degenerate = false;
            switch (nv)
            {
            case 1:
                r.expandBy(verts[src[0]]);
                break;
            case 2:
            {
                const osg::Vec3& v0 = verts[src[0]];
                const osg::Vec3& v1 = verts[src[1]];
                degenerate = (v0 == v1);
                if (!degenerate) { r.expandBy(v0); r.expandBy(v1); }
                break;
            }
            case 3:
            {
                const osg::Vec3& v0 = verts[src[0]];
                const osg::Vec3& v1 = verts[src[1]];
                const osg::Vec3& v2 = verts[src[2]];
                degenerate = (v0 == v1 || v1 == v2 || v2 == v0);
                if (!degenerate) { r.expandBy(v0); r.expandBy(v1); r.expandBy(v2); }
                break;
            }
            default:
            {
                const osg::Vec3& v0 = verts[src[0]];
                const osg::Vec3& v1 = verts[src[1]];
                const osg::Vec3& v2 = verts[src[2]];
                const osg::Vec3& v3 = verts[src[3]];
                degenerate = (v0 == v1 || v1 == v2 || v2 == v0 || v3 == v0 || v3 == v1 || v3 == v2);
                if (!degenerate) { r.expandBy(v0); r.expandBy(v1); r.expandBy(v2); r.expandBy(v3); }
                break;
            }
            }
            if (degenerate) return false;

            r._offset = (unsigned int)(si.vtxBase + (size_t)k * si.entriesPerPrim);
            *vp++ = si.ordinalBase + (unsigned int)k;
            *vp++ = (unsigned int)nv;
            for (int i = 0; i < nv; ++i) *vp++ = src[i];
            *pp++ = r;
        }
        return true;
    }

    //------------------------------------------------------------------
    // subdivision task: an independent subtree build.
    struct Task
    {
        int tempRoot = -1;                   // node index in the owner's node list
        osg::BoundingBox bb;                 // spatial box at task entry
        unsigned int level = 0;

        // results:
        // localNodes[0] is an unused sentinel (so "child index 0" still means
        // "no child" in the local recursion), localNodes[1] is the working
        // copy of the task root, and descendants follow from index 2.
        osg::KdTree::KdNodeList localNodes;
        osg::KdTree::KdNode rootResult;
        int finalBase = 0;                   // final index of the first descendant
    };

    void computeAxisStack(const osg::BoundingBox& bb, unsigned int maxNumLevels, std::vector<int>& axisStack)
    {
        osg::Vec3 dimensions(
            bb.xMax() - bb.xMin(),
            bb.yMax() - bb.yMin(),
            bb.zMax() - bb.zMin());

        axisStack.reserve(maxNumLevels);
        for (unsigned int level = 0; level < maxNumLevels; ++level)
        {
            int axis = 0;
            if (dimensions[0] >= dimensions[1])
            {
                if (dimensions[0] >= dimensions[2]) axis = 0;
                else axis = 2;
            }
            else if (dimensions[1] >= dimensions[2]) axis = 1;
            else axis = 2;

            axisStack.push_back(axis);
            dimensions[axis] /= 2.0f;
        }
    }

    // Recursive midpoint subdivision, mirroring BuildKdTree::divide() in
    // OSG's KdTree.cpp step for step (same partitioning, same in-situ
    // division handling, same leaf epsilon) so it produces an identical
    // node list. Optionally queues sub-ranges below _taskThreshold as
    // tasks instead of recursing into them.
    struct Subdivider
    {
        std::vector<PrimRecord>& _prims;
        osg::KdTree::KdNodeList& _nodes;
        const std::vector<int>& _axisStack;
        unsigned int _targetNumTrianglesPerLeaf;

        // osg's algorithm uses node index 0 both for the root and as the
        // "no child" sentinel, so an in-situ division AT THE GLOBAL ROOT
        // stops subdividing. _rootQuirkIndex is the node index that must
        // reproduce that quirk (the global root), or -1.
        int _rootQuirkIndex = -1;

        // when set, sub-ranges of at most this many primitives are queued
        // on _tasks instead of being subdivided here.
        int _taskThreshold = INT_MAX;
        std::vector<Task>* _tasks = nullptr;
        std::vector<int>* _taskOfNode = nullptr; // node index -> task id or -1

        Subdivider(std::vector<PrimRecord>& prims, osg::KdTree::KdNodeList& nodes,
            const std::vector<int>& axisStack, unsigned int target) :
            _prims(prims), _nodes(nodes), _axisStack(axisStack), _targetNumTrianglesPerLeaf(target)
        {
        }

        int addNode(const osg::KdTree::KdNode& node)
        {
            int num = static_cast<int>(_nodes.size());
            _nodes.push_back(node);
            if (_taskOfNode) _taskOfNode->push_back(-1);
            return num;
        }

        void computeLeafBounds(osg::KdTree::KdNode& node)
        {
            int istart = -node.first - 1;
            int iend = istart + node.second - 1;

            // the leaf bound is the union of its primitives' bounds, which
            // equals the bound over their vertices.
            node.bb.init();
            for (int i = istart; i <= iend; ++i)
            {
                const PrimRecord& p = _prims[i];
                node.bb.expandBy(osg::Vec3(p._min[0], p._min[1], p._min[2]));
                node.bb.expandBy(osg::Vec3(p._max[0], p._max[1], p._max[2]));
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

        int divide(osg::BoundingBox& bb, int nodeIndex, unsigned int level)
        {
            // copy the node header; _nodes may reallocate during recursion.
            int nodeFirst = _nodes[nodeIndex].first;
            int nodeSecond = _nodes[nodeIndex].second;

            if (_tasks && nodeFirst < 0 && nodeSecond <= _taskThreshold)
            {
                Task t;
                t.tempRoot = nodeIndex;
                t.bb = bb;
                t.level = level;
                (*_taskOfNode)[nodeIndex] = (int)_tasks->size();
                _tasks->push_back(std::move(t));
                return nodeIndex;
            }

            bool needToDivide =
                level < _axisStack.size() &&
                (nodeFirst < 0 && static_cast<unsigned int>(nodeSecond) > _targetNumTrianglesPerLeaf);

            if (!needToDivide)
            {
                if (nodeFirst < 0)
                {
                    computeLeafBounds(_nodes[nodeIndex]);
                }
                return nodeIndex;
            }

            int axis = _axisStack[level];

            if (nodeFirst < 0)
            {
                int istart = -nodeFirst - 1;
                int iend = istart + nodeSecond - 1;

                float mid = (bb._min[axis] + bb._max[axis]) * 0.5f;

                int originalLeftChildIndex = 0;
                int originalRightChildIndex = 0;
                bool insitueDivision = false;

                {
                    int left = istart;
                    int right = iend;

                    while (left < right)
                    {
                        while (left < right && (_prims[left].center(axis) <= mid)) { ++left; }
                        while (left < right && (_prims[right].center(axis) > mid)) { --right; }
                        if (left < right)
                        {
                            std::swap(_prims[left], _prims[right]);
                            ++left;
                            --right;
                        }
                    }

                    if (left == right)
                    {
                        if (_prims[left].center(axis) <= mid) ++left;
                        else --right;
                    }

                    osg::KdTree::KdNode leftLeaf(-istart - 1, (right - istart) + 1);
                    osg::KdTree::KdNode rightLeaf(-left - 1, (iend - left) + 1);

                    // in-situ: reuse this node. at the global root, "reuse"
                    // collides with the 0 == "no child" sentinel and stops
                    // the subdivision, exactly like osg.
                    int selfIndex = (nodeIndex == _rootQuirkIndex) ? 0 : nodeIndex;

                    if (leftLeaf.second <= 0)
                    {
                        originalLeftChildIndex = 0;
                        originalRightChildIndex = selfIndex;
                        insitueDivision = true;
                    }
                    else if (rightLeaf.second <= 0)
                    {
                        originalLeftChildIndex = selfIndex;
                        originalRightChildIndex = 0;
                        insitueDivision = true;
                    }
                    else
                    {
                        originalLeftChildIndex = addNode(leftLeaf);
                        originalRightChildIndex = addNode(rightLeaf);
                    }
                }

                float restore = bb._max[axis];
                bb._max[axis] = mid;
                int leftChildIndex = originalLeftChildIndex != 0 ? divide(bb, originalLeftChildIndex, level + 1) : 0;
                bb._max[axis] = restore;

                restore = bb._min[axis];
                bb._min[axis] = mid;
                int rightChildIndex = originalRightChildIndex != 0 ? divide(bb, originalRightChildIndex, level + 1) : 0;
                bb._min[axis] = restore;

                if (!insitueDivision)
                {
                    osg::KdTree::KdNode& node = _nodes[nodeIndex];
                    node.first = leftChildIndex;
                    node.second = rightChildIndex;

                    node.bb.init();
                    if (leftChildIndex != 0) node.bb.expandBy(_nodes[leftChildIndex].bb);
                    if (rightChildIndex != 0) node.bb.expandBy(_nodes[rightChildIndex].bb);
                }
            }

            return nodeIndex;
        }
    };

    void runTask(Task& t, std::vector<PrimRecord>& prims, const osg::KdTree::KdNode& rootState,
        const std::vector<int>& axisStack, unsigned int target, bool taskRootIsGlobalRoot)
    {
        int count = rootState.second;
        unsigned int t2 = target > 0u ? target : 1u;
        t.localNodes.reserve(2u * ((unsigned int)count / t2) + 3u);
        t.localNodes.push_back(osg::KdTree::KdNode()); // sentinel
        t.localNodes.push_back(rootState);             // task root at local index 1

        Subdivider sub(prims, t.localNodes, axisStack, target);
        sub._rootQuirkIndex = taskRootIsGlobalRoot ? 1 : -1;
        osg::BoundingBox bb = t.bb;
        sub.divide(bb, 1, t.level);

        t.rootResult = t.localNodes[1];
    }

    template<typename T>
    inline void trimToExactSize(std::vector<T>& v)
    {
        if (v.capacity() > v.size())
        {
            std::vector<T>(v.begin(), v.end()).swap(v);
        }
    }

    // run "worker" on this thread plus (numThreads-1) helpers.
    template<typename F>
    void runParallel(unsigned int numThreads, F&& worker)
    {
        unsigned int helpers = numThreads > 1u ? numThreads - 1u : 0u;
        std::vector<std::thread> pool;
        pool.reserve(helpers);
        for (unsigned int i = 0; i < helpers; ++i) pool.emplace_back(worker);
        worker();
        for (auto& th : pool) th.join();
    }

    unsigned int defaultNumThreads()
    {
        const char* env = ::getenv("OSGEARTH_KDTREE_BUILDER_THREADS");
        if (env)
        {
            int n = ::atoi(env);
            return n > 0 ? (unsigned int)n : 1u;
        }
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0u) hw = 1u;
        return std::min(4u, hw);
    }
}

bool
KdTreeBuilder::build(osg::KdTree& kdTree, osg::KdTree::BuildOptions& options, osg::Geometry* geometry, unsigned int numThreads)
{
    osg::Vec3Array* vertices = dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray());
    if (!vertices) return false;

    if (vertices->size() <= options._targetNumTrianglesPerLeaf) return false;

    osg::BoundingBox geometryBB = geometry->getBoundingBox();

    kdTree.setVertices(vertices);
    options._numVerticesProcessed += vertices->size();

    if (numThreads == 0u) numThreads = defaultNumThreads();

    const osg::Vec3* verts = vertices->empty() ? nullptr : &vertices->front();

    //------------------------------------------------------------------
    // collection: pack the primitives into the kdtree's vertex index array
    // and build one PrimRecord per (non-degenerate) primitive.

    osg::KdTree::Indices& vertexIndices = kdTree.getVertexIndices();
    std::vector<PrimRecord> prims;

    bool parallelCollected = false;
    std::vector<SetInfo> sets;
    size_t totalVtx = 0, totalPrims = 0;

    if (numThreads > 1u &&
        analyzePrimitiveSets(geometry, sets, totalVtx, totalPrims) &&
        totalPrims >= MIN_PRIMS_FOR_PARALLEL_COLLECT)
    {
        // the parallel pass computes each primitive's output position from
        // its ordinal, which is only correct when nothing gets discarded;
        // a degenerate primitive aborts it and the sequential pass runs.
        vertexIndices.resize(totalVtx);
        prims.resize(totalPrims);

        struct Chunk { const SetInfo* si; int k0, k1; };
        std::vector<Chunk> chunks;
        int chunkPrims = (int)(totalPrims / ((size_t)numThreads * 8u) + 1u);
        if (chunkPrims < 4096) chunkPrims = 4096;
        for (auto& si : sets)
            for (int k = 0; k < si.numPrims; k += chunkPrims)
                chunks.push_back({ &si, k, std::min(k + chunkPrims, si.numPrims) });

        std::atomic<bool> degenerate{ false };
        std::atomic<size_t> nextChunk{ 0 };

        runParallel(numThreads, [&]() {
            for (;;)
            {
                if (degenerate.load(std::memory_order_relaxed)) break;
                size_t c = nextChunk++;
                if (c >= chunks.size()) break;
                const Chunk& ch = chunks[c];
                if (!emitRange(*ch.si, verts, ch.k0, ch.k1,
                    vertexIndices.data() + ch.si->vtxBase, prims.data() + ch.si->primBase))
                {
                    degenerate.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        });

        if (!degenerate.load())
        {
            parallelCollected = true;
        }
        else
        {
            vertexIndices.clear();
            prims.clear();
        }
    }

    if (!parallelCollected)
    {
        osg::TemplatePrimitiveIndexFunctor<PrimitiveCounter> counter;
        geometry->accept(counter);

        vertexIndices.reserve(counter._numVertexIndexEntries);
        prims.reserve(counter._numPrimitives);

        osg::TemplatePrimitiveIndexFunctor<PrimitiveCollector> collector;
        collector._vertexIndices = &vertexIndices;
        collector._prims = &prims;
        collector._verts = verts;
        geometry->accept(collector);

        kdTree._degenerateCount += collector._primitiveIndex - static_cast<unsigned int>(prims.size());

        if (vertexIndices.capacity() > vertexIndices.size())
        {
            trimToExactSize(vertexIndices);
        }
    }

    //------------------------------------------------------------------
    // subdivision

    unsigned int target = options._targetNumTrianglesPerLeaf > 0u ? options._targetNumTrianglesPerLeaf : 1u;

    std::vector<int> axisStack;
    computeAxisStack(geometryBB, options._maxNumLevels, axisStack);

    osg::KdTree::KdNodeList& nodes = kdTree.getNodes();

    if (numThreads <= 1u || prims.size() < MIN_PRIMS_FOR_PARALLEL_BUILD)
    {
        // single-threaded: subdivide directly into the kdtree's node list.
        nodes.reserve(2u * (static_cast<unsigned int>(prims.size()) / target) + 1u);

        Subdivider subdivider(prims, nodes, axisStack, options._targetNumTrianglesPerLeaf);
        subdivider._rootQuirkIndex = 0;

        osg::KdTree::KdNode root(-1, static_cast<osg::KdTree::value_type>(prims.size()));
        root.bb = geometryBB;
        int rootIndex = subdivider.addNode(root);

        osg::BoundingBox bb = geometryBB;
        subdivider.divide(bb, rootIndex, 0);

        trimToExactSize(nodes);

        // the subdivision partitioned the primitive records in place, so the
        // final primitive index list is just each record's offset.
        osg::KdTree::Indices& primitiveIndices = kdTree.getPrimitiveIndices();
        primitiveIndices.resize(prims.size());
        for (size_t i = 0; i < prims.size(); ++i)
        {
            primitiveIndices[i] = prims[i]._offset;
        }

        return !nodes.empty();
    }

    // multi-threaded: subdivide the top of the tree on this thread until the
    // sub-ranges drop below the task threshold, build those subtrees in
    // parallel into local node lists, then renumber everything into the
    // exact node order the sequential algorithm produces.

    std::vector<Task> tasks;
    std::vector<int> taskOfNode;
    osg::KdTree::KdNodeList temp;

    Subdivider prefix(prims, temp, axisStack, options._targetNumTrianglesPerLeaf);
    prefix._rootQuirkIndex = 0;
    prefix._taskThreshold = std::max((int)(prims.size() / ((size_t)numThreads * 8u) + 1u), 4096);
    prefix._tasks = &tasks;
    prefix._taskOfNode = &taskOfNode;

    osg::KdTree::KdNode root(-1, static_cast<osg::KdTree::value_type>(prims.size()));
    root.bb = geometryBB;
    int rootIndex = prefix.addNode(root);

    osg::BoundingBox bb = geometryBB;
    prefix.divide(bb, rootIndex, 0);

    // build the subtrees in parallel
    {
        std::atomic<size_t> next{ 0 };
        runParallel(tasks.size() > 1 ? numThreads : 1u, [&]() {
            for (;;)
            {
                size_t i = next++;
                if (i >= tasks.size()) break;
                runTask(tasks[i], prims, temp[tasks[i].tempRoot],
                    axisStack, options._targetNumTrianglesPerLeaf,
                    tasks[i].tempRoot == 0);
            }
        });
    }

    // replay the sequential DFS to assign final node indices: visiting a
    // node numbers its two children, then descends left, then right; a
    // task root's descendants occupy one contiguous block.
    size_t totalNodes = temp.size();
    for (auto& t : tasks) totalNodes += t.localNodes.size() - 2;

    std::vector<int> finalIndex(temp.size(), -1);
    {
        struct Replayer
        {
            const osg::KdTree::KdNodeList& temp;
            const std::vector<int>& taskOfNode;
            std::vector<Task>& tasks;
            std::vector<int>& finalIndex;
            int counter;

            void visit(int tempIdx)
            {
                int taskId = taskOfNode[tempIdx];
                if (taskId >= 0)
                {
                    Task& t = tasks[taskId];
                    t.finalBase = counter;
                    counter += (int)t.localNodes.size() - 2;
                    return;
                }
                const osg::KdTree::KdNode& node = temp[tempIdx];
                if (node.first > 0)
                {
                    finalIndex[node.first] = counter++;
                    finalIndex[node.second] = counter++;
                    visit(node.first);
                    visit(node.second);
                }
            }
        };
        Replayer rp{ temp, taskOfNode, tasks, finalIndex, 0 };
        finalIndex[0] = rp.counter++;
        rp.visit(0);
    }

    nodes.resize(totalNodes);

    osg::KdTree::Indices& primitiveIndices = kdTree.getPrimitiveIndices();
    primitiveIndices.resize(prims.size());

    // copy the task node blocks into place and emit the primitive indices,
    // in parallel; both are pure writes to disjoint ranges.
    {
        std::atomic<size_t> nextTask{ 0 };
        std::atomic<size_t> nextEmit{ 0 };
        const size_t emitChunk = 262144;
        const size_t numPrims = prims.size();
        unsigned int* piData = primitiveIndices.data();
        const PrimRecord* primData = prims.data();

        runParallel(numThreads, [&]() {
            for (;;)
            {
                size_t i = nextTask++;
                if (i >= tasks.size()) break;
                Task& t = tasks[i];
                int base = t.finalBase;
                for (size_t j = 2; j < t.localNodes.size(); ++j)
                {
                    osg::KdTree::KdNode n = t.localNodes[j];
                    if (n.first > 0)
                    {
                        n.first = base + n.first - 2;
                        n.second = base + n.second - 2;
                    }
                    nodes[base + (int)j - 2] = n;
                }
            }
            for (;;)
            {
                size_t c = nextEmit++;
                size_t begin = c * emitChunk;
                if (begin >= numPrims) break;
                size_t end = std::min(begin + emitChunk, numPrims);
                for (size_t i = begin; i < end; ++i) piData[i] = primData[i]._offset;
            }
        });
    }

    // place the prefix nodes (task roots take their task's result)
    for (size_t i = 0; i < temp.size(); ++i)
    {
        int fi = finalIndex[i];
        if (fi < 0) continue;

        int taskId = taskOfNode[i];
        if (taskId >= 0)
        {
            Task& t = tasks[taskId];
            osg::KdTree::KdNode n = t.rootResult;
            if (n.first > 0)
            {
                n.first = t.finalBase + n.first - 2;
                n.second = t.finalBase + n.second - 2;
            }
            nodes[fi] = n;
        }
        else
        {
            osg::KdTree::KdNode n = temp[i];
            if (n.first > 0)
            {
                n.first = finalIndex[n.first];
                n.second = finalIndex[n.second];
            }
            nodes[fi] = n;
        }
    }

    // resolve the prefix's internal bounding boxes; children always have
    // higher temp indices than their parents, so a reverse sweep sees
    // children first. (task subtree bounds are already final.)
    for (size_t i = temp.size(); i-- > 0;)
    {
        int fi = finalIndex[i];
        if (fi < 0 || taskOfNode[i] >= 0) continue;
        osg::KdTree::KdNode& node = nodes[fi];
        if (node.first > 0)
        {
            node.bb.init();
            node.bb.expandBy(nodes[node.first].bb);
            node.bb.expandBy(nodes[node.second].bb);
        }
    }

    return !nodes.empty();
}

KdTreeBuilder::KdTreeBuilder() :
    osg::KdTreeBuilder(),
    _numThreads(0u)
{
}

KdTreeBuilder::KdTreeBuilder(const KdTreeBuilder& rhs) :
    osg::KdTreeBuilder(rhs),
    _numThreads(rhs._numThreads)
{
}

void
KdTreeBuilder::apply(osg::Geometry& geometry)
{
    if (dynamic_cast<osg::KdTree*>(geometry.getShape())) return;

    osg::ref_ptr<osg::KdTree> kdTree = osg::clone(_kdTreePrototype.get());

    if (typeid(*kdTree) != typeid(osg::KdTree))
    {
        // a custom KdTree subclass is installed; honor its virtual build().
        if (kdTree->build(_buildOptions, &geometry))
        {
            geometry.setShape(kdTree.get());
        }
        return;
    }

    if (build(*kdTree, _buildOptions, &geometry, _numThreads))
    {
        geometry.setShape(kdTree.get());
    }
}
