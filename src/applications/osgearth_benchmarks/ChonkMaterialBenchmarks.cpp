/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <benchmark/benchmark.h>
#include "../osgearth_tests/ChonkMaterialTestUtils.h"

using namespace osgEarth;

namespace
{
    // Validate against ordinary rendering, then time warmed-up Chonk frames
    // including glFinish. Arguments control mesh subdivisions and instance count;
    // counters report vertex storage separately from complete frame time.
    void BM_ChonkMaterials_Render(benchmark::State& state)
    {
        if (!Capabilities::get().supportsNVGL())
        {
            state.SkipWithError("NVIDIA bindless rendering is unavailable");
            return;
        }
        static ChonkTest::Renderer renderer;
        static const bool ready = renderer.initialize();
        if (!ready) { state.SkipWithError("Cannot create GL context"); return; }
        ChonkTest::materialShader(renderer);
        auto source = ChonkTest::materialScene(unsigned(state.range(0)));
        auto chonk = renderer.factory->getOrCreateChonk(source);
        if (!chonk) { state.SkipWithError("Conversion failed"); return; }
        osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable();
        drawable->setUseGPUCulling(false);
        // Identical overlapping placements stress vertex work without growing
        // the framebuffer; depth testing retains the same reference image.
        for (int i = 0; i < state.range(1); ++i) drawable->add(chonk);
        renderer.setScene(source);
        ShaderGenerator().run(source);
        renderer.frame(); renderer.frame();
        auto reference = renderer.pixels();
        renderer.setScene(drawable);
        for (unsigned i = 0; i < 8; ++i) renderer.frame();
        auto actual = renderer.pixels();
        unsigned differences = 0, nonzero = 0;
        for (unsigned i = 0; i < 256u*256u*4u; ++i)
        {
            differences += std::abs(int(reference->data()[i])-int(actual->data()[i])) > 3;
            if (i%4 != 3) nonzero += actual->data()[i] != 0;
        }
        if (!nonzero || differences > 100 || glGetError() != GL_NO_ERROR)
        {
            auto a = actual->getColor(100,100), b = reference->getColor(100,100);
            const std::string error = "Material rendering differs: " + std::to_string(differences) +
                " channels; actual " + std::to_string(a.r()) + "," + std::to_string(a.g()) + "," + std::to_string(a.b()) +
                " reference " + std::to_string(b.r()) + "," + std::to_string(b.g()) + "," + std::to_string(b.b());
            state.SkipWithError(error.c_str());
            return;
        }
        for (auto _ : state) renderer.frame();
        state.counters["vertex_bytes"] = chonk->_vbo_store.size()*sizeof(Chonk::VertexGPU);
        state.counters["vertex_stride"] = sizeof(Chonk::VertexGPU);
        state.counters["vertices"] = chonk->_vbo_store.size();
        state.counters["instances"] = state.range(1);
    }
    BENCHMARK(BM_ChonkMaterials_Render)->Args({16,1})->Args({128,1})->Args({128,16})
        ->UseRealTime()->Unit(benchmark::kMillisecond);
}
