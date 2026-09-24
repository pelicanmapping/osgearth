/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/CloudLayer>
#include <osgEarth/WindLayer>
#include "SkyNode2TestScene.h"
#include <iostream>

using namespace osgEarth;
using namespace osgEarth::Sky2Tests;

namespace
{
    //! Exposes cloud transmission after lighting so coverage comparisons are independent of sun and tone mapping.
    osg::Geometry* transmissionProbe(Scene& scene)
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
            #pragma vp_function cloudFilterClip, vertex_clip, 0.9
            in vec4 osg_Vertex;
            out vec2 cloudFilterUV;
            // Covers the viewport independently of the geographic test camera.
            void cloudFilterClip(inout vec4 vertex)
            {
                vertex = vec4(osg_Vertex.xy,0,1);
                cloudFilterUV = osg_Vertex.xy*0.5+0.5;
            }
            [break]
            #pragma vp_function cloudFilterOutput, fragment_output, 0.99
            in vec2 cloudFilterUV;
            layout(location=0) out vec4 cloudFilterResult;
            uniform mat3 oe_sky2_viewToEarth;
            vec3 oe_s2_viewRay(vec2 uv);
            vec4 oe_cloud_sample(vec3 direction, float distance);
            // Records opacity directly, including partial coverage within a filtered cloud sample.
            void cloudFilterOutput(inout vec4 color)
            {
                vec3 direction = normalize(oe_sky2_viewToEarth*oe_s2_viewRay(cloudFilterUV));
                color = vec4(vec3(1.0-oe_cloud_sample(direction,1e6).a),1);
                cloudFilterResult = color;
            }
        )");
        scene.sky->addChild(geometry);
        return geometry.get();
    }

    //! Moves a narrow horizon view through repeatable subpixel camera and wind offsets.
    void filteringView(Scene& scene, unsigned frame, double altitude)
    {
        scene.skyView(altitude);
        scene.viewer->getCamera()->setProjectionMatrixAsPerspective(12.0,2.0,0.1,1e8);
        osg::Vec3d eye(6378137.0+altitude,frame*0.75,0);
        scene.viewer->getCamera()->setViewMatrixAsLookAt(eye,
            eye+osg::Vec3d(0.015+frame*0.000015,1,frame*0.00002),osg::Vec3d(1,0,0));
        unsigned errors = Diagnostics::get().errors.load();
        scene.viewer->frame(frame*0.15);
        scene.context->makeCurrent();
        REQUIRE(Diagnostics::get().errors.load() == errors);
    }

    //! Averages independently traced reference pixels into the low-resolution ray footprint.
    std::vector<float> reduce(const std::vector<float>& source, unsigned width, unsigned height, unsigned factor)
    {
        std::vector<float> result(width*height,0.0f);
        for (unsigned y=0; y<height; ++y)
        for (unsigned x=0; x<width; ++x)
        for (unsigned dy=0; dy<factor; ++dy)
        for (unsigned dx=0; dx<factor; ++dx)
            result[y*width+x] += source[((y*factor+dy)*width*factor+x*factor+dx)*4]/float(factor*factor);
        return result;
    }
}

// The default and serialized opt-out must agree so applications can make reproducible quality comparisons.
TEST_CASE("Cloud detail filtering round trips", "[clouds][cloudfiltering]")
{
    CloudLayer::Options options;
    CHECK(options.detailFiltering);
    options.detailFiltering = false;
    CloudLayer::Options restored(options.getConfig());
    CHECK_FALSE(restored.detailFiltering);
}

// Dense spatial and distance supersampling is an independent reference for moving, sparsely covered horizon clouds.
TEST_CASE("Cloud detail filtering preserves distant coverage during motion", "[clouds][cloudfiltering][.gl]")
{
    CloudLayer::Options options;
    options.quality = CloudLayer::BALANCED;
    options.size = 1000.0f;
    options.erosion = 0.5f;
    options.shadowStrength = 0.0f;
    options.resolution = 128;
    options.depthSlices = 8;
    options.samples = 128;
    osg::ref_ptr<WindLayer> wind = new WindLayer;
    osg::ref_ptr<Wind> source = new Wind;
    source->setSpeed(Speed(100,Units::METERS_PER_SECOND));
    wind->addWind(source);
    REQUIRE(wind->open().isOK());
    osg::ref_ptr<SkyNode2> sky = new SkyNode2, referenceSky = new SkyNode2;
    osg::ref_ptr<CloudLayer> clouds = new CloudLayer(options), referenceClouds = new CloudLayer(options);
    clouds->setWindLayer(wind); referenceClouds->setWindLayer(wind);
    sky->setCloudLayer(clouds); referenceSky->setCloudLayer(referenceClouds);
    Scene scene(sky,128,64), reference(referenceSky,512,256);
    auto probe = transmissionProbe(scene);
    transmissionProbe(reference);
    for (float coverage : {0.3f,0.4f,0.5f})
    {
        options.coverage = coverage;
        auto dense = options;
        dense.resolution = 512; dense.samples = 512; dense.detailFiltering = false;
        referenceClouds->setOptions(dense);
        double errors[2] = {}, temporal[2] = {}, means[3] = {};
        std::vector<float> previous[3];
        for (unsigned frame=0; frame<16; ++frame)
        {
            filteringView(reference,frame,1000.0);
            auto expected = reduce(reference.pixels(),128,64,4);
            for (unsigned mode=0; mode<2; ++mode)
            {
                options.detailFiltering = mode != 0;
                clouds->setOptions(options);
                filteringView(scene,frame,1000.0);
                auto actual = reduce(scene.pixels(),128,64,1);
                bool finite = true;
                // Compare sky pixels; the bottom quarter includes the planet, which has no cloud intersection.
                for (unsigned i=128*16; i<actual.size(); ++i)
                {
                    finite = finite && std::isfinite(actual[i]);
                    errors[mode] += std::abs(actual[i]-expected[i]);
                    means[mode] += actual[i];
                    if (frame)
                        temporal[mode] += std::abs((actual[i]-previous[mode][i])-(expected[i]-previous[2][i]));
                    if (!mode) means[2] += expected[i];
                }
                REQUIRE(finite);
                previous[mode] = std::move(actual);
            }
            previous[2] = std::move(expected);
        }
        double count = 16.0*128.0*48.0;
        std::cout << "Cloud filtering coverage=" << coverage << " error=" << errors[0]/count << " -> "
            << errors[1]/count << " temporal=" << temporal[0]/count << " -> " << temporal[1]/count
            << " mean=" << means[0]/count << "," << means[1]/count << " reference=" << means[2]/count << '\n';
        INFO(coverage);
        CHECK(means[2]/count > 0.01);
        CHECK(errors[1] < errors[0]);
        CHECK(temporal[1] < temporal[0]);
        CHECK(std::abs(means[1]-means[2])/count < 0.015);
        // Capture the ordinary production composite at matching settings for visual inspection.
        probe->setNodeMask(0);
        for (unsigned mode=0; mode<2; ++mode)
        {
            options.detailFiltering = mode != 0; clouds->setOptions(options);
            filteringView(scene,0,1000.0);
            REQUIRE(scene.save("cloud-filter-"+std::to_string(int(coverage*100))+(mode ? "-on.png" : "-off.png")));
        }
        probe->setNodeMask(~0u);
    }
    probe->setNodeMask(0);
    Scene capture(sky,1280,720);
    options.coverage = 0.55f; options.resolution = 0; options.depthSlices = 0; options.samples = 0;
    for (unsigned mode=0; mode<2; ++mode)
    {
        options.detailFiltering = mode != 0; clouds->setOptions(options);
        capture.skyView(1000.0); capture.viewer->frame(0.0); capture.context->makeCurrent();
        REQUIRE(capture.save(mode ? "cloud-filter-horizon-on.png" : "cloud-filter-horizon-off.png"));
    }
    CHECK(glGetError() == GL_NO_ERROR);
}

// A resolved view inside the cloud layer must retain the point-sampled field exactly, including its erosion detail.
TEST_CASE("Cloud detail filtering retains resolved near density", "[clouds][cloudfiltering][.gl]")
{
    CloudLayer::Options options;
    options.resolution = 384; options.samples = 512; options.shadowStrength = 0.0f;
    osg::ref_ptr<CloudLayer> clouds = new CloudLayer(options);
    osg::ref_ptr<SkyNode2> sky = new SkyNode2;
    sky->setCloudLayer(clouds);
    Scene scene(sky,384,192);
    transmissionProbe(scene);
    scene.skyView(2500.0);
    osg::Vec3d eye(6380637.0,0,0);
    scene.viewer->getCamera()->setViewMatrixAsLookAt(eye,eye+osg::Vec3d(1,0,0),osg::Vec3d(0,0,1));
    std::vector<float> point;
    double maximum = 0.0;
    for (unsigned mode=0; mode<2; ++mode)
    {
        options.detailFiltering = mode != 0; clouds->setOptions(options);
        scene.viewer->frame(0.0); scene.context->makeCurrent();
        auto values = scene.pixels();
        if (!mode) point = values;
        else for (unsigned i=0; i<values.size(); ++i)
            maximum = std::max(maximum,double(std::abs(values[i]-point[i])));
    }
    CHECK(maximum < 0.00001);
    CHECK(glGetError() == GL_NO_ERROR);
}
