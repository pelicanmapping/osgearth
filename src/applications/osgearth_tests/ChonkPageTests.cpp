/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/DrawInstanced>
#include "ChonkUniqueTestUtils.h"
#include "ChonkMaterialTestUtils.h"
#include <osgUtil/LineSegmentIntersector>
#include <osgUtil/IntersectionVisitor>
#include <osg/TexMat>
#include <osg/CullFace>
#include <set>
#include <cstdlib>
#include <iostream>

using namespace osgEarth;
namespace osgEarth { namespace Tests { extern std::string executablePath; } }

namespace
{
    struct PageProbe : ChonkDrawable
    {
        // Count storage owners independently from the number of logical meshes.
        std::size_t pages() const { return _pageBatches.size(); }
        // Inspect immutable pages without extending ownership past this drawable.
        const PageBatches& pageBatches() const { return _pageBatches; }
        struct Snapshot
        {
            std::vector<Instance> sources;
            std::vector<VisibleInstance> visible;
            Chonk::DrawCommands commands; // GPU culling: one list of `stride` commands per List
            unsigned stride = 0;
        };
        // Read completed commands/visibility for this current GL context only.
        Snapshot snapshot(osg::State& state) const
        {
            const auto& objects = GLObjects::get(_globjects, state);
            auto* gl = state.get<osg::GLExtensions>();
            gl->glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            GLint previous = 0;
            glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &previous);
            Snapshot result;
            result.sources = objects._all_instances;
            result.stride = unsigned(objects._commands.size());
            result.commands.resize(result.stride * (_gpucull ? unsigned(NUM_LISTS) : 1u));
            result.visible.resize(objects._instanceOutputBuf->size()/sizeof(VisibleInstance));
            objects._commandBuf->bind();
            objects._commandBuf->getBufferSubData(0, result.commands.size()*sizeof(Chonk::DrawCommand), result.commands.data());
            objects._instanceOutputBuf->bind();
            objects._instanceOutputBuf->getBufferSubData(0, result.visible.size()*sizeof(VisibleInstance), result.visible.data());
            gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, previous);
            return result;
        }
    };

    // Intersect through the drawable's primitive functor, exercising CPU offsets.
    bool hits(osg::Node* node, float x, float y)
    {
        osg::ref_ptr<osgUtil::LineSegmentIntersector> ray = new osgUtil::LineSegmentIntersector(
            osg::Vec3d(x,y,10), osg::Vec3d(x,y,-10));
        osgUtil::IntersectionVisitor visitor(ray);
        node->accept(visitor);
        return ray->containsIntersections();
    }
}

// Nonzero index/base-vertex offsets must retain object-local coordinates and
// correct picking when the same range has more than one placement.
TEST_CASE("Chonk pages share geometry and preserve bounds and intersections", "[chonk][chonk-pages]")
{
    osg::ref_ptr<TextureArena> textures = new TextureArena();
    ChonkFactory factory(textures);
    auto builder = factory.createGeometryPageBuilder();
    auto a = ChonkTest::mesh(), b = ChonkTest::mesh(2);
    const auto first = builder.addMesh(a), second = builder.addMesh(b);
    REQUIRE(first == 0); REQUIRE(second == 1);
    auto page = builder.finish();
    REQUIRE(page);
    REQUIRE_FALSE(builder.finish());
    REQUIRE(builder.addMesh(a) == ChonkGeometryPage::INVALID_MESH);
    REQUIRE(page->meshes()[second].firstVertex == 4);
    REQUIRE(page->meshes()[second].firstIndex == 6);
    REQUIRE(page->indices()[6] == 0);
    REQUIRE(page->vertices().size() == 13);
    REQUIRE(a->getVertexArray()->getNumElements() == 4);
    osg::ref_ptr<PageProbe> drawable = new PageProbe();
    REQUIRE(drawable->add(page, std::vector<ChonkDrawable::PagePlacement>{
        {first, osg::Matrixf::translate(-10,0,0), {}},
        {second, osg::Matrixf::translate(10,0,0), {}},
        {first, osg::Matrixf::translate(20,0,0), {}}}));
    REQUIRE(drawable->pages() == 1);
    REQUIRE(drawable->getNumBatches() == 2);
    REQUIRE(drawable->getNumInstances() == 3);
    REQUIRE(drawable->computeBoundingBox().xMin() == -12.0f);
    REQUIRE(drawable->computeBoundingBox().xMax() == 22.0f);
    REQUIRE(hits(drawable, -10, 0));
    REQUIRE(hits(drawable, 10, 0));
    REQUIRE(hits(drawable, 20, 0));
    REQUIRE_FALSE(hits(drawable, 0, 0));
    // Adding after the first proxy build must dirty both proxy and scene bounds.
    REQUIRE(drawable->add(page, second, osg::Matrixf::translate(30,0,0)));
    REQUIRE(hits(drawable, 30, 0));
    auto legacy = factory.getOrCreateChonk(a);
    drawable->add(legacy, osg::Matrixf::translate(40,0,0));
    drawable->add(legacy, osg::Matrixf::translate(50,0,0));
    REQUIRE(hits(drawable, 40, 0));
    REQUIRE(hits(drawable, 50, 0));
}

// All invalid input paths leave existing geometry and placements intact.
TEST_CASE("Chonk page appends validate ranges state capacity and arenas", "[chonk][chonk-pages]")
{
    osg::ref_ptr<TextureArena> textures = new TextureArena();
    ChonkFactory factory(textures);
    auto builder = factory.createGeometryPageBuilder(500);
    auto mesh = ChonkTest::mesh();
    REQUIRE(builder.addMesh(mesh) == 0);
    auto invalid = ChonkTest::mesh();
    (*static_cast<osg::DrawElementsUInt*>(invalid->getPrimitiveSet(0)))[0] = ~0u;
    REQUIRE(builder.addMesh(invalid) == ChonkGeometryPage::INVALID_MESH);
    auto blending = ChonkTest::mesh();
    blending->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
    REQUIRE(builder.addMesh(blending) == ChonkGeometryPage::INVALID_MESH);
    auto unsupported = ChonkTest::mesh();
    unsupported->getNormalArray()->setBinding(osg::Array::BIND_PER_PRIMITIVE_SET);
    REQUIRE(builder.addMesh(unsupported) == ChonkGeometryPage::INVALID_MESH);
    unsupported->getNormalArray()->setBinding(osg::Array::BIND_OVERALL);
    unsupported->getVertexArray()->setDataVariance(osg::Object::DYNAMIC);
    REQUIRE(builder.addMesh(unsupported) == ChonkGeometryPage::INVALID_MESH);
    auto textureState = ChonkTest::mesh();
    textureState->getOrCreateStateSet()->setTextureAttribute(0, new osg::TexMat());
    REQUIRE(builder.addMesh(textureState) == ChonkGeometryPage::INVALID_MESH);
    REQUIRE(builder.addMesh(mesh) == 1);
    REQUIRE(builder.addMesh(mesh) == ChonkGeometryPage::INVALID_MESH);
    auto page = builder.finish();
    REQUIRE(page->meshes().size() == 2);
    REQUIRE(page->vertices().size() == 8);
    REQUIRE(page->indices().size() == 12);
    osg::ref_ptr<PageProbe> drawable = new PageProbe();
    REQUIRE(drawable->add(page, 0));
    REQUIRE_FALSE(drawable->add(page, std::vector<ChonkDrawable::PagePlacement>{{1, {}, {}}, {99, {}, {}}}));
    REQUIRE_FALSE(drawable->add(page, 1, osg::Matrixf::scale(-1,1,1)));
    REQUIRE(drawable->getNumInstances() == 1);
    osg::ref_ptr<TextureArena> otherTextures = new TextureArena();
    ChonkFactory otherFactory(otherTextures);
    auto otherBuilder = otherFactory.createGeometryPageBuilder();
    REQUIRE(otherBuilder.addMesh(mesh) == 0);
    REQUIRE_FALSE(drawable->add(otherBuilder.finish(), 0));
    REQUIRE(drawable->getNumInstances() == 1);
}

// Ripper must keep sibling transforms distinct, roll over pages, and treat
// explicit instancing as repeated placements rather than copying its proxy mesh.
TEST_CASE("Chonk Ripper packs unique and instanced geometry into pages", "[chonk][chonk-pages]")
{
    osg::ref_ptr<TextureArena> textures = new TextureArena();
    ChonkFactory factory(textures);
    factory.geometryPageSize = 500;
    ChonkTest::UniqueScene scene(3, 1);
    osg::ref_ptr<Util::DrawInstanced::InstanceGeometry> repeated =
        new Util::DrawInstanced::InstanceGeometry(*ChonkTest::mesh());
    repeated->setMatrices({osg::Matrixf::translate(30,0,0), osg::Matrixf::translate(40,0,0)});
    scene.root->addChild(repeated);
    osg::ref_ptr<PageProbe> drawable = new PageProbe();
    REQUIRE(drawable->add(scene.root, factory, 0.0f));
    REQUIRE(drawable->pages() == 2);
    REQUIRE(drawable->getNumBatches() == 4);
    REQUIRE(drawable->getNumInstances() == 5);
    REQUIRE(hits(drawable, 30, 0)); REQUIRE(hits(drawable, 40, 0));
    for (unsigned i = 0; i < 3; ++i)
    {
        const auto center = scene.transforms[i].getTrans();
        REQUIRE(hits(drawable, center.x(), center.y()));
    }
    auto broken = ChonkTest::mesh();
    (*static_cast<osg::DrawElementsUInt*>(broken->getPrimitiveSet(0)))[0] = ~0u;
    scene.root->addChild(broken);
    REQUIRE_FALSE(drawable->add(scene.root, factory, 0.0f));
    REQUIRE(drawable->getNumInstances() == 5);
    REQUIRE(drawable->pages() == 2);
}

// A page retains its material IDs even when the source/factory are released.
TEST_CASE("Chonk pages retain materials and transformed submesh data", "[chonk][chonk-pages]")
{
    osg::ref_ptr<TextureArena> textures = new TextureArena();
    textures->setAutoRelease(true);
    ChonkGeometryPage::Ptr page;
    {
        ChonkFactory factory(textures);
        auto builder = factory.createGeometryPageBuilder();
        auto source = ChonkTest::mesh();
        source->getOrCreateStateSet()->setTextureAttribute(0, ChonkTest::solidTexture(osg::Vec4(1,0,0,1)));
        osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform(osg::Matrix::scale(2,3,1));
        transform->addChild(source);
        REQUIRE(builder.addMesh(transform) == 0);
        auto pbrSource = ChonkTest::mesh();
        osg::ref_ptr<PBRTexture> pbr = new PBRTexture();
        pbr->albedo = ChonkTest::solidTexture(osg::Vec4(0,1,0,1));
        pbrSource->getOrCreateStateSet()->setTextureAttribute(0, pbr);
        REQUIRE(builder.addMesh(pbrSource) == 1);
        page = builder.finish();
    }
    REQUIRE(page->vertices()[0].position == osg::Vec3(-4,-6,0));
    const auto material = textures->getMaterialArena()->find(page->vertices()[0].material_index);
    REQUIRE(material);
    REQUIRE(material->textures[0] >= 0);
}

namespace
{
    // Exercise page commands, legacy LOD commands, dense source packing and
    // per-camera visibility in a dedicated context with a fixed culling mode.
    void validatePageRendering(bool cull, bool finishEmpty = false)
    {
        if (!Capabilities::get().supportsNVGL())
        { WARN("Chonk page GPU test requires NVGL and OpenGL 4.6"); return; }
        ChonkTest::Renderer renderer;
        REQUIRE(renderer.initialize());
        ChonkTest::UniqueScene source(65, 1);
        renderer.factory->geometryPageSize = 4096; // exercise multiple VBO/EBO pairs
        osg::ref_ptr<PageProbe> drawable = new PageProbe();
        drawable->setUseGPUCulling(cull);
        drawable->setBirthday(-10);
        REQUIRE(drawable->add(source.root, *renderer.factory, 0.0f));
        REQUIRE(drawable->pages() > 1);
        auto legacy = renderer.factory->getOrCreateChonk(ChonkTest::mesh());
        REQUIRE(legacy);
        legacy->_lods.push_back(legacy->_lods.front());
        drawable->add(legacy, osg::Matrixf::translate(10000,0,0));
        auto builder = renderer.factory->createGeometryPageBuilder();
        REQUIRE(builder.addMesh(ChonkTest::mesh()) == 0);
        auto repeated = builder.finish();
        REQUIRE(drawable->add(repeated, std::vector<ChonkDrawable::PagePlacement>{
            {0, osg::Matrixf::translate(11000,0,0), {}},
            {0, osg::Matrixf::translate(12000,0,0), {}}}));
        REQUIRE(drawable->getNumInstances() == 68);
        renderer.setScene(source.root);
        renderer.frame(); renderer.frame();
        auto reference = renderer.pixels();
        renderer.setScene(drawable);
        for (double x : {0.0, 4000.0, 0.0, finishEmpty ? 4000.0 : 0.0})
        {
            renderer.viewer.getCamera()->setViewMatrixAsLookAt(
                osg::Vec3d(x,0,100), osg::Vec3d(x,0,0), osg::Vec3d(0,1,0));
            renderer.frame(); renderer.frame();
            auto pixels = renderer.pixels();
            if (x == 0) REQUIRE(ChonkTest::differentPixels(*reference, *pixels) <= 30);
            else
            {
                unsigned colored = 0;
                for (unsigned i = 0; i < 256u*256u; ++i) colored += pixels->data()[4*i+2] != 0;
                REQUIRE(colored == 0);
            }
        }
        // Read once after the camera-transition sequence. On this NV driver,
        // readback followed by reusing the indirect buffer can terminate GL.
        {
            const double x = finishEmpty ? 4000.0 : 0.0;
            auto snapshot = drawable->snapshot(*renderer.context->getState());
            REQUIRE(snapshot.sources.size() == 96); // global padding only
            REQUIRE(snapshot.stride == 68); // 65 + legacy's two LODs + repeated mesh
            std::size_t survivors = 0, valid = 0;
            for (const auto& s : snapshot.sources) valid += s.first_lod_cmd_index >= 0;
            REQUIRE(valid == 68);
            for (unsigned command = 0; command < snapshot.commands.size(); ++command)
            {
                const auto& draw = snapshot.commands[command];
                REQUIRE(draw.cmd.baseInstance + draw.cmd.instanceCount <= snapshot.visible.size());
                survivors += draw.cmd.instanceCount;
                for (unsigned j = 0; j < draw.cmd.instanceCount; ++j)
                {
                    const auto& visible = snapshot.visible[draw.cmd.baseInstance+j];
                    REQUIRE(visible.sourceIndex < snapshot.sources.size());
                    REQUIRE(unsigned(snapshot.sources[visible.sourceIndex].first_lod_cmd_index) + visible.lod ==
                        command % snapshot.stride);
                }
            }
            if (cull && x > 0) REQUIRE(survivors == 0);
            if (cull && x == 0) REQUIRE(survivors == 65);
            if (!cull) REQUIRE(survivors == 68);
            const auto& commands = repeated->getOrCreateCommands(*renderer.context->getState());
            REQUIRE(commands.size() == 1);
            REQUIRE(glGetError() == GL_NO_ERROR);
        }
        // Page commands share buffer addresses but use distinct, nonzero offsets.
        for (const auto& batch : drawable->pageBatches())
        {
            const auto& commands = batch.first->getOrCreateCommands(*renderer.context->getState());
            for (unsigned i = 0; i < commands.size(); ++i)
            {
                REQUIRE(commands[i].vertexBuffer.address == commands[0].vertexBuffer.address);
                REQUIRE(commands[i].indexBuffer.address == commands[0].indexBuffer.address);
                REQUIRE(commands[i].cmd.baseVertex == batch.first->meshes()[i].firstVertex);
                REQUIRE(commands[i].cmd.firstIndex == batch.first->meshes()[i].firstIndex);
            }
        }
    }
}

// Isolate viewers from other GPU fixtures whose global render-bin programs live on.
TEST_CASE("Chonk page rendering matches OSG with culling enabled and disabled", "[chonk][chonk-pages][gpu]")
{
    for (const std::string tag : {"[.chonk-page-gpu-worker]", "[.chonk-page-empty-worker]", "[.chonk-page-cpu-worker]"})
    {
#ifdef _WIN32
        const auto command = "\"\"" + osgEarth::Tests::executablePath + "\" \"" + tag + "\"\"";
#else
        std::string quoted = "'";
        for (char c : osgEarth::Tests::executablePath) quoted += c == '\'' ? "'\\''" : std::string(1, c);
        const auto command = quoted + "' '" + tag + "'";
#endif
        REQUIRE(std::system(command.c_str()) == 0);
    }
}

// Run the actual compute shader with multiple pages and mixed placement counts.
TEST_CASE("Chonk page GPU worker", "[.chonk-page-gpu-worker]") { validatePageRendering(true); }
// Validate the matching CPU-built visible records without changing modes mid-frame.
TEST_CASE("Chonk page unculled worker", "[.chonk-page-cpu-worker]") { validatePageRendering(false); }

// Inspect an all-rejected final dispatch in an independent GL context.
TEST_CASE("Chonk page empty worker", "[.chonk-page-empty-worker]") { validatePageRendering(true, true); }

// The policy is startup-only, so exercise the legacy toggle in a child process.
TEST_CASE("Chonk Ripper can select its original storage policy", "[chonk][chonk-pages]")
{
#ifdef _WIN32
    const auto command = "set OSGEARTH_CHONK_GEOMETRY_PAGES=0&& \"" +
        osgEarth::Tests::executablePath + "\" \"[.chonk-legacy-ripper-worker]\"";
#else
    std::string quoted = "'";
    for (char c : osgEarth::Tests::executablePath) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    const auto command = "OSGEARTH_CHONK_GEOMETRY_PAGES=0 " + quoted + "' '[.chonk-legacy-ripper-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
}

// Preserve coarse ordinary geometry and explicit instances, including transforms,
// when pages are disabled. Invalid input must leave existing batches intact.
TEST_CASE("Chonk legacy Ripper toggle worker", "[.chonk-legacy-ripper-worker]")
{
    osg::ref_ptr<TextureArena> textures = new TextureArena();
    ChonkFactory factory(textures);
    factory.geometryPageSize = 1; // The legacy layout does not use page capacity.
    ChonkTest::UniqueScene scene(3, 1);
    osg::ref_ptr<Util::DrawInstanced::InstanceGeometry> repeated =
        new Util::DrawInstanced::InstanceGeometry(*ChonkTest::mesh());
    repeated->setMatrices({osg::Matrixf::translate(30,0,0), osg::Matrixf::translate(40,0,0)});
    scene.root->addChild(repeated);
    osg::ref_ptr<PageProbe> drawable = new PageProbe();
    REQUIRE(drawable->add(scene.root, factory, 0.0f));
    REQUIRE(drawable->pages() == 0);
    REQUIRE(drawable->getNumBatches() == 2);
    REQUIRE(drawable->getNumInstances() == 3); // one merged + two explicit placements
    for (const auto& transform : scene.transforms)
        REQUIRE(hits(drawable, transform.getTrans().x(), transform.getTrans().y()));
    REQUIRE(hits(drawable, 30, 0));
    REQUIRE(hits(drawable, 40, 0));
    auto broken = ChonkTest::mesh();
    (*static_cast<osg::DrawElementsUInt*>(broken->getPrimitiveSet(0)))[0] = ~0u;
    scene.root->addChild(broken);
    REQUIRE_FALSE(drawable->add(scene.root, factory, 0.0f));
    REQUIRE(drawable->getNumBatches() == 2);
    REQUIRE(drawable->getNumInstances() == 3);
    auto sceneFactory = std::make_shared<ChonkFactory>(textures);
    ChonkTest::UniqueScene ordinary(3, 1);
    auto unchanged = ChonkFactory::convertScene(ordinary.root, sceneFactory);
    ChonkTest::DrawCounts counts;
    unchanged->accept(counts);
    REQUIRE(counts.ordinary == 3);
    REQUIRE(counts.chonks == 0);
}

namespace
{
    // Mirror glTF's material layout, including the shared root program and
    // redundant per-material component bindings that Ripper replaces with IDs.
    void installSceneMaterials(ChonkTest::UniqueScene& scene)
    {
        PBRTexture::installProgram(scene.root->getOrCreateStateSet());
        scene.root->getStateSet()->setAttributeAndModes(new osg::CullFace(osg::CullFace::BACK));
        for (unsigned i = 0; i < scene.meshes.size(); ++i)
        {
            osg::ref_ptr<PBRTexture> pbr = new PBRTexture();
            pbr->albedo = ChonkTest::solidTexture(osg::Vec4(.5f + .01f*i, .7f, .9f, 1));
            if (i % 3 == 0)
            {
                pbr->normal = ChonkTest::solidTexture(osg::Vec4(.5f,.5f,1,1));
                pbr->pbr = ChonkTest::solidTexture(osg::Vec4(.6f,.7f,.4f,1));
                pbr->occlusion = ChonkTest::solidTexture(osg::Vec4(.8f,.8f,.8f,1));
                pbr->layoutAndFactors = osg::Vec4(PBRMaterial::RM,.8f,.7f,.6f);
            }
            pbr->install(scene.meshes[i]->getOrCreateStateSet());
            scene.meshes[i]->setUserValue(CHONK_HINT_LINEAR_COLOR, true);
            if (i % 2)
            {
                scene.meshes[i]->getStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
                auto* indices = static_cast<osg::DrawElementsUInt*>(scene.meshes[i]->getPrimitiveSet(0));
                for (unsigned k = 0; k < indices->size(); k += 3) std::swap((*indices)[k], (*indices)[k+1]);
            }
            // The real loader places geometries inside Geodes.
            auto* transform = scene.root->getChild(i)->asGroup();
            osg::ref_ptr<osg::Geode> geode = new osg::Geode();
            geode->addDrawable(scene.meshes[i]);
            transform->removeChildren(0, transform->getNumChildren());
            transform->addChild(geode);
        }
    }
}

// A single glTF-style draw keeps its original graph/materials. Explicit Chonk
// conversion remains available, and two source draws still justify packing.
TEST_CASE("Chonk scene conversion bypasses ordinary singleton draws", "[chonk][chonk-pages]")
{
    ChonkTest::UniqueScene scene(1, 1);
    installSceneMaterials(scene);
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    auto factory = std::make_shared<ChonkFactory>(arena);
    auto unchanged = ChonkFactory::convertScene(scene.root, factory);
    REQUIRE(unchanged == scene.root);
    ChonkTest::DrawCounts counts;
    unchanged->accept(counts);
    REQUIRE(counts.ordinary == 1);
    REQUIRE(counts.chonks == 0);
    REQUIRE(hits(unchanged, scene.transforms[0].getTrans().x(), scene.transforms[0].getTrans().y()));

    auto ordinary = ChonkTest::mesh();
    REQUIRE(ChonkFactory::convertScene(ordinary, factory) == ordinary);
    osg::ref_ptr<ChonkDrawable> explicitRipper = new ChonkDrawable();
    REQUIRE(explicitRipper->add(ordinary, *factory, 0.0f));
    REQUIRE(explicitRipper->getNumBatches() == 1);

    // One Geometry may issue multiple draws; merge its two disjoint triangles.
    auto indices = new osg::DrawElementsUInt(GL_TRIANGLES);
    indices->push_back(0); indices->push_back(3); indices->push_back(2);
    static_cast<osg::DrawElementsUInt*>(ordinary->getPrimitiveSet(0))->resize(3);
    ordinary->addPrimitiveSet(indices);
    auto packed = ChonkFactory::convertScene(ordinary, factory);
    ChonkTest::DrawCounts multipleSets;
    packed->accept(multipleSets);
    REQUIRE(multipleSets.ordinary == 0);
    REQUIRE(multipleSets.chonks == 1);
    REQUIRE(multipleSets.batches == 1);
}

// A unique draw can share an already useful instance batch; child order must
// not affect the decision, and a single instanced prototype still converts.
TEST_CASE("Chonk scene conversion keeps singleton meshes alongside instances", "[chonk][chonk-pages]")
{
    ChonkTest::AssetFile file;
    REQUIRE(file.write(ChonkTest::mesh()));
    osg::ref_ptr<InstancedExternalNode> instances = new InstancedExternalNode(file.path,
        {osg::Matrixf::translate(10,0,0), osg::Matrixf::translate(20,0,0)});
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    auto factory = std::make_shared<ChonkFactory>(arena);
    for (unsigned order = 0; order < 2; ++order)
    {
        ChonkTest::UniqueScene scene(1, 1);
        scene.root->insertChild(order, instances);
        auto packed = ChonkFactory::convertScene(scene.root, factory);
        ChonkTest::DrawCounts counts;
        packed->accept(counts);
        REQUIRE(counts.ordinary == 0);
        REQUIRE(counts.chonks == 1);
        REQUIRE(counts.batches == 2);
        REQUIRE(counts.placements == 3);
    }
    auto packed = ChonkFactory::convertScene(instances, factory);
    ChonkTest::DrawCounts counts;
    packed->accept(counts);
    REQUIRE(counts.chonks == 1);
    REQUIRE(counts.placements == 2);
}

// Reloads can turn a singleton into a batch and back. Keeping the external
// wrapper observed must not allocate page drawables while only one draw exists.
TEST_CASE("Chonk singleton bypass reevaluates ordinary asset reloads", "[chonk][chonk-pages]")
{
    ChonkTest::AssetFile file;
    REQUIRE(file.write(ChonkTest::mesh()));
    osg::ref_ptr<ExternalNode> external = new ExternalNode(file.path);
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    auto factory = std::make_shared<ChonkFactory>(arena);
    auto result = ChonkFactory::convertScene(external, factory);
    ChonkTest::DrawCounts initial;
    result->accept(initial);
    REQUIRE(initial.ordinary == 1);
    REQUIRE(initial.chonks == 0);
    ChonkTest::UniqueScene multiple(2, 1);
    REQUIRE(file.write(multiple.root));
    REQUIRE(ExternalAssetManager::instance().reload(file.path) == 1);
    ChonkTest::update(result);
    ChonkTest::DrawCounts packed;
    result->accept(packed);
    REQUIRE(packed.ordinary == 0);
    REQUIRE(packed.chonks == 1);
    REQUIRE(packed.placements == 2);
    REQUIRE(file.write(ChonkTest::mesh()));
    REQUIRE(ExternalAssetManager::instance().reload(file.path) == 1);
    ChonkTest::update(result);
    ChonkTest::DrawCounts singleton;
    result->accept(singleton);
    REQUIRE(singleton.ordinary == 1);
    REQUIRE(singleton.chonks == 0);
    REQUIRE(ExternalAssetManager::instance().unload(file.path) == 1);
    ChonkTest::update(result);
    ChonkTest::DrawCounts empty;
    result->accept(empty);
    REQUIRE(empty.ordinary == 0);
    REQUIRE(empty.chonks == 0);
    REQUIRE(ExternalAssetManager::instance().reload(file.path) == 1);
    ChonkTest::update(result);
    ChonkTest::DrawCounts reloaded;
    result->accept(reloaded);
    REQUIRE(reloaded.ordinary == 1);
    REQUIRE(reloaded.chonks == 0);
}

// Exercise the same mixed converter as Prestige, rather than calling Ripper
// directly. Unknown uniforms, blending, and dynamic branches remain ordinary.
TEST_CASE("Chonk scene conversion packs ordinary PBR geometry and retains fallback state", "[chonk][chonk-pages]")
{
    ChonkTest::UniqueScene scene(3, 1);
    installSceneMaterials(scene);
    scene.meshes[0]->setUserValue(CHONK_HINT_LINEAR_COLOR, false);
    auto* material = static_cast<PBRTexture*>(scene.meshes[0]->getStateSet()->getTextureAttribute(
        0, osg::StateAttribute::TEXTURE));
    auto blended = ChonkTest::mesh();
    material->install(blended->getOrCreateStateSet());
    blended->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
    scene.root->addChild(blended);
    auto custom = ChonkTest::mesh();
    material->install(custom->getOrCreateStateSet());
    custom->getOrCreateStateSet()->addUniform(new osg::Uniform("custom", 1.0f));
    scene.root->addChild(custom);
    osg::ref_ptr<osg::MatrixTransform> dynamic = new osg::MatrixTransform();
    dynamic->setDataVariance(osg::Object::DYNAMIC);
    dynamic->addChild(ChonkTest::mesh());
    scene.root->addChild(dynamic);
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    auto factory = std::make_shared<ChonkFactory>(arena);
    factory->geometryPageSize = 250; // one quad per page
    auto result = ChonkFactory::convertScene(scene.root, factory);
    ChonkTest::DrawCounts before, after;
    scene.root->accept(before); result->accept(after);
    REQUIRE(before.ordinary == 6);
    REQUIRE(after.ordinary == 3);
    REQUIRE(after.chonks == 2); // front faces and two-sided raster state
    REQUIRE(after.batches == 3);
    REQUIRE(after.placements == 3);
    REQUIRE(scene.root->getNumChildren() == 6);
    bool originalHint = true;
    REQUIRE(scene.meshes[0]->getUserValue(CHONK_HINT_LINEAR_COLOR, originalHint));
    REQUIRE_FALSE(originalHint);
    for (const auto& transform : scene.transforms)
        REQUIRE(hits(result, transform.getTrans().x(), transform.getTrans().y()));
}

// Keep manager-owned ordinary references reloadable after their payloads move
// into pages. Existing external instance batches must still share the drawable.
TEST_CASE("Chonk scene pages follow ordinary external reloads alongside instances", "[chonk][chonk-pages]")
{
    ChonkTest::AssetFile file;
    REQUIRE(file.write(ChonkTest::mesh()));
    osg::ref_ptr<ExternalNode> external = new ExternalNode(file.path);
    osg::ref_ptr<InstancedExternalNode> instanced = new InstancedExternalNode(file.path,
        {osg::Matrixf::translate(10,0,0), osg::Matrixf::translate(20,0,0)});
    ChonkTest::UniqueScene scene(3, 1);
    scene.root->addChild(external); scene.root->addChild(instanced);
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    auto factory = std::make_shared<ChonkFactory>(arena);
    auto result = ChonkFactory::convertScene(scene.root, factory);
    ChonkTest::DrawCounts counts;
    result->accept(counts);
    REQUIRE(counts.ordinary == 0);
    REQUIRE(counts.chonks == 1);
    REQUIRE(counts.placements == 6);
    REQUIRE(ExternalAssetManager::instance().unload(file.path) == 1);
    ChonkTest::update(result);
    ChonkTest::DrawCounts unloaded;
    result->accept(unloaded);
    REQUIRE(unloaded.placements == 3);
    REQUIRE(ExternalAssetManager::instance().reload(file.path) == 1);
    ChonkTest::update(result);
    ChonkTest::DrawCounts reloaded;
    result->accept(reloaded);
    REQUIRE(reloaded.placements == 6);
    REQUIRE(reloaded.ordinary == 0);
}

// Run material/raster verification in a fresh context, like the page workers.
TEST_CASE("Chonk scene conversion renders ordinary geometry through pages", "[chonk][chonk-pages][gpu]")
{
#ifdef _WIN32
    const auto command = "\"\"" + osgEarth::Tests::executablePath + "\" \"[.chonk-scene-worker]\"\"";
#else
    std::string quoted = "'";
    for (char c : osgEarth::Tests::executablePath) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    const auto command = quoted + "' '[.chonk-scene-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
}

// Ordinary one/two-sided glTF-style material state must match packed pixels,
// and leave no ordinary drawable issuing separate glDrawElements calls.
TEST_CASE("Chonk scene rendering worker", "[.chonk-scene-worker]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("NVGL unavailable"); return; }
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    ChonkTest::UniqueScene scene(16, 1);
    installSceneMaterials(scene);
    ChonkTest::materialShader(renderer);
    renderer.viewer.getCamera()->setViewMatrixAsLookAt(
        osg::Vec3d(0,0,100), osg::Vec3d(), osg::Vec3d(0,1,0));
    auto converted = ChonkFactory::convertScene(scene.root, renderer.factory);
    ChonkTest::DrawCounts counts;
    converted->accept(counts);
    REQUIRE(counts.ordinary == 0);
    REQUIRE(counts.chonks == 2);
    REQUIRE(counts.placements == 16);
    renderer.setScene(scene.root); renderer.frame(); renderer.frame();
    auto reference = renderer.pixels();
    renderer.setScene(converted); renderer.frame(); renderer.frame();
    REQUIRE(ChonkTest::differentPixels(*reference, *renderer.pixels()) <= 30);
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Optional real-data regression: detail cells pack ordinary draws, while low-LOD
// singleton tiles retain their original graph. Compare without mutating inputs.
TEST_CASE("Prestige model conversion selects useful batches", "[.prestige-unique]")
{
    const char* path = std::getenv("OSGEARTH_PRESTIGE_TEST_TILE");
    REQUIRE(path != nullptr);
    auto source = osgDB::readRefNodeFile(path);
    REQUIRE(source);
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    auto factory = std::make_shared<ChonkFactory>(arena);
    auto old = ChonkFactory::convertExternalInstances(source, factory);
    auto packed = ChonkFactory::convertScene(source, factory);
    ChonkTest::DrawCounts before, after;
    old->accept(before); packed->accept(after);
    std::cout << "Prestige ordinary drawables: " << before.ordinary << " -> " << after.ordinary
        << "; Chonk drawables: " << before.chonks << " -> " << after.chonks
        << "; mesh batches: " << before.batches << " -> " << after.batches << std::endl;
    REQUIRE(before.ordinary > 0);
    if (before.ordinaryDraws == 1 && before.chonks == 0)
    {
        REQUIRE(after.ordinary == 1);
        REQUIRE(after.chonks == 0);
        REQUIRE(packed == source);
    }
    else
    {
        REQUIRE(after.ordinary < before.ordinary);
        REQUIRE(after.chonks > 0);
    }
    REQUIRE(packed->getBound().valid());
}
