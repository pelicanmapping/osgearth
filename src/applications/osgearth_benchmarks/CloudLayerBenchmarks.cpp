/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <benchmark/benchmark.h>
#include <osgEarth/CloudLayer>
#include "../osgearth_tests/SkyNode2TestScene.h"
#include "../osgearth_tests/CloudShadowTestScene.h"
#include <iostream>

using namespace osgEarth;

namespace
{
    //! Measures all sky/cloud compute and scene passes, comparing clear, low, balanced, and high on the same view.
    void clouds(benchmark::State& state)
    {
        try
        {
            osg::ref_ptr<SkyNode2> sky = new SkyNode2;
            if (state.range(0) >= 0)
            {
                CloudLayer::Options options;
                options.quality = static_cast<CloudLayer::Quality>(state.range(0));
                options.coverage = float(state.range(1))*0.01f;
                options.wind.set(0,0,0);
                if (state.range(2) == 4)
                {
                    options.detailFiltering = state.range(3) != 0;
                    options.size = 1000.0f; options.erosion = 0.5f;
                    options.wind.set(0,100,0);
                }
                if (state.range(2) == 5)
                {
                    options.farShadows = state.range(3) != 0;
                    options.size = 1500.0f; options.erosion = 0.4f; options.density = 4.0f;
                    options.topAltitude = 2300.0f; options.seed = 3;
                    options.wind.set(0,100,0);
                }
                if (state.range(2) == 3)
                {
                    options.fadeStartAltitude = 200000.0f;
                    options.fadeEndAltitude = 300000.0f;
                }
                sky->setCloudLayer(new CloudLayer(options));
            }
            Sky2Tests::Scene scene(sky,1920,1080);
            if (state.range(2) == 5)
            {
                scene.skyView(1000.0);
                Sky2Tests::groundShadowProbe(scene,1920,1080,30.0f);
            }
            else if (state.range(2) == 4) scene.skyView(1000.0);
            else if (state.range(2) >= 2) scene.planetView(150000.0);
            else scene.skyView(state.range(2) ? 2500.0 : 2.0);
            scene.readback->enabled = false;
            for (unsigned i=0; i<8; ++i) scene.draw();
            if (state.range(0) >= 0 && !sky->getStateSet()->getDefinePair("OE_CLOUD_LAYER"))
            {
                state.SkipWithError("Cloud compute unavailable; request OSG_GL_CONTEXT_VERSION=4.6");
                return;
            }
            auto gl = scene.context->getState()->get<osg::GLExtensions>();
            std::cout << "Cloud benchmark GPU: " << glGetString(GL_RENDERER) << '\n';
            GLuint query = 0; gl->glGenQueries(1,&query);
            unsigned frame = 0;
            for (auto _ : state)
            {
                scene.viewer->advance(double(frame++%120)*0.1);
                scene.viewer->updateTraversal();
                gl->glBeginQuery(GL_TIME_ELAPSED,query);
                scene.viewer->renderingTraversals();
                scene.context->makeCurrent();
                gl->glEndQuery(GL_TIME_ELAPSED);
                GLuint64 elapsed = 0; gl->glGetQueryObjectui64v(query,GL_QUERY_RESULT,&elapsed);
                state.SetIterationTime(double(elapsed)*1e-9);
            }
            gl->glDeleteQueries(1,&query);
            if (glGetError() != GL_NO_ERROR) state.SkipWithError("Cloud benchmark OpenGL error");
        }
        catch (const std::exception& e) { state.SkipWithError(e.what()); }
    }

    auto cloudBenchmarks = benchmark::RegisterBenchmark("CloudLayer/Frame",clouds)
        ->Args({-1,80,0})->Args({0,80,0})->Args({1,80,0})->Args({2,80,0})
        ->Args({1,0,0})->Args({1,55,0})->Args({1,100,0})->Args({1,80,1})
        ->Args({-1,80,2})->Args({1,80,2})->Args({1,80,3})
        ->UseManualTime()->Unit(benchmark::kMillisecond);

    // Paired fixed-wind runs isolate filtering cost for small distant clouds at each existing quality budget.
    auto filterBenchmarks = benchmark::RegisterBenchmark("CloudLayer/DetailFiltering",clouds)
        ->Args({0,55,4,0})->Args({0,55,4,1})->Args({1,55,4,0})->Args({1,55,4,1})
        ->Args({2,55,4,0})->Args({2,55,4,1})
        ->UseManualTime()->Unit(benchmark::kMillisecond);

    // Paired GPU timings include cloud compute and 1080p ground lookups across near, transition, and far regions.
    auto shadowBenchmarks = benchmark::RegisterBenchmark("CloudLayer/ShadowCascades",clouds)
        ->Args({1,60,5,0})->Args({1,60,5,1})->Args({2,60,5,0})->Args({2,60,5,1})
        ->UseManualTime()->Unit(benchmark::kMillisecond);

    //! Measures incremental ray cost on identical moving clouds, from below and inside the layer.
    void cloudRays(benchmark::State& state)
    {
        try
        {
            osg::ref_ptr<SkyNode2> sky = new SkyNode2;
            CloudLayer::Options options;
            options.coverage = 0.55f;
            options.crepuscularRays = state.range(0) >= 0;
            if (options.crepuscularRays) options.rayQuality = static_cast<CloudLayer::Quality>(state.range(0));
            options.wind.set(0,100,0);
            sky->setCloudLayer(new CloudLayer(options));
            Sky2Tests::Scene scene(sky,1920,1080);
            scene.skyView(state.range(1) ? 2500.0 : 1000.0);
            sky->setEphemeris(new Sky2Tests::Sun(8.0));
            scene.readback->enabled = false;
            for (unsigned i=0; i<8; ++i) scene.draw();
            if (!sky->getStateSet()->getDefinePair("OE_CLOUD_LAYER"))
            {
                state.SkipWithError("Cloud compute unavailable; request OSG_GL_CONTEXT_VERSION=4.6");
                return;
            }
            auto gl = scene.context->getState()->get<osg::GLExtensions>();
            std::cout << "Cloud ray benchmark GPU: " << glGetString(GL_RENDERER) << '\n';
            GLuint query = 0; gl->glGenQueries(1,&query);
            unsigned frame = 0;
            for (auto _ : state)
            {
                // Repeat a fixed wind interval so all configurations integrate the same moving cloud field.
                scene.viewer->advance(double(frame++%120)*0.1);
                scene.viewer->updateTraversal();
                gl->glBeginQuery(GL_TIME_ELAPSED,query);
                scene.viewer->renderingTraversals();
                scene.context->makeCurrent();
                gl->glEndQuery(GL_TIME_ELAPSED);
                GLuint64 elapsed = 0; gl->glGetQueryObjectui64v(query,GL_QUERY_RESULT,&elapsed);
                state.SetIterationTime(double(elapsed)*1e-9);
            }
            gl->glDeleteQueries(1,&query);
            if (glGetError() != GL_NO_ERROR) state.SkipWithError("Cloud ray benchmark OpenGL error");
        }
        catch (const std::exception& e) { state.SkipWithError(e.what()); }
    }

    auto rayBenchmarks = benchmark::RegisterBenchmark("CloudLayer/Rays",cloudRays)
        ->Args({-1,0})->Args({0,0})->Args({1,0})->Args({2,0})
        ->Args({-1,1})->Args({0,1})->Args({1,1})->Args({2,1})
        ->UseManualTime()->Unit(benchmark::kMillisecond);
}
