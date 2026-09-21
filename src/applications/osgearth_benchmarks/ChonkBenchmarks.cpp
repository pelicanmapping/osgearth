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
    // Exercise Prestige's model converter; toggle pages between separate processes
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

namespace
{
    // Model a detail cell: each asset's placements span the entire cell, so
    // ordinary batch bounds cannot reject the individual offscreen instances.
    // Both paths use the same source geometry, matrices, camera and shaders.
    void renderInstances(benchmark::State& state, bool chonk)
    {
        if (!Capabilities::get().supportsNVGL())
        {
            state.SkipWithError("NVIDIA bindless rendering is unavailable");
            return;
        }
        // Reuse a context across Google Benchmark's calibration/repetition
        // calls; osgEarth's shared render-bin programs outlive individual runs.
        static ChonkTest::Renderer renderer;
        static const bool ready = renderer.initialize();
        if (!ready)
        {
            state.SkipWithError("Cannot create an offscreen GL context");
            return;
        }
        std::vector<std::unique_ptr<ChonkTest::AssetFile>> files;
        osg::ref_ptr<osg::Group> source = new osg::Group();
        constexpr unsigned batchCount = 32, instanceCount = 32768;
        auto geometry = ChonkTest::mesh(16);
        for (unsigned batch = 0; batch < batchCount; ++batch)
        {
            auto file = std::make_unique<ChonkTest::AssetFile>();
            if (!file->write(geometry))
            {
                state.SkipWithError("Cannot write benchmark asset");
                return;
            }
            InstancedExternalNode::MatrixList matrices;
            for (unsigned i = batch; i < instanceCount; i += batchCount)
            {
                const float x = (int(i % 256u) - 128) * 8.0f;
                const float y = (int(i / 256u) - 64) * 8.0f;
                matrices.emplace_back(osg::Matrixf::scale(1.2f,.6f,1) *
                    osg::Matrixf::rotate(.9, osg::Vec3(0,0,1)) * osg::Matrixf::translate(x,y,0));
            }
            auto* external = new InstancedExternalNode(file->path, matrices);
            source->addChild(external);
            if (!external->isUsingHardwareInstancing())
            {
                state.SkipWithError("Attribute-instancing baseline is unavailable");
                return;
            }
            files.emplace_back(std::move(file));
        }
        osg::ref_ptr<osg::Node> scene = source;
        if (chonk)
        {
            scene = ChonkFactory::convertExternalInstances(source, renderer.factory);
            ChonkTest::FindDrawables find;
            scene->accept(find);
            if (find.drawables.size() != 1 || find.drawables[0]->getNumInstances() != instanceCount ||
                find.drawables[0]->getNumBatches() != batchCount)
            {
                state.SkipWithError("Chonk did not preserve every batch and instance");
                return;
            }
        }
        renderer.viewer.getCamera()->setViewMatrixAsLookAt(
            osg::Vec3d(0,0,double(state.range(0))), osg::Vec3d(), osg::Vec3d(0,1,0));
        osg::ref_ptr<osg::Image> reference;
        if (chonk)
        {
            renderer.setScene(source);
            renderer.frame(); renderer.frame();
            reference = renderer.pixels();
        }
        renderer.setScene(scene);
        for (unsigned i = 0; i < 5; ++i) renderer.frame();
        auto pixels = renderer.pixels();
        bool nonempty = false;
        for (unsigned i = 0; i < 256u*256u; ++i) nonempty |= pixels->data()[4*i+2] > 0;
        if (!nonempty || glGetError() != GL_NO_ERROR)
        {
            state.SkipWithError("Rendering failed or produced an empty image");
            return;
        }
        if (reference)
        {
            unsigned different = 0;
            for (unsigned i = 0; i < 256u*256u*4u; ++i)
                different += std::abs(int(reference->data()[i]) - int(pixels->data()[i])) > 2;
            if (different > 30)
            {
                state.SkipWithError("Chonk image differs from attribute instancing");
                return;
            }
        }
        for (auto _ : state) renderer.frame();
        state.counters["instances"] = instanceCount;
        state.counters["assets"] = batchCount;
        state.counters["triangles_before_culling"] = instanceCount*16u*16u*2u;
    }

    void BM_Prestige_AttributeInstancing(benchmark::State& state) { renderInstances(state, false); }
    void BM_Prestige_ChonkGPUCulling(benchmark::State& state) { renderInstances(state, true); }
    // Near camera: about 5% of the cell is visible. Far camera: all visible.
    BENCHMARK(BM_Prestige_AttributeInstancing)->Arg(400)->Arg(3000)->UseRealTime()->Unit(benchmark::kMillisecond);
    BENCHMARK(BM_Prestige_ChonkGPUCulling)->Arg(400)->Arg(3000)->UseRealTime()->Unit(benchmark::kMillisecond);
}
