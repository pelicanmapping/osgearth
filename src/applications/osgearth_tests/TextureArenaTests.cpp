/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/Capabilities>
#include <osgEarth/TextureArena>
#include <osgEarth/TerrainTileModel>
#include <osg/GraphicsContext>
#include <osg/Texture2D>
#include <osgUtil/IncrementalCompileOperation>
#include <chrono>
#include <functional>
#include <future>

using namespace osgEarth;

namespace
{
    osg::ref_ptr<osg::GraphicsContext> createContext(osg::GraphicsContext* shared = nullptr)
    {
        auto traits = new osg::GraphicsContext::Traits(osg::DisplaySettings::instance());
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = traits->height = 16;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->sharedContext = shared;
        auto context = osg::GraphicsContext::createGraphicsContext(traits);
        REQUIRE(context != nullptr);
        REQUIRE(context->realize());
        return context;
    }

    // Pause handle creation at the exact point where compilation used to expose
    // a valid GL name with a zero bindless handle to the rendering thread.
    struct HandleCreationProbe
    {
        using GetHandle = GLuint64 (GL_APIENTRY*)(GLuint);
        static thread_local HandleCreationProbe* active;
        osg::GLExtensions* ext;
        GetHandle original;
        std::function<bool()> observe;
        std::future<bool> result;
        bool completedDuringCompilation = false;

        HandleCreationProbe(osg::State& state, std::function<bool()> observer) :
            ext(state.get<osg::GLExtensions>()),
            original(ext->glGetTextureHandle),
            observe(std::move(observer))
        {
            active = this;
            ext->glGetTextureHandle = getHandle;
        }

        ~HandleCreationProbe()
        {
            if (result.valid()) result.wait();
            ext->glGetTextureHandle = original;
            active = nullptr;
        }

        static GLuint64 GL_APIENTRY getHandle(GLuint name)
        {
            auto& probe = *active;
            std::promise<void> started;
            auto ready = started.get_future();
            probe.result = std::async(std::launch::async, [&probe, &started]() {
                started.set_value();
                return probe.observe();
            });
            ready.wait();
            probe.completedDuringCompilation =
                probe.result.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
            return probe.original(name);
        }
    };

    thread_local HandleCreationProbe* HandleCreationProbe::active = nullptr;

    osg::ref_ptr<osg::Texture2D> createTexture()
    {
        auto image = new osg::Image();
        image->allocateImage(2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        memset(image->data(), 255, image->getTotalSizeInBytes());
        auto texture = new osg::Texture2D(image);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        texture->setUnRefImageDataAfterApply(false);
        return texture;
    }

    struct RejectHandle
    {
        static thread_local unsigned calls;
        osg::GLExtensions* ext;
        HandleCreationProbe::GetHandle original;
        explicit RejectHandle(osg::State& state) :
            ext(state.get<osg::GLExtensions>()), original(ext->glGetTextureHandle)
        {
            calls = 0;
            ext->glGetTextureHandle = reject;
        }
        ~RejectHandle() { ext->glGetTextureHandle = original; }
        static GLuint64 GL_APIENTRY reject(GLuint) { ++calls; return 0; }
    };
    thread_local unsigned RejectHandle::calls = 0;
}

TEST_CASE("Texture residency waits for bindless compilation", "[texturearena][gpu]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Requires NVGL and OSG_GL_CONTEXT_VERSION=4.6");
        return;
    }

    auto compiler = createContext();
    auto renderer = createContext(compiler);
    REQUIRE(compiler->makeCurrent());
    auto& state = *compiler->getState();
    REQUIRE(state.getContextID() == renderer->getState()->getContextID());

    osg::ref_ptr<osg::Image> image = new osg::Image();
    image->allocateImage(2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    memset(image->data(), 255, image->getTotalSizeInBytes());
    osg::ref_ptr<osg::Texture2D> osgTexture = new osg::Texture2D(image);
    osgTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
    osgTexture->setUnRefImageDataAfterApply(false);

    SECTION("New texture storage") { }
    SECTION("Wrapped OSG texture")
    {
        osgTexture->apply(state);
        REQUIRE(osgTexture->getTextureObject(state.getContextID()) != nullptr);
    }

    auto texture = Texture::create(osgTexture);
    texture->compress() = false;
    {
        HandleCreationProbe probe(state, [&]() {
            if (!renderer->makeCurrent()) return false;
            auto& renderState = *renderer->getState();
            texture->makeResident(renderState, true);
            const bool resident = texture->isResident(renderState);
            const auto error = glGetError();
            renderer->releaseContext();
            return resident && error == GL_NO_ERROR;
        });

        REQUIRE(texture->compileGLObjects(state));
        REQUIRE(probe.result.valid());
        const bool resident = probe.result.get();
        CHECK_FALSE(probe.completedDuringCompilation);
        CHECK(resident);
    }
    REQUIRE(texture->isCompiled(state));
    REQUIRE(texture->isResident(state));
    REQUIRE(glGetError() == GL_NO_ERROR);

    texture->releaseGLObjects(&state);
    compiler->releaseContext();
}

TEST_CASE("Texture arena recovers from failed handle creation", "[texturearena][gpu]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Requires NVGL and OSG_GL_CONTEXT_VERSION=4.6");
        return;
    }
    auto context = createContext();
    REQUIRE(context->makeCurrent());
    auto& state = *context->getState();
    auto frame = new osg::FrameStamp();
    frame->setFrameNumber(1);
    state.setFrameStamp(frame);
    auto osgTexture = createTexture();

    SECTION("New texture") { }
    SECTION("Wrapped texture") { osgTexture->apply(state); }
    SECTION("Dynamic texture") { osgTexture->setDataVariance(osg::Object::DYNAMIC); }

    auto texture = Texture::create(osgTexture);
    texture->compress() = false;
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    REQUIRE(arena->add(texture) >= 0);
    {
        RejectHandle reject(state);
        arena->apply(state);
        CHECK_FALSE(texture->isCompiled(state));
        CHECK_FALSE(texture->isResident(state));
        CHECK(texture->needsCompile(state));
        CHECK(texture->getGLObject(state) == nullptr);
        CHECK(texture->hasImage());
        CHECK(RejectHandle::calls == 1);
        frame->setFrameNumber(2);
        arena->apply(state);
        CHECK(RejectHandle::calls == 2);
        frame->setFrameNumber(3);
        arena->apply(state);
        CHECK(RejectHandle::calls == 3);
    }
    // A static texture must remain queued so a transient failure can recover.
    frame->setFrameNumber(4);
    arena->apply(state);
    REQUIRE(texture->isCompiled(state));
    REQUIRE(texture->isResident(state));
    REQUIRE_FALSE(texture->needsCompile(state));
    REQUIRE(glGetError() == GL_NO_ERROR);
    arena = nullptr;
    texture->releaseGLObjects(nullptr, true);
    osgTexture->releaseGLObjects(&state);
    context->releaseContext();
}

TEST_CASE("Paged-out textures do not retry failed compilations", "[texturearena][gpu]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Requires NVGL and OSG_GL_CONTEXT_VERSION=4.6");
        return;
    }
    auto context = createContext();
    REQUIRE(context->makeCurrent());
    auto& state = *context->getState();
    auto frame = new osg::FrameStamp();
    frame->setFrameNumber(1);
    state.setFrameStamp(frame);
    auto texture = Texture::create(createTexture());
    texture->compress() = false;
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    arena->setAutoRelease(true);
    arena->setAutoPaging(true);
    const int slot = arena->add(texture);
    REQUIRE(slot >= 0);
    {
        RejectHandle reject(state);
        arena->apply(state);
        CHECK(RejectHandle::calls == 1);
    }
    arena->setRevision(1);
    arena->flush();
    REQUIRE(texture->dormant());
    frame->setFrameNumber(2);
    arena->apply(state);
    REQUIRE_FALSE(texture->isCompiled(state));
    REQUIRE(texture->getGLObject(state) == nullptr);
    REQUIRE(arena->add(texture) == slot);
    frame->setFrameNumber(3);
    arena->apply(state);
    REQUIRE(texture->isResident(state));
    REQUIRE(glGetError() == GL_NO_ERROR);
    arena = nullptr;
    texture->releaseGLObjects(nullptr, true);
    context->releaseContext();
}

TEST_CASE("Unallocated OSG texture objects are not treated as compiled", "[texturearena][gpu]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Requires NVGL and OSG_GL_CONTEXT_VERSION=4.6");
        return;
    }
    auto context = createContext();
    REQUIRE(context->makeCurrent());
    auto& state = *context->getState();
    auto osgTexture = createTexture();
    auto reserved = osgTexture->generateAndAssignTextureObject(state.getContextID(), GL_TEXTURE_2D);
    REQUIRE(reserved != nullptr);
    REQUIRE_FALSE(reserved->isAllocated());
    auto texture = Texture::create(osgTexture);
    texture->compress() = false;
    REQUIRE(texture->compileGLObjects(state));
    REQUIRE(texture->isResident(state));
    REQUIRE(glGetError() == GL_NO_ERROR);
    texture->releaseGLObjects(nullptr, true);
    osgTexture->releaseGLObjects(&state);
    context->releaseContext();
}

TEST_CASE("Canceled terrain compilation can be compiled later by an arena", "[texturearena][gpu]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Requires NVGL and OSG_GL_CONTEXT_VERSION=4.6");
        return;
    }
    auto context = createContext();
    REQUIRE(context->makeCurrent());
    auto& state = *context->getState();
    auto frame = new osg::FrameStamp();
    frame->setFrameNumber(1);
    state.setFrameStamp(frame);
    auto texture = Texture::create(createTexture());
    texture->compress() = false;
    osg::ref_ptr<TerrainTileModel> model = new TerrainTileModel(TileKey(), 0);
    model->elevation.texture = texture;
    osg::ref_ptr<osg::Object> token = new osg::DummyObject();
    auto pending = GLObjectsCompiler().collectState(nullptr);
    model->getStateToCompile(*pending, true, token);
    REQUIRE(pending->_textures.size() == 1);
    token = nullptr;
    (*pending->_textures.begin())->apply(state);
    REQUIRE_FALSE(texture->isCompiled(state));
    REQUIRE(texture->getGLObject(state) == nullptr);
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    REQUIRE(arena->add(texture) >= 0);
    arena->apply(state);
    REQUIRE(texture->isCompiled(state));
    REQUIRE(texture->isResident(state));
    REQUIRE(glGetError() == GL_NO_ERROR);
    arena = nullptr;
    texture->releaseGLObjects(nullptr, true);
    context->releaseContext();
}

TEST_CASE("Incomplete wrapped textures recover after their GL state is repaired", "[texturearena][gpu]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Requires NVGL and OSG_GL_CONTEXT_VERSION=4.6");
        return;
    }
    auto context = createContext();
    REQUIRE(context->makeCurrent());
    auto& state = *context->getState();
    auto osgTexture = createTexture();
    osgTexture->apply(state);
    REQUIRE(osgTexture->getTextureObject(state.getContextID())->isAllocated());
    // Only level zero exists. A mipmapped filter makes this mutable texture
    // incomplete, causing the real driver to reject the bindless handle.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    REQUIRE(glGetError() == GL_NO_ERROR);
    auto texture = Texture::create(osgTexture);
    texture->name() = "incomplete wrapped texture";
    REQUIRE_FALSE(texture->compileGLObjects(state));
    REQUIRE_FALSE(texture->isCompiled(state));
    REQUIRE(texture->getGLObject(state) == nullptr);
    REQUIRE(texture->needsCompile(state));
    // The diagnostic consumes and reports the original GL error.
    REQUIRE(glGetError() == GL_NO_ERROR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    REQUIRE(texture->compileGLObjects(state));
    REQUIRE(texture->isResident(state));
    REQUIRE_FALSE(texture->needsCompile(state));
    REQUIRE_FALSE(texture->compileGLObjects(state));
    REQUIRE(glGetError() == GL_NO_ERROR);
    texture->releaseGLObjects(nullptr, true);
    osgTexture->releaseGLObjects(&state);
    context->releaseContext();
}
