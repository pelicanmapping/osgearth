/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once

#include <osgEarth/SkyNode2>
#include <osgEarth/Capabilities>
#include <osgEarth/GLUtils>
#include <osgEarth/Chonk>
#include <osgEarth/VirtualProgram>
#include <osgEarth/ShaderLoader>
#include <osg/GraphicsContext>
#include <osg/ShapeDrawable>
#include <osg/MatrixTransform>
#include <osg/Notify>
#include <osg/Geometry>
#include <osg/Depth>
#include <osgViewer/Viewer>
#include <osgDB/WriteFile>
#include <stdexcept>
#include <atomic>

namespace osgEarth { namespace Sky2Tests
{
    struct Diagnostics : osg::NotifyHandler
    {
        osg::ref_ptr<osg::NotifyHandler> previous;
        std::atomic<unsigned> errors{0};
        //! Preserves normal logging while counting shader and GL failures that OSG otherwise consumes.
        Diagnostics() : previous(osg::getNotifyHandler()) { }
        //! Records rendering errors, forwarding every original diagnostic to the previous handler.
        void notify(osg::NotifySeverity severity, const char* message) override
        {
            std::string text(message);
            if (text.find("FAILED") != std::string::npos || text.find("OpenGL error") != std::string::npos ||
                text.find("Program will not link") != std::string::npos)
                ++errors;
            if (previous) previous->notify(severity,message);
        }
        //! Installs one shared test observer so nested, independent views use the same error stream.
        static Diagnostics& get()
        {
            static osg::ref_ptr<Diagnostics> instance = []()
            {
                osg::ref_ptr<Diagnostics> result = new Diagnostics;
                osg::setNotifyHandler(result);
                return result;
            }();
            return *instance;
        }
    };

    struct Sun : Ephemeris
    {
        osg::Vec3d direction;
        //! Creates deterministic lighting at the equatorial test site; angle is solar elevation in degrees.
        explicit Sun(double degrees)
        {
            double angle = osg::DegreesToRadians(degrees);
            direction.set(std::sin(angle),std::cos(angle),0.0);
        }
        //! Returns a fixed, distant sun in both supported celestial frames.
        CelestialBody getSunPosition(const DateTime&) const override
        {
            CelestialBody result;
            result.eci = result.geocentric = direction*149597870700.0;
            return result;
        }
        //! Positions a visible moon independently of the test sun, retaining a meaningful lunar phase.
        CelestialBody getMoonPosition(const DateTime&) const override
        {
            CelestialBody result;
            result.eci = result.geocentric = osg::Vec3d(0.4,0.916515,0.0)*384400000.0;
            return result;
        }
    };

    struct Readback : osg::Camera::DrawCallback
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        unsigned width, height;
        bool enabled = true;
        //! Captures the actual final framebuffer only when image validation is enabled.
        Readback(unsigned w, unsigned h) : width(w), height(h) { }
        //! Runs on the draw thread after all scene passes; a float readback exposes NaNs and clipping.
        void operator()(osg::RenderInfo&) const override
        {
            if (enabled) image->readPixels(0,0,width,height,GL_RGBA,GL_FLOAT);
        }
    };

    struct Scene
    {
        osg::ref_ptr<osgViewer::Viewer> viewer = new osgViewer::Viewer;
        osg::ref_ptr<osg::GraphicsContext> context;
        osg::ref_ptr<SkyNode> sky;
        osg::ref_ptr<Readback> readback;
        osg::ref_ptr<osg::MatrixTransform> models = new osg::MatrixTransform;
        unsigned width, height;

        //! Creates an OSG pipeline and material probes; coreProfile uses a window because WGL pbuffers are legacy.
        Scene(SkyNode* node, unsigned w = 512, unsigned h = 256, bool coreProfile = false) :
            sky(node), width(w), height(h)
        {
            Capabilities::get();
            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            traits->readDISPLAY();
            traits->setUndefinedScreenDetailsToDefaultScreen();
            traits->width = w; traits->height = h;
            traits->alpha = 8; // Retain probe coverage masks in framebuffer readbacks.
            traits->pbuffer = !coreProfile; traits->doubleBuffer = false;
            if (coreProfile)
            {
                traits->glContextVersion = "4.3";
                traits->glContextProfileMask = 0x00000001; // GL_CONTEXT_CORE_PROFILE_BIT
                traits->windowDecoration = false;
                traits->x = -32000; traits->y = -32000;
            }
            context = osg::GraphicsContext::createGraphicsContext(traits);
            if (!context || !context->realize() || !context->makeCurrent())
                throw std::runtime_error("SkyNode2 offscreen GL context unavailable");
            context->getState()->setUseModelViewAndProjectionUniforms(true);
            context->getState()->setUseVertexAttributeAliasing(true);
            osg::ref_ptr<CustomRealizeOperation> realize = new CustomRealizeOperation;
            (*realize)(context.get()); // Honors OSGEARTH_GL_DEBUG for driver diagnostics.
            viewer->setThreadingModel(osgViewer::Viewer::SingleThreaded);
            auto camera = viewer->getCamera();
            camera->setGraphicsContext(context);
            camera->setViewport(0,0,w,h);
            camera->setDrawBuffer(GL_FRONT); camera->setReadBuffer(GL_FRONT);
            camera->setProjectionMatrixAsPerspective(55.0,double(w)/h,0.1,1e8);
            camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
            readback = new Readback(w,h);
            camera->setFinalDrawCallback(readback);
            sky->attach(viewer);
            sky->setDateTime(DateTime(2026,3,20,12.0));
            sky->setEphemeris(new Sun(35.0));
            GLUtils::setGlobalDefaults(sky->getOrCreateStateSet());
            models->setMatrix(osg::Matrixd(0,1,0,0,0,0,1,0,1,0,0,0,6378137.0,0,0,1));
            sky->addChild(models);
            for (unsigned i=0; i<5; ++i)
            {
                osg::ref_ptr<osg::ShapeDrawable> ball = new osg::ShapeDrawable(new osg::Sphere(osg::Vec3(15,i*5.0-10,3),2));
                ball->setColor(i < 3 ? osg::Vec4(0.65,0.18,0.06,1) : osg::Vec4(0.8,0.8,0.8,1));
                ball->setUseDisplayList(false); ball->setUseVertexBufferObjects(true);
                auto ss = ball->getOrCreateStateSet();
                ss->addUniform(new osg::Uniform("testRoughness",0.1f+0.2f*i));
                ss->addUniform(new osg::Uniform("testMetal",i < 3 ? 0.0f : 1.0f));
                ShaderLoader::load(VirtualProgram::getOrCreate(ss),R"(
                    #pragma vp_function testMaterial, fragment_coloring, 0.9
                    struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;
                    uniform float testRoughness, testMetal;
                    // Supplies independently varied test material factors before lighting.
                    void testMaterial(inout vec4 color) { oe_pbr.roughness=testRoughness; oe_pbr.metal=testMetal; }
                )");
                models->addChild(ball);
            }
            osg::ref_ptr<osg::ShapeDrawable> floor = new osg::ShapeDrawable(new osg::Box(osg::Vec3(30,0,-1),100,100,2));
            floor->setColor(osg::Vec4(0.25,0.3,0.2,1));
            floor->setUseDisplayList(false); floor->setUseVertexBufferObjects(true);
            models->addChild(floor);
            viewer->setSceneData(sky);
            groundView();
            viewer->realize();
        }

        //! Selects a ground-level material test view with sky above the horizon.
        void groundView(double offset = 0.0)
        {
            viewer->getCamera()->setViewMatrixAsLookAt(osg::Vec3d(6378143,offset-20,-22),
                osg::Vec3d(6378141,18,0),osg::Vec3d(1,0,0));
        }

        //! Selects an unobstructed sky view at a given altitude, retaining the ground/space limb.
        void skyView(double altitude = 2.0)
        {
            models->setNodeMask(0);
            osg::Vec3d eye(6378137.0+altitude,0,0);
            viewer->getCamera()->setViewMatrixAsLookAt(eye,eye+osg::Vec3d(-0.05,1,0),osg::Vec3d(1,0,0));
        }

        //! Aims at the equatorial surface from a continuously variable altitude in meters.
        void planetView(double altitude)
        {
            models->setNodeMask(0);
            viewer->getCamera()->setViewMatrixAsLookAt(osg::Vec3d(6378137.0+altitude,0,0),
                osg::Vec3d(6378137.0,0,0),osg::Vec3d(0,0,1));
        }

        //! Adds an analytic ellipsoid probe, isolating production aerial lookup error from mesh/terrain LOD.
        osg::StateSet* aerialProbe()
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
            auto ss = geometry->getOrCreateStateSet();
            ss->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS));
            ss->addUniform(new osg::Uniform("sky2TestReference",false));
            ss->addUniform(new osg::Uniform("sky2TestSurfaceHeight",0.0f));
            ShaderLoader::load(VirtualProgram::getOrCreate(ss),R"(
                #pragma vp_function sky2TestClip, vertex_clip, 0.9
                in vec4 osg_Vertex;
                out vec2 sky2TestUV;
                // Covers the viewport regardless of camera distance or terrain tessellation.
                void sky2TestClip(inout vec4 vertex)
                {
                    vertex=vec4(osg_Vertex.xy,0,1);
                    sky2TestUV=osg_Vertex.xy*0.5+0.5;
                }
                [break]
                #pragma vp_function sky2TestAerial, fragment_lighting, 0.9
                in vec2 sky2TestUV;
                uniform bool sky2TestReference;
                uniform float sky2TestSurfaceHeight;
                uniform vec3 oe_sky2_eye;
                uniform mat3 oe_sky2_viewToEarth;
                vec3 oe_s2_viewRay(vec2 uv);
                vec2 oe_s2_sphere(vec3 p, vec3 d, float radius);
                vec3 oe_s2_integrate(vec3 p, vec3 d, float limit, int count, out vec3 transmission);
                void oe_s2_aerial(vec3 direction, float distance, out vec3 scattering, out vec3 transmission);
                vec3 oe_s2_output(vec3 radiance);
                // Compares the production lookup with an oversampled ray using identical physical coefficients.
                void sky2TestAerial(inout vec4 color)
                {
                    vec3 direction=normalize(oe_sky2_viewToEarth*oe_s2_viewRay(sky2TestUV));
                    vec2 hit=oe_s2_sphere(oe_sky2_eye,direction,6360.0+sky2TestSurfaceHeight);
                    float distance=hit.x>0.0 ? hit.x : hit.y;
                    vec2 ground=oe_s2_sphere(oe_sky2_eye,direction,6360.0);
                    if (distance<=0.0 || (ground.x>0.0 && ground.x<distance-0.001))
                        { color=vec4(0); return; }
                    vec3 S,T;
                    if (sky2TestReference) S=oe_s2_integrate(oe_sky2_eye,direction,distance,256,T);
                    else oe_s2_aerial(direction,distance,S,T);
                    float mu=abs(dot(normalize(oe_sky2_eye+direction*distance),direction));
                    color=vec4(oe_s2_output(S+T*vec3(0.02)),mu>0.3 ? 1.0 : 0.0);
                }
            )");
            sky->addChild(geometry);
            return ss;
        }

        //! Adds an opaque facade spanning the horizon, with ordinary geometry depth and an optional reference atmosphere.
        osg::MatrixTransform* horizonWall()
        {
            models->removeChildren(0,models->getNumChildren());
            osg::ref_ptr<osg::MatrixTransform> wall = new osg::MatrixTransform;
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            vertices->push_back(osg::Vec3(1,-1,-0.5));
            vertices->push_back(osg::Vec3(1,1,-0.5));
            vertices->push_back(osg::Vec3(1,1,0.5));
            vertices->push_back(osg::Vec3(1,-1,0.5));
            geometry->setVertexArray(vertices);
            geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_FAN,0,4));
            geometry->setUseDisplayList(false);
            geometry->setUseVertexBufferObjects(true);
            auto ss = wall->getOrCreateStateSet();
            ss->setMode(GL_CULL_FACE,osg::StateAttribute::OFF);
            ss->addUniform(new osg::Uniform("sky2TestReference",false));
            ss->addUniform(new osg::Uniform("sky2TestConstant",false));
            ShaderLoader::load(VirtualProgram::getOrCreate(ss),R"(
                #pragma vp_function sky2TestWall, fragment_lighting, 0.9
                in vec3 vp_VertexView;
                uniform bool sky2TestReference, sky2TestConstant;
                uniform vec3 oe_sky2_eye;
                uniform mat3 oe_sky2_viewToEarth;
                vec3 oe_s2_integrate(vec3 p, vec3 d, float limit, int count, out vec3 transmission);
                void oe_s2_aerial(vec3 direction, float distance, out vec3 scattering, out vec3 transmission);
                vec3 oe_s2_output(vec3 radiance);
                // Isolates finite-distance haze; solid magenta separately detects background/depth overwrites.
                void sky2TestWall(inout vec4 color)
                {
                    if (sky2TestConstant) { color=vec4(1,0,1,1); return; }
                    vec3 ray=oe_sky2_viewToEarth*vp_VertexView;
                    float distance=length(ray);
                    vec3 direction=ray/distance;
                    vec3 S,T;
                    if (sky2TestReference) S=oe_s2_integrate(oe_sky2_eye,direction,distance,256,T);
                    else oe_s2_aerial(direction,distance,S,T);
                    color=vec4(oe_s2_output(S+T*vec3(0.02)),length(oe_sky2_eye+ray)>6360.0 ? 1.0 : 0.0);
                }
            )");
            wall->addChild(geometry);
            models->addChild(wall);
            return wall;
        }

        //! Positions the facade and observer in meters, looking east through the geometric horizon.
        void wallView(osg::MatrixTransform* wall, double altitude, double distance)
        {
            models->setNodeMask(~0u);
            wall->setMatrix(osg::Matrixd::scale(distance,distance,distance)*osg::Matrixd::translate(0,0,altitude));
            osg::Vec3d eye(6378137.0+altitude,0,0);
            viewer->getCamera()->setViewMatrixAsLookAt(eye,eye+osg::Vec3d(0,1,0),osg::Vec3d(1,0,0));
        }

        //! Renders a complete update/cull/draw frame and keeps the GL context current for assertions.
        void draw()
        {
            unsigned errors = Diagnostics::get().errors.load();
            viewer->frame();
            context->makeCurrent();
            if (Diagnostics::get().errors.load() != errors)
                throw std::runtime_error("Shader compilation, link, or OpenGL failure during SkyNode2 frame");
        }

        //! Returns a copy, since the next draw overwrites the readback image.
        std::vector<float> pixels() const
        {
            auto begin = reinterpret_cast<const float*>(readback->image->data());
            return std::vector<float>(begin,begin+width*height*4);
        }

        //! Saves a display-encoded PNG for manual visual review without changing the rendered image.
        bool save(const std::string& name) const
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(width,height,1,GL_RGBA,GL_UNSIGNED_BYTE);
            auto values = pixels();
            for (unsigned i=0; i<values.size(); ++i)
                image->data()[i] = static_cast<unsigned char>(255.0f*osg::clampBetween(values[i],0.0f,1.0f));
            return osgDB::writeImageFile(*image,name);
        }

        //! Stops draw threads and releases GL resources before the owning context disappears.
        ~Scene()
        {
            viewer->stopThreading();
            context->makeCurrent();
            viewer->getCamera()->setFinalDrawCallback(nullptr);
            viewer->getCamera()->releaseGLObjects(context->getState());
            sky->releaseGLObjects(context->getState());
            ChonkRenderBin::releaseSharedGLObjects(context->getState());
            context->releaseContext();
        }
    };
} }
