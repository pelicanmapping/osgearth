/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/Shadowing>
#include <osgEarth/ShadowingMath.h>
#include <osgEarth/ChonkRenderPass>
#include <osgEarth/ShadowingCache.h>
#include <osgEarth/Shaders>
#include <osgEarth/CullingUtils>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/CameraUtils>
#include <osg/Texture2DArray>
#include <osg/Depth>
#include <osg/ColorMask>
#include <osg/BlendFunc>
#include <array>
#include <map>
#include <mutex>
#include <atomic>

using namespace osgEarth;
using namespace osgEarth::Util;

//! Keeps the historical camera-marking API and both public shader defines.
void Shadowing::setIsShadowCamera(osg::Camera* camera) { CameraUtils::setIsShadowCamera(camera); }
//! Keeps the historical camera query API.
bool Shadowing::isShadowCamera(const osg::Camera* camera) { return CameraUtils::isShadowCamera(camera); }

struct ShadowCaster::Impl
{
    struct Cache : osg::Referenced
    {
        std::vector<std::uint64_t> key;
        std::vector<osg::ref_ptr<const osg::Object>> retained;
        unsigned sequence = 0;
        std::atomic<unsigned> completed{0};
    };
    struct Complete : osg::Camera::DrawCallback
    {
        osg::ref_ptr<Cache> cache;
        unsigned sequence;
        //! Retains the cache until the final queued depth pass has submitted all its commands.
        Complete(Cache* value, unsigned number) : cache(value), sequence(number) { }
        //! Publishes completion to the cull thread; consumers share this context's ordered GL stream.
        void operator()(osg::RenderInfo&) const override { cache->completed.store(sequence); }
    };
    struct Frame
    {
        osg::ref_ptr<osg::Texture2DArray> texture;
        osg::ref_ptr<osg::StateSet> receivers;
        std::vector<osg::ref_ptr<osg::Camera>> cameras;
        osg::ref_ptr<osg::Uniform> matrices, intervals, bias;
        unsigned size = 0;
        int unit = -1;
        std::uint64_t viewID = 0;
        osg::ref_ptr<Cache> cache;
    };
    struct View
    {
        osg::observer_ptr<osg::Camera> camera;
        osg::ref_ptr<osg::Texture2DArray> texture;
        std::array<Frame,2> frames;
        std::uint64_t id = 0;
        osg::ref_ptr<Cache> cache = new Cache;
    };
    std::map<std::pair<osg::Camera*,osgUtil::CullVisitor*>,std::unique_ptr<View>> views;
    mutable std::mutex mutex;
    std::atomic<unsigned> rendered{0}, reused{0};
    //! Builds view-local render targets only when resolution, cascade count or the reserved unit changes.
    void initialize(Frame& f, View& view, ShadowCaster& owner)
    {
        unsigned count = unsigned(owner._ranges.size()-1);
        f = Frame();
        f.size = owner._size;
        f.unit = owner._texImageUnit;
        f.viewID = view.id;
        f.cache = view.cache;
        // GPU commands in one view's context are ordered. Only CPU camera/uniform state needs buffering;
        // sharing the depth target avoids doubling VRAM for cull/draw overlap.
        if (!view.texture || view.texture->getTextureWidth() != int(f.size) ||
            view.texture->getTextureDepth() != int(count))
        {
            view.texture = new osg::Texture2DArray;
            view.texture->setTextureSize(f.size,f.size,count);
            view.texture->setInternalFormat(GL_DEPTH_COMPONENT24);
            view.texture->setSourceFormat(GL_DEPTH_COMPONENT);
            view.texture->setSourceType(GL_UNSIGNED_INT);
            view.texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
            view.texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
            view.texture->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_BORDER);
            view.texture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_BORDER);
            view.texture->setBorderColor(osg::Vec4(1,1,1,1));
            view.texture->setShadowComparison(true);
            view.texture->setShadowCompareFunc(osg::Texture::LEQUAL);
        }
        f.texture = view.texture;
        for (unsigned i=0; i<count; ++i)
        {
            osg::ref_ptr<osg::Camera> camera = new osg::Camera;
            camera->setName("Shadow cascade " + std::to_string(i));
            CameraUtils::setIsShadowCamera(camera);
            camera->setReferenceFrame(osg::Camera::ABSOLUTE_RF_INHERIT_VIEWPOINT);
            camera->setClearDepth(1.0);
            camera->setClearMask(GL_DEPTH_BUFFER_BIT);
            camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
            camera->setViewport(0,0,f.size,f.size);
            camera->setRenderOrder(osg::Camera::PRE_RENDER);
            camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
            camera->setImplicitBufferAttachmentMask(0,0);
            camera->setDrawBuffer(GL_NONE);
            camera->setReadBuffer(GL_NONE);
            camera->attach(osg::Camera::DEPTH_BUFFER,f.texture.get(),0,i);
            camera->addChild(owner._castingGroup);
            camera->addCullCallback(new InstallCameraUniform);
            auto ss = camera->getOrCreateStateSet();
            auto overrideMode = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE;
            ss->setAttributeAndModes(new osg::Depth(osg::Depth::LESS,0.0,1.0,true),overrideMode);
            ss->setAttributeAndModes(new osg::ColorMask(false,false,false,false),overrideMode);
            ss->setMode(GL_BLEND,osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            ss->setDefine("OE_LIGHTING",osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            ss->setDefine("OE_SHADOWING",osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            ss->getOrCreateUniform("oe_shadowToPrimaryMatrix",osg::Uniform::FLOAT_MAT4);
            ss->getOrCreateUniform("oe_primaryProjectionMatrix",osg::Uniform::FLOAT_MAT4);
            ss->getOrCreateUniform("oe_primaryViewport",osg::Uniform::FLOAT_VEC2);
            f.cameras.push_back(camera);
        }
        f.receivers = new osg::StateSet;
        auto vp = VirtualProgram::getOrCreate(f.receivers);
        vp->setName("Stable cascaded shadows");
        Shaders package;
        package.replace("$OE_SHADOW_NUM_SLICES",std::to_string(count));
        package.load(vp,package.ShadowCaster);
        f.receivers->setDefine("OE_SHADOWING");
        f.receivers->setTextureAttribute(f.unit,f.texture,osg::StateAttribute::ON);
        f.receivers->addUniform(new osg::Uniform("oe_shadow_map",f.unit));
        f.matrices = f.receivers->getOrCreateUniform("oe_shadow_matrix",osg::Uniform::FLOAT_MAT4,count);
        f.intervals = f.receivers->getOrCreateUniform("oe_shadow_interval",osg::Uniform::FLOAT_VEC2,count);
        f.bias = f.receivers->getOrCreateUniform("oe_shadow_bias",osg::Uniform::FLOAT,count);
        f.receivers->getOrCreateUniform("oe_shadow_color",osg::Uniform::FLOAT);
        f.receivers->getOrCreateUniform("oe_shadow_blur",osg::Uniform::FLOAT);
        f.receivers->getOrCreateUniform("oe_shadow_filter",osg::Uniform::INT);
        f.receivers->getOrCreateUniform("oe_shadow_blend",osg::Uniform::FLOAT);
        f.receivers->getOrCreateUniform("oe_shadow_light",osg::Uniform::FLOAT_VEC3);
    }

    //! Obtains isolated frame storage; separate cull visitors can prepare separate views concurrently.
    Frame& frame(ShadowCaster& owner, osgUtil::CullVisitor& cv)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto i=views.begin(); i!=views.end(); )
        {
            if (!i->second->camera.valid()) i = views.erase(i);
            else ++i;
        }
        auto camera = cv.getCurrentCamera();
        auto& view = views[std::make_pair(camera,&cv)];
        if (!view)
        {
            view.reset(new View);
            view->id = ChonkRenderPass::createViewID();
            view->camera = camera;
        }
        unsigned number = cv.getFrameStamp() ? cv.getFrameStamp()->getFrameNumber() : 0u;
        Frame& f = view->frames[number%2];
        if (f.size != owner._size || f.unit != owner._texImageUnit || f.cameras.size()+1 != owner._ranges.size())
            initialize(f,*view,owner);
        return f;
    }
};

//! Constructs settings only; GPU resources are allocated on the first eligible cull.
ShadowCaster::ShadowCaster() : _impl(new Impl), _castingGroup(new osg::Group)
{
    _supported = Registry::capabilities().supportsGLSL(130u);
    setTextureSize(_size);
    setCascades(3,2500.0f);
}
ShadowCaster::~ShadowCaster() = default;
//! Reads draw-work diagnostics without racing the cull thread.
unsigned ShadowCaster::getRenderedCascades() const { return _impl->rendered.load(); }
//! Reads map-reuse diagnostics without racing the cull thread.
unsigned ShadowCaster::getReusedCascades() const { return _impl->reused.load(); }
//! Validates all boundaries before changing live settings.
void ShadowCaster::setRanges(const std::vector<float>& ranges)
{
    if (ranges.size() < 2 || ranges.size() > 5) return;
    for (unsigned i=0; i<ranges.size(); ++i)
        if (!std::isfinite(ranges[i]) || ranges[i] < 0.0f || (i && ranges[i] <= ranges[i-1])) return;
    _ranges = ranges;
}

//! Uses fixed practical splits, avoiding terrain near-plane feedback and frame-to-frame resizing.
void ShadowCaster::setCascades(unsigned count, float distance, float lambda)
{
    if (count < 1 || count > 4 || !std::isfinite(distance) || distance < 1.0f ||
        !std::isfinite(lambda) || lambda < 0.0f || lambda > 1.0f) return;
    std::vector<float> ranges(1,0.0f);
    double n = std::max(0.1,double(distance)*0.001);
    for (unsigned i=1; i<count; ++i)
    {
        double fraction = double(i)/count;
        ranges.push_back(float(lambda*n*std::pow(distance/n,fraction)+(1.0-lambda)*distance*fraction));
    }
    ranges.push_back(distance);
    setRanges(ranges);
}

//! Records the application's reserved unit without touching resources already queued for drawing.
void ShadowCaster::setTextureImageUnit(int unit)
{
    if (unit >= 0 && unit < Registry::capabilities().getMaxGPUTextureUnits()) _texImageUnit = unit;
}
//! Bounds resource usage and defers reallocation to each view's next cull.
void ShadowCaster::setTextureSize(unsigned size)
{
    unsigned limit = unsigned(std::max(256,Registry::capabilities().getMaxTextureSize()));
    _size = std::min(osg::clampBetween(size,256u,8192u),limit);
}
//! Rejects NaN and clamps direct-sun visibility.
void ShadowCaster::setShadowColor(float value)
{
    if (std::isfinite(value)) _color = osg::clampBetween(value,0.0f,1.0f);
}
//! Converts the legacy normalized radius to resolution-independent texel units.
void ShadowCaster::setBlurFactor(float value) { setFilterRadius(value*float(_size)); }
//! Rejects invalid filter enum values without rebuilding shader programs.
void ShadowCaster::setFilter(Filter value) { if (value >= HARD && value <= SOFT) _filter = value; }
//! Rejects nonfinite radii and bounds the filter footprint used for projection guards.
void ShadowCaster::setFilterRadius(float value)
{
    if (std::isfinite(value)) _filterRadius = osg::clampBetween(value,0.0f,8.0f);
}
//! Bounds receiver bias; each cascade converts texels to normalized light depth during cull.
void ShadowCaster::setDepthBias(float value)
{
    if (std::isfinite(value)) _depthBias = osg::clampBetween(value,0.0f,10.0f);
}
//! Rejects nonfinite distances and prevents negative caster extrusion.
void ShadowCaster::setCasterDistance(float value)
{
    if (std::isfinite(value)) _casterDistance = osg::clampBetween(value,0.0f,1000000.0f);
}
//! Bounds overlap so adjacent intervals remain useful independently.
void ShadowCaster::setCascadeBlend(float value)
{
    if (std::isfinite(value)) _cascadeBlend = osg::clampBetween(value,0.0f,0.3f);
}

//! Preserves subgraph lifecycle propagation and resizes the privately owned render resources.
void ShadowCaster::resizeGLObjectBuffers(unsigned size)
{
    osg::Group::resizeGLObjectBuffers(size);
    std::lock_guard<std::mutex> lock(_impl->mutex);
    for (auto& view : _impl->views)
    for (auto& f : view.second->frames)
    {
        if (f.receivers) f.receivers->resizeGLObjectBuffers(size);
        for (auto& camera : f.cameras) camera->resizeGLObjectBuffers(size);
    }
}

//! Releases all view/context resources while keeping CPU descriptions ready for context recreation.
void ShadowCaster::releaseGLObjects(osg::State* state) const
{
    osg::Group::releaseGLObjects(state);
    std::lock_guard<std::mutex> lock(_impl->mutex);
    for (auto& view : _impl->views)
    for (auto& f : view.second->frames)
    {
        if (f.cache) { f.cache->key.clear(); f.cache->completed = 0; }
        if (f.receivers) f.receivers->releaseGLObjects(state);
        for (auto& camera : f.cameras) camera->releaseGLObjects(state);
    }
}

//! Renders valid receiver slices, skipping auxiliary cameras and retaining ordinary traversal on every fallback.
void ShadowCaster::traverse(osg::NodeVisitor& nv)
{
    auto cv = nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR ? Culling::asCullVisitor(nv) : nullptr;
    auto camera = cv ? cv->getCurrentCamera() : nullptr;
    auto ss = camera ? camera->getStateSet() : nullptr;
    bool auxiliary = ss && (ss->getDefinePair("OE_IS_SHADOW_CAMERA") ||
        ss->getDefinePair("OE_IS_DEPTH_CAMERA") || ss->getDefinePair("OE_IS_PICK_CAMERA"));
    if (!_supported || !_enabled || !_light || !camera || auxiliary || !_castingGroup->getNumChildren() ||
        (cv->getTraversalMask() & _traversalMask) == 0u)
    {
        osg::Group::traverse(nv);
        return;
    }
    const auto& position = _light->getPosition();
    osg::Vec3d light(position.x(),position.y(),position.z());
    if (!std::isfinite(light.length2()) || light.length2() < 1e-12)
    {
        osg::Group::traverse(nv);
        return;
    }
    light.normalize();
    osg::Matrixd inverseView;
    if (!inverseView.invert(*cv->getModelViewMatrix()))
    {
        osg::Group::traverse(nv);
        return;
    }
    auto& f = _impl->frame(*this,*cv);
    unsigned count = unsigned(f.cameras.size());
    std::array<bool,4> active{};
    bool anyActive = false;
    osg::Matrixd projection = *cv->getProjectionMatrix();
    ChonkRenderPass::Parameters batch;
    batch.viewID = f.viewID;
    batch.frameNumber = cv->getFrameStamp() ? cv->getFrameStamp()->getFrameNumber() : 0;
    batch.count = count;
    batch.lodView = *cv->getModelViewMatrix();
    batch.lodProjection = projection;
    auto lodViewport = cv->getViewport();
    batch.lodViewport = lodViewport ?
        osg::Vec2f(lodViewport->width(),lodViewport->height()) : osg::Vec2f(1,1);
    batch.coreDraws = _submission == CORE;
    batch.retainOutsideLODView = true;
    for (unsigned i=0; i<count; ++i)
    {
        double start = _ranges[i];
        if (i > 0) start -= _cascadeBlend*(_ranges[i]-_ranges[i-1]);
        ShadowMath::Corners points;
        ShadowMath::Cascade fit;
        active[i] = ShadowMath::corners(projection,start,_ranges[i+1],points) &&
            ShadowMath::fit(points,inverseView,light,_size,_casterDistance,_filterRadius,fit);
        anyActive |= active[i];
        f.intervals->setElement(i,osg::Vec2(_ranges[i],_ranges[i+1]));
        // An invalid slice samples outside the map, never stale contents from a previous view.
        f.matrices->setElement(i,active[i] ? fit.viewToTexture :
            osg::Matrixd::scale(0,0,0)*osg::Matrixd::translate(-1,-1,-1));
        f.bias->setElement(i,active[i] ? float(_depthBias*fit.texelSize/fit.depthSpan) : 0.0f);
        if (!active[i]) continue;
        auto pass = f.cameras[i].get();
        pass->setViewMatrix(fit.view);
        pass->setProjectionMatrix(fit.projection);
        batch.clipFromWorld[i] = fit.view*fit.projection;
        batch.activeViews |= 1u << i;
        auto state = pass->getOrCreateStateSet();
        state->getUniform("oe_shadowToPrimaryMatrix")->set(osg::Matrixd::inverse(fit.view)*(*cv->getModelViewMatrix()));
        state->getUniform("oe_primaryProjectionMatrix")->set(projection);
        auto viewport = cv->getViewport();
        state->getUniform("oe_primaryViewport")->set(viewport ?
            osg::Vec2(viewport->width(),viewport->height()) : osg::Vec2(1,1));
    }
    if (!anyActive)
    {
        osg::Group::traverse(nv);
        return;
    }
    f.receivers->getUniform("oe_shadow_color")->set(_color);
    f.receivers->getUniform("oe_shadow_blur")->set(getBlurFactor());
    f.receivers->getUniform("oe_shadow_filter")->set(int(_filter));
    f.receivers->getUniform("oe_shadow_blend")->set(_cascadeBlend);
    osg::Vec3d lightView = osg::Matrixd::transform3x3(light,*cv->getModelViewMatrix());
    lightView.normalize();
    f.receivers->getUniform("oe_shadow_light")->set(osg::Vec3(lightView));
    unsigned mask = cv->getTraversalMask();
    ShadowDetail::Snapshot snapshot;
    if (_cacheEnabled)
    {
        snapshot.time = cv->getFrameStamp() ? cv->getFrameStamp()->getReferenceTime() : 0.0;
        _castingGroup->accept(snapshot);
        for (auto graph = cv->getCurrentStateGraph(); graph; graph = graph->_parent)
            snapshot.state(graph->getStateSet());
    }
    bool modern = _submission != LEGACY && cv->getFrameStamp() && Registry::capabilities().supportsGLSL(460u);
    bool reuse = false;
    if (_cacheEnabled)
    {
        snapshot.value(_cacheRevision);
        snapshot.value(mask & _traversalMask);
        snapshot.value(f.texture.get());
        snapshot.value(camera->getGraphicsContext());
        snapshot.value(cv->getState()->getContextID());
        snapshot.value(int(_submission));
        auto viewport = cv->getViewport();
        if (viewport) { snapshot.value(viewport->width()); snapshot.value(viewport->height()); }
        for (unsigned i=0; i<16; ++i)
        {
            snapshot.value(batch.lodView.ptr()[i]);
            snapshot.value(projection.ptr()[i]);
            for (unsigned j=0; j<count; ++j)
            {
                snapshot.value(batch.clipFromWorld[j].ptr()[i]);
            }
        }
        snapshot.value(batch.activeViews);
        reuse = snapshot.safe && !f.cache->key.empty() && snapshot.key == f.cache->key &&
            f.cache->completed.load() == f.cache->sequence;
        if (!reuse)
        {
            f.cache->key = snapshot.safe ? std::move(snapshot.key) : std::vector<std::uint64_t>();
            f.cache->retained = std::move(snapshot.retained);
        }
    }
    else f.cache->key.clear();
    unsigned rendered = 0, last = 0;
    for (unsigned i=0; i<count; ++i) if (active[i]) { ++rendered; last = i; }
    _impl->rendered = reuse ? 0 : rendered;
    _impl->reused = reuse ? rendered : 0;
    if (!reuse) ++f.cache->sequence;
    cv->setTraversalMask(mask & _traversalMask);
    if (!reuse)
    {
        auto renderBatch = modern ? ChonkRenderPass::createBatch(batch) : nullptr;
        for (unsigned i=0; i<count; ++i)
        {
            if (!active[i]) continue;
            auto state = f.cameras[i]->getOrCreateStateSet();
            osg::ref_ptr<ChonkRenderPass> packet = modern ?
                new ChonkRenderPass(renderBatch,i,f.cameras[i]->getViewMatrix()) : nullptr;
            ChonkRenderPass::set(state,packet);
            f.cameras[i]->setFinalDrawCallback(i == last ? new Impl::Complete(f.cache,f.cache->sequence) : nullptr);
            f.cameras[i]->accept(nv);
        }
    }
    cv->setTraversalMask(mask);
    cv->pushStateSet(f.receivers);
    osg::Group::traverse(nv);
    cv->popStateSet();
}
