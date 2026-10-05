/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <benchmark/benchmark.h>
#include "../osgearth_tests/ChonkTestUtils.h"
#include "../osgearth_tests/ChonkUniqueTestUtils.h"

using namespace osgEarth;

namespace
{
    // Expose allocation counts without GPU readback in the timed render loop.
    struct UniqueBenchmarkDrawable : ChonkDrawable
    {
        // Return the padded source count after the first frame built GPU tables.
        std::size_t sourceCount(osg::State& state) const
        {
            return GLObjects::get(_globjects, state)._all_instances.size();
        }
    };

    // Compare independent Chonks with subtree extraction using identical unique
    // geometry; validate pixels before measuring completed, warmed viewer frames.
    void uniqueGeometry(benchmark::State& state, bool rip, bool sceneConversion = false)
    {
        if (!Capabilities::get().supportsNVGL())
        { state.SkipWithError("NVGL unavailable; set OSG_GL_CONTEXT_VERSION=4.6"); return; }
        static ChonkTest::Renderer renderer;
        static const bool ready = renderer.initialize();
        if (!ready) { state.SkipWithError("Cannot create GL context"); return; }
        // Match Chonk's zero-SSE policy; ordinary OSG otherwise rejects tiny
        // reference meshes before rasterization in the fully-visible workload.
        renderer.viewer.getCamera()->setCullingMode(
            renderer.viewer.getCamera()->getCullingMode() & ~osg::CullSettings::SMALL_FEATURE_CULLING);
        ChonkTest::UniqueScene source(unsigned(state.range(0)));
        osg::ref_ptr<UniqueBenchmarkDrawable> drawable = new UniqueBenchmarkDrawable();
        drawable->setBirthday(-10.0);
        osg::ref_ptr<osg::Node> converted;
        if (sceneConversion)
            converted = ChonkFactory::convertScene(source.root, renderer.factory);
        else if (rip)
        {
            if (!drawable->add(source.root, *renderer.factory, 0.0f))
            { state.SkipWithError("Ripper failed"); return; }
        }
        else
        {
            for (unsigned i = 0; i < source.meshes.size(); ++i)
            {
                auto chonk = renderer.factory->getOrCreateChonk(source.meshes[i]);
                if (!chonk) { state.SkipWithError("Conversion failed"); return; }
                drawable->add(chonk, source.transforms[i]);
            }
        }
        const double height = source.width * (state.range(1) ? 0.2 : 1.3);
        renderer.viewer.getCamera()->setViewMatrixAsLookAt(
            osg::Vec3d(0,0,height), osg::Vec3d(), osg::Vec3d(0,1,0));
        renderer.setScene(source.root);
        renderer.frame(); renderer.frame();
        auto reference = renderer.pixels();
        renderer.setScene(sceneConversion ? converted.get() : drawable.get());
        for (unsigned i = 0; i < 8; ++i) renderer.frame();
        const auto difference = ChonkTest::differentPixels(*reference, *renderer.pixels());
        if (difference > 30 || glGetError() != GL_NO_ERROR)
        {
            const auto message = "Unique geometry differs from ordinary OSG rendering: " + std::to_string(difference) + " pixels";
            state.SkipWithError(message.c_str()); return;
        }
        state.counters["pixel_differences"] = difference;
        for (auto _ : state) renderer.frame();
        state.counters["meshes"] = source.meshes.size();
        if (sceneConversion)
        {
            ChonkTest::DrawCounts counts;
            converted->accept(counts);
            state.counters["ordinary_drawables"] = counts.ordinary;
            state.counters["chonk_drawables"] = counts.chonks;
            state.counters["batches"] = counts.batches;
        }
        else
        {
            state.counters["sources"] = drawable->sourceCount(*renderer.context->getState());
            state.counters["batches"] = drawable->getNumBatches();
        }
    }

    // Baseline: each unique mesh owns a Chonk, within one multi-draw drawable.
    void BM_ChonkUnique_Separate(benchmark::State& state) { uniqueGeometry(state, false); }
    // Exercise the production subtree Ripper path, including its storage policy.
    void BM_ChonkUnique_Ripper(benchmark::State& state) { uniqueGeometry(state, true); }
    // Exercise the scene converter; toggle pages between separate processes
    // to measure ordinary OSG drawables versus the actual integrated page path.
    void BM_ChonkUnique_Scene(benchmark::State& state) { uniqueGeometry(state, false, true); }
    BENCHMARK(BM_ChonkUnique_Separate)->Args({1000,0})->Args({1000,1})->Args({10000,0})->Args({10000,1})
        ->UseRealTime()->Unit(benchmark::kMillisecond);
    BENCHMARK(BM_ChonkUnique_Ripper)->Args({1000,0})->Args({1000,1})->Args({10000,0})->Args({10000,1})
        ->UseRealTime()->Unit(benchmark::kMillisecond);
    BENCHMARK(BM_ChonkUnique_Scene)->Args({1000,0})->Args({1000,1})->Args({10000,0})->Args({10000,1})
        ->UseRealTime()->Unit(benchmark::kMillisecond);

    // Model independently loaded low-LOD tiles: each conversion receives one
    // mesh, rather than the entire grid. Compare pixels before timing frames.
    void BM_ChonkSingletonTiles(benchmark::State& state)
    {
        if (!Capabilities::get().supportsNVGL())
        { state.SkipWithError("NVGL unavailable; set OSG_GL_CONTEXT_VERSION=4.6"); return; }
        static ChonkTest::Renderer renderer;
        static const bool ready = renderer.initialize();
        if (!ready) { state.SkipWithError("Cannot create GL context"); return; }
        renderer.viewer.getCamera()->setCullingMode(
            renderer.viewer.getCamera()->getCullingMode() & ~osg::CullSettings::SMALL_FEATURE_CULLING);
        ChonkTest::UniqueScene source(unsigned(state.range(0)), 8);
        osg::ref_ptr<osg::Group> converted = new osg::Group();
        for (unsigned i = 0; i < source.root->getNumChildren(); ++i)
            converted->addChild(ChonkFactory::convertScene(source.root->getChild(i), renderer.factory));
        renderer.viewer.getCamera()->setViewMatrixAsLookAt(
            osg::Vec3d(0,0,source.width*1.3), osg::Vec3d(), osg::Vec3d(0,1,0));
        renderer.setScene(source.root); renderer.frame(); renderer.frame();
        auto reference = renderer.pixels();
        renderer.setScene(converted);
        for (unsigned i = 0; i < 8; ++i) renderer.frame();
        const auto difference = ChonkTest::differentPixels(*reference, *renderer.pixels());
        if (difference > 30 || glGetError() != GL_NO_ERROR)
        { state.SkipWithError("Singleton tile pixels differ from ordinary OSG"); return; }
        for (auto _ : state) renderer.frame();
        ChonkTest::DrawCounts counts;
        converted->accept(counts);
        state.counters["ordinary_drawables"] = counts.ordinary;
        state.counters["chonk_drawables"] = counts.chonks;
        state.counters["pixel_differences"] = difference;
    }
    BENCHMARK(BM_ChonkSingletonTiles)->Arg(64)->Arg(256)
        ->UseRealTime()->Unit(benchmark::kMillisecond);
}
