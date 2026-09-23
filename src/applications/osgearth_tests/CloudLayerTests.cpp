/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/CloudLayer>
#include <osgEarth/LogarithmicDepthBuffer>
#include <osgEarth/MapNode>
#include <osgEarth/ExampleResources>
#include <osgEarth/NodeUtils>
#include <osgEarth/EarthManipulator>
#include <osgEarth/WindLayer>
#include <osgEarth/TerrainEngineNode>
#include "SkyNode2TestScene.h"
#include "CloudShadowTestScene.h"
#include <limits>
#include <iostream>
#include <chrono>
#include <cstdlib>

using namespace osgEarth;
using namespace osgEarth::Sky2Tests;

namespace
{
    //! Compares framebuffer images in display space, rejecting non-finite transport output.
    double difference(const std::vector<float>& a, const std::vector<float>& b)
    {
        REQUIRE(a.size() == b.size());
        double sum = 0.0;
        bool finite = true;
        for (unsigned i=0; i<a.size(); ++i)
        {
            finite = finite && std::isfinite(a[i]) && std::isfinite(b[i]);
            sum += std::abs(a[i]-b[i]);
        }
        REQUIRE(finite);
        return sum/a.size();
    }

    //! Returns repeatable weather, eliminating wall-clock wind movement from visual comparisons.
    CloudLayer::Options weather()
    {
        CloudLayer::Options options;
        options.wind.set(0,0,0);
        options.coverage = 0.8f;
        options.quality = CloudLayer::LOW;
        return options;
    }

    //! Measures coherent horizontal integration error above the horizon against an independently denser march.
    double bandError(const std::vector<float>& actual, const std::vector<float>& reference, unsigned width, unsigned height)
    {
        std::vector<double> rows(height,0.0);
        for (unsigned y=height/2; y<height; ++y)
        for (unsigned x=0; x<width; ++x)
        for (unsigned c=0; c<3; ++c)
            rows[y] += (actual[(y*width+x)*4+c]-reference[(y*width+x)*4+c])/(3.0*width);
        double sum = 0.0;
        for (unsigned y=height/2+1; y+1<height; ++y)
        {
            double curvature = rows[y-1]-2.0*rows[y]+rows[y+1];
            sum += curvature*curvature;
        }
        return std::sqrt(sum/(height-height/2-2));
    }

    //! Displays shadow/cloud transmission; returns the scene-owned state for optional diagnostic uniforms or shaders.
    osg::StateSet* shadowProbe(Scene& scene)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(osg::Vec3(-1,-1,0));
        vertices->push_back(osg::Vec3(3,-1,0));
        vertices->push_back(osg::Vec3(-1,3,0));
        geometry->setVertexArray(vertices);
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
        geometry->setUseDisplayList(false);
        geometry->setUseVertexBufferObjects(true);
        geometry->setCullingActive(false);
        auto state = geometry->getOrCreateStateSet();
        state->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS));
        ShaderLoader::load(VirtualProgram::getOrCreate(state),R"(
            #pragma vp_function cloudTestClip, vertex_clip, 0.9
            in vec4 osg_Vertex;
            // Covers the framebuffer independently of the test camera's solar elevation.
            void cloudTestClip(inout vec4 vertex) { vertex=vec4(osg_Vertex.xy,0,1); }
            [break]
            #pragma vp_function cloudTestShadow, fragment_lighting, 0.9
            uniform vec3 oe_cloud_eye, oe_cloud_sun;
            float oe_cloud_shadow(vec3 position);
            vec4 oe_cloud_sample(vec3 direction, float distance);
            // Both values integrate the same sunward column; green comes from the visible-cloud volume.
            void cloudTestShadow(inout vec4 color)
            {
                color=vec4(oe_cloud_shadow(oe_cloud_eye),oe_cloud_sample(oe_cloud_sun,1000000.0).a,0,1);
            }
        )");
        scene.sky->addChild(geometry);
        return state;
    }

    //! Advances both cloud frame slots at an explicit simulation time, retaining shader/GL error detection.
    void drawAtTime(Scene& scene, double time)
    {
        unsigned errors = Diagnostics::get().errors.load();
        scene.viewer->frame(time);
        scene.context->makeCurrent();
        REQUIRE(Diagnostics::get().errors.load() == errors);
    }
}

// Opt-in local-asset captures use the actual city pipeline; they are not required on machines without Prestige data.
TEST_CASE("Capture cloud rays over a local city", "[cloudraycity][.gl]")
{
    const char* filename=std::getenv("CLOUD_RAY_CITY");
    REQUIRE(filename != nullptr);
    GLUtils::useNVGL(true);
    std::vector<std::string> args={"cloud-city",filename,"--nvgl"};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(&arg[0]);
    int argc=int(argv.size());
    osg::ArgumentParser arguments(&argc,argv.data());
    osg::ref_ptr<osgViewer::Viewer> loader=new osgViewer::Viewer;
    auto root=MapNodeHelper().load(arguments,loader);
    auto sky=findTopMostNodeOfType<SkyNode2>(root.get());
    auto map=MapNode::get(root.get());
    REQUIRE(sky != nullptr); REQUIRE(map != nullptr);
    Scene scene(sky,1280,720);
    scene.models->setNodeMask(0);
    auto manip=new Util::EarthManipulator;
    scene.viewer->setCameraManipulator(manip); manip->setNode(sky);
    const auto viewpoints=map->getConfig().child("viewpoints").children("viewpoint");
    REQUIRE_FALSE(viewpoints.empty());
    manip->setViewpoint(Viewpoint(viewpoints.front()),0.0);
    LogarithmicDepthBuffer depth; depth.install(scene.viewer->getCamera());
    auto options=weather(); options.coverage=0.55f; options.quality=CloudLayer::BALANCED;
    options.crepuscularRays=true; options.rayQuality=CloudLayer::HIGH; options.rayHaze=0.5f;
    options.rayDistance=30000.0f;
    if (const char* value=std::getenv("CLOUD_RAY_CAPTURE_INTENSITY")) options.rayIntensity=std::stof(value);
    osg::ref_ptr<CloudLayer> layer=new CloudLayer(options); sky->setCloudLayer(layer);
    scene.readback->enabled=false;
    auto start=std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<20.0)
        drawAtTime(scene,0.0);
    scene.readback->enabled=true;
    osg::Vec3d eye,center,up;
    scene.viewer->getCamera()->getViewMatrixAsLookAt(eye,center,up);
    osg::Vec3d vertical=eye; vertical.normalize();
    osg::Vec3d forward=center-eye; forward-=vertical*(forward*vertical); forward.normalize();
    osg::Vec3d right=forward^vertical; right.normalize();
    // A controlled sun lets different renderer versions see identical illumination at this geographic viewpoint.
    for (double elevation : {12.0,25.0,40.0})
    {
        osg::ref_ptr<Sun> sun=new Sun(elevation);
        double angle=osg::DegreesToRadians(elevation), yaw=osg::DegreesToRadians(20.0);
        sun->direction=vertical*std::sin(angle)+(forward*std::cos(yaw)+right*std::sin(yaw))*std::cos(angle);
        sky->setEphemeris(sun);
        for (unsigned seed : {1u,2u})
        for (bool rays : {false,true})
        {
            options.seed=seed; options.crepuscularRays=rays; layer->setOptions(options); drawAtTime(scene,0.0);
            std::string name="cloud-city-"+std::to_string(int(elevation))+"-"+std::to_string(seed)+(rays ? "-on" : "-off");
            REQUIRE(scene.save(name+".png"));
        }
    }
    // A thinner broken deck provides through-openings; thick low-sun columns can be opaque even at visible gaps.
    options.baseAltitude=1500.0f; options.topAltitude=2300.0f; options.size=1500.0f;
    options.coverage=0.6f; options.erosion=0.4f; options.density=4.0f;
    for (double elevation : {20.0,35.0})
    {
        osg::ref_ptr<Sun> sun=new Sun(elevation);
        double angle=osg::DegreesToRadians(elevation), yaw=osg::DegreesToRadians(20.0);
        sun->direction=vertical*std::sin(angle)+(forward*std::cos(yaw)+right*std::sin(yaw))*std::cos(angle);
        sky->setEphemeris(sun);
        for (unsigned seed : {1u,2u,3u,4u})
        for (bool rays : {false,true})
        {
            options.seed=seed; options.crepuscularRays=rays; layer->setOptions(options); drawAtTime(scene,0.0);
            std::string name="cloud-city-broken-"+std::to_string(int(elevation))+"-"+std::to_string(seed)+
                (rays ? "-on" : "-off");
            REQUIRE(scene.save(name+".png"));
        }
    }
    osg::ref_ptr<Sun> sun=new Sun(20.0);
    double angle=osg::DegreesToRadians(20.0);
    sun->direction=vertical*std::sin(angle)+(forward*std::cos(angle)+right*std::sin(angle))*std::cos(angle);
    sky->setEphemeris(sun);
    options.seed=3; options.crepuscularRays=true; layer->setOptions(options);
    scene.viewer->setCameraManipulator(nullptr);
    osg::Vec3d direction=center-eye; direction.normalize();
    std::vector<float> first;
    for (unsigned i=0;i<4;++i)
    {
        double height=i==3 ? 0.0 : 350.0*i;
        scene.viewer->getCamera()->setViewMatrixAsLookAt(eye+vertical*height,eye+direction*3000.0,vertical);
        drawAtTime(scene,0.0);
        if (i==0) first=scene.pixels();
        REQUIRE(scene.save("cloud-city-motion-"+std::to_string(i)+".png"));
    }
    auto restored=scene.pixels();
    double skyError=0.0;
    for (unsigned i=scene.width*scene.height*3;i<restored.size();++i)
        skyError+=std::abs(first[i]-restored[i]);
    CHECK(skyError/(scene.width*scene.height) < 0.002);
    options.wind=osg::Vec3(right*100.0); layer->setOptions(options);
    drawAtTime(scene,20.0);
    REQUIRE(scene.save("cloud-city-wind.png"));
    CHECK(difference(first,scene.pixels()) > 0.003);
    CHECK(glGetError() == GL_NO_ERROR);
}

// Shared sources retain units, local east/north directions, runtime edits, and configuration before opening.
TEST_CASE("WindLayer exposes shared directional weather", "[clouds][cloudsharedwind][wind]")
{
    WindLayer::Options input(Config("wind"));
    osg::ref_ptr<WindLayer> wind = new WindLayer(input);
    osg::ref_ptr<Wind> east = new Wind;
    east->setDirection(osg::Vec2(2,0)); east->setSpeed(Speed(10,Units::KNOTS));
    wind->addWind(east);
    osg::ref_ptr<Wind> north = new Wind;
    north->setDirection(osg::Vec2(0,3)); north->setSpeed(Speed(4,Units::METERS_PER_SECOND));
    wind->addWind(north);
    osg::ref_ptr<Wind> point = new Wind;
    point->setType(Wind::TYPE_POINT); point->setSpeed(Speed(100,Units::METERS_PER_SECOND));
    wind->addWind(point);
    wind->setSpeedFactor(2.0f);
    auto velocity = wind->getDirectionalVelocity();
    CHECK(velocity.x() == Approx(Speed(20,Units::KNOTS).as(Units::METERS_PER_SECOND)));
    CHECK(velocity.y() == Approx(8.0));
    WindLayer::Options copy(wind->getConfig());
    osg::ref_ptr<WindLayer> restored = new WindLayer(copy);
    CHECK((restored->getDirectionalVelocity()-velocity).length() < 1e-5);
    wind->removeWind(north);
    CHECK(wind->getDirectionalVelocity().y() == 0.0);
    east->setSpeed(Speed(3,Units::METERS_PER_SECOND));
    CHECK(wind->getDirectionalVelocity().x() == Approx(6.0));
    wind->setSpeedFactor(-1.0f);
    CHECK(wind->getDirectionalVelocity().length() == 0.0);
    wind->setSpeedFactor(std::numeric_limits<float>::quiet_NaN());
    CHECK(wind->getSpeedFactor() == 1.0f);
}

// The vegetation LUT must use the same physical units and shared multiplier as cloud motion, including calm wind.
TEST_CASE("WindLayer GPU consumers share speed units and multiplier", "[clouds][cloudsharedwind][wind][.gl]")
{
    osg::ref_ptr<Map> map = new Map;
    osg::ref_ptr<WindLayer> wind = new WindLayer;
    osg::ref_ptr<Wind> source = new Wind;
    source->setSpeed(Speed(10,Units::METERS_PER_SECOND)); wind->addWind(source);
    map->addLayer(wind);
    osg::ref_ptr<MapNode> mapNode = new MapNode(map);
    osg::ref_ptr<SkyNode2> sky = new SkyNode2; sky->addChild(mapNode);
    Scene scene(sky,32,32);
    scene.models->setNodeMask(0);
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
    vertices->push_back(osg::Vec3(-1,-1,0)); vertices->push_back(osg::Vec3(3,-1,0));
    vertices->push_back(osg::Vec3(-1,3,0)); geometry->setVertexArray(vertices);
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    geometry->setUseDisplayList(false); geometry->setUseVertexBufferObjects(true); geometry->setCullingActive(false);
    auto state = geometry->getOrCreateStateSet();
    state->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS));
    state->setRenderBinDetails(100,"RenderBin");
    ShaderLoader::load(VirtualProgram::getOrCreate(state),R"(
        #pragma vp_function windUnitClip, vertex_clip, 0.9
        in vec4 osg_Vertex;
        // Covers the viewport after the shared wind compute pass.
        void windUnitClip(inout vec4 vertex) { vertex=vec4(osg_Vertex.xy,0,1); }
        [break]
        #pragma vp_function windUnitSample, fragment_lighting, 0.99
        #pragma import_defines(OE_WIND_TEX)
        uniform sampler3D OE_WIND_TEX;
        uniform float oe_wind_power;
        // Exposes encoded speed and multiplier without lighting or output transforms.
        void windUnitSample(inout vec4 color)
        {
            color=vec4(texture(OE_WIND_TEX,vec3(0.5)).a,oe_wind_power/10.0,0,1);
        }
    )");
    mapNode->addChild(geometry);
    wind->setSpeedFactor(3.0f); drawAtTime(scene,0.0);
    auto pixels = scene.pixels();
    CHECK(std::abs(pixels[0]-10.0/50.0) < 0.005);
    CHECK(std::abs(pixels[1]-0.3) < 0.005);
    CHECK(wind->getDirectionalVelocity().x() == Approx(30.0));
    wind->removeWind(source); drawAtTime(scene,1.0);
    CHECK(scene.pixels()[0] == 0.0f);
    CHECK(glGetError() == GL_NO_ERROR);
}

// Automatic map wind drives one weather phase across views; changing speed must never rephase existing clouds.
TEST_CASE("Clouds reuse map wind without motion discontinuities", "[clouds][cloudsharedwind][wind][.gl]")
{
    osg::ref_ptr<Map> map = new Map;
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    osg::ref_ptr<MapNode> mapNode = new MapNode(map);
    sky->addChild(mapNode);
    auto options = weather(); options.coverage = 0.6f; options.crepuscularRays = true;
    options.rayQuality = CloudLayer::LOW; options.rayHaze = 0.5f;
    osg::ref_ptr<CloudLayer> clouds = new CloudLayer(options);
    sky->setCloudLayer(clouds);
    Scene scene(sky,384,216); scene.skyView(350.0);
    // Keep map discovery and wind traversal, while excluding asynchronous terrain paging from pixel comparisons.
    drawAtTime(scene,0.0);
    auto terrainNode = dynamic_cast<osg::Node*>(mapNode->getTerrainEngine());
    REQUIRE(terrainNode != nullptr); terrainNode->setNodeMask(0);
    drawAtTime(scene,0.0); auto initial = scene.pixels();
    osg::ref_ptr<WindLayer> wind = new WindLayer;
    osg::ref_ptr<Wind> source = new Wind;
    source->setSpeed(Speed(100,Units::METERS_PER_SECOND));
    wind->addWind(source); map->addLayer(wind);
    REQUIRE(wind->isOpen());
    drawAtTime(scene,0.0);
    CHECK(difference(initial,scene.pixels()) < 0.0001);
    drawAtTime(scene,60.0); auto moved = scene.pixels();
    CHECK(difference(initial,moved) > 0.003);
    CHECK(wind->getDirectionalDisplacement(60.0).x() == Approx(6000.0));
    wind->setSpeedFactor(0.0f); drawAtTime(scene,60.0);
    CHECK(difference(moved,scene.pixels()) < 0.0001);
    drawAtTime(scene,120.0);
    CHECK(difference(moved,scene.pixels()) < 0.0001);
    // A second independent view sees identical weather and cannot advance the shared phase twice.
    Scene other(sky,384,216); other.skyView(350.0);
    drawAtTime(other,120.0);
    CHECK(difference(moved,other.pixels()) < 0.0001);
    source->setDirection(osg::Vec2(0,1)); wind->setSpeedFactor(2.0f);
    drawAtTime(scene,120.0);
    CHECK(difference(moved,scene.pixels()) < 0.0001);
    drawAtTime(scene,150.0); auto turned = scene.pixels();
    CHECK(difference(moved,turned) > 0.003);
    CHECK(wind->getDirectionalDisplacement(150.0).y() == Approx(6000.0));
    wind->close(); drawAtTime(scene,150.0); drawAtTime(scene,180.0);
    CHECK(difference(turned,scene.pixels()) < 0.0001);
    REQUIRE(wind->open().isOK());
    drawAtTime(scene,180.0); drawAtTime(scene,210.0);
    CHECK(difference(turned,scene.pixels()) > 0.003);
    // Explicit providers override map discovery; a removed layer restores the calm fallback.
    osg::ref_ptr<WindLayer> calm = new WindLayer;
    REQUIRE(calm->open().isOK());
    clouds->setWindLayer(calm); drawAtTime(scene,210.0);
    CHECK(difference(initial,scene.pixels()) < 0.0001);
    clouds->setWindLayer(nullptr); map->removeLayer(wind); drawAtTime(scene,210.0);
    CHECK(difference(initial,scene.pixels()) < 0.0001);
    CHECK(glGetError() == GL_NO_ERROR);
}

// Shared east/north advection must feed visible density and projected shadows with the identical moving coordinates.
TEST_CASE("Shared wind keeps cloud and shadow density aligned", "[clouds][cloudsharedwind][wind][.gl]")
{
    osg::ref_ptr<WindLayer> wind = new WindLayer;
    osg::ref_ptr<Wind> source = new Wind;
    source->setSpeed(Speed(100,Units::METERS_PER_SECOND)); wind->addWind(source);
    REQUIRE(wind->open().isOK());
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    auto options = weather(); options.coverage = 0.55f; options.shadowStrength = 1.0f;
    options.resolution = 128; options.samples = 256;
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(options); layer->setWindLayer(wind);
    sky->setCloudLayer(layer);
    Scene scene(sky,64,64); scene.skyView(2.0); shadowProbe(scene);
    osg::ref_ptr<Sun> sun = new Sun(35.0); sky->setEphemeris(sun);
    osg::Vec3d eye(6378139.0,0,0);
    scene.viewer->getCamera()->setViewMatrixAsLookAt(eye,eye+sun->direction,osg::Vec3d(0,0,1));
    double error = 0.0;
    float lo = 1.0f, hi = 0.0f;
    for (unsigned step=0; step<24; ++step)
    {
        drawAtTime(scene,step*10.0); auto pixels = scene.pixels();
        error += std::abs(pixels[0]-pixels[1]);
        lo = std::min(lo,pixels[0]); hi = std::max(hi,pixels[0]);
    }
    CHECK(error/24.0 < 0.04);
    CHECK(hi-lo > 0.1f);
    CHECK(wind->getDirectionalDisplacement(0.0).length() == 0.0);
    CHECK(wind->getDirectionalDisplacement(10.0).x() == Approx(1000.0));
    CHECK(wind->getDirectionalDisplacement(std::numeric_limits<double>::quiet_NaN()).x() == Approx(1000.0));
    CHECK(glGetError() == GL_NO_ERROR);
}

// Animated shadows must follow the visible, eroded cloud field even with a stationary camera and fixed sunlight.
TEST_CASE("Ground cloud shadows track the visible wind-advected cloud column", "[clouds][cloudwind][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    auto options = weather();
    options.coverage = 0.55f; options.shadowStrength = 1.0f;
    options.resolution = 128; options.samples = 256;
    options.wind.set(0,100,0);
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(options);
    sky->setCloudLayer(layer);
    Scene scene(sky,64,64);
    scene.skyView(2.0);
    shadowProbe(scene);
    for (double elevation : {35.0,80.0})
    {
        osg::ref_ptr<Sun> sun = new Sun(elevation);
        sky->setEphemeris(sun);
        osg::Vec3d eye(6378139.0,0,0);
        scene.viewer->getCamera()->setViewMatrixAsLookAt(eye,eye+sun->direction,osg::Vec3d(0,0,1));
        for (auto quality : {CloudLayer::LOW,CloudLayer::BALANCED,CloudLayer::HIGH})
        for (float erosion : {0.0f,0.3f,0.6f})
        {
            options.quality = quality; options.erosion = erosion; layer->setOptions(options);
            double error = 0.0, largest = 0.0;
            float minimum = 1.0f, maximum = 0.0f;
            std::vector<float> first, last;
            for (unsigned step=0; step<24; ++step)
            {
                drawAtTime(scene,step*10.0);
                auto values = scene.pixels();
                if (step == 0) first = values;
                if (step == 23) last = values;
                float shadow = values[0], cloud = values[1];
                error += std::abs(shadow-cloud);
                largest = std::max(largest,double(std::abs(shadow-cloud)));
                minimum = std::min(minimum,shadow); maximum = std::max(maximum,shadow);
            }
            std::cout << "Cloud wind sun=" << elevation << " quality=" << int(quality)
                << " erosion=" << erosion << " mean=" << error/24.0
                << " max=" << largest << " shadow range=" << maximum-minimum << '\n';
            INFO(elevation); INFO(quality); INFO(erosion);
            // The finite shadow footprint softens edges, so compare mean column error across the animation.
            CHECK(error/24.0 < 0.04);
            CHECK(maximum-minimum > 0.1f);
            drawAtTime(scene,230.0);
            CHECK(difference(last,scene.pixels()) < 0.0001); // Both frame slots must agree at identical times.
            auto stopped = options; stopped.wind.set(0,0,0);
            layer->setOptions(stopped);
            drawAtTime(scene,240.0);
            CHECK(difference(first,scene.pixels()) < 0.0001);
            drawAtTime(scene,250.0);
            CHECK(difference(first,scene.pixels()) < 0.0001);
        }
    }
    CHECK(glGetError() == GL_NO_ERROR);
}

// Invalid controls must not create degenerate shells, unbounded allocations, or NaNs; serialized overrides survive.
TEST_CASE("Cloud controls sanitize and round trip", "[clouds]")
{
    auto options = weather();
    options.baseAltitude = 10000.0f; options.topAltitude = -1.0f;
    options.density = std::numeric_limits<float>::quiet_NaN();
    options.coverage = 3.0f; options.resolution = 999999; options.samples = 1;
    options.lightSamples = 99999; options.depthSlices = 99999;
    options.shadowCoverage = 42000.0f;
    options.farShadows = true; options.farShadowCoverage = 150000.0f;
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(options);
    const auto& sanitized = layer->getOptions();
    CHECK(sanitized.coverage == 1.0f);
    CHECK(sanitized.topAltitude == 10100.0f);
    CHECK(sanitized.density == 1.0f);
    CHECK(sanitized.resolution == 768u);
    CHECK(sanitized.samples == 16u);
    CHECK(sanitized.lightSamples == 16u);
    CHECK(sanitized.depthSlices == 64u);
    CloudLayer::Options copy(sanitized.getConfig());
    CHECK(copy.getConfig().toJSON() == sanitized.getConfig().toJSON());
    Config config("sky2"); config.add(copy.getConfig());
    osg::ref_ptr<SkyNode2> sky = new SkyNode2(SkyNode2::Options(config));
    REQUIRE(sky->getCloudLayer());
    CHECK(sky->getCloudLayer()->getOptions().resolution == 768u);
    CHECK(sky->getCloudLayer()->getOptions().shadowCoverage == 42000.0f);
    CHECK(sky->getCloudLayer()->getOptions().farShadows);
    CHECK(sky->getCloudLayer()->getOptions().farShadowCoverage == 150000.0f);
    for (float width : {-1.0f,100.0f,1e9f,std::numeric_limits<float>::quiet_NaN()})
    {
        options.farShadowCoverage = width; layer->setOptions(options);
        CHECK(layer->getOptions().farShadowCoverage >= 1.25f*layer->getOptions().shadowCoverage);
        CHECK(layer->getOptions().farShadowCoverage <= 1000000.0f);
    }
}

// Exercise the real shadow lookup at fixed ground positions to catch width/radius/unit errors and stale frame slots.
TEST_CASE("Ground cloud shadow coverage updates and fades at its configured boundary", "[clouds][cloudshadowcoverage][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    auto options = weather();
    CHECK(options.shadowCoverage == 20000.0f);
    CHECK_FALSE(options.farShadows);
    CHECK(options.farShadowCoverage == 100000.0f);
    options.coverage = 1.0f; options.density = 10.0f; options.erosion = 0.0f; options.shadowStrength = 1.0f;
    osg::ref_ptr<CloudLayer> clouds = new CloudLayer(options);
    sky->setCloudLayer(clouds);
    Scene scene(sky,32,32); scene.skyView(2.0);
    auto state = shadowProbe(scene);
    osg::ref_ptr<osg::Uniform> offset = new osg::Uniform("cloudTestGroundOffset",0.0f);
    state->addUniform(offset);
    ShaderLoader::load(VirtualProgram::getOrCreate(state),R"(
        #pragma vp_function cloudTestCoverage, fragment_output, 0.99
        uniform float cloudTestGroundOffset;
        uniform vec3 oe_cloud_eye;
        uniform vec4 oe_cloud_shell;
        uniform mat3 oe_cloud_basis;
        layout(location=0) out vec4 cloudTestCoverageResult;
        float oe_cloud_shadow(vec3 position);
        // Measure transmission on the curved ground at a fixed offset, bypassing atmosphere and tone mapping.
        void cloudTestCoverage(inout vec4 color)
        {
            vec3 p = normalize(oe_cloud_eye+oe_cloud_basis*vec3(cloudTestGroundOffset,0,0))*(oe_cloud_shell.x+0.002);
            cloudTestCoverageResult = color = vec4(vec3(oe_cloud_shadow(p)),1);
        }
    )");
    for (float width : {20000.0f,100000.0f,20000.0f})
    {
        options.shadowCoverage = width; clouds->setOptions(options);
        // Cycle both frame slots at each size; the receiver stays 12 km from the camera's ground position.
        offset->set(12.0f);
        for (unsigned frame=0; frame<2; ++frame)
        {
            scene.draw();
            float transmission = scene.pixels()[0];
            if (width == 20000.0f) CHECK(transmission == Approx(1.0f));
            else CHECK(transmission < 0.1f);
        }
    }
    offset->set(0.0f); scene.draw(); CHECK(scene.pixels()[0] < 0.1f);
    offset->set(9.0f); scene.draw();
    CHECK(scene.pixels()[0] > 0.4f);
    CHECK(scene.pixels()[0] < 0.6f);
    // A dense column must stay dark through the overlap, and strength must be applied once.
    for (bool far : {true,false,true})
    {
        options.farShadows = far;
        options.crepuscularRays = far; // Validate shared texture bindings with the optional ray passes active.
        clouds->setOptions(options);
        for (float distance : {0.0f,8.5f,9.5f,12.0f,35.0f,45.0f,55.0f})
        {
            offset->set(distance);
            for (unsigned frame=0; frame<2; ++frame)
            {
                scene.draw(); float transmission = scene.pixels()[0];
                INFO("far=" << far << " offset=" << distance << " frame=" << frame);
                if (far && distance <= 35.0f) CHECK(transmission < 0.1f);
                if (far && distance == 45.0f)
                {
                    CHECK(transmission > 0.4f); CHECK(transmission < 0.6f);
                }
                if (distance == 55.0f || (!far && distance >= 12.0f)) CHECK(transmission == Approx(1.0f));
            }
        }
    }
    options.shadowStrength = 0.5f; clouds->setOptions(options);
    offset->set(9.0f); scene.draw(); CHECK(std::abs(scene.pixels()[0]-0.5f) < 0.01f);
    options.farShadowCoverage = 50000.0f; clouds->setOptions(options);
    offset->set(30.0f); scene.draw(); CHECK(scene.pixels()[0] == Approx(1.0f));
    offset->set(15.0f); scene.draw(); CHECK(std::abs(scene.pixels()[0]-0.5f) < 0.01f);
    CHECK(glGetError() == GL_NO_ERROR);
}

// Verify the near field is preserved and the anchored far grid follows wind, independently of camera translation.
TEST_CASE("Cloud shadow cascades preserve near detail and stabilize distant receivers", "[clouds][cloudcascades][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    auto options = weather(); options.coverage = 0.6f; options.density = 4.0f;
    options.topAltitude = 2300.0f; options.seed = 3;
    options.size = 1500.0f; options.erosion = 0.4f; options.shadowStrength = 1.0f;
    osg::ref_ptr<CloudLayer> clouds = new CloudLayer(options); sky->setCloudLayer(clouds);
    Scene scene(sky,256,256); scene.skyView(1000.0);
    groundShadowProbe(scene,256,256,60.0f);
    drawAtTime(scene,0.0); auto nearOnly = scene.pixels();
    REQUIRE(scene.save("cloud-shadow-cascades-off.png"));
    options.farShadows = true; clouds->setOptions(options);
    drawAtTime(scene,0.0); auto both = scene.pixels();
    REQUIRE(scene.save("cloud-shadow-cascades-on.png"));
    double nearError = 0.0, farShadow = 0.0; unsigned nearCount = 0, farCount = 0;
    for (unsigned y=0; y<256; ++y)
    for (unsigned x=0; x<256; ++x)
    {
        float radius = std::max(std::abs((x+0.5f)/256.0f*120.0f-60.0f),
            std::abs((y+0.5f)/256.0f*120.0f-60.0f));
        unsigned i=(y*256+x)*4;
        if (radius < 7.0f) { nearError += std::abs(nearOnly[i]-both[i]); ++nearCount; }
        if (radius > 12.0f && radius < 35.0f) { farShadow += 1.0f-both[i]; ++farCount; }
    }
    CHECK(nearError/nearCount < 0.0001);
    CHECK(farShadow/farCount > 0.05);
    // Sub-texel and multi-texel camera translations must not resample a stationary world field.
    osg::Vec3d eye, center, up;
    scene.viewer->getCamera()->getViewMatrixAsLookAt(eye,center,up);
    for (double meters : {50.0,750.0,4000.0})
    {
        osg::Vec3d shift(0,meters,0);
        scene.viewer->getCamera()->setViewMatrixAsLookAt(eye+shift,center+shift,up);
        drawAtTime(scene,0.0); auto moved = scene.pixels();
        double error = 0.0; unsigned count = 0;
        for (unsigned y=0; y<256; ++y)
        for (unsigned x=0; x<256; ++x)
        {
            float radius = std::max(std::abs((x+0.5f)/256.0f*120.0f-60.0f),
                std::abs((y+0.5f)/256.0f*120.0f-60.0f));
            if (radius > 16.0f && radius < 35.0f)
            {
                unsigned i=(y*256+x)*4; error += std::abs(moved[i]-both[i]); ++count;
            }
        }
        INFO("camera shift=" << meters << " mean far transmission error=" << error/count);
        CHECK(error/count < 0.005);
    }
    scene.viewer->getCamera()->setViewMatrixAsLookAt(eye,center,up);
    options.wind.set(0,100,0); clouds->setOptions(options);
    drawAtTime(scene,10.0);
    CHECK(difference(both,scene.pixels()) > 0.01);
    CHECK(glGetError() == GL_NO_ERROR);
}

// Below-horizon pixels cannot contain clouds here: moving, bright shaft structure must come from scattering in air.
TEST_CASE("Cloud shafts illuminate air beneath the deck", "[clouds][cloudrays][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    auto options = weather(); options.quality = CloudLayer::BALANCED;
    options.coverage = 0.55f; options.seed = 2; options.shadowStrength = 0.0f;
    options.rayQuality = CloudLayer::HIGH; options.rayDistance = 30000.0f;
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(options);
    sky->setCloudLayer(layer);
    Scene scene(sky,1280,720);
    scene.skyView(1000.0);
    sky->setEphemeris(new Sun(12.0));
    osg::Vec3d eye(6379137.0,0,0);
    double pitch = osg::DegreesToRadians(-5.0), yaw = osg::DegreesToRadians(20.0);
    scene.viewer->getCamera()->setViewMatrixAsLookAt(eye,
        eye+osg::Vec3d(std::sin(pitch),std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw)),
        osg::Vec3d(1,0,0));
    scene.draw(); auto off = scene.pixels();
    REQUIRE(scene.save("cloud-shafts-off.png"));
    options.crepuscularRays = true; options.rayHaze = 0.0f; layer->setOptions(options);
    scene.draw(); auto shadowOnly = scene.pixels();
    options.rayHaze = 2.0f; layer->setOptions(options);
    scene.draw(); auto on = scene.pixels();
    REQUIRE(scene.save("cloud-shafts-on.png"));
    options.wind.set(0,100,0); layer->setOptions(options); drawAtTime(scene,60.0);
    auto moved = scene.pixels();
    double maximum = 0.0, structure = 0.0, motion = 0.0;
    unsigned count = 0;
    for (unsigned y=scene.height*2/5; y<scene.height/2; ++y)
    {
        double lo = 1.0, hi = -1.0;
        for (unsigned x=0; x<scene.width; ++x)
        {
            unsigned i = (y*scene.width+x)*4;
            double added = 0.0;
            for (unsigned c=0; c<3; ++c)
            {
                added += (on[i+c]-shadowOnly[i+c])/3.0;
                motion += std::abs(on[i+c]-moved[i+c])/3.0;
            }
            maximum = std::max(maximum,added);
            lo = std::min(lo,added); hi = std::max(hi,added);
            ++count;
        }
        structure = std::max(structure,hi-lo);
    }
    std::cout << "Air shafts added=" << maximum << " lateral contrast=" << structure << " motion=" << motion/count << '\n';
    CHECK(maximum > 0.03);
    CHECK(structure > 0.04);
    CHECK(motion/count > 0.002);
    CHECK(difference(off,on) > 0.003);
    CHECK(glGetError() == GL_NO_ERROR);
}

// Clearing weather must remove added ray haze continuously, including its contribution to material environment lighting.
TEST_CASE("Cloud ray haze clears with decreasing coverage", "[clouds][cloudrays][cloudraycoverage][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    auto options = weather(); options.quality = CloudLayer::BALANCED;
    options.baseAltitude = 1500.0f; options.topAltitude = 2300.0f; options.size = 1500.0f;
    options.density = 4.0f; options.erosion = 0.4f; options.seed = 3;
    options.crepuscularRays = true; options.rayIntensity = 8.0f; options.rayDistance = 30000.0f;
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(options);
    sky->setCloudLayer(layer);
    Scene scene(sky,640,360);
    sky->setEphemeris(new Sun(20.0));
    LogarithmicDepthBuffer depth; depth.install(scene.viewer->getCamera());
    for (bool ground : {false,true})
    {
        if (ground) { scene.models->setNodeMask(~0u); scene.groundView(); }
        else scene.skyView(350.0);
        for (auto quality : {CloudLayer::LOW,CloudLayer::HIGH})
        {
            options.rayQuality = quality;
            double previous = 1.0;
            std::vector<float> nearlyClear;
            for (float coverage : {0.6f,0.5f,0.4f,0.25f,0.2f,0.1f,0.02f,0.001f,0.0f})
            {
                INFO(ground); INFO(quality); INFO(coverage);
                options.coverage = coverage; options.rayHaze = 0.0f;
                layer->setOptions(options); scene.draw(); auto reference = scene.pixels();
                options.rayHaze = 0.5f;
                layer->setOptions(options); scene.draw(); auto actual = scene.pixels();
                double added = difference(reference,actual);
                std::cout << "Ray haze ground=" << ground << " quality=" << int(quality)
                    << " coverage=" << coverage << " difference=" << added << '\n';
                if (coverage >= 0.5f) CHECK(added > 0.005);
                if (coverage <= 0.25f)
                {
                    CHECK(added < 0.03);
                    CHECK(added <= previous+0.0001);
                    previous = added;
                }
                if (coverage <= 0.02f) CHECK(added < 0.002);
                if (coverage == 0.001f) nearlyClear = actual;
                if (coverage == 0.0f)
                {
                    CHECK(added < 0.0001);
                    CHECK(difference(nearlyClear,actual) < 0.001);
                }
                if (!ground && quality == CloudLayer::HIGH && (coverage == 0.6f || coverage == 0.02f))
                    REQUIRE(scene.save("cloud-coverage-"+std::to_string(int(coverage*100.0f))+".png"));
            }
            // Even the maximum haze setting must converge to clear air instead of popping at zero coverage.
            options.coverage = 0.001f; options.rayHaze = 4.0f;
            layer->setOptions(options); scene.draw(); auto maximumHaze = scene.pixels();
            options.crepuscularRays = false;
            layer->setOptions(options); scene.draw();
            CHECK(difference(maximumHaze,scene.pixels()) < 0.001);
            options.crepuscularRays = true;
        }
    }
    CHECK(glGetError() == GL_NO_ERROR);
}

// Optional rays must default off, survive configuration round trips, and keep invalid budgets finite.
TEST_CASE("Cloud ray controls are optional and bounded", "[clouds][cloudrays]")
{
    CloudLayer::Options defaults;
    CHECK_FALSE(defaults.crepuscularRays);
    Config config("clouds"); config.set("quality","low");
    CHECK(CloudLayer::Options(config).rayQuality == CloudLayer::BALANCED);
    auto options = defaults;
    options.crepuscularRays = true; options.rayQuality = CloudLayer::HIGH;
    options.rayStrength = 0.6f; options.rayDistance = 30000.0f; options.rayHaze = 2.0f; options.rayIntensity = 4.0f;
    CloudLayer::Options copy(options.getConfig());
    CHECK(copy.getConfig().toJSON() == options.getConfig().toJSON());
    options.rayStrength = std::numeric_limits<float>::quiet_NaN();
    options.rayHaze = -1.0f;
    options.rayIntensity = std::numeric_limits<float>::infinity();
    options.rayDistance = -1.0f; options.rayQuality = static_cast<CloudLayer::Quality>(99);
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(options);
    CHECK(layer->getOptions().rayStrength == 1.0f);
    CHECK(layer->getOptions().rayDistance == 1000.0f);
    CHECK(layer->getOptions().rayQuality == CloudLayer::BALANCED);
    CHECK(layer->getOptions().rayHaze == 0.0f);
    CHECK(layer->getOptions().rayIntensity == 3.0f);
}

// Actual atmospheric rays must remove shadowed direct light, preserve disabled output, and survive altitude/preset changes.
TEST_CASE("Optional cloud rays shadow air and disable without residue", "[clouds][cloudrays][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    auto options = weather(); options.coverage = 0.55f; options.quality = CloudLayer::BALANCED;
    options.rayHaze = 0.0f; // Isolate shadowing of existing air; additional haze can scatter more light.
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(options);
    sky->setCloudLayer(layer);
    Scene scene(sky,1280,640);
    sky->setEphemeris(new Sun(8.0));
    double largest = 0.0;
    for (double altitude : {2.0,1000.0,2500.0,6000.0,150000.0})
    {
        if (altitude < 5000.0) scene.skyView(altitude); else scene.planetView(altitude);
        options.crepuscularRays = false; layer->setOptions(options); scene.draw();
        auto clear = scene.pixels();
        if (altitude == 1000.0) REQUIRE(scene.save("cloud-rays-off.png"));
        options.crepuscularRays = true;
        for (auto quality : {CloudLayer::LOW,CloudLayer::BALANCED,CloudLayer::HIGH})
        {
            INFO(altitude); INFO(quality);
            options.rayQuality = quality; layer->setOptions(options); scene.draw();
            auto actual = scene.pixels();
            double error = difference(clear,actual);
            largest = std::max(largest,error);
            std::cout << "Cloud rays altitude=" << altitude << " quality=" << int(quality) << " change=" << error << '\n';
            if (altitude == 150000.0) CHECK(error < 0.0001);
            float added = 0.0f;
            for (unsigned i=0; i<actual.size(); ++i) added = std::max(added,actual[i]-clear[i]);
            CHECK(added < 0.005f); // Shadowing existing air cannot add scattering energy.
            if (quality == CloudLayer::BALANCED)
                REQUIRE(scene.save("cloud-rays-on-"+std::to_string(int(altitude))+".png"));
        }
        auto disabled = options; disabled.rayStrength = 0.0f;
        layer->setOptions(disabled); scene.draw();
        CHECK(difference(clear,scene.pixels()) < 0.0001);
        disabled = options; disabled.crepuscularRays = false;
        layer->setOptions(disabled); scene.draw();
        CHECK(difference(clear,scene.pixels()) < 0.0001);
    }
    CHECK(largest > 0.003);
    scene.skyView(1000.0); options.crepuscularRays = true;
    layer->setOptions(options); scene.draw();
    auto before = scene.pixels();
    sky->releaseGLObjects(scene.context->getState()); scene.draw();
    CHECK(difference(before,scene.pixels()) < 0.0001);
    CHECK(glGetError() == GL_NO_ERROR);
}

// Ray strength blends in linear light; wind, offscreen sun, independent cameras, and atmosphere bypass stay coherent.
TEST_CASE("Cloud rays follow lighting wind and independent views", "[clouds][cloudrays][.gl]")
{
    SkyNode2::Options lighting;
    lighting.outputSRGB = false; lighting.toneMapping = false; lighting.exposure = 0.25f;
    osg::ref_ptr<SkyNode2> sky = new SkyNode2(lighting);
    sky->setEnvironmentIntensity(0.0f);
    auto options = weather(); options.coverage = 0.55f; options.shadowStrength = 0.0f;
    options.rayQuality = CloudLayer::BALANCED;
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(options);
    sky->setCloudLayer(layer);
    Scene scene(sky,512,256);
    scene.skyView(1000.0); sky->setEphemeris(new Sun(12.0));
    scene.draw(); auto off = scene.pixels();
    options.crepuscularRays = true; layer->setOptions(options);
    scene.draw(); auto full = scene.pixels();
    CHECK(difference(off,full) > 0.001);
    options.rayStrength = 0.5f; layer->setOptions(options);
    scene.draw(); auto half = scene.pixels();
    float error = 0.0f;
    unsigned count = 0;
    for (unsigned i=0; i<half.size(); ++i)
    {
        // The 8-bit framebuffer clips HDR solar pixels; their pre-clamp linear values cannot be reconstructed.
        if (i%4 == 3 || off[i] >= 0.98f || full[i] >= 0.98f) continue;
        ++count;
        error = std::max(error,std::abs(half[i]-(off[i]+full[i])*0.5f));
    }
    REQUIRE(count > 10000);
    CHECK(error < 0.008f);
    options.rayStrength = 1.0f; layer->setOptions(options);
    scene.draw();
    {
        Scene second(sky,320,180);
        sky->setEphemeris(new Sun(12.0));
        second.skyView(2800.0); second.draw();
        scene.draw();
        CHECK(difference(full,scene.pixels()) < 0.0001);
    }
    scene.context->makeCurrent();
    options.wind.set(0,100,0);
    std::vector<float> corrections[2];
    for (unsigned time=0; time<2; ++time)
    {
        options.crepuscularRays = false; layer->setOptions(options); drawAtTime(scene,time*60.0);
        corrections[time] = scene.pixels();
        options.crepuscularRays = true; layer->setOptions(options); drawAtTime(scene,time*60.0);
        auto actual = scene.pixels();
        for (unsigned i=0; i<actual.size(); ++i) corrections[time][i] -= actual[i];
    }
    CHECK(difference(corrections[0],corrections[1]) > 0.001);
    options.wind.set(0,0,0); layer->setOptions(options);
    osg::ref_ptr<Sun> sun = new Sun(12.0);
    sun->direction.y() *= 0.5; sun->direction.z() = std::cos(osg::DegreesToRadians(12.0))*std::sqrt(0.75);
    sky->setEphemeris(sun);
    scene.draw(); auto offscreen = scene.pixels();
    options.crepuscularRays = false; layer->setOptions(options); scene.draw();
    CHECK(difference(offscreen,scene.pixels()) > 0.0001);
    for (double elevation : {0.5,-30.0})
    {
        sky->setEphemeris(new Sun(elevation)); scene.draw(); auto baseline = scene.pixels();
        options.crepuscularRays = true; layer->setOptions(options); scene.draw();
        auto actual = scene.pixels();
        CHECK(difference(actual,actual) == 0.0);
        if (elevation < 0.0) CHECK(difference(baseline,actual) < 0.0001);
        options.crepuscularRays = false; layer->setOptions(options);
    }
    sky->setEphemeris(new Sun(12.0)); sky->setAtmosphereVisible(false);
    scene.draw(); auto noAir = scene.pixels();
    options.crepuscularRays = true; layer->setOptions(options); scene.draw();
    CHECK(difference(noAir,scene.pixels()) < 0.0001);
    CHECK(glGetError() == GL_NO_ERROR);
}

// Finite-distance compositing must not paint distant shafts over nearby opaque terrain or models.
TEST_CASE("Cloud rays stop at foreground material distance", "[clouds][cloudrays][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    sky->setEnvironmentIntensity(0.0f);
    Scene scene(sky,384,192);
    sky->setEphemeris(new Sun(8.0));
    LogarithmicDepthBuffer depth; depth.install(scene.viewer->getCamera());
    auto options = weather(); options.coverage = 0.55f; options.shadowStrength = 0.0f;
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(options);
    sky->setCloudLayer(layer); scene.draw();
    auto clear = scene.pixels();
    std::vector<float> depths(scene.width*scene.height);
    glReadPixels(0,0,scene.width,scene.height,GL_DEPTH_COMPONENT,GL_FLOAT,depths.data());
    options.crepuscularRays = true; layer->setOptions(options); scene.draw();
    auto actual = scene.pixels();
    float maximum = 0.0f;
    unsigned count = 0;
    for (unsigned i=0; i<depths.size(); ++i)
    {
        if (depths[i] >= 0.99999f) continue;
        ++count;
        for (unsigned c=0; c<3; ++c) maximum = std::max(maximum,std::abs(actual[i*4+c]-clear[i*4+c]));
    }
    REQUIRE(count > 500);
    CHECK(maximum < 0.015f);
    CHECK(difference(actual,clear) > 0.001);
    CHECK(glGetError() == GL_NO_ERROR);
}

// The altitude transition is continuous, finite, serializable, and never fades cameras inside the cloud layer.
TEST_CASE("Cloud altitude visibility has smooth configurable endpoints", "[clouds][cloudfade]")
{
    osg::ref_ptr<CloudLayer> layer = new CloudLayer;
    CHECK(layer->getVisibility(20000.0) == 1.0f);
    CHECK(layer->getVisibility(60000.0) == Approx(0.5f));
    CHECK(layer->getVisibility(100000.0) == 0.0f);
    CHECK(layer->getVisibility(1e9) == 0.0f);
    CHECK(layer->getVisibility(std::numeric_limits<double>::quiet_NaN()) == 0.0f);
    CHECK(layer->getVisibility(20001.0) > 0.99999f);
    CHECK(layer->getVisibility(99999.0) < 0.00001f);
    auto options = weather();
    options.fadeStartAltitude = -1.0f; options.fadeEndAltitude = -1.0f;
    layer->setOptions(options);
    CHECK(layer->getOptions().fadeStartAltitude == options.topAltitude);
    CHECK(layer->getOptions().fadeEndAltitude == options.topAltitude+100.0f);
    options.fadeStartAltitude = 30000.0f; options.fadeEndAltitude = 80000.0f;
    layer->setOptions(options);
    CloudLayer::Options copy(layer->getOptions().getConfig());
    CHECK(copy.fadeStartAltitude == 30000.0f);
    CHECK(copy.fadeEndAltitude == 80000.0f);
    CHECK(layer->getVisibility(55000.0) == Approx(0.5f));
}

// Verifies the actual linear-radiance dissolve, first-use orbital bypass, re-entry, and simultaneous high/low views.
TEST_CASE("Clouds fade continuously when descending from orbit", "[clouds][cloudfade][.gl]")
{
    SkyNode2::Options lighting;
    lighting.outputSRGB = false; lighting.toneMapping = false; lighting.exposure = 0.25f;
    osg::ref_ptr<SkyNode2> sky = new SkyNode2(lighting);
    Scene scene(sky,512,256);
    scene.planetView(150000.0); scene.draw();
    auto orbitClear = scene.pixels();
    auto options = weather();
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(options);
    sky->setCloudLayer(layer); scene.draw();
    CHECK(difference(orbitClear,scene.pixels()) < 0.0001);
    double cloudDifference = 0.0;
    for (double altitude : {150000.0,80000.0,60000.0,40000.0,20000.0,6000.0})
    {
        INFO(altitude);
        scene.planetView(altitude);
        auto control = options; control.enabled = false;
        layer->setOptions(control); scene.draw();
        auto clear = scene.pixels();
        control = options; control.fadeStartAltitude = 200000.0f; control.fadeEndAltitude = 300000.0f;
        layer->setOptions(control); scene.draw();
        auto full = scene.pixels();
        cloudDifference = std::max(cloudDifference,difference(clear,full));
        layer->setOptions(options); scene.draw();
        auto actual = scene.pixels();
        float weight = layer->getVisibility(altitude*6360000.0/6378137.0);
        float maximum = 0.0f;
        for (unsigned i=0; i<actual.size(); ++i)
            maximum = std::max(maximum,std::abs(actual[i]-(clear[i]*(1.0f-weight)+full[i]*weight)));
        CHECK(maximum < 0.008f); // Quantization of three 8-bit framebuffer reads bounds the comparison accuracy.
        REQUIRE(scene.save("cloud-fade-"+std::to_string(int(altitude))+".png"));
    }
    CHECK(cloudDifference > 0.002);
    scene.planetView(150000.0);
    {
        Scene low(sky,256,128);
        low.skyView(2000.0); low.draw();
        scene.draw();
        CHECK(difference(orbitClear,scene.pixels()) < 0.0001);
    }
    scene.context->makeCurrent();
    CHECK(glGetError() == GL_NO_ERROR);
}

// Real compute/fragment integration must change the image, support all view regimes, and detach without residue.
TEST_CASE("Procedural clouds render from ground through orbit and disable cleanly", "[clouds][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    Scene scene(sky,512,256);
    scene.skyView();
    scene.draw();
    const auto clear = scene.pixels();
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(weather());
    sky->setCloudLayer(layer);
    REQUIRE(sky->getCloudLayer() == layer.get());
    scene.draw();
    auto cloudy = scene.pixels();
    REQUIRE(sky->getStateSet()->getDefinePair("OE_CLOUD_LAYER"));
    CHECK(difference(clear,cloudy) > 0.005);
    REQUIRE(scene.save("clouds-ground-low.png"));
    for (auto quality : {CloudLayer::LOW,CloudLayer::BALANCED,CloudLayer::HIGH})
    {
        auto options = weather(); options.quality = quality;
        layer->setOptions(options);
        for (double altitude : {2.0,2500.0,6000.0,150000.0})
        {
            if (altitude < 5000.0) scene.skyView(altitude); else scene.planetView(altitude);
            scene.draw();
            auto pixels = scene.pixels();
            CHECK(difference(pixels,pixels) == 0.0);
            REQUIRE(scene.save("clouds-"+std::to_string(int(quality))+"-"+std::to_string(int(altitude))+".png"));
        }
    }
    scene.skyView();
    auto options = weather(); options.coverage = 0.0f;
    layer->setOptions(options); scene.draw();
    CHECK(difference(clear,scene.pixels()) < 0.0001);
    options.coverage = 0.8f; options.enabled = false;
    layer->setOptions(options); scene.draw();
    CHECK(difference(clear,scene.pixels()) < 0.0001);
    options.enabled = true; options.density = 0.0f;
    layer->setOptions(options); scene.draw();
    CHECK(difference(clear,scene.pixels()) < 0.0001);
    sky->setCloudLayer(nullptr); scene.draw();
    CHECK(difference(clear,scene.pixels()) < 0.0001);
    CHECK(glGetError() == GL_NO_ERROR);
}

// Ordinary material fragments before the cloud entry retain their shading, including with logarithmic depth enabled.
TEST_CASE("Cloud transport preserves nearby material shading", "[clouds][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    sky->setEnvironmentIntensity(0.0f);
    Scene scene(sky,384,192);
    LogarithmicDepthBuffer depth; depth.install(scene.viewer->getCamera());
    scene.draw();
    auto clear = scene.pixels();
    std::vector<float> depths(scene.width*scene.height);
    glReadPixels(0,0,scene.width,scene.height,GL_DEPTH_COMPONENT,GL_FLOAT,depths.data());
    auto options = weather(); options.shadowStrength = 0.0f;
    sky->setCloudLayer(new CloudLayer(options));
    scene.draw();
    auto actual = scene.pixels();
    float maximum = 0.0f;
    unsigned count = 0;
    for (unsigned i=0; i<depths.size(); ++i)
    {
        if (depths[i] >= 0.99999f) continue;
        ++count;
        for (unsigned c=0; c<3; ++c) maximum = std::max(maximum,std::abs(actual[i*4+c]-clear[i*4+c]));
    }
    REQUIRE(count > 500);
    CHECK(maximum < 0.005f);
    CHECK(glGetError() == GL_NO_ERROR);
}

// Measures integration error against a denser march, then verifies independent views and projection changes.
TEST_CASE("Cloud quality and camera resources remain coherent", "[clouds][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    auto options = weather(); options.quality = CloudLayer::BALANCED; options.coverage = 0.65f;
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(options);
    sky->setCloudLayer(layer);
    Scene first(sky,512,256);
    first.skyView(100.0); first.draw();
    auto balanced = first.pixels();
    REQUIRE(first.save("clouds-balanced.png"));
    options.samples = 512; options.lightSamples = 12;
    layer->setOptions(options); first.draw();
    auto reference = first.pixels();
    double error = difference(balanced,reference);
    std::cout << "Cloud balanced/reference mean absolute display error=" << error << '\n';
    CHECK(error < 0.04);
    REQUIRE(first.save("clouds-reference.png"));
    {
        Scene second(sky,257,129);
        second.skyView(3500.0); second.draw();
        first.draw();
        CHECK(difference(reference,first.pixels()) < 0.0001);
        second.viewer->getCamera()->setProjectionMatrixAsPerspective(35.0,257.0/129.0,0.1,1e8);
        second.draw();
        CHECK(difference(second.pixels(),second.pixels()) == 0.0);
        second.viewer->getCamera()->setProjectionMatrixAsOrtho(-100,100,-50,50,0.1,1e8);
        second.draw();
        auto orthographic = second.pixels();
        auto disabled = options; disabled.enabled = false;
        layer->setOptions(disabled); second.draw();
        CHECK(difference(orthographic,second.pixels()) < 0.0001);
        layer->setOptions(options);
    }
    first.context->makeCurrent();
    sky->releaseGLObjects(first.context->getState());
    first.draw();
    CHECK(difference(reference,first.pixels()) < 0.0001);
}

// Nearby opaque geometry cannot inherit distant clouds; lighting changes must refresh cloud transport immediately.
TEST_CASE("Clouds respect foreground distance and lighting changes", "[clouds][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    auto options = weather(); options.shadowStrength = 0.0f;
    sky->setCloudLayer(new CloudLayer(options));
    Scene scene(sky,384,192);
    auto wall = scene.horizonWall();
    wall->getOrCreateStateSet()->getUniform("sky2TestConstant")->set(true);
    scene.wallView(wall,100.0,300.0);
    LogarithmicDepthBuffer logDepth; logDepth.install(scene.viewer->getCamera());
    scene.draw();
    auto pixels = scene.pixels();
    for (unsigned y=scene.height/3; y<2*scene.height/3; ++y)
    for (unsigned x=scene.width/3; x<2*scene.width/3; ++x)
    {
        unsigned i=(y*scene.width+x)*4;
        CHECK(pixels[i] == 1.0f); CHECK(pixels[i+1] == 0.0f); CHECK(pixels[i+2] == 1.0f);
    }
    scene.skyView(2500.0);
    scene.draw(); auto noon = scene.pixels();
    sky->setEphemeris(new Sun(3.0)); scene.draw();
    CHECK(difference(noon,scene.pixels()) > 0.01);
    REQUIRE(scene.save("clouds-sunset.png"));
    sky->setEphemeris(new Sun(-30.0)); sky->setAmbientIntensity(0.0f); scene.draw();
    REQUIRE(scene.save("clouds-night.png"));
    double energy = 0.0;
    pixels = scene.pixels();
    for (unsigned i=0; i<pixels.size(); i+=4) energy += pixels[i]+pixels[i+1]+pixels[i+2];
    CHECK(energy/(scene.width*scene.height*3.0) < 0.02);
    CHECK(glGetError() == GL_NO_ERROR);
}

// Loads the documented extension with real terrain, checking sampler allocation and finite-distance cloud compositing.
TEST_CASE("Cloud earth configuration renders with terrain", "[clouds][.gl]")
{
    char executable[] = "cloud-test", file[] = "clouds.earth";
    char* argv[] = {executable,file};
    int argc = 2;
    osg::ArgumentParser arguments(&argc,argv);
    osg::ref_ptr<osgViewer::Viewer> viewer = new osgViewer::Viewer;
    auto root = MapNodeHelper().load(arguments,viewer.get());
    REQUIRE(root.valid());
    auto sky = findTopMostNodeOfType<SkyNode2>(root.get());
    REQUIRE(sky != nullptr);
    REQUIRE(sky->getCloudLayer() != nullptr);
    REQUIRE(MapNode::get(root.get()) != nullptr);
    Scene scene(sky,512,256);
    LogarithmicDepthBuffer depth; depth.install(scene.viewer->getCamera());
    auto options = sky->getCloudLayer()->getOptions(); options.wind.set(0,0,0);
    sky->getCloudLayer()->setOptions(options);
    scene.skyView(500.0);
    for (unsigned i=0; i<12; ++i) scene.draw();
    auto actual = scene.pixels();
    REQUIRE(scene.save("clouds-terrain.png"));
    options.enabled = false; sky->getCloudLayer()->setOptions(options);
    scene.draw();
    CHECK(difference(actual,scene.pixels()) > 0.005);
    CHECK(glGetError() == GL_NO_ERROR);
}

// The cloud component remains usable without atmospheric LUTs when SkyNode2 selects its flat-lighting preset.
TEST_CASE("Clouds adapt to flat SkyNode2 lighting", "[clouds][.gl]")
{
    SkyNode2::Options flat; flat.preset = SkyNode2::FLAT;
    osg::ref_ptr<SkyNode2> sky = new SkyNode2(flat);
    Scene scene(sky,256,128);
    scene.skyView(); scene.draw();
    auto clear = scene.pixels();
    sky->setCloudLayer(new CloudLayer(weather()));
    scene.draw();
    CHECK(difference(clear,scene.pixels()) > 0.005);
    auto cloudy = scene.pixels();
    auto options = weather(); options.crepuscularRays = true;
    sky->getCloudLayer()->setOptions(options); scene.draw();
    CHECK(difference(cloudy,scene.pixels()) < 0.0001);
    CHECK(glGetError() == GL_NO_ERROR);
}

// Compares grazing cloud views with a denser march to expose altitude- and solar-dependent integration bands.
TEST_CASE("Cloud grazing rays converge across altitude and sun changes", "[clouds][cloudbands][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    auto options = weather(); options.quality = CloudLayer::BALANCED; options.coverage = 0.65f;
    // Hold the spatial lattice fixed to isolate ray integration from preset-dependent reconstruction error.
    options.resolution = 384; options.depthSlices = 24;
    osg::ref_ptr<CloudLayer> layer = new CloudLayer(options);
    sky->setCloudLayer(layer);
    Scene scene(sky,768,384);
    for (auto quality : {CloudLayer::LOW,CloudLayer::BALANCED,CloudLayer::HIGH})
    for (unsigned site=0; site<2; ++site)
    for (double altitude : {1000.0,1400.0,2000.0,6000.0})
    for (double sun : {3.0,35.0})
    {
        options.quality = quality;
        scene.skyView(altitude);
        osg::ref_ptr<Sun> lighting = new Sun(sun);
        if (site)
        {
            osg::Matrixd local;
            GeoPoint(SpatialReference::get("wgs84"),-80.0,30.0,altitude).createLocalToWorld(local);
            osg::Vec3d eye = local.getTrans();
            auto east = osg::Matrixd::transform3x3(osg::Vec3d(1,0,0),local);
            auto up = osg::Matrixd::transform3x3(osg::Vec3d(0,0,1),local);
            scene.viewer->getCamera()->setViewMatrixAsLookAt(eye,eye+east-up*0.05,up);
            double angle = osg::DegreesToRadians(sun);
            lighting->direction = up*std::sin(angle)+east*std::cos(angle);
        }
        sky->setEphemeris(lighting);
        layer->setOptions(options); scene.draw();
        auto actual = scene.pixels();
        std::string name = "cloud-bands-"+std::to_string(int(quality))+"-"+std::to_string(site)+"-"+
            std::to_string(int(altitude))+"-"+std::to_string(int(sun));
        if (quality == CloudLayer::BALANCED) REQUIRE(scene.save(name+"-actual.png"));
        auto reference = options; reference.samples = 512;
        layer->setOptions(reference); scene.draw();
        auto expected = scene.pixels();
        if (quality == CloudLayer::BALANCED) REQUIRE(scene.save(name+"-reference.png"));
        double bands = bandError(actual,expected,scene.width,scene.height);
        double error = difference(actual,expected);
        std::cout << name << " error=" << error << " bands=" << bands << '\n';
        INFO(name);
        CHECK(error < 0.001);
        CHECK(bands < (quality == CloudLayer::LOW ? 0.0004 : 0.00015));
    }
}
