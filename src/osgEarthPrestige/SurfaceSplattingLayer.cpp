/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarthPrestige/SurfaceSplattingLayer>
#include <osgEarthPrestige/PrestigeShaders>
#include <osgEarth/Map>
#include <osgEarth/MaterialTexturePreparation>
#include <osgEarth/PolygonSymbol>
#include <osgEarth/Progress>
#include <osgEarth/Shaders>
#include <osgEarth/TerrainEngineNode>
#include <osgEarth/TerrainResources>
#include <osgEarth/VirtualProgram>
#include <osgUtil/CullVisitor>
#include <osg/Math>
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

using namespace osgEarth;

namespace osgEarthPrestige
{
REGISTER_OSGEARTH_LAYER_FACTORY("prestige:surface_splatting", SurfaceSplattingLayer);

namespace
{
    //! Preserve exact coverage bytes in a private R8 image; legacy RGBA caches contribute alpha.
    //! Empty/unsupported inputs fail closed, and cancellation never publishes a partial tile.
    GeoImage redCoverage(const GeoImage& source, ProgressCallback* progress)
    {
        if (!source.valid() || (progress && progress->isCanceled())) return GeoImage::INVALID;
        const auto* input = source.getImage();
        const auto format = input->getPixelFormat();
        if (input->isCompressed() || input->getDataType() != GL_UNSIGNED_BYTE || input->r() != 1)
            return GeoImage(Status(Status::ConfigurationError, "Surface coverage requires uncompressed 8-bit pixels"));
        if (format == GL_RED && input->getInternalTextureFormat() == GL_R8) return source;
        const unsigned channels = format == GL_RGBA || format == GL_BGRA ? 4u :
            format == GL_RED || format == GL_LUMINANCE ? 1u : 0u;
        if (channels == 0u)
            return GeoImage(Status(Status::ConfigurationError, "Unsupported surface coverage cache encoding"));
        osg::ref_ptr<osg::Image> output = new osg::Image;
        output->allocateImage(input->s(), input->t(), 1, GL_RED, GL_UNSIGNED_BYTE, 1);
        output->setInternalTextureFormat(GL_R8);
        output->setOrigin(input->getOrigin());
        output->setName(input->getName());
        for (int y = 0; y < input->t(); ++y)
        {
            if (progress && progress->isCanceled()) return GeoImage::INVALID;
            const auto* read = input->data(0, y);
            auto* write = output->data(0, y);
            for (int x = 0; x < input->s(); ++x) write[x] = read[x*channels + channels-1u];
        }
        GeoImage result(output.get(), source.getExtent());
        result.setTrackingToken(source.getTrackingToken());
        return result;
    }

    //! Construct a one-texel fallback; an absent file differs from a requested file that failed to load.
    osg::Image* constantImage(const osg::Vec4& value)
    {
        auto* image = new osg::Image;
        image->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        image->setColor(value, 0, 0);
        return image;
    }

    //! Recognize hardware sRGB decoding, including precompressed material images.
    bool decodesSRGB(GLenum format)
    {
        return format == GL_SRGB || format == GL_SRGB_ALPHA || format == GL_SRGB8 || format == GL_SRGB8_ALPHA8 ||
            format == 0x8C4C || format == 0x8C4D || format == 0x8C4E || format == 0x8C4F || format == 0x8E8D;
    }

    //! Own a regional frame independently of the layer; uniforms are private to each cull traversal.
    struct SurfaceCullCallback : Layer::TraversalCallback
    {
        osg::Matrixd worldToRegion;
        double period = 4.0;
        //! Without a configured anchor, the frame follows the camera across a grid of cells (degrees).
        bool fixed = true;
        double cell = 1.0;
        mutable std::mutex mutex;
        mutable bool anchored = false;
        mutable osg::Vec2d anchor;
        mutable osg::Matrixd cellToRegion;

        //! Return the frame of the cell holding the camera. A frame is kept until the camera is three quarters
        //! of a cell from its center, so hovering on a cell edge never flips the texture back and forth;
        //! crossing re-anchors and shifts the repeating pattern once. Thread safe for concurrent culls.
        osg::Matrixd frameFor(const osg::Vec3d& eye) const
        {
            if (fixed) return worldToRegion;
            const auto* wgs84 = SpatialReference::get("wgs84");
            const osg::Vec3d geo = wgs84->getEllipsoid().geocentricToGeodetic(eye);
            std::lock_guard<std::mutex> lock(mutex);
            double dx = std::abs(geo.x() - anchor.x());
            dx = std::min(dx, 360.0 - dx);
            if (!anchored || dx > 0.75*cell || std::abs(geo.y() - anchor.y()) > 0.75*cell)
            {
                anchor.set((std::floor(geo.x()/cell) + 0.5)*cell,
                    osg::clampBetween((std::floor(geo.y()/cell) + 0.5)*cell, -89.0, 89.0));
                GeoPoint(wgs84, anchor.x(), anchor.y(), 0.0, ALTMODE_ABSOLUTE).createWorldToLocal(cellToRegion);
                anchored = true;
            }
            return cellToRegion;
        }

        //! Cancel ECEF translation in doubles, then reduce only translation to preserve UV derivatives.
        void operator()(osg::Node* node, osg::NodeVisitor* nv) const override
        {
            auto* cv = dynamic_cast<osgUtil::CullVisitor*>(nv);
            osg::Matrixd inverseView;
            if (!cv || !inverseView.invert(*cv->getModelViewMatrix()))
            {
                traverse(node, nv);
                return;
            }
            osg::Matrixd matrix = inverseView * frameFor(inverseView.getTrans()) *
                osg::Matrixd::scale(1.0/period, 1.0/period, 1.0/period);
            for (unsigned axis = 0; axis < 3; ++axis) matrix(3, axis) -= std::floor(matrix(3, axis));
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->addUniform(new osg::Uniform("oe_surface_viewToTexture", osg::Matrixf(matrix)));
            cv->pushStateSet(state);
            traverse(node, nv);
            cv->popStateSet();
        }
    };
}

struct SurfaceSplattingLayer::Impl
{
    osg::ref_ptr<PBRTexture> texture;
    //! Relief height map (red channel), present only when relief_depth is positive.
    osg::ref_ptr<osg::Texture> height;
    osg::ref_ptr<SurfaceCullCallback> callback;
    std::array<TextureImageUnitReservation, 5> units;
    bool normal = false;
    bool albedoLinear = false;

    //! Roll back every unit, including a partially successful allocation.
    void releaseUnits(osg::StateSet* state)
    {
        for (auto& unit : units)
        {
            if (state && unit.valid()) state->removeTextureAttribute(unit.unit(), osg::StateAttribute::TEXTURE);
            unit.release();
        }
        if (state)
        {
            state->removeDefine("OE_TERRAIN_LAYER_MATERIAL");
            state->removeDefine("OE_TERRAIN_LAYER_COVERAGE_RED");
            PrestigeShaders shaders;
            osgEarth::Util::Shaders core;
            shaders.add("PBRMaterial.glsl", core.context().at(core.PBRMaterial));
            shaders.unload(VirtualProgram::get(state), "SurfaceSplattingLayer.glsl");
        }
    }
};

Config SurfaceSplattingLayer::Options::getConfig() const
{
    Config conf = FeatureImageLayer::Options::getConfig();
    conf.set("material", material());
    conf.set("anchor", anchor());
    conf.set("anchor_cell", anchorCell());
    conf.set("texture_period", texturePeriod());
    conf.set("normal_strength", normalStrength());
    conf.set("coverage_outline", coverageOutline());
    conf.set("max_slope", maxSlope());
    conf.set("relief_depth", reliefDepth());
    // Distinguish native R8 tiles from former RGBA tiles in automatic persistent-cache identities.
    conf.set("coverage_encoding", "r8");
    return conf;
}

void SurfaceSplattingLayer::Options::fromConfig(const Config& conf)
{
    conf.get("material", material());
    conf.get("anchor", anchor());
    conf.get("anchor_cell", anchorCell());
    conf.get("texture_period", texturePeriod());
    conf.get("normal_strength", normalStrength());
    conf.get("coverage_outline", coverageOutline());
    conf.get("max_slope", maxSlope());
    conf.get("relief_depth", reliefDepth());
}

void SurfaceSplattingLayer::init()
{
    FeatureImageLayer::init();
    _impl.reset(new Impl);
    // Coverage is filtered alpha, not categorical land-cover data. Fill all supplied polygons.
    Style style;
    style.getOrCreate<PolygonSymbol>()->fill()->color() = Color::White;
    osg::ref_ptr<StyleSheet> styles = new StyleSheet;
    styles->addStyle(style);
    setStyleSheet(styles);
    options().backgroundColor() = Color::Transparent;
    options().coverage() = false;
    // An empty child must hide its parent's roads, using the same channel as populated tiles.
    _emptyImage = new osg::Image;
    _emptyImage->allocateImage(1, 1, 1, GL_RED, GL_UNSIGNED_BYTE, 1);
    _emptyImage->setInternalTextureFormat(GL_R8);
    *_emptyImage->data() = 0;
}

SurfaceSplattingLayer::~SurfaceSplattingLayer() = default;

void SurfaceSplattingLayer::setMaxSlope(float degrees)
{
    if (!std::isfinite(degrees) || degrees <= 0.0f || degrees > 90.0f) return;
    options().maxSlope() = degrees;
    // Fade five degrees either side of the cutoff; 90 keeps coverage on every slope.
    const float fade = 5.0f;
    const osg::Vec2f edges = degrees >= 90.0f ? osg::Vec2f(-2.0f, -1.0f) : osg::Vec2f(
        std::cos(osg::DegreesToRadians(std::min(90.0f, degrees + fade))),
        std::cos(osg::DegreesToRadians(std::max(0.0f, degrees - fade))));
    getOrCreateStateSet()->getOrCreateUniform("oe_surface_slopeEdges", osg::Uniform::FLOAT_VEC2)->set(edges);
}

float SurfaceSplattingLayer::getMaxSlope() const
{
    return options().maxSlope().get();
}

Status SurfaceSplattingLayer::openImplementation()
{
    const auto& anchor = options().anchor().get();
    const auto period = options().texturePeriod().get();
    const auto strength = options().normalStrength().get();
    const auto cell = options().anchorCell().get();
    const auto slope = options().maxSlope().get();
    const auto relief = options().reliefDepth().get();
    if (!std::isfinite(slope) || slope <= 0.0f || slope > 90.0f)
        return Status(Status::ConfigurationError, "Invalid surface max_slope (degrees, above 0 and at most 90)");
    if (!std::isfinite(relief) || relief < 0.0f || relief > 1.0f)
        return Status(Status::ConfigurationError, "Invalid surface relief_depth (meters, 0 to 1)");
    if (!std::isfinite(period) || period <= 0.0 || !std::isfinite(strength) || strength < 0.0f ||
        !std::isfinite(cell) || cell <= 0.0 || cell > 10.0 ||
        !std::isfinite(anchor.x()) || !std::isfinite(anchor.y()) || !std::isfinite(anchor.z()) ||
        std::abs(anchor.x()) > 180.0 || std::abs(anchor.y()) > 90.0)
        return Status(Status::ConfigurationError, "Invalid surface anchor, texture period, or normal strength");

    // Resolve immutable raster settings at open, including options configured after construction.
    Style coverageStyle;
    auto* polygon = coverageStyle.getOrCreate<PolygonSymbol>();
    polygon->fill()->color() = Color::White;
    polygon->outline() = options().coverageOutline().get();
    osg::ref_ptr<StyleSheet> coverageStyles = new StyleSheet;
    coverageStyles->addStyle(coverageStyle);
    setStyleSheet(coverageStyles);
    // Coverage is linear fractional data. Keep filtering, but prevent global imagery compression
    // settings from changing its exact 8-bit values or expanding it to RGBA blocks.
    options().textureCompression() = "none";
    auto status = FeatureImageLayer::openImplementation();
    if (status.isError()) return status;
    auto material = options().material().get();
    _impl->normal = material.normalImage.valid() || material.normal().isSet();
    if (!material.colorImage && !material.color().isSet())
        material.colorImage = constantImage(osg::Vec4(0.22f, 0.23f, 0.24f, 1.0f));
    if (!material.roughnessImage && !material.roughness().isSet() &&
        !material.packedImage && !material.packed().isSet() && material.layout() == PBRMaterial::DRAM)
        material.roughnessImage = constantImage(osg::Vec4(0.85f, 0.85f, 0.85f, 1.0f));
    osg::ref_ptr<PBRTexture> loaded = new PBRTexture;
    status = loaded->load(material, getReadOptions());
    if (status.isError()) return status;
    // Generate albedo mips in linear light. The shader re-encodes hardware-decoded samples for terrain lighting.
    if (!loaded->albedo->getImage(0)->isCompressed()) loaded->albedo->setInternalFormat(GL_SRGB8_ALPHA8);
    // OSG 3.6's ordinary Texture2D upload rewrites compressed formats when the
    // base dimensions are not block aligned, including 1x1 constant fallbacks.
    // Preserve those uncompressed inputs; the shared preparer also serves a
    // direct-upload arena which does not have this restriction.
    osg::ref_ptr<PBRTexture> compatible = new PBRTexture(*loaded, osg::CopyOp::SHALLOW_COPY);
    for (auto* component : { &compatible->albedo, &compatible->normal, &compatible->pbr, &compatible->occlusion })
    {
        if (!*component) continue;
        const auto* image = (*component)->getImage(0);
        if (image && (image->s()%4 != 0 || image->t()%4 != 0))
        {
            if (image->isCompressed())
                return Status(Status::ConfigurationError, "Precompressed surface textures need block-aligned base dimensions");
            *component = nullptr;
        }
    }
    MaterialTexturePreparation preparation(0);
    _impl->texture = preparation.prepare(compatible);
    if (!_impl->texture->albedo) _impl->texture->albedo = loaded->albedo;
    if (!_impl->texture->normal) _impl->texture->normal = loaded->normal;
    if (!_impl->texture->pbr) _impl->texture->pbr = loaded->pbr;
    if (!_impl->texture->occlusion) _impl->texture->occlusion = loaded->occlusion;
    // Older OSG upload code does not size sRGB S3TC formats correctly. Keep the
    // prepared sRGB bytes/mips in ordinary BC storage and let terrain lighting decode.
    auto* albedoImage = _impl->texture->albedo->getImage(0);
    if (albedoImage->isCompressed() && !decodesSRGB(albedoImage->getPixelFormat()))
        _impl->texture->albedo->setInternalFormat(albedoImage->getPixelFormat());
    _impl->albedoLinear = decodesSRGB(_impl->texture->albedo->getInternalFormat()) ||
        decodesSRGB(_impl->texture->albedo->getImage(0)->getInternalTextureFormat());
    _impl->texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
    _impl->texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);

    // PBRTexture ignores a separate displacement raster beside a packed map, so the relief height map is loaded
    // here and prepared like an occlusion map (one compressed red channel).
    _impl->height = nullptr;
    if (relief > 0.0f && (material.displacementImage.valid() || material.displacement().isSet()))
    {
        osg::ref_ptr<osg::Image> image = material.displacementImage;
        if (!image.valid())
        {
            auto result = material.displacement()->readImage(getReadOptions());
            if (result.failed())
                return Status(Status::ResourceUnavailable,
                    "Failed to load " + material.displacement()->full() + " ... " + result.errorDetail());
            image = result.getImage();
        }
        osg::ref_ptr<osg::Texture2D> height = new osg::Texture2D(image.get());
        height->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        height->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        height->setMaxAnisotropy(4.0f);
        // Block compression needs aligned dimensions (see the material components above).
        osg::ref_ptr<PBRTexture> prepared;
        if (image->s()%4 == 0 && image->t()%4 == 0)
        {
            osg::ref_ptr<PBRTexture> heightOnly = new PBRTexture;
            heightOnly->occlusion = height;
            prepared = preparation.prepare(heightOnly.get());
        }
        _impl->height = prepared.valid() && prepared->occlusion.valid() ? prepared->occlusion.get() : height.get();
        _impl->height->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        _impl->height->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
    }
    return Status::OK();
}

GeoImage SurfaceSplattingLayer::createImageImplementation(const TileKey& key, ProgressCallback* progress) const
{
    return redCoverage(FeatureImageLayer::createImageImplementation(key, progress), progress);
}

void SurfaceSplattingLayer::postCreateImageImplementation(
    GeoImage& image, const TileKey& key, ProgressCallback* progress) const
{
    image = redCoverage(image, progress);
}

void SurfaceSplattingLayer::addedToMap(const Map* map)
{
    FeatureImageLayer::addedToMap(map);
    if (!map->getSRS()->isGeographic())
    {
        setStatus(Status(Status::ConfigurationError, "Surface splatting currently requires a geocentric map"));
        return;
    }
    const auto& anchor = options().anchor().get();
    _impl->callback = new SurfaceCullCallback;
    _impl->callback->fixed = options().anchor().isSet();
    _impl->callback->cell = options().anchorCell().get();
    GeoPoint(SpatialReference::get("wgs84"), anchor.x(), anchor.y(), anchor.z(), ALTMODE_ABSOLUTE)
        .createWorldToLocal(_impl->callback->worldToRegion);
    _impl->callback->period = options().texturePeriod().get();
    setCullCallback(_impl->callback);
}

void SurfaceSplattingLayer::removedFromMap(const Map* map)
{
    setCullCallback(nullptr);
    _impl->callback = nullptr;
    _impl->releaseUnits(getStateSet());
    FeatureImageLayer::removedFromMap(map);
}

Status SurfaceSplattingLayer::closeImplementation()
{
    setCullCallback(nullptr);
    _impl->callback = nullptr;
    _impl->releaseUnits(getStateSet());
    _impl->texture = nullptr;
    _impl->height = nullptr;
    return FeatureImageLayer::closeImplementation();
}

void SurfaceSplattingLayer::prepareForRendering(TerrainEngine* engine)
{
    FeatureImageLayer::prepareForRendering(engine);
    if (!_impl->texture || getStatus().isError()) return;
    auto* state = getOrCreateStateSet();
    _impl->releaseUnits(getStateSet());
    const char* names[] = { "oe_surface_albedo", "oe_surface_normal", "oe_surface_pbr", "oe_surface_ao",
        "oe_surface_height" };
    const auto& material = _impl->texture;
    osg::Texture* textures[] = { material->albedo.get(), material->normal.get(),
        material->pbr.get(), material->occlusion.get(), _impl->height.get() };
    for (unsigned i = 0; i < 5; ++i)
    {
        if (!textures[i]) continue;
        if (!engine->getResources()->reserveTextureImageUnitForLayer(_impl->units[i], this, "Prestige surface material"))
        {
            _impl->releaseUnits(getStateSet());
            setStatus(Status(Status::ResourceUnavailable, "Insufficient texture units for surface material"));
            return;
        }
    }
    for (unsigned i = 0; i < 5; ++i)
    {
        if (!textures[i]) continue;
        state->setTextureAttributeAndModes(_impl->units[i].unit(), textures[i], osg::StateAttribute::ON);
        state->addUniform(new osg::Uniform(names[i], _impl->units[i].unit()));
    }
    state->addUniform(new osg::Uniform("oe_surface_factors", material->layoutAndFactors));
    state->addUniform(new osg::Uniform("oe_surface_flags", int(_impl->normal) |
        (material->pbr ? 2 : 0) | (material->occlusion ? 4 : 0) | (_impl->albedoLinear ? 8 : 0) |
        (_impl->height ? 16 : 0)));
    state->addUniform(new osg::Uniform("oe_surface_reliefDepth", options().reliefDepth().get()));
    state->addUniform(new osg::Uniform("oe_surface_normalStrength", options().normalStrength().get()));
    setMaxSlope(getMaxSlope());
    state->setDefine("OE_TERRAIN_LAYER_MATERIAL");
    state->setDefine("OE_TERRAIN_LAYER_COVERAGE_RED");
    PrestigeShaders shaders;
    osgEarth::Util::Shaders core;
    shaders.add("PBRMaterial.glsl", core.context().at(core.PBRMaterial));
    shaders.load(VirtualProgram::getOrCreate(state), "SurfaceSplattingLayer.glsl");
}
}
