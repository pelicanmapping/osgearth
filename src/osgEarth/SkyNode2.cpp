/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/SkyNode2>
#include <osgEarth/Lighting>
#include <osgEarth/MapNode>
#include <osgEarth/NodeUtils>
#include <osgEarth/Shaders>
#include <osgEarth/ShaderGenerator>
#include <osgEarth/CameraUtils>
#include <osgEarth/StarData>
#include <osgEarth/TerrainEngineNode>
#include <osgEarth/TerrainResources>
#include <osgEarth/VirtualProgram>
#include <osgEarth/Extension>
#include <osg/Camera>
#include <osg/Depth>
#include <osg/Geometry>
#include <osg/Texture2D>
#include <osgUtil/CullVisitor>
#include <osgViewer/View>
#include "SkyNode2Atmosphere.h"
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <sstream>

using namespace osgEarth;

namespace
{
    // Private cameras use ordinary GL programs, independent of inherited material shaders.
    const char* triangleVertex = R"(
        #version 330
        in vec4 osg_Vertex;
        out vec2 oe_s2_uv;
        // Generates clip coordinates and smooth screen UVs from an oversized triangle.
        void main() { gl_Position=vec4(osg_Vertex.xy,1.0,1.0); oe_s2_uv=osg_Vertex.xy*0.5+0.5; }
    )";

    //! Returns a finite nonnegative option, or a documented fallback.
    float validScalar(float value, float fallback)
    {
        return std::isfinite(value) && value >= 0.0f ? value : fallback;
    }

    //! Creates a clamp-filtered HDR target; texture storage is allocated lazily in each GL context.
    osg::ref_ptr<osg::Texture2D> target(unsigned width, unsigned height, bool repeat = false)
    {
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D;
        texture->setTextureSize(width,height);
        texture->setInternalFormat(GL_RGBA16F_ARB);
        texture->setSourceFormat(GL_RGBA);
        texture->setSourceType(GL_FLOAT);
        texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
        texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
        texture->setWrap(osg::Texture::WRAP_S,repeat ? osg::Texture::REPEAT : osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
        texture->setResizeNonPowerOfTwoHint(false);
        return texture;
    }

    //! Assembles a standalone GLSL 3.3 pass from the embedded core shader package.
    osg::ref_ptr<osg::Program> program(const std::string& defines, const std::string& file)
    {
        Shaders shaders;
        osg::ref_ptr<osg::Program> result = new osg::Program;
        result->setName(file+defines);
        result->addBindAttribLocation("osg_Vertex",0);
        result->addShader(new osg::Shader(osg::Shader::VERTEX,triangleVertex));
        result->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 330\n"+defines+shaders.context().at("SkyNode2.Common.glsl")+shaders.context().at(file)));
        return result;
    }

    //! Builds a VBO triangle that cannot affect scene bounds or be frustum culled.
    osg::ref_ptr<osg::Geometry> triangle(osg::Program* shader)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(osg::Vec3(-1,-1,0));
        vertices->push_back(osg::Vec3(3,-1,0));
        vertices->push_back(osg::Vec3(-1,3,0));
        geometry->setVertexArray(vertices);
        geometry->setVertexAttribArray(0,vertices,osg::Array::BIND_PER_VERTEX);
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
        geometry->setUseDisplayList(false);
        geometry->setUseVertexBufferObjects(true);
        geometry->setCullingActive(false);
        auto ss = geometry->getOrCreateStateSet();
        ss->setAttributeAndModes(shader,osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
        ss->setMode(GL_BLEND,osg::StateAttribute::OFF|osg::StateAttribute::OVERRIDE);
        ss->setMode(GL_CULL_FACE,osg::StateAttribute::OFF|osg::StateAttribute::OVERRIDE);
        return geometry;
    }

    //! Creates an off-graph render-to-texture pass, without depth or stencil attachments.
    osg::ref_ptr<osg::Camera> pass(osg::Texture2D* texture, osg::Program* shader, int order)
    {
        osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        camera->setName("SkyNode2 lookup");
        camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        camera->setViewMatrix(osg::Matrix::identity());
        camera->setProjectionMatrix(osg::Matrix::identity());
        camera->setViewport(0,0,texture->getTextureWidth(),texture->getTextureHeight());
        camera->setRenderOrder(osg::Camera::PRE_RENDER,order);
        camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        camera->setImplicitBufferAttachmentMask(0,0);
        camera->setClearMask(0);
        camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        camera->setCullingMode(osg::CullSettings::NO_CULLING);
        camera->setAllowEventFocus(false);
        camera->attach(osg::Camera::COLOR_BUFFER,texture);
        auto geometry = triangle(shader);
        geometry->getOrCreateStateSet()->setMode(GL_DEPTH_TEST,osg::StateAttribute::OFF|osg::StateAttribute::OVERRIDE);
        camera->addChild(geometry);
        return camera;
    }

    //! Bakes the existing astronomical catalog into a periodic, flux-weighted star texture once.
    osg::ref_ptr<osg::Texture2D> starTexture()
    {
        static osg::ref_ptr<osg::Texture2D> stars = []()
        {
            constexpr int w = 2048, h = 1024;
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(w,h,1,GL_RGB,GL_UNSIGNED_BYTE);
            std::fill(image->data(),image->data()+image->getTotalSizeInBytes(),0);
            for (const char** entry = s_defaultStarData; *entry; ++entry)
            {
                std::string record(*entry);
                std::replace(record.begin(),record.end(),',',' ');
                // The first ten characters are the catalog name, which can contain spaces.
                std::istringstream input(record.substr(11));
                double ra, declination, magnitude;
                if (!(input >> ra >> declination >> magnitude)) continue;
                int x = int(ra/(2.0*osg::PI)*w), y = int((0.5-declination/osg::PI)*h);
                double flux = std::min(1.0,std::pow(10.0,-0.22*(magnitude-1.0)));
                for (int dy=-1; dy<=1; ++dy)
                for (int dx=-1; dx<=1; ++dx)
                {
                    unsigned char* p = image->data((x+dx+w)%w,std::max(0,std::min(h-1,y+dy)));
                    int light = int(255.0*flux*std::exp(-2.0*(dx*dx+dy*dy)));
                    for (unsigned c=0; c<3; ++c) p[c] = static_cast<unsigned char>(std::min(255,int(p[c])+light));
                }
            }
            auto texture = target(w,h,true);
            texture->setInternalFormat(GL_RGB8);
            texture->setImage(image);
            return texture;
        }();
        return stars;
    }

    //! Shares immutable atmosphere coefficients between skies without any camera-dependent CPU work.
    osg::ref_ptr<osg::Texture2D> atmosphereTexture()
    {
        static osg::ref_ptr<osg::Texture2D> texture = []()
        {
            auto result = target(256,96);
            result->setInternalFormat(GL_RGB32F_ARB);
            result->setSourceFormat(GL_RGB);
            result->setImage(Sky2Atmosphere::createAtlas());
            return result;
        }();
        return texture;
    }
}

struct SkyNode2::Impl
{
    Options options;
    osg::ref_ptr<LightGL3> light = new LightGL3(0);
    osg::ref_ptr<osg::LightSource> lightSource = new osg::LightSource;
    osg::ref_ptr<osg::Texture2D> atmosphere, stars;
    osg::ref_ptr<osg::Program> skyProgram, environmentProgram, aerialProgram, backgroundProgram;
    osg::ref_ptr<TerrainResources> resources;
    std::array<TextureImageUnitReservation,4> units;
    std::atomic_bool celestialDirty{true};
    TimeStamp date = 0;
    osg::Vec3d sun, moon;
    osg::Matrixd worldToECEF, earthToECI;
    bool ready = false;
    bool atmospheric = false;

    struct Frame
    {
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
        osg::ref_ptr<osg::Geometry> background;
        osg::ref_ptr<osg::Texture2D> sky, environment, aerial;
        osg::ref_ptr<osg::Camera> skyPass, environmentPass, aerialPass;
        osg::Vec3d lastSun;
        osg::Vec3 lastEye, lastSolar;
        bool valid = false;
        bool scheduled = false;
        unsigned frameNumber = ~0u;
    };
    struct View
    {
        osg::observer_ptr<osg::Camera> camera;
        std::array<Frame,2> frames;
    };
    std::map<std::pair<osg::Camera*,osgUtil::CullVisitor*>,std::unique_ptr<View>> views;
    mutable std::mutex mutex;

    //! Validates construction settings and sets a white directional sun.
    explicit Impl(const Options& input) : options(input)
    {
        if (options.preset != FLAT && options.preset != BALANCED && options.preset != HIGH) options.preset = BALANCED;
        options.exposure = validScalar(options.exposure,1.0f);
        options.sunIntensity = validScalar(options.sunIntensity,10.0f);
        options.environmentIntensity = validScalar(options.environmentIntensity,1.0f);
        light->setAmbient(osg::Vec4(0,0,0,1));
        light->setDiffuse(osg::Vec4(1,1,1,1));
        light->setSpecular(osg::Vec4(1,1,1,1));
        lightSource->setLight(light);
        lightSource->setCullingActive(false);
        lightSource->addCullCallback(new LightSourceGL3UniformGenerator);
    }

    //! Reserves terrain units and compiles pass descriptions once after the child graph is installed.
    void prepare(SkyNode2& owner)
    {
        if (ready) return;
        atmospheric = options.preset != FLAT;
        if (atmospheric)
        {
            auto terrain = findTopMostNodeOfType<TerrainEngineNode>(&owner);
            resources = terrain ? terrain->getResources() : new TerrainResources;
            // Standalone model graphs commonly own material units 0 through 7.
            if (!terrain) for (int i=0; i<8; ++i) resources->setTextureImageUnitOffLimits(i);
            for (auto& unit : units)
            {
                if (!resources->reserveTextureImageUnit(unit,"SkyNode2"))
                {
                    OE_WARN << "[SkyNode2] Four texture units unavailable; using atmosphere-free PBR\n";
                    atmospheric = false;
                    for (auto& reserved : units) reserved.release();
                    break;
                }
            }
        }
        auto ss = owner.getOrCreateStateSet();
        if (atmospheric) ss->setDefine("OE_SKY2_ATMOSPHERE");
        std::string defines;
        if (options.toneMapping) defines += "#define OE_SKY2_TONEMAP\n";
        if (options.outputSRGB) defines += "#define OE_SKY2_SRGB\n";
        if (atmospheric) defines += "#define OE_SKY2_ATMOSPHERE\n";
        backgroundProgram = program(defines,"SkyNode2.Background.glsl");
        stars = starTexture();
        if (atmospheric)
        {
            atmosphere = atmosphereTexture();
            std::string samples = options.preset == HIGH ? "32" : "16";
            defines += "#define OE_SKY2_SAMPLES "+samples+"\n#define OE_SKY2_FILTER_SAMPLES "+samples+"\n";
            skyProgram = program(defines+"#define OE_SKY2_SKY_PASS\n","SkyNode2.LUT.glsl");
            aerialProgram = program(defines+"#define OE_SKY2_AERIAL_PASS\n","SkyNode2.LUT.glsl");
            environmentProgram = program(defines,"SkyNode2.LUT.glsl");
        }
        ready = true;
    }

    //! Allocates a frame's private LUTs and uniforms once; subsequent culls reuse them.
    void initialize(Frame& f)
    {
        f.background = triangle(backgroundProgram);
        auto bs = f.background->getOrCreateStateSet();
        bs->setRenderBinDetails(5,"RenderBin"); // opaque terrain is bin 0; transparency normally bin 10
        bs->setAttributeAndModes(new osg::Depth(osg::Depth::LEQUAL,0.0,1.0,false),
            osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
        bs->setTextureAttributeAndModes(0,stars,osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
        bs->addUniform(new osg::Uniform("oe_sky2_stars",0));
        f.state->addUniform(new osg::Uniform("oe_sky2_sunIndex",light->getLightNum()));
        f.state->addUniform(new osg::Uniform("oe_sky2_eye",osg::Vec3()));
        f.state->addUniform(new osg::Uniform("oe_sky2_sun",osg::Vec3()));
        f.state->addUniform(new osg::Uniform("oe_sky2_solarIrradiance",osg::Vec3()));
        f.state->addUniform(new osg::Uniform("oe_sky2_settings",osg::Vec4()));
        f.state->addUniform(new osg::Uniform("oe_sky2_flags",osg::Vec4()));
        f.state->addUniform(new osg::Uniform("oe_sky2_moon",osg::Vec4()));
        f.state->addUniform(new osg::Uniform("oe_sky2_horizon",1.57f));
        f.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT3,"oe_sky2_basis"));
        f.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT3,"oe_sky2_earthToECI"));
        f.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT3,"oe_sky2_viewToEarth"));
        f.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT3,"oe_sky2_viewToSky"));
        f.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT4,"oe_sky2_inverseProjection"));
        unsigned width = options.preset == HIGH ? 256 : 192, height = options.preset == HIGH ? 144 : 108;
        unsigned slices = options.preset == HIGH ? 32 : 16;
        f.state->addUniform(new osg::Uniform("oe_sky2_viewSize",osg::Vec2(float(width),float(height))));
        f.state->addUniform(new osg::Uniform("oe_sky2_aerialSlices",float(slices)));
        if (atmospheric)
        {
            f.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_VEC2,"oe_sky2_rowInterval",32));
            f.state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_VEC4,"oe_sky2_rowWarp",32));
            f.sky = target(width,height,true);
            f.environment = target(64,224,true);
            f.aerial = target(64*slices,64); // 64 azimuths x 32 elevations; RGB radiance and transmission halves.
            f.skyPass = pass(f.sky,skyProgram,-103);
            f.environmentPass = pass(f.environment,environmentProgram,-102);
            f.aerialPass = pass(f.aerial,aerialProgram,-101);
            f.state->setTextureAttributeAndModes(units[0].unit(),atmosphere);
            f.state->setTextureAttributeAndModes(units[1].unit(),f.sky);
            f.state->setTextureAttributeAndModes(units[2].unit(),f.environment);
            f.state->setTextureAttributeAndModes(units[3].unit(),f.aerial);
            const char* samplers[] = {"atmosphere","view","environment","aerial"};
            for (unsigned i=0; i<4; ++i)
                f.state->addUniform(new osg::Uniform((std::string("oe_sky2_")+samplers[i]).c_str(),units[i].unit()));
        }
    }

    //! Converts the rotation/scale part of an OSG row-vector matrix to its GLSL uniform representation.
    static osg::Matrix3 matrix3(const osg::Matrixd& m)
    {
        osg::Matrix3 result;
        for (unsigned r=0; r<3; ++r)
        for (unsigned c=0; c<3; ++c) result(r,c) = float(m(r,c));
        return result;
    }

    //! Tabulates the 32 elevation-row geometries once per eye position, on this view's cull thread.
    static void updateAerialRows(Frame& f, const osg::Vec3& eye, const osg::Matrix3& basis, float horizon)
    {
        auto intervals = f.state->getUniform("oe_sky2_rowInterval");
        auto warps = f.state->getUniform("oe_sky2_rowWarp");
        float radius = eye.length(), top = float(Sky2Atmosphere::top), groundRadius = float(Sky2Atmosphere::radius);
        for (unsigned row=0; row<32; ++row)
        {
            float y = 2.0f*(float(row)/31.0f)-1.0f;
            float theta = horizon+(y < 0.0f ? -1.0f : 1.0f)*y*y*(y < 0.0f ? horizon : float(osg::PI)-horizon);
            float sine = std::sin(theta), cosine = std::cos(theta);
            osg::Vec3 direction;
            for (unsigned i=0; i<3; ++i) direction[i] = sine*basis(0,i)+cosine*basis(2,i);
            float b = eye*direction;
            // Match the shader's stable altitude product; subtracting squared planet radii loses ground precision.
            float shell = b*b-(radius-top)*(radius+top);
            float start = 0.0f, finish = -1.0f;
            if (shell >= 0.0f)
            {
                float root = std::sqrt(shell);
                start = std::max(0.0f,-b-root);
                finish = -b+root;
            }
            float ground = b*b-(radius-groundRadius)*(radius+groundRadius);
            if (ground >= 0.0f)
            {
                float hit = -b-std::sqrt(ground);
                if (hit > 0.0f) finish = std::min(finish,hit);
            }
            finish = std::max(start,finish);
            float closest = osg::clampBetween(-b,start,finish);
            float nearWarp = -std::sqrt(std::max(0.0f,closest-start));
            float farWarp = std::sqrt(std::max(0.0f,finish-closest));
            intervals->setElement(row,osg::Vec2(start,finish));
            warps->setElement(row,osg::Vec4(nearWarp,farWarp,closest,1.0f/std::max(1e-6f,farWarp-nearWarp)));
        }
    }

    //! Culls bounded GPU lookup work only when the corresponding view or ephemeris has changed.
    void cull(SkyNode2& owner, osgUtil::CullVisitor& cv)
    {
        auto camera = cv.getCurrentCamera();
        if (!camera || CameraUtils::isShadowCamera(camera) ||
            CameraUtils::isDepthCamera(camera) || CameraUtils::isPickCamera(camera))
        {
            owner.SkyNode::traverse(cv);
            return;
        }
        Frame* frame;
        unsigned number = cv.getFrameStamp() ? cv.getFrameStamp()->getFrameNumber() : 0u;
        {
            std::lock_guard<std::mutex> lock(mutex);
            prepare(owner);
            auto& view = views[std::make_pair(camera,&cv)];
            if (!view)
            {
                view.reset(new View);
                view->camera = camera;
            }
            frame = &view->frames[number%2];
            if (!frame->background) initialize(*frame);
        }
        auto& f = *frame;
        osg::Matrixd inverse = osg::Matrixd::inverse(*cv.getModelViewMatrix());
        osg::Matrixd toEarth = inverse*worldToECEF*osg::Matrixd::scale(
            Sky2Atmosphere::radius/6378137.0,Sky2Atmosphere::radius/6378137.0,Sky2Atmosphere::radius/6356752.314245);
        osg::Vec3d eye = toEarth.getTrans();
        if (eye.length2() < 1.0) eye.set(0,0,Sky2Atmosphere::radius+0.001);
        if (eye.length() < Sky2Atmosphere::radius+0.001)
            eye *= (Sky2Atmosphere::radius+0.001)/eye.length();
        osg::Vec3d up = eye; up.normalize();
        osg::Vec3d east = (std::abs(up.z()) < 0.99 ? osg::Vec3d(0,0,1) : osg::Vec3d(1,0,0)) ^ up;
        east.normalize();
        osg::Vec3d north = up ^ east;
        osg::Matrixd basis(east.x(),east.y(),east.z(),0,north.x(),north.y(),north.z(),0,up.x(),up.y(),up.z(),0,0,0,0,1);
        const auto& diffuse = light->getDiffuse();
        osg::Vec3 solar(diffuse.r(),diffuse.g(),diffuse.b());
        solar *= light->getEnabled() ? options.sunIntensity : 0.0f;
        // Compare shader inputs: double-precision matrix inversion jitter must not invalidate a stationary observer.
        osg::Vec3 shaderEye(eye);
        bool skyDirty = !f.valid || shaderEye != f.lastEye || sun != f.lastSun || solar != f.lastSolar;
        if (f.frameNumber != number) f.scheduled = false;
        f.frameNumber = number;
        f.state->getUniform("oe_sky2_eye")->set(shaderEye);
        f.state->getUniform("oe_sky2_sun")->set(osg::Vec3(sun));
        f.state->getUniform("oe_sky2_solarIrradiance")->set(solar);
        f.state->getUniform("oe_sky2_sunIndex")->set(light->getLightNum());
        auto shaderBasis = matrix3(basis);
        f.state->getUniform("oe_sky2_basis")->set(shaderBasis);
        f.state->getUniform("oe_sky2_viewToEarth")->set(matrix3(toEarth));
        osg::Matrixd earthToSky;
        earthToSky.transpose(basis);
        f.state->getUniform("oe_sky2_viewToSky")->set(matrix3(toEarth*earthToSky));
        f.state->getUniform("oe_sky2_inverseProjection")->set(osg::Matrixf::inverse(*cv.getProjectionMatrix()));
        f.state->getUniform("oe_sky2_earthToECI")->set(matrix3(earthToECI));
        float horizon = float(std::acos(-std::sqrt(std::max(0.0,
            1.0-Sky2Atmosphere::radius*Sky2Atmosphere::radius/eye.length2()))));
        f.state->getUniform("oe_sky2_horizon")->set(horizon);
        if (atmospheric && (!f.valid || shaderEye != f.lastEye))
            updateAerialRows(f,shaderEye,shaderBasis,horizon);
        f.state->getUniform("oe_sky2_settings")->set(osg::Vec4(options.sunIntensity,options.exposure,
            options.environmentIntensity,validScalar(options.ambient().get(),0.033f)));
        f.state->getUniform("oe_sky2_flags")->set(osg::Vec4(owner.getAtmosphereVisible(),owner.getSunVisible(),
            owner.getMoonVisible(),owner.getStarsVisible()));
        osg::Vec3d observerECEF = (inverse*worldToECEF).getTrans();
        osg::Vec3d toMoon = moon-observerECEF;
        double moonDistance = toMoon.normalize();
        f.state->getUniform("oe_sky2_moon")->set(osg::Vec4(float(toMoon.x()),float(toMoon.y()),float(toMoon.z()),
            float(std::asin(std::min(1.0,1737400.0/std::max(1737401.0,moonDistance))))));
        lightSource->accept(cv);
        cv.pushStateSet(f.state);
        if (atmospheric && !f.scheduled)
        {
            // Pass programs only sample their inputs; their output samplers are compiled out.
            if (skyDirty) { f.skyPass->accept(cv); f.environmentPass->accept(cv); }
            if (skyDirty) f.aerialPass->accept(cv);
            f.scheduled = true;
        }
        f.background->accept(cv);
        owner.SkyNode::traverse(cv);
        cv.popStateSet();
        f.lastEye = shaderEye; f.lastSun = sun;
        f.lastSolar = solar;
        f.valid = true;
    }
};

SkyNode2::Options::Options(const ConfigOptions& input) : SkyOptions(input)
{
    std::string value;
    input.getConfig().get("preset",value);
    if (value == "flat") preset = FLAT;
    else if (value == "high") preset = HIGH;
    else if (value.empty())
    {
        if (quality() == QUALITY_LOW) preset = FLAT;
        else if (quality() == QUALITY_HIGH || quality() == QUALITY_BEST) preset = HIGH;
    }
    input.getConfig().get("exposure",exposure);
    input.getConfig().get("sun_intensity",sunIntensity);
    input.getConfig().get("environment_intensity",environmentIntensity);
    input.getConfig().get("output_srgb",outputSRGB);
    input.getConfig().get("tone_mapping",toneMapping);
}

Config SkyNode2::Options::getConfig() const
{
    Config config = SkyOptions::getConfig();
    config.set("preset",preset == FLAT ? "flat" : preset == HIGH ? "high" : "balanced");
    config.set("exposure",exposure);
    config.set("sun_intensity",sunIntensity);
    config.set("environment_intensity",environmentIntensity);
    config.set("output_srgb",outputSRGB);
    config.set("tone_mapping",toneMapping);
    return config;
}

SkyNode2::SkyNode2(const Options& options) : SkyNode(options), _impl(new Impl(options))
{
    setName("SkyNode2");
    setCullingActive(false);
    setNumChildrenRequiringUpdateTraversal(1);
    ShaderGenerator::setIgnoreHint(this,true);
    auto ss = getOrCreateStateSet();
    ss->setDefine("OE_SKY2");
    ss->setDefine("OE_USE_PBR");
    // Fixed small capacity handles sparse OSG light indices without shader recompilation.
    ss->setDefine("OE_NUM_LIGHTS","8",osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE|osg::StateAttribute::PROTECTED);
    if (_impl->options.outputSRGB) ss->setDefine("OE_SKY2_SRGB");
    if (_impl->options.toneMapping) ss->setDefine("OE_SKY2_TONEMAP");
    Lighting::installDefaultMaterial(ss);
    auto vp = VirtualProgram::getOrCreate(ss);
    vp->setName("SkyNode2 unified lighting");
    Shaders shaders;
    shaders.load(vp,shaders.PBR);
    shaders.load(vp,"SkyNode2.Lighting.glsl");
}

SkyNode2::~SkyNode2() = default;
const SkyNode2::Options& SkyNode2::getOptions() const { return _impl->options; }
osg::Light* SkyNode2::getSunLight() const { return _impl->light; }

void SkyNode2::attach(osg::View* view, int lightNum)
{
    if (!view || lightNum < 0 || lightNum > 7) return;
    _impl->light->setLightNum(lightNum);
    view->setLight(_impl->light);
    view->setLightingMode(osg::View::NO_LIGHT);
    view->getCamera()->setClearColor(osg::Vec4(0,0,0,1));
}

void SkyNode2::setExposure(float value)
{
    if (std::isfinite(value) && value >= 0.0f) _impl->options.exposure = value;
}

void SkyNode2::setEnvironmentIntensity(float value)
{
    if (std::isfinite(value) && value >= 0.0f) _impl->options.environmentIntensity = value;
}

void SkyNode2::setSunIntensity(float value)
{
    if (std::isfinite(value) && value >= 0.0f) _impl->options.sunIntensity = value;
}

void SkyNode2::setAmbientIntensity(float value)
{
    if (std::isfinite(value) && value >= 0.0f) _impl->options.ambient() = value;
}

void SkyNode2::onSetEphemeris() { _impl->celestialDirty = true; }
void SkyNode2::onSetDateTime() { _impl->celestialDirty = true; }

void SkyNode2::onSetReferencePoint()
{
    _impl->worldToECEF.makeIdentity();
    if (getReferencePoint().isValid())
    {
        GeoPoint geographic;
        if (getReferencePoint().transform(SpatialReference::get("wgs84"),geographic))
        {
            osg::Matrixd localToECEF;
            geographic.createLocalToWorld(localToECEF);
            const auto& point = getReferencePoint();
            _impl->worldToECEF = osg::Matrixd::translate(-point.x(),-point.y(),-point.z())*localToECEF;
        }
    }
    _impl->celestialDirty = true;
}

void SkyNode2::traverse(osg::NodeVisitor& visitor)
{
    if (visitor.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR)
    {
        auto dt = getDateTime();
        if (_impl->celestialDirty.exchange(false) || dt.asTimeStamp() != _impl->date)
        {
            bool eci = _impl->options.coordinateSystem() == SkyOptions::COORDSYS_ECI;
            auto sun = getEphemeris()->getSunPosition(dt), moon = getEphemeris()->getMoonPosition(dt);
            _impl->sun = eci ? sun.eci : sun.geocentric;
            _impl->sun.normalize();
            _impl->moon = eci ? moon.eci : moon.geocentric;
            auto localSun = osg::Matrixd::transform3x3(_impl->sun,osg::Matrixd::inverse(_impl->worldToECEF));
            localSun.normalize();
            _impl->light->setPosition(osg::Vec4(float(localSun.x()),float(localSun.y()),float(localSun.z()),0.0f));
            double days = dt.getJulianDay()-2451545.0;
            double degrees = std::fmod(280.46061837+360.98564736629*days,360.0);
            _impl->earthToECI = osg::Matrixd::rotate(eci ? 0.0 : osg::DegreesToRadians(degrees),osg::Vec3d(0,0,1));
            _impl->date = dt.asTimeStamp();
        }
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->prepare(*this);
        for (auto i = _impl->views.begin(); i != _impl->views.end();)
            if (!i->second->camera.valid()) i = _impl->views.erase(i); else ++i;
    }
    auto cv = dynamic_cast<osgUtil::CullVisitor*>(&visitor);
    if (cv) _impl->cull(*this,*cv);
    else SkyNode::traverse(visitor);
}

void SkyNode2::releaseGLObjects(osg::State* state) const
{
    SkyNode::releaseGLObjects(state);
    std::lock_guard<std::mutex> lock(_impl->mutex);
    for (auto& entry : _impl->views)
    for (auto& frame : entry.second->frames)
    {
        frame.state->releaseGLObjects(state);
        if (frame.background) frame.background->releaseGLObjects(state);
        if (frame.skyPass) frame.skyPass->releaseGLObjects(state);
        if (frame.environmentPass) frame.environmentPass->releaseGLObjects(state);
        if (frame.aerialPass) frame.aerialPass->releaseGLObjects(state);
        frame.valid = false;
        frame.scheduled = false;
    }
    _impl->lightSource->releaseGLObjects(state);
}

void SkyNode2::resizeGLObjectBuffers(unsigned size)
{
    SkyNode::resizeGLObjectBuffers(size);
    std::lock_guard<std::mutex> lock(_impl->mutex);
    for (auto& entry : _impl->views)
    for (auto& frame : entry.second->frames)
    {
        frame.state->resizeGLObjectBuffers(size);
        if (frame.background) frame.background->resizeGLObjectBuffers(size);
        if (frame.skyPass) frame.skyPass->resizeGLObjectBuffers(size);
        if (frame.environmentPass) frame.environmentPass->resizeGLObjectBuffers(size);
        if (frame.aerialPass) frame.aerialPass->resizeGLObjectBuffers(size);
    }
    _impl->lightSource->resizeGLObjectBuffers(size);
}

namespace
{
    // Core registration permits <sky2> in earth files without a sky driver library.
    class SkyNode2Extension : public Extension,
        public ExtensionInterface<MapNode>, public ExtensionInterface<osg::View>, public SkyNodeFactory
    {
    public:
        META_OE_Extension(osgEarth,SkyNode2Extension,sky2);
        //! Creates a default core sky extension.
        SkyNode2Extension() = default;
        //! Retains serialized configuration until the map is connected.
        explicit SkyNode2Extension(const ConfigOptions& config) : _options(config) { }
        //! Returns the persisted extension settings.
        const ConfigOptions& getConfigOptions() const override { return _options; }
        //! Creates the same implementation used by direct construction.
        SkyNode* createSkyNode() override { return new SkyNode2(_options); }
        //! Inserts the sky above its map and selects a tangent frame for projected coordinates.
        bool connect(MapNode* map) override
        {
            if (!map) return false;
            osg::ref_ptr<SkyNode2> sky = new SkyNode2(_options);
            if (map->getMapSRS()->isProjected())
                sky->setReferencePoint(map->getMap()->getProfile()->getExtent().getCentroid());
            _map = map;
            _sky = sky;
            if (map->getNumParents() > 0)
                insertParent(sky.get(),map);
            else
                _pendingSky = sky; // Await a view, without creating a map -> extension -> sky -> map cycle.
            return true;
        }
        //! Removes the inserted group, preserving the map and other parents.
        bool disconnect(MapNode*) override
        {
            osg::ref_ptr<SkyNode2> sky;
            if (_sky.lock(sky)) removeGroup(sky.get());
            _pendingSky = nullptr;
            _sky = nullptr;
            _map = nullptr;
            return true;
        }
        //! Connects the sun light to an application's view.
        bool connect(osg::View* view) override
        {
            osg::ref_ptr<SkyNode2> sky;
            if (!view || !_sky.lock(sky)) return false;
            if (_pendingSky)
            {
                osg::ref_ptr<MapNode> map;
                if (!_map.lock(map)) return false;
                if (map->getNumParents() > 0)
                    insertParent(sky.get(),map.get());
                else
                {
                    auto viewer = dynamic_cast<osgViewer::View*>(view);
                    if (!viewer || (viewer->getSceneData() && viewer->getSceneData() != map.get())) return false;
                    sky->addChild(map.get());
                    viewer->setSceneData(sky.get());
                }
                _pendingSky = nullptr;
            }
            sky->attach(view);
            return true;
        }
        //! Removes only the light owned by this extension from a disconnected view.
        bool disconnect(osg::View* view) override
        {
            osg::ref_ptr<SkyNode2> sky;
            if (view && _sky.lock(sky) && view->getLight() == sky->getSunLight()) view->setLight(nullptr);
            return true;
        }
    private:
        SkyNode2::Options _options;
        osg::observer_ptr<SkyNode2> _sky;
        osg::observer_ptr<MapNode> _map;
        osg::ref_ptr<SkyNode2> _pendingSky;
    };
    REGISTER_OSGEARTH_EXTENSION(osgearth_sky2,SkyNode2Extension)
}
