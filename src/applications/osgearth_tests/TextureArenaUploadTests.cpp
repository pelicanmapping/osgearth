/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/Capabilities>
#include <osgEarth/TextureArena>
#include <osg/Texture2D>
#include <osg/GraphicsContext>

using namespace osgEarth;

namespace
{
    //! Create a current offscreen GL context and give it a frame stamp for budget accounting.
    osg::ref_ptr<osg::GraphicsContext> uploadContext(osg::GraphicsContext* shared = nullptr)
    {
        auto* traits = new osg::GraphicsContext::Traits(osg::DisplaySettings::instance());
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = traits->height = 16;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->sharedContext = shared;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        REQUIRE(context.valid());
        REQUIRE(context->realize());
        REQUIRE(context->makeCurrent());
        context->getState()->setFrameStamp(new osg::FrameStamp());
        return context;
    }

    //! Make an uncompressed white texture with a predictable source byte count and no mip generation.
    Texture::Ptr uploadTexture(unsigned size = 2, bool dynamic = false)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image();
        image->allocateImage(size, size, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        std::memset(image->data(), 255, image->getTotalSizeInBytes());
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        if (dynamic) texture->setDataVariance(osg::Object::DYNAMIC);
        auto result = Texture::create(texture);
        result->compress() = false;
        return result;
    }

    //! Select a deterministic count-only budget, avoiding timing assumptions in most tests.
    TextureArena::UploadBudget countBudget(unsigned count)
    {
        TextureArena::UploadBudget result;
        result.milliseconds = 0;
        result.bytes = 0;
        result.textures = count;
        return result;
    }

    //! Fail handle creation temporarily; verify that attempts, including failures, consume the budget.
    struct UploadFailure
    {
        using Function = GLuint64 (GL_APIENTRY*)(GLuint);
        osg::GLExtensions* ext;
        Function original;
        //! Install the failure hook only in the current context.
        explicit UploadFailure(osg::State& state) : ext(state.get<osg::GLExtensions>()), original(ext->glGetTextureHandle)
        { ext->glGetTextureHandle = fail; }
        //! Restore the driver entry point before subsequent successful uploads.
        ~UploadFailure() { ext->glGetTextureHandle = original; }
        //! Simulate a driver failure without touching texture storage.
        static GLuint64 GL_APIENTRY fail(GLuint) { return 0; }
    };
}

// All passes sharing an arena/share group must observe one budget, while each context gains residency.
TEST_CASE("Texture arena upload budget is shared across passes and contexts", "[texturearena][uploadbudget][gpu]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Requires bindless textures"); return; }
    auto first = uploadContext();
    auto second = uploadContext(first);
    auto& state = *first->getState();
    REQUIRE(first->makeCurrent());
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    arena->setUploadBudget(countBudget(1));
    auto a = uploadTexture(), b = uploadTexture(), c = uploadTexture();
    arena->add(a); arena->add(b); arena->add(c);
    arena->apply(state);
    CHECK(a->isCompiled(state));
    CHECK_FALSE(b->isCompiled(state));
    CHECK(arena->getUploadStats(state).pending == 2);
    arena->apply(state);
    REQUIRE(second->makeCurrent());
    arena->apply(*second->getState());
    CHECK(a->isResident(*second->getState()));
    CHECK_FALSE(b->isCompiled(state));
    REQUIRE(first->makeCurrent());
    state.getFrameStamp()->setFrameNumber(1);
    arena->apply(state);
    CHECK(b->isCompiled(state));
    CHECK_FALSE(c->isCompiled(state));
    state.getFrameStamp()->setFrameNumber(2);
    arena->apply(state);
    CHECK(c->isCompiled(state));
    CHECK(arena->getUploadStats(state).pending == 0);
    CHECK(glGetError() == GL_NO_ERROR);
    arena = nullptr;
    for (auto& texture : {a, b, c}) texture->releaseGLObjects(nullptr, true);
    first->releaseContext();
}

// A texture larger than the byte allowance is permitted alone, so the queue cannot starve.
TEST_CASE("Texture arena byte and time budgets retain unfinished uploads", "[texturearena][uploadbudget][gpu]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Requires bindless textures"); return; }
    auto context = uploadContext();
    auto& state = *context->getState();
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    auto budget = countBudget(0);
    SECTION("Source bytes") { budget.bytes = 16; }
    SECTION("Elapsed time") { budget.milliseconds = 0.000000001; }
    arena->setUploadBudget(budget);
    auto a = uploadTexture(), b = uploadTexture(4), c = uploadTexture();
    arena->add(a); arena->add(b); arena->add(c);
    arena->apply(state);
    CHECK(arena->getUploadStats(state).attempted == 1);
    CHECK(arena->getUploadStats(state).bytes == 16);
    CHECK_FALSE(b->isCompiled(state));
    state.getFrameStamp()->setFrameNumber(1);
    arena->apply(state);
    CHECK(b->isCompiled(state));
    CHECK(arena->getUploadStats(state).attempted == 1);
    CHECK(arena->getUploadStats(state).bytes == 64);
    state.getFrameStamp()->setFrameNumber(2);
    arena->apply(state);
    CHECK(c->isCompiled(state));
    CHECK(arena->getUploadStats(state).pending == 0);
    CHECK(glGetError() == GL_NO_ERROR);
    arena = nullptr;
    for (auto& texture : {a, b, c}) texture->releaseGLObjects(nullptr, true);
    context->releaseContext();
}

// A failed dynamic upload must occur once per frame, without duplicate queue entries or starving later work.
TEST_CASE("Texture arena failed uploads consume the budget and retry fairly", "[texturearena][uploadbudget][gpu]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Requires bindless textures"); return; }
    auto context = uploadContext();
    auto& state = *context->getState();
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    arena->setUploadBudget(countBudget(1));
    auto a = uploadTexture(2, true), b = uploadTexture();
    arena->add(a); arena->add(b);
    {
        UploadFailure failure(state);
        arena->apply(state);
        for (unsigned i = 0; i < 8; ++i) arena->apply(state);
        CHECK(arena->getUploadStats(state).attempted == 1);
        CHECK(arena->getUploadStats(state).compiled == 0);
        CHECK(arena->getUploadStats(state).pending == 2);
    }
    state.getFrameStamp()->setFrameNumber(1);
    arena->apply(state);
    CHECK(b->isCompiled(state));
    CHECK_FALSE(a->isCompiled(state));
    state.getFrameStamp()->setFrameNumber(2);
    arena->apply(state);
    CHECK(a->isCompiled(state));
    CHECK(arena->getUploadStats(state).pending == 0);
    CHECK(glGetError() == GL_NO_ERROR);
    arena = nullptr;
    a->releaseGLObjects(nullptr, true); b->releaseGLObjects(nullptr, true);
    context->releaseContext();
}

// Explicit precompilation must still complete, and callers without a frame stamp must make progress.
TEST_CASE("Texture arena explicit compilation bypasses render upload limits", "[texturearena][uploadbudget][gpu]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Requires bindless textures"); return; }
    auto context = uploadContext();
    auto& state = *context->getState();
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    arena->setUploadBudget(countBudget(1));
    auto a = uploadTexture(), b = uploadTexture(), c = uploadTexture();
    arena->add(a); arena->add(b); arena->add(c);
    arena->apply(state);
    SECTION("Explicit compile in the same frame")
    {
        arena->compileGLObjects(state);
        CHECK(b->isCompiled(state));
        CHECK(c->isCompiled(state));
        CHECK(arena->getUploadStats(state).pending == 0);
    }
    SECTION("No frame stamp")
    {
        state.setFrameStamp(nullptr);
        arena->apply(state);
        CHECK(b->isCompiled(state));
        CHECK_FALSE(c->isCompiled(state));
        arena->apply(state);
        CHECK(c->isCompiled(state));
    }
    CHECK(glGetError() == GL_NO_ERROR);
    arena = nullptr;
    for (auto& texture : {a, b, c}) texture->releaseGLObjects(nullptr, true);
    context->releaseContext();
}

// Pending updates retain the last complete GL object until the dynamic texture gets its turn.
TEST_CASE("Texture arena preserves a dynamic texture while its update is deferred", "[texturearena][uploadbudget][gpu]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Requires bindless textures"); return; }
    auto context = uploadContext();
    auto& state = *context->getState();
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    arena->setUploadBudget(countBudget(1));
    auto dynamic = uploadTexture(2, true);
    arena->add(dynamic);
    arena->compileGLObjects(state);
    auto previous = dynamic->getGLObject(state);
    REQUIRE(previous != nullptr);
    auto incoming = uploadTexture();
    arena->add(incoming);
    auto* image = dynamic->osgTexture()->getImage(0);
    std::memset(image->data(), 0, image->getTotalSizeInBytes());
    image->dirty();
    arena->apply(state);
    CHECK(incoming->isCompiled(state));
    CHECK(dynamic->needsCompile(state));
    CHECK(dynamic->getGLObject(state) == previous);
    CHECK(dynamic->isResident(state));
    state.getFrameStamp()->setFrameNumber(1);
    arena->apply(state);
    CHECK_FALSE(dynamic->needsCompile(state));
    glBindTexture(GL_TEXTURE_2D, dynamic->getGLObject(state)->name());
    unsigned char pixels[16];
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (auto value : pixels) CHECK(value == 0);
    CHECK(glGetError() == GL_NO_ERROR);
    previous = nullptr;
    arena = nullptr;
    dynamic->releaseGLObjects(nullptr, true); incoming->releaseGLObjects(nullptr, true);
    context->releaseContext();
}
