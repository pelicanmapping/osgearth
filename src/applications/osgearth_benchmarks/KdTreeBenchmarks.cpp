/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <benchmark/benchmark.h>

#include <osgEarth/KdTreeBuilder>

#include <osg/Geometry>
#include <osg/KdTree>

namespace
{
    // terrain-like grid of indexed triangles, dim x dim vertices,
    // 2*(dim-1)^2 triangles.
    osg::ref_ptr<osg::Geometry> makeGrid(unsigned dim)
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array();
        verts->reserve(dim * dim);
        for (unsigned r = 0; r < dim; ++r)
        {
            for (unsigned c = 0; c < dim; ++c)
            {
                float z = sinf((float)r * 0.35f) * cosf((float)c * 0.25f) * 3.0f;
                verts->push_back(osg::Vec3((float)c, (float)r, z));
            }
        }
        geom->setVertexArray(verts.get());

        osg::ref_ptr<osg::DrawElementsUInt> de = new osg::DrawElementsUInt(GL_TRIANGLES);
        de->reserve((dim - 1) * (dim - 1) * 6);
        for (unsigned r = 0; r < dim - 1; ++r)
        {
            for (unsigned c = 0; c < dim - 1; ++c)
            {
                unsigned i0 = r * dim + c, i1 = i0 + 1, i2 = i0 + dim, i3 = i2 + 1;
                de->push_back(i0); de->push_back(i1); de->push_back(i2);
                de->push_back(i1); de->push_back(i3); de->push_back(i2);
            }
        }
        geom->addPrimitiveSet(de.get());

        // prime the cached bounding box so the benchmarks time only the kdtree build
        geom->getBoundingBox();
        return geom;
    }

    size_t capacityBytes(const osg::KdTree& t)
    {
        return
            t.getPrimitiveIndices().capacity() * sizeof(unsigned int) +
            t.getVertexIndices().capacity() * sizeof(unsigned int) +
            t.getNodes().capacity() * sizeof(osg::KdTree::KdNode);
    }

    size_t usedBytes(const osg::KdTree& t)
    {
        return
            t.getPrimitiveIndices().size() * sizeof(unsigned int) +
            t.getVertexIndices().size() * sizeof(unsigned int) +
            t.getNodes().size() * sizeof(osg::KdTree::KdNode);
    }

    template<typename T>
    void trimVector(std::vector<T>& v)
    {
        if (v.capacity() > v.size())
        {
            std::vector<T>(v.begin(), v.end()).swap(v);
        }
    }

    void setMemoryCounters(benchmark::State& state, const osg::KdTree& t, double numTris)
    {
        state.counters["alloc_MB"] = (double)capacityBytes(t) / (1024.0 * 1024.0);
        state.counters["used_MB"] = (double)usedBytes(t) / (1024.0 * 1024.0);
        state.counters["tris/s"] = benchmark::Counter(numTris, benchmark::Counter::kIsIterationInvariantRate);
    }
}

// The stock osg::KdTree builder, as-is (no capacity trimming).
static void BM_KdTreeBuild_OSG(benchmark::State& state)
{
    osg::ref_ptr<osg::Geometry> geom = makeGrid((unsigned)state.range(0));
    double numTris = 2.0 * (state.range(0) - 1) * (state.range(0) - 1);

    osg::ref_ptr<osg::KdTree> last;
    for (auto _ : state)
    {
        osg::ref_ptr<osg::KdTree> kdTree = new osg::KdTree();
        osg::KdTree::BuildOptions options;
        bool ok = kdTree->build(options, geom.get());
        benchmark::DoNotOptimize(ok);
        last = kdTree;
    }
    setMemoryCounters(state, *last, numTris);
}
BENCHMARK(BM_KdTreeBuild_OSG)->Arg(33)->Arg(129)->Arg(513)->Arg(1025)->Unit(benchmark::kMillisecond);

// The current osgEarth production path: stock builder followed by the
// trimKdTrees capacity fix-up.
static void BM_KdTreeBuild_OSG_Trimmed(benchmark::State& state)
{
    osg::ref_ptr<osg::Geometry> geom = makeGrid((unsigned)state.range(0));
    double numTris = 2.0 * (state.range(0) - 1) * (state.range(0) - 1);

    osg::ref_ptr<osg::KdTree> last;
    for (auto _ : state)
    {
        osg::ref_ptr<osg::KdTree> kdTree = new osg::KdTree();
        osg::KdTree::BuildOptions options;
        bool ok = kdTree->build(options, geom.get());
        benchmark::DoNotOptimize(ok);
        trimVector(kdTree->getPrimitiveIndices());
        trimVector(kdTree->getVertexIndices());
        trimVector(kdTree->getNodes());
        last = kdTree;
    }
    setMemoryCounters(state, *last, numTris);
}
BENCHMARK(BM_KdTreeBuild_OSG_Trimmed)->Arg(33)->Arg(129)->Arg(513)->Arg(1025)->Unit(benchmark::kMillisecond);

// The osgEarth builder: exact allocations, no trimming needed.
// Second argument = number of build threads.
static void BM_KdTreeBuild_osgEarth(benchmark::State& state)
{
    osg::ref_ptr<osg::Geometry> geom = makeGrid((unsigned)state.range(0));
    double numTris = 2.0 * (state.range(0) - 1) * (state.range(0) - 1);
    unsigned numThreads = (unsigned)state.range(1);

    osg::ref_ptr<osg::KdTree> last;
    for (auto _ : state)
    {
        osg::ref_ptr<osg::KdTree> kdTree = new osg::KdTree();
        osg::KdTree::BuildOptions options;
        bool ok = osgEarth::Util::KdTreeBuilder::build(*kdTree, options, geom.get(), numThreads);
        benchmark::DoNotOptimize(ok);
        last = kdTree;
    }
    setMemoryCounters(state, *last, numTris);
}
BENCHMARK(BM_KdTreeBuild_osgEarth)
    ->Args({ 33, 1 })->Args({ 129, 1 })->Args({ 513, 1 })->Args({ 1025, 1 })
    ->Args({ 129, 4 })->Args({ 513, 4 })->Args({ 1025, 4 })
    ->Args({ 129, 8 })->Args({ 513, 8 })->Args({ 1025, 8 })
    ->Unit(benchmark::kMillisecond);
