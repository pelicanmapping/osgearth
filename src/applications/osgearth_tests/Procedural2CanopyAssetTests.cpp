/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarthProcedural2/AssetCatalog>
#include <osgEarthProcedural2/Canopy>
#include <osgEarthProcedural2/CanopyTransition.h>
#include <osgEarthProcedural2/AssetLighting.h>
#include <osgEarthProcedural2/TreeCards.h>
#include "ChonkTestUtils.h"
#include <future>
#include <osg/Multisample>
#include <osgEarth/NodeUtils>
#include <thread>
#include <chrono>
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
            for (unsigned side : {8u,4u,2u,1u})
            {
                auto* split = new ChonkDrawable(); split->setBirthday(-10.0f); split->setAlphaCutoff(0.15f);
                for (unsigned y=0; y<16; y+=side)
                    for (unsigned x=0; x<16; x+=side)
                        split->add(source,transform,osg::Vec2f(-511,float(1u+x+16u*y+256u*(side-1u))));
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
            quarter->add(source,transform,osg::Vec2f(-511,1793));
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


//! Cluster grouping changes only culling units; source dimensions, complete placement, and source leases survive.
TEST_CASE("Procedural2 tree cards preserve placement and share source art", "[procedural2][treecards]")
{
    auto asset = canopySource("broadleaf");
    asset.nearModel = URI("must-not-load-near.osg");
    asset.canopyModel = URI("must-not-load-old-canopy.osg");
    AssetCatalog catalog({asset},32u*1024u*1024u);
    ScatterGroup group; group.models = {"broadleaf"};
    auto source = catalog.acquireImpostor(group,"broadleaf"); REQUIRE(source);
    const auto bytes = catalog.residency().bytes;
    auto cards = catalog.acquireTreeCards(group,"broadleaf",32); REQUIRE(cards);
    CHECK(catalog.acquireTreeCards(group,"broadleaf",32) == cards);
    CHECK(catalog.residency().bytes-bytes < 1024u*1024u);
    CHECK(cards->_lods[0].length == source->_lods[0].length*32u);
    CHECK(cards->_materials == source->_materials);
    std::vector<TreeCardPlacement> trees;
    for (unsigned i=0; i<137; ++i)
        trees.push_back({osg::Matrixd::scale(0.8+0.002*i,0.8+0.002*i,0.8+0.002*i)*
            osg::Matrixd::rotate(i*0.19,osg::Vec3d(0,0,1))*
            osg::Matrixd::translate((i%17)*7.0,(i/17)*9.0,i*0.05),0});
    for (unsigned size : {8u,32u,64u,256u})
    {
        std::vector<TreeCardCluster> clusters;
        std::vector<osg::Vec4f> records;
        REQUIRE(buildTreeCardClusters(trees,{source},size,clusters,records).isOK());
        CHECK(records.size() == trees.size()*4u);
        std::set<unsigned> matched;
        for (const auto& cluster : clusters)
        {
            CHECK(cluster.count <= size);
            for (bool far : {false,true})
            {
                const auto uv = treeCardInstanceUV(cluster,far);
                CHECK(unsigned(std::round(-uv.x())) == cluster.count);
                CHECK(unsigned(std::round(uv.y())) == cluster.offset);
                CHECK((-uv.x()-std::floor(-uv.x()) > 0.125f) == far);
            }
            for (unsigned i=0; i<cluster.count; ++i)
            {
                osg::Matrixd matrix;
                for (unsigned c=0; c<3; ++c)
                    for (unsigned r=0; r<4; ++r) matrix(r,c) = records[(cluster.offset+i)*4u+c][r];
                for (unsigned corner=0; corner<8; ++corner)
                {
                    const auto p = osg::Vec3d(source->_box.corner(corner))*matrix;
                    CHECK(std::abs(p.x()) <= 1.00001); CHECK(std::abs(p.y()) <= 1.00001);
                    CHECK(std::abs(p.z()) <= 1.00001);
                }
                const auto sphere = records[(cluster.offset+i)*4u+3u];
                const auto center = osg::Vec3d(source->_box.center())*matrix;
                CHECK((center-osg::Vec3d(sphere.x(),sphere.y(),sphere.z())).length() < 1e-5);
                const double radius = source->_box.radius()*
                    osg::Vec3d(matrix(0,0),matrix(0,1),matrix(0,2)).length();
                CHECK(std::abs(radius-sphere.w()) < 1e-5);
                matrix = matrix*osg::Matrixd(cluster.transform);
                const auto found = std::find_if(trees.begin(),trees.end(),[&](const TreeCardPlacement& tree)
                    { return (tree.transform.getTrans()-matrix.getTrans()).length() < 1e-4; });
                REQUIRE(found != trees.end());
                CHECK(matched.insert(unsigned(found-trees.begin())).second);
                for (unsigned element=0; element<16; ++element)
                    CHECK(std::abs(found->transform.ptr()[element]-matrix.ptr()[element]) < 1e-4);
            }
        }
        CHECK(matched.size() == trees.size());
        osg::ref_ptr<ProgressCallback> canceled = new ProgressCallback(); canceled->cancel();
        CHECK(buildTreeCardClusters(trees,{source},size,clusters,records,canceled).isError());
        CHECK(clusters.empty()); CHECK(records.empty());
    }
    std::vector<TreeCardCluster> invalidClusters;
    std::vector<osg::Vec4f> invalidRecords;
    for (const auto& invalid : {osg::Matrixd::scale(0,0,0),osg::Matrixd::scale(1,2,1),
        osg::Matrixd::scale(-1,1,1)})
    {
        CHECK(buildTreeCardClusters({{invalid,0}},{source},32,invalidClusters,invalidRecords).isError());
        CHECK(invalidClusters.empty()); CHECK(invalidRecords.empty());
    }
    source.reset(); CHECK(catalog.residency().bytes > 0u);
    cards.reset(); CHECK(catalog.residency().bytes == 0u);
    CHECK_FALSE(catalog.acquireTreeCards(group,"broadleaf",257));
}

//! Aggregate pages enumerate the exact source cells used by their detailed children, without reseeding placement.
TEST_CASE("Procedural2 tree card tiers retain source identities", "[procedural2][treecards]")
{
    ScatterGroup group; group.canopy = true; group.cellLevel = 6; group.renderCellLevel = 4;
    group.density = 5000;
    auto profile = Profile::create("epsg:3857",0,0,1600,1600,"",1,1);
    const TileKey parent(2,0,0,profile);
    UniformScatterSource source;
    std::vector<ScatterPlacement> coarse, fine;
    REQUIRE(source.generateBatch(parent,group,9,coarse).isOK());
    std::set<std::uint64_t> ids;
    for (const auto& p : coarse) ids.insert(p.id);
    std::set<std::uint64_t> detailed;
    for (unsigned y=0; y<4; ++y)
        for (unsigned x=0; x<4; ++x)
        {
            REQUIRE(source.generateBatch(TileKey(4,x,y,profile),group,9,fine).isOK());
            for (const auto& p : fine) CHECK(detailed.insert(p.id).second);
        }
    CHECK(ids == detailed); CHECK(ids.size() == coarse.size()); CHECK_FALSE(ids.empty());
    group.maxPerBatch = 1;
    CHECK(source.generateBatch(parent,group,9,coarse).isError()); CHECK(coarse.empty());
    group.canopyMidClusterSize = 128; group.canopyFarClusterSize = 16;
    ScatterGroup restored(group.getConfig());
    CHECK(restored.canopyMidClusterSize == 128); CHECK(restored.canopyFarClusterSize == 16);
}

//! Runs card expansion in a fresh context, independent of other GPU tests' process-global caches.
TEST_CASE("Procedural2 clustered tree cards GPU contract", "[procedural2][treecards][gpu]")
{
    const auto& executable = osgEarth::Tests::executablePath;
#ifdef _WIN32
    const std::string command = "\"\""+executable+"\" \"[.tree-cards-worker]\"\"";
#else
    std::string quoted = "'";
    for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1,c);
    const std::string command = quoted+"' '[.tree-cards-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
}

//! Color, depth, shadow, culled, and unculled draws must retain the same crowns across independent page buffers.
TEST_CASE("Procedural2 clustered tree cards match individual impostors", "[.tree-cards-worker]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Tree-card validation requires NVGL"); return; }
    osg::DisplaySettings::instance()->setNumMultiSamples(4);
    ChonkTest::Renderer renderer; REQUIRE(renderer.initialize());
    AssetCatalog catalog({canopySource("broadleaf"),canopySource("conifer")},32u*1024u*1024u);
    ScatterGroup group;
    std::vector<Chonk::Ptr> sources, templates;
    for (const std::string name : {"broadleaf","conifer"})
    {
        sources.push_back(catalog.acquireImpostor(group,name)); REQUIRE(sources.back());
        templates.push_back(catalog.acquireTreeCards(group,name,16)); REQUIRE(templates.back());
    }
    auto* state = renderer.root->getOrCreateStateSet();
    state->setAttribute(catalog.textures());
    state->setMode(GL_CULL_FACE,osg::StateAttribute::OFF);
    state->setMode(GL_BLEND,osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED);
    state->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB,osg::StateAttribute::ON);
    state->setDefine("OE_CHONK_ALPHA_TO_COVERAGE");
    installTreeCardShader(state); installAssetLighting(state); installPopulationFadeShader(state);
    VirtualProgram::getOrCreate(state)->setFunction("p2_cards_view","void p2_cards_view(inout vec4 vertex) { }",
        VirtualProgram::LOCATION_VERTEX_VIEW);
    auto* camera = renderer.viewer.getCamera();
    camera->setProjectionMatrixAsOrtho(-65,65,-30,65,1,500);
    camera->setViewMatrixAsLookAt(osg::Vec3(0,-140,95),osg::Vec3(0,0,10),osg::Vec3(0,0,1));
    osg::ref_ptr<osg::Group> ordinary = new osg::Group(), grouped = new osg::Group();
    osg::ref_ptr<osg::Group> farGrouped = new osg::Group();
    std::vector<osg::ref_ptr<ChonkDrawable>> clusterDraws;
    for (unsigned page=0; page<2; ++page)
    {
        std::vector<TreeCardPlacement> trees;
        auto* detailed = new ChonkDrawable(); detailed->setBirthday(-10); detailed->setAlphaCutoff(0.75f);
        for (unsigned i=0; i<31; ++i)
        {
            const auto matrix = osg::Matrixd::scale(0.8,0.8,0.8)*
                osg::Matrixd::rotate(i*0.7,osg::Vec3d(0,0,1))*
                osg::Matrixd::translate(-55.0+page*65+(i%5)*10.0,(i/5)*9.0-25.0,i*0.06);
            trees.push_back({matrix,i%2});
            detailed->add(sources[i%2],osg::Matrixf(matrix));
        }
        ordinary->addChild(detailed);
        std::vector<TreeCardCluster> clusters;
        std::vector<osg::Vec4f> records;
        REQUIRE(buildTreeCardClusters(trees,sources,16,clusters,records).isOK());
        for (auto* page : {grouped.get(),farGrouped.get()})
        {
            // Each resident tier owns independent GPU output buffers, as in the production pager.
            auto* draw = new ChonkDrawable(); draw->setBirthday(-10); draw->setAlphaCutoff(0.75f);
            draw->setAuxiliaryData(records);
            REQUIRE(draw->setMemberLOD(4u,3u,0.0f));
            for (const auto& c : clusters)
                draw->add(templates[c.model],c.transform,osg::Vec2f(-float(c.count),float(c.offset)));
            page->addChild(draw); clusterDraws.push_back(draw);
        }
    }
    // Exercise production replacement callbacks, including nested handovers and a faded outer page.
    auto transitions = std::make_shared<CanopyTransitionStates>();
    osg::ref_ptr<Util::PagedNode2> mid = new Util::PagedNode2(), far = new Util::PagedNode2();
    mid->addChild(grouped); far->addChild(farGrouped);
    for (auto* page : {mid.get(),far.get()})
    {
        page->setCenter(grouped->getBound().center());
        page->setRadius(grouped->getBound().radius());
    }
    mid->setPreCompileGLObjects(false); far->setPreCompileGLObjects(false);
    mid->setLoadFunction([ordinary](Cancelable*) -> osg::ref_ptr<osg::Node> { return ordinary; });
    far->setLoadFunction([mid](Cancelable*) -> osg::ref_ptr<osg::Node> { return mid; });
    group.canopyFadeSeconds = 0.0f;
    configureCanopyTransition(mid,100.0,group,transitions);
    configureCanopyTransition(far,100.0,group,transitions);
    osg::ref_ptr<Util::PagingManager> paging = new Util::PagingManager("oe.tree-card-test");
    paging->addChild(far);
    unsigned outer = 64u;
    float globalError = 25.0f;
    paging->addCullCallback(new Util::LambdaCullCallback([&outer,&globalError](osg::Node* node, osg::NodeVisitor* nv)
    {
        nv->setUserValue("oe_sse",globalError);
        osg::Vec2f inherited(0,64);
        nv->getUserValue("oe_p2_coverage_interval",inherited);
        nv->setUserValue("oe_p2_coverage_interval",osg::Vec2f(0,float(outer)));
        nv->traverse(*node);
        nv->setUserValue("oe_p2_coverage_interval",inherited);
    }));
    renderer.setScene(paging);
    for (unsigned frame=0; frame<200u && (!mid->isLoadComplete() || !far->isLoadComplete()); ++frame)
    {
        renderer.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(mid->isLoadComplete()); REQUIRE(far->isLoadComplete());
    for (const std::string pass : {"color","OE_IS_DEPTH_CAMERA","OE_IS_SHADOW_CAMERA"})
    {
        if (pass != "color") state->setDefine(pass);
        renderer.setScene(ordinary); renderer.frame(); renderer.frame();
        auto reference = renderer.pixels();
        for (bool cull : {false,true})
        {
            for (auto& draw : clusterDraws) draw->setUseGPUCulling(cull);
            renderer.setScene(grouped); renderer.frame(); renderer.frame();
            auto pixels = renderer.pixels();
            unsigned changed = 0, visible = 0;
            for (unsigned y=0; y<256; ++y)
                for (unsigned x=0; x<256; ++x)
                {
                    const auto* a = reference->data(x,y); const auto* b = pixels->data(x,y);
                    if (unsigned(a[0])+a[1]+a[2] > 0u) ++visible;
                    if (std::abs(int(a[0])-int(b[0]))+std::abs(int(a[1])-int(b[1]))+
                        std::abs(int(a[2])-int(b[2])) > 12) ++changed;
                }
            INFO(pass << " culling " << cull << " changed " << changed << " visible " << visible);
            CHECK(visible > 2000u); CHECK(changed < visible/100u+8u);
            if (pass == "color")
                REQUIRE(osgDB::writeImageFile(*pixels,"../build/tree-cards-grouped.png"));
        }
        for (unsigned visibility : {64u,40u})
        {
            outer = visibility;
            auto* coverage = state->getOrCreateUniform("oe_chonk_coverage",osg::Uniform::FLOAT_VEC2);
            coverage->set(osg::Vec2f(0,outer/64.0f));
            renderer.setScene(ordinary); renderer.frame(); renderer.frame();
            auto full = renderer.pixels();
            for (unsigned culling=0; culling<(pass == "color" ? 4u : 1u); ++culling)
            for (bool nested : {false,true})
                for (double errorPixels : {18.75,21.875,25.0,28.125,31.25})
                {
                    for (unsigned i=0; i<clusterDraws.size(); ++i)
                        clusterDraws[i]->setUseGPUCulling((culling & (1u << (i%2u))) != 0u);
                    // Orthographic projection cancels the page radius in the error estimate.
                    const double error = errorPixels*95.0/256.0;
                    configureCanopyTransition(far,error,group,transitions);
                    configureCanopyTransition(mid,nested ? error : 100.0,group,transitions);
                    renderer.setScene(paging); renderer.frame(); renderer.frame();
                    auto pixels = renderer.pixels();
                    double expected = 0.0, actual = 0.0;
                    for (unsigned y=0; y<256; ++y)
                        for (unsigned x=0; x<256; ++x)
                            for (unsigned c=0; c<3; ++c)
                            {
                                expected += full->data(x,y)[c];
                                actual += pixels->data(x,y)[c];
                            }
                    INFO(pass << " culling=" << culling << " nested=" << nested << " outer=" << outer << " error=" << errorPixels <<
                        " retained coverage=" << actual/expected);
                    CHECK(actual > expected*0.985);
                    CHECK(actual < expected*1.015);
                }
        }
        state->getUniform("oe_chonk_coverage")->set(osg::Vec2f(0,1));
        if (pass != "color") state->removeDefine(pass);
    }

    // Quality must choose individual, medium, and far pages without reducing settled forest coverage.
    // Count actual cull traversals so an unchanged image cannot accidentally hide an unchanged LOD choice.
    state->setDefine("OE_CHONK_SSE_ADJUST"); state->setDefine("OE_CHONK_SSE_LOD_ONLY");
    auto* quality = new osg::Uniform("oe_chonk_sse_adjust",osg::Vec2f(0,1.0f/25.0f));
    state->addUniform(quality);
    configureCanopyTransition(mid,40.0*95.0/256.0,group,transitions,quality);
    configureCanopyTransition(far,100.0*95.0/256.0,group,transitions,quality);
    unsigned visits[3] = {0,0,0};
    for (unsigned tier=0; tier<3; ++tier)
    {
        osg::Node* node = tier == 0 ? ordinary.get() : tier == 1 ? grouped.get() : farGrouped.get();
        // Counters are local to this single-threaded renderer and remain alive through every frame below.
        node->addCullCallback(new Util::LambdaCullCallback([&visits,tier](osg::Node* node, osg::NodeVisitor* nv)
        { ++visits[tier]; nv->traverse(*node); }));
    }
    globalError = 10.0f;
    outer = 64u;
    state->getUniform("oe_sse")->set(globalError);
    renderer.setScene(ordinary); renderer.frame(); renderer.frame();
    auto full = renderer.pixels(); double expected = 0.0;
    for (unsigned y=0; y<256; ++y)
        for (unsigned x=0; x<256; ++x)
            for (unsigned c=0; c<3; ++c) expected += full->data(x,y)[c];
    REQUIRE(expected > 10000.0);
    for (unsigned culling=0; culling<4; ++culling)
        for (const auto& budget : {osg::Vec2f(10,0),osg::Vec2f(70,0),osg::Vec2f(10,60),
            osg::Vec2f(200,0),osg::Vec2f(25,400),osg::Vec2f(10,0)})
        {
            for (unsigned i=0; i<clusterDraws.size(); ++i)
                clusterDraws[i]->setUseGPUCulling((culling & (1u << (i%2u))) != 0u);
            globalError = budget.x(); state->getUniform("oe_sse")->set(globalError);
            quality->set(osg::Vec2f(budget.y(),1.0f/25.0f));
            renderer.setScene(paging); renderer.frame(); renderer.frame();
            const unsigned selected = budget.x()+budget.y() < 20 ? 0u : budget.x()+budget.y() < 100 ? 1u : 2u;
            // Coarse-only frames may expire detailed children. Wait for their asynchronous reload before
            // checking settled tier identity; the coverage checks above already exercise parent fallback.
            if (selected == 0u)
            {
                for (unsigned frame=0; frame<200u && (!mid->isLoadComplete() || !far->isLoadComplete()); ++frame)
                { renderer.frame(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
                REQUIRE(mid->isLoadComplete()); REQUIRE(far->isLoadComplete());
            }
            visits[0] = visits[1] = visits[2] = 0u;
            renderer.frame(); renderer.frame();
            INFO("budget=" << budget.x()+budget.y() << " visits=" << visits[0] << "," << visits[1] << "," << visits[2]);
            for (unsigned tier=0; tier<3; ++tier) CHECK((visits[tier] > 0u) == (tier == selected));
            auto pixels = renderer.pixels(); double actual = 0.0;
            for (unsigned y=0; y<256; ++y)
                for (unsigned x=0; x<256; ++x)
                    for (unsigned c=0; c<3; ++c) actual += pixels->data(x,y)[c];
            INFO("forest handover: tier=" << selected << " culling=" << culling << " coverage=" << actual/expected);
            CHECK(actual > expected*0.985); CHECK(actual < expected*1.015);
        }
}


//! Isolates generic member cutoffs and the range-limited forest policy in a fresh GL process.
TEST_CASE("Procedural2 tree card quality and range policies match individual visibility", "[procedural2][treecards][gpu][quality]")
{
    const auto& executable = osgEarth::Tests::executablePath;
#ifdef _WIN32
    const std::string command = "\"\""+executable+"\" \"[.tree-cards-quality-worker]\"\"";
#else
    std::string quoted = "'";
    for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1,c);
    const std::string command = quoted+"' '[.tree-cards-quality-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
}

//! Compares identical trees across grouping sizes, both projections, live additive budgets, and GPU culling modes.
TEST_CASE("Procedural2 tree card member-sized GPU LOD", "[.tree-cards-quality-worker]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Tree-card quality validation requires NVGL"); return; }
    osg::DisplaySettings::instance()->setNumMultiSamples(4);
    ChonkTest::Renderer renderer; REQUIRE(renderer.initialize());
    AssetCatalog catalog({canopySource("broadleaf"),canopySource("conifer")},32u*1024u*1024u);
    ScatterGroup group;
    std::vector<Chonk::Ptr> sources;
    for (const std::string name : {"broadleaf","conifer"})
    {
        sources.push_back(catalog.acquireImpostor(group,name)); REQUIRE(sources.back());
        // Set the ordinary reference's authored lower LOD cutoff before it creates any GPU commands.
        sources.back()->_lods[0].far_pixel_scale = 4.0f;
    }
    auto* state = renderer.root->getOrCreateStateSet();
    state->setAttribute(catalog.textures());
    state->setMode(GL_CULL_FACE,osg::StateAttribute::OFF);
    state->setMode(GL_BLEND,osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED);
    state->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB,osg::StateAttribute::ON);
    state->setDefine("OE_CHONK_ALPHA_TO_COVERAGE"); state->setDefine("OE_CHONK_SSE_ADJUST");
    state->setDefine("OE_CHONK_SSE_PIXEL_CUTOFF");
    auto* global = state->getUniform("oe_sse");
    auto* quality = new osg::Uniform("oe_chonk_sse_adjust",osg::Vec2f(0,1.0f/25.0f));
    state->addUniform(quality);
    state->addUniform(new osg::Uniform("oe_chonk_lod_transition_factor",0.2f));
    installTreeCardShader(state); installAssetLighting(state);
    VirtualProgram::getOrCreate(state)->setFunction("p2_member_view","void p2_member_view(inout vec4 vertex) { }",
        VirtualProgram::LOCATION_VERTEX_VIEW);
    std::vector<TreeCardPlacement> trees;
    osg::ref_ptr<ChonkDrawable> ordinary = new ChonkDrawable();
    ordinary->setBirthday(-10); ordinary->setAlphaCutoff(0.75f); ordinary->setFadeNearFar(200,500);
    for (unsigned i=0; i<97; ++i)
    {
        const double scale = 0.55+0.009*i;
        const auto matrix = osg::Matrixd::scale(scale,scale,scale)*
            osg::Matrixd::rotate(i*0.7,osg::Vec3d(0,0,1))*
            osg::Matrixd::translate(-45.0+(i%11)*9.0,(i/11)*10.0-40.0,i*0.04);
        trees.push_back({matrix,i%2}); ordinary->add(sources[i%2],osg::Matrixf(matrix));
    }
    auto* camera = renderer.viewer.getCamera();
    camera->setViewMatrixAsLookAt(osg::Vec3(0,-190,125),osg::Vec3(0,0,10),osg::Vec3(0,0,1));
    auto* ext = renderer.context->getState()->get<osg::GLExtensions>();
    for (bool lodOnly : {false,true})
    for (unsigned slots : {8u,32u,128u})
    {
        if (lodOnly) state->setDefine("OE_CHONK_SSE_LOD_ONLY");
        else state->removeDefine("OE_CHONK_SSE_LOD_ONLY");
        std::vector<Chonk::Ptr> templates;
        for (const auto& source : sources)
        {
            Chonk::Ptr model; REQUIRE(createTreeCardTemplate(*source,slots,model).isOK()); templates.push_back(model);
        }
        std::vector<TreeCardCluster> clusters; std::vector<osg::Vec4f> records;
        REQUIRE(buildTreeCardClusters(trees,sources,slots,clusters,records).isOK());
        osg::ref_ptr<ChonkDrawable> grouped = new ChonkDrawable();
        grouped->setBirthday(-10); grouped->setAlphaCutoff(0.75f); grouped->setFadeNearFar(200,500);
        grouped->setAuxiliaryData(records); REQUIRE(grouped->setMemberLOD(4,3,4.0f));
        CHECK_FALSE(grouped->setMemberLOD(4,4,4.0f));
        CHECK_FALSE(grouped->setMemberLOD(4,3,-1.0f));
        for (const auto& c : clusters)
            grouped->add(templates[c.model],c.transform,osg::Vec2f(-float(c.count),float(c.offset)));
        for (bool ortho : {false,true})
        {
            if (ortho) camera->setProjectionMatrixAsOrtho(-80,80,-55,85,1,700);
            else camera->setProjectionMatrixAsPerspective(45,1,1,700);
            double full = 0.0, last = 0.0;
            for (const auto& budget : {osg::Vec2f(5,0),osg::Vec2f(15,0),osg::Vec2f(25,0),
                osg::Vec2f(5,20),osg::Vec2f(40,0),osg::Vec2f(425,0),osg::Vec2f(5,0)})
            {
                global->set(budget.x()); quality->set(osg::Vec2f(budget.y(),1.0f/25.0f));
                renderer.setScene(ordinary); renderer.frame(); renderer.frame();
                auto reference = renderer.pixels();
                double expected = 0.0;
                for (unsigned y=0; y<256; ++y)
                    for (unsigned x=0; x<256; ++x)
                        for (unsigned c=0; c<3; ++c) expected += reference->data(x,y)[c];
                if (budget == osg::Vec2f(5,0))
                {
                    if (full == 0.0) full = expected;
                    CHECK(expected == full); CHECK(full > 10000.0);
                }
                if (budget == osg::Vec2f(25,0)) last = expected;
                if (budget == osg::Vec2f(5,20))
                {
                    CHECK(expected == last);
                    if (!lodOnly) CHECK(expected < full);
                }
                if (lodOnly) CHECK(expected == full);
                else if (budget.x() == 425) CHECK(expected == 0.0);
                for (bool cull : {false,true})
                {
                    grouped->setUseGPUCulling(cull);
                    renderer.setScene(grouped); renderer.frame(); renderer.frame();
                    auto pixels = renderer.pixels();
                    double actual = 0.0; unsigned changed = 0u;
                    for (unsigned y=0; y<256; ++y)
                        for (unsigned x=0; x<256; ++x)
                        {
                            unsigned delta = 0u;
                            for (unsigned c=0; c<3; ++c)
                            {
                                actual += pixels->data(x,y)[c];
                                delta += unsigned(std::abs(int(reference->data(x,y)[c])-int(pixels->data(x,y)[c])));
                            }
                            if (delta > 12u) ++changed;
                        }
                    INFO("slots=" << slots << " ortho=" << ortho << " cull=" << cull <<
                        " budget=" << budget.x()+budget.y() << " expected=" << expected << " actual=" << actual);
                    CHECK(changed < 100u);
                    CHECK(std::abs(actual-expected) <= std::max(16.0,expected*0.015));
                    if (!lodOnly && budget.x() == 425)
                    {
                        constexpr GLenum primitiveQuery = 0x8C87; // GL_PRIMITIVES_GENERATED (missing in OSG's GL headers)
                        GLuint query = 0, primitives = 0;
                        ext->glGenQueries(1,&query); ext->glBeginQuery(primitiveQuery,query);
                        renderer.frame(); ext->glEndQuery(primitiveQuery);
                        ext->glGetQueryObjectuiv(query,GL_QUERY_RESULT,&primitives); ext->glDeleteQueries(1,&query);
                        // A fully rejected cluster issues no geometry with GPU culling; CPU-only culling keeps slots.
                        CHECK((cull ? primitives == 0u : primitives > 0u));
                    }
                }
            }
        }
    }
    state->removeDefine("OE_CHONK_SSE_LOD_ONLY");
    // Independent unit check: this orthographic viewport has exactly one pixel per world unit.
    // A scaled sphere of diameter D therefore projects to D pixels, without reproducing the shader's math.
    state->setDefine("OE_CHONK_SSE_PIXEL_CUTOFF");
    camera->setProjectionMatrixAsOrtho(-128,128,-128,128,1,1000);
    camera->setViewMatrixAsLookAt(osg::Vec3(0,-500,0),osg::Vec3(),osg::Vec3(0,0,1));
    auto detailed = catalog.acquire(group,"broadleaf"); REQUIRE(detailed); REQUIRE(detailed->_lods.size() == 2u);
    for (const auto& source : {sources.front(),detailed})
    for (float diameter : {20.0f,80.0f,200.0f})
    {
        const double scale = diameter/(2.0*source->_box.radius());
        const auto matrix = osg::Matrixd::translate(-osg::Vec3d(source->_box.center()))*
            osg::Matrixd::scale(scale,scale,scale);
        osg::ref_ptr<ChonkDrawable> single = new ChonkDrawable();
        single->setBirthday(-10); single->setAlphaCutoff(0.75f); single->add(source,osg::Matrixf(matrix));
        osg::ref_ptr<ChonkDrawable> cluster;
        if (source == sources.front())
        {
            Chonk::Ptr model; REQUIRE(createTreeCardTemplate(*source,32,model).isOK());
            std::vector<TreeCardCluster> clusters; std::vector<osg::Vec4f> records;
            REQUIRE(buildTreeCardClusters({{matrix,0}},{source},32,clusters,records).isOK());
            REQUIRE(clusters.size() == 1u);
            cluster = new ChonkDrawable(); cluster->setBirthday(-10); cluster->setAlphaCutoff(0.75f);
            cluster->setAuxiliaryData(records); REQUIRE(cluster->setMemberLOD(4,3,4.0f));
            cluster->add(model,clusters[0].transform,osg::Vec2f(-1,0));
        }
        double full = 0.0;
        for (const auto& budget : {osg::Vec2f(1,0),osg::Vec2f(25,0),osg::Vec2f(1,diameter*0.9f-1),
            osg::Vec2f(1,diameter*1.1f-1),osg::Vec2f(1,diameter*1.4f-1),
            osg::Vec2f(25,400),osg::Vec2f(425,0),osg::Vec2f(1,0)})
        {
            global->set(budget.x()); quality->set(osg::Vec2f(budget.y(),1.0f/25.0f));
            renderer.setScene(single); renderer.frame(); renderer.frame();
            auto reference = renderer.pixels();
            double expected = 0.0;
            for (unsigned y=0; y<256; ++y)
                for (unsigned x=0; x<256; ++x)
                    for (unsigned c=0; c<3; ++c) expected += reference->data(x,y)[c];
            const float error = budget.x()+budget.y();
            INFO("literal pixel cutoff: diameter=" << diameter << " error=" << error <<
                " lods=" << source->_lods.size() << " color=" << expected);
            if (error == 1.0f) { if (full == 0.0) full = expected; CHECK(expected == full); CHECK(full > 1000.0); }
            if (error < diameter*0.95f) CHECK(expected > 0.0);
            if (error > diameter && error < diameter*1.2f)
            {
                CHECK(expected > 0.0);
                // Compare identical art: the two-LOD model may have changed representation since the baseline.
                if (source->_lods.size() == 1u) CHECK(expected < full);
            }
            if (error > diameter*1.25f) CHECK(expected == 0.0);
            // At global 25 plus offset 400, every tree in this 256px test viewport must be invisible.
            if (budget.y() == 400.0f || budget.x() == 425.0f) CHECK(expected == 0.0);
            if (cluster)
                for (bool cull : {false,true})
                {
                    cluster->setUseGPUCulling(cull);
                    renderer.setScene(cluster); renderer.frame(); renderer.frame();
                    auto pixels = renderer.pixels(); double actual = 0.0;
                    for (unsigned y=0; y<256; ++y)
                        for (unsigned x=0; x<256; ++x)
                            for (unsigned c=0; c<3; ++c) actual += pixels->data(x,y)[c];
                    CHECK(std::abs(actual-expected) <= std::max(16.0,expected*0.015));
                    if (error > diameter*1.25f) CHECK(actual == 0.0);
                }
        }
        // Forest quality may change the mesh LOD but cannot erase the final representation, even at 425px.
        // Moving the independent range cap inside the camera distance must still remove that same tree.
        state->setDefine("OE_CHONK_SSE_LOD_ONLY");
        global->set(25.0f); quality->set(osg::Vec2f(400,1.0f/25.0f));
        for (float range : {0.0f,400.0f,600.0f})
        {
            single->setFadeNearFar(range*0.8f,range);
            renderer.setScene(single); renderer.frame(); renderer.frame();
            auto reference = renderer.pixels(); double expected = 0.0;
            for (unsigned y=0; y<256; ++y)
                for (unsigned x=0; x<256; ++x)
                    for (unsigned c=0; c<3; ++c) expected += reference->data(x,y)[c];
            INFO("range-limited forest: diameter=" << diameter << " range=" << range);
            if (range == 400.0f) CHECK(expected == 0.0);
            else CHECK(expected > 1000.0);
            if (cluster)
                for (bool cull : {false,true})
                {
                    cluster->setUseGPUCulling(cull); cluster->setFadeNearFar(range*0.8f,range);
                    renderer.setScene(cluster); renderer.frame(); renderer.frame();
                    auto pixels = renderer.pixels(); double actual = 0.0;
                    for (unsigned y=0; y<256; ++y)
                        for (unsigned x=0; x<256; ++x)
                            for (unsigned c=0; c<3; ++c) actual += pixels->data(x,y)[c];
                    CHECK(std::abs(actual-expected) <= std::max(16.0,expected*0.015));
                }
        }
        state->removeDefine("OE_CHONK_SSE_LOD_ONLY");
    }
    CHECK(glGetError() == GL_NO_ERROR);
}

//! Isolates live cluster visualization from other tests' shared GL programs.
TEST_CASE("Procedural2 cluster debug colors preserve rendering", "[procedural2][treecards][gpu][debug]")
{
    const auto& executable = osgEarth::Tests::executablePath;
#ifdef _WIN32
    const std::string command = "\"\""+executable+"\" \"[.tree-cards-debug-worker]\"\"";
#else
    std::string quoted = "'";
    for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1,c);
    const std::string command = quoted+"' '[.tree-cards-debug-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
}

//! Checks tier isolation, stable cluster colors, A2C coverage, restore, and depth-pass exclusion with both cull paths.
TEST_CASE("Procedural2 cluster visualization GPU contract", "[.tree-cards-debug-worker]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Cluster visualization validation requires NVGL"); return; }
    osg::DisplaySettings::instance()->setNumMultiSamples(4);
    ChonkTest::Renderer renderer; REQUIRE(renderer.initialize());
    AssetCatalog catalog({canopySource("broadleaf")},16u*1024u*1024u);
    ScatterGroup group;
    auto source = catalog.acquireImpostor(group,"broadleaf"); REQUIRE(source);
    Chonk::Ptr model; REQUIRE(createTreeCardTemplate(*source,8,model).isOK());
    auto* state = renderer.root->getOrCreateStateSet();
    state->setAttribute(catalog.textures());
    state->setMode(GL_CULL_FACE,osg::StateAttribute::OFF);
    state->setMode(GL_BLEND,osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED);
    state->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB,osg::StateAttribute::ON);
    state->setDefine("OE_CHONK_ALPHA_TO_COVERAGE");
    auto* debug = new osg::Uniform("oe_p2_cluster_debug",osg::Vec3f(0,1,1)); state->addUniform(debug);
    installTreeCardShader(state); installAssetLighting(state);
    VirtualProgram::getOrCreate(state)->setFunction("p2_debug_view","void p2_debug_view(inout vec4 vertex) { }",
        VirtualProgram::LOCATION_VERTEX_VIEW);
    osg::ref_ptr<osg::Group> scene = new osg::Group();
    osg::ref_ptr<ChonkDrawable> draws[2];
    for (unsigned tier=0; tier<2; ++tier)
    {
        std::vector<TreeCardPlacement> trees;
        for (unsigned i=0; i<16; ++i)
            trees.push_back({osg::Matrixd::scale(0.55,0.55,0.55)*
                osg::Matrixd::translate(-55.0+(i%4)*11.0,-35.0+tier*60.0+(i/4)*10.0,0),0});
        std::vector<TreeCardCluster> clusters; std::vector<osg::Vec4f> records;
        REQUIRE(buildTreeCardClusters(trees,{source},8,clusters,records).isOK()); REQUIRE(clusters.size() == 2u);
        auto draw = new ChonkDrawable(); draws[tier] = draw;
        draw->setBirthday(-10); draw->setAlphaCutoff(0.75f); draw->setAuxiliaryData(records);
        REQUIRE(draw->setMemberLOD(4,3,0));
        for (const auto& cluster : clusters) draw->add(model,cluster.transform,treeCardInstanceUV(cluster,tier != 0u));
        scene->addChild(draw);
    }
    auto individual = new ChonkDrawable(); individual->setBirthday(-10); individual->setAlphaCutoff(0.75f);
    individual->add(source,osg::Matrixf::translate(45,0,0)); scene->addChild(individual);
    renderer.setScene(scene);
    auto* camera = renderer.viewer.getCamera();
    camera->setProjectionMatrixAsOrtho(-75,75,-60,80,1,700);
    camera->setViewMatrixAsLookAt(osg::Vec3(0,-200,160),osg::Vec3(),osg::Vec3(0,0,1));
    renderer.frame(); renderer.frame(); const auto normal = renderer.pixels();
    //! Reads depth coverage directly; black source texels can become colored without changing coverage.
    auto depthPixels = []()
    {
        std::vector<float> result(256u*256u);
        glReadPixels(0,0,256,256,GL_DEPTH_COMPONENT,GL_FLOAT,result.data());
        return result;
    };
    const auto normalDepth = depthPixels();
    //! Compares color only; visibility/alpha are checked separately, without using debug palette implementation details.
    auto differences = [](const osg::Image* a, const osg::Image* b)
    {
        unsigned result = 0;
        for (unsigned i=0; i<256u*256u; ++i)
            if (std::abs(int(a->data()[4*i])-int(b->data()[4*i]))+
                std::abs(int(a->data()[4*i+1])-int(b->data()[4*i+1]))+
                std::abs(int(a->data()[4*i+2])-int(b->data()[4*i+2])) > 5) ++result;
        return result;
    };
    for (unsigned mode : {1u,2u})
    {
        osg::ref_ptr<osg::Image> combined;
        unsigned changed[4] = {};
        for (unsigned tiers : {1u,2u,3u})
        {
            debug->set(osg::Vec3f(float(mode),float(tiers & 1u),float((tiers >> 1u) & 1u)));
            renderer.frame(); renderer.frame(); auto pixels = renderer.pixels();
            changed[tiers] = differences(normal,pixels); CHECK(changed[tiers] > 100u);
            const auto currentDepth = depthPixels();
            unsigned coverageChanges = 0, individualChanges = 0;
            std::set<unsigned> cool, warm;
            for (unsigned i=0; i<256u*256u; ++i)
            {
                const auto* a = normal->data()+4*i; const auto* b = pixels->data()+4*i;
                if (normalDepth[i] != currentDepth[i] || a[3] != b[3]) ++coverageChanges;
                if (i%256u > 180u && (a[0] != b[0] || a[1] != b[1] || a[2] != b[2])) ++individualChanges;
                const unsigned rgb = (unsigned(b[0]) << 16u) | (unsigned(b[1]) << 8u) | b[2];
                if (b[2] > b[0]*2u) cool.insert(rgb);
                if (b[0] > b[2]*2u) warm.insert(rgb);
            }
            CHECK(coverageChanges == 0u); CHECK(individualChanges == 0u);
            if (tiers & 1u) CHECK(cool.size() > 1u);
            if (tiers & 2u) CHECK(warm.size() > 1u);
            if (tiers == 3u) combined = pixels;
        }
        CHECK(changed[3] == changed[1]+changed[2]);
        for (bool cull : {false,true})
        {
            for (auto& draw : draws) draw->setUseGPUCulling(cull);
            // Reverse source traversal to catch colors keyed to visible draw order or lost per-page tier state.
            scene->removeChildren(0,scene->getNumChildren());
            scene->addChild(individual); scene->addChild(draws[1]); scene->addChild(draws[0]);
            renderer.frame(); renderer.frame(); CHECK(differences(combined,renderer.pixels()) == 0u);
        }
    }
    debug->set(osg::Vec3f(0,1,1)); renderer.frame(); renderer.frame();
    CHECK(differences(normal,renderer.pixels()) == 0u);
    state->setDefine("OE_IS_DEPTH_CAMERA"); renderer.frame(); renderer.frame(); auto depth = renderer.pixels();
    debug->set(osg::Vec3f(2,1,1)); renderer.frame(); renderer.frame();
    CHECK(differences(depth,renderer.pixels()) == 0u);
    CHECK(glGetError() == GL_NO_ERROR);
}
