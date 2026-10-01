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
#include <osg/Multisample>

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

//! New art must fit the existing budget, retain cutouts/PBR bindings, and load only precompressed mip chains.
TEST_CASE("Procedural2 textured art stays low poly and carries complete PBR materials", "[procedural2][pbr-art]")
{
    const char* names[] = {"broadleaf", "conifer", "shrub", "grass-tuft", "fern", "boulder", "canopy-cluster"};
    const unsigned limits[] = {1000, 1000, 512, 16, 64, 156, 78};
    std::vector<ScatterAsset> definitions;
    for (unsigned i = 0; i < 7; ++i)
    {
        Config conf("asset");
        conf.setReferrer(osgDB::getRealPath("procedural2-art.earth"));
        const std::string base = "../data/procedural2/pbr/" + std::string(names[i]);
        conf.set("name", names[i]); conf.set("near", base+"-near.osg"); conf.set("coarse", base+"-coarse.osg");
        if (i < 2) conf.set("canopy", base+"-canopy.osg");
        definitions.emplace_back(conf);
    }
    AssetCatalog catalog(definitions, 64u*1024u*1024u);
    ScatterGroup group;
    std::vector<Chonk::Ptr> retained;
    for (unsigned i = 0; i < 7; ++i)
    {
        INFO(names[i]);
        auto model = catalog.acquire(group,names[i]);
        REQUIRE(model);
        retained.push_back(model);
        REQUIRE(model->_lods.size() == 2);
        CHECK(model->_lods[0].length/3 <= limits[i]);
        CHECK(model->_lods[1].length == 18);
        CHECK(model->_lods[0].alphaTested == (i != 5));
        CHECK(model->_lods[1].alphaTested);
        for (const auto& material : model->_materials)
        {
            for (unsigned slot = 0; slot < 3; ++slot)
            {
                REQUIRE(material->textures[slot] >= 0);
                auto texture = catalog.textures()->find(material->textures[slot]);
                REQUIRE(texture);
                auto image = texture->osgTexture()->getImage(0);
                REQUIRE(image);
                REQUIRE(image->isCompressed());
                unsigned levels = 1;
                for (int dim = std::max(image->s(),image->t()); dim > 1; dim /= 2) ++levels;
                CHECK(image->getNumMipmapLevels() == levels);
            }
        }
        for (const auto& vertex : model->_vbo_store)
        {
            CHECK(std::isfinite(vertex.position.length2()));
            CHECK(std::abs(vertex.normal.length()-1.0f) < 0.002f);
        }
    }
    group.canopy = true; group.models = {"broadleaf", "conifer"};
    for (unsigned layout = 0; layout < 4; ++layout)
    {
        auto canopy = catalog.acquireCanopy(group,8,layout);
        REQUIRE(canopy);
        CHECK(canopy->_lods.front().length == 9u*6u*3u);
        retained.push_back(canopy);
    }
    CHECK(catalog.residency().failedLoads == 0);
    CHECK(catalog.residency().budgetDenials == 0);
    CHECK(catalog.residency().bytes <= catalog.residency().budget);
}

//! Canopy assembly must rotate baked card frames without replacing them with upward crown-volume normals.
TEST_CASE("Procedural2 canopy retains baked normal atlas semantics", "[procedural2][pbr-art]")
{
    auto geometry = ChonkTest::mesh();
    auto* vertices = dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray());
    for (auto& v : *vertices) v.set(v.x(),0,v.y()+2.0f);
    (*dynamic_cast<osg::Vec3Array*>(geometry->getNormalArray()))[0].set(0,-1,0);
    auto technique = new osg::UByteArray();
    technique->push_back(Chonk::NORMAL_TECHNIQUE_BAKED);
    geometry->setVertexAttribArray(6,technique,osg::Array::BIND_OVERALL);
    ChonkTest::AssetFile file;
    REQUIRE(file.write(geometry));
    Config config("asset");
    config.set("name","baked"); config.set("near",file.path); config.set("coarse",file.path);
    config.set("canopy",file.path);
    AssetCatalog catalog({ScatterAsset(config)},1024u*1024u);
    ScatterGroup group;
    group.models = {"baked"};
    for (unsigned layout=0; layout<4; ++layout)
    {
        auto canopy = catalog.acquireCanopy(group,8,layout);
        REQUIRE(canopy);
        CHECK(canopy->_ebo_store.size() == 9u*6u);
        for (const auto& v : canopy->_vbo_store)
        {
            CHECK(v.normal_technique == Chonk::NORMAL_TECHNIQUE_BAKED);
            CHECK(std::abs(v.normal.z()) < 1e-6f);
            CHECK(std::abs(v.normal.length()-1.0f) < 1e-6f);
        }
    }
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

//! Reads actual A2C ramps and shadow cutouts: same-bin weights must remain independent and masked crowns must vanish.
TEST_CASE("Procedural2 canopy GPU coverage and crown masks", "[.procedural2-canopy-gpu-worker]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Canopy validation requires NVGL"); return; }
    osg::DisplaySettings::instance()->setNumMultiSamples(4);
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    auto* rootState = renderer.root->getOrCreateStateSet();
    installCanopyShader(rootState);
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES_ARB, &samples);
    REQUIRE(samples == 4);
    rootState->setMode(GL_CULL_FACE,osg::StateAttribute::OFF);
    rootState->setDefine("OE_CHONK_ALPHA_TO_COVERAGE");
    rootState->setMode(GL_MULTISAMPLE_ARB,osg::StateAttribute::ON);
    rootState->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB,osg::StateAttribute::ON);
    rootState->setMode(GL_BLEND,osg::StateAttribute::OFF);
    auto* camera = renderer.viewer.getCamera();
    camera->setProjectionMatrixAsOrtho(-2,2,-2,2,1,20);
    camera->setViewMatrixAsLookAt(osg::Vec3d(0,0,8),osg::Vec3d(),osg::Vec3d(0,1,0));
    osg::ref_ptr<osg::Group> scene = new osg::Group();
    osg::ref_ptr<osg::Uniform> intervals[2];
    osg::ref_ptr<ChonkDrawable> tierDrawables[2];
    // Minification deliberately boosts material alpha above one; it must not cancel the subsequent fade.
    osg::ref_ptr<osg::Image> opaqueImage = new osg::Image();
    opaqueImage->allocateImage(512,512,1,GL_RGBA,GL_UNSIGNED_BYTE);
    std::fill(opaqueImage->data(),opaqueImage->data()+opaqueImage->getTotalSizeInBytes(),255);
    osg::ref_ptr<osg::Texture2D> opaqueTexture = new osg::Texture2D(opaqueImage);
    opaqueTexture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);
    for (unsigned i=0; i<2; ++i)
    {
        auto geometry = ChonkTest::mesh();
        auto* colors = dynamic_cast<osg::Vec4Array*>(geometry->getColorArray());
        (*colors)[0] = i == 0 ? osg::Vec4(1,0,0,1) : osg::Vec4(0,1,0,1);
        geometry->getOrCreateStateSet()->setTextureAttribute(0,opaqueTexture);
        auto model = Chonk::create();
        REQUIRE(model->add(geometry,0,FLT_MAX,*renderer.factory));
        auto* drawable = new ChonkDrawable();
        // Separate screen halves expose each branch's weight without overlapping A2C sample masks.
        drawable->add(model,osg::Matrixf::scale(0.5f,1,1)*osg::Matrixf::translate(i == 0 ? -1.0f : 1.0f,0,0));
        drawable->setBirthday(-10);
        drawable->setAlphaCutoff(1.0f);
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
                    for (unsigned tier=0; tier<2; ++tier)
                        for (unsigned y=32; y<224; y+=16)
                            for (unsigned x=16; x<112; x+=16)
                            {
                                const float weight = tier == 0 ? split : 1.0f-split;
                                const float alpha = mode == 0 ? weight : float(weight >= 0.5f);
                                const int linear = int(std::round(255.0f*alpha));
                                const int srgb = int(std::round(255.0f*(alpha <= 0.0031308f ? 12.92f*alpha :
                                    1.055f*std::pow(alpha,1.0f/2.4f)-0.055f)));
                                const auto* p = pixels->data(x+128*tier,y);
                                // WGL can resolve linear or sRGB. Both must ramp every pixel, without binary noise.
                                CHECK(std::min(std::abs(int(p[tier])-linear),std::abs(int(p[tier])-srgb)) <= 2);
                                CHECK(p[1-tier] == 0);
                                CHECK(p[2] == 0);
                            }
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
            osg::Matrixf::translate(e.getCentroid().vec3d()+osg::Vec3d(0,0,0.01*key.getLOD())));
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
    //! Reads resolved child opacity in the common interior, excluding silhouette/MSAA edge pixels.
    auto centerCoverage = [&]()
    {
        const auto pixels = renderer.pixels();
        unsigned green = 0;
        for (unsigned y=116; y<140; ++y)
            for (unsigned x=116; x<140; ++x)
            {
                const auto* p = pixels->data(x,y);
                CHECK(unsigned(p[0])+p[1] > 30u);
                green += p[1];
            }
        return green/576u;
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
            intermediate = intermediate || (green > 0u && green < 255u);
            complete = green == 255u;
        }
        CHECK(intermediate);
        CHECK(complete);
    }
    // Reverse through the resident handover: incoming coverage ramps up before outgoing coverage ramps down.
    distance(2.0+2.0*(1.0/std::tan(osg::DegreesToRadians(22.5)))*256.0/87.5);
    renderer.frame();
    const unsigned entering = centerCoverage();
    CHECK(entering > 0u); CHECK(entering < 255u);
    distance(2.0+2.0*(1.0/std::tan(osg::DegreesToRadians(22.5)))*256.0/100.0);
    renderer.frame();
    CHECK(centerCoverage() == 255u); // The nearer child is fully covered at the midpoint, not half transparent.
    distance(20); renderer.frame(); CHECK(centerCoverage() == 0u);
    // Two asynchronous levels may ramp at once; verify intermediate resolved colors and eventual full detail.
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
            osg::Matrixf::scale(e.width()/4,e.height()/4,1)*
            osg::Matrixf::translate(e.getCentroid().vec3d()+osg::Vec3d(0,0,0.01*key.getLOD())));
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
                CHECK(unsigned(p[0])+p[1]+p[2] > 30u);
                for (unsigned c=0; c<3; ++c) colors[c] += p[c];
            }
        mixed = mixed || (colors[2] > 0u && colors[0]+colors[1] > 0u);
        blueComplete = colors[2] == 576u*255u;
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
                        count += pixels->data(x,y)[1];
                counts.push_back(count);
            }
            INFO("page fade pass=" << pass);
            CHECK(counts[0] > 100u);
            if (pass == 0) { CHECK(counts[1] > 0u); CHECK(counts[1] < counts[0]); }
            else CHECK((counts[1] == 0u || counts[1] == counts[0]));
            CHECK(counts[2] == counts[1]);
            CHECK(counts[3] == 0u); CHECK(counts[4] == counts[0]);
        }
    }
    CHECK(glGetError() == GL_NO_ERROR);
}


//! Stand art remains at its authored dimensions; the two template caches must never substitute one for the other.
TEST_CASE("Procedural2 reusable stand templates keep authored geometry and metadata", "[procedural2][coverage-stands]")
{
    Config conf("asset"); conf.setReferrer(osgDB::getRealPath("procedural2.earth"));
    conf.set("name","broadleaf");
    conf.set("near","../data/procedural2/pbr/broadleaf-near.osg");
    conf.set("coarse","../data/procedural2/pbr/broadleaf-coarse.osg");
    conf.set("canopy","../data/procedural2/pbr/broadleaf-canopy.osg");
    conf.set("canopy_trees",5u);
    ScatterAsset asset(conf);
    CHECK(ScatterAsset(asset.getConfig()).canopyTrees == 5u);
    AssetCatalog catalog({asset},64u*1024u*1024u);
    ScatterGroup group; group.models = {"broadleaf"};
    auto tree = catalog.acquireImpostor(group,"broadleaf");
    auto stand = catalog.acquireStand(group,"broadleaf");
    REQUIRE(tree); REQUIRE(stand);
    CHECK(tree != stand);
    CHECK(stand->_ebo_store.size() == 18u);
    CHECK(stand->_box.xMax()-stand->_box.xMin() > tree->_box.xMax()-tree->_box.xMin());
    auto trees = catalog.acquireTreeCards(group,"broadleaf",8);
    auto stands = catalog.acquireTreeCards(group,"broadleaf",8,nullptr,true);
    REQUIRE(trees); REQUIRE(stands); CHECK(trees != stands);
    CHECK(stands->_ebo_store.size() == 8u*stand->_ebo_store.size());
    CHECK(catalog.acquireTreeCards(group,"broadleaf",8,nullptr,true) == stands);
}
