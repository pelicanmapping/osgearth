/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <benchmark/benchmark.h>
#include <osgEarth/VirtualProgram>
#include <osgEarth/Capabilities>
#include <osg/GraphicsContext>
#include <osg/State>
#include <osg/StateSet>

using namespace osgEarth;

namespace
{
    ProgramRepo::Key makeKey(unsigned value)
    {
        ProgramRepo::Key key;
        for (unsigned i = 0; i < 32; ++i)
            key.insert(key.end(), value * 32 + i);
        return key;
    }

    osg::ref_ptr<osg::Program> makeProgram(unsigned value)
    {
        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->addShader(new osg::Shader(osg::Shader::VERTEX,
            "void main() { gl_Position = vec4(" + std::to_string(value) + ".0); }"));
        return program;
    }

    void fillRepo(ProgramRepo& repo, unsigned size)
    {
        for (unsigned i = 0; i < size; ++i)
        {
            auto program = makeProgram(i);
            repo.add(makeKey(i), program, 0, 1);
        }
    }

    // Includes the same repository lock as VirtualProgram::apply.
    void repoLookup(benchmark::State& bench)
    {
        ProgramRepo repo;
        fillRepo(repo, static_cast<unsigned>(bench.range(0)));
        std::vector<ProgramRepo::Key> keys;
        for (unsigned i = 0; i < bench.range(0); ++i)
            keys.push_back(makeKey(i));
        unsigned i = 0;
        for (auto _ : bench)
        {
            std::lock_guard<ProgramRepo> lock(repo);
            auto program = repo.use(keys[i++ % keys.size()], 1, 1);
            benchmark::DoNotOptimize(program.get());
        }
    }
    BENCHMARK(repoLookup)->Name("VirtualProgram/RepoLookup")->Arg(128)->Arg(4096);

    // Time insertion into a populated cache, excluding construction and cleanup.
    void repoInsert(benchmark::State& bench)
    {
        ProgramRepo repo;
        const unsigned size = static_cast<unsigned>(bench.range(0));
        fillRepo(repo, size);
        auto key = makeKey(size);
        auto program = makeProgram(size);
        for (auto _ : bench)
        {
            repo.lock();
            repo.add(key, program, 1, 2);
            repo.unlock();
            bench.PauseTiming();
            repo.release(2, nullptr);
            bench.ResumeTiming();
        }
    }
    BENCHMARK(repoInsert)->Name("VirtualProgram/RepoInsert")->Arg(128)->Arg(4096);

    void repoRelease(benchmark::State& bench)
    {
        ProgramRepo repo;
        const unsigned size = static_cast<unsigned>(bench.range(0));
        fillRepo(repo, size);
        auto key = makeKey(size / 2);
        for (auto _ : bench)
        {
            bench.PauseTiming();
            repo.use(key, 1, 2);
            bench.ResumeTiming();
            repo.lock();
            repo.release(2, nullptr);
            repo.unlock();
        }
    }
    BENCHMARK(repoRelease)->Name("VirtualProgram/RepoRelease")->Arg(128)->Arg(4096);

    void applyWarm(benchmark::State& bench)
    {
        // Capability discovery creates a temporary context. Complete it before
        // making the benchmark context current.
        Capabilities::get();
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = 16;
        traits->height = 16;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent())
        {
            bench.SkipWithError("An OpenGL pbuffer is required for the apply benchmark");
            return;
        }

        osg::State& state = *context->getState();
        state.setUseVertexAttributeAliasing(true);
        state.setUseModelViewAndProjectionUniforms(true);
        std::vector<osg::ref_ptr<osg::StateSet>> stack;
        for (int depth = 0; depth < bench.range(0); ++depth)
        {
            osg::ref_ptr<osg::StateSet> ss = new osg::StateSet;
            auto vp = VirtualProgram::getOrCreate(ss);
            for (int i = 0; i < 8; ++i)
            {
                std::string name = "bench_" + std::to_string(depth) + "_" + std::to_string(i);
                vp->setFunction(name, "void " + name + "(inout vec4 v) { v.x += 0.00001; }",
                    VirtualProgram::LOCATION_VERTEX_MODEL, static_cast<float>(depth * 8 + i));
            }
            stack.push_back(ss);
            state.pushStateSet(ss);
        }
        auto vp = VirtualProgram::get(stack.back());
        vp->apply(state);
        if (!state.getLastAppliedProgramObject())
        {
            bench.SkipWithError("VirtualProgram failed to link the benchmark shaders");
        }
        else
        {
            for (auto _ : bench)
            {
                vp->apply(state);
                benchmark::ClobberMemory();
            }
        }
        state.popAllStateSets();
        for (auto& ss : stack)
            VirtualProgram::get(ss)->releaseGLObjects(&state);
        state.setLastAppliedProgramObject(nullptr);
        VirtualProgram::PolyShader::clearShaderCache();
        context->releaseContext();
    }
    BENCHMARK(applyWarm)->Name("VirtualProgram/ApplyWarm")->Arg(1)->Arg(4)->Arg(8)->UseRealTime();
}
