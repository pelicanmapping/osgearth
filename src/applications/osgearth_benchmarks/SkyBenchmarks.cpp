/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <benchmark/benchmark.h>
#include <osgEarth/Capabilities>
#include <osgEarth/MapNode>
#include <osgEarth/Sky>
#include <osgEarth/VirtualProgram>
#include <osg/GraphicsContext>
#include <osg/FrameBufferObject>
#include <osg/GLDefines>
#include <osg/Texture3D>
#include <osgUtil/UpdateVisitor>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

using namespace osgEarth;

namespace
{
    // Diagnostic shader variants only. Production shaders are not modified.
    enum Variant { Current, Disabled, ZeroStrength, GuardedZero, PackedSH, DiffuseOnly };

    void replace(std::string& source, const std::string& from, const std::string& to)
    {
        auto pos = source.find(from);
        if (pos == std::string::npos) throw std::runtime_error("Sky benchmark source no longer matches: " + from);
        source.replace(pos,from.size(),to);
    }

    std::string body(const std::string& source)
    {
        std::istringstream input(source);
        std::string line, result;
        while (std::getline(input,line))
            if (line.find("#version") == std::string::npos && line.find("#pragma") == std::string::npos)
                result += line + '\n';
        return result;
    }

    struct Fixture
    {
        osg::ref_ptr<osg::GraphicsContext> gc;
        osg::ref_ptr<osg::View> view = new osg::View;
        osg::ref_ptr<SkyNode> sky;
        osg::ref_ptr<osg::FrameStamp> frame = new osg::FrameStamp;
        osg::ref_ptr<osg::Texture> environment, brdf;
        osg::ref_ptr<osg::Program> program;
        osg::GLExtensions* gl = nullptr;
        osg::Vec3d sun;
        int environmentUnit = 0, brdfUnit = 1;
        GLuint vao = 0, fbo = 0, target = 0, query = 0;
        unsigned width, height;
        bool glossy;
        std::string groundSource, environmentSource;

        Fixture(unsigned w = 1920, unsigned h = 1080, bool metal = false) : width(w), height(h), glossy(metal) { }

        bool initialize()
        {
            Capabilities::get();
            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
            traits->width = traits->height = 16;
            traits->pbuffer = true; traits->doubleBuffer = false;
            gc = osg::GraphicsContext::createGraphicsContext(traits);
            if (!gc || !gc->realize() || !gc->makeCurrent()) return false;
            gl = state().get<osg::GLExtensions>();
            if (!gl->glGetQueryObjectui64v) return false;
            static bool reported = false;
            if (!reported)
            {
                std::cerr << "Sky benchmark GPU: " << glGetString(GL_RENDERER) << "; GL " << glGetString(GL_VERSION) << '\n';
                reported = true;
            }
            osg::ref_ptr<MapNode> map = new MapNode;
            if (!map->open()) return false;
            Config config;
            config.set("environment_lighting",true);
            SkyOptions options(config); options.setDriver("simple");
            options.quality() = SkyOptions::QUALITY_MEDIUM;
            sky = SkyNode::create(options);
            if (!sky) return false;
            sky->addChild(map); sky->attach(view,0);
            const auto& p = view->getLight()->getPosition();
            sun.set(p.x(),p.y(),p.z()); sun.normalize();
            update(sun);
            auto ss = sky->getStateSet();
            if (!ss->getUniform("oe_sky_environmentTex")) return false;
            ss->getUniform("oe_sky_environmentTex")->get(environmentUnit);
            ss->getUniform("oe_sky_brdfTex")->get(brdfUnit);
            environment = static_cast<osg::Texture*>(ss->getTextureAttribute(environmentUnit,osg::StateAttribute::TEXTURE));
            brdf = static_cast<osg::Texture*>(ss->getTextureAttribute(brdfUnit,osg::StateAttribute::TEXTURE));
            auto vp = VirtualProgram::get(ss);
            groundSource = body(vp->getPolyShader("atmos_fragment_main")->getShaderSource());
            environmentSource = body(vp->getPolyShader("oe_sky_environment_init")->getShaderSource());

            gl->glGenVertexArrays(1,&vao); gl->glBindVertexArray(vao);
            gl->glGenFramebuffers(1,&fbo); gl->glBindFramebuffer(GL_FRAMEBUFFER_EXT,fbo);
            glGenTextures(1,&target); glBindTexture(GL_TEXTURE_2D,target);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA16F_ARB,width,height,0,GL_RGBA,GL_FLOAT,nullptr);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            gl->glFramebufferTexture2D(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D,target,0);
            if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) return false;
            glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT); glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
            glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
            gl->glGenQueries(1,&query);
            state().setActiveTextureUnit(environmentUnit); environment->apply(state());
            state().setActiveTextureUnit(brdfUnit); brdf->apply(state());
            return glGetError() == GL_NO_ERROR;
        }

        osg::State& state() { return *gc->getState(); }
        void update(const osg::Vec3d& up)
        {
            view->getCamera()->setViewMatrix(osg::Matrixd::translate(up*(-7000000.0)));
            osgUtil::UpdateVisitor visitor;
            frame->setFrameNumber(frame->getFrameNumber()+1); visitor.setFrameStamp(frame);
            sky->accept(visitor);
        }

        bool select(Variant variant)
        {
            std::string env = environmentSource, ground = groundSource;
            if (variant == DiffuseOnly)
                replace(env,"(diffuse+specular)","diffuse");
            if (variant == PackedSH)
            {
                const char* constants[] = {"0.282095","0.488603","0.488603","0.488603","1.092548","1.092548","0.315392","1.092548","0.546274"};
                for (auto value : constants) replace(env,value,"1.0");
            }
            if (variant == GuardedZero)
            {
                replace(ground,"    vec3 environment = oe_sky_environment", "    if (oe_sky_iblStrength > 0.0 && osg_LightSource[0].enabled) {\n    vec3 environment = oe_sky_environment");
                replace(ground,"clamp(oe_sky_iblStrength, 0.0, 1.0));", "clamp(oe_sky_iblStrength, 0.0, 1.0));\n    }");
            }
            std::string defines = "#version 330\n#define OE_LIGHTING\n#define OE_USE_PBR\n#define OE_NUM_LIGHTS 1\n";
            if (variant != Disabled) defines += "#define OE_SKY_ENVIRONMENT\n";
            osg::ref_ptr<osg::Program> next = new osg::Program;
            next->addShader(new osg::Shader(osg::Shader::VERTEX,R"(#version 330
out vec3 atmos_lightDir, atmos_color, atmos_atten, atmos_up, vp_Normal, vp_VertexView;
out float atmos_space;
void main() {
    vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2)*2.0-1.0;
    gl_Position=vec4(p,0,1);
    vp_Normal=vec3(p*0.5,1.0); vp_VertexView=vec3(p*2.0,-3.0);
    atmos_up=vec3(0,0,1); atmos_color=vec3(0.08,0.10,0.12);
    atmos_lightDir=vec3(0,0,1); atmos_atten=vec3(1); atmos_space=0;
})"));
            next->addShader(new osg::Shader(osg::Shader::FRAGMENT,defines+env+ground+R"(
uniform float benchmarkRoughness, benchmarkMetal;
out vec4 result;
void main() {
    oe_pbr.roughness=benchmarkRoughness; oe_pbr.metal=benchmarkMetal; oe_pbr.ao=1.0;
    result=vec4(0.35,0.55,0.25,1.0); atmos_fragment_main(result);
})"));
            next->apply(state());
            auto pcp = state().getLastAppliedProgramObject();
            if (!pcp || !pcp->isLinked()) return false;
            if (program) program->releaseGLObjects(&state());
            program = next;
            auto location = [&](const char* name) { return gl->glGetUniformLocation(pcp->getHandle(),name); };
            gl->glUniform1i(location("oe_sky_environmentTex"),environmentUnit);
            gl->glUniform1i(location("oe_sky_brdfTex"),brdfUnit);
            gl->glUniform1f(location("benchmarkRoughness"),glossy ? 0.25f : 1.0f);
            gl->glUniform1f(location("benchmarkMetal"),glossy ? 1.0f : 0.0f);
            gl->glUniform1f(location("oe_sky_iblStrength"),variant == ZeroStrength || variant == GuardedZero ? 0.0f : 1.0f);
            gl->glUniform1f(location("oe_sky_maxAmbientIntensity"),0.15f);
            gl->glUniform1f(location("oe_sky_exposure"),3.3f);
            gl->glUniform1i(location("osg_LightSource[0].enabled"),1);
            gl->glUniform4f(location("osg_LightSource[0].position"),0.2f,0.3f,1.0f,0.0f);
            gl->glUniform4f(location("osg_LightSource[0].diffuse"),1,1,1,1);
            gl->glUniform4f(location("osg_LightSource[0].ambient"),0.033f,0.033f,0.033f,1);
            osg::Matrixf matrix;
            gl->glUniformMatrix4fv(location("osg_ViewMatrixInverse"),1,GL_FALSE,matrix.ptr());
            const float scale[] = {0.282095f,0.488603f,0.488603f,0.488603f,1.092548f,1.092548f,0.315392f,1.092548f,0.546274f};
            for (unsigned i=0; i<9; ++i)
            {
                osg::Vec3 c; sky->getStateSet()->getUniform("oe_sky_irradiance")->getElement(i,c);
                if (variant == PackedSH) c *= scale[i];
                std::string name = "oe_sky_irradiance[" + std::to_string(i) + "]";
                gl->glUniform3f(location(name.c_str()),c.x(),c.y(),c.z());
            }
            return glGetError() == GL_NO_ERROR;
        }

        std::vector<float> pixels()
        {
            glViewport(0,0,64,64); glDrawArrays(GL_TRIANGLES,0,3);
            std::vector<float> result(64*64*4);
            glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,result.data());
            return result;
        }

        double draw()
        {
            gl->glBeginQuery(GL_TIME_ELAPSED,query);
            for (int i=0; i<8; ++i) glDrawArrays(GL_TRIANGLES,0,3);
            gl->glEndQuery(GL_TIME_ELAPSED);
            GLuint64 time;
            gl->glGetQueryObjectui64v(query,GL_QUERY_RESULT,&time);
            return double(time)*1e-9/8.0;
        }

        ~Fixture()
        {
            if (!gc || !gl) return;
            state().setLastAppliedProgramObject(nullptr);
            if (program) program->releaseGLObjects(&state());
            if (sky) sky->releaseGLObjects(&state());
            if (query) gl->glDeleteQueries(1,&query);
            gl->glBindFramebuffer(GL_FRAMEBUFFER_EXT,0);
            if (fbo) gl->glDeleteFramebuffers(1,&fbo);
            if (target) glDeleteTextures(1,&target);
            gl->glBindVertexArray(0);
            if (vao) gl->glDeleteVertexArrays(1,&vao);
            gc->releaseContext();
        }
    };

    void shading(benchmark::State& bench, Variant variant)
    {
        Fixture f(unsigned(bench.range(0)),unsigned(bench.range(0))*9/16,bench.range(1)!=0);
        if (!f.initialize()) { bench.SkipWithError("Could not initialize sky GL fixture"); return; }
        Variant reference = variant == GuardedZero ? ZeroStrength : Current;
        if (!f.select(reference)) { bench.SkipWithError("Reference shader failed"); return; }
        auto before = f.pixels();
        if (!f.select(variant)) { bench.SkipWithError("Variant shader failed"); return; }
        auto after = f.pixels();
        double maximum = 0, square = 0;
        for (unsigned i=0; i<before.size(); ++i)
        {
            double error = std::abs(double(before[i])-after[i]);
            maximum = std::max(maximum,error); square += error*error;
        }
        if ((variant == PackedSH || variant == GuardedZero) && maximum > 0.001)
        { bench.SkipWithError("Equivalent prototype changed the rendered image"); return; }
        glViewport(0,0,f.width,f.height);
        for (int i=0; i<10; ++i) f.draw();
        for (auto _ : bench) bench.SetIterationTime(f.draw());
        bench.counters["max_error"] = maximum;
        bench.counters["rmse"] = std::sqrt(square/before.size());
        if (glGetError() != GL_NO_ERROR) bench.SkipWithError("OpenGL error during shader benchmark");
    }

    void stationary(benchmark::State& bench)
    {
        Fixture f;
        if (!f.initialize()) { bench.SkipWithError("Could not initialize sky fixture"); return; }
        auto image = f.environment->getImage(0);
        for (auto _ : bench) f.update(f.sun);
        if (image != f.environment->getImage(0)) bench.SkipWithError("Stationary sky unexpectedly regenerated its probe");
    }

    void regenerate(benchmark::State& bench)
    {
        Fixture f;
        if (!f.initialize()) { bench.SkipWithError("Could not initialize sky fixture"); return; }
        osg::Vec3d other = f.sun + osg::Vec3d(0.2,0.1,0.05); other.normalize();
        unsigned iteration = 0;
        for (auto _ : bench)
        {
            bench.PauseTiming();
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            auto image = f.environment->getImage(0);
            bench.ResumeTiming();
            f.update((iteration++%2) == 0 ? other : f.sun);
            if (image == f.environment->getImage(0)) { bench.SkipWithError("Probe did not regenerate"); break; }
        }
    }

    BENCHMARK_CAPTURE(shading, Current, Current)->Args({1920,0})->Args({3840,0})->Args({1920,1})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(shading, Disabled, Disabled)->Args({1920,0})->Args({3840,0})->Args({1920,1})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(shading, ZeroStrength, ZeroStrength)->Args({1920,0})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(shading, GuardedZero, GuardedZero)->Args({1920,0})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(shading, PackedSH, PackedSH)->Args({1920,0})->Args({3840,0})->Args({1920,1})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK_CAPTURE(shading, DiffuseOnly, DiffuseOnly)->Args({1920,0})->Args({3840,0})->Args({1920,1})->UseManualTime()->Unit(benchmark::kMillisecond);
    BENCHMARK(stationary)->Name("SimpleSky/StationaryUpdate")->UseRealTime();
    BENCHMARK(regenerate)->Name("SimpleSky/RegenerateProbe")->Iterations(5)->UseRealTime()->Unit(benchmark::kMillisecond);
}
