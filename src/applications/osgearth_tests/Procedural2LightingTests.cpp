/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "ChonkTestUtils.h"
#include <osgEarth/catch.hpp>
#include <osgEarthProcedural2/AssetLighting.h>
#include <osgEarthProcedural2/CanopyTransition.h>
#include <osgEarthProcedural2/AssetCatalog>
#include <osg/Multisample>
#include <osg/ColorMask>
#include <osgEarthProcedural2/VegetationLayer2>
#include <osgEarth/Map>
#include <cstdlib>
#include <cmath>

using namespace osgEarth;
using namespace osgEarth::Procedural2;
namespace osgEarth { namespace Tests { extern std::string executablePath; } }

//! Records the inherited GL modes when OSG applies the actual vegetation draw state, before frame cleanup.
struct CrownCoverageProbe : osg::ColorMask
{
    mutable bool a2c = false, blend = true, depth = false, write = false, multisample = false;
    mutable unsigned calls = 0;
    mutable GLint samples = 0;
    //! Reads driver state on the graphics thread after mode inheritance; does not change the coverage policy.
    void apply(osg::State& state) const override
    {
        osg::ColorMask::apply(state);
        ++calls;
        multisample = glIsEnabled(GL_MULTISAMPLE_ARB);
        a2c = glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB);
        blend = glIsEnabled(GL_BLEND); depth = glIsEnabled(GL_DEPTH_TEST);
        GLboolean mask = GL_FALSE; glGetBooleanv(GL_DEPTH_WRITEMASK,&mask); write = mask;
        glGetIntegerv(GL_SAMPLES_ARB,&samples);
    }
};

//! Isolates the multisample inheritance regression from other tests that explicitly track global GL modes.
TEST_CASE("Procedural2 preserves application multisampling", "[procedural2][coverage][gpu]")
{
    const auto& executable = osgEarth::Tests::executablePath;
    for (const std::string tag : {"[.procedural2-msaa-single-worker]","[.procedural2-msaa-worker]"})
    {
#ifdef _WIN32
        const std::string command = "\"\""+executable+"\" \""+tag+"\"\"";
#else
        std::string quoted = "'";
        for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1,c);
        const std::string command = quoted+"' '"+tag+"'";
#endif
        REQUIRE(std::system(command.c_str()) == 0);
    }
}

//! Reads real draw state before/inside/after Vegetation2 across frames, inherited policy changes, and layer removal.
static void validateMultisampleInheritance(unsigned samples)
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Coverage validation requires NVGL"); return; }
    CAPTURE(samples);
    osg::DisplaySettings::instance()->setNumMultiSamples(samples);
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    REQUIRE(glIsEnabled(GL_MULTISAMPLE_ARB) == GL_TRUE); // OpenGL default; do not enroll it in OSG's state map.
    VegetationLayer2::Options options;
    options.profile() = ProfileOptions("global-geodetic");
    options.groups().clear();
    osg::ref_ptr<VegetationLayer2> layer = new VegetationLayer2(options);
    REQUIRE(layer->open().isOK());
    osg::ref_ptr<Map> map = new Map();
    map->addLayer(layer);
    layer->setOpacity(1.0f);
    auto* root = layer->getNode()->asGroup();
    REQUIRE(root);
    REQUIRE(root->getNumChildren() == 1u);
    auto* content = root->getChild(0)->asGroup();
    REQUIRE(content);
    auto* vegetation = new CrownCoverageProbe();
    content->getOrCreateStateSet()->setAttribute(vegetation);
    renderer.setScene(root);
    std::array<osg::ref_ptr<CrownCoverageProbe>,2> siblings;
    for (unsigned i=0; i<2; ++i)
    {
        auto mesh = ChonkTest::mesh();
        mesh->setUseVertexArrayObject(true);
        auto* state = mesh->getOrCreateStateSet();
        state->setRenderBinDetails(i == 0 ? 1 : 10,"RenderBin");
        siblings[i] = new CrownCoverageProbe();
        state->setAttribute(siblings[i]);
        renderer.root->addChild(mesh);
    }
    auto mesh = ChonkTest::mesh();
    auto* image = new osg::Image(); image->allocateImage(1,1,1,GL_RGBA,GL_UNSIGNED_BYTE);
    image->data()[0] = image->data()[1] = image->data()[2] = 255; image->data()[3] = 128;
    mesh->getOrCreateStateSet()->setTextureAttribute(0,new osg::Texture2D(image));
    auto model = Chonk::create();
    REQUIRE(model->add(mesh,0,FLT_MAX,*renderer.factory));
    auto* camera = renderer.viewer.getCamera();
    camera->setViewMatrixAsLookAt(osg::Vec3(0,0,8),osg::Vec3(),osg::Vec3(0,1,0));
    // First inherit OpenGL's enabled default; then toggle a tracked application policy ON/OFF/ON.
    for (int policy : {-1,1,0,1})
    {
        INFO("application policy=" << policy);
        if (policy >= 0) renderer.root->getOrCreateStateSet()->setMode(GL_MULTISAMPLE_ARB,
            policy == 0 ? osg::StateAttribute::OFF : osg::StateAttribute::ON);
        for (bool gpu : {false,true})
        {
            auto* drawable = new ChonkDrawable();
            drawable->setUseGPUCulling(gpu);
            drawable->setBirthday(-10);
            drawable->add(model,osg::Matrixf::identity());
            content->removeChildren(0,content->getNumChildren());
            content->addChild(drawable);
            for (unsigned frame=0; frame<3; ++frame)
            {
                vegetation->calls = siblings[0]->calls = siblings[1]->calls = 0;
                renderer.frame();
                REQUIRE(vegetation->calls > 0);
                CHECK(vegetation->multisample == (policy != 0));
                CHECK(vegetation->a2c);
                CHECK_FALSE(vegetation->blend);
                CHECK(vegetation->samples == GLint(samples));
                for (const auto& sibling : siblings)
                {
                    REQUIRE(sibling->calls > 0);
                    CHECK(sibling->multisample == (policy != 0));
                    CHECK_FALSE(sibling->a2c);
                    CHECK(sibling->samples == GLint(samples));
                }
            }
        }
        renderer.root->removeChild(root);
        renderer.frame();
        for (const auto& sibling : siblings)
        {
            CHECK(sibling->multisample == (policy != 0));
            CHECK_FALSE(sibling->a2c);
        }
        renderer.root->addChild(root);
    }
    CHECK(glGetError() == GL_NO_ERROR);
}

//! Exercises the default single-sample framebuffer in its own process/context.
TEST_CASE("Procedural2 single-sample mode inheritance", "[.procedural2-msaa-single-worker]")
{
    validateMultisampleInheritance(0);
}

//! Exercises four-sample MSAA in its own process/context.
TEST_CASE("Procedural2 multisampling is inherited without affecting sibling draws", "[.procedural2-msaa-worker]")
{
    validateMultisampleInheritance(4);
}

//! Validates coverage through the real VisibleLayer parent, with reversed and GPU-compacted instance order.
TEST_CASE("Procedural2 coverage overrides inherited blending", "[.procedural2-coverage-worker]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Coverage validation requires NVGL"); return; }
    osg::DisplaySettings::instance()->setNumMultiSamples(4);
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    VegetationLayer2::Options options;
    options.profile() = ProfileOptions("global-geodetic");
    options.groups().clear(); // No network or pager jobs; exercise the production layer/content StateSets.
    osg::ref_ptr<VegetationLayer2> layer = new VegetationLayer2(options);
    REQUIRE(layer->open().isOK());
    osg::ref_ptr<Map> map = new Map();
    map->addLayer(layer);
    layer->setOpacity(1.0f); // Initializes VisibleLayer's ON|OVERRIDE blend state, as prepareForRendering does.
    auto* root = layer->getNode()->asGroup();
    REQUIRE(root);
    REQUIRE(root->getNumChildren() == 1u);
    auto* content = root->getChild(0)->asGroup();
    REQUIRE(content);
    auto* probe = new CrownCoverageProbe();
    content->getOrCreateStateSet()->setAttribute(probe);
    renderer.setScene(root);
    auto geometry = ChonkTest::mesh();
    auto* colors = new osg::Vec4Array();
    for (const auto& p : *dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray()))
        colors->push_back(osg::Vec4((p.x()+2)*0.25f,0.5f,(2-p.x())*0.25f,1));
    geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
    auto* image = new osg::Image(); image->allocateImage(8,8,1,GL_RGBA,GL_UNSIGNED_BYTE);
    for (unsigned y=0; y<8; ++y)
        for (unsigned x=0; x<8; ++x)
        {
            auto* p = image->data(x,y);
            p[0] = p[1] = p[2] = 255; p[3] = x < 4 ? 128 : 192;
        }
    auto* texture = new osg::Texture2D(image);
    texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
    texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
    geometry->getOrCreateStateSet()->setTextureAttribute(0, texture);
    auto model = Chonk::create();
    REQUIRE(model->add(geometry,0,FLT_MAX,*renderer.factory));
    auto* camera = renderer.viewer.getCamera();
    camera->setViewMatrixAsLookAt(osg::Vec3(0,0,8),osg::Vec3(),osg::Vec3(0,1,0));
    for (bool gpu : {false,true})
        for (float opacity : {1.0f,0.5f})
        {
            INFO("GPU culling=" << gpu << " layer opacity=" << opacity);
            layer->setOpacity(opacity);
            osg::ref_ptr<osg::Image> reference;
            for (unsigned reverse=0; reverse<2; ++reverse)
            {
                auto* drawable = new ChonkDrawable();
                drawable->setUseGPUCulling(gpu); drawable->setBirthday(-10);
                for (unsigned j=0; j<128; ++j)
                {
                    const unsigned i = reverse ? 127-j : j;
                    // Distinct depths isolate coverage/blending from the separate problem of coplanar Z fighting.
                    drawable->add(model, osg::Matrixf::translate(0.4f*std::sin(float(i)),0,0.012f*i));
                }
                content->removeChildren(0,content->getNumChildren()); content->addChild(drawable);
                renderer.frame(); renderer.frame();
                REQUIRE(probe->a2c); REQUIRE(probe->depth); REQUIRE(probe->write);
                REQUIRE(!probe->blend); REQUIRE(probe->samples == 4);
                for (unsigned frame=0; frame<8; ++frame)
                {
                    renderer.frame(); auto next = renderer.pixels();
                    REQUIRE(next->data(128,128)[1] > 20); // Real covered fragments, not two identical clear frames.
                    if (!reference) reference = next;
                    unsigned changed = 0;
                    for (unsigned y=0; y<256; ++y)
                        for (unsigned x=0; x<256; ++x)
                        {
                            const auto* a=reference->data(x,y); const auto* b=next->data(x,y);
                            for (unsigned c=0; c<3; ++c)
                                if (std::abs(int(a[c])-int(b[c])) > 1) { ++changed; break; }
                        }
                    CHECK(changed == 0u);
                }
            }
        }
    CHECK(glGetError() == GL_NO_ERROR);
}

//! Keeps GPU resources isolated from other Chonk test contexts.
TEST_CASE("Procedural2 impostor lighting contract", "[procedural2][lighting][gpu]")
{
    const auto& executable = osgEarth::Tests::executablePath;
#ifdef _WIN32
    const std::string command = "\"\""+executable+
        "\" \"[.procedural2-lighting-worker],[.procedural2-crown-worker],[.procedural2-coverage-worker],"
        "[.procedural2-shadow-worker]\"\"";
#else
    std::string quoted = "'";
    for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1,c);
    const std::string command = quoted+
        "' '[.procedural2-lighting-worker],[.procedural2-crown-worker],[.procedural2-coverage-worker],"
        "[.procedural2-shadow-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
}

//! Checks view-conditioned baked normals and grazing-card coverage in color, depth, and shadow variants with A2C.
TEST_CASE("Procedural2 baked crowns present a volume instead of lit sheets", "[.procedural2-crown-worker]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Crown validation requires NVGL"); return; }
    osg::DisplaySettings::instance()->setNumMultiSamples(4);
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES_ARB, &samples);
    REQUIRE(samples == 4);
    auto* state = renderer.root->getOrCreateStateSet();
    installAssetLighting(state);
    state->setDefine("OE_CHONK_ALPHA_TO_COVERAGE");
    state->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB, osg::StateAttribute::ON);
    state->setMode(GL_MULTISAMPLE_ARB, osg::StateAttribute::ON);
    state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    state->setMode(GL_BLEND, osg::StateAttribute::OFF);
    auto* program = VirtualProgram::getOrCreate(state);
    program->setFunction("crown_test_view", "void crown_test_view(inout vec4 vertex) { }",
        VirtualProgram::LOCATION_VERTEX_VIEW);
    program->setFunction("crown_test_normal", R"glsl(
        in vec3 vp_Normal;
        uniform bool crown_test_show_normal;
        // Retain production alpha so the readback also tests multisample card coverage.
        void crown_test_normal(inout vec4 color)
        { if (crown_test_show_normal) color.rgb = normalize(vp_Normal)*0.5+0.5; }
    )glsl", VirtualProgram::LOCATION_FRAGMENT_LIGHTING, 1.0f);
    auto* showNormal = new osg::Uniform("crown_test_show_normal", true);
    state->addUniform(showNormal);
    auto geometry = ChonkTest::mesh();
    (*dynamic_cast<osg::Vec4Array*>(geometry->getColorArray()))[0].set(1,1,1,1);
    auto* modes = new osg::UByteArray(1);
    (*modes)[0] = Chonk::NORMAL_TECHNIQUE_BAKED;
    geometry->setVertexAttribArray(6, modes, osg::Array::BIND_OVERALL);
    for (auto& uv : *dynamic_cast<osg::Vec2Array*>(geometry->getTexCoordArray(0)))
        uv.set(0.1f + 0.8f*uv.x(), 0.55f + 0.4f*uv.y());
    for (unsigned unit=0; unit<2; ++unit)
    {
        auto* image = new osg::Image();
        image->allocateImage(16,8,1,GL_RGBA,GL_UNSIGNED_BYTE);
        for (unsigned y=0; y<8; ++y)
            for (unsigned x=0; x<16; ++x)
            {
                auto* p = image->data(x,y);
                p[0] = p[1] = unit == 0 ? 255 : 128;
                p[2] = unit == 0 || y >= 4 ? 255 : 0;
                // Mark the albedo as cutout so Chonk cannot classify this fixture as opaque.
                p[3] = unit == 0 && x == 0 ? 0 : 255;
            }
        auto* texture = new osg::Texture2D(image);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        geometry->getOrCreateStateSet()->setTextureAttribute(unit, texture);
    }
    auto model = Chonk::create();
    REQUIRE(model->add(geometry, 0, FLT_MAX, *renderer.factory));
    auto* camera = renderer.viewer.getCamera();
    for (bool orthographic : {false,true})
        for (float rotation : {0.0f,0.7f})
            for (float side : {1.0f,-1.0f})
                for (float degrees : {0.0f,25.0f,45.0f,75.0f})
                {
                    INFO("ortho=" << orthographic << " rotation=" << rotation << " side=" << side <<
                        " angle=" << degrees);
                    if (orthographic) camera->setProjectionMatrixAsOrtho(-3,3,-3,3,1,30);
                    else camera->setProjectionMatrixAsPerspective(45,1,1,30);
                    const float angle = degrees*0.01745329252f;
                    const auto turn = osg::Matrixf::rotate(rotation, 0,0,1);
                    const osg::Vec3 eye = osg::Vec3(8*std::sin(angle),0,side*8*std::cos(angle))*turn;
                    const osg::Vec3 up = osg::Vec3(0,1,0)*turn;
                    camera->setViewMatrixAsLookAt(eye, osg::Vec3(), up);
                    auto* drawable = new ChonkDrawable();
                    drawable->setBirthday(-10);
                    drawable->setUseGPUCulling(false);
                    drawable->add(model, osg::Matrixf::scale(1.4f,0.8f,1)*turn);
                    renderer.setScene(drawable);
                    for (unsigned pass=0; pass<3; ++pass)
                    {
                        INFO("pass=" << pass);
                        state->removeDefine("OE_IS_DEPTH_CAMERA");
                        state->removeDefine("OE_IS_SHADOW_CAMERA");
                        if (pass == 1) state->setDefine("OE_IS_DEPTH_CAMERA");
                        if (pass == 2) state->setDefine("OE_IS_SHADOW_CAMERA");
                        // Depth-only programs need not install a lighting/view-stage normal transform.
                        if (pass == 0)
                            program->setFunction("crown_test_view", "void crown_test_view(inout vec4 vertex) { }",
                                VirtualProgram::LOCATION_VERTEX_VIEW);
                        else program->removeShader("crown_test_view");
                        showNormal->set(pass == 0);
                        renderer.frame(); renderer.frame();
                        auto pixels = renderer.pixels();
                        const auto* p = pixels->data(128,128);
                        if (degrees == 75 && pass != 2)
                        {
                            CHECK(p[0] == 0); CHECK(p[1] == 0); CHECK(p[2] == 0);
                        }
                        else if (pass == 0)
                        {
                            // The capture's central normal follows the visible volume, not the tilted carrier plane.
                            CHECK(std::abs(int(p[0])-128) <= 4);
                            CHECK(std::abs(int(p[1])-128) <= 4);
                            CHECK(p[2] >= 251);
                        }
                        else
                        {
                            CHECK(p[0] >= 251); CHECK(p[1] >= 251); CHECK(p[2] >= 251);
                        }
                    }
                }
    CHECK(glGetError() == GL_NO_ERROR);
}

//! Exercises authored crown normals, true leaf backs, and atlas coordinates with real Chonk GPU draws.
TEST_CASE("Procedural2 impostor backs retain volume lighting and atlas view", "[.procedural2-lighting-worker]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Impostor validation requires NVGL"); return; }
    osg::DisplaySettings::instance()->setNumMultiSamples(4);
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    auto* state = renderer.root->getOrCreateStateSet();
    state->setMode(GL_CULL_FACE,osg::StateAttribute::OFF);
    installAssetLighting(state);
    auto* program = VirtualProgram::getOrCreate(state);
    // Real lighting (Sky2/Phong) installs a view stage. Request that stage in this otherwise unlit test harness,
    // so VirtualProgram transforms normals from the local frame into the light's view coordinates.
    program->setFunction("p2_test_view", "void p2_test_view(inout vec4 vertex) { }",
        VirtualProgram::LOCATION_VERTEX_VIEW);
    program->setFunction("p2_test_lambert", R"glsl(
        in vec3 vp_Normal;
        uniform vec3 p2_test_sun;
        // Exposes the production normal to a controlled light, independently of ambient/atmosphere settings.
        void p2_test_lambert(inout vec4 color)
        { color.rgb *= 0.15 + 0.85 * max(0.0, dot(normalize(vp_Normal),p2_test_sun)); }
    )glsl",VirtualProgram::LOCATION_FRAGMENT_LIGHTING,0.0f);
    auto* sun = new osg::Uniform("p2_test_sun",osg::Vec3(0,0,1));
    state->addUniform(sun);
    auto* camera = renderer.viewer.getCamera();
    camera->setProjectionMatrixAsOrtho(-2,2,-2,2,1,30);

    auto geometry = ChonkTest::mesh();
    (*dynamic_cast<osg::Vec4Array*>(geometry->getColorArray()))[0].set(1,1,1,1);
    auto* modes = new osg::UByteArray(1);
    geometry->setVertexAttribArray(6,modes,osg::Array::BIND_OVERALL);
    for (auto& uv : *dynamic_cast<osg::Vec2Array*>(geometry->getTexCoordArray(0)))
        uv.x() = 0.04f + 0.17f*uv.x();
    osg::ref_ptr<osg::Image> atlas = new osg::Image();
    atlas->allocateImage(4,4,1,GL_RGBA,GL_UNSIGNED_BYTE);
    for (unsigned y=0; y<4; ++y)
        for (unsigned x=0; x<4; ++x)
        {
            auto* p = atlas->data(x,y);
            p[0] = x == 0 ? 255 : 0; p[1] = 0; p[2] = x == 0 ? 0 : 255; p[3] = 255;
        }
    auto* texture = new osg::Texture2D(atlas);
    texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::NEAREST);
    texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::NEAREST);
    geometry->getOrCreateStateSet()->setTextureAttribute(0,texture);

    // Nonuniform instance scale and a rotated local frame exercise the same transform chain as globe placement.
    const osg::Matrixf local = osg::Matrixf::scale(1.1f,0.8f,1.3f)*osg::Matrixf::rotate(0.4f,osg::Vec3(0,1,0));
    const osg::Matrixf frame = osg::Matrixf::rotate(0.7f,osg::Vec3(1,0,0));
    const osg::Vec3 normal = osg::Matrixf::transform3x3(osg::Vec3(0,0,1),local*frame);
    const osg::Vec3 up = osg::Matrixf::transform3x3(osg::Vec3(0,1,0),frame);
    for (auto technique : {Chonk::NORMAL_TECHNIQUE_DEFAULT,Chonk::NORMAL_TECHNIQUE_VOLUME})
    {
        (*modes)[0] = technique;
        auto model = Chonk::create();
        REQUIRE(model->add(geometry,0,FLT_MAX,*renderer.factory));
        auto* drawable = new ChonkDrawable();
        drawable->setBirthday(-10.0f);
        drawable->add(model,local);
        auto* placement = new osg::MatrixTransform(frame);
        placement->addChild(drawable);
        renderer.setScene(placement);
        for (float face : {1.0f,-1.0f})
        {
            camera->setViewMatrixAsLookAt(normal*(8.0f*face),osg::Vec3(),up);
            for (float angle : {0.0f,0.8f,1.8f})
            {
                INFO("normal technique=" << unsigned(technique) << " face=" << face << " sun angle=" << angle);
                osg::Vec3 light = normal; light.normalize();
                light = light*std::cos(angle)+up*std::sin(angle); light.normalize();
                sun->set(osg::Vec3(osg::Matrixd::transform3x3(osg::Vec3d(light),camera->getViewMatrix())));
                renderer.frame(); renderer.frame();
                auto pixels = renderer.pixels();
                const auto* p = pixels->data(128,128);
                const float facing = technique == Chonk::NORMAL_TECHNIQUE_DEFAULT ? face : 1.0f;
                const float expected = 255.0f*(0.15f+0.85f*std::max(0.0f,facing*std::cos(angle)));
                CHECK(std::abs(float(p[0])-expected) < 4.0f);
                CHECK(p[1] < 3);
                CHECK(p[2] < 3); // Mirroring the full atlas U on the back would sample the blue view.
            }
        }
    }

    // Aggregates retain a canonical foliage lobe while their proxy footprint stretches and shears over terrain.
    // A unit-length but inverse-transpose-stretched normal fails these tests when the patch scale changes.
    installCanopyShader(state);
    {
        osg::Vec3 crownNormal(0.4f,-0.25f,0.85f); crownNormal.normalize();
        (*dynamic_cast<osg::Vec3Array*>(geometry->getNormalArray()))[0] = crownNormal;
        (*modes)[0] = Chonk::NORMAL_TECHNIQUE_VOLUME;
        auto model = Chonk::create();
        REQUIRE(model->add(geometry,0,FLT_MAX,*renderer.factory));
        const osg::Matrixf globe = osg::Matrixf::rotate(0.9f,osg::Vec3(1,0,0))*
            osg::Matrixf::rotate(-1.2f,osg::Vec3(0,0,1));
        for (unsigned turn=0; turn<4; ++turn)
            for (float width : {20.0f,80.0f,200.0f})
                for (float slope : {0.0f,0.6f})
                {
                    const auto rotation = osg::Matrixf::rotate(turn*1.57079632679f,osg::Vec3(0,0,1));
                    const osg::Matrixf terrain(width,0,width*slope,0, 0,width*0.6f,-width*slope*0.5f,0,
                        0,0,12,0, 0,0,0,1);
                    auto* drawable = new ChonkDrawable();
                    drawable->setBirthday(-10.0f);
                    drawable->add(model,rotation*terrain,osg::Vec2f(-1,0));
                    auto* placement = new osg::MatrixTransform(globe);
                    placement->addChild(drawable); renderer.setScene(placement);
                    camera->setProjectionMatrixAsOrtho(-3*width,3*width,-3*width,3*width,1,20*width);
                    const auto expectedNormal = osg::Matrixf::transform3x3(crownNormal,rotation*globe);
                    for (float face : {1.0f,-1.0f})
                    {
                        camera->setViewMatrixAsLookAt(osg::Vec3(0,0,8*width*face)*globe,osg::Vec3(),
                            osg::Matrixf::transform3x3(osg::Vec3(0,1,0),globe));
                        for (float angle : {0.0f,0.8f,1.8f})
                        {
                            INFO("canopy turn=" << turn << " width=" << width << " slope=" << slope <<
                                " face=" << face << " sun angle=" << angle);
                            const osg::Vec3 light = osg::Matrixf::transform3x3(
                                osg::Vec3(std::sin(angle),0,std::cos(angle)),globe);
                            sun->set(osg::Vec3(osg::Matrixd::transform3x3(osg::Vec3d(light),camera->getViewMatrix())));
                            renderer.frame(); renderer.frame();
                            const auto pixels = renderer.pixels();
                            const auto* p = pixels->data(128,128);
                            const float expected = 255.0f*(0.15f+0.85f*std::max(0.0f,expectedNormal*light));
                            CHECK(std::abs(float(p[0])-expected) < 4.0f);
                            CHECK(p[1] < 3);
                            CHECK(p[2] < 3);
                        }
                    }
                }
    }

    // Capture real shared aggregate art with its old flat normals and corrected foliage normals.
    // Keep the same albedo, triangles, instance footprint and light so only the normal treatment differs.
    {
        AssetCatalog catalog({},4u*1024u*1024u);
        ScatterGroup group; group.asset = "canopy8-0"; group.lodPixels = group.minPixels = 0.0f;
        auto smooth = catalog.acquire(group,"");
        REQUIRE(smooth);
        auto* oldGeometry = new osg::Geometry();
        auto* positions = new osg::Vec3Array();
        auto* normals = new osg::Vec3Array();
        auto* colors = new osg::Vec4Array();
        for (std::size_t i=0; i<smooth->_ebo_store.size(); i+=3)
        {
            const auto& a = smooth->_vbo_store[smooth->_ebo_store[i]].position;
            const auto& b = smooth->_vbo_store[smooth->_ebo_store[i+1]].position;
            const auto& c = smooth->_vbo_store[smooth->_ebo_store[i+2]].position;
            osg::Vec3 n = (b-a)^(c-a); n.normalize();
            for (unsigned j=0; j<3; ++j)
            {
                const auto& vertex = smooth->_vbo_store[smooth->_ebo_store[i+j]];
                positions->push_back(vertex.position); normals->push_back(n);
                colors->push_back(osg::Vec4(vertex.color[0]/255.0f,vertex.color[1]/255.0f,
                    vertex.color[2]/255.0f,1));
            }
        }
        oldGeometry->setVertexArray(positions);
        oldGeometry->setNormalArray(normals,osg::Array::BIND_PER_VERTEX);
        oldGeometry->setColorArray(colors,osg::Array::BIND_PER_VERTEX);
        oldGeometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,GLsizei(positions->size())));
        auto flat = Chonk::create();
        REQUIRE(flat->add(oldGeometry,0,FLT_MAX,*renderer.factory));
        camera->setProjectionMatrixAsOrtho(-48,48,-36,60,1,500);
        camera->setViewMatrixAsLookAt(osg::Vec3(35,-110,75),osg::Vec3(0,0,5),osg::Vec3(0,0,1));
        const osg::Matrixf stretch(80,0,20,0, 0,60,-8,0, 0,0,14,0, 0,0,0,1);
        for (int elevation : {20,65})
        {
            const float angle = elevation*0.01745329252f;
            const osg::Vec3 light(std::cos(angle)*0.6f,-std::cos(angle)*0.8f,std::sin(angle));
            sun->set(osg::Vec3(osg::Matrixd::transform3x3(osg::Vec3d(light),camera->getViewMatrix())));
            osg::ref_ptr<osg::Image> frames[2];
            for (unsigned fixed=0; fixed<2; ++fixed)
            {
                auto* drawable = new ChonkDrawable(); drawable->setBirthday(-10.0f);
                drawable->add(fixed ? smooth : flat,stretch,osg::Vec2f(fixed ? -511.0f : 0.0f,0));
                renderer.setScene(drawable); renderer.frame(); renderer.frame();
                frames[fixed] = renderer.pixels();
                REQUIRE(osgDB::writeImageFile(*frames[fixed],"../build/canopy-lighting-"+
                    std::to_string(elevation)+(fixed ? "-fixed.png" : "-before.png")));
            }
            unsigned coverage[2] = {0,0}, changed = 0;
            for (unsigned y=0; y<256; ++y)
                for (unsigned x=0; x<256; ++x)
                {
                    const auto* a = frames[0]->data(x,y);
                    const auto* b = frames[1]->data(x,y);
                    if (unsigned(a[0])+a[1]+a[2] > 0) ++coverage[0];
                    if (unsigned(b[0])+b[1]+b[2] > 0) ++coverage[1];
                    if (std::abs(int(a[1])-int(b[1])) > 5) ++changed;
                }
            CHECK(coverage[0] > 500u);
            CHECK(coverage[0] == coverage[1]);
            CHECK(changed > coverage[0]/4u);
        }
    }

    // Compare the actual starter impostors with the previous shader; preserve their albedo and silhouette.
    camera->setProjectionMatrixAsOrtho(-5.5,5.5,-7.5,7.5,1,100);
    camera->setViewMatrixAsLookAt(osg::Vec3(0,35,7),osg::Vec3(0,0,7),osg::Vec3(0,0,1));
    osg::Vec3 light(0.3f,0.2f,1.0f); light.normalize();
    sun->set(osg::Vec3(osg::Matrixd::transform3x3(osg::Vec3d(light),camera->getViewMatrix())));
    for (const std::string name : {"broadleaf","conifer"})
    {
        auto node = osgDB::readRefNodeFile("../data/procedural2/starter/"+name+"-coarse.osg");
        REQUIRE(node);
        auto model = Chonk::create();
        REQUIRE(model->add(node,0,FLT_MAX,*renderer.factory));
        for (const auto& vertex : model->_vbo_store)
            REQUIRE(vertex.normal_technique == Chonk::NORMAL_TECHNIQUE_VOLUME);
        auto* drawable = new ChonkDrawable();
        drawable->setBirthday(-10.0f); drawable->add(model);
        renderer.setScene(drawable);
        unsigned coverage[2] = {0,0};
        double energy[2] = {0,0};
        for (unsigned fixed=0; fixed<2; ++fixed)
        {
            if (fixed) installAssetLighting(state);
            else program->setFunction("oe_p2_leaf_normal", R"glsl(
                in vec3 vp_Normal;
                // Reproduces the old unconditional back-face flip for regression and comparison captures.
                void oe_p2_leaf_normal(inout vec4 color) { if (!gl_FrontFacing) vp_Normal = -vp_Normal; }
            )glsl",VirtualProgram::LOCATION_FRAGMENT_LIGHTING,-1.0f);
            renderer.frame(); renderer.frame();
            auto pixels = renderer.pixels();
            REQUIRE(osgDB::writeImageFile(*pixels,"../build/impostor-"+name+(fixed ? "-fixed.png" : "-before.png")));
            for (unsigned y=0; y<256; ++y)
                for (unsigned x=0; x<256; ++x)
                {
                    const auto* p = pixels->data(x,y);
                    const unsigned sum = unsigned(p[0])+p[1]+p[2];
                    if (sum > 0) ++coverage[fixed];
                    energy[fixed] += sum;
                }
        }
        INFO(name);
        CHECK(coverage[0] > 500u);
        CHECK(coverage[0] == coverage[1]);
        CHECK(energy[1] > energy[0]*2.0);
    }
}

//! Sparse leaf mips must still write shadow depth while transparent gaps remain open, with either culling path.
TEST_CASE("Procedural2 minified foliage retains shadow coverage", "[.procedural2-shadow-worker]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Foliage shadow validation requires NVGL"); return; }
    osg::DisplaySettings::instance()->setNumMultiSamples(0);
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    GLint samples = -1;
    glGetIntegerv(GL_SAMPLES_ARB,&samples);
    REQUIRE(samples == 0);
    auto* state = renderer.root->getOrCreateStateSet();
    state->setMode(GL_CULL_FACE,osg::StateAttribute::OFF);
    state->setMode(GL_BLEND,osg::StateAttribute::OFF);
    state->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB,osg::StateAttribute::ON);
    state->setDefine("OE_CHONK_ALPHA_TO_COVERAGE");
    installPopulationFadeShader(state);
    auto* camera = renderer.viewer.getCamera();
    camera->setProjectionMatrixAsOrtho(-2,2,-2,2,1,20);
    camera->setViewMatrixAsLookAt(osg::Vec3(0,0,8),osg::Vec3(),osg::Vec3(0,1,0));
    state->addUniform(new osg::Uniform("oe_shadowToPrimaryMatrix",osg::Matrixf()));
    state->addUniform(new osg::Uniform("oe_primaryProjectionMatrix",osg::Matrixf(camera->getProjectionMatrix())));
    state->addUniform(new osg::Uniform("oe_primaryViewport",osg::Vec2(256,256)));

    auto geometry = ChonkTest::mesh();
    auto* image = new osg::Image(); image->allocateImage(1024,1024,1,GL_RGBA,GL_UNSIGNED_BYTE);
    for (unsigned y=0; y<1024; ++y)
        for (unsigned x=0; x<1024; ++x)
        {
            auto* p = image->data(x,y);
            p[0] = p[1] = p[2] = 255;
            // Opaque trunk, empty gap, then 1/8 leaf coverage. Mip averaging takes every leaf below 0.5.
            p[3] = x < 256 || (x >= 512 && x%4 == 0 && y%2 == 0) ? 255 : 0;
        }
    auto* texture = new osg::Texture2D(image);
    texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);
    texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
    geometry->getOrCreateStateSet()->setTextureAttribute(0,texture);
    auto model = Chonk::create();
    REQUIRE(model->add(geometry,0,FLT_MAX,*renderer.factory));
    for (unsigned mode=0; mode<3; ++mode)
    {
        if (mode == 1) state->setDefine("OE_IS_DEPTH_CAMERA");
        if (mode == 2) state->setDefine("OE_IS_SHADOW_CAMERA");
        for (bool gpu : {false,true})
            for (float scale : {0.0625f,0.125f})
            {
                INFO("camera variant=" << mode << " GPU culling=" << gpu << " scale=" << scale);
                auto* drawable = new ChonkDrawable();
                drawable->setUseGPUCulling(gpu); drawable->setBirthday(-10);
                drawable->add(model,osg::Matrixf::scale(scale,scale,1));
                renderer.setScene(drawable);
                for (float compensation : {0.0f,0.75f})
                {
                    INFO("mip compensation=" << compensation);
                    drawable->setAlphaCutoff(compensation);
                    renderer.frame(); renderer.frame();
                    osg::ref_ptr<osg::Image> pixels = new osg::Image();
                    pixels->readPixels(0,0,256,256,GL_DEPTH_COMPONENT,GL_FLOAT);
                    //! Reads actual depth writes at an interior sample, avoiding the texture's macro-region edges.
                    auto written = [&](float u)
                    {
                        const unsigned x = unsigned(128.0f+(u-0.5f)*256.0f*scale);
                        return *reinterpret_cast<const float*>(pixels->data(x,128)) < 0.99f;
                    };
                    CHECK(written(0.125f)); // Trunks already cast correctly.
                    CHECK_FALSE(written(0.375f)); // Compensation must not turn empty cards into solid rectangles.
                    CHECK(written(0.75f) == (compensation > 0.0f));
                }
            }
    }
    CHECK(glGetError() == GL_NO_ERROR);
}
