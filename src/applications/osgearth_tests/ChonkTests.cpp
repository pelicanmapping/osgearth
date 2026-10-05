/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include "ChonkTestUtils.h"
#include <osgEarth/ShaderLoader>
#include <osgEarth/LogarithmicDepthBuffer>
#include <osg/Texture3D>
#include <osg/Multisample>
#include <osgDB/FileNameUtils>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <future>
#include <set>

using namespace osgEarth;
namespace osgEarth { namespace Tests { extern std::string executablePath; } }

//! Baked normals must survive atlas scaling, instance rotation, and viewing either face without a second leaf flip.
TEST_CASE("Chonk baked impostor normals retain their source frame", "[chonk][gpu][chonk-baked-normals]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Baked normal test needs NVGL");
        return;
    }
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    auto geometry = ChonkTest::mesh();
    auto* uv = dynamic_cast<osg::Vec2Array*>(geometry->getTexCoordArray(0));
    for (auto& p : *uv) p.set(0.05f+p.x()*0.15f,0.55f+p.y()*0.4f);
    auto technique = new osg::UByteArray();
    technique->push_back(Chonk::NORMAL_TECHNIQUE_BAKED);
    geometry->setVertexAttribArray(6,technique,osg::Array::BIND_OVERALL);
    osg::ref_ptr<osg::Image> normal = new osg::Image();
    normal->allocateImage(16,8,1,GL_RGBA,GL_UNSIGNED_BYTE);
    for (unsigned y=0; y<8; ++y)
        for (unsigned x=0; x<16; ++x)
        {
            auto* p = normal->data(x,y);
            p[0] = y < 4 ? 128 : 204; p[1] = y < 4 ? 204 : 128;
            p[2] = y < 4 ? 26 : 230; p[3] = 255;
        }
    auto texture = new osg::Texture2D(normal);
    texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::NEAREST);
    texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::NEAREST);
    geometry->getOrCreateStateSet()->setTextureAttributeAndModes(1,texture);
    auto model = Chonk::create();
    REQUIRE(model->add(geometry,0,FLT_MAX,*renderer.factory));
    auto* state = renderer.root->getOrCreateStateSet();
    state->setMode(GL_CULL_FACE,osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    // A view-stage hook requests the same model-to-view normal transform used by the scene lighting shaders.
    VirtualProgram::getOrCreate(state)->setFunction("baked_normal_view",
        "void baked_normal_view(inout vec4 vertex) { }", VirtualProgram::LOCATION_VERTEX_VIEW);
    VirtualProgram::getOrCreate(state)->setFunction("baked_normal_test", R"glsl(
        in vec3 vp_Normal;
        void baked_normal_test(inout vec4 color) { color = vec4(normalize(vp_Normal)*0.5+0.5,1); }
    )glsl", VirtualProgram::LOCATION_FRAGMENT_LIGHTING,1.0f);
    for (float rotation : {0.0f,0.7f})
        for (int side : {1,-1})
        {
            INFO("rotation=" << rotation << " side=" << side);
            auto* drawable = new ChonkDrawable();
            drawable->setUseGPUCulling(false);
            drawable->setBirthday(-10);
            drawable->add(model,osg::Matrixf::scale(2,0.5f,1)*osg::Matrixf::rotate(rotation,0,0,1));
            renderer.setScene(drawable);
            auto* camera = renderer.viewer.getCamera();
            camera->setViewMatrixAsLookAt(osg::Vec3d(0,0,side*5),osg::Vec3d(),osg::Vec3d(0,1,0));
            renderer.frame(); renderer.frame();
            const osg::Vec3f source = side > 0 ? osg::Vec3f(0.6f,0,0.8f) : osg::Vec3f(0,0.6f,-0.8f);
            auto expected = osg::Matrixf::transform3x3(source,osg::Matrixf::rotate(rotation,0,0,1)*
                osg::Matrixf(camera->getViewMatrix()));
            auto pixels = renderer.pixels();
            const auto* actual = pixels->data(128,128);
            for (unsigned c=0; c<3; ++c)
                CHECK(std::abs(int(actual[c])-int((expected[c]*0.5f+0.5f)*255.0f)) <= 4);
        }
    CHECK(glGetError() == GL_NO_ERROR);
}

//! Exercise A2C and its single-sample fallback in separate processes so each framebuffer owns its GL resources.
TEST_CASE("Chonk alpha coverage follows the active framebuffer", "[chonk][gpu][chonk-alpha-coverage]")
{
    for (const std::string tag : {"[.chonk-alpha-single-worker]", "[.chonk-alpha-msaa-worker]"})
    {
#ifdef _WIN32
        const auto command = "\"\"" + osgEarth::Tests::executablePath + "\" \"" + tag + "\"\"";
#else
        std::string quoted = "'";
        for (char c : osgEarth::Tests::executablePath) quoted += c == '\'' ? "'\\''" : std::string(1, c);
        const auto command = quoted + "' '" + tag + "'";
#endif
        INFO(tag);
        REQUIRE(std::system(command.c_str()) == 0);
    }
}

//! Render transparent, quarter, half, and opaque coverage; disabled MSAA must never expose carrier rectangles.
static void validateAlphaCoverage(unsigned samples)
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Chonk coverage test needs NVGL");
        return;
    }
    osg::DisplaySettings::instance()->setNumMultiSamples(samples);
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    GLint actual = 0;
    glGetIntegerv(GL_SAMPLES_ARB, &actual);
    REQUIRE(actual == int(samples));
    auto* state = renderer.root->getOrCreateStateSet();
    state->setDefine("OE_CHONK_ALPHA_TO_COVERAGE");
    state->setMode(GL_MULTISAMPLE_ARB, osg::StateAttribute::ON);
    state->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB, osg::StateAttribute::ON);
    state->setMode(GL_BLEND, osg::StateAttribute::OFF);
    renderer.viewer.getCamera()->setViewMatrixAsLookAt(
        osg::Vec3d(0,0,5), osg::Vec3d(), osg::Vec3d(0,1,0));
    auto geometry = ChonkTest::mesh();
    osg::ref_ptr<osg::Image> image = new osg::Image();
    image->allocateImage(8, 8, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    const unsigned char alpha[] = {0,64,128,255};
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
        {
            auto* pixel = image->data(x,y);
            pixel[0] = pixel[1] = pixel[2] = 255;
            pixel[3] = alpha[x/2];
        }
    osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
    texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
    texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
    geometry->getOrCreateStateSet()->setTextureAttribute(0, texture);
    auto model = renderer.factory->getOrCreateChonk(geometry);
    REQUIRE(model);
    osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable();
    drawable->add(model, osg::Matrixf::identity());
    drawable->setUseGPUCulling(false);
    renderer.setScene(drawable);
    renderer.frame(); renderer.frame();
    auto pixels = renderer.pixels();
    CHECK(pixels->data(32,128)[2] == 0);
    CHECK(pixels->data(224,128)[2] > 240);
    if (samples > 1)
    {
        // WGL may resolve a linear or sRGB default framebuffer; either must retain ordered fractional coverage.
        const int quarter = pixels->data(96,128)[2], half = pixels->data(160,128)[2];
        CHECK(quarter > 30);
        CHECK(quarter < 170);
        CHECK(half > quarter + 30);
        CHECK(half < 230);
    }
    else
    {
        CHECK(pixels->data(96,128)[2] == 0);
        CHECK(pixels->data(160,128)[2] > 240);
    }
    CHECK(glGetError() == GL_NO_ERROR);
}

//! A2C requested on a non-MSAA target must retain ordinary alpha-tested cutouts.
TEST_CASE("Chonk alpha single-sample worker", "[.chonk-alpha-single-worker]") { validateAlphaCoverage(0); }

//! Four samples retain fractional foliage coverage while preserving fully transparent texels.
TEST_CASE("Chonk alpha multisample worker", "[.chonk-alpha-msaa-worker]") { validateAlphaCoverage(4); }

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
            Chonk::DrawCommands commands; // GPU culling: one list of `stride` commands per List
            unsigned stride = 0;
            bool sourceUnchanged;
        };

        // Read completed GPU results with this drawable's context current. Safe between frames: see
        // readBuffer. Publishes shader writes for the copies.
        Snapshot snapshot(osg::State& state) const
        {
            auto& objects = GLObjects::get(_globjects, state);
            state.get<osg::GLExtensions>()->glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            Snapshot result;
            result.sources.resize(objects._all_instances.size());
            result.visible.resize(objects._instanceOutputBuf->size() / sizeof(VisibleInstance));
            result.stride = unsigned(objects._commands.size());
            result.commands.resize(result.stride * (_gpucull ? unsigned(NUM_LISTS) : 1u));
            readBuffer(*objects._instanceInputBuf, result.sources.size() * sizeof(Instance), result.sources.data());
            readBuffer(*objects._instanceOutputBuf, result.visible.size() * sizeof(VisibleInstance),
                result.visible.data());
            readBuffer(*objects._commandBuf, result.commands.size() * sizeof(Chonk::DrawCommand),
                result.commands.data());
            result.sourceUnchanged = std::memcmp(result.sources.data(), objects._all_instances.data(),
                result.sources.size() * sizeof(Instance)) == 0;
            return result;
        }

        // Copies the start of a buffer to host memory through a temporary buffer. Reading a live
        // buffer directly lets the NVIDIA driver move it to host memory, where the culler's atomic
        // command counts fault the GPU on the next frame. Call with the owning context current.
        static void readBuffer(const GLBuffer& source, std::size_t bytes, void* data)
        {
            static void (GL_APIENTRY* createBuffers)(GLsizei, GLuint*) = nullptr;
            static void (GL_APIENTRY* namedBufferData)(GLuint, GLsizeiptr, const void*, GLenum) = nullptr;
            static void (GL_APIENTRY* copyNamedBufferSubData)(GLuint, GLuint, GLintptr, GLintptr, GLsizeiptr) = nullptr;
            static void (GL_APIENTRY* getNamedBufferSubData)(GLuint, GLintptr, GLsizeiptr, void*) = nullptr;
            static void (GL_APIENTRY* deleteBuffers)(GLsizei, const GLuint*) = nullptr;
            if (!createBuffers)
            {
                osg::setGLExtensionFuncPtr(namedBufferData, "glNamedBufferData");
                osg::setGLExtensionFuncPtr(copyNamedBufferSubData, "glCopyNamedBufferSubData");
                osg::setGLExtensionFuncPtr(getNamedBufferSubData, "glGetNamedBufferSubData");
                osg::setGLExtensionFuncPtr(deleteBuffers, "glDeleteBuffers");
                osg::setGLExtensionFuncPtr(createBuffers, "glCreateBuffers");
            }
            if (bytes == 0) return;
            GLuint staging = 0;
            createBuffers(1, &staging);
            namedBufferData(staging, GLsizeiptr(bytes), nullptr, GL_STREAM_READ);
            copyNamedBufferSubData(source.name(), staging, 0, 0, GLsizeiptr(bytes));
            getNamedBufferSubData(staging, 0, GLsizeiptr(bytes), data);
            deleteBuffers(1, &staging);
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

//! Density LOD must preserve source records, restore the same survivors, and dither fades without losing alpha tests.
TEST_CASE("Chonk density LOD preserves stable survivors and dithered coverage", "[chonk][gpu]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Chonk density LOD test needs NVGL and OSG_GL_CONTEXT_VERSION=4.6");
        return;
    }
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    auto state = renderer.root->getOrCreateStateSet();
    state->setDefine("OE_CHONK_DENSITY_LOD");
    state->setDefine("OE_CHONK_DITHER_FADE");
    auto policy = new osg::Uniform("oe_chonk_density_lod", osg::Vec3f(100,200,0.25f));
    state->addUniform(policy);
    auto asset = renderer.factory->getOrCreateChonk(ChonkTest::mesh(), 0.0f);
    REQUIRE(asset);
    osg::ref_ptr<InspectableDrawable> drawable = new InspectableDrawable();
    for (unsigned i = 0; i < 4; ++i)
        drawable->add(asset, osg::Matrixf::translate(float(i%2)*16-8, float(i/2)*16-8, 0),
            osg::Vec2f((float(i)+0.5f)/4.0f,0));
    renderer.setScene(drawable);
    std::vector<std::set<float>> survivors;
    for (const osg::Vec3f setting : {osg::Vec3f(100,200,0.25f), osg::Vec3f(10,80,0.25f),
        osg::Vec3f(10,80,0), osg::Vec3f(100,200,0.25f)})
    {
        policy->set(setting);
        for (unsigned frame = 0; frame < 4; ++frame) renderer.frame();
        auto snapshot = drawable->snapshot(*renderer.context->getState());
        REQUIRE(snapshot.sourceUnchanged);
        std::set<float> ranks;
        for (const auto& command : snapshot.commands)
        for (unsigned i = 0; i < command.cmd.instanceCount; ++i)
        {
            const auto& visible = snapshot.visible[command.cmd.baseInstance+i];
            REQUIRE(visible.sourceIndex < snapshot.sources.size());
            ranks.insert(snapshot.sources[visible.sourceIndex].uv.x());
        }
        survivors.push_back(ranks);
    }
    CHECK(survivors[0].size() == 4);
    CHECK(survivors[1] == std::set<float>{0.125f});
    CHECK(survivors[2].empty());
    CHECK(survivors[3] == survivors[0]);

    // Different population policies must survive sharing a renderer and arena. A common Chonk bin would
    // incorrectly apply the first population's state to both, so exercise separate bins under one parent.
    osg::ref_ptr<osg::Group> populations = new osg::Group();
    populations->getOrCreateStateSet()->setRenderBinDetails(3, "RenderBin");
    std::vector<osg::ref_ptr<InspectableDrawable>> populationDrawables;
    for (unsigned population = 0; population < 2; ++population)
    {
        auto group = new osg::Group();
        group->getOrCreateStateSet()->addUniform(new osg::Uniform("oe_chonk_density_lod",
            osg::Vec3f(10,80,population == 0 ? 0.25f : 0.75f)));
        osg::ref_ptr<InspectableDrawable> instances = new InspectableDrawable();
        instances->setRenderBinNumber(1 + int(population));
        for (unsigned i = 0; i < 4; ++i)
            instances->add(asset, osg::Matrixf::translate(float(i%2)*16-8 + float(population)*5,
                float(i/2)*16-8, 0), osg::Vec2f((float(i)+0.5f)/4.0f,0));
        group->addChild(instances);
        populations->addChild(group);
        populationDrawables.push_back(instances);
    }
    renderer.setScene(populations);
    for (unsigned frame = 0; frame < 4; ++frame) renderer.frame();
    for (unsigned population = 0; population < 2; ++population)
    {
        const auto snapshot = populationDrawables[population]->snapshot(*renderer.context->getState());
        REQUIRE(snapshot.sourceUnchanged);
        std::set<float> ranks;
        for (const auto& command : snapshot.commands)
        for (unsigned i = 0; i < command.cmd.instanceCount; ++i)
        {
            const auto& visible = snapshot.visible[command.cmd.baseInstance+i];
            ranks.insert(snapshot.sources[visible.sourceIndex].uv.x());
        }
        CHECK(ranks.size() == (population == 0 ? 1u : 3u));
    }

    policy->set(osg::Vec3f(0,0,1));
    osg::ref_ptr<InspectableDrawable> fading = new InspectableDrawable();
    // Isolate range coverage from the birthday ramp, which uses wall-clock reference time, not viewer.frame(time).
    fading->setBirthday(-10.0);
    fading->add(asset, osg::Matrixf::scale(4,4,1));
    renderer.setScene(fading);
    std::vector<unsigned> coverage;
    for (float farRange : {0.0f, 400.0f, 200.0f, 133.33333f})
    {
        fading->setFadeNearFar(0.0f, farRange);
        renderer.frame(); renderer.frame();
        const auto faded = fading->snapshot(*renderer.context->getState());
        for (const auto& command : faded.commands)
            for (unsigned i=0; i<command.cmd.instanceCount; ++i)
            {
                const float expected = farRange > 0.0f ? 1.0f-100.0f/farRange : 1.0f;
                CHECK(std::abs(faded.visible[command.cmd.baseInstance+i].fade-expected) < 1e-5f);
            }
        auto pixels = renderer.pixels();
        unsigned count = 0;
        for (unsigned i = 0; i < 256u*256u; ++i)
            if (pixels->data()[4*i+2] > 128) ++count;
        coverage.push_back(count);
    }
    REQUIRE(coverage[0] > 1000);
    CHECK(std::abs(double(coverage[1])/coverage[0] - 0.75) < 0.08);
    CHECK(std::abs(double(coverage[2])/coverage[0] - 0.50) < 0.08);
    CHECK(std::abs(double(coverage[3])/coverage[0] - 0.25) < 0.08);
    auto cutout = ChonkTest::mesh();
    static_cast<osg::Vec4Array*>(cutout->getColorArray())->front().a() = 0.25f;
    auto cutoutAsset = renderer.factory->getOrCreateChonk(cutout, 0.0f);
    REQUIRE(cutoutAsset);
    osg::ref_ptr<ChonkDrawable> transparent = new ChonkDrawable();
    transparent->add(cutoutAsset, osg::Matrixf::scale(4,4,1));
    renderer.setScene(transparent);
    renderer.frame(); renderer.frame();
    auto pixels = renderer.pixels();
    unsigned remaining = 0;
    for (unsigned i = 0; i < 256u*256u; ++i)
        if (pixels->data()[4*i+2] > 0) ++remaining;
    CHECK(remaining == 0);
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Keep adjacent LODs eligible when only their conservative sphere touches the near plane.
// The tilted quad is visible with conventional depth; log depth also keeps it visible before the original near plane.
// GPU readback distinguishes LOD rejection from alpha fading, and pixel checks catch a fully missing transition.
TEST_CASE("Chonk LOD transitions survive a near-plane intersection", "[chonk][gpu][chonk-lod-transition]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Chonk LOD test needs NVGL and OSG_GL_CONTEXT_VERSION=4.6");
        return;
    }
    unsigned depthMode = 0u;
    SECTION("Conventional depth") { }
    SECTION("Vertex logarithmic depth") { depthMode = 1u; }
    SECTION("Fragment logarithmic depth") { depthMode = 2u; }
    INFO("Depth mode " << depthMode);
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    auto* ss = renderer.root->getOrCreateStateSet();
    ss->getUniform("oe_sse")->set(1.0f);
    ss->addUniform(new osg::Uniform("oe_chonk_lod_transition_factor", 0.15f));
    ss->setDefine("OE_CHONK_DITHER_FADE");
    auto asset = renderer.factory->getOrCreateChonk(ChonkTest::mesh(), 20.0f);
    REQUIRE(asset);
    asset->_lods.push_back(asset->_lods.front());
    asset->_lods.back().far_pixel_scale = 0.0f;
    asset->_lods.back().near_pixel_scale = 20.0f;
    osg::ref_ptr<InspectableDrawable> drawable = new InspectableDrawable();
    drawable->setBirthday(-10.0);
    drawable->add(asset, osg::Matrixf());
    renderer.setScene(drawable);
    auto* camera = renderer.viewer.getCamera();
    if (depthMode != 0u)
    {
        Util::LogarithmicDepthBuffer depth;
        depth.setUseFragDepth(depthMode == 2u);
        depth.install(camera);
    }
    for (double range : {90.0, 100.0, 120.0})
    {
        camera->setViewMatrixAsLookAt(osg::Vec3d(0, -range/std::sqrt(2.0), range/std::sqrt(2.0)),
            osg::Vec3d(), osg::Vec3d(0, 0, 1));
        std::vector<double> nearPlanes = {1.0, range - 2.0};
        if (depthMode != 0u) nearPlanes.push_back(1000.0);
        for (double nearPlane : nearPlanes)
        {
            INFO("Range " << range << ", near plane " << nearPlane);
            camera->setProjectionMatrixAsPerspective(45.0, 1.0, nearPlane, 10000.0);
            for (unsigned frame = 0; frame < 4u; ++frame) renderer.frame();
            const auto snapshot = drawable->snapshot(*renderer.context->getState());
            unsigned coarse = 0u;
            for (const auto& command : snapshot.commands)
                for (unsigned i = 0u; i < command.cmd.instanceCount; ++i)
                    if (snapshot.visible[command.cmd.baseInstance + i].lod == 1u) ++coarse;
            CHECK(coarse == 1u);
            const auto pixels = renderer.pixels();
            unsigned covered = 0u;
            for (unsigned y = 0u; y < 256u; ++y)
                for (unsigned x = 0u; x < 256u; ++x)
                    if (pixels->data(x, y)[2] > 200u) ++covered;
            CHECK(covered > 0u);
        }
    }
    REQUIRE(glGetError() == GL_NO_ERROR);
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
                REQUIRE(unsigned(source.first_lod_cmd_index) + visible.lod == command % snapshot.stride);
                REQUIRE(actual.emplace(visible.sourceIndex, visible.lod).second);
                REQUIRE(visible.alphaCutoff == (drawable == first.get() ? 0.371234f : 0.612345f));
                if (cull && source.uv.x() < 0.3f && visible.lod == 0)
                {
                    REQUIRE(visible.fade > 0.1f);
                    REQUIRE(visible.fade < 1.0f);
                }
                else REQUIRE(visible.fade == 1.0f);
                if (cull)
                {
                    // These materials cannot fail the alpha test, so only fading instances belong in
                    // the cutout list; a settled static view leaves the late occlusion lists empty.
                    const unsigned list = command / snapshot.stride;
                    REQUIRE(list <= 1u);
                    REQUIRE((list == 1u) == (visible.fade < 1.0f));
                }
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

//! The opt-in additive budget must affect actual GPU LOD/cull decisions, preserve sources, and remain population-local.
TEST_CASE("Chonk additive quality shares global SSE without population leakage", "[chonk][gpu]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Additive quality GPU test needs NVGL");
        return;
    }
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    auto model = Chonk::create();
    REQUIRE(model->add(ChonkTest::mesh(),32.0f,FLT_MAX,*renderer.factory));
    REQUIRE(model->add(ChonkTest::mesh(),4.0f,32.0f,*renderer.factory));
    osg::ref_ptr<InspectableDrawable> first = new InspectableDrawable(), second = new InspectableDrawable();
    osg::ref_ptr<osg::Uniform> offsets[2];
    auto scene = new osg::Group();
    unsigned index = 0;
    for (auto* drawable : {first.get(),second.get()})
    {
        drawable->setBirthday(-10);
        drawable->add(model,osg::Matrixf::translate(index ? 5.0f : -5.0f,0,0));
        auto* branch = new osg::Group();
        auto* ss = branch->getOrCreateStateSet();
        ss->setDefine("OE_CHONK_SSE_ADJUST");
        offsets[index] = new osg::Uniform("oe_chonk_sse_adjust",osg::Vec2f(0,1.0f/25.0f));
        ss->addUniform(offsets[index]);
        branch->addChild(drawable); scene->addChild(branch);
        ++index;
    }
    renderer.setScene(scene);
    renderer.viewer.getCamera()->setViewMatrixAsLookAt(osg::Vec3d(0,0,40),osg::Vec3d(),osg::Vec3d(0,1,0));
    auto* global = renderer.root->getStateSet()->getUniform("oe_sse");
    //! Reads emitted LODs after a completed frame; source buffers must stay byte-for-byte unchanged.
    auto lods = [&](InspectableDrawable* drawable)
    {
        const auto snapshot = drawable->snapshot(*renderer.context->getState());
        REQUIRE(snapshot.sourceUnchanged);
        std::set<unsigned> result;
        for (const auto& command : snapshot.commands)
            for (unsigned j=0; j<command.cmd.instanceCount; ++j)
                result.insert(snapshot.visible[command.cmd.baseInstance+j].lod);
        return result;
    };
    global->set(25.0f);
    renderer.frame(); renderer.frame();
    CHECK(lods(first) == std::set<unsigned>{0u});
    CHECK(lods(second) == std::set<unsigned>{0u});
    offsets[0]->set(osg::Vec2f(25,1.0f/25.0f));
    renderer.frame(); renderer.frame();
    CHECK(lods(first) == std::set<unsigned>{1u});
    CHECK(lods(second) == std::set<unsigned>{0u});
    // Same sum through the global control must produce the same representation, without applying SSE twice.
    offsets[0]->set(osg::Vec2f(0,1.0f/25.0f)); global->set(50.0f);
    renderer.frame(); renderer.frame();
    CHECK(lods(first) == std::set<unsigned>{1u});
    CHECK(lods(second) == std::set<unsigned>{1u});
    global->set(1000.0f);
    renderer.frame(); renderer.frame();
    CHECK(lods(first).empty()); CHECK(lods(second).empty());
    global->set(25.0f);
    renderer.frame(); renderer.frame();
    CHECK(lods(first) == std::set<unsigned>{0u});
    CHECK(lods(second) == std::set<unsigned>{0u});
    CHECK(glGetError() == GL_NO_ERROR);
}
