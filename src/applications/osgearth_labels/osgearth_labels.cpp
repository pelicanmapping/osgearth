/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#include <osgEarthPrestige/LabelRendererInternal.h>
#include <osgEarthPrestige/LabelTilesInternal.h>
#include <osgEarthPrestige/LabelDynamicInternal.h>
#include <osgEarth/OGRFeatureSource>
#include <osgEarth/GDAL>
#include <osgEarth/EarthManipulator>
#include <osgEarth/ExampleResources>
#include <osgEarth/GLUtils>
#include <osgEarth/MapNode>
#include <osgEarth/LogarithmicDepthBuffer>
#include <osgViewer/CompositeViewer>
#include <osgDB/WriteFile>
#include <array>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <cmath>

using namespace osgEarth;
using namespace osgEarthPrestige::Labeling;

namespace
{
    struct Place { const char* text; double lon, lat; unsigned style; };
    const Place places[] = {
        { "Central Park", -73.9740, 40.7740, 1 },
        { "Lincoln Center", -73.9835, 40.7725, 0 },
        { "Times Square", -73.9855, 40.7580, 1 },
        { "Grand Central", -73.9772, 40.7527, 0 },
        { "Empire State", -73.9857, 40.7484, 1 },
        { "Hudson River", -74.0030, 40.7650, 2 },
        { u8"office · café", -73.9660, 40.7620, 0 },
        { u8"مرحبا بالعالم", -73.9760, 40.7420, 0 },
        { u8"A\u0301 · Å · ffi", -74.0005, 40.7490, 0 },
        { u8"Small text · 12 px", -73.9980, 40.7390, 3 }
    };

    struct Capture : osg::Camera::DrawCallback
    {
        std::string path;
        int width, height;
        unsigned frame;
        mutable bool saved = false;
        //! Captures the composed back buffer once after fonts and fades have had time to settle.
        Capture(std::string value, int w, int h, unsigned f) : path(std::move(value)), width(w), height(h), frame(f) { }
        //! Runs on the drawing context and reads both independent viewports before their shared buffer swaps.
        void operator()(osg::RenderInfo& info) const override
        {
            if (!saved && info.getState()->getFrameStamp()->getFrameNumber() >= frame)
            {
                osg::ref_ptr<osg::Image> image = new osg::Image;
                image->readPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE);
                saved = osgDB::writeImageFile(*image, path);
            }
        }
    };

    //! Chooses a local demo font without bundling or redistributing system font files.
    std::string defaultFont()
    {
#ifdef _WIN32
        const char* windows = std::getenv("WINDIR");
        return std::string(windows ? windows : "C:/Windows") + "/Fonts/segoeui.ttf";
#else
        return "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif
    }

    //! Exercises shared annotation edits on the control thread; each camera owns its own adoption and fades.
    void updateAnnotations(DynamicSource& source, bool tiled, double time, unsigned& previousPhase)
    {
        const unsigned phase = static_cast<unsigned>(time / 2) % 5;
        DynamicLabel moving;
        moving.text = phase == 1 || phase == 2 ? "Survey team - updated" :
            phase == 4 ? "Survey team - returned" : "Survey team";
        moving.priority = 100;
        moving.style.pixels = phase == 1 || phase == 2 ? 24 : 21;
        moving.style.color = phase == 1 || phase == 2 ? osg::Vec4f(0.3f,1,1,1) : osg::Vec4f(1,0.45f,0.7f,1);
        GeoPoint(SpatialReference::get("wgs84"), tiled ? 7 + 4 * std::sin(time * 0.5) : -73.98 +
            0.008 * std::sin(time * 0.5), tiled ? 48 : 40.77, 10).toWorld(moving.worldAnchor);
        if (phase != 3) source.set(1, moving);
        else if (previousPhase != phase) source.erase(1);
        if (previousPhase != phase)
        {
            if (phase == 2)
            {
                DynamicLabel report;
                report.text = "Temporary report"; report.priority = 100;
                report.style.color = osg::Vec4f(0.5f, 1, 0.5f, 1);
                GeoPoint(SpatialReference::get("wgs84"), tiled ? 12 : -74.0, tiled ? 42 : 40.75, 10)
                    .toWorld(report.worldAnchor);
                source.set(2, report);
            }
            else if (phase == 4) source.erase(2);
            std::cout << "Dynamic phase=" << phase << " (move/edit/add/remove/re-add cycle)" << std::endl;
        }
        previousPhase = phase;
    }
}

//! Runs the private Slughorn slice over shared geocentric labels, with independently manipulable cameras.
int main(int argc, char** argv)
{
    try
    {
        osg::ArgumentParser args(&argc, argv);
        std::string fontPath = defaultFont(), capturePath;
        unsigned frameLimit = 0;
        unsigned captureFrame = 90;
        double seconds = 0;
        std::string featurePath = "../data/cities.gpkg", imagePath = "../data/world.tif";
        args.read("--font", fontPath);
        args.read("--capture", capturePath);
        args.read("--frames", frameLimit);
        args.read("--seconds", seconds);
        args.read("--capture-frame", captureFrame);
        args.read("--features", featurePath);
        args.read("--imagery", imagePath);
        const bool threaded = args.read("--threaded");
        const bool separate = args.read("--separate-contexts");
        const bool animate = args.read("--animate");
        const bool noDeclutter = args.read("--no-declutter");
        const bool motionTest = args.read("--motion-test");
        const bool onDemand = args.read("--on-demand");
        const bool tiled = args.read("--tiled");
        const bool tour = args.read("--tour");
        const bool dynamic = args.read("--dynamic");
        const unsigned viewCount = args.read("--single") ? 1u : 2u;
        if (args.read("--help"))
        {
            std::cout << "osgearth_labels [--font font.ttf] [--threaded] [--separate-contexts] "
                "[--animate] [--motion-test] [--on-demand] [--seconds N] [--no-declutter] "
                "[--tiled] [--features cities.gpkg] [--imagery world.tif] [--tour] [--dynamic] [--single] "
                "[--frames N] [--capture image.png] [--capture-frame N]\n"
                "Drag/zoom either viewport independently.\n";
            return 0;
        }
        osgEarth::initialize(args);
        // Font work is confined to preparation, before the viewer begins culling. No font object enters a view.
        std::shared_ptr<const TextResources> text;
        if (!tiled)
        {
            auto preparation = std::async(std::launch::async, [fontPath]()
            {
                TextService service(fontPath);
                std::vector<std::string> strings;
                for (const auto& place : places) strings.emplace_back(place.text);
                return service.prepare(strings);
            });
            text = preparation.get();
        }
        std::vector<TextStyle> styles(4);
        styles[0].pixels = 25;
        styles[1].pixels = 30; styles[1].color = osg::Vec4f(1.0f, 0.81f, 0.32f, 1.0f);
        styles[2].pixels = 24; styles[2].color = osg::Vec4f(0.5f, 0.85f, 1.0f, 1.0f);
        styles[2].rotation = 0.35f;
        styles[3].pixels = 12; styles[3].haloWidth = 0.5f;
        std::shared_ptr<TileSource> tiles;
        if (tiled)
        {
            styles.resize(2);
            styles[0].pixels = 20; styles[0].color = osg::Vec4f(1, 0.9f, 0.55f, 1);
            styles[1].pixels = 17; styles[1].color = osg::Vec4f(1, 1, 1, 1);
            osg::ref_ptr<OGRFeatureSource> source = new OGRFeatureSource;
            source->setURL(URI(featurePath));
            auto status = source->open();
            if (status.isError()) throw std::runtime_error(status.toString());
            TileOptions tileOptions;
            tileOptions.repairLegacyControls = true;
            tiles = std::make_shared<TileSource>(source, fontPath, styles, tileOptions);
            std::cout << "Tiled source: " << featurePath << " | features: " << source->getFeatureCount() << std::endl;
        }
        Limits limits;
        if (tiled || dynamic) limits.inputBytes = 32 * 1024 * 1024;
        std::shared_ptr<DynamicSource> updates;
        if (dynamic) updates = std::make_shared<DynamicSource>(fontPath);
        auto engine = std::make_shared<Engine>(limits, noDeclutter ? Solver(project) : Solver(),
            prepareText(text, styles));
        std::vector<Record> records;
        for (unsigned i = 0; !tiled && i < sizeof(places) / sizeof(places[0]); ++i)
        {
            Record record;
            record.key = { 1, i, 0, 0 }; record.revision = 1;
            GeoPoint(SpatialReference::get("wgs84"), places[i].lon, places[i].lat, 10).toWorld(record.worldAnchor);
            record.shapedRun = i; record.style = places[i].style;
            record.priority = record.style == 1 ? 10.0f : 0.0f;
            const auto& run = *text->runs[i];
            const auto& style = styles[record.style];
            setTextFootprint(record, run, style);
            records.push_back(record);
        }
        std::vector<BatchPtr> batches;
        if (!tiled) batches.push_back(std::make_shared<const Batch>(std::move(records)));
        osgViewer::CompositeViewer viewer;
        if (onDemand) viewer.setRunFrameScheme(osgViewer::ViewerBase::ON_DEMAND);
        viewer.setThreadingModel(threaded ? osgViewer::ViewerBase::CullThreadPerCameraDrawThreadPerContext :
            osgViewer::ViewerBase::SingleThreaded);
        viewer.setRealizeOperation(new GL3RealizeOperation);
        MapNode::Options options;
        options.terrain()->color() = Color(0.12f, 0.18f, 0.25f, 1.0f);
        options.enableLighting() = false;
        osg::ref_ptr<MapNode> map = new MapNode(new Map, options);
        if (tiled)
        {
            osg::ref_ptr<GDALImageLayer> imagery = new GDALImageLayer;
            imagery->setURL(URI(imagePath));
            map->getMap()->addLayer(imagery);
        }
        osg::ref_ptr<osg::GraphicsContext> shared;
        std::array<osg::ref_ptr<ViewNode>, 2> roots;
        std::array<osg::ref_ptr<Util::EarthManipulator>, 2> manipulators;
        osg::ref_ptr<Capture> capture;
        for (unsigned i = 0; i < viewCount; ++i)
        {
            osg::ref_ptr<osg::GraphicsContext> context = shared;
            if (!context || separate)
            {
                osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
                traits->x = 30 + (separate ? i * 740 : 0); traits->y = 50;
                traits->width = separate ? 720 : viewCount * 720; traits->height = 800;
                traits->windowName = viewCount == 1 ? "Prestige labels | Single camera" :
                    "Slughorn labels | Camera A: north / Camera B: rotated, 1.25x DPI";
                traits->windowDecoration = true; traits->doubleBuffer = true; traits->vsync = true;
                traits->glContextVersion = "4.6"; traits->glContextProfileMask = 1;
                context = osg::GraphicsContext::createGraphicsContext(traits);
                if (!context) throw std::runtime_error("Cannot create OpenGL context");
                shared = context;
            }
            osg::ref_ptr<osgViewer::View> view = new osgViewer::View;
            auto* camera = view->getCamera();
            camera->setGraphicsContext(context);
            camera->setViewport(separate ? 0 : i * 720, 0, 720, 800);
            camera->setProjectionMatrixAsPerspective(30, 720.0 / 800.0, 1, 1e8);
            camera->setClearColor(osg::Vec4(0.025f, 0.04f, 0.07f, 1));
            camera->setDrawBuffer(GL_BACK); camera->setReadBuffer(GL_BACK);
            roots[i] = new ViewNode(engine, batches, i == 0 ? 1.0f : 1.25f, true, tiles, updates);
            roots[i]->addChild(map);
            view->setSceneData(roots[i]);
            osg::ref_ptr<Util::EarthManipulator> manipulator = new Util::EarthManipulator;
            manipulators[i] = manipulator;
            view->setCameraManipulator(manipulator);
            Util::MapNodeHelper().configureView(view);
            Util::LogarithmicDepthBuffer depth;
            depth.install(camera);
            if (tiled && dynamic)
                manipulator->setViewpoint(Viewpoint("", 10, 47, 0, i == 0 ? 0 : 35, -90,
                    i == 0 ? 7500000 : 5000000), 0);
            else if (tiled)
                manipulator->setViewpoint(Viewpoint("", i == 0 ? -95 : 15, i == 0 ? 35 : 48, 0,
                    i == 0 ? 0 : 25, -90, i == 0 ? 14000000 : 9000000), 0);
            else manipulator->setViewpoint(Viewpoint("", -73.985, 40.758, 0, i == 0 ? 0 : 48, -70,
                i == 0 ? 10500 : 12500), 0);
            if (i + 1 == viewCount && !capturePath.empty())
            {
                capture = new Capture(capturePath, separate ? 720 : viewCount * 720, 800, captureFrame);
                camera->setFinalDrawCallback(capture);
            }
            viewer.addView(view);
        }
        std::cout << "Font: " << fontPath;
        if (text) std::cout << " | atlas pages: " << text->pages.size() << " | atlas bytes: " << text->bytes;
        std::cout << std::endl;
        viewer.realize();
        if (!viewer.isRealized()) throw std::runtime_error("Viewer did not realize");
        unsigned frames = 0;
        const auto start = std::chrono::steady_clock::now();
        double lastEdit = -1;
        unsigned editPhase = 99;
        while (!viewer.done() && (frameLimit == 0 || frames < frameLimit))
        {
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (seconds > 0 && elapsed >= seconds)
                break;
            if (updates && elapsed - lastEdit >= 0.1)
            {
                updateAnnotations(*updates, tiled, elapsed, editPhase);
                lastEdit = elapsed;
            }
            if (onDemand && frames > 0 && !animate && !motionTest && !tour && !viewer.checkNeedToDoFrame() &&
                !roots[0]->needsFrame() && (viewCount == 1 || !roots[1]->needsFrame()))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }
            if (tiled && animate)
                manipulators[0]->setViewpoint(Viewpoint("", -95 + frames * 0.25, 35, 0, 0, -90, 14000000), 0);
            else if (animate)
                manipulators[0]->setViewpoint(Viewpoint("", -73.985, 40.758, 0, frames * 0.2, -70, 10500), 0);
            if (!tiled && motionTest && (frames == 60 || frames == 120))
                manipulators[0]->setViewpoint(Viewpoint("", -73.985, 40.758, 0, frames == 60 ? 120 : 0, -70,
                    frames == 60 ? 5000 : 10500), 0);
            if (tiled && tour && frames % 180 == 0)
            {
                const double stops[][3] = { {-95,35,14000000}, {15,48,9000000}, {139.7,35.7,150000},
                    {-74,40.7,70000}, {151,-33,3000000}, {179,-17,2500000}, {0,83,4000000}, {2.35,48.85,100000} };
                for (unsigned i = 0; i < viewCount; ++i)
                {
                    const auto& stop = stops[(frames / 180 + i * 3) % 8];
                    manipulators[i]->setViewpoint(Viewpoint("", stop[0], stop[1], 0, i * 25, -90, stop[2]), 0);
                }
            }
            viewer.frame();
            ++frames;
        }
        viewer.stopThreading();
        if (updates)
        {
            const auto stats = updates->stats();
            std::cout << "Dynamic: submitted=" << stats.submitted << " completed=" << stats.completed
                << " published=" << stats.published << " superseded=" << stats.superseded
                << " labels=" << stats.labels << " failures=" << stats.failures << " error=" << stats.error << std::endl;
            if (stats.failures) throw std::runtime_error("Dynamic preparation failed: " + stats.error);
        }
        if (tiles)
        {
            const auto stats = tiles->stats();
            std::cout << "Tiles: loads=" << stats.loads << " resident=" << stats.residentTiles
                << " bytes=" << stats.residentBytes << " evictions=" << stats.evictions
                << " failures=" << stats.failures << " skipped=" << stats.skipped << " truncated=" << stats.truncated
                << " error=" << stats.error << std::endl;
            if (stats.failures) throw std::runtime_error("Tiled preparation reported failures: " + stats.error);
        }
        for (unsigned i = 0; i < viewCount; ++i)
        {
            std::cout << "view=" << i << " publications=" << roots[i]->publications()
                << " draw submissions=" << roots[i]->submissions() << std::endl;
            const auto layout = roots[i]->layoutStats();
            std::cout << "  candidates=" << layout.candidates << " accepted=" << layout.accepted
                << " collision tests=" << layout.collisionTests << " solve ms=" << layout.milliseconds
                << " budget exhausted=" << layout.budgetExhausted << std::endl;
            std::cout << "  invalidations=" << roots[i]->invalidations() << " needs frame=" << roots[i]->needsFrame()
                << " frames=" << frames << " dynamic revision=" << roots[i]->displayedDynamicRevision() << std::endl;
            if (frames >= 90 && roots[i]->submissions() == 0)
                throw std::runtime_error("A label view did not submit any text");
        }
        if (capture && frames >= captureFrame && !capture->saved)
            throw std::runtime_error("Cannot save requested capture");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Label demo: " << error.what() << std::endl;
        return 1;
    }
}
