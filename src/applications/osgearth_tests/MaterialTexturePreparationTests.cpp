/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/MaterialTexturePreparation>
#include <osgEarth/TextureArena>
#include <osgEarth/Capabilities>
#include <osg/Texture2D>
#include <osg/GraphicsContext>
#include <osgDB/Registry>
#include <future>

using namespace osgEarth;

namespace
{
    //! Build deterministic material inputs without files or a GL context.
    osg::ref_ptr<osg::Texture2D> preparationTexture(const osg::Vec4& color, int w = 8, int h = 8)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image();
        image->allocateImage(w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        image->setFileName("preparation-source.png");
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) image->setColor(color, x, y);
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        texture->setUnRefImageDataAfterApply(false);
        return texture;
    }

    //! Require the configured CPU compressor; missing plugins must not make these tests silently pass.
    void requirePreparationCompressor()
    {
        REQUIRE(osgDB::Registry::instance()->getImageProcessorForExtension("stbdxt") != nullptr);
    }
}

// Verify material policy, complete block-sized mip tails, metadata, and source immutability.
TEST_CASE("Material preparation respects roles without modifying shared sources", "[materialprepare]")
{
    requirePreparationCompressor();
    MaterialTexturePreparation preparation;
    osg::ref_ptr<PBRTexture> material = new PBRTexture();
    material->albedo = preparationTexture(osg::Vec4(1, 0, 0, 1), 7, 5);
    material->albedo->setInternalFormat(GL_SRGB8_ALPHA8);
    material->normal = preparationTexture(osg::Vec4(.5f, .5f, 1, 0));
    material->pbr = preparationTexture(osg::Vec4(.1f, .3f, .6f, .8f));
    material->occlusion = preparationTexture(osg::Vec4(.4f, 0, 0, 1));
    auto result = preparation.prepare(material);
    REQUIRE(result.valid());
    REQUIRE(result != material);
    CHECK_FALSE(material->albedo->getImage(0)->isCompressed());
    CHECK(material->albedo->getImage(0)->getNumMipmapLevels() == 1);
    CHECK(result->albedo->getImage(0)->s() == 7);
    CHECK(result->albedo->getImage(0)->t() == 5);
    CHECK(result->albedo->getImage(0)->getNumMipmapLevels() == 3);
    CHECK(result->albedo->getImage(0)->getTotalSizeInBytesIncludingMipmaps() == 48);
    CHECK(result->albedo->getImage(0)->getFileName() == "preparation-source.png");
    CHECK(result->albedo->getImage(0)->getPixelFormat() == GL_COMPRESSED_RGB_S3TC_DXT1_EXT);
    CHECK(result->normal->getImage(0)->getPixelFormat() == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);
    CHECK(result->pbr->getImage(0)->getPixelFormat() == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);
    CHECK(result->occlusion->getImage(0)->getPixelFormat() == GL_COMPRESSED_RED_RGTC1_EXT);
    CHECK(result->normal->getImage(0)->getNumMipmapLevels() == 4);
    material->layoutAndFactors.x() = PBRMaterial::ORM;
    auto orm = preparation.prepare(material);
    CHECK(orm->pbr->getImage(0)->getPixelFormat() == GL_COMPRESSED_RGB_S3TC_DXT1_EXT);
    CHECK(orm->albedo->getImage(0) == result->albedo->getImage(0));
    CHECK(orm->pbr->getImage(0) != result->pbr->getImage(0));
}

// Cache hits survive distinct loader allocations and tile unloads, but not source edits or policy changes.
TEST_CASE("Material preparation reuses images across concurrent tile loads", "[materialprepare]")
{
    requirePreparationCompressor();
    MaterialTexturePreparation preparation;
    osg::ref_ptr<PBRTexture> a = new PBRTexture(), b = new PBRTexture();
    a->albedo = preparationTexture(osg::Vec4(1, 0, 0, 1), 64, 64);
    b->albedo = preparationTexture(osg::Vec4(1, 0, 0, 1), 64, 64);
    // Two simultaneous cache misses must publish the same prepared image.
    auto first = std::async(std::launch::async, [&]() { return preparation.prepare(a); });
    auto second = std::async(std::launch::async, [&]() { return preparation.prepare(b); });
    auto preparedA = first.get(), preparedB = second.get();
    REQUIRE(preparedA->albedo->getImage(0) == preparedB->albedo->getImage(0));
    osg::observer_ptr<osg::Image> retained = preparedA->albedo->getImage(0);
    preparedA = nullptr;
    preparedB = nullptr;
    REQUIRE(retained.valid());
    auto again = preparation.prepare(b);
    REQUIRE(again->albedo->getImage(0) == retained.get());
    b->albedo->getImage(0)->setColor(osg::Vec4(0, 1, 0, 1), 0, 0);
    auto changed = preparation.prepare(b);
    CHECK(changed->albedo->getImage(0) != retained.get());
    a->albedo->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
    auto noMips = preparation.prepare(a);
    CHECK(noMips->albedo->getImage(0)->getNumMipmapLevels() == 1);
    CHECK(noMips->albedo->getImage(0) != retained.get());

    // Even a result larger than the retention budget stays reusable while a tile owns it.
    MaterialTexturePreparation tinyCache(1);
    auto live = tinyCache.prepare(b);
    CHECK(tinyCache.prepare(b)->albedo->getImage(0) == live->albedo->getImage(0));
    osg::observer_ptr<osg::Image> evicted = live->albedo->getImage(0);
    live = nullptr;
    CHECK_FALSE(evicted.valid());
}

// Preparation is idempotent and must not touch dynamic, ambiguous, or already encoded inputs.
TEST_CASE("Material preparation passes through unsupported textures", "[materialprepare]")
{
    requirePreparationCompressor();
    MaterialTexturePreparation preparation;
    osg::ref_ptr<PBRTexture> source = new PBRTexture();
    source->albedo = preparationTexture(osg::Vec4(1, 0, 0, .4f));
    auto prepared = preparation.prepare(source);
    REQUIRE(prepared->albedo->getImage(0)->getPixelFormat() == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);
    auto twice = preparation.prepare(prepared);
    CHECK(twice->albedo == prepared->albedo);
    source->albedo->setDataVariance(osg::Object::DYNAMIC);
    CHECK(preparation.prepare(source)->albedo == source->albedo);
    source->pbr = preparationTexture(osg::Vec4(1, 1, 1, 1));
    source->layoutAndFactors.x() = 99;
    CHECK(preparation.prepare(source)->pbr == source->pbr);
    source->normal = preparationTexture(osg::Vec4(.5f, .5f, 1, 1));
    source->normal->setInternalFormat(GL_SRGB8);
    CHECK(preparation.prepare(source)->normal == source->normal);
    CHECK_FALSE(preparation.prepare(nullptr).valid());
}

// Exercise actual compressed uploads, gamma-correct mips, normal alpha, and scalar data on the GPU.
TEST_CASE("Prepared material mipmaps upload with correct color and normal semantics", "[materialprepare][gpu]")
{
    requirePreparationCompressor();
    if (!Capabilities::get().supportsNVGL()) { WARN("Requires bindless textures"); return; }
    auto traits = new osg::GraphicsContext::Traits(osg::DisplaySettings::instance());
    traits->readDISPLAY();
    traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = traits->height = 16;
    traits->pbuffer = true;
    traits->doubleBuffer = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    REQUIRE(context.valid());
    REQUIRE(context->realize());
    REQUIRE(context->makeCurrent());
    MaterialTexturePreparation preparation;
    osg::ref_ptr<PBRTexture> source = new PBRTexture();
    source->albedo = preparationTexture(osg::Vec4(0, 0, 0, 1), 7, 5);
    source->albedo->setInternalFormat(GL_SRGB8_ALPHA8);
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 7; ++x)
            if ((x + y) % 2) source->albedo->getImage(0)->setColor(osg::Vec4(1, 1, 1, 1), x, y);
    source->normal = preparationTexture(osg::Vec4(.5f, .5f, 1, 0));
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            source->normal->getImage(0)->setColor(osg::Vec4((x % 2 ? .7071f : -.7071f) * .5f + .5f,
                .5f, .85355f, 0), x, y);
    source->occlusion = preparationTexture(osg::Vec4(.4f, 0, 0, 1));
    auto result = preparation.prepare(source);
    unsigned index = 0;
    for (auto* texture : { result->albedo.get(), result->normal.get(), result->occlusion.get() })
    {
        auto arenaTexture = Texture::create(texture);
        REQUIRE(arenaTexture->compileGLObjects(*context->getState()));
        glBindTexture(GL_TEXTURE_2D, arenaTexture->getGLObject(*context->getState())->name());
        unsigned char pixel[4] = {};
        glGetTexImage(GL_TEXTURE_2D, texture->getImage(0)->getNumMipmapLevels() - 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        REQUIRE(glGetError() == GL_NO_ERROR);
        if (index == 0) CHECK(std::abs(int(pixel[0]) - 188) < 10);
        else if (index == 1)
        {
            CHECK(std::abs(int(pixel[0]) - 128) < 10);
            CHECK(pixel[2] > 245);
            CHECK(pixel[3] == 255);
        }
        else CHECK(std::abs(int(pixel[0]) - 102) < 4);
        arenaTexture->releaseGLObjects(nullptr, true);
        ++index;
    }
    context->releaseContext();
}
