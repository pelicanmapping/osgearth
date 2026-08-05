/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <osgEarth/EarthManipulator>
#include <osgEarth/ExampleResources>
#include <osgEarth/MapNode>
#include <osgEarth/PhongLightingEffect>
#include <osgGA/TrackballManipulator>
#include <osgDB/WriteFile>
#include <osg/Image>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <thread>

#include <osgEarth/Metrics>

#define LC "[viewer] "

using namespace osgEarth;
using namespace osgEarth::Util;

namespace
{
    class ScreenshotWriter :
        public osgViewer::ScreenCaptureHandler::CaptureOperation
    {
    public:
        explicit ScreenshotWriter(std::string filename) :
            _filename(std::move(filename)) { }

        void operator()(const osg::Image& image, unsigned) override
        {
            _succeeded = osgDB::writeImageFile(image, _filename);
        }

        bool succeeded() const { return _succeeded; }

    private:
        std::string _filename;
        bool _succeeded = false;
    };

    class ScreenshotDrawCallback : public osg::Camera::DrawCallback
    {
    public:
        ScreenshotDrawCallback(
            ScreenshotWriter* writer,
            osg::Camera::DrawCallback* nested) :
            _writer(writer),
            _nested(nested) { }

        void operator()(osg::RenderInfo& renderInfo) const override
        {
            if (_nested.valid())
                (*_nested)(renderInfo);
            if (_captured || !_writer.valid())
                return;

            osg::Camera* camera = renderInfo.getCurrentCamera();
            osg::Viewport* viewport = camera ? camera->getViewport() : nullptr;
            osg::State* state = renderInfo.getState();
            if (!viewport || !state)
                return;

            state->glReadBuffer(GL_BACK);
            osg::ref_ptr<osg::Image> image = new osg::Image();
            image->readPixels(
                static_cast<int>(viewport->x()),
                static_cast<int>(viewport->y()),
                static_cast<int>(viewport->width()),
                static_cast<int>(viewport->height()),
                GL_RGBA,
                GL_UNSIGNED_BYTE);
            (*_writer)(*image, state->getContextID());
            _captured = true;
        }

    private:
        osg::ref_ptr<ScreenshotWriter> _writer;
        osg::ref_ptr<osg::Camera::DrawCallback> _nested;
        mutable bool _captured = false;
    };

    int
    run(
        osgViewer::Viewer& viewer,
        const std::string& screenshotFile,
        int frameLimit)
    {
        if (screenshotFile.empty())
            return viewer.run();

        const std::filesystem::path output =
            std::filesystem::absolute(screenshotFile);
        if (output.has_parent_path())
        {
            std::error_code error;
            std::filesystem::create_directories(output.parent_path(), error);
            if (error)
            {
                OE_WARN << LC << "Failed to create screenshot directory "
                    << output.parent_path() << std::endl;
                return 3;
            }
        }

        osg::ref_ptr<ScreenshotWriter> writer =
            new ScreenshotWriter(output.string());
        osg::ref_ptr<osg::Camera::DrawCallback> previous =
            viewer.getCamera()->getFinalDrawCallback();
        osg::ref_ptr<ScreenshotDrawCallback> callback =
            new ScreenshotDrawCallback(writer.get(), previous.get());

        viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
        viewer.setDone(false);
        for (int frame = 0; frame < frameLimit && !viewer.done(); ++frame)
        {
            if (frame == frameLimit - 1)
                viewer.getCamera()->setFinalDrawCallback(callback.get());
            viewer.frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        viewer.getCamera()->setFinalDrawCallback(previous.get());

        if (!writer->succeeded())
        {
            OE_WARN << LC << "Failed to write screenshot "
                << output << std::endl;
            return 3;
        }

        OE_NOTICE << LC << "Wrote screenshot " << output << std::endl;
        return 0;
    }
}

int
usage(const char* name)
{
    std::cout
        << "View an earth file: " << name << " file.earth" << std::endl
        << "View an OSG model: " << name << " [modelfile.ext] [--light]" << std::endl
        << "Capture after paging: --screenshot file.png [--frames 180]"
        << std::endl
        << Util::MapNodeHelper().usage() << std::endl;

    return 0;
}


int
main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc,argv);
    if ( arguments.read("--help") )
        return usage(argv[0]);

    std::string screenshotFile;
    int frameLimit = 180;
    arguments.read("--screenshot", screenshotFile);
    arguments.read("--frames", frameLimit);
    if (!screenshotFile.empty() && frameLimit < 1)
    {
        OE_WARN << LC << "--frames must be greater than zero" << std::endl;
        return 1;
    }

    // start up osgEarth
    osgEarth::initialize(arguments);

    // create a simple view
    osgViewer::Viewer viewer(arguments);

    // install our default manipulator (do this before calling load)
    viewer.setCameraManipulator(new EarthManipulator(arguments));

    // disable the small-feature culling; necessary for some feature rendering
    viewer.getCamera()->setSmallFeatureCullingPixelSize(-1.0f);

    // load an earth file, and support all or our example command-line options
    auto node = MapNodeHelper().load(arguments, &viewer);
    if (node.valid())
    {
        if (MapNode::get(node))
        {
            viewer.setSceneData(node);
            return run(viewer, screenshotFile, frameLimit);
        }
        else
        {
            // not an earth file? Just view as a normal OSG node or image with basic lighting
            viewer.setCameraManipulator(new osgGA::TrackballManipulator);

            if (arguments.read("--light"))
            {
                osg::LightSource* lightSource = new osg::LightSource();
                lightSource->getLight()->setAmbient(osg::Vec4(0.75f, 0.75f, 0.75f, 1.0f));
                
                auto group = new PhongLightingGroup();
                group->addChild(lightSource);
                group->addChild(node);

                ShaderGenerator gen;
                gen.run(group);

                viewer.setSceneData(group);

                if (!screenshotFile.empty())
                    return run(viewer, screenshotFile, frameLimit);

                while (!viewer.done())
                {
                    auto cam = viewer.getCamera()->getInverseViewMatrix().getTrans();
                    cam.normalize();
                    lightSource->getLight()->setPosition(osg::Vec4d(cam.x(), cam.y(), cam.z(), 0));
                    viewer.frame();
                }
                return 0;
            }
            else
            {
                ShaderGenerator gen;
                gen.run(node);
                viewer.setSceneData(node);
                return run(viewer, screenshotFile, frameLimit);
            }
        }
    }

    return usage(argv[0]);
}
