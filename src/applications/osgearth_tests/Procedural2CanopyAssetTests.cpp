/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarthProcedural2/AssetCatalog>
#include <osgEarthProcedural2/Canopy>
#include <osgEarthProcedural2/CanopyTransition.h>
#include <osgEarthProcedural2/AssetLighting.h>
#include "ChonkTestUtils.h"
#include <future>
#include <set>
#include <cstdlib>
#include <cmath>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

namespace
{
    //! Resolves checked-in source proxies from the required tests working directory.
    ScatterAsset canopySource(const std::string& name)
    {
        ScatterAsset asset; asset.name = name;
        asset.nearModel = URI("../data/procedural2/starter/"+name+"-near.osg");
        asset.coarseModel = URI("../data/procedural2/starter/"+name+"-coarse.osg");
        asset.canopyModel = URI("../data/procedural2/starter/"+name+"-canopy.osg");
        return asset;
    }
}

//! Source proxies supply art without loading detailed meshes; templates share bounded source leases.
TEST_CASE("Procedural2 source canopy recipes share art and preserve certified crowns", "[procedural2][canopy]")
{
    auto broadleaf = canopySource("broadleaf"), conifer = canopySource("conifer");
    for (auto* asset : {&broadleaf,&conifer})
    {
        asset->nearModel = URI("deliberately-missing-detailed-model.osg");
        asset->coarseModel = URI("deliberately-missing-individual-impostor.osg");
        ScatterAsset restored(asset->getConfig());
        CHECK(restored.canopyModel.full() == asset->canopyModel.full());
    }
    AssetCatalog catalog({broadleaf,conifer},32u*1024u*1024u);
    ScatterGroup group; group.models = {"broadleaf","conifer"};
    std::vector<std::future<Chonk::Ptr>> jobs;
    for (unsigned i=0; i<4; ++i)
        jobs.push_back(std::async(std::launch::async,[&]() { return catalog.acquireCanopy(group,8,0); }));
    std::vector<Chonk::Ptr> owners;
    for (auto& job : jobs) owners.push_back(job.get());
    REQUIRE(owners.front());
    for (const auto& owner : owners) CHECK(owner == owners.front());
    CHECK(catalog.residency().assets == 3u); // two shared sources, one recipe; no near-model load
    const auto initialBytes = catalog.residency().bytes;
    REQUIRE(initialBytes > 1024u*1024u);
    for (unsigned coverage=1; coverage<=8; ++coverage)
        for (unsigned layout=0; layout<4; ++layout)
        {
            auto model = catalog.acquireCanopy(group,coverage,layout);
            REQUIRE(model);
            owners.push_back(model);
            REQUIRE(model->_lods.size() == 1u);
            CHECK(model->_lods[0].length == 9u*6u*3u);
            CHECK(model->_lods[0].alphaTested);
            for (const auto& v : model->_vbo_store)
            {
                const auto crown = unsigned(v.flex.x());
                REQUIRE(crown < 9u);
                const auto& box = canopyTemplate(coverage,layout)[crown];
                CHECK(std::abs(v.position.x()-box.center.x()) <= box.size.x()+1e-6f);
                CHECK(std::abs(v.position.y()-box.center.y()) <= box.size.y()+1e-6f);
                CHECK(v.position.z() >= -1e-6f);
                CHECK(v.position.z() <= box.center.z()+box.size.z()+1e-6f);
                CHECK(v.normal_technique == Chonk::NORMAL_TECHNIQUE_VOLUME);
                CHECK(std::abs(v.normal.length()-1.0f) < 1e-5f);
                CHECK(v.color == osg::Vec4ub(255,255,255,255)); // source albedo, no placeholder tint
            }
        }
    CHECK(catalog.residency().assets == 34u);
    CHECK(catalog.residency().bytes-initialBytes < 1024u*1024u); // 31 meshes, no duplicate decoded atlases
    group.density *= 2; group.maxRange *= 2; group.canopyHeight *= 2;
    CHECK(catalog.acquireCanopy(group,8,0) == owners.front());
    group.models = {"conifer"};
    auto changed = catalog.acquireCanopy(group,8,0);
    REQUIRE(changed);
    CHECK(changed != owners.front());
    CHECK(changed->_materials.size() == 1u);
    changed.reset(); owners.clear(); jobs.clear();
    CHECK(catalog.residency().assets == 0u);
    CHECK(catalog.residency().bytes == 0u);
    auto reloaded = catalog.acquireCanopy(group,8,0);
    REQUIRE(reloaded);
    reloaded.reset();
    osg::ref_ptr<ProgressCallback> canceled = new ProgressCallback(); canceled->cancel();
    CHECK_FALSE(catalog.acquireCanopy(group,8,0,canceled));
    CHECK(catalog.residency().bytes == 0u);
    AssetCatalog tight({broadleaf,conifer},initialBytes-1u);
    group.models = {"broadleaf","conifer"};
    CHECK_FALSE(tight.acquireCanopy(group,8,0));
    CHECK(tight.residency().budgetDenials == 1u);
    CHECK(tight.residency().bytes == 0u);
    auto ordinary = canopySource("broadleaf"); ordinary.canopyModel = URI();
    AssetCatalog fallback({ordinary},16u*1024u*1024u);
    group.models = {"broadleaf"};
    REQUIRE(fallback.acquireCanopy(group,8,0)); // no explicit canopy: use the ordinary coarse proxy
    ordinary.canopyModel = ordinary.nearModel;
    AssetCatalog oversized({ordinary},16u*1024u*1024u);
    CHECK_FALSE(oversized.acquireCanopy(group,8,0));
    CHECK(oversized.residency().lastError.find("128") != std::string::npos);
    CHECK(oversized.residency().bytes == 0u);
    ordinary.canopyModel = URI("missing-explicit-canopy.osg");
    AssetCatalog broken({ordinary},16u*1024u*1024u);
    CHECK_FALSE(broken.acquireCanopy(group,8,0)); // explicit broken art must not silently change sources
    CHECK(broken.residency().failedLoads == 1u);
}

namespace osgEarth { namespace Tests { extern std::string executablePath; } }

//! Runs textured aggregate checks in their own process to isolate Chonk context resources.
TEST_CASE("Procedural2 source canopy GPU contract", "[procedural2][canopy][gpu]")
{
    const auto& executable = osgEarth::Tests::executablePath;
#ifdef _WIN32
    const std::string command = "\"\""+executable+"\" \"[.canopy-source-worker]\"\"";
#else
    std::string quoted = "'";
    for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1,c);
    const std::string command = quoted+"' '[.canopy-source-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
}

//! Real GPU output must preserve source textures and alpha while masks remove independently certified pieces.
TEST_CASE("Procedural2 source canopy albedo and exclusion pieces", "[.canopy-source-worker]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Source canopy validation requires NVGL"); return; }
    osg::DisplaySettings::instance()->setNumMultiSamples(4);
    ChonkTest::Renderer renderer; REQUIRE(renderer.initialize());
    AssetCatalog catalog({canopySource("broadleaf"),canopySource("conifer")},32u*1024u*1024u);
    auto* state = renderer.root->getOrCreateStateSet();
    state->setAttribute(catalog.textures());
    state->setMode(GL_CULL_FACE,osg::StateAttribute::OFF);
    installCanopyShader(state); installAssetLighting(state);
    auto* program = VirtualProgram::getOrCreate(state);
    program->setFunction("p2_source_view","void p2_source_view(inout vec4 vertex) { }",
        VirtualProgram::LOCATION_VERTEX_VIEW);
    program->setFunction("p2_source_light",R"glsl(
        in vec3 vp_Normal;
        uniform vec3 p2_source_sun;
        // Controlled light keeps this art comparison independent of atmosphere and shadow settings.
        void p2_source_light(inout vec4 color)
        { color.rgb *= 0.15+0.85*max(0.0,dot(normalize(vp_Normal),p2_source_sun)); }
    )glsl",VirtualProgram::LOCATION_FRAGMENT_LIGHTING,0.0f);
    auto* camera = renderer.viewer.getCamera();
    camera->setProjectionMatrixAsOrtho(-48,48,-36,60,1,500);
    camera->setViewMatrixAsLookAt(osg::Vec3(35,-110,75),osg::Vec3(0,0,5),osg::Vec3(0,0,1));
    osg::Vec3 light(0.3f,-0.4f,0.8660254f); light.normalize();
    state->addUniform(new osg::Uniform("p2_source_sun",
        osg::Vec3(osg::Matrixd::transform3x3(osg::Vec3d(light),camera->getViewMatrix()))));
    ScatterGroup group; group.models = {"broadleaf","conifer"};
    auto source = catalog.acquireCanopy(group,8,0); REQUIRE(source);
    group.models.clear();
    auto placeholder = catalog.acquireCanopy(group,8,0); REQUIRE(placeholder);
    const osg::Matrixf stretch(80,0,20,0, 0,60,-8,0, 0,0,14,0, 0,0,0,1);
    unsigned fullCoverage = 0;
    for (unsigned stage=0; stage<4; ++stage)
    {
        auto* drawable = new ChonkDrawable(); drawable->setBirthday(-10.0f);
        drawable->setAlphaCutoff(0.15f);
        const unsigned mask = stage < 2 ? 511u : (stage == 2 ? 1u<<4 : 512u);
        drawable->add(stage == 0 ? placeholder : source,stretch,osg::Vec2f(-float(mask),0));
        renderer.setScene(drawable); renderer.frame(); renderer.frame();
        auto pixels = renderer.pixels();
        REQUIRE(osgDB::writeImageFile(*pixels,"../build/canopy-source-"+std::to_string(stage)+".png"));
        unsigned coverage = 0;
        std::set<unsigned> colors;
        for (unsigned y=0; y<256; ++y)
            for (unsigned x=0; x<256; ++x)
            {
                const auto* p = pixels->data(x,y);
                if (unsigned(p[0])+p[1]+p[2] == 0) continue;
                ++coverage;
                colors.insert((unsigned(p[0])<<16u)|(unsigned(p[1])<<8u)|unsigned(p[2]));
            }
        if (stage == 1)
        {
            fullCoverage = coverage;
            CHECK(coverage > 500u);
            CHECK(colors.size() > 100u); // source albedo survives material sharing and upload
        }
        if (stage == 2) { CHECK(coverage > 30u); CHECK(coverage < fullCoverage); }
        if (stage == 3) CHECK(coverage == 0u);
    }

    // Clipping into coverage/terrain pieces must reconstruct the original textured canopy at every quarter turn.
    camera->setProjectionMatrixAsOrtho(-48,48,-48,48,1,500);
    camera->setViewMatrixAsLookAt(osg::Vec3(0,0,150),osg::Vec3(0,0,0),osg::Vec3(0,1,0));
    for (const std::string pass : {"color","OE_IS_DEPTH_CAMERA","OE_IS_SHADOW_CAMERA"})
    {
        if (pass != "color") state->setDefine(pass);
        for (unsigned turn=0; turn<4; ++turn)
        {
            const auto transform = osg::Matrixf::rotate(turn*1.57079632679f,osg::Vec3(0,0,1))*
                osg::Matrixf::scale(80,60,14);
            auto* whole = new ChonkDrawable(); whole->setBirthday(-10.0f); whole->setAlphaCutoff(0.15f);
            whole->add(source,transform,osg::Vec2f(-511,0));
            renderer.setScene(whole); renderer.frame(); renderer.frame();
            auto reference = renderer.pixels();
            for (unsigned side : {2u,1u})
            {
                auto* split = new ChonkDrawable(); split->setBirthday(-10.0f); split->setAlphaCutoff(0.15f);
                for (unsigned y=0; y<4; y+=side)
                    for (unsigned x=0; x<4; x+=side)
                        split->add(source,transform,osg::Vec2f(-511,float(1u+x+4u*y+16u*(side-1u))));
                renderer.setScene(split); renderer.frame(); renderer.frame();
                auto actual = renderer.pixels();
                unsigned changed = 0, visible = 0;
                for (unsigned y=0; y<256; ++y)
                    for (unsigned x=0; x<256; ++x)
                    {
                        const auto* a = actual->data(x,y); const auto* b = reference->data(x,y);
                        if (unsigned(b[0])+b[1]+b[2] > 0) ++visible;
                        if (std::abs(int(a[0])-int(b[0]))+std::abs(int(a[1])-int(b[1]))+
                            std::abs(int(a[2])-int(b[2])) > 12) ++changed;
                    }
                INFO("pass " << pass << " turn " << turn << " side " << side <<
                    " changed " << changed << " visible " << visible);
                CHECK(visible > 500u);
                CHECK(changed < visible/100u+4u);
            }
            // A single window also needs to remove real pixels; a disabled clip would pass reconstruction alone.
            auto* quarter = new ChonkDrawable(); quarter->setBirthday(-10.0f); quarter->setAlphaCutoff(0.15f);
            quarter->add(source,transform,osg::Vec2f(-511,17));
            renderer.setScene(quarter); renderer.frame(); renderer.frame();
            auto actual = renderer.pixels();
            unsigned retained = 0, removed = 0;
            for (unsigned y=0; y<256; ++y)
                for (unsigned x=0; x<256; ++x)
                {
                    const auto* a = actual->data(x,y); const auto* b = reference->data(x,y);
                    if (unsigned(a[0])+a[1]+a[2] > 0) ++retained;
                    else if (unsigned(b[0])+b[1]+b[2] > 0) ++removed;
                }
            CHECK(retained > 100u); CHECK(removed > retained);
        }
        if (pass != "color") state->removeDefine(pass);
    }

    // Far proxies must retain visible color when an entire source clump is only a few pixels across.
    camera->setProjectionMatrixAsOrtho(-768,768,-768,768,1,500);
    auto* distant = new ChonkDrawable(); distant->setBirthday(-10.0f);
    distant->setAlphaCutoff(0.15f);
    distant->add(source,stretch,osg::Vec2f(-511,0));
    renderer.setScene(distant); renderer.frame(); renderer.frame();
    auto pixels = renderer.pixels();
    unsigned distantCoverage = 0;
    for (unsigned y=0; y<256; ++y)
        for (unsigned x=0; x<256; ++x)
        {
            const auto* p = pixels->data(x,y);
            if (unsigned(p[0])+p[1]+p[2] > 0) ++distantCoverage;
        }
    CHECK(distantCoverage > 15u);
}
