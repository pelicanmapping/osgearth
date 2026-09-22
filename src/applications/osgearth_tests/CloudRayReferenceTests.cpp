/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/Shaders>
#include <osg/Texture3D>
#include <osg/BindImageTexture>
#include "SkyNode2TestScene.h"
#include <iostream>

using namespace osgEarth;
using namespace osgEarth::Sky2Tests;

namespace
{
    // A thin deck with repeated 200 m openings has known separated sunlight columns, without procedural weather ambiguity.
    const char* density = R"(
        float oe_cloud_density(vec3 p, bool detail)
        {
            float h=length(p)-oe_cloud_shell.x;
            if (h<oe_cloud_shell.y || h>oe_cloud_shell.z) return 0.0;
            float gap=abs(fract((p.y+0.35*p.x)/1.2)-0.5)*1.2;
            return gap<0.1 ? 0.0 : 24.0;
        }
    )";

    struct Probe
    {
        Scene scene;
        osg::GLExtensions* gl;
        std::vector<GLuint> programs, textures;

        //! Owns an offscreen context and raw GL resources; no viewer draw occurs while raw bindings are active.
        Probe() : scene(new SkyNode2,16,16), gl(scene.context->getState()->get<osg::GLExtensions>()) { }

        //! Releases probe-only resources while their context is still current.
        ~Probe()
        {
            gl->glUseProgram(0);
            for (auto program : programs) gl->glDeleteProgram(program);
            if (!textures.empty()) glDeleteTextures(GLsizei(textures.size()),textures.data());
        }

        //! Compiles a compute kernel and reports the complete driver diagnostic on failure.
        GLuint program(const std::string& source)
        {
            GLuint shader=gl->glCreateShader(GL_COMPUTE_SHADER);
            const char* text=source.c_str();
            gl->glShaderSource(shader,1,&text,nullptr); gl->glCompileShader(shader);
            GLint ok=0; gl->glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
            char log[8192]={0}; gl->glGetShaderInfoLog(shader,sizeof(log),nullptr,log);
            INFO(log); REQUIRE(ok == GL_TRUE);
            GLuint result=gl->glCreateProgram(); programs.push_back(result);
            gl->glAttachShader(result,shader); gl->glLinkProgram(result); gl->glDeleteShader(shader);
            gl->glGetProgramiv(result,GL_LINK_STATUS,&ok);
            gl->glGetProgramInfoLog(result,sizeof(log),nullptr,log);
            INFO(log); REQUIRE(ok == GL_TRUE);
            return result;
        }

        //! Allocates the same filtered R16F light cache as production, or an unquantized RGBA32F diagnostic target.
        GLuint texture(unsigned width, unsigned height, unsigned depth, bool single, GLenum format=0)
        {
            GLuint result=0; glGenTextures(1,&result); textures.push_back(result);
            glBindTexture(GL_TEXTURE_3D,result);
            gl->glTexImage3D(GL_TEXTURE_3D,0,format ? format : single ? GL_R16F : GL_RGBA32F_ARB,width,height,depth,0,
                single ? GL_RED : GL_RGBA,GL_FLOAT,nullptr);
            glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            // Windows' OpenGL 1.1 headers omit GL_TEXTURE_WRAP_R (0x8072).
            for (GLenum axis : {GLenum(GL_TEXTURE_WRAP_S),GLenum(GL_TEXTURE_WRAP_T),GLenum(0x8072)})
                glTexParameteri(GL_TEXTURE_3D,axis,GL_CLAMP_TO_EDGE);
            return result;
        }

        //! Sets a scalar probe control only if the selected kernel uses it.
        void scalar(GLuint program, const char* name, float value)
        {
            gl->glUniform1f(gl->glGetUniformLocation(program,name),value);
        }

        //! Supplies an equatorial local frame in scaled kilometers, matching production light-space conventions.
        void uniforms(GLuint program, float elevation)
        {
            gl->glUseProgram(program);
            float a=osg::DegreesToRadians(elevation), c=std::cos(a), s=std::sin(a);
            gl->glUniform3f(gl->glGetUniformLocation(program,"oe_cloud_eye"),0,0,6360.2f);
            gl->glUniform3f(gl->glGetUniformLocation(program,"oe_cloud_sun"),c,0,s);
            gl->glUniform4f(gl->glGetUniformLocation(program,"oe_cloud_shell"),6360,1.5f,1.7f,1.57f);
            gl->glUniform4f(gl->glGetUniformLocation(program,"oe_cloud_raySettings"),50,1,64,0.01f);
            float basis[]={0,1,0,-s,0,c,c,0,s};
            gl->glUniformMatrix3fv(gl->glGetUniformLocation(program,"oe_cloud_sunBasis"),1,GL_FALSE,basis);
            gl->glUniform1i(gl->glGetUniformLocation(program,"oe_cloud_sunSamples"),128);
            gl->glUniform1i(gl->glGetUniformLocation(program,"oe_cloud_sunVolume"),0);
        }

        //! Dispatches a bounded kernel with the image and texture dependencies needed by subsequent probes.
        void dispatch(GLuint output, unsigned width, unsigned height, bool single, GLenum format=0)
        {
            gl->glBindImageTexture(0,output,0,GL_TRUE,0,osg::BindImageTexture::WRITE_ONLY,
                format ? format : single ? GL_R16F : GL_RGBA32F_ARB);
            gl->glDispatchCompute((width+7)/8,(height+7)/8,1);
            gl->glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT|GL_SHADER_IMAGE_ACCESS_BARRIER_BIT|GL_TEXTURE_UPDATE_BARRIER_BIT);
        }

        //! Reads diagnostic radiance without exposure, tone mapping, clipping, or low-precision framebuffer storage.
        std::vector<float> pixels(GLuint texture, unsigned width, unsigned height)
        {
            std::vector<float> result(width*height*4);
            glBindTexture(GL_TEXTURE_3D,texture);
            glGetTexImage(GL_TEXTURE_3D,0,GL_RGBA,GL_FLOAT,result.data());
            return result;
        }
    };

    // Reference sunlight marches actual density from each receiver; it never reads or reconstructs the light cache.
    const char* reference = R"(
        layout(local_size_x=8,local_size_y=8) in;
        layout(rgba32f,binding=0) uniform writeonly image3D outputImage;
        uniform float probeHeight, probeRays;
        float referenceSun(vec3 p)
        {
            vec4 span=oe_cloud_intervals(p,oe_cloud_sun);
            float total=span.y-span.x+span.w-span.z, tau=0.0;
            for (int i=0;i<256 && total>0.0;++i)
            {
                float distance=oe_cloud_distance(span,total*(float(i)+0.5)/256.0);
                tau+=oe_cloud_density(p+oe_cloud_sun*distance,true)*total/256.0;
            }
            return exp(-tau);
        }
        // Stores accelerated and independent-reference values side by side in R/G; B is absolute error.
        void main()
        {
            ivec2 pixel=ivec2(gl_GlobalInvocationID.xy), size=imageSize(outputImage).xy;
            if (any(greaterThanEqual(pixel,size))) return;
            vec2 uv=(vec2(pixel)+0.5)/vec2(size)*2.0-1.0;
            vec3 p=vec3(uv*5.0,6360.0+probeHeight);
            float actual=oe_cloud_sunVisibility(p), expected=referenceSun(p);
            if (probeRays>0.5)
            {
                vec3 direction=normalize(vec3(1,uv.x*0.8,0.08+uv.y*0.23));
                float T=1.0, ds=10.0/512.0;
                actual=0.0; expected=0.0;
                for (int i=0;i<512;++i)
                {
                    p=oe_cloud_eye+direction*(float(i)+0.5)*ds;
                    if (length(p)<6360.0) break;
                    float cloud=oe_cloud_density(p,true), sigma=0.02+cloud;
                    float weight=T*(1.0-exp(-sigma*ds))*0.02/sigma;
                    actual+=weight*oe_cloud_sunVisibility(p);
                    expected+=weight*referenceSun(p);
                    T*=exp(-sigma*ds);
                }
            }
            imageStore(outputImage,ivec3(pixel,0),vec4(actual,expected,abs(actual-expected),1));
        }
    )";

    // Isolates the production air integrator with exact view extinction and simple, independently reconstructible lighting.
    const char* adapter = R"(
        // Removes pre-existing atmospheric transport from the isolated haze comparison.
        void oe_cloud_air(vec3 d,float distance,out vec3 S,out vec3 T) { S=vec3(0); T=vec3(1); }
        // Keeps production's daylight gate active without contributing measurable clear-air shadow correction.
        vec3 oe_cloud_airSource(vec3 p,vec3 d) { return vec3(1e-8); }
        // Provides unit incident solar radiance for a reference with no atmosphere lookup dependency.
        vec3 oe_cloud_sunlight(vec3 p) { return vec3(1); }
        // Excludes ambient fill so only the separated direct-light shafts are measured.
        vec3 oe_cloud_ambient(vec3 p) { return vec3(0); }
        // Evaluates the same normalized phase law with a nonsingular fixed test anisotropy.
        float oe_cloud_phase(float cosine,float g)
        { return (1.0-g*g)/(12.56637061436*pow(1.0+g*g-2.0*g*cosine,1.5)); }
        // Dense view extinction removes interpolation of the separate cloud volume from this air-only comparison.
        vec4 oe_cloud_sample(vec3 direction,float distance)
        {
            float tau=0.0;
            vec4 span=oe_cloud_intervals(oe_cloud_eye,direction);
            float total=clamp(distance-span.x,0.0,span.y-span.x)+clamp(distance-span.z,0.0,span.w-span.z);
            for (int i=0;i<256 && total>0.0;++i)
                tau+=oe_cloud_density(oe_cloud_eye+direction*oe_cloud_distance(span,total*(float(i)+0.5)/256.0),true)*
                    total/256.0;
            return vec4(0,0,0,exp(-tau));
        }
    )";

    // Independently integrates all view extinction and source terms at 2048 steps, bypassing cumulative ray-volume sampling.
    const char* airReference = R"(
        uniform sampler3D actualRays;
        void main()
        {
            ivec2 pixel=ivec2(gl_GlobalInvocationID.xy), size=imageSize(outputImage).xy;
            if (any(greaterThanEqual(pixel,size))) return;
            vec2 uv=(vec2(pixel)+0.5)/vec2(size);
            vec4 v=oe_cloud_inverseProjection*vec4(uv*2.0-1.0,0,1);
            vec3 direction=normalize(oe_cloud_viewToEarth*v.xyz);
            vec2 ground=oe_cloud_sphere(oe_cloud_eye,direction,oe_cloud_shell.x);
            float limit=ground.x>0.0 ? min(ground.x,50.0) : 50.0;
            float ds=limit/2048.0, T=1.0, expected=0.0;
            for (int i=0;i<2048;++i)
            {
                float distance=(float(i)+0.5)*ds;
                vec3 p=oe_cloud_eye+direction*distance;
                float haze=0.02*exp(-max(0.0,length(p)-oe_cloud_shell.x)/2.0)*
                    (1.0-smoothstep(0.8,1.0,distance/50.0));
                float sunT=referenceSun(p)*exp(-min(20.0,haze*2.0/max(0.05,dot(normalize(p),oe_cloud_sun))));
                float sigma=haze+oe_cloud_density(p,true), weight=(1.0-exp(-sigma*ds))/max(sigma,1e-8);
                float cosine=dot(direction,oe_cloud_sun), g=0.7;
                float phase=(1.0-g*g)/(12.56637061436*pow(1.0+g*g-2.0*g*cosine,1.5));
                expected+=T*weight*0.9*haze*sunT*phase;
                T*=exp(-sigma*ds);
            }
            float actual=texture(actualRays,vec3(uv,23.5/24.0)).r;
            imageStore(outputImage,ivec3(pixel,0),vec4(actual,expected,abs(actual-expected),1));
        }
    )";

    //! Exports the diagnostic kernel's actual radiance values at a fixed exposure for visual comparison.
    void saveProbe(const std::vector<float>& pixels, unsigned width, unsigned height, unsigned channel,
        float exposure, const std::string& path)
    {
        osg::ref_ptr<osg::Image> image=new osg::Image;
        image->allocateImage(width,height,1,GL_RGB,GL_UNSIGNED_BYTE);
        for (unsigned i=0;i<width*height;++i)
        {
            auto value=static_cast<unsigned char>(255.0f*osg::clampBetween(pixels[4*i+channel]*exposure,0.0f,1.0f));
            for (unsigned c=0;c<3;++c) image->data()[i*3+c]=value;
        }
        REQUIRE(osgDB::writeImageFile(*image,path));
    }
}

// Compares the actual cumulative air pass with dense view/sun marching, independently of exposure and cloud radiance.
TEST_CASE("Cloud air volume preserves reference shafts", "[clouds][cloudrayreference][.gl]")
{
    Probe probe; Shaders shaders;
    auto common=shaders.context().at("CloudLayer.Common.glsl"), light=shaders.context().at("CloudLayer.Light.glsl");
    auto rays=shaders.context().at("CloudLayer.Rays.glsl");
    GLuint sun=probe.program("#version 430\n#define OE_CLOUD_LAYER\n#define OE_CLOUD_SUN_PASS\n"+
        common+density+light+rays);
    GLuint air=probe.program("#version 430\n#define OE_CLOUD_LAYER\n#define oe_cloud_sample unused_cloud_sample\n"+
        common+"\n#undef oe_cloud_sample\n"+density+adapter+light+rays);
    std::string ref=reference;
    ref.replace(ref.find("void main()"),11,"void unusedMain()");
    GLuint comparison=probe.program("#version 430\n#define OE_CLOUD_LAYER\n"+common+density+light+ref+airReference);
    GLuint cache=probe.texture(256,256,24,true), airVolume=probe.texture(64,40,24,false,GL_RGBA16F_ARB);
    GLuint output=probe.texture(64,40,1,false);
    probe.uniforms(sun,35.0f); probe.dispatch(cache,256,256,true);
    osg::Matrixf projection=osg::Matrixf::perspective(28.0,1.6,0.01,100.0);
    osg::Matrixf inverse=osg::Matrixf::inverse(projection);
    float pitch=0.08f,c=std::cos(pitch),s=std::sin(pitch), view[]={0,1,0,-s,0,c,-c,0,-s};
    for (GLuint program : {air,comparison})
    {
        probe.uniforms(program,35.0f);
        probe.gl->glUniform1i(probe.gl->glGetUniformLocation(program,"oe_cloud_screenSpace"),1);
        probe.gl->glUniformMatrix3fv(probe.gl->glGetUniformLocation(program,"oe_cloud_viewToEarth"),1,GL_FALSE,view);
        probe.gl->glUniformMatrix4fv(probe.gl->glGetUniformLocation(program,"oe_cloud_inverseProjection"),1,
            GL_FALSE,inverse.ptr());
        probe.gl->glUniform4f(probe.gl->glGetUniformLocation(program,"oe_cloud_raySettings"),50,1,96,0.02f);
        probe.scalar(program,"oe_cloud_rayIntensity",1.0f);
        probe.gl->glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_3D,cache);
        if (program == air) probe.dispatch(airVolume,64,40,false,GL_RGBA16F_ARB);
        else
        {
            probe.gl->glActiveTexture(GL_TEXTURE0+1); glBindTexture(GL_TEXTURE_3D,airVolume);
            probe.gl->glUniform1i(probe.gl->glGetUniformLocation(program,"actualRays"),1);
            probe.dispatch(output,64,40,false);
        }
    }
    auto pixels=probe.pixels(output,64,40);
    double error=0,energy=0;
    for (unsigned i=0;i<pixels.size();i+=4) { error+=pixels[i+2]; energy+=pixels[i+1]; }
    std::cout << "Production air relative error=" << error/energy << '\n';
    CHECK(error/energy < 0.15);
    saveProbe(pixels,64,40,0,30.0f,"cloud-reference-air-cached.png");
    saveProbe(pixels,64,40,1,30.0f,"cloud-reference-air-reference.png");
    CHECK(glGetError() == GL_NO_ERROR);
}

// Narrow known openings must survive the light cache, including partial columns and integration through empty air.
TEST_CASE("Cloud sunlight cache matches a dense independent ray march", "[clouds][cloudrayreference][.gl]")
{
    Probe probe;
    Shaders shaders;
    auto common=shaders.context().at("CloudLayer.Common.glsl");
    auto light=shaders.context().at("CloudLayer.Light.glsl");
    GLuint sun=probe.program("#version 430\n#define OE_CLOUD_LAYER\n#define OE_CLOUD_SUN_PASS\n"+
        common+density+light+shaders.context().at("CloudLayer.Rays.glsl"));
    GLuint comparison=probe.program("#version 430\n#define OE_CLOUD_LAYER\n"+common+density+light+reference);
    GLuint cache=probe.texture(256,256,24,true), output=probe.texture(128,80,1,false);
    for (float elevation : {12.0f,35.0f})
    {
        probe.uniforms(sun,elevation); probe.dispatch(cache,256,256,true);
        probe.gl->glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_3D,cache);
        probe.uniforms(comparison,elevation);
        for (float height : {0.2f,1.6f,2.0f,-1.0f})
        {
            probe.scalar(comparison,"probeHeight",height);
            probe.scalar(comparison,"probeRays",height<0 ? 1.0f : 0.0f);
            glBindTexture(GL_TEXTURE_3D,cache);
            probe.dispatch(output,128,80,false);
            auto pixels=probe.pixels(output,128,80);
            double error=0.0, energy=0.0, contrast=0.0;
            bool finite=true;
            for (unsigned i=0;i<pixels.size();i+=4)
            {
                finite=finite && std::isfinite(pixels[i]);
                error+=pixels[i+2]; energy+=pixels[i+1];
                contrast=std::max(contrast,double(pixels[i+1]));
            }
            error/=128*80; energy/=128*80;
            REQUIRE(finite);
            std::cout << "Reference sun=" << elevation << " height=" << height << " MAE=" << error
                << " relative=" << error/std::max(energy,1e-6) << " maximum=" << contrast << '\n';
            if (height<0)
            {
                // Matching the shape of the independent shaft image rejects broad glow and blurred shadow patches.
                CHECK(error/std::max(energy,1e-6) < 0.10);
                CHECK(contrast > 0.08);
            }
            else CHECK(error < 0.07);
            std::string name="cloud-reference-"+std::to_string(int(elevation))+"-"+std::to_string(int(height*10));
            saveProbe(pixels,128,80,0,height<0 ? 10.0f : 1.0f,name+"-cached.png");
            saveProbe(pixels,128,80,1,height<0 ? 10.0f : 1.0f,name+"-reference.png");
        }
    }
    CHECK(glGetError() == GL_NO_ERROR);
}
