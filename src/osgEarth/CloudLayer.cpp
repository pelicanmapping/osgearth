/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/CloudLayer>
#include <osgEarth/Capabilities>
#include <osgEarth/Shaders>
#include <osgEarth/CameraUtils>
#include <osgEarth/WindLayer>
#include "CloudLayerRenderer.h"
#include <osg/BindImageTexture>
#include <osg/DispatchCompute>
#include <osg/Texture2D>
#include <osg/Texture3D>
#include <osg/Program>
#include <osg/GLExtensions>
#include <array>
#include <map>
#include <mutex>
#include <cmath>
#include <cstdint>

using namespace osgEarth;

CloudLayer::~CloudLayer() = default;
void CloudLayer::setWindLayer(WindLayer* layer) { _windLayer = layer; ++_revision; }
WindLayer* CloudLayer::getWindLayer() const { return _windLayer.get(); }

namespace
{
    //! Sanitizes a scalar without allowing NaNs to enter shader uniforms.
    float bounded(float value, float lo, float hi, float fallback)
    {
        return std::isfinite(value) ? std::max(lo,std::min(hi,value)) : fallback;
    }

    //! Deterministic integer lattice hash, independent of the platform's random-number implementation.
    float hash(std::uint32_t x, std::uint32_t y, std::uint32_t z)
    {
        std::uint32_t n = std::uint32_t(x)*1597334677u ^ std::uint32_t(y)*3812015801u ^ std::uint32_t(z)*2798796415u;
        n ^= n >> 16; n *= 2246822519u; n ^= n >> 13;
        return float(n & 0x00ffffffu)/16777215.0f;
    }

    //! Periodic smooth value noise used only to initialize the immutable shared texture.
    float noise(float x, float y, float z, int period)
    {
        int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
        float u = x-ix, v = y-iy, w = z-iz;
        u = u*u*(3.0f-2.0f*u); v = v*v*(3.0f-2.0f*v); w = w*w*(3.0f-2.0f*w);
        float sum = 0.0f;
        for (int k=0; k<2; ++k)
        for (int j=0; j<2; ++j)
        for (int i=0; i<2; ++i)
            sum += hash((ix+i)%period,(iy+j)%period,(iz+k)%period)*(i ? u : 1-u)*(j ? v : 1-v)*(k ? w : 1-w);
        return sum;
    }

    //! Generates shared tileable noise and second moments; mip levels retain unresolved shape/erosion variance.
    osg::Texture3D* noiseTexture()
    {
        static osg::ref_ptr<osg::Texture3D> texture = []()
        {
            constexpr int size = 64, cells = 8;
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(size,size,size,GL_RGBA,GL_FLOAT);
            for (int z=0; z<size; ++z)
            for (int y=0; y<size; ++y)
            for (int x=0; x<size; ++x)
            {
                float px = (x+0.5f)*cells/size, py = (y+0.5f)*cells/size, pz = (z+0.5f)*cells/size;
                float closest = 4.0f;
                for (int k=-1; k<=1; ++k)
                for (int j=-1; j<=1; ++j)
                for (int i=-1; i<=1; ++i)
                {
                    int cx = int(px)+i, cy = int(py)+j, cz = int(pz)+k;
                    int hx = (cx+cells)%cells, hy = (cy+cells)%cells, hz = (cz+cells)%cells;
                    float dx = cx+hash(hx,hy,hz)-px;
                    float dy = cy+hash(hy,hz,hx+13)-py;
                    float dz = cz+hash(hz,hx,hy+31)-pz;
                    closest = std::min(closest,dx*dx+dy*dy+dz*dz);
                }
                float value = 0.65f*noise(px*0.5f,py*0.5f,pz*0.5f,4)+0.35f*noise(px,py,pz,8);
                auto pixel = reinterpret_cast<float*>(image->data(x,y,z));
                // Retain the original quantized field so enabling filtering does not regenerate the weather.
                pixel[0] = std::floor(255.0f*value)/255.0f;
                pixel[1] = std::floor(255.0f*std::max(0.0f,1.0f-std::sqrt(closest)))/255.0f;
                float shape = pixel[0]*0.65f+pixel[1]*0.35f;
                pixel[2] = shape*shape;
                pixel[3] = pixel[1]*pixel[1];
            }
            osg::ref_ptr<osg::Texture3D> result = new osg::Texture3D(image);
            result->setInternalFormat(GL_RGBA16F_ARB);
            result->setUseHardwareMipMapGeneration(true);
            result->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);
            result->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
            for (auto axis : {osg::Texture::WRAP_S,osg::Texture::WRAP_T,osg::Texture::WRAP_R})
                result->setWrap(axis,osg::Texture::REPEAT);
            result->setResizeNonPowerOfTwoHint(false);
            return result;
        }();
        return texture.get();
    }

    //! Compute drawable with a texture-fetch dependency before any subsequent sampler reads.
    struct Dispatch : osg::DispatchCompute
    {
        //! Creates a bounded 8x8 dispatch with no scene bounds or geometry submission.
        Dispatch(unsigned width, unsigned height) : osg::DispatchCompute((width+7)/8,(height+7)/8,1)
        {
            setUseDisplayList(false);
            setCullingActive(false);
        }
        //! Runs only on OSG's draw thread; BindImageTexture owns image binding/state tracking.
        void drawImplementation(osg::RenderInfo& info) const override
        {
            auto gl = info.getState()->get<osg::GLExtensions>();
            osg::DispatchCompute::drawImplementation(info);
            gl->glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }
    };

    //! Creates an off-graph ordered compute stage; the 1x1 FBO isolates inherited framebuffer state.
    osg::Camera* computePass(osg::Texture* output, osg::Program* program, unsigned width, unsigned height,
        bool volume, unsigned noiseUnit, GLenum format = 0, bool bindNoise = true)
    {
        osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        camera->setName(volume ? "Cloud transport" : "Cloud shadows");
        camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        camera->setViewMatrix(osg::Matrix::identity());
        camera->setProjectionMatrix(osg::Matrix::identity());
        camera->setViewport(0,0,1,1);
        camera->setRenderOrder(osg::Camera::PRE_RENDER,-102);
        camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        camera->setImplicitBufferAttachmentMask(0,0);
        camera->attach(osg::Camera::COLOR_BUFFER,GL_RGBA8);
        camera->setClearMask(0);
        camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        camera->setCullingMode(osg::CullSettings::NO_CULLING);
        camera->setAllowEventFocus(false);
        osg::ref_ptr<Dispatch> dispatch = new Dispatch(width,height);
        auto ss = dispatch->getOrCreateStateSet();
        ss->setAttributeAndModes(program,osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
        ss->setAttribute(new osg::BindImageTexture(0,output,osg::BindImageTexture::WRITE_ONLY,
            format ? format : volume ? GL_RGBA16F_ARB : GL_R16F,0,volume));
        // Reuse our reserved volume unit: compute writes the volume as an image and never samples it.
        // A fixed unit could alias an active sampler in the host's lighting adapter.
        if (bindNoise)
        {
            ss->setTextureAttributeAndModes(noiseUnit,noiseTexture(),osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
            ss->addUniform(new osg::Uniform("oe_cloud_noise",int(noiseUnit)));
        }
        camera->addChild(dispatch);
        return camera.release();
    }
}

CloudLayer::Options::Options(const ConfigOptions& input) : ConfigOptions(input)
{
    const auto& c = input.getConfig();
    std::string preset;
    c.get("quality",preset);
    if (preset == "low") quality = LOW;
    else if (preset == "high") quality = HIGH;
    c.get("enabled",enabled); c.get("coverage",coverage); c.get("density",density);
    c.get("base_altitude",baseAltitude); c.get("top_altitude",topAltitude); c.get("size",size);
    c.get("fade_start_altitude",fadeStartAltitude); c.get("fade_end_altitude",fadeEndAltitude);
    c.get("erosion",erosion); c.get("shadow_strength",shadowStrength); c.get("seed",seed);
    c.get("shadow_coverage",shadowCoverage);
    c.get("far_shadows",farShadows); c.get("far_shadow_coverage",farShadowCoverage);
    c.get("detail_filtering",detailFiltering);
    c.get("crepuscular_rays",crepuscularRays); c.get("ray_strength",rayStrength); c.get("ray_distance",rayDistance);
    c.get("ray_haze",rayHaze);
    c.get("ray_intensity",rayIntensity);
    preset.clear(); c.get("ray_quality",preset);
    if (preset == "low") rayQuality = LOW;
    else if (preset == "high") rayQuality = HIGH;
    c.get("wind_x",wind.x()); c.get("wind_y",wind.y()); c.get("wind_z",wind.z());
    c.get("resolution",resolution); c.get("samples",samples); c.get("light_samples",lightSamples);
    c.get("depth_slices",depthSlices);
}

Config CloudLayer::Options::getConfig() const
{
    Config c("clouds");
    c.set("quality",quality == LOW ? "low" : quality == HIGH ? "high" : "balanced");
    c.set("enabled",enabled); c.set("coverage",coverage); c.set("density",density);
    c.set("base_altitude",baseAltitude); c.set("top_altitude",topAltitude); c.set("size",size);
    c.set("fade_start_altitude",fadeStartAltitude); c.set("fade_end_altitude",fadeEndAltitude);
    c.set("erosion",erosion); c.set("shadow_strength",shadowStrength); c.set("seed",seed);
    c.set("shadow_coverage",shadowCoverage);
    c.set("far_shadows",farShadows); c.set("far_shadow_coverage",farShadowCoverage);
    c.set("detail_filtering",detailFiltering);
    c.set("crepuscular_rays",crepuscularRays); c.set("ray_strength",rayStrength); c.set("ray_distance",rayDistance);
    c.set("ray_haze",rayHaze);
    c.set("ray_intensity",rayIntensity);
    c.set("ray_quality",rayQuality == LOW ? "low" : rayQuality == HIGH ? "high" : "balanced");
    c.set("wind_x",wind.x()); c.set("wind_y",wind.y()); c.set("wind_z",wind.z());
    c.set("resolution",resolution); c.set("samples",samples); c.set("light_samples",lightSamples);
    c.set("depth_slices",depthSlices);
    return c;
}

CloudLayer::CloudLayer(const Options& options) { setOptions(options); }
const CloudLayer::Options& CloudLayer::getOptions() const { return _options; }
unsigned CloudLayer::getRevision() const { return _revision; }

float CloudLayer::getVisibility(double altitude) const
{
    if (!std::isfinite(altitude) || altitude >= _options.fadeEndAltitude) return 0.0f;
    if (altitude <= _options.fadeStartAltitude) return 1.0f;
    double t = (altitude-_options.fadeStartAltitude)/(_options.fadeEndAltitude-_options.fadeStartAltitude);
    return float(1.0-t*t*(3.0-2.0*t));
}

void CloudLayer::setOptions(const Options& input)
{
    _options = input;
    if (_options.quality != LOW && _options.quality != BALANCED && _options.quality != HIGH) _options.quality = BALANCED;
    _options.coverage = bounded(input.coverage,0.0f,1.0f,0.55f);
    _options.density = bounded(input.density,0.0f,10.0f,1.0f);
    _options.baseAltitude = bounded(input.baseAltitude,0.0f,19000.0f,1500.0f);
    _options.topAltitude = bounded(input.topAltitude,_options.baseAltitude+100.0f,20000.0f,
        std::max(4500.0f,_options.baseAltitude+100.0f));
    _options.fadeStartAltitude = bounded(input.fadeStartAltitude,_options.topAltitude,1000000.0f,20000.0f);
    _options.fadeEndAltitude = bounded(input.fadeEndAltitude,_options.fadeStartAltitude+100.0f,10000000.0f,
        std::max(100000.0f,_options.fadeStartAltitude+100.0f));
    _options.size = bounded(input.size,200.0f,50000.0f,4000.0f);
    _options.erosion = bounded(input.erosion,0.0f,1.0f,0.3f);
    _options.shadowStrength = bounded(input.shadowStrength,0.0f,1.0f,0.8f);
    _options.shadowCoverage = bounded(input.shadowCoverage,1000.0f,200000.0f,20000.0f);
    float minimumFarCoverage = _options.shadowCoverage*1.25f;
    _options.farShadowCoverage = bounded(input.farShadowCoverage,minimumFarCoverage,1000000.0f,
        std::max(100000.0f,minimumFarCoverage));
    if (input.rayQuality != LOW && input.rayQuality != BALANCED && input.rayQuality != HIGH) _options.rayQuality = BALANCED;
    _options.rayStrength = bounded(input.rayStrength,0.0f,1.0f,1.0f);
    _options.rayDistance = bounded(input.rayDistance,1000.0f,100000.0f,50000.0f);
    _options.rayHaze = bounded(input.rayHaze,0.0f,4.0f,0.35f);
    _options.rayIntensity = bounded(input.rayIntensity,0.0f,8.0f,3.0f);
    for (unsigned i=0; i<3; ++i) _options.wind[i] = bounded(input.wind[i],-500.0f,500.0f,0.0f);
    if (input.resolution) _options.resolution = std::max(64u,std::min(768u,input.resolution));
    if (input.samples) _options.samples = std::max(16u,std::min(512u,input.samples));
    if (input.lightSamples) _options.lightSamples = std::max(1u,std::min(16u,input.lightSamples));
    if (input.depthSlices) _options.depthSlices = std::max(8u,std::min(64u,input.depthSlices));
    ++_revision;
}

struct CloudLayerRenderer::Impl
{
    osg::ref_ptr<CloudLayer> layer;
    osg::ref_ptr<TerrainResources> resources;
    TextureImageUnitReservation volumeUnit, shadowUnit, raysUnit;
    osg::ref_ptr<osg::Program> volumeProgram, shadowProgram, sunProgram, raysProgram;
    std::string lightingAdapter;
    bool raysUnavailable = false;
    osg::ref_ptr<osg::StateSet> disabledState;
    bool available = false;
    struct Slot
    {
        osg::ref_ptr<osg::StateSet> state, environmentState;
        osg::ref_ptr<osg::Camera> volumePass, shadowPass, environmentPass;
        osg::ref_ptr<osg::Camera> sunPass, raysPass, environmentRaysPass;
        unsigned rayWidth = 0, rayHeight = 0, rayDepth = 0, sunWidth = 0;
        unsigned width = 0, height = 0, depth = 0, frame = ~0u, shadowWidth = 0;
    };
    struct View
    {
        osg::observer_ptr<osg::Camera> camera;
        std::array<Slot,2> slots;
        osg::Vec3d shadowAnchor;
        osg::Matrix3 shadowBasis;
        float shadowRadius = 0.0f;
    };
    std::map<std::pair<osg::Camera*,osgUtil::CullVisitor*>,View> views;
    mutable std::mutex mutex;

    //! Builds immutable compute programs around the host-supplied physical lighting adapter.
    Impl(CloudLayer* l, TerrainResources* r, const std::string& adapter) :
        layer(l), resources(r), lightingAdapter(adapter)
    {
        if (Capabilities::get().getGLSLVersionInt() < 430 || !r ||
            !r->reserveTextureImageUnit(volumeUnit,"Cloud transport") ||
            !r->reserveTextureImageUnit(shadowUnit,"Cloud shadows"))
        {
            volumeUnit.release(); shadowUnit.release();
            OE_WARN << "[CloudLayer] OpenGL 4.3 and two free texture units are required\n";
            return;
        }
        // Distinct sampler assignments remain necessary even when the shader's cloud branch is disabled.
        disabledState = new osg::StateSet;
        disabledState->addUniform(new osg::Uniform("oe_cloud_enabled",false));
        disabledState->addUniform(new osg::Uniform("oe_cloud_fade",0.0f));
        disabledState->addUniform(new osg::Uniform("oe_cloud_volume",volumeUnit.unit()));
        disabledState->addUniform(new osg::Uniform("oe_cloud_shadowMap",shadowUnit.unit()));
        disabledState->addUniform(new osg::Uniform("oe_cloud_raysEnabled",false));
        disabledState->addUniform(new osg::Uniform("oe_cloud_raysVolume",volumeUnit.unit()));
        Shaders shaders;
        for (unsigned i=0; i<2; ++i)
        {
            std::string source = "#version 430\n#define OE_CLOUD_LAYER\n";
            if (i) source += "#define OE_CLOUD_SHADOW_PASS\n";
            source += adapter+shaders.context().at("CloudLayer.Common.glsl")+
                shaders.context().at("CloudLayer.Density.glsl")+shaders.context().at("CloudLayer.Compute.glsl");
            osg::ref_ptr<osg::Program> program = new osg::Program;
            program->setName(i ? "Cloud shadow compute" : "Cloud transport compute");
            program->addShader(new osg::Shader(osg::Shader::COMPUTE,source));
            if (i) shadowProgram = program; else volumeProgram = program;
        }
        available = true;
    }

    //! Removes dead camera caches even while all clouds are hidden; the caller holds mutex.
    void pruneViews()
    {
        for (auto i = views.begin(); i != views.end();)
            if (!i->second.camera.valid()) i = views.erase(i); else ++i;
    }

    //! Replaces only this slot's shadow atlas under mutex; no extra sampler or cloud-volume allocation is needed.
    void initializeShadows(Slot& slot, unsigned width)
    {
        osg::ref_ptr<osg::Texture2D> shadow = new osg::Texture2D;
        shadow->setTextureSize(width,256); shadow->setInternalFormat(GL_R16F);
        shadow->setSourceFormat(GL_RED); shadow->setSourceType(GL_FLOAT);
        shadow->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
        shadow->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
        shadow->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);
        shadow->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
        shadow->setResizeNonPowerOfTwoHint(false);
        slot.state->setTextureAttributeAndModes(shadowUnit.unit(),shadow);
        slot.shadowPass = computePass(shadow,shadowProgram,width,256,false,volumeUnit.unit());
        slot.shadowWidth = width;
        slot.frame = ~0u;
    }

    //! Anchors the far grid in world space and snaps translation to texels; caller holds the per-view mutex.
    void updateFarShadow(View& view, Slot& slot, const Frame& input, float coverage)
    {
        if (coverage <= 0.0f)
        {
            slot.state->getUniform("oe_cloud_shadowFarOrigin")->set(osg::Vec4());
            return;
        }
        osg::Vec3d up(input.eye); up.normalize();
        osg::Vec3d anchorUp = view.shadowAnchor; anchorUp.normalize();
        // Keep a fixed tangent plane through local camera motion; reanchor after globe-scale travel or radius changes.
        if (view.shadowRadius != input.radius || up*anchorUp < 0.995)
        {
            view.shadowAnchor = up*(input.radius+0.001);
            view.shadowBasis = input.basis;
            view.shadowRadius = input.radius;
        }
        osg::Vec3d right, north, normal;
        for (unsigned i=0; i<3; ++i)
        {
            right[i] = view.shadowBasis(0,i);
            north[i] = view.shadowBasis(1,i);
            normal[i] = view.shadowBasis(2,i);
        }
        osg::Vec3d eye(input.eye);
        osg::Vec3d delta = eye*((view.shadowAnchor*normal)/(eye*normal))-view.shadowAnchor;
        double texel = coverage*0.001/256.0;
        osg::Vec3d center = view.shadowAnchor+right*(std::round(delta*right/texel)*texel)+
            north*(std::round(delta*north/texel)*texel);
        slot.state->getUniform("oe_cloud_shadowFarOrigin")->set(osg::Vec4(osg::Vec3(center),coverage*0.0005f));
        slot.state->getUniform("oe_cloud_shadowFarBasis")->set(view.shadowBasis);
    }

    //! Allocates a slot's bounded volume and shadow target; rebuilding only this slot preserves pipelined draws.
    void initialize(Slot& slot, unsigned width, unsigned height, unsigned depth)
    {
        slot = Slot(); slot.width = width; slot.height = height; slot.depth = depth;
        slot.state = new osg::StateSet;
        osg::ref_ptr<osg::Texture3D> volume = new osg::Texture3D;
        volume->setTextureSize(width,height,depth);
        volume->setInternalFormat(GL_RGBA16F_ARB);
        volume->setSourceFormat(GL_RGBA); volume->setSourceType(GL_FLOAT);
        volume->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
        volume->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
        volume->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);
        volume->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
        volume->setWrap(osg::Texture::WRAP_R,osg::Texture::CLAMP_TO_EDGE);
        volume->setResizeNonPowerOfTwoHint(false);
        slot.state->setTextureAttributeAndModes(volumeUnit.unit(),volume);
        slot.state->addUniform(new osg::Uniform("oe_cloud_volume",volumeUnit.unit()));
        slot.state->addUniform(new osg::Uniform("oe_cloud_shadowMap",shadowUnit.unit()));
        slot.state->addUniform(new osg::Uniform("oe_cloud_raysEnabled",false));
        slot.state->addUniform(new osg::Uniform("oe_cloud_raysVolume",volumeUnit.unit()));
        slot.state->addUniform(new osg::Uniform("oe_cloud_raySettings",osg::Vec4()));
        slot.state->addUniform(new osg::Uniform("oe_cloud_rayIntensity",1.0f));
        slot.state->addUniform(new osg::Uniform("oe_cloud_rayGrid",osg::Vec3()));
        slot.state->addUniform(new osg::Uniform("oe_cloud_sunSamples",64));
        slot.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT3,"oe_cloud_sunBasis"));
        slot.state->addUniform(new osg::Uniform("oe_cloud_enabled",false));
        slot.state->addUniform(new osg::Uniform("oe_cloud_fade",1.0f));
        slot.state->addUniform(new osg::Uniform("oe_cloud_eye",osg::Vec3()));
        slot.state->addUniform(new osg::Uniform("oe_cloud_sun",osg::Vec3()));
        slot.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT3,"oe_cloud_basis"));
        slot.state->addUniform(new osg::Uniform("oe_cloud_shell",osg::Vec4()));
        slot.state->addUniform(new osg::Uniform("oe_cloud_shape",osg::Vec4()));
        slot.state->addUniform(new osg::Uniform("oe_cloud_grid",osg::Vec4()));
        slot.state->addUniform(new osg::Uniform("oe_cloud_wind",osg::Vec3()));
        slot.state->addUniform(new osg::Uniform("oe_cloud_advection",osg::Vec4(0,0,1,0)));
        slot.state->addUniform(new osg::Uniform("oe_cloud_seed",osg::Vec3()));
        slot.state->addUniform(new osg::Uniform("oe_cloud_shadowOrigin",osg::Vec4()));
        slot.state->addUniform(new osg::Uniform("oe_cloud_shadowFarOrigin",osg::Vec4()));
        slot.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT3,"oe_cloud_shadowFarBasis"));
        slot.state->addUniform(new osg::Uniform("oe_cloud_samples",64));
        slot.state->addUniform(new osg::Uniform("oe_cloud_lightSamples",4));
        slot.state->addUniform(new osg::Uniform("oe_cloud_detailFiltering",true));
        slot.state->addUniform(new osg::Uniform("oe_cloud_screenSpace",true));
        slot.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT3,"oe_cloud_viewToEarth"));
        slot.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT3,"oe_cloud_earthToView"));
        slot.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT4,"oe_cloud_projection"));
        slot.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT4,"oe_cloud_inverseProjection"));
        slot.volumePass = computePass(volume,volumeProgram,width,height,true,volumeUnit.unit());
        initializeShadows(slot,256);
        osg::ref_ptr<osg::Texture3D> environment = new osg::Texture3D;
        environment->setTextureSize(64,32,2);
        environment->setInternalFormat(GL_RGBA16F_ARB);
        environment->setSourceFormat(GL_RGBA); environment->setSourceType(GL_FLOAT);
        environment->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
        environment->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
        environment->setWrap(osg::Texture::WRAP_S,osg::Texture::REPEAT);
        environment->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
        environment->setWrap(osg::Texture::WRAP_R,osg::Texture::CLAMP_TO_EDGE);
        slot.environmentState = new osg::StateSet;
        slot.environmentState->setTextureAttributeAndModes(volumeUnit.unit(),environment);
        slot.environmentState->addUniform(new osg::Uniform("oe_cloud_screenSpace",false));
        slot.environmentState->addUniform(new osg::Uniform("oe_cloud_grid",osg::Vec4(64,32,2,0)));
        slot.environmentState->addUniform(new osg::Uniform("oe_cloud_samples",48));
        slot.environmentPass = computePass(environment,volumeProgram,64,32,true,volumeUnit.unit());
        slot.environmentPass->setStateSet(slot.environmentState);
    }

    //! Creates filtered, non-mipmapped cumulative volumes; private passes and state sets retain ownership.
    osg::Texture3D* rayTexture(unsigned width, unsigned height, unsigned depth, bool singleChannel)
    {
        osg::ref_ptr<osg::Texture3D> texture = new osg::Texture3D;
        texture->setTextureSize(width,height,depth);
        texture->setInternalFormat(singleChannel ? GL_R16F : GL_RGBA16F_ARB);
        texture->setSourceFormat(singleChannel ? GL_RED : GL_RGBA);
        texture->setSourceType(GL_FLOAT);
        texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
        texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
        for (auto axis : {osg::Texture::WRAP_S,osg::Texture::WRAP_T,osg::Texture::WRAP_R})
            texture->setWrap(axis,osg::Texture::CLAMP_TO_EDGE);
        texture->setResizeNonPowerOfTwoHint(false);
        return texture.release();
    }

    //! Lazily reserves ray resources on the cull thread under mutex; failure leaves ordinary clouds available.
    bool initializeRays(Slot& slot, unsigned width, unsigned height, unsigned depth, unsigned sunWidth)
    {
        if (raysUnavailable) return false;
        if (!raysProgram)
        {
            if (!resources->reserveTextureImageUnit(raysUnit,"Cloud atmospheric rays"))
            {
                raysUnavailable = true;
                OE_WARN << "[CloudLayer] Atmospheric rays require one additional texture unit\n";
                return false;
            }
            Shaders shaders;
            for (unsigned i=0; i<2; ++i)
            {
                std::string source = "#version 430\n#define OE_CLOUD_LAYER\n";
                if (i == 0) source += "#define OE_CLOUD_SUN_PASS\n";
                source += lightingAdapter+shaders.context().at("CloudLayer.Common.glsl")+
                    shaders.context().at("CloudLayer.Density.glsl")+shaders.context().at("CloudLayer.Light.glsl")+
                    shaders.context().at("CloudLayer.Rays.glsl");
                osg::ref_ptr<osg::Program> program = new osg::Program;
                program->setName(i == 0 ? "Cloud sunlight columns" : "Cloud atmospheric rays");
                program->addShader(new osg::Shader(osg::Shader::COMPUTE,source));
                if (i == 0) sunProgram = program; else raysProgram = program;
            }
        }
        if (slot.raysPass && slot.rayWidth == width && slot.rayHeight == height && slot.rayDepth == depth &&
            slot.sunWidth == sunWidth) return true;
        slot.rayWidth = width; slot.rayHeight = height; slot.rayDepth = depth;
        slot.sunWidth = sunWidth;
        osg::ref_ptr<osg::Texture3D> sunlight = rayTexture(sunWidth,sunWidth,depth,true);
        osg::ref_ptr<osg::Texture3D> rays = rayTexture(width,height,depth,false);
        osg::ref_ptr<osg::Texture3D> environment = rayTexture(64,32,depth,false);
        environment->setWrap(osg::Texture::WRAP_S,osg::Texture::REPEAT);
        slot.state->setTextureAttributeAndModes(raysUnit.unit(),rays);
        slot.state->getUniform("oe_cloud_raysVolume")->set(raysUnit.unit());
        slot.environmentState->setTextureAttributeAndModes(raysUnit.unit(),environment);
        slot.environmentState->addUniform(new osg::Uniform("oe_cloud_rayGrid",osg::Vec3(64,32,float(depth))));
        slot.sunPass = computePass(sunlight,sunProgram,sunWidth,sunWidth,true,volumeUnit.unit(),GL_R16F);
        slot.sunPass->setName("Cloud sunlight columns");
        slot.raysPass = computePass(rays,raysProgram,width,height,true,volumeUnit.unit(),0,false);
        slot.environmentRaysPass = computePass(environment,raysProgram,64,32,true,volumeUnit.unit(),0,false);
        // Sunlight reuses the reserved ground-shadow unit only in these private compute passes.
        // The view cloud volume remains bound so air shadows are attenuated by foreground clouds.
        for (auto pass : {slot.raysPass.get(),slot.environmentRaysPass.get()})
        {
            pass->setName("Cloud atmospheric rays");
            pass->setRenderOrder(osg::Camera::PRE_RENDER,-101);
            auto state = pass->getOrCreateStateSet();
            state->setTextureAttributeAndModes(shadowUnit.unit(),sunlight);
            state->addUniform(new osg::Uniform("oe_cloud_sunVolume",shadowUnit.unit()));
        }
        // Inherit the angular cloud capture while retaining this pass's private sunlight binding.
        auto environmentState = slot.environmentRaysPass->getOrCreateStateSet();
        environmentState->merge(*slot.environmentState);
        return true;
    }
};

CloudLayerRenderer::CloudLayerRenderer(CloudLayer* layer, TerrainResources* resources, const std::string& lighting) :
    _impl(new Impl(layer,resources,lighting)) { }
CloudLayerRenderer::~CloudLayerRenderer() = default;
bool CloudLayerRenderer::valid() const { return _impl->available; }

osg::StateSet* CloudLayerRenderer::cull(osgUtil::CullVisitor& cv, const Frame& input, osg::StateSet*& environmentState)
{
    environmentState = nullptr;
    if (!valid()) return nullptr;
    const auto& options = _impl->layer->getOptions();
    WindLayer* windLayer = _impl->layer->getWindLayer();
    if (!windLayer) windLayer = input.windLayer;
    osg::Vec2d displacement;
    if (windLayer) displacement = windLayer->getDirectionalDisplacement(input.time)*0.001;
    double altitude = (osg::Vec3d(input.eye).length()-input.radius)*1000.0;
    float visibility = _impl->layer->getVisibility(altitude);
    // No per-view volume allocation or compute work outside the altitude range, including first visits from orbit.
    // Perspective rays must share one origin; orthographic views retain the clear-sky fallback.
    if (!options.enabled || options.coverage <= 0.0f || options.density <= 0.0f || visibility <= 0.0f ||
        std::abs(input.projection(3,3)) >= 1e-6f)
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->pruneViews();
        return _impl->disabledState.get();
    }
    unsigned preset = unsigned(options.quality);
    const unsigned widths[] = {256,384,512}, depths[] = {16,24,28}, steps[] = {48,96,192}, lights[] = {2,4,6};
    unsigned width = options.resolution ? options.resolution : widths[preset];
    unsigned depth = options.depthSlices ? options.depthSlices : depths[preset];
    // Cap the two RGBA16F frame slots at 64 MiB even when several explicit overrides are combined.
    auto viewport = cv.getViewport();
    double aspect = viewport && viewport->width() > 0.0 ? viewport->height()/viewport->width() : 0.5625;
    aspect = std::max(0.125,std::min(8.0,aspect));
    unsigned height = std::max(8u,unsigned(width*aspect));
    while (std::uint64_t(width)*height*depth > 4194304u)
    {
        width -= 8;
        height = std::max(8u,unsigned(width*aspect));
    }
    unsigned frame = cv.getFrameStamp() ? cv.getFrameStamp()->getFrameNumber() : 0;
    unsigned rayPreset = unsigned(options.rayQuality);
    const unsigned rayWidths[] = {128,192,256}, rayDepths[] = {16,24,32}, raySteps[] = {48,96,160};
    const unsigned sunWidths[] = {160,256,320};
    const unsigned sunSteps[] = {64,128,192};
    unsigned rayWidth = rayWidths[rayPreset], rayDepth = rayDepths[rayPreset];
    unsigned sunWidth = sunWidths[rayPreset];
    unsigned rayHeight = std::max(8u,unsigned(rayWidth*aspect));
    // Bound both frame slots to 32 MiB including sunlight, view correction, and angular environment volumes.
    while (2ull*rayDepth*(2ull*sunWidth*sunWidth+8ull*rayWidth*rayHeight+8ull*64*32) > 32ull*1024*1024)
    {
        rayWidth -= 8;
        rayHeight = std::max(8u,unsigned(rayWidth*aspect));
        sunWidth = rayWidth*sunWidths[rayPreset]/rayWidths[rayPreset];
    }
    bool rays = options.crepuscularRays && options.rayStrength > 0.0f && input.airScattering;
    Impl::Slot* slot;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->pruneViews();
        auto& view = _impl->views[std::make_pair(cv.getCurrentCamera(),&cv)];
        view.camera = cv.getCurrentCamera();
        slot = &view.slots[frame%2];
        if (!slot->state || slot->width != width || slot->height != height || slot->depth != depth)
            _impl->initialize(*slot,width,height,depth);
        unsigned shadowWidth = options.farShadows ? 512u : 256u;
        if (slot->shadowWidth != shadowWidth) _impl->initializeShadows(*slot,shadowWidth);
        _impl->updateFarShadow(view,*slot,input,options.farShadows ? options.farShadowCoverage : 0.0f);
        if (rays) rays = _impl->initializeRays(*slot,rayWidth,rayHeight,rayDepth,sunWidth);
    }
    auto state = slot->state.get();
    state->getUniform("oe_cloud_enabled")->set(true);
    state->getUniform("oe_cloud_fade")->set(visibility);
    state->getUniform("oe_cloud_raysEnabled")->set(rays);
    if (rays)
    {
        // Sparse clouds expose the whole aerosol layer to sunlight, producing a uniform wash instead of shafts.
        // Fade only the added medium as the deck clears; retain shadowing of the host's existing atmosphere.
        float hazeCoverage = osg::clampBetween((options.coverage-0.2f)/0.4f,0.0f,1.0f);
        hazeCoverage = hazeCoverage*hazeCoverage*(3.0f-2.0f*hazeCoverage);
        state->getUniform("oe_cloud_raySettings")->set(osg::Vec4(options.rayDistance*0.001f,
            options.rayStrength,float(raySteps[rayPreset]),options.rayHaze*0.02f*hazeCoverage));
        state->getUniform("oe_cloud_rayIntensity")->set(options.rayIntensity);
        state->getUniform("oe_cloud_rayGrid")->set(osg::Vec3(float(rayWidth),float(rayHeight),float(rayDepth)));
        state->getUniform("oe_cloud_sunSamples")->set(int(sunSteps[rayPreset]));
        osg::Vec3 axis = input.sun;
        axis.normalize();
        osg::Vec3 right = (std::abs(axis.z()) < 0.99f ? osg::Vec3(0,0,1) : osg::Vec3(1,0,0)) ^ axis;
        right.normalize();
        osg::Vec3 up = axis ^ right;
        osg::Matrix3 basis;
        for (unsigned i=0; i<3; ++i) { basis(0,i) = right[i]; basis(1,i) = up[i]; basis(2,i) = axis[i]; }
        state->getUniform("oe_cloud_sunBasis")->set(basis);
    }
    state->getUniform("oe_cloud_eye")->set(input.eye);
    state->getUniform("oe_cloud_detailFiltering")->set(options.detailFiltering);
    state->getUniform("oe_cloud_sun")->set(input.sun);
    state->getUniform("oe_cloud_basis")->set(input.basis);
    state->getUniform("oe_cloud_viewToEarth")->set(input.viewToEarth);
    state->getUniform("oe_cloud_earthToView")->set(input.earthToView);
    state->getUniform("oe_cloud_projection")->set(input.projection);
    state->getUniform("oe_cloud_inverseProjection")->set(input.inverseProjection);
    state->getUniform("oe_cloud_shell")->set(osg::Vec4(input.radius,options.baseAltitude*0.001f,
        options.topAltitude*0.001f,input.horizon));
    state->getUniform("oe_cloud_shape")->set(osg::Vec4(options.coverage,options.density*3.0f,
        options.size*0.001f,options.erosion));
    state->getUniform("oe_cloud_grid")->set(osg::Vec4(float(width),float(height),float(depth),options.shadowStrength));
    osg::Vec3 wind;
    // All density frequencies share this period; double arithmetic keeps long-running wind phase continuous.
    double period = options.size*0.004*8.0;
    for (unsigned i=0; i<3; ++i)
        wind[i] = float(std::fmod(input.time*options.wind[i]*0.001,period));
    state->getUniform("oe_cloud_wind")->set(wind);
    // Backtrace east/north flow on the cloud shell, independently of camera position and view count.
    // Precompute the angle on the CPU; density samples need only a local tangent basis, with no extra textures.
    osg::Vec4 advection(0,0,1,0);
    if (windLayer)
    {
        double distance = displacement.length();
        double radius = input.radius+(options.baseAltitude+options.topAltitude)*0.0005;
        double angle = std::fmod(distance/radius,2.0*osg::PI);
        double scale = distance > 0.0 ? std::sin(angle)/distance : 0.0;
        advection.set(float(displacement.x()*scale),float(displacement.y()*scale),float(std::cos(angle)),1.0f);
        state->getUniform("oe_cloud_wind")->set(osg::Vec3());
    }
    state->getUniform("oe_cloud_advection")->set(advection);
    state->getUniform("oe_cloud_seed")->set(osg::Vec3(hash(options.seed,1,2)*8.0f,
        hash(options.seed,3,4)*8.0f,hash(options.seed,5,6)*8.0f));
    osg::Vec3 center = input.eye; center.normalize(); center *= input.radius+0.001f;
    // The shared generation/lookup transform expects half-width in kilometers.
    state->getUniform("oe_cloud_shadowOrigin")->set(osg::Vec4(center,options.shadowCoverage*0.0005f));
    state->getUniform("oe_cloud_samples")->set(int(std::max(depth-1,options.samples ? options.samples : steps[preset])));
    state->getUniform("oe_cloud_lightSamples")->set(int(options.lightSamples ? options.lightSamples : lights[preset]));
    if (slot->frame != frame)
    {
        cv.pushStateSet(state);
        slot->volumePass->accept(cv);
        slot->environmentPass->accept(cv);
        if (options.shadowStrength > 0.0f) slot->shadowPass->accept(cv);
        if (rays)
        {
            slot->sunPass->accept(cv);
            slot->raysPass->accept(cv);
            slot->environmentRaysPass->accept(cv);
        }
        cv.popStateSet();
    }
    slot->frame = frame;
    environmentState = slot->environmentState.get();
    return state;
}

void CloudLayerRenderer::releaseGLObjects(osg::State* state) const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    for (auto& entry : _impl->views)
    for (auto& slot : entry.second.slots)
    {
        if (slot.state) slot.state->releaseGLObjects(state);
        if (slot.volumePass) slot.volumePass->releaseGLObjects(state);
        if (slot.shadowPass) slot.shadowPass->releaseGLObjects(state);
        if (slot.environmentPass) slot.environmentPass->releaseGLObjects(state);
        if (slot.sunPass) slot.sunPass->releaseGLObjects(state);
        if (slot.raysPass) slot.raysPass->releaseGLObjects(state);
        if (slot.environmentRaysPass) slot.environmentRaysPass->releaseGLObjects(state);
        slot.frame = ~0u;
    }
}

void CloudLayerRenderer::resizeGLObjectBuffers(unsigned size)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    for (auto& entry : _impl->views)
    for (auto& slot : entry.second.slots)
    {
        if (slot.state) slot.state->resizeGLObjectBuffers(size);
        if (slot.volumePass) slot.volumePass->resizeGLObjectBuffers(size);
        if (slot.shadowPass) slot.shadowPass->resizeGLObjectBuffers(size);
        if (slot.environmentPass) slot.environmentPass->resizeGLObjectBuffers(size);
        if (slot.sunPass) slot.sunPass->resizeGLObjectBuffers(size);
        if (slot.raysPass) slot.raysPass->resizeGLObjectBuffers(size);
        if (slot.environmentRaysPass) slot.environmentRaysPass->resizeGLObjectBuffers(size);
    }
}
