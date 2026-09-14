/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include "ChonkTestUtils.h"
#include <osgEarth/ShaderLoader>
#include <osg/Texture3D>
#include <osgDB/FileNameUtils>
#include <cstring>
#include <cstdlib>
#include <future>
#include <set>

using namespace osgEarth;
namespace osgEarth { namespace Tests { extern std::string executablePath; } }

namespace
{
    struct InspectableDrawable : ChonkDrawable
    {
        float radius() const { return _batches.begin()->second.front().radius; }
        osg::Matrixf matrix() const { return _batches.begin()->second.front().xform; }

        struct Snapshot
        {
            std::vector<Instance> sources;
            std::vector<VisibleInstance> visible;
            Chonk::DrawCommands commands;
            bool sourceUnchanged;
        };

        // Read completed GPU results with this drawable's context current.
        // Publish shader writes for readback and preserve the generic SSBO binding.
        Snapshot snapshot(osg::State& state) const
        {
            auto& objects = GLObjects::get(_globjects, state);
            auto* ext = state.get<osg::GLExtensions>();
            ext->glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            GLint previous = 0;
            glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &previous);
            Snapshot result;
            result.sources.resize(objects._all_instances.size());
            result.visible.resize(objects._instanceOutputBuf->size() / sizeof(VisibleInstance));
            result.commands.resize(objects._commands.size());
            objects._instanceInputBuf->bind();
            objects._instanceInputBuf->getBufferSubData(0,
                result.sources.size() * sizeof(Instance), result.sources.data());
            objects._instanceOutputBuf->bind();
            objects._instanceOutputBuf->getBufferSubData(0,
                result.visible.size() * sizeof(VisibleInstance), result.visible.data());
            objects._commandBuf->bind();
            objects._commandBuf->getBufferSubData(0,
                result.commands.size() * sizeof(Chonk::DrawCommand), result.commands.data());
            ext->glBindBuffer(GL_SHADER_STORAGE_BUFFER, previous);
            result.sourceUnchanged = std::memcmp(result.sources.data(), objects._all_instances.data(),
                result.sources.size() * sizeof(Instance)) == 0;
            return result;
        }
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

TEST_CASE("Chonk external instance eligibility restrictions are opt-in", "[chonk]")
{
    auto geometry = ChonkTest::mesh();
    geometry->setName("double-sided test asset");
    geometry->getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    ChonkTest::AssetFile file;
    REQUIRE(file.write(geometry));
    InstancedExternalNode::MatrixList matrices = {
        osg::Matrixf::translate(-10,0,0), osg::Matrixf::translate(10,0,0)};
    osg::ref_ptr<InstancedExternalNode> external = new InstancedExternalNode(file.path, matrices);
    REQUIRE(external->isUsingHardwareInstancing());
    osg::ref_ptr<osg::Group> source = new osg::Group();
    source->setName("double-sided test cell");
    source->getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    source->addChild(external);
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    auto factory = std::make_shared<ChonkFactory>(arena);
    const char* value = std::getenv("OSGEARTH_CHONK_ENFORCE_ELIGIBILITY");
    const bool enforce = value && std::string(value) == "1";
    auto prototype = factory->getOrCreateChonk(external->getExternalNode(), 1.0f);
    REQUIRE(bool(prototype) == !enforce);
    auto converted = ChonkFactory::convertExternalInstances(source, factory);
    ChonkTest::FindDrawables find;
    converted->accept(find);
    REQUIRE(find.drawables.size() == (enforce ? 0u : 1u));
    if (!enforce)
        REQUIRE(find.drawables[0]->getNumInstances() == matrices.size());
    REQUIRE(source->getChild(0) == external.get());
    REQUIRE(source->getStateSet()->getMode(GL_CULL_FACE) == osg::StateAttribute::OFF);
    REQUIRE(converted->getBound().valid());
}

TEST_CASE("Chonk GPU rendering preserves instances and responds to SSE", "[chonk][gpu]")
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

    // Changing global SSE must cull small instances and restore them without
    // rebuilding the cell. Keep the camera and all instance transforms fixed.
    // External conversion defaults to no size culling. Opt in explicitly for
    // this SSE-specific fixture, preserving the production conversion defaults.
    auto sseChonk = renderer.factory->getOrCreateChonk(source->getExternalNode(), 1.0f);
    REQUIRE(sseChonk);
    osg::ref_ptr<ChonkDrawable> sseDrawable = new ChonkDrawable();
    for (const auto& matrix : matrices) sseDrawable->add(sseChonk, matrix);
    renderer.setScene(sseDrawable);
    renderer.viewer.getCamera()->setViewMatrixAsLookAt(
        osg::Vec3d(0,0,100), osg::Vec3d(), osg::Vec3d(0,1,0));
    auto* sse = renderer.root->getStateSet()->getUniform("oe_sse");
    REQUIRE(sse);
    std::vector<unsigned> litPixels;
    for (float value : {0.0f, 8.0f, 128.0f, 8.0f})
    {
        sse->set(value);
        renderer.frame(); renderer.frame();
        auto pixels = renderer.pixels();
        unsigned lit = 0;
        for (unsigned i = 0; i < 256u*256u; ++i)
            if (pixels->data()[4*i+2] > 0) ++lit;
        litPixels.push_back(lit);
        INFO("Global SSE: " << value);
        REQUIRE(glGetError() == GL_NO_ERROR);
    }
    REQUIRE(litPixels[0] > 100);
    REQUIRE(litPixels[1] == litPixels[0]);
    REQUIRE(litPixels[2] == 0);
    REQUIRE(litPixels[3] == litPixels[0]);
}

// Isolate the visibility regression from viewer tests that reuse context IDs;
// Chonk's shared render-bin resources can outlive the previous viewer. Each
// worker chooses its culling mode before the first frame and keeps it fixed.
TEST_CASE("Chonk visibility records preserve source placements and LOD results", "[chonk][gpu]")
{
    const auto& executable = osgEarth::Tests::executablePath;
    for (const std::string tag : {"[.chonk-visibility-gpu-worker]", "[.chonk-visibility-cpu-worker]"})
    {
#ifdef _WIN32
        const std::string command = "\"\"" + executable + "\" \"" + tag + "\"\"";
#else
        std::string quoted = "'";
        for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1, c);
        const std::string command = quoted + "' '" + tag + "'";
#endif
        INFO(tag);
        REQUIRE(std::system(command.c_str()) == 0);
    }
}

// Exercise sparse source indices, padded batches, two simultaneous LODs,
// multiple drawables, and wind shader consumers with a fixed culling mode.
// Run in an isolated worker so the context owns all shared Chonk GL resources.
static void validateChonkVisibility(bool cull)
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Chonk GPU test needs NVGL and OSG_GL_CONTEXT_VERSION=4.6");
        return;
    }
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    auto* state = renderer.context->getState();
    auto* ss = renderer.root->getOrCreateStateSet();
    ss->getUniform("oe_sse")->set(32.0f);
    ss->addUniform(new osg::Uniform("oe_chonk_lod_transition_factor", 2.0f));

    // Load the real vegetation shader and exercise its source-table reads.
    const auto vegetationShader = osgDB::getFilePath(__FILE__) +
        "/../../osgEarthProcedural/Procedural.Vegetation.glsl";
    REQUIRE(ShaderLoader::load(VirtualProgram::getOrCreate(ss), vegetationShader, ShaderPackage()));
    osg::ref_ptr<osg::Image> windImage = new osg::Image();
    windImage->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    windImage->data()[0] = 255;
    windImage->data()[1] = windImage->data()[2] = 0;
    windImage->data()[3] = 128;
    const int noiseIndex = renderer.textures->add(Texture::create(windImage));
    ss->setDefine("OE_NOISE_TEX_INDEX", std::to_string(noiseIndex));
    ss->setDefine("OE_WIND_TEX", "chonk_test_wind");
    ss->setDefine("OE_WIND_TEX_MATRIX", "chonk_test_wind_matrix");
    ss->setTextureAttributeAndModes(3, new osg::Texture3D(windImage));
    ss->addUniform(new osg::Uniform("chonk_test_wind", 3));
    ss->addUniform(new osg::Uniform("chonk_test_wind_matrix", osg::Matrixf()));

    auto geometry = ChonkTest::mesh();
    auto flex = new osg::Vec3Array();
    flex->assign(4, osg::Vec3(0, 0, 0.1f));
    geometry->setTexCoordArray(3, flex);
    auto dual = renderer.factory->getOrCreateChonk(geometry, 1.0f);
    auto single = renderer.factory->getOrCreateChonk(ChonkTest::mesh(), 0.0f);
    REQUIRE(dual);
    REQUIRE(single);
    // Both LODs deliberately share geometry; only their culling ranges differ.
    dual->_lods.push_back(dual->_lods.front());
    dual->_lods.back().far_pixel_scale = 0.0f;

    osg::ref_ptr<InspectableDrawable> first = new InspectableDrawable();
    osg::ref_ptr<InspectableDrawable> second = new InspectableDrawable();
    first->setUseGPUCulling(cull);
    second->setUseGPUCulling(cull);
    first->setBirthday(-10.0);
    second->setBirthday(-10.0);
    first->setAlphaCutoff(0.371234f);
    second->setAlphaCutoff(0.612345f);
    first->add(dual, osg::Matrixf::translate(10000, 0, 0), osg::Vec2f(0.01f, 0));
    first->add(dual, osg::Matrixf::translate(-8, 0, 0), osg::Vec2f(0.11f, 0));
    first->add(dual, osg::Matrixf::translate(8, 0, 0), osg::Vec2f(0.21f, 0));
    first->add(single, osg::Matrixf::translate(12000, 0, 0), osg::Vec2f(0.31f, 0));
    first->add(single, osg::Matrixf::translate(0, 8, 0), osg::Vec2f(0.41f, 0));
    second->add(single, osg::Matrixf::translate(15000, 0, 0), osg::Vec2f(0.51f, 0));
    second->add(single, osg::Matrixf::translate(0, -8, 0), osg::Vec2f(0.61f, 0));
    osg::ref_ptr<osg::Group> scene = new osg::Group();
    scene->addChild(first);
    scene->addChild(second);
    renderer.setScene(scene);

    // Complete the rendering sequence before readback; inspect only the final
    // results so validation does not change buffer usage between draw frames.
    for (unsigned frame = 0; frame < 4; ++frame)
        renderer.frame();

    for (auto* drawable : {first.get(), second.get()})
    {
        auto snapshot = drawable->snapshot(*state);
        REQUIRE(snapshot.sourceUnchanged);
        std::set<std::pair<unsigned, unsigned>> expected, actual;
        for (unsigned i = 0; i < snapshot.sources.size(); ++i)
        {
            const auto& source = snapshot.sources[i];
            if (source.first_lod_cmd_index < 0) continue;
            if (cull && source.xform.getTrans().x() > 1000.0f) continue;
            expected.emplace(i, 0u);
            if (cull && source.uv.x() < 0.3f) expected.emplace(i, 1u);
        }
        for (unsigned command = 0; command < snapshot.commands.size(); ++command)
        {
            const auto& draw = snapshot.commands[command].cmd;
            REQUIRE(draw.baseInstance + draw.instanceCount <= snapshot.visible.size());
            for (unsigned j = 0; j < draw.instanceCount; ++j)
            {
                const auto& visible = snapshot.visible[draw.baseInstance + j];
                REQUIRE(visible.sourceIndex < snapshot.sources.size());
                const auto& source = snapshot.sources[visible.sourceIndex];
                REQUIRE(source.first_lod_cmd_index >= 0);
                REQUIRE(unsigned(source.first_lod_cmd_index) + visible.lod == command);
                REQUIRE(actual.emplace(visible.sourceIndex, visible.lod).second);
                REQUIRE(visible.alphaCutoff == (drawable == first.get() ? 0.371234f : 0.612345f));
                if (cull && source.uv.x() < 0.3f && visible.lod == 0)
                {
                    REQUIRE(visible.fade > 0.1f);
                    REQUIRE(visible.fade < 1.0f);
                }
                else REQUIRE(visible.fade == 1.0f);
            }
        }
        REQUIRE(actual == expected);
    }

    // Every on-screen placement must render, including the first drawable
    // after another drawable's cull dispatch has changed the source binding.
    auto pixels = renderer.pixels();
    auto* camera = renderer.viewer.getCamera();
    const auto toWindow = camera->getViewMatrix() * camera->getProjectionMatrix() *
        camera->getViewport()->computeWindowMatrix();
    for (const auto& center : {osg::Vec3d(-8,0,0), osg::Vec3d(8,0,0),
        osg::Vec3d(0,8,0), osg::Vec3d(0,-8,0)})
    {
        const auto p = center * toWindow;
        REQUIRE(pixels->data(unsigned(p.x()), unsigned(p.y()))[2] > 200);
    }
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Validate GPU-generated visibility records in a fresh process/context.
TEST_CASE("Chonk GPU visibility validation", "[.chonk-visibility-gpu-worker]")
{
    validateChonkVisibility(true);
}

// Validate CPU-generated visibility records with GPU culling disabled at startup.
TEST_CASE("Chonk unculled visibility validation", "[.chonk-visibility-cpu-worker]")
{
    validateChonkVisibility(false);
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
