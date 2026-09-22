/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "GrimeLayer"
#include "GeoData"
#include "Map"
#include "MapCallback"
#include "Shaders"
#include "TerrainEngineNode"
#include "TerrainResources"
#include "VirtualProgram"
#include <osg/Texture3D>
#include <osgUtil/CullVisitor>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

using namespace osgEarth;
using namespace osgEarth::Util;

REGISTER_OSGEARTH_LAYER(Grime, GrimeLayer);

namespace
{
    // Hash integer lattice coordinates reproducibly, independent of load order.
    double lattice(unsigned x, unsigned y, unsigned z, unsigned seed)
    {
        std::uint32_t h = seed ^ (x * 0x8da6b343u) ^ (y * 0xd8163841u) ^ (z * 0xcb1ab31fu);
        h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
        return double(h & 0x00ffffffu) / double(0x00ffffffu);
    }

    // Sample periodic value noise with a quintic fade, including across all faces.
    double periodicNoise(double x, double y, double z, unsigned period, unsigned seed)
    {
        double p[] = { x * period, y * period, z * period };
        unsigned base[3];
        double w[3];
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            base[axis] = unsigned(std::floor(p[axis]));
            double t = p[axis] - std::floor(p[axis]);
            w[axis] = t*t*t*(t*(t*6.0 - 15.0) + 10.0);
        }
        double result = 0.0;
        for (unsigned dz = 0; dz < 2; ++dz)
        for (unsigned dy = 0; dy < 2; ++dy)
        for (unsigned dx = 0; dx < 2; ++dx)
            result += lattice((base[0]+dx)%period, (base[1]+dy)%period,
                (base[2]+dz)%period, seed) *
                (dx ? w[0] : 1.0-w[0]) * (dy ? w[1] : 1.0-w[1]) * (dz ? w[2] : 1.0-w[2]);
        return result;
    }

    // Generate two independent periodic fields and a complete 3D box-filtered mip
    // chain. The caller validates a power-of-two size in [16,128]; OSG owns the data.
    osg::ref_ptr<osg::Image> makeVolume(unsigned size, unsigned seed)
    {
        osg::Image::MipmapDataType offsets;
        unsigned bytes = size*size*size*2u;
        for (unsigned dim = size/2; dim; dim /= 2)
        {
            offsets.push_back(bytes);
            bytes += dim*dim*dim*2u;
        }
        auto data = new unsigned char[bytes];
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->setImage(size, size, size, GL_RG8, GL_RG, GL_UNSIGNED_BYTE,
            data, osg::Image::USE_NEW_DELETE, 1);
        image->setMipmapLevels(offsets);
        for (unsigned z = 0; z < size; ++z)
        for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x)
        for (unsigned c = 0; c < 2; ++c)
        {
            const double s = (x+0.5)/size, t = (y+0.5)/size, r = (z+0.5)/size;
            const unsigned channelSeed = seed + c*0x9e3779b9u;
            double value = 0.58*periodicNoise(s,t,r,4,channelSeed) +
                0.28*periodicNoise(s,t,r,8,channelSeed+1) +
                0.14*periodicNoise(s,t,r,16,channelSeed+2);
            data[((z*size+y)*size+x)*2+c] = static_cast<unsigned char>(value*255.0+0.5);
        }
        unsigned previousOffset = 0, previousSize = size;
        for (auto offset : offsets)
        {
            const unsigned dim = previousSize/2;
            for (unsigned z = 0; z < dim; ++z)
            for (unsigned y = 0; y < dim; ++y)
            for (unsigned x = 0; x < dim; ++x)
            for (unsigned c = 0; c < 2; ++c)
            {
                unsigned sum = 0;
                for (unsigned dz = 0; dz < 2; ++dz)
                for (unsigned dy = 0; dy < 2; ++dy)
                for (unsigned dx = 0; dx < 2; ++dx)
                    sum += data[previousOffset + (((2*z+dz)*previousSize+2*y+dy)*previousSize+2*x+dx)*2+c];
                data[offset+((z*dim+y)*dim+x)*2+c] = (sum+4)/8;
            }
            previousOffset = offset;
            previousSize = dim;
        }
        return image;
    }

    // Reduce only the double-precision translation modulo the texture period;
    // keeping vertex coordinates unwrapped preserves interpolation and derivatives.
    osg::Matrixf textureMatrix(const osg::Matrixd& viewToRegion, const osg::Vec3d& period)
    {
        osg::Matrixd matrix = viewToRegion * osg::Matrixd::scale(1.0/period.x(), 1.0/period.y(), 1.0/period.z());
        for (unsigned axis = 0; axis < 3; ++axis)
            matrix(3,axis) -= std::floor(matrix(3,axis));
        return osg::Matrixf(matrix);
    }

    // Own effect state independently of the layer. Each traversal gets fresh
    // uniforms, so simultaneous cameras and pipelined cull/draw never overwrite them.
    struct GrimeCullCallback : osg::NodeCallback
    {
        osg::ref_ptr<osg::StateSet> state;
        osg::Matrixd worldToRegion;
        std::mutex parametersMutex;
        osg::Vec3d macroPeriod, streakPeriod;
        osg::Vec3 roughness; // target, strength, roughen-only flag
        osg::Vec3 smooth; // original-roughness threshold, feather width, retained color strength
        osg::Vec3 attenuation; // maximum distance, fade width, upward-surface suppression
        std::atomic<float> amount{0.0f};
        std::atomic<float> opacity{1.0f};
        std::atomic<bool> visible{true};

        //! Push this effect around the target subtree, preserving nested callbacks.
        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            auto* cv = dynamic_cast<osgUtil::CullVisitor*>(nv);
            // Fade both color and roughness through coverage, preserving target alpha.
            const float strength = amount.load() * osg::clampBetween(opacity.load(), 0.0f, 1.0f);
            if (!cv || !visible.load() || strength <= 0.0f)
            {
                traverse(node, nv);
                return;
            }
            osg::Matrixd inverseView;
            if (!inverseView.invert(*cv->getModelViewMatrix()))
            {
                traverse(node, nv);
                return;
            }
            const osg::Matrixd viewToRegion = inverseView * worldToRegion;
            osg::Matrixd regionToView;
            if (!regionToView.invert(viewToRegion))
            {
                traverse(node, nv);
                return;
            }
            // Displacements use the linear transform, normals its inverse transpose.
            // This also handles projected maps whose horizontal units are not meters.
            osg::Matrix3 viewToMeters, normalToRegion;
            for (unsigned row = 0; row < 3; ++row)
            for (unsigned col = 0; col < 3; ++col)
            {
                viewToMeters(row, col) = static_cast<float>(viewToRegion(row, col));
                normalToRegion(row, col) = static_cast<float>(regionToView(col, row));
            }
            // Snapshot related controls together; release the lock before traversing geometry.
            osg::Vec3d macro, streak;
            osg::Vec3 roughnessParameters, smoothParameters, attenuationParameters;
            {
                std::lock_guard<std::mutex> lock(parametersMutex);
                macro = macroPeriod;
                streak = streakPeriod;
                roughnessParameters = roughness;
                smoothParameters = smooth;
                attenuationParameters = attenuation;
            }
            osg::ref_ptr<osg::StateSet> viewState = new osg::StateSet;
            viewState->addUniform(new osg::Uniform("oe_grime_viewToMacro", textureMatrix(viewToRegion, macro)));
            viewState->addUniform(new osg::Uniform("oe_grime_viewToStreak", textureMatrix(viewToRegion, streak)));
            viewState->addUniform(new osg::Uniform("oe_grime_amount", strength));
            viewState->addUniform(new osg::Uniform("oe_grime_roughness", roughnessParameters));
            viewState->addUniform(new osg::Uniform("oe_grime_smooth", smoothParameters));
            viewState->addUniform(new osg::Uniform("oe_grime_attenuation", attenuationParameters));
            viewState->addUniform(new osg::Uniform("oe_grime_viewToMeters", viewToMeters));
            viewState->addUniform(new osg::Uniform("oe_grime_normalToRegion", normalToRegion));
            cv->pushStateSet(state);
            cv->pushStateSet(viewState);
            traverse(node, nv);
            cv->popStateSet();
            cv->popStateSet();
        }
    };
}

struct GrimeLayer::Impl
{
    // Watch target lifecycle without retaining either the Map or the owning layer.
    struct Observer : MapCallback
    {
        osg::observer_ptr<GrimeLayer> owner;
        //! Reconcile attachments after any map change, including open and close.
        void onMapModelChanged(const MapModelChange&) override
        {
            osg::ref_ptr<GrimeLayer> layer;
            if (owner.lock(layer)) layer->refresh();
        }
    };
    osg::observer_ptr<const Map> map;
    osg::observer_ptr<TiledModelLayer> requestedModel;
    osg::observer_ptr<TiledModelLayer> attachedModel;
    osg::ref_ptr<osg::Node> node;
    osg::ref_ptr<Observer> observer;
    osg::ref_ptr<TerrainResources> resources;
    osg::ref_ptr<osg::Texture3D> texture;
    osg::ref_ptr<GrimeCullCallback> callback;
    std::shared_ptr<TextureImageUnitReservation> reservation;

    //! Remove only the callback we installed; existing target state stays owned by it.
    void detach()
    {
        if (node && callback) node->removeCullCallback(callback);
        node = nullptr;
        callback = nullptr;
        attachedModel = nullptr;
        reservation.reset();
    }
};

Config GrimeLayer::Options::getConfig() const
{
    Config conf = VisibleLayer::Options::getConfig();
    conf.set("model", model()); conf.set("anchor", anchor());
    conf.set("seed", seed()); conf.set("volume_size", volumeSize());
    conf.set("amount", amount()); conf.set("tint", tint());
    conf.set("macro_period", macroPeriod()); conf.set("streak_period", streakPeriod());
    conf.set("roughness", roughness()); conf.set("roughness_amount", roughnessAmount());
    conf.set("roughen_only", roughenOnly());
    conf.set("smooth_threshold", smoothThreshold()); conf.set("smooth_feather", smoothFeather());
    conf.set("smooth_amount", smoothAmount());
    conf.set("max_distance", maxDistance()); conf.set("fade_distance", fadeDistance());
    conf.set("up_attenuation", upAttenuation());
    return conf;
}

void GrimeLayer::Options::fromConfig(const Config& conf)
{
    conf.get("model", model()); conf.get("anchor", anchor());
    conf.get("seed", seed()); conf.get("volume_size", volumeSize());
    conf.get("amount", amount()); conf.get("tint", tint());
    conf.get("macro_period", macroPeriod()); conf.get("streak_period", streakPeriod());
    conf.get("roughness", roughness()); conf.get("roughness_amount", roughnessAmount());
    conf.get("roughen_only", roughenOnly());
    conf.get("smooth_threshold", smoothThreshold()); conf.get("smooth_feather", smoothFeather());
    conf.get("smooth_amount", smoothAmount());
    conf.get("max_distance", maxDistance()); conf.get("fade_distance", fadeDistance());
    conf.get("up_attenuation", upAttenuation());
}

void GrimeLayer::init()
{
    VisibleLayer::init();
    _impl = std::make_shared<Impl>();
    // This layer owns the listener. Publish update-thread visibility changes to
    // cull traversals, including calls made through the VisibleLayer interface.
    onVisibleChanged([this](const VisibleLayer* layer)
    {
        if (_impl->callback) _impl->callback->visible.store(layer->getVisible());
    });
    // This layer owns the listener; publish update-thread opacity changes to cull
    // traversals independently of amount, including changes made while hidden.
    onOpacityChanged([this](const VisibleLayer* layer)
    {
        if (_impl->callback) _impl->callback->opacity.store(layer->getOpacity());
    });
    layerHints().cachePolicy() = CachePolicy::NO_CACHE;
}

GrimeLayer::~GrimeLayer()
{
    if (_impl)
    {
        _impl->detach();
        osg::ref_ptr<const Map> map;
        if (_impl->map.lock(map) && _impl->observer) map->removeMapCallback(_impl->observer);
    }
}

void GrimeLayer::setModelLayer(TiledModelLayer* layer)
{
    _impl->requestedModel = layer;
    if (layer) options().model() = layer->getName();
    refresh();
}

TiledModelLayer* GrimeLayer::getModelLayer() const
{
    return _impl->attachedModel.valid() ? _impl->attachedModel.get() : _impl->requestedModel.get();
}

void GrimeLayer::setAmount(float value)
{
    if (!std::isfinite(value)) return;
    options().amount() = osg::clampBetween(value, 0.0f, 1.0f);
    if (_impl->callback) _impl->callback->amount.store(options().amount().get());
}

float GrimeLayer::getAmount() const { return options().amount().get(); }

void GrimeLayer::setMacroPeriod(double value)
{
    if (!std::isfinite(value) || value <= 0.0) return;
    options().macroPeriod() = value;
    if (_impl->callback)
    {
        std::lock_guard<std::mutex> lock(_impl->callback->parametersMutex);
        _impl->callback->macroPeriod.set(value, value, value);
    }
}

double GrimeLayer::getMacroPeriod() const { return options().macroPeriod().get(); }

void GrimeLayer::setStreakPeriod(const osg::Vec3d& value)
{
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!std::isfinite(value[axis]) || value[axis] <= 0.0) return;
    options().streakPeriod() = value;
    if (_impl->callback)
    {
        std::lock_guard<std::mutex> lock(_impl->callback->parametersMutex);
        _impl->callback->streakPeriod = value;
    }
}

osg::Vec3d GrimeLayer::getStreakPeriod() const { return options().streakPeriod().get(); }

void GrimeLayer::setRoughness(float value)
{
    if (!std::isfinite(value)) return;
    options().roughness() = osg::clampBetween(value, 0.0f, 1.0f);
    if (_impl->callback)
    {
        std::lock_guard<std::mutex> lock(_impl->callback->parametersMutex);
        _impl->callback->roughness.x() = options().roughness().get();
    }
}

float GrimeLayer::getRoughness() const { return options().roughness().get(); }

void GrimeLayer::setRoughnessAmount(float value)
{
    if (!std::isfinite(value)) return;
    options().roughnessAmount() = osg::clampBetween(value, 0.0f, 1.0f);
    if (_impl->callback)
    {
        std::lock_guard<std::mutex> lock(_impl->callback->parametersMutex);
        _impl->callback->roughness.y() = options().roughnessAmount().get();
    }
}

float GrimeLayer::getRoughnessAmount() const { return options().roughnessAmount().get(); }

void GrimeLayer::setRoughenOnly(bool value)
{
    options().roughenOnly() = value;
    if (_impl->callback)
    {
        std::lock_guard<std::mutex> lock(_impl->callback->parametersMutex);
        _impl->callback->roughness.z() = value ? 1.0f : 0.0f;
    }
}

bool GrimeLayer::getRoughenOnly() const { return options().roughenOnly().get(); }

void GrimeLayer::setSmoothThreshold(float value)
{
    if (!std::isfinite(value)) return;
    options().smoothThreshold() = osg::clampBetween(value, 0.0f, 1.0f);
    if (_impl->callback)
    {
        std::lock_guard<std::mutex> lock(_impl->callback->parametersMutex);
        _impl->callback->smooth.x() = options().smoothThreshold().get();
    }
}

float GrimeLayer::getSmoothThreshold() const { return options().smoothThreshold().get(); }

void GrimeLayer::setSmoothFeather(float value)
{
    if (!std::isfinite(value)) return;
    options().smoothFeather() = osg::clampBetween(value, 0.0f, 1.0f);
    if (_impl->callback)
    {
        std::lock_guard<std::mutex> lock(_impl->callback->parametersMutex);
        _impl->callback->smooth.y() = options().smoothFeather().get();
    }
}

float GrimeLayer::getSmoothFeather() const { return options().smoothFeather().get(); }

void GrimeLayer::setSmoothAmount(float value)
{
    if (!std::isfinite(value)) return;
    options().smoothAmount() = osg::clampBetween(value, 0.0f, 1.0f);
    if (_impl->callback)
    {
        std::lock_guard<std::mutex> lock(_impl->callback->parametersMutex);
        _impl->callback->smooth.z() = options().smoothAmount().get();
    }
}

float GrimeLayer::getSmoothAmount() const { return options().smoothAmount().get(); }

void GrimeLayer::setMaxDistance(float value)
{
    if (!std::isfinite(value) || value <= 0.0f) return;
    options().maxDistance() = value;
    if (_impl->callback)
    {
        std::lock_guard<std::mutex> lock(_impl->callback->parametersMutex);
        _impl->callback->attenuation.x() = value;
    }
}

float GrimeLayer::getMaxDistance() const { return options().maxDistance().get(); }

void GrimeLayer::setFadeDistance(float value)
{
    if (!std::isfinite(value) || value < 0.0f) return;
    options().fadeDistance() = value;
    if (_impl->callback)
    {
        std::lock_guard<std::mutex> lock(_impl->callback->parametersMutex);
        _impl->callback->attenuation.y() = value;
    }
}

float GrimeLayer::getFadeDistance() const { return options().fadeDistance().get(); }

void GrimeLayer::setUpAttenuation(float value)
{
    if (!std::isfinite(value)) return;
    options().upAttenuation() = osg::clampBetween(value, 0.0f, 1.0f);
    if (_impl->callback)
    {
        std::lock_guard<std::mutex> lock(_impl->callback->parametersMutex);
        _impl->callback->attenuation.z() = options().upAttenuation().get();
    }
}

float GrimeLayer::getUpAttenuation() const { return options().upAttenuation().get(); }

const osg::Image* GrimeLayer::getNoiseImage() const
{
    return _impl->texture ? _impl->texture->getImage() : nullptr;
}

Status GrimeLayer::openImplementation()
{
    Status status = VisibleLayer::openImplementation();
    if (status.isError()) return status;
    const auto& o = options();
    unsigned size = o.volumeSize().get();
    if (size < 16 || size > 128 || (size & (size-1)) != 0)
        return Status(Status::ConfigurationError, "Grime volume_size must be a power of two in [16,128]");
    if (!o.model().isSet() && !_impl->requestedModel.valid())
        return Status(Status::ConfigurationError, "Grime requires a model layer name or setModelLayer");
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(o.anchor().get()[axis]) || !std::isfinite(o.streakPeriod().get()[axis]) ||
            o.streakPeriod().get()[axis] <= 0.0 || !std::isfinite(o.tint().get()[axis]) ||
            o.tint().get()[axis] < 0.0f || o.tint().get()[axis] > 1.0f)
            return Status(Status::ConfigurationError, "Invalid Grime anchor, streak_period, or tint");
    }
    if (std::abs(o.anchor()->x()) > 180.0 || std::abs(o.anchor()->y()) > 90.0 ||
        !std::isfinite(o.macroPeriod().get()) || o.macroPeriod().get() <= 0.0 ||
        !std::isfinite(o.amount().get()) || o.amount().get() < 0 || o.amount().get() > 1 ||
        !std::isfinite(o.roughness().get()) || o.roughness().get() < 0 || o.roughness().get() > 1 ||
        !std::isfinite(o.roughnessAmount().get()) || o.roughnessAmount().get() < 0 || o.roughnessAmount().get() > 1)
        return Status(Status::ConfigurationError, "Invalid Grime period, coverage, roughness, or WGS84 anchor");
    if (!std::isfinite(o.smoothThreshold().get()) || o.smoothThreshold().get() < 0 || o.smoothThreshold().get() > 1 ||
        !std::isfinite(o.smoothFeather().get()) || o.smoothFeather().get() < 0 || o.smoothFeather().get() > 1 ||
        !std::isfinite(o.smoothAmount().get()) || o.smoothAmount().get() < 0 || o.smoothAmount().get() > 1)
        return Status(Status::ConfigurationError, "Grime smooth-surface controls must be finite and in [0,1]");

    if (!std::isfinite(o.maxDistance().get()) || o.maxDistance().get() <= 0.0f ||
        !std::isfinite(o.fadeDistance().get()) || o.fadeDistance().get() < 0.0f ||
        !std::isfinite(o.upAttenuation().get()) || o.upAttenuation().get() < 0.0f ||
        o.upAttenuation().get() > 1.0f)
        return Status(Status::ConfigurationError, "Invalid Grime max_distance, fade_distance, or up_attenuation");

    _impl->texture = new osg::Texture3D(makeVolume(size, o.seed().get()));
    _impl->texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
    _impl->texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    _impl->texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
    _impl->texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
    _impl->texture->setWrap(osg::Texture::WRAP_R, osg::Texture::REPEAT);
    _impl->texture->setResizeNonPowerOfTwoHint(false);
    // Keep the image for subsequent contexts and context loss recovery.
    _impl->texture->setUnRefImageDataAfterApply(false);
    return Status::NoError;
}

Status GrimeLayer::closeImplementation()
{
    _impl->detach();
    _impl->texture = nullptr;
    return VisibleLayer::closeImplementation();
}

void GrimeLayer::addedToMap(const Map* map)
{
    VisibleLayer::addedToMap(map);
    _impl->map = map;
    if (!_impl->observer)
    {
        _impl->observer = new Impl::Observer;
        _impl->observer->owner = this;
        map->addMapCallback(_impl->observer);
    }
    refresh();
}

void GrimeLayer::removedFromMap(const Map* map)
{
    _impl->detach();
    if (_impl->observer) map->removeMapCallback(_impl->observer);
    _impl->observer = nullptr;
    _impl->map = nullptr;
    _impl->resources = nullptr;
    VisibleLayer::removedFromMap(map);
}

void GrimeLayer::prepareForRendering(TerrainEngine* engine)
{
    VisibleLayer::prepareForRendering(engine);
    _impl->resources = engine->getResources();
    refresh();
}

void GrimeLayer::refresh()
{
    osg::ref_ptr<const Map> map;
    if (!_impl->map.lock(map) || !isOpen() || !_impl->texture || !_impl->resources)
    {
        _impl->detach();
        return;
    }
    osg::ref_ptr<TiledModelLayer> model;
    if (!_impl->requestedModel.lock(model) && options().model().isSet())
        model = map->getLayerByName<TiledModelLayer>(options().model().get());
    if (!model || !model->isOpen() || map->getIndexOfLayer(model) == map->getNumLayers())
    {
        _impl->detach();
        return;
    }
    if (_impl->attachedModel.get() == model.get() && _impl->node == model->getNode()) return;
    _impl->detach();
    if (!model->getNode()) return;

    // Reject accidental stacking rather than silently shadowing another field's uniforms.
    for (auto* cb = model->getNode()->getCullCallback(); cb; cb = cb->getNestedCallback())
        if (dynamic_cast<GrimeCullCallback*>(cb))
        {
            OE_WARN << "[Grime] Target already has a grime effect: " << model->getName() << std::endl;
            return;
        }

    osg::Matrixd worldToRegion;
    GeoPoint anchor(SpatialReference::get("wgs84"), options().anchor().get(), ALTMODE_ABSOLUTE);
    if (!anchor.transformInPlace(map->getSRS()) || !anchor.createWorldToLocal(worldToRegion))
    {
        OE_WARN << "[Grime] Cannot transform anchor into map coordinates" << std::endl;
        return;
    }
    if (map->getSRS()->isProjected())
    {
        // Projected maps may use feet horizontally; GeoPoint altitudes use meters.
        double meters = map->getSRS()->getUnits().convertTo(Units::METERS, 1.0);
        worldToRegion = worldToRegion * osg::Matrixd::scale(meters, meters, 1.0);
    }

    // Reserve through the shared tracker, temporarily skipping the conventional
    // model/material slots. These lower reservations are released before returning.
    std::vector<std::shared_ptr<TextureImageUnitReservation>> skipped;
    auto reservation = std::make_shared<TextureImageUnitReservation>();
    while (_impl->resources->reserveTextureImageUnitForLayer(*reservation, model, "Grime"))
    {
        if (reservation->unit() >= 8) break;
        skipped.push_back(reservation);
        reservation = std::make_shared<TextureImageUnitReservation>();
    }
    if (!reservation->valid())
    {
        OE_WARN << "[Grime] No texture unit available for " << model->getName() << std::endl;
        return;
    }
    auto callback = new GrimeCullCallback;
    callback->worldToRegion = worldToRegion;
    callback->macroPeriod.set(options().macroPeriod().get(), options().macroPeriod().get(), options().macroPeriod().get());
    callback->streakPeriod = options().streakPeriod().get();
    callback->roughness.set(getRoughness(), getRoughnessAmount(), getRoughenOnly() ? 1.0f : 0.0f);
    callback->smooth.set(getSmoothThreshold(), getSmoothFeather(), getSmoothAmount());
    callback->attenuation.set(getMaxDistance(), getFadeDistance(), getUpAttenuation());
    callback->amount.store(options().amount().get());
    callback->opacity.store(getOpacity());
    callback->visible.store(getVisible());
    callback->state = new osg::StateSet;
    callback->state->setTextureAttribute(reservation->unit(), _impl->texture);
    callback->state->addUniform(new osg::Uniform("oe_grime_volume", reservation->unit()));
    callback->state->addUniform(new osg::Uniform("oe_grime_tint", options().tint().get()));
    Shaders shaders;
    shaders.load(VirtualProgram::getOrCreate(callback->state), shaders.GrimeLayer);
    _impl->callback = callback;
    _impl->reservation = reservation;
    _impl->node = model->getNode();
    _impl->attachedModel = model;
    _impl->node->addCullCallback(callback);
}
