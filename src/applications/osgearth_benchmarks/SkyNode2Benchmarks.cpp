/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <benchmark/benchmark.h>
#include <osgEarth/SkyNode2Atmosphere.h>
#include <osgEarth/MapNode>
#include "../osgearth_tests/SkyNode2TestScene.h"
#include <iostream>

using namespace osgEarth;

namespace
{
    //! Measures an atmospheric query before/after tabulation, checking error against the same reference model.
    void transmission(benchmark::State& state, bool lookup)
    {
        auto table = Sky2Atmosphere::createTransmittance();
        std::vector<osg::Vec2d> rays;
        double maximumError = 0.0;
        for (unsigned i=0; i<128; ++i)
        {
            double r = Sky2Atmosphere::radius+0.1+i*0.25, mu = -0.05+i/127.0;
            rays.emplace_back(r,mu);
            auto reference = Sky2Atmosphere::integrate(r,mu,128);
            auto actual = Sky2Atmosphere::sample(*table,r,mu);
            for (unsigned c=0; c<3; ++c) maximumError = std::max(maximumError,std::abs(actual[c]-reference[c]));
        }
        if (maximumError > 0.015) { state.SkipWithError("Transmittance lookup exceeds error bound"); return; }
        for (auto _ : state)
        for (const auto& ray : rays)
        {
            auto result = lookup ? Sky2Atmosphere::sample(*table,ray.x(),ray.y()) :
                Sky2Atmosphere::integrate(ray.x(),ray.y(),128);
            benchmark::DoNotOptimize(result);
        }
        state.SetItemsProcessed(state.iterations()*rays.size());
        state.counters["max_absolute_error"] = maximumError;
    }

    //! Measures a full production frame, including all LUT updates, at stationary and moving viewpoints.
    void render(benchmark::State& state, int quality)
    {
        SkyNode2::Options options;
        options.preset = static_cast<SkyNode2::Quality>(quality);
        osg::ref_ptr<SkyNode2> sky = new SkyNode2(options);
        try
        {
            Sky2Tests::Scene scene(sky,unsigned(state.range(0)),unsigned(state.range(0))*9/16);
            osg::ref_ptr<MapNode> map = new MapNode;
            if (!map->open()) { state.SkipWithError("Empty map failed to open"); return; }
            sky->addChild(map);
            if (state.range(1) == 2 || state.range(1) == 3) scene.aerialProbe();
            if (state.range(1) == 4) scene.wallView(scene.horizonWall(),100.0,300.0);
            scene.readback->enabled = false;
            auto gl = scene.context->getState()->get<osg::GLExtensions>();
            if (!gl->glGetQueryObjectui64v) { state.SkipWithError("GPU timer queries unavailable"); return; }
            static bool reported = false;
            if (!reported)
            {
                std::cerr << "SkyNode2 benchmark GPU: " << glGetString(GL_RENDERER) << "; " << glGetString(GL_VERSION) << '\n';
                reported = true;
            }
            for (unsigned i=0; i<8; ++i) scene.draw();
            GLuint query = 0;
            gl->glGenQueries(1,&query);
            unsigned frame = 0;
            unsigned errors = Sky2Tests::Diagnostics::get().errors.load();
            for (auto _ : state)
            {
                if (state.range(1) == 1) scene.groundView((frame++%120)*0.25);
                else if (state.range(1) == 2) scene.planetView(2.0*std::pow(7500000.0,(frame++%120)/119.0));
                else if (state.range(1) == 3)
                {
                    scene.models->setNodeMask(0);
                    double angle = 0.3*std::sin((frame++%120)*osg::PI/60.0);
                    osg::Vec3d eye(11378137.0,0,0), direction(-std::cos(angle),std::sin(angle),0);
                    scene.viewer->getCamera()->setViewMatrixAsLookAt(eye,eye+direction,osg::Vec3d(0,0,1));
                }
                scene.viewer->advance();
                scene.viewer->updateTraversal();
                gl->glBeginQuery(GL_TIME_ELAPSED,query);
                scene.viewer->renderingTraversals();
                scene.context->makeCurrent();
                gl->glEndQuery(GL_TIME_ELAPSED);
                GLuint64 nanoseconds = 0;
                gl->glGetQueryObjectui64v(query,GL_QUERY_RESULT,&nanoseconds);
                state.SetIterationTime(double(nanoseconds)*1e-9);
            }
            gl->glDeleteQueries(1,&query);
            if (glGetError() != GL_NO_ERROR || Sky2Tests::Diagnostics::get().errors.load() != errors)
                state.SkipWithError("GL or shader error in production frame benchmark");
        }
        catch (const std::exception& error) { state.SkipWithError(error.what()); }
    }

    BENCHMARK_CAPTURE(transmission, Reference128, false)->Name("SkyNode2/TransmittanceBefore");
    BENCHMARK_CAPTURE(transmission, Lookup, true)->Name("SkyNode2/TransmittanceAfter");
    BENCHMARK_CAPTURE(render, Flat, 0)->Name("SkyNode2/FlatFrame")
        ->Args({1920,0})->Args({1920,1})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(render, Balanced, 1)->Name("SkyNode2/BalancedFrame")
        ->Args({1920,0})->Args({1920,1})->Args({3840,1})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(render, High, 2)->Name("SkyNode2/HighFrame")
        ->Args({1920,0})->Args({1920,1})->Args({3840,1})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(render, BalancedDescent, 1)->Name("SkyNode2/BalancedDescent")
        ->Args({1920,2})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(render, HighDescent, 2)->Name("SkyNode2/HighDescent")
        ->Args({1920,2})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(render, BalancedRotation, 1)->Name("SkyNode2/BalancedRotation")
        ->Args({1920,3})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(render, HighRotation, 2)->Name("SkyNode2/HighRotation")
        ->Args({1920,3})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(render, BalancedHorizon, 1)->Name("SkyNode2/BalancedHorizon")
        ->Args({1920,4})->Args({3840,4})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(render, HighHorizon, 2)->Name("SkyNode2/HighHorizon")
        ->Args({1920,4})->Args({3840,4})->UseManualTime()->Unit(benchmark::kMillisecond);
}
