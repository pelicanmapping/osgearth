/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/Extension>
#include <osgEarth/ExampleResources>
#include <osgEarth/Lighting>
#include <osgEarth/LogarithmicDepthBuffer>
#include <osgEarth/MapNode>
#include <osgEarth/NodeUtils>
#include <osgEarth/SkyNode2Atmosphere.h>
#include "SkyNode2TestScene.h"
#include <limits>
#include <iostream>

using namespace osgEarth;
using namespace osgEarth::Sky2Tests;

namespace
{
    //! Checks that framebuffer output is finite, bounded and actually contains visible energy.
    double energy(const std::vector<float>& pixels)
    {
        double sum = 0.0;
        bool finite = true;
        float minimum = 1.0f, maximum = 0.0f;
        for (unsigned i=0; i<pixels.size(); ++i)
        {
            finite = finite && std::isfinite(pixels[i]);
            minimum = std::min(minimum,pixels[i]);
            maximum = std::max(maximum,pixels[i]);
            if (i%4 != 3) sum += pixels[i];
        }
        REQUIRE(finite);
        REQUIRE(minimum >= 0.0f);
        REQUIRE(maximum <= 1.001f);
        return sum/(pixels.size()*0.75);
    }

    //! Measures display-space RMS and maximum aerial error away from the immediate silhouette; optionally saves both.
    osg::Vec2d aerialError(Scene& scene, osg::StateSet* probe, const std::string& name = "")
    {
        probe->getUniform("sky2TestReference")->set(false);
        scene.draw();
        auto actual = scene.pixels();
        if (!name.empty()) REQUIRE(scene.save(name+".png"));
        probe->getUniform("sky2TestReference")->set(true);
        scene.draw();
        auto reference = scene.pixels();
        if (!name.empty()) REQUIRE(scene.save(name+"-reference.png"));
        double squared = 0.0, maximum = 0.0;
        unsigned count = 0;
        for (unsigned i=0; i<actual.size(); i+=4)
        {
            if (reference[i+3] < 0.99f) continue;
            for (unsigned c=0; c<3; ++c)
            {
                double delta = std::abs(actual[i+c]-reference[i+c]);
                squared += delta*delta;
                maximum = std::max(maximum,delta);
                ++count;
            }
        }
        REQUIRE(count > 0);
        return osg::Vec2d(std::sqrt(squared/count),maximum);
    }
}

// Sweeps the ground/space boundary and orbital slice distances against a 256-step per-pixel reference.
TEST_CASE("SkyNode2 aerial perspective remains accurate throughout descent", "[sky2][sky2zoom][.gl]")
{
    for (auto quality : {SkyNode2::BALANCED,SkyNode2::HIGH})
    {
        SkyNode2::Options options;
        options.preset = quality;
        Scene scene(new SkyNode2(options),256,256);
        auto probe = scene.aerialProbe();
        std::vector<double> altitudes = {2.0,1000.0,10000.0,80000.0,99000.0,100000.0,101000.0,250000.0};
        for (unsigned i=0; i<57; ++i) altitudes.push_back(1000000.0+i*250000.0);
        for (double altitude : altitudes)
        {
            scene.planetView(altitude);
            auto error = aerialError(scene,probe,altitude == 5000000.0 ? "sky2-zoom-"+std::to_string(int(quality)) : "");
            INFO("quality=" << int(quality) << " altitude=" << altitude << " RMS=" << error.x() << " max=" << error.y());
            std::cout << "Sky2 descent quality=" << int(quality) << " altitude=" << altitude <<
                " RMS=" << error.x() << " max=" << error.y() << '\n';
            CHECK(error.x() < 0.025);
            CHECK(error.y() < 0.1);
        }
    }
}

// Checks partial atmospheric paths above/below an airborne observer, including sunrise and twilight.
TEST_CASE("SkyNode2 aerial depth interpolation resolves airborne surfaces", "[sky2][sky2zoom][.gl]")
{
    for (auto quality : {SkyNode2::BALANCED,SkyNode2::HIGH})
    {
        SkyNode2::Options options;
        options.preset = quality;
        Scene scene(new SkyNode2(options),256,256);
        auto probe = scene.aerialProbe();
        for (double sun : {-6.0,5.0,35.0,85.0})
        for (double altitude : {2000.0,20000.0,150000.0,3000000.0})
        for (float height : {1.0f,12.0f,80.0f,120.0f})
        {
            scene.sky->setEphemeris(new Sun(sun));
            probe->getUniform("sky2TestSurfaceHeight")->set(height);
            if (height*1000.0 > altitude) scene.skyView(altitude);
            else scene.planetView(altitude);
            auto error = aerialError(scene,probe);
            INFO("quality=" << int(quality) << " sun=" << sun << " altitude=" << altitude << " height=" << height);
            std::cout << "Sky2 airborne quality=" << int(quality) << " sun=" << sun << " altitude=" << altitude <<
                " height=" << height << " RMS=" << error.x() << " max=" << error.y() << '\n';
            CHECK(error.x() < 0.025);
            CHECK(error.y() < 0.1);
        }
    }
}

// Checks cached row geometry against reference integration at oblique and polar globe orientations.
TEST_CASE("SkyNode2 aerial rows retain accuracy away from the equator", "[sky2][sky2rows][.gl]")
{
    for (auto quality : {SkyNode2::BALANCED,SkyNode2::HIGH})
    {
        SkyNode2::Options options;
        options.preset = quality;
        Scene scene(new SkyNode2(options),128,128);
        scene.models->setNodeMask(0);
        auto probe = scene.aerialProbe();
        for (double latitude : {-89.0,-45.0,40.7,89.0})
        for (double altitude : {100.0,150000.0,3000000.0})
        {
            double lat = osg::DegreesToRadians(latitude), lon = osg::DegreesToRadians(-74.0);
            osg::Vec3d up(std::cos(lat)*std::cos(lon),std::cos(lat)*std::sin(lon),std::sin(lat));
            osg::Vec3d east(-std::sin(lon),std::cos(lon),0.0);
            // Work in the atmosphere's ellipsoid-scaled frame, then return to ECEF meters.
            osg::Matrixd ecef = osg::Matrixd::scale(6378137.0,6378137.0,6356752.314245);
            osg::Vec3d eye = (up*(1.0+altitude/6378137.0))*ecef;
            osg::Vec3d target = altitude < 1000.0 ? eye+east*1000.0 : up*ecef;
            scene.viewer->getCamera()->setViewMatrixAsLookAt(eye,target,altitude < 1000.0 ? up : east);
            probe->getUniform("sky2TestSurfaceHeight")->set(altitude < 1000.0 ? 1.0f : 12.0f);
            auto error = aerialError(scene,probe);
            INFO("quality=" << int(quality) << " latitude=" << latitude << " altitude=" << altitude);
            CHECK(error.x() < 0.025);
            CHECK(error.y() < 0.1);
        }
    }
}

// Separates sky depth occlusion from finite-distance aerial interpolation across the horizon on an opaque facade.
TEST_CASE("SkyNode2 horizon stays behind nearby opaque geometry", "[sky2][sky2wall][.gl]")
{
    for (auto quality : {SkyNode2::BALANCED,SkyNode2::HIGH})
    {
        SkyNode2::Options options;
        options.preset = quality;
        Scene scene(new SkyNode2(options),512,256);
        auto wall = scene.horizonWall();
        auto state = wall->getOrCreateStateSet();
        for (bool logarithmic : {false,true})
        {
            LogarithmicDepthBuffer depth;
            if (logarithmic) depth.install(scene.viewer->getCamera());
            scene.wallView(wall,100.0,300.0);
            state->getUniform("sky2TestConstant")->set(true);
            scene.draw();
            auto pixels = scene.pixels();
            bool occluded = true;
            for (unsigned y=scene.height/4; y<scene.height*3/4; ++y)
            for (unsigned x=scene.width/4; x<scene.width*3/4; ++x)
            {
                unsigned i=(y*scene.width+x)*4;
                occluded = occluded && pixels[i] == 1.0f && pixels[i+1] == 0.0f && pixels[i+2] == 1.0f;
            }
            REQUIRE(occluded);
            state->getUniform("sky2TestConstant")->set(false);
            for (double sun : {5.0,35.0,85.0})
            for (double altitude : {2.0,100.0,1000.0})
            for (double distance : {50.0,300.0,1000.0,10000.0})
            {
                scene.sky->setEphemeris(new Sun(sun));
                scene.wallView(wall,altitude,distance);
                std::string name = sun == 5.0 && altitude == 100.0 && distance == 300.0 && !logarithmic ?
                    "sky2-wall-"+std::to_string(int(quality)) : "";
                auto error = aerialError(scene,state,name);
                INFO("quality=" << int(quality) << " logarithmic=" << logarithmic << " sun=" << sun <<
                    " altitude=" << altitude << " distance=" << distance);
                std::cout << "Sky2 wall quality=" << int(quality) << " log=" << logarithmic << " sun=" << sun <<
                    " altitude=" << altitude << " distance=" << distance <<
                    " RMS=" << error.x() << " max=" << error.y() << '\n';
                CHECK(error.x() < 0.015);
                CHECK(error.y() < 0.05);
            }
        }
    }
}

// Validates the accelerated production lookup against independently oversampled integration.
TEST_CASE("SkyNode2 transmittance conserves energy and agrees with reference rays", "[sky2]")
{
    auto image = Sky2Atmosphere::createTransmittance();
    for (double altitude : {0.01,1.0,10.0,50.0,99.0})
    for (double mu : {-0.5,-0.1,0.0,0.1,0.5,1.0})
    {
        auto reference = Sky2Atmosphere::integrate(Sky2Atmosphere::radius+altitude,mu,2048);
        auto actual = Sky2Atmosphere::sample(*image,Sky2Atmosphere::radius+altitude,mu);
        for (unsigned c=0; c<3; ++c)
        {
            INFO("altitude=" << altitude << " mu=" << mu << " channel=" << c);
            REQUIRE(actual[c] >= 0.0);
            REQUIRE(actual[c] <= 1.000001);
            REQUIRE(std::abs(actual[c]-reference[c]) < 0.015);
        }
    }
    REQUIRE(Sky2Atmosphere::sample(*image,Sky2Atmosphere::radius+0.01,-1.0).length2() == 0.0);
}

// Exercises serialization, direct core construction, factory dispatch and invalid option sanitization.
TEST_CASE("SkyNode2 core API round trips options and rejects invalid scalars", "[sky2]")
{
    SkyNode2::Options options;
    options.preset = SkyNode2::HIGH;
    options.exposure = 1.7f;
    options.outputSRGB = false;
    SkyNode2::Options roundTrip(options.getConfig());
    REQUIRE(roundTrip.preset == SkyNode2::HIGH);
    REQUIRE(roundTrip.exposure == Approx(1.7));
    REQUIRE_FALSE(roundTrip.outputSRGB);
    options.exposure = std::numeric_limits<float>::quiet_NaN();
    osg::ref_ptr<SkyNode2> sky = new SkyNode2(options);
    REQUIRE(sky->getOptions().exposure == 1.0f);
    sky->setExposure(-2.0f);
    REQUIRE(sky->getOptions().exposure == 1.0f);
    sky->setSunIntensity(-1.0f);
    REQUIRE(sky->getOptions().sunIntensity == 10.0f);
    sky->setAmbientIntensity(std::numeric_limits<float>::infinity());
    REQUIRE(sky->getOptions().ambient().get() == Approx(0.033));
    osg::ref_ptr<SkyNode> factory = SkyNode::create("sky2");
    REQUIRE(dynamic_cast<SkyNode2*>(factory.get()) != nullptr);
    osg::ref_ptr<Extension> extension = Extension::create("sky2",options);
    REQUIRE(extension.valid());
}

// Checks extension insertion, sun attachment, and ownership without retaining the map through its own extension.
TEST_CASE("SkyNode2 extension connects without a scene ownership cycle", "[sky2]")
{
    osg::ref_ptr<osg::Group> root = new osg::Group;
    osg::ref_ptr<MapNode> map = new MapNode;
    root->addChild(map);
    REQUIRE(map->open());
    osg::ref_ptr<Extension> extension = Extension::create("sky2",SkyNode2::Options());
    map->addExtension(extension);
    osg::observer_ptr<SkyNode2> sky = findTopMostNodeOfType<SkyNode2>(root.get());
    REQUIRE(sky.valid());
    osg::ref_ptr<osgViewer::View> view = new osgViewer::View;
    auto connection = ExtensionInterface<osg::View>::get(extension);
    REQUIRE(connection != nullptr);
    REQUIRE(connection->connect(view));
    REQUIRE(view->getLight() == sky->getSunLight());
    REQUIRE(connection->disconnect(view));
    REQUIRE(view->getLight() == nullptr);
    root = nullptr;
    REQUIRE_FALSE(sky.valid());
    osg::observer_ptr<MapNode> weakMap = map.get();
    map = nullptr;
    REQUIRE_FALSE(weakMap.valid());
}

// Loads the documented earth file, overrides its preset, and renders real terrain with logarithmic depth.
TEST_CASE("SkyNode2 earth files and command-line replacement use the core renderer", "[sky2][.gl]")
{
    char executable[] = "sky2-test", file[] = "sky2.earth", model[] = "--sky2", quality[] = "--sky-high";
    char* argv[] = {executable,file,model,quality};
    int argc = 4;
    osg::ArgumentParser arguments(&argc,argv);
    osg::ref_ptr<osgViewer::Viewer> viewer = new osgViewer::Viewer;
    auto root = MapNodeHelper().load(arguments,viewer.get());
    REQUIRE(root.valid());
    auto sky = findTopMostNodeOfType<SkyNode2>(root.get());
    REQUIRE(sky != nullptr);
    REQUIRE(sky->getOptions().preset == SkyNode2::HIGH);
    auto map = MapNode::get(root.get());
    REQUIRE(map != nullptr);
    unsigned count = 0;
    for (auto& extension : map->getExtensions())
        if (extension->as<SkyNodeFactory>()) ++count;
    REQUIRE(count == 1);
    REQUIRE(viewer->getLight() == sky->getSunLight());
    Scene scene(sky);
    LogarithmicDepthBuffer depth;
    depth.install(scene.viewer->getCamera());
    scene.skyView(400000.0);
    for (unsigned i=0; i<12; ++i) scene.draw();
    REQUIRE(energy(scene.pixels()) > 0.001);
    REQUIRE(scene.save("sky2-map.png"));
    for (double altitude : {3000000.0,10000000.0})
    {
        scene.planetView(altitude);
        for (unsigned i=0; i<12; ++i) scene.draw();
        REQUIRE(energy(scene.pixels()) > 0.001);
        REQUIRE(scene.save("sky2-map-descent-"+std::to_string(int(altitude))+".png"));
    }
}

// Renders the complete production pipeline, including pre-render LUTs, PBR materials and celestial background.
TEST_CASE("SkyNode2 renders finite ground and orbital views at every quality", "[sky2][.gl]")
{
    for (auto quality : {SkyNode2::FLAT,SkyNode2::BALANCED,SkyNode2::HIGH})
    {
        SkyNode2::Options options;
        options.preset = quality;
        Scene scene(new SkyNode2(options));
        scene.draw(); scene.draw(); scene.draw();
        REQUIRE(glGetError() == GL_NO_ERROR);
        auto first = scene.pixels();
        REQUIRE(scene.save("sky2-ground-"+std::to_string(int(quality))+".png"));
        REQUIRE(energy(first) > 0.05);
        scene.draw();
        REQUIRE(scene.pixels() == first);
        scene.skyView(2.0);
        scene.draw();
        REQUIRE(scene.save("sky2-horizon-"+std::to_string(int(quality))+".png"));
        REQUIRE(energy(scene.pixels()) > 0.0);
        scene.skyView(400000.0);
        scene.draw();
        REQUIRE(energy(scene.pixels()) >= 0.0);
        REQUIRE(scene.save("sky2-orbit-"+std::to_string(int(quality))+".png"));
        scene.sky->setEphemeris(new Sun(-15.0));
        scene.skyView();
        scene.draw();
        REQUIRE(energy(scene.pixels()) < energy(first));
        REQUIRE(scene.save("sky2-night-"+std::to_string(int(quality))+".png"));
        REQUIRE(glGetError() == GL_NO_ERROR);
    }
}

// Verifies shader specialization matches the former eight-slot shader, including sparse sun and point-light slots.
TEST_CASE("SkyNode2 specializes light capacity without changing PBR output", "[sky2][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    Scene scene(sky);
    osg::ref_ptr<LightGL3> point = new LightGL3(3);
    point->setPosition(osg::Vec4(6378150.0f,0,0,1));
    point->setDiffuse(osg::Vec4(4,2,1,1));
    osg::ref_ptr<osg::LightSource> source = new osg::LightSource;
    source->setLight(point);
    source->setCullingActive(false);
    source->addCullCallback(new LightSourceGL3UniformGenerator);
    sky->addChild(source);
    auto ss = sky->getOrCreateStateSet();
    for (int sunIndex : {0,7})
    for (bool localLight : {false,true})
    {
        CAPTURE(sunIndex,localLight);
        sky->attach(scene.viewer,sunIndex);
        source->setNodeMask(localLight ? ~0u : 0u);
        ss->setDefine("OE_NUM_LIGHTS","1");
        scene.draw();
        auto actual = scene.pixels();
        REQUIRE(energy(actual) > 0.01);
        ss->setDefine("OE_NUM_LIGHTS","8",
            osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE|osg::StateAttribute::PROTECTED);
        scene.draw();
        auto reference = scene.pixels();
        double maximum = 0.0;
        for (unsigned i=0; i<actual.size(); ++i)
            maximum = std::max(maximum,double(std::abs(actual[i]-reference[i])));
        REQUIRE(maximum <= 1.0/255.0+1e-6);
    }
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Exercises sparse OSG indices, distance attenuation, cone cutoff, disabled lights and the shadow interface.
TEST_CASE("SkyNode2 respects OSG point and spot lights and solar shadows", "[sky2][.gl]")
{
    SkyNode2::Options options;
    options.preset = SkyNode2::FLAT;
    options.ambient() = 0.0f;
    options.environmentIntensity = 0.0f;
    osg::ref_ptr<SkyNode2> sky = new SkyNode2(options);
    Scene scene(sky);
    sky->setSunVisible(false); sky->setMoonVisible(false); sky->setStarsVisible(false);
    scene.draw();
    double sunlight = energy(scene.pixels());
    REQUIRE(sunlight > 0.05);
    auto ss = sky->getOrCreateStateSet();
    ss->setDefine("OE_SHADOWING");
    ss->addUniform(new osg::Uniform("testShadow",0.0f));
    ShaderLoader::load(VirtualProgram::getOrCreate(ss),R"(
        #pragma vp_function testShadowInput, fragment_lighting, 0.7
        float oe_shadow_visibility;
        uniform float testShadow;
        // Supplies a controlled visibility value through the production shadow contract.
        void testShadowInput(inout vec4 color) { oe_shadow_visibility=testShadow; }
    )");
    scene.draw();
    REQUIRE(energy(scene.pixels()) < 0.001);
    osg::ref_ptr<LightGL3> light = new LightGL3(7);
    light->setPosition(osg::Vec4(6378150.0f,0,0,1));
    light->setDiffuse(osg::Vec4(10,10,10,1));
    light->setConstantAttenuation(1.0f);
    osg::ref_ptr<osg::LightSource> source = new osg::LightSource;
    source->setLight(light);
    source->setCullingActive(false);
    source->addCullCallback(new LightSourceGL3UniformGenerator);
    sky->addChild(source);
    scene.draw();
    double point = energy(scene.pixels());
    REQUIRE(point > 0.05);
    light->setLinearAttenuation(1.0f);
    scene.draw();
    REQUIRE(energy(scene.pixels()) < point*0.7);
    light->setLinearAttenuation(0.0f);
    light->setSpotCutoff(20.0f);
    light->setDirection(osg::Vec3(1,0,0)); // outward, away from the ground
    scene.draw();
    REQUIRE(energy(scene.pixels()) < 0.001);
    light->setSpotCutoff(180.0f);
    light->setEnabled(false);
    scene.draw();
    REQUIRE(energy(scene.pixels()) < 0.001);
    ss->getUniform("testShadow")->set(1.0f);
    scene.draw();
    REQUIRE(energy(scene.pixels()) == Approx(sunlight).epsilon(0.001));
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Alternates independent contexts/views and validates recovery after releasing per-context LUT objects.
TEST_CASE("SkyNode2 isolates camera state and recreates released GPU resources", "[sky2][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    Scene ground(sky,256,128), orbit(sky,256,128);
    ground.skyView(); orbit.skyView(400000.0);
    ground.draw();
    auto expected = ground.pixels();
    orbit.draw();
    REQUIRE(energy(orbit.pixels()) < energy(expected)*0.5);
    ground.draw();
    REQUIRE(ground.pixels() == expected);
    sky->releaseGLObjects(ground.context->getState());
    ground.context->getState()->dirtyAllAttributes();
    ground.context->getState()->dirtyAllVertexArrays();
    ground.draw();
    REQUIRE(ground.pixels() == expected);
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Checks the projected-map tangent transform against the same physical observer in ECEF.
TEST_CASE("SkyNode2 projected reference preserves the local horizon", "[sky2][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    Scene scene(sky,256,128);
    scene.skyView();
    scene.draw();
    auto expected = scene.pixels();
    sky->setReferencePoint(GeoPoint(SpatialReference::get("wgs84"),0,0,0,ALTMODE_ABSOLUTE));
    scene.viewer->getCamera()->setViewMatrixAsLookAt(osg::Vec3d(0,0,2),osg::Vec3d(1,0,1.95),osg::Vec3d(0,0,1));
    scene.draw();
    auto actual = scene.pixels();
    double maximum = 0.0;
    for (unsigned i=0; i<actual.size(); ++i) maximum = std::max(maximum,double(std::abs(actual[i]-expected[i])));
    REQUIRE(maximum < 0.01);
}

// Looks directly at each disk, proving visibility controls and lunar phase affect the actual framebuffer.
TEST_CASE("SkyNode2 solar and lunar disks respond to visibility and phase", "[sky2][.gl]")
{
    SkyNode2::Options options;
    options.preset = SkyNode2::FLAT;
    osg::ref_ptr<SkyNode2> sky = new SkyNode2(options);
    Scene scene(sky,512,256);
    scene.models->setNodeMask(0);
    sky->setStarsVisible(false); sky->setMoonVisible(false);
    osg::Vec3d eye(6378139,0,0);
    osg::ref_ptr<Sun> ephemeris = new Sun(35.0);
    scene.viewer->getCamera()->setViewMatrixAsLookAt(eye,eye+ephemeris->direction,osg::Vec3d(1,0,0));
    scene.draw();
    REQUIRE(energy(scene.pixels()) > 0.0001);
    sky->setSunIntensity(0.0f);
    scene.draw();
    REQUIRE(energy(scene.pixels()) == 0.0);
    sky->setSunIntensity(10.0f);
    sky->setSunVisible(false);
    scene.draw();
    REQUIRE(energy(scene.pixels()) == 0.0);
    sky->setMoonVisible(true);
    auto moon = ephemeris->getMoonPosition(DateTime()).geocentric;
    scene.viewer->getCamera()->setViewMatrixAsLookAt(eye,moon,osg::Vec3d(1,0,0));
    scene.draw();
    double crescent = energy(scene.pixels());
    sky->setEphemeris(new Sun(-155.0));
    scene.draw();
    REQUIRE(energy(scene.pixels()) > crescent*2.0);
}

// Verifies inherited visibility switches really remove all sky content while preserving alpha and geometry.
TEST_CASE("SkyNode2 visibility and exposure update without rebuilding the node", "[sky2][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    Scene scene(sky);
    scene.skyView();
    scene.draw();
    REQUIRE(energy(scene.pixels()) > 0.01);
    sky->setAtmosphereVisible(false);
    sky->setSunVisible(false);
    sky->setMoonVisible(false);
    sky->setStarsVisible(false);
    scene.draw();
    REQUIRE(energy(scene.pixels()) == 0.0);
    sky->setAtmosphereVisible(true);
    sky->setExposure(0.0f);
    scene.draw();
    REQUIRE(energy(scene.pixels()) == 0.0);
    sky->setExposure(1.0f);
    scene.draw();
    REQUIRE(energy(scene.pixels()) > 0.01);
    sky->setSunIntensity(0.0f);
    scene.draw();
    REQUIRE(energy(scene.pixels()) == 0.0);
    sky->setSunIntensity(10.0f);
    scene.draw();
    REQUIRE(energy(scene.pixels()) > 0.01);
}

// Confirms runtime solar, environment and night-fill controls reach the production PBR shader.
TEST_CASE("SkyNode2 lighting controls independently change surface illumination", "[sky2][.gl]")
{
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    Scene scene(sky);
    sky->setAtmosphereVisible(false);
    sky->setSunVisible(false);
    sky->setMoonVisible(false);
    sky->setStarsVisible(false);
    sky->setSunIntensity(0.0f);
    sky->setAmbientIntensity(0.0f);
    sky->setEnvironmentIntensity(0.0f);
    scene.draw();
    REQUIRE(energy(scene.pixels()) == 0.0);
    sky->setAmbientIntensity(0.1f);
    scene.draw();
    REQUIRE(energy(scene.pixels()) > 0.001);
    sky->setAmbientIntensity(0.0f);
    sky->setSunIntensity(10.0f);
    scene.draw();
    double direct = energy(scene.pixels());
    REQUIRE(direct > 0.01);
    sky->setEnvironmentIntensity(4.0f);
    scene.draw();
    REQUIRE(energy(scene.pixels()) > direct+0.01);
}
