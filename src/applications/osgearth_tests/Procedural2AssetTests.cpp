/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarthProcedural2/VegetationLayer2>
#include <osgEarthProcedural2/Canopy>
#include <osgEarthProcedural2/CanopyTransition.h>
#include "ChonkTestUtils.h"
#include <cstdlib>
#include <thread>
#include <osgEarth/SimplePager>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <future>
#include <chrono>
#include <set>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

namespace
{
    //! Resolves the checked-in starter catalog from the required tests working directory.
    ScatterAsset starter(const std::string& name)
    {
        Config conf("asset");
        conf.setReferrer(osgDB::getRealPath("procedural2.earth"));
        conf.set("name", name);
        conf.set("near", "../data/procedural2/starter/" + name + "-near.osg");
        conf.set("coarse", "../data/procedural2/starter/" + name + "-coarse.osg");
        return ScatterAsset(conf);
    }
}

//! Model selection survives density changes and config round trips without changing source positions.
TEST_CASE("Procedural2 catalog choices are deterministic and validated", "[procedural2]")
{
    auto a = starter("broadleaf");
    ScatterAsset restored(a.getConfig());
    CHECK(restored.nearModel.full() == a.nearModel.full());
    VegetationLayer2::Options options;
    options.profile() = ProfileOptions("global-geodetic");
    options.assets().push_back(a);
    options.groups()[0].models = {"broadleaf"};
    osg::ref_ptr<VegetationLayer2> layer = new VegetationLayer2(options);
    REQUIRE(layer->open().isOK());
    auto group = layer->options().groups()[0];
    auto invalid = group;
    invalid.models = {"missing"};
    CHECK(layer->setGroup(invalid).isError());
    CHECK(layer->options().groups()[0].models == group.models);
    VegetationLayer2::Options copy(layer->options().getConfig());
    CHECK(copy.groups()[0].models == group.models);
    CHECK(copy.assets()[0].nearModel.full() == a.nearModel.full());
    auto profile = Profile::create("global-geodetic");
    auto key = profile->createTileKey(-75.0, 40.65, group.cellLevel);
    UniformScatterSource source;
    std::vector<ScatterPlacement> before, after;
    REQUIRE(source.generate(key, group, 17, before).isOK());
    group.models.clear();
    REQUIRE(source.generate(key, group, 17, after).isOK());
    REQUIRE(before.size() == after.size());
    for (std::size_t i = 0; i < before.size(); ++i)
    {
        CHECK(before[i].id == after[i].id);
        CHECK(before[i].point == after[i].point);
    }
    unsigned counts[2] = {0, 0};
    double rankSum[2] = {0, 0};
    for (unsigned i = 0; i < 10000; ++i)
    {
        ScatterPlacement p;
        p.id = i;
        auto choice = p.modelIndex(2);
        REQUIRE(choice < 2u);
        ++counts[choice];
        rankSum[choice] += p.densityRank();
        CHECK(p.modelIndex(2) == choice);
    }
    for (unsigned i = 0; i < 2; ++i)
    {
        CHECK(counts[i] > 4700u);
        CHECK(std::abs(rankSum[i] / counts[i] - 0.5) < 0.02);
    }
}

//! OSG must retain precompressed mips and the source's orientation/cutouts, avoiding render-time conversion.
TEST_CASE("Procedural2 runtime atlases retain compression mips and cutouts", "[procedural2]")
{
    const char* names[] = {"broadleaf", "conifer", "shrub", "grass-tuft", "fern", "boulder", "canopy-cluster",
        "broadleaf-canopy", "conifer-canopy"};
    for (const auto* name : names)
    {
        INFO(name);
        const std::string path = "../data/procedural2/starter/" + std::string(name) + "-atlas";
        auto source = osgDB::readRefImageFile(path + ".png");
        auto runtime = osgDB::readRefImageFile(path + ".dds");
        REQUIRE(source);
        REQUIRE(runtime);
        REQUIRE(runtime->isCompressed());
        CHECK(runtime->getPixelFormat() == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);
        REQUIRE(runtime->s() == source->s());
        REQUIRE(runtime->t() == source->t());
        unsigned mipCount = 0;
        std::size_t bytes = 0;
        for (int w = source->s(), h = source->t(); ; w = std::max(1, w/2), h = std::max(1, h/2))
        {
            ++mipCount;
            bytes += std::max(1, (w+3)/4) * std::max(1, (h+3)/4) * 16u;
            if (w == 1 && h == 1) break;
        }
        CHECK(runtime->getNumMipmapLevels() == mipCount);
        CHECK(runtime->getTotalSizeInBytesIncludingMipmaps() == bytes);
        double alphaError = 0.0, colorError = 0.0;
        unsigned samples = 0, opaque = 0;
        for (int y = 0; y < source->t(); y += 3)
            for (int x = 0; x < source->s(); x += 3)
            {
                const auto a = source->getColor(x, y), b = runtime->getColor(x, y);
                alphaError += std::abs(a.a()-b.a());
                ++samples;
                if (a.a() > 0.95f)
                {
                    colorError += (std::abs(a.r()-b.r())+std::abs(a.g()-b.g())+std::abs(a.b()-b.b()))/3.0;
                    ++opaque;
                }
            }
        REQUIRE(opaque > 0u);
        CHECK(alphaError/samples < 0.02);
        CHECK(colorError/opaque < 0.08);
    }
}

//! Concurrent workers share one real textured bundle; source/LOD edits and final-owner release obey the budget.
TEST_CASE("Procedural2 asset residency follows actual Chonk ownership", "[procedural2]")
{
    AssetCatalog catalog({starter("broadleaf"), starter("conifer")}, 32u * 1024u * 1024u);
    ScatterGroup group;
    std::vector<std::future<Chonk::Ptr>> jobs;
    for (unsigned i = 0; i < 4; ++i)
        jobs.emplace_back(std::async(std::launch::async, [&]() { return catalog.acquire(group, "broadleaf"); }));
    std::vector<Chonk::Ptr> owners;
    for (auto& job : jobs) owners.push_back(job.get());
    REQUIRE(owners.front());
    for (auto& owner : owners) CHECK(owner == owners.front());
    CHECK(owners.front()->_lods.size() == 2u);
    CHECK_FALSE(owners.front()->_lods[0].alphaTested);
    CHECK(owners.front()->_lods[1].alphaTested);
    CHECK(catalog.residency().assets == 1u);
    const auto bytes = catalog.residency().bytes;
    CHECK(bytes > 1024u * 1024u); // includes decoded atlas, not just mesh bytes
    group.density *= 2;
    group.maxRange *= 2;
    auto same = catalog.acquire(group, "broadleaf");
    CHECK(same == owners.front());
    auto drawable = new ChonkDrawable(0);
    osg::ref_ptr<ChonkDrawable> retained = drawable;
    drawable->add(same, osg::Matrixf());
    same.reset();
    owners.clear();
    CHECK(catalog.residency().bytes == bytes);
    retained = nullptr;
    CHECK(catalog.residency().bytes == 0u);
    CHECK(catalog.residency().assets == 0u);
    auto reloaded = catalog.acquire(group, "broadleaf");
    REQUIRE(reloaded);
    CHECK(catalog.residency().bytes == bytes);
    AssetCatalog tight({starter("broadleaf"), starter("conifer")}, bytes);
    auto admitted = tight.acquire(group, "broadleaf");
    REQUIRE(admitted);
    CHECK_FALSE(tight.acquire(group, "conifer"));
    CHECK(tight.residency().bytes <= tight.residency().budget);
    CHECK(tight.residency().budgetDenials == 1u);
    osg::ref_ptr<ProgressCallback> canceled = new ProgressCallback();
    canceled->cancel();
    CHECK_FALSE(catalog.acquire(group, "conifer", canceled));
    CHECK(catalog.residency().assets == 1u);
}

//! Starter files all load through the production Chonk path; invalid URIs remain bounded and allow fallback.
TEST_CASE("Procedural2 starter art has two valid representations", "[procedural2]")
{
    const char* names[] = {"broadleaf", "conifer", "shrub", "grass-tuft", "fern", "boulder", "canopy-cluster"};
    std::vector<ScatterAsset> assets;
    for (auto name : names) assets.push_back(starter(name));
    AssetCatalog catalog(assets, 64u * 1024u * 1024u);
    ScatterGroup group;
    for (auto name : names)
    {
        INFO(name);
        auto model = catalog.acquire(group, name);
        REQUIRE(model);
        CHECK(model->getBound().valid());
        CHECK(model->_lods[1].length == 18u);
        CHECK(model->_lods[0].length > model->_lods[1].length);
        // The disk marker must survive OSG parsing and Chonk conversion; real near leaves keep geometric normals.
        for (unsigned level=0; level<2; ++level)
        {
            const auto& lod = model->_lods[level];
            for (std::size_t i=lod.offset; i<lod.offset+lod.length; ++i)
            {
                const auto& vertex = model->_vbo_store[model->_ebo_store[i]+lod.base_vertex];
                CHECK(vertex.normal_technique == (level == 1 || std::string(name) == "canopy-cluster" ?
                    Chonk::NORMAL_TECHNIQUE_VOLUME : Chonk::NORMAL_TECHNIQUE_DEFAULT));
            }
        }
    }
    CHECK(catalog.residency().bytes == 0u);
    CHECK_FALSE(catalog.acquire(group, "not-in-catalog"));
    CHECK_FALSE(catalog.acquire(group, "not-in-catalog"));
    CHECK(catalog.residency().failedLoads == 1u);
    auto fallback = catalog.acquire(group, "");
    REQUIRE(fallback);
    CHECK(catalog.residency().assets == 1u);
    // A disabled coarse representation must not decode a coarse file or texture.
    group.lodPixels = 0;
    auto nearOnly = catalog.acquire(group, "broadleaf");
    REQUIRE(nearOnly);
    CHECK(nearOnly->_lods.size() == 1u);
    auto broken = starter("broadleaf");
    broken.coarseModel = URI("missing-procedural2-coarse-model.osg");
    AssetCatalog failed({broken}, 64u * 1024u * 1024u);
    group.lodPixels = 32;
    CHECK_FALSE(failed.acquire(group, "broadleaf"));
    CHECK(failed.residency().bytes == 0u);
    CHECK(failed.residency().assets == 0u);
    CHECK(failed.residency().failedLoads == 1u);
    // The near-only mode remains usable even if the unused coarse URI is broken.
    group.lodPixels = 0;
    auto usable = failed.acquire(group, "broadleaf");
    REQUIRE(usable);
}

//! Unsupported dynamic semantics and broken texture references fail instead of producing incorrect white geometry.
TEST_CASE("Procedural2 rejects unsupported and incomplete model graphs", "[procedural2]")
{
    auto definition = starter("broadleaf");
    osg::ref_ptr<osg::Node> node = definition.nearModel.getNode();
    REQUIRE(node);
    node->setDataVariance(osg::Object::DYNAMIC);
    const std::string file = "../build/procedural2-invalid-model.osgt";
    REQUIRE(osgDB::writeNodeFile(*node, file));
    definition.nearModel = URI(osgDB::getRealPath(file));
    AssetCatalog dynamic({definition}, 64u * 1024u * 1024u);
    ScatterGroup group;
    CHECK_FALSE(dynamic.acquire(group, "broadleaf"));
    CHECK(dynamic.residency().bytes == 0u);
    node->setDataVariance(osg::Object::STATIC);
    osg::ref_ptr<osg::Texture2D> missingTexture = new osg::Texture2D();
    node->getOrCreateStateSet()->setTextureAttribute(0, missingTexture);
    REQUIRE(osgDB::writeNodeFile(*node, file));
    AssetCatalog missing({definition}, 64u * 1024u * 1024u);
    CHECK_FALSE(missing.acquire(group, "broadleaf"));
    CHECK(missing.residency().bytes == 0u);
    auto repaired = starter("broadleaf");
    const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto repairFile = "../build/procedural2-repair-" + std::to_string(serial) + ".osg";
    repaired.nearModel = URI(repairFile);
    group.lodPixels = 0;
    AssetCatalog beforeRepair({repaired}, 64u * 1024u * 1024u);
    CHECK_FALSE(beforeRepair.acquire(group, "broadleaf"));
    node->setStateSet(nullptr);
    REQUIRE(osgDB::writeNodeFile(*node, repairFile));
    AssetCatalog afterRepair({repaired}, 64u * 1024u * 1024u);
    REQUIRE(afterRepair.acquire(group, "broadleaf"));
}

//! Irregular shared art must stay inside certified footprints at every occupancy, without per-patch mesh growth.
TEST_CASE("Procedural2 canopy layouts are shared and remain inside exclusion footprints", "[procedural2][canopy]")
{
    AssetCatalog catalog({}, 4u*1024u*1024u);
    ScatterGroup group;
    group.lodPixels = group.minPixels = 0.0f;
    std::vector<Chonk::Ptr> owners;
    std::set<std::pair<float,float>> firstPositions;
    for (unsigned coverage=1; coverage<=8; ++coverage)
        for (unsigned layout=0; layout<4; ++layout)
        {
            group.asset = "canopy"+std::to_string(coverage)+"-"+std::to_string(layout);
            auto model = catalog.acquire(group, "");
            REQUIRE(model);
            CHECK(model == catalog.acquire(group, ""));
            owners.push_back(model);
            CHECK(model->_ebo_store.size() == 9u*12u*3u);
            REQUIRE_FALSE(model->_vbo_store.empty());
            if (coverage == 8u)
                firstPositions.emplace(model->_vbo_store.front().position.x(), model->_vbo_store.front().position.y());
            for (const auto& vertex : model->_vbo_store)
            {
                CHECK(std::abs(vertex.position.x()) <= 0.500001f);
                CHECK(std::abs(vertex.position.y()) <= 0.500001f);
                CHECK(std::isfinite(vertex.position.z()));
                CHECK(vertex.position.z() >= 0.0f);
                CHECK(vertex.normal_technique == Chonk::NORMAL_TECHNIQUE_VOLUME);
                CHECK(std::abs(vertex.normal.length()-1.0f) < 1e-5f);
                CHECK(vertex.normal.z() >= 0.5f); // upward foliage lobes, including the proxy's lower faces
                // Coincident corners must shade continuously across the proxy's triangle boundaries.
                for (const auto& other : model->_vbo_store)
                    if (other.position == vertex.position) CHECK((other.normal-vertex.normal).length() < 1e-5f);
            }
        }
    CHECK(firstPositions.size() == 4u);
    CHECK(catalog.residency().assets == 32u);
    CHECK(catalog.residency().bytes < 4u*1024u*1024u);
}

namespace osgEarth { namespace Tests { extern std::string executablePath; } }

//! Isolates canopy GPU validation from process-global Chonk graphics-context resources used by other tests.
TEST_CASE("Procedural2 canopy GPU contract", "[procedural2][canopy][gpu]")
{
    const auto& executable = osgEarth::Tests::executablePath;
#ifdef _WIN32
    const std::string command = "\"\""+executable+"\" \"[.procedural2-canopy-gpu-worker]\"\"";
#else
    std::string quoted = "'";
    for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1,c);
    const std::string command = quoted+"' '[.procedural2-canopy-gpu-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
}

//! Reads actual color/shadow shader output: same-bin fades must remain complementary and masked crowns must vanish.
TEST_CASE("Procedural2 canopy GPU coverage and crown masks", "[.procedural2-canopy-gpu-worker]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Canopy validation requires NVGL"); return; }
    osg::DisplaySettings::instance()->setNumMultiSamples(4);
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    auto* rootState = renderer.root->getOrCreateStateSet();
    installCanopyShader(rootState);
    rootState->setMode(GL_CULL_FACE,osg::StateAttribute::OFF);
    auto* camera = renderer.viewer.getCamera();
    camera->setProjectionMatrixAsOrtho(-2,2,-2,2,1,20);
    camera->setViewMatrixAsLookAt(osg::Vec3d(0,0,8),osg::Vec3d(),osg::Vec3d(0,1,0));
    osg::ref_ptr<osg::Group> scene = new osg::Group();
    osg::ref_ptr<osg::Uniform> intervals[2];
    osg::ref_ptr<ChonkDrawable> tierDrawables[2];
    for (unsigned i=0; i<2; ++i)
    {
        auto geometry = ChonkTest::mesh();
        auto* colors = dynamic_cast<osg::Vec4Array*>(geometry->getColorArray());
        (*colors)[0] = i == 0 ? osg::Vec4(1,0,0,1) : osg::Vec4(0,1,0,1);
        auto model = Chonk::create();
        REQUIRE(model->add(geometry,0,FLT_MAX,*renderer.factory));
        auto* drawable = new ChonkDrawable();
        drawable->add(model);
        auto* branch = new osg::Group();
        intervals[i] = new osg::Uniform("oe_chonk_coverage",osg::Vec2f(0,1));
        branch->getOrCreateStateSet()->addUniform(intervals[i]);
        branch->addChild(drawable); scene->addChild(branch);
        tierDrawables[i] = drawable;
    }
    renderer.setScene(scene);
    for (unsigned mode=0; mode<3; ++mode)
    {
        if (mode == 1) rootState->setDefine("OE_IS_SHADOW_CAMERA");
        if (mode == 2) { rootState->removeDefine("OE_IS_SHADOW_CAMERA"); rootState->setDefine("OE_IS_DEPTH_CAMERA"); }
        for (unsigned culling=0; culling<4; ++culling)
        {
            INFO("tier culling combination=" << culling);
            tierDrawables[0]->setUseGPUCulling((culling & 1u) != 0u);
            tierDrawables[1]->setUseGPUCulling((culling & 2u) != 0u);
            for (bool opaque : {false,true})
            {
                ChonkRenderBin::setOpaquePath(opaque);
                for (float split : {0.0f,0.25f,0.5f,0.75f,1.0f})
                {
                    INFO("camera variant=" << mode << " opaque=" << opaque << " split=" << split);
                    intervals[0]->set(osg::Vec2f(0,split)); intervals[1]->set(osg::Vec2f(split,1));
                    renderer.frame(); renderer.frame();
                    auto pixels = renderer.pixels();
                    unsigned red = 0, green = 0, other = 0;
                    for (unsigned y=8; y<248; ++y)
                        for (unsigned x=8; x<248; ++x)
                        {
                            const auto* p = pixels->data(x,y);
                            if (p[0] > 128 && p[1] < 32) ++red;
                            else if (p[1] > 128 && p[0] < 32) ++green;
                            else ++other;
                        }
                    // Catches flattened per-page uniforms, missing coverage, and zero-alpha opaque draws.
                    CHECK(other == 0u);
                    CHECK(std::abs(double(red)/57600.0-split) < 0.025);
                    CHECK(red+green == 57600u);
                }
            }
        }
    }
    rootState->removeDefine("OE_IS_DEPTH_CAMERA");
    camera->setProjectionMatrixAsOrtho(-0.5,0.5,-0.5,0.5,1,20);
    AssetCatalog catalog({},4u*1024u*1024u);
    ScatterGroup art; art.asset = "canopy8-0"; art.lodPixels = art.minPixels = 0.0f;
    auto model = catalog.acquire(art,"");
    REQUIRE(model);
    for (unsigned crown=0; crown<=9; ++crown)
    {
        INFO("single crown=" << crown);
        auto* drawable = new ChonkDrawable();
        // Bit 9 is an unused sentinel, allowing an empty mask to retain the negative aggregate marker.
        drawable->add(model,osg::Matrixf(),osg::Vec2f(-float(512u | (crown < 9 ? 1u<<crown : 0u)),0));
        renderer.setScene(drawable);
        renderer.frame(); renderer.frame();
        auto pixels = renderer.pixels();
        unsigned visible = 0;
        const auto& shape = canopyTemplate(8,0)[std::min(8u,crown)];
        for (unsigned y=0; y<256; ++y)
            for (unsigned x=0; x<256; ++x)
            {
                const auto* p = pixels->data(x,y);
                if (p[0]+p[1]+p[2] < 10) continue;
                ++visible;
                const double u = (x+0.5)/256.0-0.5, v = (y+0.5)/256.0-0.5;
                CHECK(std::abs(u-shape.center.x()) <= shape.size.x()+0.008);
                CHECK(std::abs(v-shape.center.y()) <= shape.size.y()+0.008);
            }
        CHECK((crown == 9 ? visible == 0u : visible > 100u));
    }
    // Exercise real asynchronous SimplePager replacement, not just the coverage shader in isolation.
    auto profile = Profile::create(SpatialReference::get("epsg:3857"),-2,-2,2,2,1,1);
    osg::ref_ptr<Util::SimplePager> pager = new Util::SimplePager(nullptr,profile);
    pager->setMinLevel(0); pager->setMaxLevel(1); pager->setClusterCullingEnabled(false);
    pager->setTimeoutSeconds(2.0);
    Chonk::Ptr levels[2];
    for (unsigned i=0; i<2; ++i)
    {
        auto geometry = ChonkTest::mesh();
        (*dynamic_cast<osg::Vec4Array*>(geometry->getColorArray()))[0] =
            i == 0 ? osg::Vec4(1,0,0,1) : osg::Vec4(0,1,0,1);
        levels[i] = Chonk::create();
        REQUIRE(levels[i]->add(geometry,0,FLT_MAX,*renderer.factory));
    }
    pager->setCreateNodeFunction([levels](const TileKey& key,ProgressCallback*) -> osg::ref_ptr<osg::Node>
    {
        auto* drawable = new ChonkDrawable();
        const auto& e = key.getExtent();
        drawable->add(levels[key.getLOD()],osg::Matrixf::scale(e.width()/4,e.height()/4,1)*
            osg::Matrixf::translate(e.getCentroid().vec3d()));
        return drawable;
    });
    osg::observer_ptr<Util::PagedNode2> rootPage;
    ScatterGroup policy; policy.canopyFadeSeconds = 0.3f;
    auto transitions = std::make_shared<CanopyTransitionStates>();
    pager->setConfigurePagedNodeFunction([&](const TileKey&,Util::PagedNode2* page)
    {
        rootPage = page;
        page->setCenter(osg::Vec3()); page->setRadius(2.0f); page->setLODMethod(LODMethod::CAMERA_DISTANCE);
        configureCanopyTransition(page,1.0,policy,transitions);
    });
    pager->build();
    osg::ref_ptr<Util::PagingManager> manager = new Util::PagingManager("p2-transition-test");
    manager->addChild(pager);
    renderer.setScene(manager);
    camera->setProjectionMatrixAsPerspective(45.0,1.0,1.0,100.0);
    //! Moves along the view axis; a small center region stays inside the common parent/child quad at every distance.
    auto distance = [&](double meters)
    { camera->setViewMatrixAsLookAt(osg::Vec3d(0,0,meters),osg::Vec3d(),osg::Vec3d(0,1,0)); };
    //! Reads complementary colors in the common interior, excluding silhouette/MSAA pixels.
    auto centerCoverage = [&]()
    {
        const auto pixels = renderer.pixels();
        unsigned red = 0, green = 0;
        for (unsigned y=116; y<140; ++y)
            for (unsigned x=116; x<140; ++x)
            {
                const auto* p = pixels->data(x,y);
                if (p[0] > 128 && p[1] < 32) ++red;
                if (p[1] > 128 && p[0] < 32) ++green;
            }
        CHECK(red+green == 576u);
        return green;
    };
    distance(20); renderer.frame();
    CHECK(centerCoverage() == 0u);
    CHECK(rootPage->getPriority() < 0.0f); // refinement stays pixel-based without positive job priorities
    for (unsigned cycle=0; cycle<2; ++cycle)
    {
        if (cycle == 1) rootPage->unload(); // reload must restart arrival fading, even at the same camera position
        distance(8); renderer.frame();
        CHECK(centerCoverage() == 0u); // pending children leave the complete parent visible
        bool intermediate = false, complete = false;
        for (unsigned frame=0; frame<200 && !complete; ++frame)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            renderer.frame();
            const unsigned green = centerCoverage();
            intermediate = intermediate || (green > 0u && green < 576u);
            complete = green == 576u;
        }
        CHECK(intermediate);
        CHECK(complete);
    }
    // Reversing through the middle of the distance band uses already resident children without a pop or gap.
    distance(2.0+2.0*(1.0/std::tan(osg::DegreesToRadians(22.5)))*256.0/100.0);
    renderer.frame();
    const unsigned middle = centerCoverage();
    CHECK(middle > 200u); CHECK(middle < 376u);
    distance(20); renderer.frame(); CHECK(centerCoverage() == 0u);
    // Two asynchronous levels may fade at once; a shared pixel must still have exactly one owner.
    auto blueGeometry = ChonkTest::mesh();
    (*dynamic_cast<osg::Vec4Array*>(blueGeometry->getColorArray()))[0] = osg::Vec4(0,0,1,1);
    auto blueModel = Chonk::create();
    REQUIRE(blueModel->add(blueGeometry,0,FLT_MAX,*renderer.factory));
    osg::ref_ptr<Util::SimplePager> nested = new Util::SimplePager(nullptr,profile);
    nested->setMinLevel(0); nested->setMaxLevel(2); nested->setClusterCullingEnabled(false);
    nested->setTimeoutSeconds(2.0); // production residency window, so reversing quality can reuse child pages
    nested->setCreateNodeFunction([levels,blueModel](const TileKey& key,ProgressCallback*) -> osg::ref_ptr<osg::Node>
    {
        auto* drawable = new ChonkDrawable();
        const auto& e = key.getExtent();
        drawable->add(key.getLOD() < 2 ? levels[key.getLOD()] : blueModel,
            osg::Matrixf::scale(e.width()/4,e.height()/4,1)*osg::Matrixf::translate(e.getCentroid().vec3d()));
        return drawable;
    });
    nested->setConfigurePagedNodeFunction([policy,transitions](const TileKey&,Util::PagedNode2* page)
    {
        page->setCenter(osg::Vec3()); page->setRadius(2.0f);
        page->setLODMethod(LODMethod::CAMERA_DISTANCE);
        configureCanopyTransition(page,1.0,policy,transitions);
    });
    nested->build();
    manager->removeChildren(0,manager->getNumChildren()); manager->addChild(nested);
    distance(8);
    bool blueComplete = false, mixed = false;
    for (unsigned frame=0; frame<200 && !blueComplete; ++frame)
    {
        renderer.frame();
        const auto pixels = renderer.pixels();
        unsigned colors[3] = {};
        for (unsigned y=116; y<140; ++y)
            for (unsigned x=116; x<140; ++x)
            {
                const auto* p = pixels->data(x,y);
                for (unsigned c=0; c<3; ++c)
                    if (p[c] > 128 && p[(c+1)%3] < 32 && p[(c+2)%3] < 32) ++colors[c];
            }
        CHECK(colors[0]+colors[1]+colors[2] == 576u);
        mixed = mixed || (colors[2] > 0u && colors[0]+colors[1] > 0u);
        blueComplete = colors[2] == 576u;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(mixed); CHECK(blueComplete);

    // Drive the already-loaded hierarchy with global and local error, without moving the camera or reloading assets.
    osg::ref_ptr<osg::Uniform> global = rootState->getUniform("oe_sse");
    renderer.root->addCullCallback(new Util::LambdaCullCallback([global](osg::Node* node,osg::NodeVisitor* nv)
    {
        float value = 25.0f; global->get(value);
        nv->setUserValue("oe_sse",value);
        nv->traverse(*node);
    }));
    global->set(200.0f);
    renderer.frame();
    {
        auto pixels = renderer.pixels();
        CHECK(pixels->data(128,128)[0] > 128); // first parent returns when the error budget is relaxed
    }
    global->set(25.0f);
    renderer.frame();
    {
        auto pixels = renderer.pixels();
        CHECK(pixels->data(128,128)[2] > 128); // already resident detail returns immediately
    }

    // Whole-page visibility uses the same primary-view pixel budget in color, depth and shadow modes.
    auto fading = new osg::Group();
    auto pageArt = new ChonkDrawable();
    pageArt->setBirthday(-10);
    pageArt->add(levels[1],osg::Matrixf());
    fading->addChild(pageArt);
    osg::ref_ptr<osg::Uniform> quality = new osg::Uniform("oe_chonk_sse_adjust",osg::Vec2f(0,1.0f/25.0f));
    installPopulationPageFade(fading,quality,transitions);
    renderer.setScene(fading);
    distance(20);
    for (bool gpuCull : {true,false})
    {
        INFO("page GPU culling=" << gpuCull);
        pageArt->setUseGPUCulling(gpuCull);
        for (unsigned pass=0; pass<3; ++pass)
        {
            rootState->removeDefine("OE_IS_DEPTH_CAMERA");
            rootState->removeDefine("OE_IS_SHADOW_CAMERA");
            camera->getOrCreateStateSet()->removeDefine("OE_IS_SHADOW_CAMERA");
            if (pass == 1) rootState->setDefine("OE_IS_DEPTH_CAMERA");
            if (pass == 2)
            {
                rootState->setDefine("OE_IS_SHADOW_CAMERA");
                auto* ss = camera->getOrCreateStateSet();
                ss->setDefine("OE_IS_SHADOW_CAMERA");
                ss->addUniform(new osg::Uniform("oe_shadowToPrimaryMatrix",osg::Matrixf()));
                ss->addUniform(new osg::Uniform("oe_primaryProjectionMatrix",osg::Matrixf(camera->getProjectionMatrix())));
                ss->addUniform(new osg::Uniform("oe_primaryViewport",osg::Vec2f(256,256)));
                ss->addUniform(new osg::Uniform("oe_primaryLODScale",1.0f));
            }
            std::vector<unsigned> counts;
            for (const auto& values : {osg::Vec2f(25,0),osg::Vec2f(100,0),osg::Vec2f(25,75),
                osg::Vec2f(200,0),osg::Vec2f(25,0)})
            {
                global->set(values.x()); quality->set(osg::Vec2f(values.y(),1.0f/25.0f));
                renderer.frame(); renderer.frame();
                auto pixels = renderer.pixels();
                unsigned count = 0;
                for (unsigned y=0; y<256; ++y)
                    for (unsigned x=0; x<256; ++x)
                        if (pixels->data(x,y)[1] > 128) ++count;
                counts.push_back(count);
            }
            INFO("page fade pass=" << pass);
            CHECK(counts[0] > 100u);
            CHECK(counts[1] > 0u); CHECK(counts[1] < counts[0]);
            CHECK(counts[2] == counts[1]);
            CHECK(counts[3] == 0u); CHECK(counts[4] == counts[0]);
        }
    }
    CHECK(glGetError() == GL_NO_ERROR);
}
