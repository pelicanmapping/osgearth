/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "SkyEnvironment"
#include "SimpleSkyShaders"
#include <osgEarth/VirtualProgram>
#include <osgEarth/Capabilities>
#include <osgEarth/Registry>
#include <osg/Camera>
#include <osg/GLDefines>
#include <cmath>
#include <cstdint>

using namespace osgEarth;
using namespace osgEarth::SimpleSky;

namespace
{
    constexpr float PI = 3.14159265359f;
    constexpr unsigned SIZE = 32, LEVELS = 6, SAMPLES = 64;

    float saturate(float x) { return osg::clampBetween(x, 0.0f, 1.0f); }
    float smooth(float a, float b, float x)
    {
        float t = saturate((x-a)/(b-a));
        return t*t*(3.0f-2.0f*t);
    }
    osg::Vec3 mix(const osg::Vec3& a, const osg::Vec3& b, float t) { return a*(1-t)+b*t; }

    float radicalInverse(uint32_t bits)
    {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return float(bits) * 2.3283064365386963e-10f;
    }

    osg::Vec3 sampleGGX(unsigned i, float roughness, const osg::Vec3& n)
    {
        float phi = 2.0f*PI*float(i)/SAMPLES;
        float y = radicalInverse(i);
        float a = roughness*roughness;
        float z = std::sqrt((1.0f-y)/(1.0f+(a*a-1.0f)*y));
        float r = std::sqrt(std::max(0.0f, 1.0f-z*z));
        osg::Vec3 axis = std::abs(n.z()) < 0.999f ? osg::Vec3(0,0,1) : osg::Vec3(1,0,0);
        osg::Vec3 t = axis ^ n;
        t.normalize();
        osg::Vec3 b = n ^ t;
        return t*(r*std::cos(phi)) + b*(r*std::sin(phi)) + n*z;
    }

    osg::Vec3 cubeDirection(unsigned face, float u, float v)
    {
        osg::Vec3 d;
        switch (face)
        {
        case 0: d.set(1,-v,-u); break;
        case 1: d.set(-1,-v,u); break;
        case 2: d.set(u,1,v); break;
        case 3: d.set(u,-1,-v); break;
        case 4: d.set(u,-v,1); break;
        default: d.set(-u,-v,-1); break;
        }
        d.normalize();
        return d;
    }

    // Clear-sky approximation with a broad scattering halo. The solar disk
    // is deliberately absent: its energy comes from the direct sun light.
    osg::Vec3 skyRadiance(const osg::Vec3& d, const osg::Vec3& up, const osg::Vec3& sun)
    {
        float sunHeight = up*sun;
        float day = smooth(-0.12f, 0.15f, sunHeight);
        float noon = smooth(0.0f, 0.4f, sunHeight);
        float altitude = d*up;
        osg::Vec3 horizon = mix(osg::Vec3(1.1f,0.38f,0.12f), osg::Vec3(0.8f,0.88f,1.0f), noon);
        osg::Vec3 zenith = mix(osg::Vec3(0.12f,0.16f,0.30f), osg::Vec3(0.16f,0.36f,0.80f), noon);
        osg::Vec3 sky = mix(horizon, zenith, std::pow(std::max(altitude,0.0f),0.45f));
        sky += mix(osg::Vec3(1.0f,0.35f,0.1f),osg::Vec3(1.0f,0.9f,0.7f),noon) *
            (0.2f*std::pow(std::max(d*sun,0.0f),16.0f));
        osg::Vec3 ground(0.12f,0.10f,0.075f);
        return mix(ground,sky,smooth(-0.05f,0.03f,altitude))*day;
    }

    void basis(const osg::Vec3& n, float* b)
    {
        b[0]=0.282095f;
        b[1]=0.488603f*n.y(); b[2]=0.488603f*n.z(); b[3]=0.488603f*n.x();
        b[4]=1.092548f*n.x()*n.y(); b[5]=1.092548f*n.y()*n.z();
        b[6]=0.315392f*(3*n.z()*n.z()-1);
        b[7]=1.092548f*n.x()*n.z(); b[8]=0.546274f*(n.x()*n.x()-n.y()*n.y());
    }

    osg::Texture2D* makeBRDF()
    {
        // Split-sum environment BRDF, indexed by NdotV and perceptual roughness.
        osg::ref_ptr<osg::Image> image = new osg::Image;
        constexpr unsigned size = 64;
        image->allocateImage(size,size,1,GL_RG,GL_FLOAT);
        for (unsigned y=0; y<size; ++y)
        for (unsigned x=0; x<size; ++x)
        {
            float roughness=(y+0.5f)/size, nv=(x+0.5f)/size;
            osg::Vec3 v(std::sqrt(1-nv*nv),0,nv), n(0,0,1);
            float a=0, b=0, k=roughness*roughness*0.5f;
            for (unsigned i=0; i<SAMPLES; ++i)
            {
                osg::Vec3 h=sampleGGX(i,roughness,n);
                float vh=std::max(v*h,0.0f);
                osg::Vec3 l=h*(2*vh)-v;
                float nl=std::max(l.z(),0.0f), nh=std::max(h.z(),0.0f);
                if (nl>0 && nh>0)
                {
                    float g=(nv/(nv*(1-k)+k))*(nl/(nl*(1-k)+k));
                    float visibility=g*vh/std::max(nh*nv,1e-6f);
                    float fresnel=std::pow(1-vh,5.0f);
                    a+=(1-fresnel)*visibility; b+=fresnel*visibility;
                }
            }
            float* pixel=reinterpret_cast<float*>(image->data(x,y));
            pixel[0]=a/SAMPLES; pixel[1]=b/SAMPLES;
        }
        auto texture=new osg::Texture2D(image);
        texture->setInternalFormat(GL_RG16F);
        texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
        texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
        texture->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
        return texture;
    }
}

bool SkyEnvironment::attach(osg::StateSet* ss, TerrainResources* resources)
{
    if (!resources->reserveTextureImageUnit(_environmentUnit,"Sky environment") ||
        !resources->reserveTextureImageUnit(_brdfUnit,"Sky environment BRDF"))
    {
        _environmentUnit.release(); _brdfUnit.release();
        return false;
    }
    _environment=new osg::TextureCubeMap;
    _environment->setInternalFormat(GL_RGB16F_ARB);
    _environment->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);
    _environment->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
    for (auto wrap : {osg::Texture::WRAP_S,osg::Texture::WRAP_T,osg::Texture::WRAP_R})
        _environment->setWrap(wrap,osg::Texture::CLAMP_TO_EDGE);
    _environment->setUseHardwareMipMapGeneration(false);
    // Interpolate across cube faces, including the coarsest roughness mips.
    // GLES already requires seamless cube filtering and has no enable token.
    const auto& caps = Registry::capabilities();
    if (!caps.isGLES() && caps.getGLSLVersion() >= 1.50f)
        ss->setMode(GL_TEXTURE_CUBE_MAP_SEAMLESS, osg::StateAttribute::ON);
    _brdf=makeBRDF();
    _irradiance=new osg::Uniform(osg::Uniform::FLOAT_VEC3,"oe_sky_irradiance",9);
    ss->setTextureAttributeAndModes(_environmentUnit.unit(),_environment,osg::StateAttribute::ON);
    ss->setTextureAttributeAndModes(_brdfUnit.unit(),_brdf,osg::StateAttribute::ON);
    ss->addUniform(new osg::Uniform("oe_sky_environmentTex",_environmentUnit.unit()));
    ss->addUniform(new osg::Uniform("oe_sky_brdfTex",_brdfUnit.unit()));
    ss->addUniform(new osg::Uniform("oe_sky_environmentMaxLOD",float(LEVELS-1)));
    ss->addUniform(_irradiance);
    Shaders shaders;
    shaders.load(VirtualProgram::getOrCreate(ss),shaders.Environment);
    // The define is installed only after the initial probe has been generated.
    return true;
}

void SkyEnvironment::update(osg::View* view, const osg::Vec3d& sunDirection)
{
    if (!view || !_environment.valid()) return;
    osg::Vec3d up=view->getCamera()->getInverseViewMatrix().getTrans();
    if (up.length2()<1.0 || sunDirection.length2()<1e-12) return;
    up.normalize();
    osg::Vec3d sun=sunDirection; sun.normalize();
    auto now=std::chrono::steady_clock::now();
    if (_initialized && ((_lastUp-up).length2()<4e-6 && (_lastSun-sun).length2()<4e-6)) return;
    if (_initialized && now-_lastUpdate<std::chrono::milliseconds(250)) return;
    _lastUp=up; _lastSun=sun; _lastUpdate=now;

    osg::Vec3 u(up), s(sun), coefficients[9];
    constexpr unsigned sphereSamples=2048;
    for (unsigned i=0; i<sphereSamples; ++i)
    {
        float z=1-2*(i+0.5f)/sphereSamples, phi=i*2.3999632297f;
        float r=std::sqrt(1-z*z);
        osg::Vec3 d(r*std::cos(phi),r*std::sin(phi),z);
        float b[9]; basis(d,b);
        osg::Vec3 radiance=skyRadiance(d,u,s);
        for (unsigned j=0; j<9; ++j)
            coefficients[j]+=radiance*(b[j]*(4*PI/sphereSamples));
    }
    for (unsigned j=0; j<9; ++j)
    {
        // Cosine convolution divided by PI for Lambertian diffuse.
        float convolution=j==0 ? 1.0f : j<4 ? 2.0f/3.0f : 0.25f;
        _irradiance->setElement(j,coefficients[j]*convolution);
    }

    for (unsigned face=0; face<6; ++face)
    {
        osg::Image::MipmapDataType offsets;
        unsigned count=0;
        for (unsigned level=0; level<LEVELS; ++level)
        {
            if (level) offsets.push_back(count*sizeof(float));
            unsigned size=SIZE>>level; count+=size*size*3;
        }
        auto bytes=new unsigned char[count*sizeof(float)];
        float* pixel=reinterpret_cast<float*>(bytes);
        for (unsigned level=0; level<LEVELS; ++level)
        {
            unsigned size=SIZE>>level;
            float roughness=float(level)/(LEVELS-1);
            for (unsigned y=0; y<size; ++y)
            for (unsigned x=0; x<size; ++x)
            {
                osg::Vec3 n=cubeDirection(face,2*(x+0.5f)/size-1,2*(y+0.5f)/size-1);
                osg::Vec3 color;
                float weight=0;
                if (level==0) color=skyRadiance(n,u,s);
                else
                {
                    for (unsigned i=0; i<SAMPLES; ++i)
                    {
                        osg::Vec3 h=sampleGGX(i,roughness,n), l=h*(2*(n*h))-n;
                        float nl=std::max(n*l,0.0f);
                        color+=skyRadiance(l,u,s)*nl; weight+=nl;
                    }
                    color/=std::max(weight,1e-6f);
                }
                *pixel++=color.x(); *pixel++=color.y(); *pixel++=color.z();
            }
        }
        osg::ref_ptr<osg::Image> image=new osg::Image;
        image->setImage(SIZE,SIZE,1,GL_RGB16F_ARB,GL_RGB,GL_FLOAT,bytes,osg::Image::USE_NEW_DELETE);
        image->setMipmapLevels(offsets);
        _environment->setImage(face,image);
    }
    _initialized=true;
}
