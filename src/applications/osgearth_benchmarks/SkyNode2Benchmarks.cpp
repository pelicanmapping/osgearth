/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <benchmark/benchmark.h>
#include <osgEarth/SkyNode2Atmosphere.h>
#include <osgEarth/MapNode>
#include <osgEarth/ExampleResources>
#include <osgEarth/EarthManipulator>
#include <osgDB/DatabasePager>
#include "../osgearth_tests/SkyNode2TestScene.h"
#include "../osgearth_tests/ChonkMaterialTestUtils.h"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <chrono>

using namespace osgEarth;

namespace
{
    //! Compares display-space pixels before timing; a missing surface or coverage change fails the benchmark.
    void compareLighting(benchmark::State& state, const std::vector<float>& pixels, const std::vector<float>& reference)
    {
        double maximum = 0.0, square = 0.0;
        for (std::size_t i=0; i<pixels.size(); ++i)
        {
            double error = std::abs(double(pixels[i])-reference[i]);
            maximum = std::max(maximum,error); square += error*error;
        }
        state.counters["max_error"] = maximum;
        state.counters["rmse"] = std::sqrt(square/pixels.size());
        if (maximum > 2.0/255.0) throw std::runtime_error("Lighting changed: max="+std::to_string(maximum)+
            " RMS="+std::to_string(std::sqrt(square/pixels.size())));
    }

    //! Measures temporal/coplanar noise in GPU-compacted city draws; synthetic fixtures retain strict pixel checks.
    double lightingRMS(const std::vector<float>& a, const std::vector<float>& b)
    {
        double square = 0.0;
        for (std::size_t i=0; i<a.size(); ++i)
        {
            double delta = double(a[i])-b[i];
            square += delta*delta;
        }
        return std::sqrt(square/a.size());
    }

    //! Records or verifies an opt-in reference image; never updates a reference during verification.
    void verifyLighting(benchmark::State& state, const std::vector<float>& pixels, const std::string& name)
    {
        if (const char* directory = std::getenv("SKY2_LIGHTING_RECORD"))
        {
            std::ofstream output(std::string(directory)+"/"+name+".rgba",std::ios::binary);
            output.write(reinterpret_cast<const char*>(pixels.data()),pixels.size()*sizeof(float));
            if (!output) throw std::runtime_error("Could not write lighting reference image");
        }
        if (const char* directory = std::getenv("SKY2_LIGHTING_VERIFY"))
        {
            std::vector<float> reference(pixels.size());
            std::ifstream input(std::string(directory)+"/"+name+".rgba",std::ios::binary);
            input.read(reinterpret_cast<char*>(reference.data()),reference.size()*sizeof(float));
            if (!input) throw std::runtime_error("Missing or incomplete lighting reference image");
            compareLighting(state,pixels,reference);
        }
    }

    struct CityTimer : osg::Camera::DrawCallback
    {
        GLuint query;
        bool begin;
        //! Brackets the main camera only, excluding the private atmospheric RTT cameras.
        CityTimer(GLuint q, bool start) : query(q), begin(start) { }
        //! Issues a timer boundary on the owning camera's draw thread.
        void operator()(osg::RenderInfo& info) const override
        {
            auto gl = info.getState()->get<osg::GLExtensions>();
            if (begin) gl->glBeginQuery(GL_TIME_ELAPSED,query);
            else gl->glEndQuery(GL_TIME_ELAPSED);
        }
    };

    struct CityScene
    {
        osg::ref_ptr<SkyNode2> sky = new SkyNode2;
        std::unique_ptr<Sky2Tests::Scene> scene;
        GLuint query = 0;
        //! Loads the opt-in local Prestige scene once and warms paging before any measurements.
        explicit CityScene(const std::string& filename)
        {
            GLUtils::useNVGL(true);
            scene.reset(new Sky2Tests::Scene(sky,3840,2160));
            scene->sky->setEphemeris(new Ephemeris);
            scene->sky->setDateTime(DateTime(2026,9,21,17.0));
            auto manip = new Util::EarthManipulator;
            scene->viewer->setCameraManipulator(manip);
            std::vector<std::string> args = {"sky2-benchmark",filename,"--nvgl","--novsync"};
            std::vector<char*> argv;
            for (auto& arg : args) argv.push_back(&arg[0]);
            int argc = int(argv.size());
            osg::ArgumentParser arguments(&argc,argv.data());
            auto node = Util::MapNodeHelper().load(arguments,scene->viewer);
            auto map = MapNode::get(node);
            if (!map) throw std::runtime_error("Cannot load Prestige benchmark map");
            sky->removeChild(scene->models);
            // Keep scene layers but exclude any sky/cloud/shadow wrappers from the earth file.
            sky->addChild(map);
            sky->attach(scene->viewer);
            // The viewpoints extension schedules its home view on the first event. Benchmarks own their camera.
            scene->viewer->getEventHandlers().clear();
            manip->setNode(sky);
            const auto viewpoints = map->getConfig().child("viewpoints").children("viewpoint");
            if (viewpoints.empty()) throw std::runtime_error("Prestige benchmark requires a saved viewpoint");
            unsigned index = 0;
            if (const char* value = std::getenv("SKY2_PRESTIGE_VIEW")) index = unsigned(std::stoul(value));
            if (index >= viewpoints.size()) throw std::runtime_error("Prestige viewpoint index out of range");
            Viewpoint requested(viewpoints[index]);
            if (const char* path = std::getenv("SKY2_PRESTIGE_VIEWPOINT"))
            {
                std::ifstream input(path);
                Config config;
                if (!input || !config.fromXML(input)) throw std::runtime_error("Cannot read benchmark viewpoint XML");
                requested = Viewpoint(config.key() == "viewpoint" ? config : config.child("viewpoint"));
                if (!requested.valid()) throw std::runtime_error("Invalid benchmark viewpoint XML");
            }
            manip->setHomeViewpoint(requested,0.0);
            manip->setViewpoint(requested,0.0);
            std::cerr << "SkyNode2 city viewpoint: " << manip->getViewpoint().getConfig().toJSON(false) << '\n';
            // Freeze before paging: terrain callbacks can recenter the manipulator and adjust its roll.
            auto viewMatrix = manip->getInverseMatrix();
            scene->viewer->setCameraManipulator(nullptr,false);
            scene->viewer->getCamera()->setViewMatrix(viewMatrix);
            scene->readback->enabled = false;
            auto start = std::chrono::steady_clock::now();
            while (std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count() < 30.0)
            {
                scene->draw();
                // Bound queued GPU work while the CPU pager warms; timing starts only after this phase.
                glFinish();
            }
            scene->viewer->getDatabasePager()->setAcceptNewDatabaseRequests(false);
            scene->readback->enabled = true;
            scene->draw();
            scene->save("sky2-lighting-prestige.png");
            scene->readback->enabled = false;
            auto gl = scene->context->getState()->get<osg::GLExtensions>();
            gl->glGenQueries(1,&query);
            scene->viewer->getCamera()->setPreDrawCallback(new CityTimer(query,true));
            scene->viewer->getCamera()->setPostDrawCallback(new CityTimer(query,false));
        }
        //! Renders the warmed scene without update traversal, preventing late streamed tiles from changing the workload.
        //! The camera/date stay fixed; SkyNode2's lighting settings are consumed during cull traversal.
        void draw()
        {
            unsigned errors = Sky2Tests::Diagnostics::get().errors.load();
            scene->viewer->advance();
            scene->viewer->renderingTraversals();
            scene->context->makeCurrent();
            if (Sky2Tests::Diagnostics::get().errors.load() != errors)
                throw std::runtime_error("Shader compilation, link, or OpenGL failure during city frame");
        }
        //! Releases the query before the fixture's graphics context is destroyed.
        ~CityScene()
        {
            scene->context->makeCurrent();
            scene->viewer->getCamera()->setPreDrawCallback(nullptr);
            scene->viewer->getCamera()->setPostDrawCallback(nullptr);
            scene->context->getState()->get<osg::GLExtensions>()->glDeleteQueries(1,&query);
        }
    };

    //! Measures the same loaded city with lighting components toggled and scene updates frozen after warmup.
    void prestigeLighting(benchmark::State& state)
    {
        const char* path = std::getenv("SKY2_PRESTIGE");
        if (!path) { state.SkipWithError("Set SKY2_PRESTIGE to an earth file to opt into the local city benchmark"); return; }
        try
        {
            static CityScene city(path);
            city.sky->setAtmosphereVisible(state.range(0) != 1);
            city.sky->setEnvironmentIntensity(state.range(0) == 2 ? 0.0f : 1.0f);
            city.sky->setLighting(state.range(0) != 3);
            auto lightingState = city.sky->getOrCreateStateSet();
            lightingState->setDefine("OE_NUM_LIGHTS","8",
                osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE|osg::StateAttribute::PROTECTED);
            for (unsigned i=0; i<12; ++i) city.draw();
            city.scene->readback->enabled = true;
            std::vector<float> reference;
            double temporal = 0.0;
            unsigned settle = 0;
            // Finish late texture uploads before accepting a reference; never retry a changed lighting result.
            for (; settle<4; ++settle)
            {
                city.draw();
                reference = city.scene->pixels();
                temporal = 0.0;
                // Compaction can leave one control frame unusually stable. Measure an envelope, not one pair.
                for (unsigned i=0; i<8; ++i)
                {
                    city.draw();
                    temporal = std::max(temporal,lightingRMS(city.scene->pixels(),reference));
                }
                if (temporal <= 0.001) break;
            }
            state.counters["settle_retries"] = settle;
            if (state.range(1) == 0) lightingState->setDefine("OE_NUM_LIGHTS","1");
            city.draw();
            double difference = lightingRMS(city.scene->pixels(),reference);
            state.counters["frame_rmse"] = temporal;
            state.counters["rmse"] = difference;
            if (temporal > 0.001 || difference > temporal*2.0+0.0001)
                throw std::runtime_error("City image changed: control RMS="+std::to_string(temporal)+
                    " comparison RMS="+std::to_string(difference));
            city.scene->readback->enabled = false;
            for (unsigned i=0; i<12; ++i) city.draw();
            auto gl = city.scene->context->getState()->get<osg::GLExtensions>();
            for (auto _ : state)
            {
                city.draw();
                GLuint64 ns = 0;
                gl->glGetQueryObjectui64v(city.query,GL_QUERY_RESULT,&ns);
                state.SetIterationTime(double(ns)*1e-9);
            }
            if (glGetError() != GL_NO_ERROR) state.SkipWithError("OpenGL error during Prestige benchmark");
        }
        catch (const std::exception& error)
        {
            // Keep validation failures visible even if the installed benchmark reporter fails to aggregate skipped runs.
            std::cerr << "SkyNode2 city validation failed: " << error.what() << std::endl;
            state.SkipWithError(error.what());
        }
    }

    struct LightingTimer : osg::Drawable::DrawCallback
    {
        mutable GLuint query = 0;
        //! Times eight actual production draws with scene state already applied; the caller owns the GL context.
        void drawImplementation(osg::RenderInfo& info, const osg::Drawable* drawable) const override
        {
            auto gl = info.getState()->get<osg::GLExtensions>();
            if (!query) gl->glGenQueries(1,&query);
            gl->glBeginQuery(GL_TIME_ELAPSED,query);
            for (unsigned i=0; i<8; ++i) drawable->drawImplementation(info);
            gl->glEndQuery(GL_TIME_ELAPSED);
        }
    };

    //! Times real bindless instancing with PBR maps, small triangles and overlapping facades at 4K.
    //! No external datasets, clouds or shadows; pixel references use the same production lighting as the city.
    void instancedLighting(benchmark::State& state)
    {
        if (!Capabilities::get().supportsNVGL())
        {
            state.SkipWithError("NVIDIA bindless rendering unavailable");
            return;
        }
        try
        {
            GLUtils::useNVGL(true);
            osg::ref_ptr<SkyNode2> sky = new SkyNode2;
            Sky2Tests::Scene scene(sky,3840,2160);
            scene.models->removeChildren(0,scene.models->getNumChildren());
            osg::ref_ptr<TextureArena> textures = new TextureArena;
            ChonkFactory factory(textures);
            auto source = ChonkTest::materialScene(unsigned(state.range(0)));
            auto chonk = factory.getOrCreateChonk(source);
            if (!chonk) throw std::runtime_error("Could not convert instanced facade fixture");
            osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable;
            drawable->setUseGPUCulling(false);
            for (unsigned layer=0; layer<unsigned(state.range(1)); ++layer)
            for (unsigned y=0; y<8; ++y)
            for (unsigned x=0; x<8; ++x)
                drawable->add(chonk,osg::Matrixf::translate(
                    (float(x)-3.5f)*12.0f,(float(y)-3.5f)*12.0f,80.0f+layer*0.25f));
            scene.models->addChild(drawable);
            scene.models->getOrCreateStateSet()->setAttribute(textures);
            scene.models->getOrCreateStateSet()->addUniform(new osg::Uniform("oe_sse",0.0f));
            scene.viewer->getCamera()->setViewMatrixAsLookAt(
                osg::Vec3d(6378257,0,0),osg::Vec3d(6378217,0,0),osg::Vec3d(0,0,1));
            if (state.range(2) == 1) sky->setAtmosphereVisible(false);
            if (state.range(2) == 2) sky->setLighting(false);
            auto lightingState = sky->getOrCreateStateSet();
            lightingState->setDefine("OE_NUM_LIGHTS","8",
                osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE|osg::StateAttribute::PROTECTED);
            scene.readback->enabled = false;
            for (unsigned i=0; i<16; ++i) scene.draw();
            scene.readback->enabled = true;
            scene.draw();
            auto reference = scene.pixels();
            lightingState->setDefine("OE_NUM_LIGHTS","1");
            scene.draw();
            compareLighting(state,scene.pixels(),reference);
            verifyLighting(state,scene.pixels(),"sky2-instanced-"+std::to_string(state.range(0))+"-"+
                std::to_string(state.range(1))+"-"+std::to_string(state.range(2)));
            scene.readback->enabled = false;
            auto gl = scene.context->getState()->get<osg::GLExtensions>();
            GLuint query = 0;
            gl->glGenQueries(1,&query);
            scene.viewer->getCamera()->setPreDrawCallback(new CityTimer(query,true));
            scene.viewer->getCamera()->setPostDrawCallback(new CityTimer(query,false));
            for (auto _ : state)
            {
                scene.draw();
                GLuint64 ns = 0;
                gl->glGetQueryObjectui64v(query,GL_QUERY_RESULT,&ns);
                state.SetIterationTime(double(ns)*1e-9);
            }
            scene.viewer->getCamera()->setPreDrawCallback(nullptr);
            scene.viewer->getCamera()->setPostDrawCallback(nullptr);
            gl->glDeleteQueries(1,&query);
            state.counters["instances"] = drawable->getNumInstances();
            state.counters["triangles"] = 64*state.range(1)*16*2*state.range(0)*state.range(0);
            if (glGetError() != GL_NO_ERROR) state.SkipWithError("OpenGL error during instanced lighting benchmark");
        }
        catch (const std::exception& error) { state.SkipWithError(error.what()); }
    }

    //! Isolates full-screen production lighting, with optional component ablation and before/after pixel validation.
    void lighting(benchmark::State& state, int quality)
    {
        SkyNode2::Options options;
        options.preset = static_cast<SkyNode2::Quality>(quality);
        osg::ref_ptr<SkyNode2> sky = new SkyNode2(options);
        try
        {
            unsigned width = unsigned(state.range(0)), mode = unsigned(state.range(1));
            Sky2Tests::Scene scene(sky,width,width*9/16);
            auto wall = scene.horizonWall();
            scene.wallView(wall,100.0,300.0);
            scene.viewer->getCamera()->setProjectionMatrixAsPerspective(50.0,16.0/9.0,0.1,1e8);
            auto ss = wall->getOrCreateStateSet();
            ss->removeAttribute(osg::StateAttribute::PROGRAM);
            ss->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS,0.0,1.0,false));
            ss->setRenderBinDetails(20,"RenderBin");
            ShaderLoader::load(VirtualProgram::getOrCreate(ss),R"(
                #pragma vp_function sky2BenchMaterial, fragment_coloring, 0.9
                struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;
                in vec3 vp_Normal;
                // Vary normals/materials across the facade to retain the complete PBR shader workload.
                void sky2BenchMaterial(inout vec4 color)
                {
                    vec2 uv=fract(gl_FragCoord.xy/vec2(271.0,193.0));
                    vp_Normal=normalize(vec3(uv.x-0.5,0.9,0.2+0.3*uv.y));
                    oe_pbr.roughness=0.05+0.95*uv.x; oe_pbr.metal=uv.y; oe_pbr.ao=0.8;
                    color=vec4(0.3+0.4*uv.x,0.25+0.3*uv.y,0.2,1.0);
                }
            )");
            if (mode == 1) sky->setAtmosphereVisible(false);
            if (mode == 2) sky->setEnvironmentIntensity(0.0f);
            auto geometry = wall->getChild(0)->asGeometry();
            osg::ref_ptr<LightingTimer> timer = new LightingTimer;
            geometry->setDrawCallback(timer);
            auto gl = scene.context->getState()->get<osg::GLExtensions>();
            if (!gl->glGetQueryObjectui64v) { state.SkipWithError("GPU timer queries unavailable"); return; }
            static bool reported = false;
            if (!reported)
            {
                std::cerr << "SkyNode2 lighting GPU: " << glGetString(GL_RENDERER) << "; " << glGetString(GL_VERSION) << '\n';
                reported = true;
            }
            scene.draw();
            verifyLighting(state,scene.pixels(),"sky2-lighting-"+std::to_string(quality)+"-"+
                std::to_string(width)+"-"+std::to_string(mode));
            scene.readback->enabled = false;
            for (unsigned i=0; i<8; ++i) scene.draw();
            for (auto _ : state)
            {
                scene.draw();
                GLuint64 nanoseconds = 0;
                gl->glGetQueryObjectui64v(timer->query,GL_QUERY_RESULT,&nanoseconds);
                state.SetIterationTime(double(nanoseconds)*1e-9/8.0);
            }
            gl->glDeleteQueries(1,&timer->query);
            timer->query = 0;
            geometry->setDrawCallback(nullptr);
            if (glGetError() != GL_NO_ERROR) state.SkipWithError("OpenGL error during lighting benchmark");
        }
        catch (const std::exception& error) { state.SkipWithError(error.what()); }
    }

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
    // The dataset is machine-local and large; do not add it to ordinary benchmark runs.
    auto* cityBenchmark = std::getenv("SKY2_PRESTIGE") ?
        benchmark::RegisterBenchmark("SkyNode2/PrestigeLighting",prestigeLighting)
        ->Args({0,8})->Args({0,0})->Args({1,0})->Args({2,0})->Args({3,0})
        ->UseManualTime()->Unit(benchmark::kMillisecond) : nullptr;
    BENCHMARK_CAPTURE(lighting, BalancedLighting, 1)->Name("SkyNode2/BalancedLighting")
        ->Args({3840,0})->Args({3840,1})->Args({3840,2})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK(instancedLighting)->Name("SkyNode2/InstancedLighting")
        ->Args({4,1,0})->Args({4,4,0})->Args({16,4,0})->Args({4,4,1})->Args({4,4,2})
        ->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(lighting, HighLighting, 2)->Name("SkyNode2/HighLighting")
        ->Args({3840,0})->Args({3840,1})->Args({3840,2})->UseManualTime()->Unit(benchmark::kMillisecond);
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
