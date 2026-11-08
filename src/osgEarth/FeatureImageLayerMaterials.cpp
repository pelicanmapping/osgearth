// osgEarth - Copyright 2026 Pelican Mapping - MIT License
#include "FeatureImageLayerMaterials"
#include <osgEarth/MaterialTexturePreparation>
#include <osgEarth/Capabilities>
#include <osgEarth/Registry>
#include <osgEarth/Shaders>
#include <osgEarth/VirtualProgram>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace osgEarth;
namespace
{
    const char* paletteName = "osgEarth.FeatureImageMaterials";
}

FeatureImageLayerMaterials* FeatureImageLayerMaterials::get(const osg::Object* layer)
{
    if (!layer) return nullptr;
    return const_cast<FeatureImageLayerMaterials*>(
        dynamic_cast<const FeatureImageLayerMaterials*>(osg::getUserObject(layer, paletteName)));
}

Status FeatureImageLayerMaterials::open(FeatureImageLayer* layer)
{
    auto* container = layer->getOrCreateUserDataContainer();
    unsigned old = container->getUserObjectIndex(paletteName);
    if (old < container->getNumUserObjects())
    {
        container->removeUserObject(old);
    }
    auto* state = layer->getOrCreateStateSet();
    state->removeDefine("OE_FEATUREIMAGE_MATERIALS");
    Util::Shaders previous;
    previous.unload(VirtualProgram::get(state), "FeatureImageLayer.Materials.glsl");
    const Config conf = layer->options().getConfig();
    const auto& configs = conf.child("materials").children("material");
    if (configs.empty()) return Status::OK();
    if (!GLUtils::useNVGL() || !Registry::capabilities().supportsNVGL())
        return Status(Status::ConfigurationError, "Vector FIL materials require the NVGL bindless renderer (--nvgl)");
    optional<osg::Vec3d> configuredAnchor(osg::Vec3d(0, 0, 0));
    conf.get("material_anchor", configuredAnchor);
    const osg::Vec3d anchor = configuredAnchor.get();
    if (!std::isfinite(anchor.x()) || !std::isfinite(anchor.y()) || !std::isfinite(anchor.z()) ||
        std::abs(anchor.x()) > 180.0 || std::abs(anchor.y()) > 90.0)
        return Status(Status::ConfigurationError, "Invalid vector FIL material_anchor");
    osg::ref_ptr<FeatureImageLayerMaterials> palette = new FeatureImageLayerMaterials;
    palette->setName(paletteName);
    MaterialTexturePreparation prepare;
    for (const auto& config : configs)
    {
        Entry entry;
        entry.name = config.value("name");
        if (entry.name.empty()) return Status(Status::ConfigurationError, "FIL material requires a name");
        for (const auto& existing : palette->_entries)
            if (existing.name == entry.name) return Status(Status::ConfigurationError, "Duplicate FIL material name");
        if (palette->_entries.size() >= 65535u)
            return Status(Status::ConfigurationError, "Too many FIL materials");
        PBRMaterial material(config);
        osg::ref_ptr<PBRTexture> texture = new PBRTexture;
        Status result = texture->load(material, layer->getReadOptions());
        if (result.isError()) return result;
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        // Missing maps use shader defaults and incur no texture sample.
        if (!material.normal().isSet() && !material.normalImage) texture->normal = nullptr;
        entry.texture = prepare.prepare(texture.get());
        auto* image = entry.texture->albedo ? entry.texture->albedo->getImage(0) : nullptr;
        const GLenum format = image ? image->getInternalTextureFormat() : 0;
        entry.flags = (format == GL_SRGB || format == GL_SRGB8 || format == GL_SRGB_ALPHA ||
            format == GL_SRGB8_ALPHA8 || format == 0x8C4C || format == 0x8C4D || format == 0x8C4E ||
            format == 0x8C4F || format == 0x8E8D) ? 1u : 0u;
        palette->_entries.push_back(entry);
    }
    palette->_fixed = configuredAnchor.isSet();
    GeoPoint(SpatialReference::get("wgs84"), anchor.x(), anchor.y(), anchor.z()).createWorldToLocal(
        palette->_worldToMaterial);
    container->addUserObject(palette.get());
    state->setDefine("OE_FEATUREIMAGE_MATERIALS");
    Util::Shaders shaders;
    shaders.load(VirtualProgram::getOrCreate(state), "FeatureImageLayer.Materials.glsl");
    return Status::OK();
}

unsigned FeatureImageLayerMaterials::slot(const std::string& name) const
{
    if (name.empty()) return 0;
    for (unsigned i = 0; i < _entries.size(); ++i)
        if (_entries[i].name == name) return i + 1u;
    throw std::runtime_error("Unknown FIL material: " + name);
}

void FeatureImageLayerMaterials::apply(TextureArena* arena, osg::State& state)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto& registration = _registrations[arena];
    if (!registration)
    {
        registration = std::make_shared<Registration>();
        registration->arena = arena;
        registration->slots.emplace_back(0u, 0u, 0u, 0u);
        for (const auto& entry : _entries)
        {
            const auto* source = entry.texture.get();
            MaterialArena::Indices indices = {{ -1, -1, -1, -1, -1 }};
            int ao = -1;
            osg::Texture* images[] = { source->albedo.get(), source->normal.get(), source->pbr.get(),
                source->occlusion.get() };
            for (unsigned i = 0; i < 4; ++i)
            {
                if (!images[i]) continue;
                auto texture = Texture::create(images[i]);
                texture->compress() = false; // Already prepared using the semantic material pipeline.
                texture->ownerRevision() = std::numeric_limits<int>::max();
                int index = arena->add(texture);
                if (i == 3) ao = index; else indices[i] = index;
                registration->textures.push_back(texture);
            }
            auto material = arena->getMaterialArena()->getOrCreate(*arena, indices, osg::Vec2i(-1, -1),
                ao, source->layoutAndFactors);
            registration->slots.emplace_back(unsigned(material->index), entry.flags, 0u, 0u);
            registration->materials.push_back(material);
        }
    }
    auto& gl = Registration::GLObjects::get(registration->gl, state);
    if (!gl.buffer || !gl.buffer->valid())
    {
        gl.buffer = GLBuffer::create_shared(GL_SHADER_STORAGE_BUFFER, state);
        // glGenBuffers reserves a name; bind once before the named-buffer upload path can use it.
        gl.buffer->bind();
        gl.buffer->uploadData(registration->slots);
        gl.buffer->unbind();
    }
    gl.buffer->bindBufferBase(12);
}

osg::Matrixd FeatureImageLayerMaterials::worldToMaterial(const osg::Vec3d& eye)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_fixed)
    {
        const auto* srs = SpatialReference::get("wgs84");
        const auto geo = srs->getEllipsoid().geocentricToGeodetic(eye);
        double dx = std::abs(geo.x() - _anchor.x());
        dx = std::min(dx, 360.0 - dx);
        if (!_anchored || dx > 0.75 || std::abs(geo.y() - _anchor.y()) > 0.75)
        {
            _anchor.set(std::floor(geo.x()) + 0.5, osg::clampBetween(std::floor(geo.y()) + 0.5, -89.0, 89.0));
            GeoPoint(srs, _anchor.x(), _anchor.y(), 0.0).createWorldToLocal(_worldToMaterial);
            _anchored = true;
        }
    }
    return _worldToMaterial;
}

FeatureImageLayerMaterials::Coordinates FeatureImageLayerMaterials::coordinates(const osg::Matrixd& matrix)
{
    Coordinates result;
    result.u.set(matrix(0, 0), matrix(1, 0), matrix(2, 0), float(matrix(3, 0)));
    result.v.set(matrix(0, 1), matrix(1, 1), matrix(2, 1), float(matrix(3, 1)));
    result.low.set(float(matrix(3, 0) - double(result.u.w())), float(matrix(3, 1) - double(result.v.w())), 0, 0);
    return result;
}
