/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "ChonkTestUtils.h"
#include <osgEarth/catch.hpp>
#include <osgEarthProcedural2/AssetLighting.h>
#include <osgEarthProcedural2/CanopyTransition.h>
#include <osgEarthProcedural2/AssetCatalog>
#include <cstdlib>
#include <cmath>

using namespace osgEarth;
using namespace osgEarth::Procedural2;
namespace osgEarth { namespace Tests { extern std::string executablePath; } }

//! Keeps GPU resources isolated from other Chonk test contexts.
TEST_CASE("Procedural2 impostor lighting contract", "[procedural2][lighting][gpu]")
{
    const auto& executable = osgEarth::Tests::executablePath;
#ifdef _WIN32
    const std::string command = "\"\""+executable+"\" \"[.procedural2-lighting-worker]\"\"";
#else
    std::string quoted = "'";
    for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1,c);
    const std::string command = quoted+"' '[.procedural2-lighting-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
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
