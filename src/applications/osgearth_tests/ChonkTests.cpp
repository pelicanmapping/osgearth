/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include "ChonkTestUtils.h"
#include <osg/Texture2D>
#include <future>
#include <set>

using namespace osgEarth;

namespace
{
    struct InspectableDrawable : ChonkDrawable
    {
        float radius() const { return _batches.begin()->second.front().radius; }
        osg::Matrixf matrix() const { return _batches.begin()->second.front().xform; }
    };
}

TEST_CASE("Chonk caches immutable geometry and preserves nonuniform normals", "[chonk]")
{
    auto geometry = ChonkTest::mesh();
    auto normal = new osg::Vec3Array();
    normal->push_back(osg::Vec3(1,1,1));
    geometry->setNormalArray(normal, osg::Array::BIND_OVERALL);
    osg::ref_ptr<osg::MatrixTransform> source = new osg::MatrixTransform(osg::Matrix::scale(2,3,4));
    source->addChild(geometry);
    auto vertices = geometry->getVertexArray();
    auto primitives = geometry->getPrimitiveSet(0);
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    ChonkFactory factory(arena);
    auto chonk = factory.getOrCreateChonk(source);
    REQUIRE(chonk);
    REQUIRE(factory.getOrCreateChonk(source) == chonk);
    REQUIRE(geometry->getVertexArray() == vertices);
    REQUIRE(geometry->getPrimitiveSet(0) == primitives);
    REQUIRE(chonk->_vbo_store.size() == 4);
    osg::Vec3 expected(0.5f, 1.0f/3.0f, 0.25f);
    expected.normalize();
    REQUIRE((chonk->_vbo_store.front().normal - expected).length() < 1e-6f);
    REQUIRE(chonk->_vbo_store.front().position == osg::Vec3(-4,-6,0));

    std::vector<std::future<Chonk::Ptr>> futures;
    for (unsigned i = 0; i < 8; ++i)
        futures.emplace_back(std::async(std::launch::async, [&] { return factory.getOrCreateChonk(source); }));
    for (auto& future : futures) REQUIRE(future.get() == chonk);
}

TEST_CASE("Chonk radius contains rotated scaled and sheared instances", "[chonk]")
{
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    ChonkFactory factory(arena);
    auto chonk = factory.getOrCreateChonk(ChonkTest::mesh());
    REQUIRE(chonk);
    osg::Matrixf shear;
    shear(0,1) = 2.0f;
    for (const auto& matrix : {
        osg::Matrixf::scale(2,5,3) * osg::Matrixf::rotate(2.0, osg::Vec3(1,2,3)),
        osg::Matrixf::scale(-2,3,4) * osg::Matrixf::rotate(1.1, osg::Vec3(0,1,0)), shear})
    {
        osg::ref_ptr<InspectableDrawable> drawable = new InspectableDrawable();
        drawable->add(chonk, matrix);
        for (unsigned i = 0; i < 8; ++i)
        {
            const auto point = chonk->getBound().corner(i) * matrix;
            const auto center = chonk->getBound().center() * matrix;
            REQUIRE((point - center).length() <= drawable->radius() + 1e-5f);
        }
    }
}

TEST_CASE("Chonk retains glTF material factors and map conventions", "[chonk]")
{
    auto geometry = ChonkTest::mesh();
    auto* ss = geometry->getOrCreateStateSet();
    ss->addUniform(new osg::Uniform("oe_gltf_pbr_flags", osg::Vec4(1,1,1,1)));
    ss->addUniform(new osg::Uniform("oe_gltf_pbr_factors", osg::Vec4(.7f,.8f,.3f,.65f)));
    osg::ref_ptr<osg::Image> image = new osg::Image();
    image->allocateImage(1,1,1,GL_RGBA,GL_UNSIGNED_BYTE);
    std::fill(image->data(), image->data()+4, 128);
    for (unsigned unit = 0; unit < 4; ++unit)
    {
        auto texture = new osg::Texture2D(image);
        texture->setInternalFormat(unit == 0 ? GL_SRGB8_ALPHA8 : GL_RGBA8);
        ss->setTextureAttribute(unit, texture);
    }
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    arena->setAutoRelease(true);
    ChonkFactory factory(arena);
    auto chonk = factory.getOrCreateChonk(geometry);
    REQUIRE(chonk);
    const auto& v = chonk->_vbo_store.front();
    REQUIRE(v.gltf_material == 3);
    REQUIRE(v.pbr_factors == osg::Vec4(.7f,.8f,.3f,.65f));
    REQUIRE(v.occlusion_index >= 0);
    REQUIRE(v.albedo_index != v.pbr_index); // same image, different color space
    REQUIRE(chonk->_materials.front()->occlusion_tex);
}

TEST_CASE("Chonk cell conversion preserves placements and asset reloads", "[chonk]")
{
    ChonkTest::AssetFile file;
    REQUIRE(file.write(ChonkTest::mesh()));
    InstancedExternalNode::MatrixList matrices = {
        osg::Matrixf::translate(-10,0,0),
        osg::Matrixf::scale(2,3,1) * osg::Matrixf::rotate(.8, osg::Vec3(0,0,1)) * osg::Matrixf::translate(10,0,0)};
    osg::ref_ptr<InstancedExternalNode> external = new InstancedExternalNode(file.path, matrices);
    INFO(external->getLastError());
    INFO(external->getInstancingError());
    REQUIRE(external->isUsingHardwareInstancing());
    osg::ref_ptr<osg::MatrixTransform> source = new osg::MatrixTransform(osg::Matrix::translate(2,3,4));
    source->addChild(external);
    auto embedded = ChonkTest::mesh();
    source->addChild(embedded);
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    auto factory = std::make_shared<ChonkFactory>(arena);
    auto result = ChonkFactory::convertExternalInstances(source, factory);
    ChonkTest::FindDrawables find;
    result->accept(find);
    REQUIRE(find.drawables.size() == 1);
    REQUIRE(find.drawables[0]->getNumInstances() == 2);
    REQUIRE(find.drawables[0]->getNumBatches() == 1);
    const auto before = find.drawables[0]->getBoundingBox();
    REQUIRE(before.xMin() == Approx(-10.0f));
    REQUIRE(source->getNumChildren() == 2); // borrowed graph remains intact

    REQUIRE(ExternalAssetManager::instance().unload(file.path) == 1);
    ChonkTest::update(result);
    ChonkTest::FindDrawables unloaded;
    result->accept(unloaded);
    REQUIRE(unloaded.drawables.empty());

    auto larger = ChonkTest::mesh();
    auto* vertices = static_cast<osg::Vec3Array*>(larger->getVertexArray());
    for (auto& v : *vertices) v *= 2.0f;
    REQUIRE(file.write(larger));
    REQUIRE(ExternalAssetManager::instance().reload(file.path) == 1);
    ChonkTest::update(result);
    ChonkTest::FindDrawables reloaded;
    result->accept(reloaded);
    REQUIRE(reloaded.drawables.size() == 1);
    REQUIRE(reloaded.drawables[0]->getNumInstances() == 2);
    REQUIRE(reloaded.drawables[0]->getBoundingBox().xMin() < before.xMin());

    matrices.push_back(osg::Matrixf::translate(20,0,0));
    external->setMatrices(matrices);
    ChonkTest::update(result);
    ChonkTest::FindDrawables edited;
    result->accept(edited);
    REQUIRE(edited.drawables.size() == 1);
    REQUIRE(edited.drawables[0]->getNumInstances() == 3);

    // Unsupported transparency stays on the ordinary path after a reload.
    larger->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
    REQUIRE(file.write(larger));
    REQUIRE(ExternalAssetManager::instance().reload(file.path) == 1);
    ChonkTest::update(result);
    ChonkTest::FindDrawables fallback;
    result->accept(fallback);
    REQUIRE(fallback.drawables.empty());
    REQUIRE(result->getBound().valid());
}

TEST_CASE("Chonk GPU rendering agrees with attribute instancing", "[chonk][gpu]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Chonk GPU test needs NVGL and OSG_GL_CONTEXT_VERSION=4.6");
        return;
    }
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    ChonkTest::AssetFile file;
    REQUIRE(file.write(ChonkTest::mesh(4)));
    InstancedExternalNode::MatrixList matrices;
    for (int y = -4; y <= 4; ++y)
        for (int x = -4; x <= 4; ++x)
            matrices.emplace_back(osg::Matrixf::scale(1.2f, .6f, 1) *
                osg::Matrixf::rotate(.9, osg::Vec3(0,0,1)) * osg::Matrixf::translate(x*12.0f,y*12.0f,0));
    osg::ref_ptr<InstancedExternalNode> source = new InstancedExternalNode(file.path, matrices);
    INFO(source->getLastError());
    INFO(source->getInstancingError());
    REQUIRE(source->isUsingHardwareInstancing());
    auto converted = ChonkFactory::convertExternalInstances(source, renderer.factory);
    ChonkTest::FindDrawables find;
    converted->accept(find);
    REQUIRE(find.drawables.size() == 1);
    for (double height : {100.0, 10.0, 2.0})
    {
        renderer.viewer.getCamera()->setViewMatrixAsLookAt(
            osg::Vec3d(0,0,height), osg::Vec3d(), osg::Vec3d(0,1,0));
        renderer.setScene(source);
        renderer.frame(); renderer.frame();
        auto before = renderer.pixels();
        for (bool cull : {true, false})
        {
            find.drawables[0]->setUseGPUCulling(cull);
            renderer.setScene(converted);
            renderer.frame(); renderer.frame();
            auto after = renderer.pixels();
            unsigned lit = 0, different = 0;
            for (unsigned i = 0; i < 256u*256u; ++i)
            {
                if (before->data()[4*i+2] > 0) ++lit;
                for (unsigned c = 0; c < 3; ++c)
                    if (std::abs(int(before->data()[4*i+c]) - int(after->data()[4*i+c])) > 2) ++different;
            }
            INFO("Height: " << height << ", GPU culling: " << cull);
            REQUIRE(lit > 100);
            REQUIRE(different < 30); // raster edge/8-bit color rounding tolerance
            REQUIRE(glGetError() == GL_NO_ERROR);
        }
    }
}

TEST_CASE("Prestige detail cell preserves all external placements", "[.prestige]")
{
    const char* path = std::getenv("OSGEARTH_PRESTIGE_TEST_TILE");
    REQUIRE(path != nullptr);
    auto source = osgDB::readRefNodeFile(path);
    REQUIRE(source);
    struct Instances : osg::NodeVisitor
    {
        std::size_t count = 0;
        std::set<std::string> assets;
        Instances() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { }
        void apply(osg::Node& node) override
        {
            if (auto* external = dynamic_cast<InstancedExternalNode*>(&node))
            {
                count += external->getNumInstances();
                assets.insert(external->getFileName());
            }
            else if (auto* external = dynamic_cast<ExternalNode*>(&node))
            {
                ++count;
                assets.insert(external->getFileName());
            }
            else traverse(node);
        }
    } before, residual;
    source->accept(before);
    REQUIRE(before.count > 0);
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    auto factory = std::make_shared<ChonkFactory>(arena);
    auto result = ChonkFactory::convertExternalInstances(source, factory);
    ChonkTest::FindDrawables find;
    result->accept(find);
    result->accept(residual);
    REQUIRE(find.drawables.size() == 1);
    INFO("Source placements: " << before.count << ", assets: " << before.assets.size());
    INFO("Chonk placements: " << find.drawables[0]->getNumInstances() <<
        ", batches: " << find.drawables[0]->getNumBatches() << ", fallback placements: " << residual.count);
    REQUIRE(find.drawables[0]->getNumInstances() > 0);
    REQUIRE(find.drawables[0]->getNumInstances() + residual.count == before.count);
    REQUIRE(result->getBound().valid());
}

TEST_CASE("Prestige detail cell renders through Chonk", "[.prestige-gpu]")
{
    const char* path = std::getenv("OSGEARTH_PRESTIGE_TEST_TILE");
    REQUIRE(path != nullptr);
    REQUIRE(Capabilities::get().supportsNVGL());
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    auto source = osgDB::readRefNodeFile(path);
    REQUIRE(source);
    auto converted = ChonkFactory::convertExternalInstances(source, renderer.factory);
    Registry::shaderGenerator().run(source.get());
    ChonkTest::FindDrawables find;
    converted->accept(find);
    REQUIRE(find.drawables.size() == 1);
    REQUIRE(find.drawables[0]->getNumInstances() > 0);
    const auto bounds = source->getBound();
    const osg::Vec3d center(bounds.center());
    renderer.viewer.getCamera()->setViewMatrixAsLookAt(
        center + osg::Vec3d(0,-bounds.radius(),bounds.radius()*.7), center, osg::Vec3d(0,0,1));
    renderer.setScene(source);
    renderer.frame(); renderer.frame();
    auto before = renderer.pixels();
    renderer.setScene(converted);
    renderer.frame(); renderer.frame();
    auto after = renderer.pixels();
    if (std::getenv("OSGEARTH_PRESTIGE_TEST_IMAGES"))
    {
        REQUIRE(osgDB::writeImageFile(*before, "chonk-prestige-before.png"));
        REQUIRE(osgDB::writeImageFile(*after, "chonk-prestige-after.png"));
    }
    unsigned lit = 0, different = 0;
    for (unsigned i = 0; i < 256u*256u; ++i)
    {
        if (before->data()[4*i] || before->data()[4*i+1] || before->data()[4*i+2]) ++lit;
        for (unsigned c = 0; c < 3; ++c)
            different += std::abs(int(before->data()[4*i+c]) - int(after->data()[4*i+c])) > 5;
    }
    INFO("Differing color channels: " << different << ", nonempty pixels: " << lit);
    REQUIRE(lit > 100);
    REQUIRE(different < 256u*256u/100u);
    REQUIRE(glGetError() == GL_NO_ERROR);
}
