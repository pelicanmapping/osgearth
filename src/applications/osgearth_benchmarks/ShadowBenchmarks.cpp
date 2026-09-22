/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <benchmark/benchmark.h>
#include <osgEarth/Shadowing>
#include <osgEarth/ShadowingMath.h>
#include <osgEarth/CameraUtils>
#include <osgShadow/ConvexPolyhedron>
#include "../osgearth_tests/SkyNode2TestScene.h"
#include "../osgearth_tests/Shadow46TestScene.h"
#include <iostream>

using namespace osgEarth;

namespace
{
    //! Marks GPU execution boundaries after CPU culling, including all pre-render shadow passes.
    struct Timestamp : osg::Camera::DrawCallback
    {
        GLuint query;
        osg::observer_ptr<Util::ShadowCaster> fallback;
        //! Borrows a query owned by the benchmark, which detaches callbacks before deleting it.
        explicit Timestamp(GLuint value, Util::ShadowCaster* cached = nullptr) : query(value), fallback(cached) { }
        //! Enqueues a timestamp in the draw context without synchronizing the CPU.
        void operator()(osg::RenderInfo& info) const override
        {
            if (fallback.valid() && fallback->getRenderedCascades() != 0) return;
            info.getState()->get<osg::GLExtensions>()->glQueryCounter(query,GL_TIMESTAMP);
        }
    };

    //! Instruments the first actual shadow stage after culling; OSG draws these before the primary initial callback.
    struct ShadowTimer : osg::NodeCallback
    {
        GLuint query;
        std::vector<osg::ref_ptr<osg::Camera>> cameras;
        //! Borrows the benchmark's start timestamp for this single-threaded fixture.
        explicit ShadowTimer(GLuint value) : query(value) { }
        //! Attaches the timestamp after shadow cameras have been queued, without changing the caster graph.
        void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
        {
            traverse(node,visitor);
            auto cv = Culling::asCullVisitor(*visitor);
            for (const auto& entry : cv->getRenderStage()->getPreRenderList())
            {
                auto camera = entry.second->getCamera();
                if (!camera || !CameraUtils::isShadowCamera(camera)) continue;
                camera->setInitialDrawCallback(new Timestamp(query));
                if (std::find(cameras.begin(),cameras.end(),camera) == cameras.end()) cameras.emplace_back(camera);
                break;
            }
        }
        //! Removes borrowed query references before their GL names are deleted.
        void detach() { for (auto& camera : cameras) camera->setInitialDrawCallback(nullptr); }
    };
    //! Measures equivalent world-space frustum corners; validates both paths against each other before timing.
    void shadowCorners(benchmark::State& state, bool direct)
    {
        osg::Matrixd view = osg::Matrixd::lookAt(osg::Vec3d(6378200,10,20),osg::Vec3d(6378137,100,0),
            osg::Vec3d(1,0,0));
        osg::Matrixd inverseView = osg::Matrixd::inverse(view);
        osg::Matrixd projection = osg::Matrixd::perspective(55,1.7,1,2500);
        ShadowMath::Corners points;
        ShadowMath::corners(projection,1,2500,points);
        osg::Matrixd mvp = view*projection;
        osgShadow::ConvexPolyhedron polyhedron;
        polyhedron.setToUnitFrustum(true,true);
        polyhedron.transform(osg::Matrixd::inverse(mvp),mvp);
        std::vector<osg::Vec3d> reference;
        polyhedron.getPoints(reference);
        double error = 0.0;
        for (const auto& p : points)
        {
            double nearest = 1e10;
            for (const auto& r : reference) nearest = std::min(nearest,(p*inverseView-r).length());
            error = std::max(error,nearest);
        }
        if (error > 0.001) { state.SkipWithError("World frustum corners differ by more than 1 mm"); return; }
        for (auto _ : state)
        {
            if (direct)
            {
                ShadowMath::corners(projection,1,2500,points);
                for (auto& p : points) p = p*inverseView;
                benchmark::DoNotOptimize(points);
            }
            else
            {
                osg::Matrixd transform = view*projection;
                osgShadow::ConvexPolyhedron legacy;
                legacy.setToUnitFrustum(true,true);
                legacy.transform(osg::Matrixd::inverse(transform),transform);
                std::vector<osg::Vec3d> vertices;
                legacy.getPoints(vertices);
                benchmark::DoNotOptimize(vertices);
            }
        }
        state.counters["max_error_m"] = error;
    }

    //! Times complete production frames with identical geometry, ranges and resolution across implementations.
    void shadowFrame(benchmark::State& state)
    {
        try
        {
            SkyNode2::Options options;
            options.preset = SkyNode2::FLAT;
            Sky2Tests::Scene scene(new SkyNode2(options),1920,1080);
            osg::ref_ptr<Util::ShadowCaster> caster = new Util::ShadowCaster;
            caster->setTextureSize(unsigned(state.range(0)));
            caster->setBlurFactor(0.001f);
            caster->setRanges(state.range(1) == 1 ? std::vector<float>{0,100} : std::vector<float>{0,25,60,150});
            caster->setLight(scene.sky->getSunLight());
            caster->getShadowCastingGroup()->addChild(scene.models);
            scene.sky->removeChild(scene.models);
            caster->addChild(scene.models);
            scene.sky->addChild(caster);
            scene.readback->enabled = false;
            auto gl = scene.context->getState()->get<osg::GLExtensions>();
            if (!gl->glGetQueryObjectui64v) { state.SkipWithError("GPU timers unavailable"); return; }
            static bool reported = false;
            if (!reported)
            {
                std::cerr << "Shadow benchmark GPU: " << glGetString(GL_RENDERER) << "; " << glGetString(GL_VERSION) << '\n';
                reported = true;
            }
            for (unsigned i=0; i<12; ++i) scene.draw();
            GLuint query = 0;
            gl->glGenQueries(1,&query);
            unsigned frame = 0;
            unsigned errors = Sky2Tests::Diagnostics::get().errors.load();
            for (auto _ : state)
            {
                scene.groundView((frame++%120)*0.05);
                scene.viewer->advance();
                scene.viewer->updateTraversal();
                gl->glBeginQuery(GL_TIME_ELAPSED,query);
                scene.viewer->renderingTraversals();
                scene.context->makeCurrent();
                gl->glEndQuery(GL_TIME_ELAPSED);
                GLuint64 ns = 0;
                gl->glGetQueryObjectui64v(query,GL_QUERY_RESULT,&ns);
                state.SetIterationTime(double(ns)*1e-9);
            }
            gl->glDeleteQueries(1,&query);
            if (glGetError() != GL_NO_ERROR || Sky2Tests::Diagnostics::get().errors.load() != errors)
                state.SkipWithError("GL or shader error in shadow frame benchmark");
        }
        catch (const std::exception& error) { state.SkipWithError(error.what()); }
    }

    //! Measures cascade scaling with actual Chonk culling, caster rasterization, and receiver shading.
    void shadowCity(benchmark::State& state)
    {
        if (!Capabilities::get().supportsNVGL()) { state.SkipWithError("Requires NVGL and a 4.6 context"); return; }
        try
        {
            Shadow46Tests::Scene scene(16384,unsigned(state.range(3)));
            scene.buildings->setBirthday(-100.0);
            scene.shadows->setCascades(unsigned(state.range(0)),600);
            const auto mode = state.range(2);
            scene.shadows->setSubmission(mode == 0 ? Util::ShadowCaster::LEGACY :
                mode == 2 ? Util::ShadowCaster::CORE : Util::ShadowCaster::AUTOMATIC);
            scene.shadows->setCacheEnabled(mode == 4);
            scene.readback->enabled = false;
            auto gl = scene.context->getState()->get<osg::GLExtensions>();
            for (unsigned i=0; i<12; ++i) scene.draw();
            GLuint queries[3]{};
            gl->glGenQueries(3,queries);
            auto camera = scene.viewer->getCamera();
            osg::ref_ptr<ShadowTimer> timer = new ShadowTimer(queries[0]);
            scene.shadows->addCullCallback(timer);
            camera->setInitialDrawCallback(new Timestamp(queries[0],scene.shadows));
            camera->setPreDrawCallback(new Timestamp(queries[1]));
            camera->setFinalDrawCallback(new Timestamp(queries[2]));
            unsigned frame = 0;
            double cpu = 0.0;
            double shadow = 0.0;
            for (auto _ : state)
            {
                if (state.range(1)) scene.cityView((frame++%120)*0.05);
                scene.viewer->advance();
                scene.viewer->updateTraversal();
                auto start = osg::Timer::instance()->tick();
                scene.viewer->renderingTraversals();
                scene.context->makeCurrent();
                cpu += osg::Timer::instance()->delta_m(start,osg::Timer::instance()->tick());
                GLuint64 times[3]{};
                for (unsigned i=0; i<3; ++i) gl->glGetQueryObjectui64v(queries[i],GL_QUERY_RESULT,&times[i]);
                shadow += double(times[1]-times[0])*1e-6;
                state.SetIterationTime(double(times[2]-times[0])*1e-9);
            }
            camera->setInitialDrawCallback(nullptr);
            camera->setPreDrawCallback(nullptr);
            camera->setFinalDrawCallback(nullptr);
            timer->detach();
            scene.shadows->removeCullCallback(timer);
            gl->glDeleteQueries(3,queries);
            state.counters["cpu_submit_ms"] = cpu/state.iterations();
            state.counters["shadow_gpu_ms"] = shadow/state.iterations();
            state.counters["reused_cascades"] = scene.shadows->getReusedCascades();
            if (glGetError() != GL_NO_ERROR) state.SkipWithError("OpenGL error in city shadows");
        }
        catch (const std::exception& e) { state.SkipWithError(e.what()); }
    }

    BENCHMARK(shadowCity)->Name("Shadow46/City")->ArgsProduct({{1,2,3,4},{0,1},{0,1,2,4},{1}})
        ->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK(shadowCity)->Name("Shadow46/CityMeshes")->ArgsProduct({{4},{1},{0,1,2},{64}})
        ->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK(shadowFrame)->Name("Shadow/Frame")->Args({1024,1})->Args({4096,1})->Args({2048,3})
        ->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(shadowCorners, Before, false)->Name("Shadow/CornersBefore");
    BENCHMARK_CAPTURE(shadowCorners, After, true)->Name("Shadow/CornersAfter");
}
