/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <benchmark/benchmark.h>
#include "../osgearth_tests/ChonkTestUtils.h"

using namespace osgEarth;

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
