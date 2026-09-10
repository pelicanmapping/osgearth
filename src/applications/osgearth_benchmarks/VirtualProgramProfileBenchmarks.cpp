/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <benchmark/benchmark.h>
#include <osgEarth/Capabilities>
#include <osgEarth/Registry>
#include <osgEarth/VirtualProgram>
#include <osg/GraphicsContext>
#include <osg/FrameStamp>
#include <osg/StateSet>
#include <cstring>

using namespace osgEarth;

namespace
{
    // Diagnostic workloads, not replacements for the original before/after
    // matrix. Components run in isolation and their times are not additive.
    struct ApplyProfile
    {
        osg::ref_ptr<osg::GraphicsContext> context;
        std::vector<osg::ref_ptr<osg::StateSet>> stack;
        std::vector<osg::ref_ptr<VirtualProgram>> programs;
        osg::ref_ptr<const osg::Program> program;
        ProgramRepo::Key key;
        UID user = createUID();

        bool initialize(unsigned defineCount, unsigned bindingCount)
        {
            Capabilities::get();
            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            traits->readDISPLAY();
            traits->setUndefinedScreenDetailsToDefaultScreen();
            traits->width = traits->height = 16;
            traits->pbuffer = true;
            traits->doubleBuffer = false;
            context = osg::GraphicsContext::createGraphicsContext(traits);
            if (!context || !context->realize() || !context->makeCurrent())
                return false;
            state().setUseVertexAttributeAliasing(true);
            state().setUseModelViewAndProjectionUniforms(true);

            std::string imports;
            std::vector<std::string> defines;
            for (unsigned i = 0; i < defineCount; ++i)
            {
                defines.push_back("OE_PROFILE_DEFINE_" + std::to_string(i));
                if (i > 0) imports += ",";
                imports += defines.back();
            }
            if (!imports.empty()) imports = "#pragma import_defines(" + imports + ")\n";

            for (unsigned depth = 0; depth < 4; ++depth)
            {
                osg::ref_ptr<osg::StateSet> ss = new osg::StateSet;
                auto vp = VirtualProgram::getOrCreate(ss);
                for (unsigned i = 0; i < 8; ++i)
                {
                    std::string name = "profile_" + std::to_string(depth) + "_" + std::to_string(i);
                    vp->setFunction(name, (depth == 0 && i == 0 ? imports : "") +
                        "void " + name + "(inout vec4 v) { v.x += 0.00001; }",
                        VirtualProgram::LOCATION_VERTEX_MODEL, float(depth * 8 + i));
                }
                if (depth == 0)
                {
                    for (const auto& define : defines)
                        ss->setDefine(define, "1");
                    for (unsigned i = 0; i < bindingCount; ++i)
                        vp->addBindAttribLocation("profile_attribute_" + std::to_string(i), i % 8);
                }
                stack.push_back(ss);
                programs.push_back(vp);
                state().pushStateSet(ss);
            }
            programs.back()->apply(state());
            auto pcp = state().getLastAppliedProgramObject();
            if (!pcp || !pcp->isLinked()) return false;
            program = pcp->getProgram();
            for (const auto& define : defines)
                if (pcp->getDefineString().find("#define " + define + " 1") == std::string::npos)
                    return false;
            for (const auto& entry : Registry::programRepo().copy())
                if (entry.second->_program.get() == program.get())
                {
                    key = entry.first;
                    break;
                }
            if (key.empty()) return false;
            std::lock_guard<ProgramRepo> lock(Registry::programRepo());
            return Registry::programRepo().use(key, 0, user).valid();
        }

        osg::State& state() { return *context->getState(); }

        ~ApplyProfile()
        {
            if (!context || !context->getState()) return;
            state().popAllStateSets();
            state().setLastAppliedProgramObject(nullptr);
            {
                std::lock_guard<ProgramRepo> lock(Registry::programRepo());
                Registry::programRepo().release(user, &state());
            }
            for (const auto& vp : programs)
                vp->releaseGLObjects(&state());
            program = nullptr;
            programs.clear();
            stack.clear();
            VirtualProgram::PolyShader::clearShaderCache();
            context->releaseContext();
        }
    };

    enum class Work { Apply, ApplyExposed, ApplyFrames, MergeShaders, RepoHit, GetPCP, Bookkeeping };

    // Exploration only: production apply() is unchanged. This is the current
    // define-text validator, copied here for a direct component comparison.
    bool matchDefineText(osg::State& state, const osg::Program& program, const std::string& text)
    {
        auto& defines = state.getDefineMap();
        if (defines.changed)
        {
            static const osg::ShaderDefines empty;
            state.getDefineString(empty);
        }
        auto sd = program.getShaderDefines().begin();
        auto cd = defines.currentDefines.begin();
        std::size_t offset = 0;
        auto match = [&](const char* data, std::size_t size) {
            if (size > text.size() - offset || std::memcmp(text.data() + offset, data, size) != 0)
                return false;
            offset += size;
            return true;
        };
        while (sd != program.getShaderDefines().end() && cd != defines.currentDefines.end())
        {
            const int order = sd->compare(cd->first);
            if (order < 0) ++sd;
            else if (order > 0) ++cd;
            else
            {
                const auto& value = cd->second.first;
                if (!match("#define ", 8) || !match(cd->first.data(), cd->first.size())) return false;
                if (!value.empty())
                {
                    if (value.front() != '(' && !match(" ", 1)) return false;
                    if (!match(value.data(), value.size())) return false;
                }
#ifdef WIN32
                if (!match("\r\n", 2)) return false;
#else
                if (!match("\n", 1)) return false;
#endif
                ++sd;
                ++cd;
            }
        }
        return offset == text.size();
    }

    // A flat snapshot for one immutable program's imports and one PCP's values.
    // Record absent imports too, so enabling one later cannot reuse the old PCP.
    struct DefineSnapshot
    {
        struct Entry { std::string name, value; bool present; };
        std::vector<Entry> entries;

        DefineSnapshot(osg::State& state, const osg::Program& program)
        {
            static const osg::ShaderDefines empty;
            state.getDefineString(empty);
            const auto& current = state.getDefineMap().currentDefines;
            for (const auto& name : program.getShaderDefines())
            {
                auto i = current.find(name);
                entries.push_back({name, i != current.end() ? i->second.first : "", i != current.end()});
            }
        }

        // A push/pop can leave changed=true even when the final effective map
        // equals the previous one. Prove full equality before retaining it.
        // Leaving changed=true would cause State::apply() to reapply the program.
        static void refresh(osg::State& state, bool reuseEqualMap)
        {
            auto& defines = state.getDefineMap();
            if (!defines.changed) return;
            if (reuseEqualMap)
            {
                auto current = defines.currentDefines.begin();
                bool equal = true;
                for (const auto& definition : defines.map)
                {
                    const auto& stack = definition.second.defineVec;
                    if (stack.empty() || !(stack.back().second & osg::StateAttribute::ON)) continue;
                    if (current == defines.currentDefines.end() ||
                        current->first != definition.first || current->second != stack.back())
                    {
                        equal = false;
                        break;
                    }
                    ++current;
                }
                if (equal && current == defines.currentDefines.end())
                {
                    defines.changed = false;
                    return;
                }
            }
            static const osg::ShaderDefines empty;
            state.getDefineString(empty);
        }

        bool matches(osg::State& state, bool reuseEqualMap) const
        {
            refresh(state, reuseEqualMap);
            const auto& current = state.getDefineMap().currentDefines;
            auto i = current.begin();
            for (const auto& expected : entries)
            {
                const osg::StateSet::DefinePair* actual = nullptr;
                while (i != current.end())
                {
                    const int order = expected.name.compare(i->first);
                    if (order > 0) { ++i; continue; }
                    if (order == 0) { actual = &i->second; ++i; }
                    break;
                }
                if (bool(actual) != expected.present || (actual && actual->first != expected.value))
                    return false;
            }
            return true;
        }
    };

    enum class DefineWork { Text, TextRefresh, Flat, FlatRefresh };

    template<DefineWork work>
    void profileDefines(benchmark::State& bench)
    {
        ApplyProfile fixture;
        if (!fixture.initialize(unsigned(bench.range(0)), 0))
        {
            bench.SkipWithError("A linked OpenGL program is required");
            return;
        }
        auto& state = fixture.state();
        osg::ref_ptr<osg::StateSet> unrelated = new osg::StateSet;
        for (unsigned i = 0; i < unsigned(bench.range(1)); ++i)
            unrelated->setDefine("AA_UNRELATED_" + std::to_string(i), "1");
        fixture.stack.push_back(unrelated); // State keeps non-owning stack pointers.
        state.pushStateSet(unrelated);
        DefineSnapshot snapshot(state, *fixture.program);
        const auto text = state.getDefineString(fixture.program->getShaderDefines());
        auto validate = [&]() {
            if (work == DefineWork::TextRefresh) DefineSnapshot::refresh(state, true);
            if (work == DefineWork::Text || work == DefineWork::TextRefresh)
                return matchDefineText(state, *fixture.program, text);
            return snapshot.matches(state, work == DefineWork::FlatRefresh);
        };

        // Compare against OSG before timing. Restore the entire DefineMap so the
        // candidate sees the original dirty flag, including consumed-flag cases.
        auto checkSnapshot = [&](const DefineSnapshot& candidate, const std::string& candidateText) {
            auto before = state.getDefineMap();
            const bool expected = state.getDefineString(fixture.program->getShaderDefines()) == candidateText;
            const auto expectedMap = state.getDefineMap().currentDefines;
            state.getDefineMap() = before;
            if (work == DefineWork::TextRefresh) DefineSnapshot::refresh(state, true);
            const bool actual = work == DefineWork::Text || work == DefineWork::TextRefresh ?
                matchDefineText(state, *fixture.program, candidateText) :
                candidate.matches(state, work == DefineWork::FlatRefresh);
            return actual == expected && !state.getDefineMap().changed &&
                state.getDefineMap().currentDefines == expectedMap;
        };
        auto check = [&]() { return checkSnapshot(snapshot, text); };
        bool correct = check();
        osg::ref_ptr<osg::StateSet> transient = new osg::StateSet;
        const std::string name = bench.range(0) > 0 ? "OE_PROFILE_DEFINE_0" : "UNIMPORTED";
        for (const std::string& value : {std::string("2"), std::string(), std::string("(x) ((x) + 1)")})
        {
            transient->setDefine(name, value);
            state.pushStateSet(transient);
            correct &= check();
            state.popStateSet();
            correct &= check();
        }
        transient->setDefine(name, "1", osg::StateAttribute::OFF);
        state.pushStateSet(transient);
        correct &= check();
        DefineSnapshot absent(state, *fixture.program);
        const auto absentText = state.getDefineString(fixture.program->getShaderDefines());
        state.popStateSet();
        correct &= checkSnapshot(absent, absentText);
        correct &= check();
        transient->setDefine(name, "2", osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        state.pushStateSet(transient);
        osg::ref_ptr<osg::StateSet> protectedValue = new osg::StateSet;
        protectedValue->setDefine(name, "1");
        state.pushStateSet(protectedValue);
        correct &= check();
        state.popStateSet();
        protectedValue->setDefine(name, "1", osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);
        state.pushStateSet(protectedValue);
        correct &= check();
        state.popStateSet();
        state.popStateSet();
        correct &= check();
        transient->setDefine(name, "2");
        state.pushStateSet(transient);
        state.getDefineString(fixture.program->getShaderDefines()); // consume changed
        correct &= check();
        state.popStateSet();
        correct &= check();
        // Dirty but back to the original effective values, plus a popped name.
        transient->setDefine("NEW_UNRELATED", "3");
        state.pushStateSet(transient);
        state.popStateSet();
        correct &= check();
        if (!correct)
        {
            bench.SkipWithError("Define validation or effective-map contents differed from OSG");
            return;
        }

        bench.counters["active_defines"] = double(bench.range(0));
        bench.counters["unrelated_defines"] = double(bench.range(1));
        bench.counters["imported_names"] = double(snapshot.entries.size());
        bench.counters["effective_defines"] = double(state.getDefineMap().currentDefines.size());
        bench.counters["round_trip"] = double(bench.range(2));
        for (auto _ : bench)
        {
            if (bench.range(2))
            {
                state.pushStateSet(transient);
                state.popStateSet();
            }
            benchmark::DoNotOptimize(validate());
            benchmark::ClobberMemory();
        }
    }

    BENCHMARK_TEMPLATE(profileDefines, DefineWork::Text)->Name("VirtualProgramDefines/Text")
        ->Args({0, 0, 0})->Args({8, 0, 0})->Args({32, 0, 0})->Args({32, 128, 0})
        ->Args({32, 0, 1})->Args({32, 128, 1})->UseRealTime();
    BENCHMARK_TEMPLATE(profileDefines, DefineWork::Flat)->Name("VirtualProgramDefines/Flat")
        ->Args({0, 0, 0})->Args({8, 0, 0})->Args({32, 0, 0})->Args({32, 128, 0})
        ->Args({32, 0, 1})->Args({32, 128, 1})->UseRealTime();
    BENCHMARK_TEMPLATE(profileDefines, DefineWork::FlatRefresh)->Name("VirtualProgramDefines/FlatRefresh")
        ->Args({0, 0, 0})->Args({8, 0, 0})->Args({32, 0, 0})->Args({32, 128, 0})
        ->Args({32, 0, 1})->Args({32, 128, 1})->UseRealTime();
    BENCHMARK_TEMPLATE(profileDefines, DefineWork::TextRefresh)->Name("VirtualProgramDefines/TextRefresh")
        ->Args({0, 0, 0})->Args({8, 0, 0})->Args({32, 0, 0})->Args({32, 128, 0})
        ->Args({32, 0, 1})->Args({32, 128, 1})->UseRealTime();

    template<Work work>
    void profile(benchmark::State& bench)
    {
        ApplyProfile fixture;
        if (!fixture.initialize(unsigned(bench.range(0)), unsigned(bench.range(1))))
        {
            bench.SkipWithError("A linked OpenGL program with the requested defines is required");
            return;
        }
        bench.counters["key_words"] = double(fixture.key.size());
        bench.counters["active_defines"] = double(bench.range(0));
        bench.counters["bindings"] = double(bench.range(1));
        VirtualProgram::ShaderMap accumulated;
        auto& repo = Registry::programRepo();
        auto& state = fixture.state();
        if (work == Work::ApplyExposed)
        {
            for (const auto& vp : fixture.programs)
            {
                VirtualProgram::ShaderMap shaders;
                vp->getShaderMap(shaders);
                for (const auto& shader : shaders)
                    benchmark::DoNotOptimize(shader.second._shader->getNominalShader());
            }
            // Warm after obtaining all raw pointers. Pointer access alone must
            // have no steady-state cost under the explicit dirty() contract.
            fixture.programs.back()->apply(state);
            fixture.programs.back()->apply(state);
        }
        osg::ref_ptr<osg::FrameStamp> frame;
        if (work == Work::ApplyFrames)
        {
            frame = new osg::FrameStamp;
            state.setFrameStamp(frame);
        }
        unsigned draw = 0;
        for (auto _ : bench)
        {
            // The template argument removes unused branches from each benchmark.
            if (work == Work::Apply || work == Work::ApplyExposed)
                fixture.programs.back()->apply(state);
            else if (work == Work::ApplyFrames)
            {
                if ((draw++ & 1023) == 0) frame->setFrameNumber(frame->getFrameNumber() + 1);
                fixture.programs.back()->apply(state);
            }
            else if (work == Work::MergeShaders)
            {
                accumulated.clear();
                for (const auto& vp : fixture.programs)
                    vp->addShadersToAccumulationMap(accumulated, state);
                benchmark::DoNotOptimize(accumulated.size());
            }
            else if (work == Work::RepoHit)
            {
                std::lock_guard<ProgramRepo> lock(repo);
                auto program = repo.use(fixture.key, 0, fixture.user);
                benchmark::DoNotOptimize(program.get());
            }
            else if (work == Work::GetPCP)
                benchmark::DoNotOptimize(fixture.program->getPCP(state));
            else if (work == Work::Bookkeeping)
                state.haveAppliedAttribute(VirtualProgram::SA_TYPE);
            benchmark::ClobberMemory();
        }
    }

    BENCHMARK_TEMPLATE(profile, Work::Apply)->Name("VirtualProgramProfile/Apply")
        ->Args({0, 0})->Args({8, 0})->Args({32, 0})
        ->Args({0, 8})->Args({0, 32})->Args({32, 32})->UseRealTime();
    BENCHMARK_TEMPLATE(profile, Work::GetPCP)->Name("VirtualProgramProfile/GetPCP")
        ->Args({0, 0})->Args({8, 0})->Args({32, 0})->UseRealTime();
    BENCHMARK_TEMPLATE(profile, Work::MergeShaders)->Name("VirtualProgramProfile/MergeShaders")
        ->Args({0, 0})->UseRealTime();
    BENCHMARK_TEMPLATE(profile, Work::RepoHit)->Name("VirtualProgramProfile/RepoHit")
        ->Args({0, 0})->Args({0, 32})->UseRealTime();
    BENCHMARK_TEMPLATE(profile, Work::Bookkeeping)->Name("VirtualProgramProfile/Bookkeeping")
        ->Args({0, 0})->UseRealTime();
    // Compare these against the content-validation baseline when measuring the
    // explicit dirty() contract. Both use the same 32 exposed shaders.
    BENCHMARK_TEMPLATE(profile, Work::ApplyExposed)->Name("VirtualProgramProfile/ApplyExposed")
        ->Args({0, 0})->Args({32, 32})->UseRealTime();
    BENCHMARK_TEMPLATE(profile, Work::ApplyFrames)->Name("VirtualProgramProfile/ApplyFrames")
        ->Args({0, 0})->Args({32, 32})->UseRealTime();
}
