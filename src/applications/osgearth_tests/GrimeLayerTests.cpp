/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/GrimeLayer>
#include <osgEarth/MapNode>
#include <osgEarth/XYZModelLayer>
#include <osgEarth/Shaders>
#include "ChonkTestUtils.h"
#include <cstring>
#include <limits>

using namespace osgEarth;
namespace osgEarth { namespace Tests { extern std::string executablePath; } }

namespace
{
    // A real TiledModelLayer lifecycle with a manually populated, nonpaging subtree.
    struct Buildings : XYZModelLayer
    {
        //! Configure a local fixture without any filesystem or network reads.
        Buildings()
        {
            setName("buildings");
            setProfile(Profile::create(Profile::GLOBAL_GEODETIC));
            options().nvgl() = false;
        }
        //! Leave the root available for deterministic test geometry instead of paging.
        void create() override { }
    };

    // Preserve an unrelated callback through grime installation/removal.
    struct ExistingCallback : osg::NodeCallback
    {
        //! Forward traversal exactly as an application's existing callback would.
        void operator()(osg::Node* node, osg::NodeVisitor* nv) override { traverse(node, nv); }
    };

    //! Return small, deterministic test options anchored in Boston.
    GrimeLayer::Options settings(unsigned seed = 17)
    {
        GrimeLayer::Options options;
        options.model() = "buildings";
        options.anchor() = osg::Vec3d(-71.06, 42.36, 0.0);
        options.volumeSize() = 16u;
        options.seed() = seed;
        options.amount() = 0.9f;
        options.macroPeriod() = 37.0;
        options.streakPeriod() = osg::Vec3d(9.0, 9.0, 43.0);
        return options;
    }

    //! Count changed RGB channels without letting alpha or the clear color mask failures.
    unsigned differences(const osg::Image* a, const osg::Image* b, unsigned tolerance)
    {
        unsigned result = 0;
        for (unsigned i = 0; i < 256u*256u*4u; ++i)
            if (i%4 != 3 && unsigned(std::abs(int(a->data()[i])-int(b->data()[i]))) > tolerance)
                ++result;
        return result;
    }
}

// Verify reproducibility, seed separation, full 3D mip filtering, and nonconstant data.
TEST_CASE("Grime generates deterministic volumes and complete 3D mipmaps", "[grime]")
{
    osg::ref_ptr<GrimeLayer> a = new GrimeLayer(settings());
    osg::ref_ptr<GrimeLayer> b = new GrimeLayer(settings());
    osg::ref_ptr<GrimeLayer> c = new GrimeLayer(settings(1234));
    REQUIRE(a->open().isOK()); REQUIRE(b->open().isOK()); REQUIRE(c->open().isOK());
    auto image = a->getNoiseImage();
    REQUIRE(image);
    REQUIRE(image->s() == 16); REQUIRE(image->t() == 16); REQUIRE(image->r() == 16);
    REQUIRE(image->getNumMipmapLevels() == 5);
    const auto bytes = image->getTotalSizeInBytesIncludingMipmaps();
    REQUIRE(bytes == 2u*(4096u+512u+64u+8u+1u));
    REQUIRE(std::memcmp(image->data(), b->getNoiseImage()->data(), bytes) == 0);
    REQUIRE(std::memcmp(image->data(), c->getNoiseImage()->data(), bytes) != 0);
    auto range = std::minmax_element(image->data(), image->data()+image->getTotalSizeInBytes());
    REQUIRE(int(*range.second)-int(*range.first) > 100);
    unsigned previousSize = 16;
    for (unsigned level = 1; level < image->getNumMipmapLevels(); ++level)
    {
        const unsigned dim = previousSize/2;
        const auto* previous = image->getMipmapData(level-1);
        const auto* current = image->getMipmapData(level);
        for (unsigned z = 0; z < dim; ++z)
        for (unsigned y = 0; y < dim; ++y)
        for (unsigned x = 0; x < dim; ++x)
        for (unsigned channel = 0; channel < 2; ++channel)
        {
            unsigned sum = 0;
            for (unsigned dz = 0; dz < 2; ++dz)
            for (unsigned dy = 0; dy < 2; ++dy)
            for (unsigned dx = 0; dx < 2; ++dx)
                sum += previous[(((2*z+dz)*previousSize+2*y+dy)*previousSize+2*x+dx)*2+channel];
            REQUIRE(current[((z*dim+y)*dim+x)*2+channel] == (sum+4)/8);
        }
        previousSize = dim;
    }
    a->close();
    REQUIRE(a->getNoiseImage() == nullptr);
}

// Ensure serialized layers instantiate through the registry and reject dangerous sizes/scales.
TEST_CASE("Grime configuration round trips and rejects invalid parameters", "[grime]")
{
    osg::ref_ptr<GrimeLayer> original = new GrimeLayer(settings());
    auto config = original->getConfig();
    auto copy = Layer::create_as<GrimeLayer>(config);
    REQUIRE(copy);
    REQUIRE(copy->open().isOK());
    REQUIRE(copy->options().anchor().get() == original->options().anchor().get());
    REQUIRE(copy->options().streakPeriod().get() == original->options().streakPeriod().get());
    REQUIRE(copy->getAmount() == Approx(0.9));
    copy->setAmount(2.0f);
    REQUIRE(copy->getAmount() == 1.0f);
    copy->setAmount(std::numeric_limits<float>::quiet_NaN());
    REQUIRE(copy->getAmount() == 1.0f);
    for (unsigned size : {0u, 15u, 17u, 256u})
    {
        auto options = settings(); options.volumeSize() = size;
        osg::ref_ptr<GrimeLayer> invalid = new GrimeLayer(options);
        REQUIRE(invalid->open().isError()); REQUIRE(invalid->getNoiseImage() == nullptr);
    }
    auto options = settings(); options.streakPeriod() = osg::Vec3d(1, 0, 1);
    osg::ref_ptr<GrimeLayer> invalid = new GrimeLayer(options);
    REQUIRE(invalid->open().isError());
}

// Exercise insertion order, closure/reopening, removal/readdition, and callback ownership.
TEST_CASE("Grime follows the target model layer lifecycle", "[grime]")
{
    osg::ref_ptr<Map> map = new Map;
    osg::ref_ptr<MapNode> mapNode = new MapNode(map);
    osg::ref_ptr<GrimeLayer> grime = new GrimeLayer(settings());
    map->addLayer(grime);
    REQUIRE(mapNode->open());
    osg::ref_ptr<Buildings> buildings = new Buildings;
    osg::ref_ptr<ExistingCallback> existing = new ExistingCallback;
    buildings->getNode()->addCullCallback(existing);
    map->addLayer(buildings);
    REQUIRE(grime->getModelLayer() == buildings.get());
    REQUIRE(existing->getNestedCallback() != nullptr);
    grime->close();
    REQUIRE(buildings->getNode()->getCullCallback() == existing.get());
    REQUIRE(existing->getNestedCallback() == nullptr);
    REQUIRE(grime->open().isOK());
    REQUIRE(existing->getNestedCallback() != nullptr);
    buildings->close();
    REQUIRE(existing->getNestedCallback() == nullptr);
    REQUIRE(buildings->open().isOK());
    REQUIRE(existing->getNestedCallback() != nullptr);
    map->removeLayer(buildings);
    REQUIRE(existing->getNestedCallback() == nullptr);
    map->addLayer(buildings);
    REQUIRE(existing->getNestedCallback() != nullptr);
    map->removeLayer(grime);
    REQUIRE(existing->getNestedCallback() == nullptr);
}

// Isolate viewer/context resources from other GPU fixtures in the full suite.
TEST_CASE("Grime renders stable geographic weathering", "[grime][gpu]")
{
    const auto& executable = osgEarth::Tests::executablePath;
#ifdef _WIN32
    const std::string command = "\"\"" + executable + "\" \"[.grime-render-worker]\"\"";
#else
    std::string quoted = "'";
    for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    const std::string command = quoted + "' '[.grime-render-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
}

// Render the actual inherited effect at ECEF scale, then compare camera movement,
// new tessellation, Chonk instancing, excluded passes, and complete removal.
TEST_CASE("Grime GPU validation", "[.grime-render-worker]")
{
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    osg::ref_ptr<Map> map = new Map;
    osg::ref_ptr<MapNode> mapNode = new MapNode(map);
    osg::ref_ptr<Buildings> buildings = new Buildings;
    osg::ref_ptr<GrimeLayer> grime = new GrimeLayer(settings());
    map->addLayer(buildings); map->addLayer(grime);
    REQUIRE(mapNode->open());
    REQUIRE(buildings->getNode()->getCullCallback() != nullptr);
    osg::Matrixd localToWorld;
    REQUIRE(GeoPoint(SpatialReference::get("wgs84"), settings().anchor().get(), ALTMODE_ABSOLUTE).createLocalToWorld(localToWorld));
    osg::ref_ptr<osg::MatrixTransform> placement = new osg::MatrixTransform(
        osg::Matrixd::scale(20,20,20)*localToWorld);
    auto geometry = ChonkTest::mesh(8);
    ShaderGenerator().run(geometry);
    placement->addChild(geometry);
    buildings->getNode()->asGroup()->addChild(placement);
    renderer.setScene(buildings->getNode());
    auto camera = renderer.viewer.getCamera();
    camera->setProjectionMatrixAsOrtho(-45,45,-45,45,1,1000);
    camera->setViewMatrix(osg::Matrixd::inverse(localToWorld)*osg::Matrixd::translate(0,0,-100));
    grime->setAmount(0);
    renderer.frame(); renderer.frame();
    auto clean = renderer.pixels();
    REQUIRE(clean->getColor(128,128).b() > 0.9f);
    grime->setAmount(0.9f);
    renderer.frame(); renderer.frame();
    auto dirty = renderer.pixels();
    REQUIRE(differences(dirty, clean, 3) > 10000);
    for (unsigned i = 3; i < 256u*256u*4u; i += 4)
        REQUIRE(dirty->data()[i] == clean->data()[i]);
    REQUIRE(glGetError() == GL_NO_ERROR);

    camera->setViewMatrix(osg::Matrixd::inverse(localToWorld)*osg::Matrixd::translate(0,0,-137));
    renderer.frame(); renderer.frame();
    REQUIRE(differences(renderer.pixels(), dirty, 1) < 100);

    auto fine = ChonkTest::mesh(32);
    ShaderGenerator().run(fine);
    placement->removeChildren(0, placement->getNumChildren());
    placement->addChild(fine);
    renderer.frame(); renderer.frame();
    REQUIRE(differences(renderer.pixels(), dirty, 1) < 100);

    if (Capabilities::get().supportsNVGL())
    {
        osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable;
        drawable->setUseGPUCulling(false);
        drawable->add(ChonkTest::mesh(8), *renderer.factory);
        placement->removeChildren(0, placement->getNumChildren());
        placement->addChild(drawable);
        renderer.frame(); renderer.frame();
        REQUIRE(differences(renderer.pixels(), dirty, 2) < 100);
    }
    else WARN("NVGL unavailable; tested ordinary geometry only");

    // Conventional geometry can opt a material out without changing alpha.
    placement->removeChildren(0, placement->getNumChildren());
    placement->addChild(fine);
    fine->getOrCreateStateSet()->addUniform(new osg::Uniform("oe_grime_acceptance", 0.0f));
    renderer.frame(); renderer.frame();
    REQUIRE(differences(renderer.pixels(), clean, 1) < 100);
    fine->getStateSet()->removeUniform("oe_grime_acceptance");
    for (const char* define : {"OE_IS_SHADOW_CAMERA", "OE_IS_DEPTH_CAMERA", "OE_IS_PICK_CAMERA"})
    {
        renderer.root->getOrCreateStateSet()->setDefine(define);
        renderer.frame(); renderer.frame();
        REQUIRE(differences(renderer.pixels(), clean, 1) < 100);
        renderer.root->getOrCreateStateSet()->removeDefine(define);
    }
    map->removeLayer(grime);
    renderer.frame(); renderer.frame();
    REQUIRE(differences(renderer.pixels(), clean, 1) < 100);
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Optional integration check run from tests: load the delivered earth file and
// render an actual Boston building tile using only local/cached resources.
TEST_CASE("Boston grime demonstration renders local buildings", "[.grime-boston]")
{
    Registry::instance()->setOverrideCachePolicy(CachePolicy(CachePolicy::USAGE_CACHE_ONLY));
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    auto scene = osgDB::readRefNodeFile("boston-grime.earth");
    REQUIRE(scene);
    auto mapNode = MapNode::get(scene);
    REQUIRE(mapNode);
    REQUIRE(mapNode->open());
    auto grime = mapNode->getMap()->getLayerByName<GrimeLayer>("Building weathering");
    auto buildings = mapNode->getMap()->getLayerByName<TiledModelLayer>("Buildings");
    REQUIRE(grime); REQUIRE(grime->isOpen());
    REQUIRE(buildings); REQUIRE(buildings->isOpen());
    REQUIRE(grime->getModelLayer() == buildings);
    GeoPoint center(SpatialReference::get("wgs84"), -71.064521, 42.35727, 0, ALTMODE_ABSOLUTE);
    auto key = buildings->getProfile()->createTileKey(center, buildings->getMinLevel());
    REQUIRE(key.valid());
    auto tile = buildings->createTile(key, nullptr);
    REQUIRE(tile);
    REQUIRE(tile->getBound().valid());
    auto root = buildings->getNode()->asGroup();
    root->removeChildren(0, root->getNumChildren());
    root->addChild(tile);
    renderer.setScene(root);
    osg::Matrixd localToWorld;
    REQUIRE(center.createLocalToWorld(localToWorld));
    renderer.viewer.getCamera()->setProjectionMatrixAsPerspective(45, 1, 1, 2000);
    renderer.viewer.getCamera()->setViewMatrix(osg::Matrixd::inverse(localToWorld) *
        osg::Matrixd::lookAt(osg::Vec3d(0,-180,90), osg::Vec3d(0,0,25), osg::Vec3d(0,0,1)));
    grime->setAmount(0);
    renderer.frame(); renderer.frame();
    auto clean = renderer.pixels();
    grime->setAmount(0.8f);
    renderer.frame(); renderer.frame();
    auto dirty = renderer.pixels();
    REQUIRE(differences(dirty, clean, 2) > 1000);
    REQUIRE(osgDB::writeImageFile(*clean, "grime-boston-clean.png"));
    REQUIRE(osgDB::writeImageFile(*dirty, "grime-boston-weathered.png"));
    REQUIRE(glGetError() == GL_NO_ERROR);
}
