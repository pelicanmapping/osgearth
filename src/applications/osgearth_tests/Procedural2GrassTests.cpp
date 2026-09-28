/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarthProcedural2/VegetationLayer2>
#include <osgEarthProcedural2/Grass.h>
#include "ChonkTestUtils.h"
#include <cstdlib>
#include <cmath>
#include <limits>

namespace osgEarth { namespace Tests { extern std::string executablePath; } }
using namespace osgEarth;
using namespace osgEarth::Procedural2;

//! Optional rendering settings round-trip, reject invalid policies, and preserve the source's placement identity.
TEST_CASE("Procedural2 grass is optional and preserves source placement", "[procedural2]")
{
    VegetationLayer2::Options options;
    for (const auto& value : options.groups()) CHECK_FALSE(value.proceduralGrass);
    auto group = options.groups()[2];
    group.density = 250000;
    const auto profile = Profile::create("global-geodetic");
    const auto key = profile->createTileKey(-75.0, 40.65, group.cellLevel);
    UniformScatterSource source;
    std::vector<ScatterPlacement> original, procedural;
    REQUIRE(source.generate(key, group, 17, original).isOK());
    group.proceduralGrass = true;
    group.grassBlades = 97;
    group.grassRadius = 2.0f;
    group.grassHeight = 1.0f;
    group.grassWidth = 0.04f;
    group.grassWind = 0.0f;
    ScatterGroup restored(group.getConfig());
    CHECK(restored.getConfig().toJSON() == group.getConfig().toJSON());
    REQUIRE(source.generate(key, restored, 17, procedural).isOK());
    REQUIRE(original.size() == procedural.size());
    REQUIRE_FALSE(original.empty());
    for (std::size_t i = 0; i < original.size(); ++i)
    {
        CHECK(original[i].id == procedural[i].id);
        CHECK(original[i].point == procedural[i].point);
        CHECK(original[i].variationSeed() == procedural[i].variationSeed());
        CHECK(original[i].variationSeed() < (1u << 24u));
        CHECK(unsigned(float(original[i].variationSeed())) == original[i].variationSeed());
    }
    options.profile() = ProfileOptions("global-geodetic");
    osg::ref_ptr<VegetationLayer2> layer = new VegetationLayer2(options);
    REQUIRE(layer->setGroup(group).isOK());
    auto invalid = group;
    invalid.grassBlades = 129u;
    REQUIRE(layer->setGroup(invalid).isError());
    CHECK(layer->options().groups()[2].grassBlades == 97u);
    invalid = group;
    invalid.asset = "trees";
    CHECK(invalid.validate().isError());
    invalid = group;
    invalid.grassRadius = std::numeric_limits<float>::infinity();
    CHECK(invalid.validate().isError());
    invalid = group;
    invalid.grassWind = -1.0f;
    CHECK(invalid.validate().isError());
    group.proceduralGrass = false;
    REQUIRE(layer->setGroup(group).isOK());
    CHECK_FALSE(layer->options().groups()[2].proceduralGrass);
}

//! Isolates the actual blade shader test from viewers that reuse process-global Chonk context resources.
TEST_CASE("Procedural2 generated grass GPU contract", "[procedural2][gpu]")
{
    const auto& executable = osgEarth::Tests::executablePath;
#ifdef _WIN32
    const std::string command = "\"\"" + executable + "\" \"[.procedural2-grass-gpu-worker]\"\"";
#else
    std::string quoted = "'";
    for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    const std::string command = quoted + "' '[.procedural2-grass-gpu-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
}

//! Reads real generated vertices to check culling bounds, LOD roots, wind anchoring, normals, and seed stability.
TEST_CASE("Procedural2 grass shader validation", "[.procedural2-grass-gpu-worker]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Procedural grass GPU validation requires NVGL and an OpenGL 4.6 context");
        return;
    }
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    auto* state = renderer.context->getState();
    auto* ss = renderer.root->getOrCreateStateSet();
    ss->getUniform("oe_sse")->set(1.0f);
    ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    ScatterGroup group;
    group.asset = group.name = "grass";
    group.proceduralGrass = true;
    group.grassBlades = 64u;
    group.grassWind = 1.0f; // Exercise the largest permitted wind displacement against the actual mesh bounds.
    installGrassShader(ss, group);
    auto fixedTime = new osg::Uniform("oe_p2_grass_time", 10.0f);
    ss->addUniform(fixedTime);
    auto asset = Chonk::create();
    REQUIRE(asset->add(createGrassPatch(group, 0u), 10.0f, FLT_MAX, *renderer.factory));
    REQUIRE(asset->add(createGrassPatch(group, 1u), 0.0f, 10.0f, *renderer.factory));
    VirtualProgram::getOrCreate(ss)->setFunction("p2_capture_grass", R"glsl(
        layout(location = 5) in vec3 flex;
        out vec3 vp_Normal;
        struct P2Capture { vec4 position; vec4 normal; };
        layout(binding = 22, std430) buffer P2GrassCapture { P2Capture p2Captured[]; };
        // Store by stable blade/template coordinate, independent of Chonk's packed vertex offsets.
        void p2_capture_grass(inout vec4 vertex)
        {
            uint i = uint(flex.x)*10u + uint(round(flex.y*4.0))*2u + uint(flex.z > 0.0);
            p2Captured[i].position = vertex;
            p2Captured[i].normal = vec4(vp_Normal,1.0);
        }
    )glsl", VirtualProgram::LOCATION_VERTEX_MODEL, 0.3f);
    struct Captured { osg::Vec4f position, normal; };
    auto buffer = GLBuffer::create(GL_SHADER_STORAGE_BUFFER, *state);
    // Generate one patch through real GPU culling and capture its expanded vertices before projection.
    auto capture = [&](unsigned seed, float rank, double time, bool coarse)
    {
        std::vector<Captured> result(group.grassBlades * 10u);
        buffer->bind();
        buffer->bufferData(result, GL_DYNAMIC_READ);
        buffer->bindBufferBase(22);
        osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable();
        drawable->add(asset, osg::Matrixf(), osg::Vec2f(rank, float(seed)));
        renderer.setScene(drawable);
        ss->getUniform("oe_sse")->set(coarse ? 1000.0f : 1.0f);
        renderer.viewer.getCamera()->setViewMatrixAsLookAt(osg::Vec3d(0,-8,5), osg::Vec3d(), osg::Vec3d(0,0,1));
        renderer.time = time;
        fixedTime->set(float(time));
        renderer.frame();
        renderer.time = time;
        renderer.frame();
        state->get<osg::GLExtensions>()->glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        buffer->bind();
        buffer->getBufferSubData(0, GLsizei(result.size() * sizeof(Captured)), result.data());
        REQUIRE(glGetError() == GL_NO_ERROR);
        return result;
    };
    const auto near = capture(123456u, 0.2f, 10.0, false);
    const auto repeated = capture(123456u, 0.9f, 10.0, false);
    const auto windy = capture(123456u, 0.2f, 11.0, false);
    const auto coarse = capture(123456u, 0.2f, 10.0, true);
    const auto other = capture(765432u, 0.2f, 10.0, false);
    unsigned coarseVertices = 0;
    float windMotion = 0.0f, seedMotion = 0.0f;
    for (unsigned i = 0; i < near.size(); ++i)
    {
        REQUIRE(near[i].position.w() == 1.0f); // Fails if shader compilation/drawing silently failed.
        CHECK((near[i].position - repeated[i].position).length() < 1e-5f);
        for (const auto* values : {&near, &windy, &coarse, &other})
        {
            const auto& value = (*values)[i];
            if (value.position.w() == 0.0f) continue;
            const osg::Vec3f point(value.position.x(), value.position.y(), value.position.z());
            CHECK(asset->getBound().contains(point));
            CHECK(std::abs(osg::Vec3f(value.normal.x(), value.normal.y(), value.normal.z()).length()-1.0f) < 1e-4f);
        }
        if (coarse[i].position.w() == 1.0f) ++coarseVertices;
        windMotion += (near[i].position - windy[i].position).length();
        seedMotion += (near[i].position - other[i].position).length();
        if (i % 10u < 2u) CHECK((near[i].position - windy[i].position).length() < 1e-5f);
    }
    CHECK(coarseVertices == 16u * 6u);
    CHECK(windMotion > 1.0f);
    CHECK(seedMotion > 1.0f);
    for (unsigned blade = 0; blade < 16u; ++blade)
    {
        const unsigned i = blade * 10u;
        const auto nearRoot = (near[i].position + near[i+1u].position) * 0.5f;
        const auto farRoot = (coarse[i].position + coarse[i+1u].position) * 0.5f;
        CHECK((nearRoot - farRoot).length() < 1e-5f);
    }
    state->get<osg::GLExtensions>()->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 22, 0);
}
