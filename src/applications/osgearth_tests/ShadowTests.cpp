/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/Shadowing>
#include <osgEarth/ShadowingMath.h>
#include <osgEarth/Shaders>
#include <osgEarth/CameraUtils>
#include <osgEarth/LogarithmicDepthBuffer>
#include <osgEarth/Chonk>
#include <osgEarth/CullingUtils>
#include <osgEarth/MapNode>
#include <osgEarth/TerrainEngineNode>
#include <osgShadow/ConvexPolyhedron>
#include <osg/Texture2D>
#include <osg/Texture2DArray>
#include "SkyNode2TestScene.h"
#include <limits>

using namespace osgEarth;
using namespace osgEarth::Util;

// Compares direct extraction with the old polyhedron algorithm on asymmetric perspective frusta.
TEST_CASE("Shadow slice extraction preserves frustum corners", "[shadows]")
{
    for (unsigned j=0; j<20; ++j)
    {
        osg::Matrixd projection;
        projection.makeFrustum(-0.15-j*0.001,0.2,-0.1,0.13,0.2,10000.0);
        ShadowMath::Corners points;
        double n = 1.0+j*2, f = 50.0+j*10;
        REQUIRE(ShadowMath::corners(projection,n,f,points));
        osg::Matrixd slice;
        slice.makeFrustum((-0.15-j*0.001)*n/0.2,0.2*n/0.2,-0.1*n/0.2,0.13*n/0.2,n,f);
        osgShadow::ConvexPolyhedron legacy;
        legacy.setToUnitFrustum(true,true);
        legacy.transform(osg::Matrixd::inverse(slice),slice);
        std::vector<osg::Vec3d> reference;
        legacy.getPoints(reference);
        REQUIRE(reference.size() == 8);
        for (const auto& p : points)
        {
            double nearest = 1e10;
            for (const auto& r : reference) nearest = std::min(nearest,(p-r).length());
            REQUIRE(nearest < 1e-8);
        }
    }
}

// Covers reverse-Z, orthographic, invalid ranges, ECEF precision, polar sunlight and caster extrusion.
TEST_CASE("Shadow fitting covers its receivers and remains stable", "[shadows]")
{
    for (bool ortho : {false,true})
    for (auto type : {ProjectionMatrix::STANDARD,ProjectionMatrix::REVERSE_Z})
    {
        osg::Matrixd projection;
        if (ortho) ProjectionMatrix::setOrtho(projection,-40,70,-30,60,0.1,1000,type);
        else ProjectionMatrix::setPerspective(projection,55,1.7,0.1,1000,type);
        ShadowMath::Corners points;
        REQUIRE(ShadowMath::corners(projection,0,300,points));
        for (auto sun : {osg::Vec3d(0,0,1),osg::Vec3d(0,0,-1),osg::Vec3d(1,2,3)})
        {
            osg::Matrixd inverseView = osg::Matrixd::rotate(0.2,osg::Vec3d(1,0,0))*
                osg::Matrixd::translate(6378137,1200000,5000000);
            ShadowMath::Cascade fit;
            REQUIRE(ShadowMath::fit(points,inverseView,sun,2048,1000,1.5,fit));
            for (const auto& p : points)
            {
                auto t = p*fit.viewToTexture;
                for (unsigned c=0; c<3; ++c) REQUIRE(t[c] >= 0.0);
                for (unsigned c=0; c<3; ++c) REQUIRE(t[c] <= 1.0);
                osg::Vec3d upstream = p*inverseView + sun*(999.0/sun.length());
                osg::Vec3d clip = upstream*fit.view*fit.projection;
                REQUIRE(clip.z() >= -1.000001);
                REQUIRE(clip.z() <= 1.000001);
            }
            // World points stay on an integer texel grid even when the camera moves a small amount.
            osg::Vec3d fixed(6378137,1200000,5000000);
            auto original = fixed*fit.view*fit.projection;
            inverseView.postMultTranslate(osg::Vec3d(0.001,0.001,0.001));
            ShadowMath::Cascade moved;
            REQUIRE(ShadowMath::fit(points,inverseView,sun,2048,1000,1.5,moved));
            auto shifted = fixed*moved.view*moved.projection;
            for (unsigned c=0; c<2; ++c)
            {
                double texels = (shifted[c]-original[c])*1024.0;
                REQUIRE(std::abs(texels-std::round(texels)) < 1e-5);
            }
            REQUIRE(moved.texelSize == Approx(fit.texelSize));
        }
        REQUIRE_FALSE(ShadowMath::corners(projection,300,300,points));
        REQUIRE_FALSE(ShadowMath::corners(projection,-1,300,points));
    }
}

// Receiver coverage must not move when automatic clamping or log depth changes the nominal clip planes.
TEST_CASE("Shadow receiver intervals are independent of camera clip planes", "[shadows][shadow-zoom]")
{
    for (bool ortho : {false,true})
    for (auto type : {ProjectionMatrix::STANDARD,ProjectionMatrix::REVERSE_Z})
    {
        osg::Matrixd reference;
        if (ortho) ProjectionMatrix::setOrtho(reference,-40,70,-30,60,0.1,10000,type);
        else ProjectionMatrix::setPerspective(reference,55,1.7,0.1,10000,type);
        ShadowMath::Corners expected;
        REQUIRE(ShadowMath::corners(reference,0,250,expected));
        for (double nearPlane : {0.1,100.0,500.0})
        {
            osg::Matrixd projection;
            if (ortho) ProjectionMatrix::setOrtho(projection,-40,70,-30,60,nearPlane,nearPlane+100,type);
            else ProjectionMatrix::setPerspective(projection,55,1.7,nearPlane,nearPlane+100,type);
            ShadowMath::Corners actual;
            REQUIRE(ShadowMath::corners(projection,0,250,actual));
            for (unsigned i=0; i<actual.size(); ++i) REQUIRE((actual[i]-expected[i]).length() < 1e-8);
        }
    }
}

// Invalid application input must leave usable settings intact, and legacy camera markers remain available.
TEST_CASE("Shadow settings and camera contracts are retained", "[shadows]")
{
    osg::ref_ptr<ShadowCaster> caster = new ShadowCaster;
    auto ranges = caster->getRanges();
    for (const auto& invalid : {std::vector<float>{},std::vector<float>{0,0},std::vector<float>{2,1},
        std::vector<float>{-1,10},std::vector<float>{0,std::numeric_limits<float>::quiet_NaN()}})
    {
        caster->setRanges(invalid);
        REQUIRE(caster->getRanges() == ranges);
    }
    caster->setCascades(4,5000);
    REQUIRE(caster->getRanges().size() == 5);
    REQUIRE(caster->getRanges().back() == 5000);
    caster->setTextureSize(1024);
    caster->setBlurFactor(0.002f);
    REQUIRE(caster->getFilterRadius() == Approx(2.048f));
    caster->setShadowColor(2);
    REQUIRE(caster->getShadowColor() == 1);
    osg::ref_ptr<osg::Camera> camera = new osg::Camera;
    camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
    camera->attach(osg::Camera::DEPTH_BUFFER,GL_DEPTH_COMPONENT24);
    Shadowing::setIsShadowCamera(camera);
    REQUIRE(Shadowing::isShadowCamera(camera));
    REQUIRE(CameraUtils::isDepthCamera(camera));
    REQUIRE(camera->getStateSet()->getDefinePair("OE_IS_SHADOW_CAMERA") != nullptr);
    REQUIRE(camera->getStateSet()->getDefinePair("OE_IS_DEPTH_CAMERA") != nullptr);
}

namespace
{
    //! Installs a deterministic receiver and box in the ECEF test scene; the box casts westward.
    osg::ref_ptr<ShadowCaster> install(Sky2Tests::Scene& scene)
    {
        scene.models->removeChildren(0,scene.models->getNumChildren());
        for (auto shape : {osg::ref_ptr<osg::Box>(new osg::Box(osg::Vec3(0,0,-1),400,400,2)),
            osg::ref_ptr<osg::Box>(new osg::Box(osg::Vec3(0,0,8),8,8,16))})
        {
            osg::ref_ptr<osg::ShapeDrawable> drawable = new osg::ShapeDrawable(shape);
            drawable->setColor(osg::Vec4(0.7,0.7,0.7,1));
            drawable->setUseDisplayList(false);
            drawable->setUseVertexBufferObjects(true);
            scene.models->addChild(drawable);
        }
        osg::ref_ptr<ShadowCaster> caster = new ShadowCaster;
        caster->setTextureSize(1024);
        caster->setRanges({0,60,120,250});
        caster->setLight(scene.sky->getSunLight());
        caster->getShadowCastingGroup()->addChild(scene.models);
        scene.sky->removeChild(scene.models);
        caster->addChild(scene.models);
        scene.sky->addChild(caster);
        scene.viewer->getCamera()->setViewMatrixAsLookAt(osg::Vec3d(6378182,-40,-55),
            osg::Vec3d(6378137,0,0),osg::Vec3d(1,0,0));
        return caster;
    }

    //! Samples a small screen neighborhood of a known world-space receiver point.
    double sample(const Sky2Tests::Scene& scene, osg::Vec3d local)
    {
        auto camera = scene.viewer->getCamera();
        osg::Vec3d ndc = local*scene.models->getMatrix()*camera->getViewMatrix()*camera->getProjectionMatrix();
        int x = int((ndc.x()*0.5+0.5)*scene.width), y = int((ndc.y()*0.5+0.5)*scene.height);
        if (x < 1 || y < 1 || x+1 >= int(scene.width) || y+1 >= int(scene.height))
            throw std::runtime_error("Shadow probe falls outside test view");
        auto pixels = scene.pixels();
        double value = 0.0;
        for (int dy=-1; dy<=1; ++dy)
        for (int dx=-1; dx<=1; ++dx)
        for (unsigned c=0; c<3; ++c) value += pixels[((y+dy)*scene.width+x+dx)*4+c];
        return value/27.0;
    }
}

// An isolated depth step must blur into one continuous edge, independent of filter width and texel phase.
TEST_CASE("Shadow filters cover the complete footprint without duplicate edges", "[shadows][.gl][shadow-filter]")
{
    SkyNode2::Options options;
    options.preset = SkyNode2::FLAT;
    Sky2Tests::Scene scene(new SkyNode2(options),512,512);
    osg::ref_ptr<osg::Geometry> quad = osg::createTexturedQuadGeometry(osg::Vec3(-1,-1,0),
        osg::Vec3(2,0,0),osg::Vec3(0,2,0));
    quad->setCullingActive(false);
    quad->setUseDisplayList(false);
    quad->setUseVertexBufferObjects(true);
    scene.viewer->setSceneData(quad);
    auto state = quad->getOrCreateStateSet();
    GLUtils::setGlobalDefaults(state);
    state->setDefine("OE_LIGHTING");
    auto program = VirtualProgram::getOrCreate(state);
    Shaders package;
    package.replace("$OE_SHADOW_NUM_SLICES","1");
    package.load(program,package.ShadowCaster);
    ShaderLoader::load(program,R"(
        #pragma vp_function shadowFilterProbeVertex, vertex_clip, last
        in vec4 osg_Vertex;
        out vec3 oe_shadow_view;
        // Supplies exact texture coordinates without camera or ECEF rounding in this filter-only probe.
        void shadowFilterProbeVertex(inout vec4 vertex)
        {
            vertex = vec4(osg_Vertex.xy,0,1);
            oe_shadow_view = vec3(osg_Vertex.xy*0.5+0.5,-1.0);
        }
        [break]
        #pragma vp_function shadowFilterProbeOutput, fragment_lighting, last
        float oe_shadow_visibility;
        // Reads production shadow visibility directly, independently of lighting and tone mapping.
        void shadowFilterProbeOutput(inout vec4 color) { color = vec4(vec3(oe_shadow_visibility),1.0); }
    )");
    const unsigned size = 32;
    osg::ref_ptr<osg::Image> image = new osg::Image;
    image->allocateImage(size,size,1,GL_DEPTH_COMPONENT,GL_FLOAT);
    image->setInternalTextureFormat(GL_DEPTH_COMPONENT24);
    osg::ref_ptr<osg::Texture2DArray> texture = new osg::Texture2DArray;
    texture->setTextureSize(size,size,1);
    texture->setInternalFormat(GL_DEPTH_COMPONENT24);
    texture->setSourceFormat(GL_DEPTH_COMPONENT);
    texture->setSourceType(GL_FLOAT);
    texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
    texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
    texture->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);
    texture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
    texture->setShadowComparison(true);
    texture->setShadowCompareFunc(osg::Texture::LEQUAL);
    texture->setImage(0,image);
    state->setTextureAttributeAndModes(7,texture);
    state->addUniform(new osg::Uniform("oe_shadow_map",7));
    auto matrix = state->getOrCreateUniform("oe_shadow_matrix",osg::Uniform::FLOAT_MAT4,1);
    matrix->setElement(0,osg::Matrixf::scale(1,1,0)*osg::Matrixf::translate(0,0,0.5));
    state->getOrCreateUniform("oe_shadow_interval",osg::Uniform::FLOAT_VEC2,1)->setElement(0,osg::Vec2(0,10));
    state->getOrCreateUniform("oe_shadow_bias",osg::Uniform::FLOAT,1)->setElement(0,0.0f);
    state->addUniform(new osg::Uniform("oe_shadow_light",osg::Vec3(0,0,1)));
    state->addUniform(new osg::Uniform("oe_shadow_color",0.0f));
    state->addUniform(new osg::Uniform("oe_shadow_blend",0.0f));
    auto filterUniform = state->getOrCreateUniform("oe_shadow_filter",osg::Uniform::INT);
    auto radiusUniform = state->getOrCreateUniform("oe_shadow_blur",osg::Uniform::FLOAT);
    for (unsigned axis=0; axis<3; ++axis)
    {
        for (unsigned y=0; y<size; ++y)
        for (unsigned x=0; x<size; ++x)
            *reinterpret_cast<float*>(image->data(x,y)) =
                (axis == 0 ? x >= size/2 : axis == 1 ? y >= size/2 : x >= size/2 && y >= size/2) ? 0.75f : 0.25f;
        image->dirty();
        for (auto filter : {ShadowCaster::HARD,ShadowCaster::PCF,ShadowCaster::SOFT})
        for (float radius : {0.0f,0.25f,0.5f,0.75f,1.5f,2.0f,3.0f,7.9f,8.0f})
        {
            filterUniform->set(int(filter));
            radiusUniform->set(radius/size);
            scene.draw();
            if (radius == 3.0f)
                REQUIRE(scene.save("shadows-edge-"+std::to_string(axis)+"-"+
                    std::to_string(int(filter))+"-"+std::to_string(radius)+".png"));
            auto pixels = scene.pixels();
            double error = 0.0;
            for (unsigned p=0; p<scene.width; ++p)
            {
                double width = filter == ShadowCaster::HARD ? 0.5 : std::max(0.5,double(radius));
                double distance = ((p+0.5)/scene.width-0.5)*size/width;
                double expected = filter != ShadowCaster::SOFT || radius <= 0.5f ? 0.5+distance*0.5 :
                    (distance < 0 ? 0.5*std::pow(1+std::max(-1.0,distance),2) :
                        1.0-0.5*std::pow(1-std::min(1.0,distance),2));
                expected = osg::clampBetween(expected,0.0,1.0);
                if (axis == 2) expected *= expected;
                unsigned x = axis == 1 ? scene.width/2 : p, y = axis == 0 ? scene.height/2 : p;
                error = std::max(error,std::abs(pixels[(y*scene.width+x)*4]-expected));
            }
            INFO("Axis " << axis << ", filter " << int(filter) << ", radius " << radius);
            CHECK(error < 0.015);
        }
    }
}

// A tall caster must retain one smooth silhouette on both an elevated roof and the ground below it.
TEST_CASE("Shadow filters preserve rooftop and ground receivers", "[shadows][.gl][shadow-filter]")
{
    SkyNode2::Options options;
    options.preset = SkyNode2::FLAT;
    Sky2Tests::Scene scene(new SkyNode2(options),640,640);
    auto caster = install(scene);
    scene.models->removeChild(1,1);
    for (auto shape : {osg::ref_ptr<osg::Box>(new osg::Box(osg::Vec3(0,0,8),8,40,16)),
        osg::ref_ptr<osg::Box>(new osg::Box(osg::Vec3(-20,6,2),24,8,4))})
    {
        osg::ref_ptr<osg::ShapeDrawable> drawable = new osg::ShapeDrawable(shape);
        drawable->setColor(osg::Vec4(0.7,0.7,0.7,1));
        drawable->setUseDisplayList(false);
        drawable->setUseVertexBufferObjects(true);
        scene.models->addChild(drawable);
    }
    auto camera = scene.viewer->getCamera();
    camera->setViewMatrixAsLookAt(osg::Vec3d(6378237,-10,0),osg::Vec3d(6378137,-10,0),osg::Vec3d(0,0,1));
    camera->setProjectionMatrixAsOrtho(-36,36,-36,36,0.1,150);
    caster->setRanges({0,150});
    caster->setTextureSize(256);
    // Account for the wide filter footprint so this scene isolates silhouette filtering from self-shadow acne.
    caster->setDepthBias(2.0f);
    caster->setEnabled(false);
    scene.draw();
    osg::Vec3d ground(-15,-8,0), roof(-14,6,4), litRoof(-30,6,4);
    double groundReference = sample(scene,ground), roofReference = sample(scene,roof);
    double litReference = sample(scene,litRoof);
    REQUIRE(groundReference > 0.1);
    REQUIRE(roofReference > 0.1);
    caster->setEnabled(true);
    for (auto filter : {ShadowCaster::PCF,ShadowCaster::SOFT})
    {
        caster->setFilter(filter);
        caster->setFilterRadius(3.0f);
        scene.draw();
        REQUIRE(scene.save("shadows-roof-ground-"+std::to_string(int(filter))+".png"));
        REQUIRE(sample(scene,ground) < groundReference*0.8);
        REQUIRE(sample(scene,roof) < roofReference*0.8);
        REQUIRE(std::abs(sample(scene,litRoof)-litReference) < 0.03);
    }
}

// Exercises the production GL pipeline, all filters, view projections, runtime settings, and sunlight-only masking.
TEST_CASE("Shadow maps occlude known receivers without darkening lit ground", "[shadows][.gl]")
{
    SkyNode2::Options options;
    options.preset = SkyNode2::FLAT;
    Sky2Tests::Scene scene(new SkyNode2(options),640,480);
    auto caster = install(scene);
    caster->setEnabled(false);
    scene.draw();
    double unobstructed = sample(scene,osg::Vec3d(-14,0,0));
    double lit = sample(scene,osg::Vec3d(15,0,0));
    REQUIRE(scene.save("shadows-disabled.png"));
    REQUIRE(unobstructed > 0.1);
    caster->setEnabled(true);
    for (auto filter : {ShadowCaster::HARD,ShadowCaster::PCF,ShadowCaster::SOFT})
    {
        caster->setFilter(filter);
        scene.draw();
        scene.draw();
        REQUIRE(scene.save("shadows-filter-"+std::to_string(int(filter))+".png"));
        INFO("lit reference " << lit << " lit shadowed " << sample(scene,osg::Vec3d(15,0,0)));
        REQUIRE(sample(scene,osg::Vec3d(-14,0,0)) < unobstructed*0.8);
        REQUIRE(std::abs(sample(scene,osg::Vec3d(15,0,0))-lit) < 0.03);
    }
    REQUIRE(scene.save("shadows-perspective.png"));
    caster->setShadowColor(1.0f);
    scene.draw();
    REQUIRE(std::abs(sample(scene,osg::Vec3d(-14,0,0))-unobstructed) < 0.02);
    caster->setShadowColor(0.0f);
    scene.viewer->getCamera()->setProjectionMatrixAsOrtho(-50,50,-37.5,37.5,0.1,1000);
    scene.draw();
    REQUIRE(sample(scene,osg::Vec3d(-14,0,0)) < unobstructed*0.8);
    REQUIRE(scene.save("shadows-orthographic.png"));
    caster->setRanges({0,250});
    caster->setTextureSize(2048);
    scene.draw();
    REQUIRE(sample(scene,osg::Vec3d(-14,0,0)) < unobstructed*0.8);
    caster->setTraversalMask(0u);
    scene.draw();
    REQUIRE(std::abs(sample(scene,osg::Vec3d(-14,0,0))-unobstructed) < 0.02);
    scene.models->getChild(0)->setNodeMask(2u);
    scene.models->getChild(1)->setNodeMask(1u);
    caster->setTraversalMask(2u);
    scene.draw();
    REQUIRE(std::abs(sample(scene,osg::Vec3d(-14,0,0))-unobstructed) < 0.02);
    caster->setTraversalMask(~0u);
    caster->releaseGLObjects(scene.context->getState());
    scene.draw(); scene.draw();
    REQUIRE(sample(scene,osg::Vec3d(-14,0,0)) < unobstructed*0.8);
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Log depth exposes receivers before the automatically clamped near plane as the camera approaches a shadow.
TEST_CASE("Shadow coverage survives automatic near far adjustment while approaching geometry",
    "[shadows][.gl][shadow-zoom]")
{
    SkyNode2::Options options;
    options.preset = SkyNode2::FLAT;
    Sky2Tests::Scene scene(new SkyNode2(options),640,480);
    auto caster = install(scene);
    auto camera = scene.viewer->getCamera();
    camera->setComputeNearFarMode(osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES);
    // The distant sky bound puts the clamped near plane through the receiver during this zoom sequence.
    camera->setNearFarRatio(2e-5);
    LogarithmicDepthBuffer logDepth;
    logDepth.setUseFragDepth(true);
    logDepth.install(camera);
    for (double scale : {1.5,1.0,0.75,0.5})
    {
        camera->setViewMatrixAsLookAt(osg::Vec3d(6378137+45*scale,-40*scale,-55*scale),
            osg::Vec3d(6378137,0,0),osg::Vec3d(1,0,0));
        for (double nearPlane : {0.1,100.0})
        {
            INFO("Zoom scale " << scale << ", input near plane " << nearPlane);
            camera->setProjectionMatrixAsPerspective(55.0,640.0/480.0,nearPlane,10000.0);
            caster->setEnabled(false);
            scene.draw();
            REQUIRE(scene.save("shadows-zoom-reference.png"));
            double reference = sample(scene,osg::Vec3d(-14,0,0));
            REQUIRE(reference > 0.1);
            caster->setEnabled(true);
            scene.draw(); scene.draw();
            REQUIRE(scene.save("shadows-zoom-"+std::to_string(scale)+"-"+std::to_string(nearPlane)+".png"));
            REQUIRE(sample(scene,osg::Vec3d(-14,0,0)) < reference*0.8);
        }
    }
}

// A known shadow remains present across cascade boundaries and fades continuously at the maximum range.
TEST_CASE("Shadow cascade transitions retain occlusion and fade at distance", "[shadows][.gl]")
{
    SkyNode2::Options options;
    options.preset = SkyNode2::FLAT;
    Sky2Tests::Scene scene(new SkyNode2(options),512,512);
    auto caster = install(scene);
    caster->setEnabled(false);
    scene.draw();
    double lit = sample(scene,osg::Vec3d(-14,0,0));
    caster->setEnabled(true);
    for (double height : {55.0,59.0,60.0,61.0,115.0,119.0,120.0,121.0})
    {
        scene.viewer->getCamera()->setViewMatrixAsLookAt(osg::Vec3d(6378137+height,-14,0),
            osg::Vec3d(6378137,-14,0),osg::Vec3d(0,0,1));
        scene.draw();
        INFO("Receiver depth " << height);
        REQUIRE(sample(scene,osg::Vec3d(-14,0,0)) < lit*0.8);
    }
    double previous = 0.0;
    for (double height : {225.0,240.0,249.0,251.0})
    {
        scene.viewer->getCamera()->setViewMatrixAsLookAt(osg::Vec3d(6378137+height,-14,0),
            osg::Vec3d(6378137,-14,0),osg::Vec3d(0,0,1));
        scene.draw();
        double value = sample(scene,osg::Vec3d(-14,0,0));
        REQUIRE(value >= previous);
        previous = value;
    }
    REQUIRE(previous > lit*0.95);
}

// Casters above and outside the receiver camera still contribute when upstream extrusion admits them.
TEST_CASE("Shadow maps include offscreen casters", "[shadows][.gl]")
{
    SkyNode2::Options options;
    options.preset = SkyNode2::FLAT;
    Sky2Tests::Scene scene(new SkyNode2(options),512,512);
    auto caster = install(scene);
    scene.models->removeChild(1,1);
    osg::ref_ptr<osg::ShapeDrawable> blocker = new osg::ShapeDrawable(
        new osg::Box(osg::Vec3(100,0,70),12,12,8));
    blocker->setUseDisplayList(false);
    blocker->setUseVertexBufferObjects(true);
    scene.models->addChild(blocker);
    auto camera = scene.viewer->getCamera();
    camera->setViewMatrixAsLookAt(osg::Vec3d(6378147,0,0),osg::Vec3d(6378137,0,0),osg::Vec3d(0,0,1));
    camera->setProjectionMatrixAsOrtho(-30,30,-30,30,1,30);
    caster->setRanges({0,30});
    caster->setCasterDistance(0);
    scene.draw();
    double reference = sample(scene,osg::Vec3d(0,0,0));
    caster->setCasterDistance(200);
    scene.draw();
    REQUIRE(sample(scene,osg::Vec3d(0,0,0)) < reference*0.8);
    REQUIRE(scene.save("shadows-offscreen.png"));
    // Alpha discard must still run in depth-only passes; no replacement opaque fragment program is installed.
    ShaderLoader::load(VirtualProgram::getOrCreate(blocker->getOrCreateStateSet()),R"(
        #pragma vp_function shadowTestDiscard, fragment_coloring
        // Emulates a completely transparent cutout in both receiver and depth passes.
        void shadowTestDiscard(inout vec4 color) { discard; }
    )");
    scene.draw();
    REQUIRE(std::abs(sample(scene,osg::Vec3d(0,0,0))-reference) < 0.02);
}

// Two differently positioned cameras cull the same caster before drawing; neither may overwrite the other's maps.
TEST_CASE("Shadow maps isolate simultaneous camera state", "[shadows][.gl]")
{
    SkyNode2::Options options;
    options.preset = SkyNode2::FLAT;
    Sky2Tests::Scene scene(new SkyNode2(options),640,480);
    auto caster = install(scene);
    scene.draw();
    auto reference = scene.pixels();
    osg::ref_ptr<osg::Camera> other = new osg::Camera;
    other->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
    other->setRenderOrder(osg::Camera::PRE_RENDER);
    other->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
    other->setViewport(0,0,320,240);
    other->setViewMatrixAsLookAt(osg::Vec3d(6378190,90,10),osg::Vec3d(6378137,0,0),osg::Vec3d(1,0,0));
    other->setProjectionMatrixAsPerspective(70,4.0/3.0,0.1,1000);
    other->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    osg::ref_ptr<osg::Texture2D> color = new osg::Texture2D;
    color->setTextureSize(320,240);
    color->setInternalFormat(GL_RGBA8);
    other->attach(osg::Camera::COLOR_BUFFER,color);
    other->attach(osg::Camera::DEPTH_BUFFER,GL_DEPTH_COMPONENT24);
    other->addChild(scene.sky);
    osg::ref_ptr<osg::Group> root = new osg::Group;
    root->addChild(scene.sky);
    root->addChild(other);
    scene.viewer->setSceneData(root);
    for (unsigned i=0; i<3; ++i)
    {
        scene.draw();
        auto actual = scene.pixels();
        double error = 0;
        for (unsigned p=0; p<actual.size(); ++p) error = std::max(error,double(std::abs(actual[p]-reference[p])));
        REQUIRE(error < 0.01);
    }
    scene.viewer->setSceneData(scene.sky);
    other->releaseGLObjects(scene.context->getState());
}

// Uses the real terrain engine together with atmospheric SkyNode2 and its PBR lighting contract.
TEST_CASE("Shadow maps integrate with terrain and atmospheric SkyNode2", "[shadows][.gl]")
{
    Sky2Tests::Scene scene(new SkyNode2,640,480);
    auto caster = install(scene);
    osg::ref_ptr<MapNode> map = new MapNode;
    REQUIRE(map->open());
    int unit = -1;
    REQUIRE(map->getTerrainEngine()->getResources()->reserveTextureImageUnit(unit,"Shadow test"));
    caster->setTextureImageUnit(unit);
    caster->addChild(map);
    caster->getShadowCastingGroup()->addChild(map->getTerrainEngine()->getNode());
    map->getTerrainEngine()->getNode()->getOrCreateStateSet()->setDefine("OE_TERRAIN_CAST_SHADOWS");
    caster->setEnabled(false);
    for (unsigned i=0; i<4; ++i) scene.draw();
    double reference = sample(scene,osg::Vec3d(-14,0,0));
    caster->setEnabled(true);
    for (unsigned i=0; i<4; ++i) scene.draw();
    REQUIRE(sample(scene,osg::Vec3d(-14,0,0)) < reference*0.95);
    REQUIRE(sample(scene,osg::Vec3d(-14,0,0)) > 0.01); // Indirect sky survives full solar occlusion.
    REQUIRE(scene.save("shadows-atmosphere.png"));
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Validates the shadow-camera defines and primary-view LOD uniforms in the production GPU-culling draw path.
TEST_CASE("Shadow maps support Chonk GPU culling", "[shadows][.gl]")
{
    if (!Capabilities::get().supportsNVGL())
    {
        WARN("Chonk shadows require NVGL and OSG_GL_CONTEXT_VERSION=4.6");
        return;
    }
    SkyNode2::Options options;
    options.preset = SkyNode2::FLAT;
    Sky2Tests::Scene scene(new SkyNode2(options),640,480);
    scene.viewer->getCamera()->addCullCallback(new InstallCameraUniform);
    auto caster = install(scene);
    osg::ref_ptr<osg::Group> source = new osg::Group;
    for (unsigned i=0; i<scene.models->getNumChildren(); ++i) source->addChild(scene.models->getChild(i));
    osg::ref_ptr<TextureArena> textures = new TextureArena;
    ChonkFactory factory(textures);
    auto chonk = factory.getOrCreateChonk(source);
    REQUIRE(chonk != nullptr);
    osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable;
    drawable->add(chonk);
    drawable->setUseGPUCulling(true);
    scene.models->removeChildren(0,scene.models->getNumChildren());
    scene.models->addChild(drawable);
    scene.models->getOrCreateStateSet()->setAttribute(textures);
    scene.models->getOrCreateStateSet()->addUniform(new osg::Uniform("oe_sse",0.0f));
    caster->setEnabled(false);
    scene.draw(); scene.draw();
    double reference = sample(scene,osg::Vec3d(-14,0,0));
    REQUIRE(reference > 0.1);
    caster->setEnabled(true);
    scene.draw(); scene.draw();
    REQUIRE(sample(scene,osg::Vec3d(-14,0,0)) < reference*0.8);
    REQUIRE(scene.save("shadows-chonk.png"));
    REQUIRE(glGetError() == GL_NO_ERROR);
}
