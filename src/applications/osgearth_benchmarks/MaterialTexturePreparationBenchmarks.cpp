/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <benchmark/benchmark.h>
#include <osgEarth/MaterialTexturePreparation>
#include <osgEarth/TextureArena>
#include <osgEarth/Capabilities>
#include <osg/Texture2D>
#include <osg/GraphicsContext>
#include <chrono>

using namespace osgEarth;

namespace
{
    //! Constant red is exactly representable by both CPU and driver block compressors.
    osg::ref_ptr<PBRTexture> benchmarkMaterial(unsigned size)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image();
        image->allocateImage(size, size, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        for (unsigned y = 0; y < size; ++y)
            for (unsigned x = 0; x < size; ++x) image->setColor(osg::Vec4(1, 0, 0, 1), x, y);
        osg::ref_ptr<PBRTexture> material = new PBRTexture();
        material->albedo = new osg::Texture2D(image);
        material->albedo->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        return material;
    }

    //! Measure CPU preparation separately from upload; warm mode must reuse the prepared image.
    void preparationCPU(benchmark::State& state, bool warm)
    {
        auto source = benchmarkMaterial(unsigned(state.range(0)));
        MaterialTexturePreparation preparation(warm ? 256u * 1024u * 1024u : 0u);
        auto reference = preparation.prepare(source);
        if (!reference->albedo->getImage(0)->isCompressed())
        { state.SkipWithError("CPU compressor unavailable"); return; }
        if (!warm) reference = nullptr;
        for (auto _ : state)
        {
            auto result = preparation.prepare(source);
            if (warm && result->albedo->getImage(0) != reference->albedo->getImage(0))
            { state.SkipWithError("Cache did not reuse prepared image"); break; }
            benchmark::DoNotOptimize(result.get());
        }
    }

    //! Time completed GL compilation only, then validate every uploaded mip against the same red reference.
    void preparationUpload(benchmark::State& benchmark, bool prepared)
    {
        if (!Capabilities::get().supportsNVGL())
        { benchmark.SkipWithError("Requires bindless textures"); return; }
        auto traits = new osg::GraphicsContext::Traits(osg::DisplaySettings::instance());
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = traits->height = 16;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent())
        { benchmark.SkipWithError("Cannot create GL context"); return; }
        const unsigned size = unsigned(benchmark.range(0));
        auto material = benchmarkMaterial(size);
        MaterialTexturePreparation preparation;
        if (prepared) material = preparation.prepare(material);
        for (auto _ : benchmark)
        {
            auto texture = Texture::create(material->albedo);
            glFinish();
            const auto start = std::chrono::steady_clock::now();
            bool valid = texture->compileGLObjects(*context->getState());
            glFinish();
            benchmark.SetIterationTime(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            if (valid)
            {
                glBindTexture(GL_TEXTURE_2D, texture->getGLObject(*context->getState())->name());
                unsigned level = 0;
                for (unsigned dim = size; dim; dim /= 2, ++level)
                {
                    std::vector<unsigned char> pixels(dim * dim * 4);
                    glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                    for (unsigned i = 0; i < pixels.size(); i += 4)
                        valid &= pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 0 && pixels[i + 3] == 255;
                }
            }
            texture->releaseGLObjects(nullptr, true);
            if (!valid || glGetError() != GL_NO_ERROR)
            { benchmark.SkipWithError("Uploaded mip pixels differ from the reference"); break; }
        }
        context->releaseContext();
    }

    //! Baseline: unprepared images incur driver compression and mip generation at compile time.
    void BM_MaterialUpload_Before(benchmark::State& state) { preparationUpload(state, false); }
    //! Prepared images upload the CPU-compressed mip chain unchanged.
    void BM_MaterialUpload_After(benchmark::State& state) { preparationUpload(state, true); }
    //! Report the cold-load CPU cost explicitly instead of hiding the transferred work.
    void BM_MaterialPrepare_Cold(benchmark::State& state) { preparationCPU(state, false); }
    //! Report repeated-load cache overhead, with image identity checked on every hit.
    void BM_MaterialPrepare_Cached(benchmark::State& state) { preparationCPU(state, true); }
}

BENCHMARK(BM_MaterialUpload_Before)->Arg(256)->Arg(1024)->Iterations(12)->UseManualTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MaterialUpload_After)->Arg(256)->Arg(1024)->Iterations(12)->UseManualTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MaterialPrepare_Cold)->Arg(256)->Arg(1024)->Iterations(5)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MaterialPrepare_Cached)->Arg(256)->Arg(1024)->Iterations(12)->UseRealTime()->Unit(benchmark::kMillisecond);
