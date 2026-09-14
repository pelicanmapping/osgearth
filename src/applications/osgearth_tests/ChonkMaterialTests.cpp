/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include "ChonkMaterialTestUtils.h"
#include <future>
#include <set>
#include <stdexcept>

using namespace osgEarth;
namespace osgEarth { namespace Tests { extern std::string executablePath; } }

namespace
{
    const MaterialArena::Indices emptyTextures = {{-1, -1, -1, -1, -1}};

    // Compare 256x256 renderings with small channel/rasterization tolerances;
    // require a visible reference image so an empty render cannot pass.
    void compareImages(osg::Image* actual, osg::Image* expected)
    {
        unsigned different = 0, nonzero = 0;
        for (unsigned i = 0; i < 256u*256u*4u; ++i)
        {
            different += std::abs(int(actual->data()[i]) - int(expected->data()[i])) > 3;
            if (i%4 != 3) nonzero += expected->data()[i] != 0;
        }
        INFO("Different channels: " << different);
        REQUIRE(nonzero > 1000);
        REQUIRE(different <= 100);
        REQUIRE(glGetError() == GL_NO_ERROR);
    }

    // Read back one record from the currently bound material SSBO for validation.
    // The caller makes state's context current; preserve its generic buffer binding.
    MaterialArena::GPU readMaterial(osg::State& state, GLushort index)
    {
        GLint buffer = 0;
        auto* gl = state.get<osg::GLExtensions>();
        using GetIntegerIndexed = void (GL_APIENTRY*)(GLenum, GLuint, GLint*);
        auto getIntegerIndexed = reinterpret_cast<GetIntegerIndexed>(osg::getGLExtensionFuncPtr("glGetIntegeri_v"));
        REQUIRE(getIntegerIndexed);
        getIntegerIndexed(GL_SHADER_STORAGE_BUFFER_BINDING, MaterialArena::BINDING_POINT, &buffer);
        REQUIRE(buffer != 0);
        GLint previous = 0;
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &previous);
        gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        MaterialArena::GPU material;
        gl->glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, sizeof(material)*index, sizeof(material), &material);
        gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, previous);
        REQUIRE(glGetError() == GL_NO_ERROR);
        return material;
    }

    // Validate all five handles against this context's texture objects, including
    // residency, absent maps, and preservation of the legacy extended IDs.
    void checkHandles(TextureArena& textures, osg::State& state, const ChonkMaterial::Ptr& material)
    {
        auto gpu = readMaterial(state, material->index);
        const GLuint64 handles[] = {gpu.albedo, gpu.normal, gpu.pbr, gpu.material1, gpu.material2};
        for (unsigned i = 0; i < 5; ++i)
        {
            auto texture = material->textures[i] < 0 ? Texture::Ptr() : textures.find(unsigned(material->textures[i]));
            auto object = texture ? texture->getGLObject(state) : GLTexture::Ptr();
            REQUIRE(handles[i] == (object ? object->handle(state) : GLuint64(0)));
            if (object) REQUIRE(object->isResident(state));
        }
        REQUIRE(gpu.extended[0] == material->extended.x());
        REQUIRE(gpu.extended[1] == material->extended.y());
    }

    // Validate a fixture material using the renderer's current context and arena.
    void checkHandles(ChonkTest::Renderer& renderer, const ChonkMaterial::Ptr& material)
    {
        checkHandles(*renderer.textures, *renderer.context->getState(), material);
    }
}

// Exercise ownership across concurrent acquisitions, factory reuse, and texture
// auto-release so deduplication cannot silently recycle a still-referenced slot.
TEST_CASE("Material IDs deduplicate across factories and retain textures", "[chonk][material]")
{
    osg::ref_ptr<TextureArena> textures = new TextureArena();
    textures->setAutoRelease(true);
    auto* arena = textures->getMaterialArena();
    auto texture = Texture::create(ChonkTest::solidTexture(osg::Vec4(1,0,0,1)));
    const int slot = textures->add(texture);
    const MaterialArena::Indices indices = {{slot, -1, -1, -1, -1}};
    auto material = arena->getOrCreate(*textures, indices, osg::Vec2i(-1,-1));
    const auto id = material->index;
    texture.reset();
    textures->flush();
    REQUIRE(textures->find(unsigned(slot)));
    REQUIRE(arena->getOrCreate(*textures, indices, osg::Vec2i(-1,-1)) == material);
    std::vector<std::future<ChonkMaterial::Ptr>> futures;
    for (unsigned i = 0; i < 8; ++i)
        futures.emplace_back(std::async(std::launch::async, [&] {
            return arena->getOrCreate(*textures, indices, osg::Vec2i(-1,-1));
        }));
    for (auto& future : futures) REQUIRE(future.get() == material);
    REQUIRE(arena->size() == 1);
    material.reset();
    REQUIRE(arena->size() == 0);
    REQUIRE_FALSE(arena->find(id));
    textures->flush();
    REQUIRE_FALSE(textures->find(unsigned(slot)));
    auto replacement = arena->getOrCreate(*textures, emptyTextures, osg::Vec2i(-1,-1));
    REQUIRE(replacement->index == id);

    ChonkFactory first(textures), second(textures);
    auto geometry = ChonkTest::mesh(16);
    auto a = first.getOrCreateChonk(geometry), b = second.getOrCreateChonk(geometry);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a->_materials.size() == 1);
    REQUIRE(a->_materials.front() == b->_materials.front());
    REQUIRE(sizeof(Chonk::VertexGPU) == 52);
    for (const auto& vertex : a->_vbo_store) REQUIRE(vertex.material_index == replacement->index);
}

// Fill the complete 16-bit ID space and verify that conversion fails atomically,
// then succeeds in reusing an ID after its final owner is released.
TEST_CASE("Material ID exhaustion is bounded and failed Chonk appends are atomic", "[chonk][material]")
{
    osg::ref_ptr<TextureArena> textures = new TextureArena();
    ChonkFactory factory(textures);
    auto chonk = factory.getOrCreateChonk(ChonkTest::mesh());
    REQUIRE(chonk);
    auto* arena = textures->getMaterialArena();
    std::vector<ChonkMaterial::Ptr> owners;
    std::set<GLushort> ids = {chonk->_materials.front()->index};
    for (unsigned i = 1; i < MaterialArena::CAPACITY; ++i)
    {
        owners.push_back(arena->getOrCreate(*textures, emptyTextures, osg::Vec2i(i,-1)));
        ids.insert(owners.back()->index);
    }
    REQUIRE(ids.size() == MaterialArena::CAPACITY);
    REQUIRE(*ids.rbegin() == 65535);
    REQUIRE_THROWS_AS(arena->getOrCreate(*textures, emptyTextures, osg::Vec2i(65536,-1)), std::length_error);
    osg::ref_ptr<osg::Group> source = new osg::Group();
    source->addChild(ChonkTest::mesh()); // successfully appended before the failing geometry
    auto textured = ChonkTest::mesh();
    textured->getOrCreateStateSet()->setTextureAttribute(0, ChonkTest::solidTexture(osg::Vec4(1,1,1,1)));
    source->addChild(textured);
    const auto vertices = chonk->_vbo_store.size(), elements = chonk->_ebo_store.size(), lods = chonk->_lods.size();
    REQUIRE_FALSE(chonk->add(source, factory));
    REQUIRE(chonk->_vbo_store.size() == vertices);
    REQUIRE(chonk->_ebo_store.size() == elements);
    REQUIRE(chonk->_lods.size() == lods);
    REQUIRE(chonk->_materials.size() == 1);
    REQUIRE_FALSE(factory.getOrCreateChonk(source));
    osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable();
    drawable->add(chonk);
    REQUIRE_FALSE(drawable->add(source, factory));
    REQUIRE(drawable->getNumInstances() == 1);
    const auto released = owners.back()->index;
    owners.pop_back();
    auto replacement = arena->getOrCreate(*textures, emptyTextures, osg::Vec2i(65536,-1));
    REQUIRE(replacement->index == released);
}

// Legacy per-vertex IDs are shader data, not TextureArena indices; verify their
// material variants preserve the values without inventing texture references.
TEST_CASE("Chonk preserves per-vertex extended material IDs", "[chonk][material]")
{
    osg::ref_ptr<TextureArena> textures = new TextureArena();
    ChonkFactory factory(textures);
    auto geometry = ChonkTest::mesh();
    auto extended = new osg::ShortArray();
    for (GLshort id : {11,11,27,27}) extended->push_back(id);
    geometry->setVertexAttribArray(Chonk::MATERIAL_VERTEX_SLOT, extended, osg::Array::BIND_PER_VERTEX);
    auto chonk = factory.getOrCreateChonk(geometry);
    REQUIRE(chonk);
    for (unsigned i = 0; i < 4; ++i)
    {
        auto material = textures->getMaterialArena()->find(chonk->_vbo_store[i].material_index);
        REQUIRE(material);
        REQUIRE(material->extended == osg::Vec2i((*extended)[i],-1));
        REQUIRE(material->textures == emptyTextures);
    }
    REQUIRE(chonk->_vbo_store[0].material_index == chonk->_vbo_store[1].material_index);
    REQUIRE(chonk->_vbo_store[0].material_index != chonk->_vbo_store[2].material_index);
}

// Run the GPU checks in an isolated process to avoid cached Chonk render-bin
// resources referring to context IDs reused by earlier viewer tests.
TEST_CASE("Chonk material rendering and handle refresh", "[chonk][material][gpu]")
{
    // Chonk's render bin keeps GL resources; isolate from other viewer tests.
    const auto& executable = osgEarth::Tests::executablePath;
#ifdef _WIN32
    const std::string command = "\"\"" + executable + "\" \"[.chonk-material-render-worker]\"\"";
#else
    std::string quoted = "'";
    for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    const std::string command = quoted + "' '[.chonk-material-render-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
}

// Compare rendered output and inspect records through texture recreation,
// paging, and multiple contexts; invoked by the public test's worker process.
TEST_CASE("Chonk material GPU validation", "[.chonk-material-render-worker]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Requires NVGL and OSG_GL_CONTEXT_VERSION=4.6");
        return;
    }
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    ChonkTest::materialShader(renderer);
    auto source = ChonkTest::materialScene(8);
    auto chonk = renderer.factory->getOrCreateChonk(source);
    REQUIRE(chonk);
    REQUIRE(chonk->_materials.size() == 16);
    osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable();
    drawable->add(chonk);
    ShaderGenerator().run(source);
    renderer.setScene(source);
    renderer.frame(); renderer.frame();
    auto reference = renderer.pixels();
    renderer.setScene(drawable);
    for (bool cull : {false, true})
    {
        drawable->setUseGPUCulling(cull);
        renderer.frame(); renderer.frame();
        compareImages(renderer.pixels(), reference);
        for (const auto& material : chonk->_materials) checkHandles(renderer, material);
    }

    // Recreate texture objects while keeping material IDs stable.
    renderer.textures->setMaxTextureSize(4);
    renderer.frame(); renderer.frame();
    compareImages(renderer.pixels(), reference);
    for (const auto& material : chonk->_materials) checkHandles(renderer, material);

    // Explicitly sample both extended handles, independently of their IDs.
    auto* vp = VirtualProgram::getOrCreate(renderer.root->getOrCreateStateSet());
    vp->setFunction("material_test_values", R"(
        flat in uint64_t oe_material1_tex;
        flat in uint64_t oe_material2_tex;
        // Sample both extended handles directly, bypassing their legacy IDs.
        void material_test_values(inout vec4 c) {
            c = .5 * (texture(sampler2D(oe_material1_tex), vec2(.5)) +
                      texture(sampler2D(oe_material2_tex), vec2(.5)));
        })", VirtualProgram::LOCATION_FRAGMENT_LIGHTING);
    renderer.frame(); renderer.frame();
    auto pixel = renderer.pixels()->getColor(128,128);
    // TextureArena compresses these images; allow BC1 endpoint quantization.
    REQUIRE(std::abs(pixel.r() - .25f) <= 1/31.0f);
    REQUIRE(std::abs(pixel.g() - .5f) <= 1/31.0f);
    REQUIRE(std::abs(pixel.b() - .75f) <= 1/31.0f);
    REQUIRE(glGetError() == GL_NO_ERROR);
    vp->removeShader("material_test_values");
    ChonkTest::materialShader(renderer);

    renderer.textures->setAutoRelease(true);
    renderer.textures->setAutoPaging(true);
    renderer.textures->setRevision(10);
    renderer.textures->flush();
    renderer.frame(); renderer.frame();
    for (const auto& material : chonk->_materials)
    {
        auto gpu = readMaterial(*renderer.context->getState(), material->index);
        REQUIRE(gpu.albedo == 0);
        REQUIRE(gpu.normal == 0);
        REQUIRE(gpu.pbr == 0);
        REQUIRE(gpu.material1 == 0);
        REQUIRE(gpu.material2 == 0);
        for (int index : material->textures)
            renderer.textures->add(renderer.textures->find(unsigned(index)));
    }
    renderer.frame(); renderer.frame();
    compareImages(renderer.pixels(), reference);
    for (const auto& material : chonk->_materials) checkHandles(renderer, material);

    // Shared contexts reuse the table but need their own handle residency.
    // Independent contexts must receive handles from their own texture objects.
    std::vector<osg::ref_ptr<osg::GraphicsContext>> contexts;
    for (bool share : {true, false})
    {
        auto traits = new osg::GraphicsContext::Traits(osg::DisplaySettings::instance());
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = traits->height = 16;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        if (share) traits->sharedContext = renderer.context;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        REQUIRE(context);
        REQUIRE(context->realize());
        REQUIRE(context->makeCurrent());
        contexts.push_back(context);
        auto& state = *context->getState();
        auto frame = new osg::FrameStamp();
        frame->setFrameNumber(1000);
        state.setFrameStamp(frame);
        renderer.textures->apply(state);
        for (const auto& material : chonk->_materials)
            checkHandles(*renderer.textures, state, material);
    }
    REQUIRE(renderer.context->makeCurrent());

    // A material with no texture objects still needs a valid SSBO record.
    auto plain = renderer.factory->getOrCreateChonk(ChonkTest::mesh());
    osg::ref_ptr<ChonkDrawable> plainDrawable = new ChonkDrawable();
    plainDrawable->add(plain);
    renderer.setScene(plainDrawable);
    renderer.frame(); renderer.frame();
    checkHandles(renderer, plain->_materials.front());
}
