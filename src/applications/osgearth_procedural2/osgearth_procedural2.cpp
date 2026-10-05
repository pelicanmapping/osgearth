/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarthPrestige/VegetationLayer>
#include "AssetImport.h"
#include <osgEarth/MapNode>
#include <osgEarth/EarthManipulator>
#include <osgEarth/Registry>
#include <osgEarth/NodeUtils>
#include <osgEarth/SimplePager>
#include <osgEarth/VirtualProgram>
#include <osgEarth/GLUtils>
#include <osgEarth/Chonk>
#include <osgEarth/ElevationPool>
#include <osgEarth/AutoClipPlaneHandler>
#include <osgEarth/ExampleResources>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <osgDB/WriteFile>
#include <osgDB/FileUtils>
#include <osgText/Text>
#include <chrono>
#include <cmath>
#include <thread>
#include <iostream>
#include <set>
#include <map>
#include <sstream>
#include <iomanip>
#include <fstream>

using namespace osgEarth;
using namespace osgEarthPrestige;

namespace
{
    //! Moves continuously from ground cover to distant trees and back without changing the source population.
    Viewpoint tourViewpoint(const Viewpoint& home, double phase, bool grass)
    {
        const double blend = 0.5 - 0.5 * std::cos(phase * 6.283185307179586);
        Viewpoint result = home;
        result.setRange(Distance((grass ? 8.0 : 24.0) * std::pow(grass ? 25.0 : 250.0, blend), Units::METERS));
        result.setPitch(Angle((grass ? -12.0 : -8.0) - 27.0 * blend, Units::DEGREES));
        return result;
    }

    //! Optional live camera demonstration; T starts/stops a 24-second loop and leaves manual navigation available.
    struct LODTour : osgGA::GUIEventHandler
    {
        osg::observer_ptr<osgEarth::Util::EarthManipulator> manipulator;
        Viewpoint home;
        bool enabled;
        bool grass;
        double start = -1.0;

        //! Retains the home viewpoint, but only observes the viewer-owned camera manipulator.
        LODTour(osgEarth::Util::EarthManipulator* value, const Viewpoint& viewpoint, bool active, bool grassExperiment) :
            manipulator(value), home(viewpoint), enabled(active), grass(grassExperiment) { }

        //! Applies smooth camera motion on frame events; all non-toggle input propagates normally.
        bool handle(const osgGA::GUIEventAdapter& event, osgGA::GUIActionAdapter&) override
        {
            if (event.getEventType() == osgGA::GUIEventAdapter::KEYUP &&
                (event.getKey() == 't' || event.getKey() == 'T'))
            {
                enabled = !enabled;
                start = -1.0;
                return true;
            }
            if (enabled && event.getEventType() == osgGA::GUIEventAdapter::FRAME)
            {
                if (start < 0.0) start = event.getTime();
                osg::ref_ptr<osgEarth::Util::EarthManipulator> camera;
                if (manipulator.lock(camera))
                    camera->setViewpoint(tourViewpoint(home, std::fmod((event.getTime()-start)/24.0, 1.0), grass), 0.0);
            }
            return false;
        }
    };

    //! Saves the actual front buffer; the caller chooses when paging has settled and owns the GL context.
    bool writeFrame(osg::GraphicsContext* context, const std::string& filename)
    {
        if (!context->makeCurrent()) return false;
        osg::ref_ptr<osg::Image> image = new osg::Image();
        image->readPixels(0,0,1440,960,GL_RGBA,GL_UNSIGNED_BYTE);
        const bool saved = osgDB::writeImageFile(*image, filename);
        context->releaseContext();
        return saved;
    }

    //! Finds independent pagers once; key toggles act on scene masks, without rebuilding populations.
    struct GroupControls : osgGA::GUIEventHandler
    {
        std::vector<osg::ref_ptr<osgEarth::Util::SimplePager>> groups;
        osg::ref_ptr<osgText::Text> text = new osgText::Text();
        osg::ref_ptr<osg::Uniform> debug;
        bool grass;
        bool assetComparison;

        //! Collects the example's group roots and initializes its on-screen legend.
        GroupControls(osg::Node* node, osg::Uniform* debugLOD, bool grassExperiment, bool useAssets) :
            debug(debugLOD), grass(grassExperiment), assetComparison(useAssets)
        {
            forEachNodeOfType<osgEarth::Util::SimplePager>(node,
                [this](osgEarth::Util::SimplePager* pager) { groups.emplace_back(pager); });
            text->setFont(Registry::instance()->getDefaultFont());
            text->setCharacterSize(20);
            text->setPosition(osg::Vec3(30,90,0));
            text->setColor(osg::Vec4(0.90f,0.94f,0.82f,1));
            text->setBackdropType(osgText::Text::OUTLINE);
            refresh();
        }

        //! Rewrites the legend to reflect which populations are currently traversed.
        void refresh()
        {
            std::string caption = grass ? "OSGEARTH PRESTIGE  /  OPTIONAL GRASS EXPERIMENT\n" :
                "OSGEARTH PRESTIGE  /  LANDSCAPE DEMO\n";
            caption += "[T] Camera tour  |  [L] LOD colors: cyan = detailed, orange = coarse\n";
            caption += grass ? (assetComparison ? "Asset grass at the same patch centers  |  Procedural mode OFF\n" :
                "GPU-generated blades  |  CPU patch elevation/slope  |  Demonstration wind\n") :
                "Independent population ranges  |  Stable density thinning\n";
            for (unsigned i = 0; i < groups.size(); ++i)
                caption += "[" + std::to_string(i+1) + "] " + groups[i]->getName() +
                    (groups[i]->getNodeMask() ? " ON    " : " OFF    ");
            text->setText(caption);
        }

        //! Toggles LOD colors or groups 1..9 on key release; other events pass to the camera manipulator.
        bool handle(const osgGA::GUIEventAdapter& event, osgGA::GUIActionAdapter&) override
        {
            if (event.getEventType() == osgGA::GUIEventAdapter::KEYUP &&
                (event.getKey() == 'l' || event.getKey() == 'L'))
            {
                bool enabled = false;
                debug->get(enabled);
                debug->set(!enabled);
                refresh();
                return true;
            }
            if (event.getEventType() != osgGA::GUIEventAdapter::KEYUP || event.getKey() < '1' ||
                event.getKey() >= int('1' + groups.size())) return false;
            auto group = groups[event.getKey() - '1'];
            group->setNodeMask(group->getNodeMask() ? 0u : ~0u);
            refresh();
            return true;
        }
    };

    //! Adds a fixed-size overlay; the capture and default interactive window both use 1440x960.
    osg::Camera* createLegend(osgText::Text* text)
    {
        auto camera = new osg::Camera();
        camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        camera->setRenderOrder(osg::Camera::POST_RENDER);
        camera->setClearMask(GL_DEPTH_BUFFER_BIT);
        camera->setViewMatrix(osg::Matrix::identity());
        camera->setProjectionMatrix(osg::Matrix::ortho2D(0,1440,0,960));
        camera->setAllowEventFocus(false);
        auto geode = new osg::Geode();
        geode->addDrawable(text);
        camera->addChild(geode);
        return camera;
    }

    //! Reports scene residency after a paging interval; GPU visibility and GPU resource counts are separate.
    std::size_t reportPopulations(const GroupControls& controls, std::ostream& out = std::cout)
    {
        std::size_t total = 0;
        for (auto group : controls.groups)
        {
            std::size_t count = 0;
            std::map<std::string, std::size_t> representations;
            std::set<const ChonkDrawable*> drawables;
            forEachNodeOfType<ChonkDrawable>(group.get(),
                [&](ChonkDrawable* drawable)
                {
                    if (drawables.insert(drawable).second)
                    {
                        count += drawable->getNumInstances();
                        representations[drawable->getName()] += drawable->getNumInstances();
                    }
                });
            total += count;
            out << group->getName() << ": " << count << " resident instances in " << drawables.size()
                << " unique drawables\n";
            for (const auto& representation : representations)
                out << "  " << representation.first << ": " << representation.second << " resident instances\n";
        }
        return total;
    }

    //! Samples the demo's focal height before capture; falls back to its supplied height if elevation is unavailable.
    GeoPoint groundedFocus(const Map* map, const GeoPoint& input)
    {
        GeoPoint result = input.transform(map->getSRS());
        std::vector<osg::Vec3d> points{result.vec3d()};
        ElevationPool::WorkingSet workingSet;
        map->getElevationPool()->sampleMapCoords(points.begin(), points.end(), Distance(10, Units::METERS),
            &workingSet, nullptr, NO_DATA_VALUE);
        if (std::isfinite(points.front().z()) && points.front().z() != NO_DATA_VALUE) result.z() = points.front().z();
        return result;
    }
}

//! Runs the live example or captures actual paged Chonk rendering to a PNG using an offscreen GL context.
int main(int argc, char** argv)
{
    osg::ArgumentParser args(&argc, argv);
    if (args.find("--asset-source") >= 0) return importVegetationAsset(args);
    std::string capture, view = "ground", disabled, sequence;
    unsigned frames = 300u;
    float terrainSSE = 0.0f;
    double utcHours = 12.0;
    double rangeOverride = 0.0, pitchOverride = -35.0;
    double longitude = -75.0, latitude = 40.65;
    args.read("--capture", capture);
    args.read("--view", view);
    args.read("--frames", frames);
    const bool overrideRange = args.read("--range", rangeOverride);
    const bool overridePitch = args.read("--pitch", pitchOverride);
    if (overridePitch && (!std::isfinite(pitchOverride) || pitchOverride < -90.0 || pitchOverride > 0.0)) return 1;
    if (args.read("--no-chonk-occlusion")) ChonkRenderBin::setOcclusionCulling(false);
    if (overrideRange && (!std::isfinite(rangeOverride) || rangeOverride <= 0.0)) return 1;
    args.read("--disable-group", disabled);
    const bool location = args.read("--location", longitude, latitude);
    const bool pagingTour = args.read("--paging-tour");
    const bool lodTour = args.read("--lod-tour");
    const bool canopyTour = args.read("--canopy-tour");
    const bool lodDebug = args.read("--lod-debug");
    std::string clusterDebug = "off";
    args.read("--cluster-debug",clusterDebug);
    if (clusterDebug != "off" && clusterDebug != "tiers" && clusterDebug != "clusters") return 1;
    const bool fullDetail = args.read("--full-detail");
    const bool fullDensity = args.read("--full-density");
    const bool assetGrass = args.read("--asset-grass");
    args.read("--sequence", sequence);
    const bool frameStats = args.read("--frame-stats");
    const bool overrideSSE = args.read("--terrain-sse", terrainSSE);
    const bool overrideTime = args.read("--utc-hours", utcHours);
    if (overrideTime && (!std::isfinite(utcHours) || utcHours < 0.0 || utcHours >= 24.0)) return 1;
    if (overrideSSE && (!std::isfinite(terrainSSE) || terrainSSE <= 0.0f)) return 1;
    if (!std::isfinite(longitude) || !std::isfinite(latitude) || std::abs(longitude) > 180.0 ||
        std::abs(latitude) > 90.0 || (pagingTour && (capture.empty() || view != "ground" || lodTour)) ||
        (canopyTour && (capture.empty() || pagingTour || lodTour)) ||
        (!sequence.empty() && (!(lodTour || canopyTour) || capture.empty()))) return 1;
    if (args.read("--help") ||
        (view != "ground" && view != "top" && view != "overview" && view != "distant" && view != "orbit") || frames == 0u)
    {
        std::cout << "osgearth_procedural2 file.earth [--view ground|top|overview|distant|orbit] [--capture image.png]\n"
            << "  [--frames 300] [--disable-group grass] [--terrain-sse pixels]\n"
            << "  [--range meters] [--pitch degrees] [--samples 4] [--no-chonk-occlusion]\n"
            << "  [--location longitude latitude] [--paging-tour (ground capture only)]\n"
            << "  [--utc-hours 12]  Fix the sky time for reproducible daytime art captures.\n"
            << "  [--canopy-tour]  Capture a 16km-to-2.2km forest approach and return.\n"
            << "  [--lod-tour] [--lod-debug] [--full-detail] [--full-density] [--sequence output-directory]\n"
            << "  [--cluster-debug off|tiers|clusters]  Color medium/far culling groups without changing visibility.\n"
            << "  [--asset-grass]  Replace experimental grass patches with asset clumps at the same centers.\n"
            << "  [--prestige-sky --shadows] [--frame-stats]   Frame stats use OSG's viewer diagnostics.\n"
            << "  Keys 1-5 toggle populations; L toggles LOD colors; T toggles the camera tour.\n";
        return 0;
    }
    osgEarth::initialize(args);
    osg::DisplaySettings::instance()->readCommandLine(args);
    GLUtils::useNVGL(true);
    osg::ref_ptr<MapNode> mapNode = MapNode::load(args);
    if (!mapNode || !mapNode->open())
    {
        std::cerr << "Supply tests/procedural2.earth (see --help).\n";
        return 1;
    }
    if (overrideSSE) mapNode->setScreenSpaceError(terrainSSE);
    auto* layer = mapNode->getMap()->getLayer<VegetationLayer>();
    if (!layer || layer->getStatus().isError())
    {
        std::cerr << "Prestige vegetation did not open: " << (layer ? layer->getStatus().toString() : "missing layer") << '\n';
        return 1;
    }
    layer->setClusterDebug(clusterDebug == "clusters" ? ClusterDebugMode::CLUSTERS :
        clusterDebug == "tiers" ? ClusterDebugMode::TIERS : ClusterDebugMode::OFF);
    bool grassExperiment = false;
    for (auto group : layer->options().groups())
    {
        grassExperiment = grassExperiment || group.proceduralGrass;
        if (assetGrass && group.proceduralGrass)
        {
            group.proceduralGrass = false;
            if (layer->setGroup(group).isError()) return 1;
        }
    }
    auto profile = Profile::create(layer->options().profile().get());
    GeoPoint focus = profile->getExtent().getCentroid().transform(mapNode->getMapSRS());
    if (location || profile->getExtent().width() == 360.0)
        focus = GeoPoint(SpatialReference::get("wgs84"), longitude, latitude, 0.0).transform(mapNode->getMapSRS());
    if (view != "orbit") focus = groundedFocus(mapNode->getMap(), focus);
    std::cout << "Demo focal elevation: " << focus.z() << " m in map SRS\n";
    Viewpoint viewpoint;
    viewpoint.setName(grassExperiment ? "Optional grass experiment" : "Prestige landscape");
    viewpoint.setFocalPoint(focus);
    viewpoint.setHeading(Angle(-20, Units::DEGREES));
    viewpoint.setPitch(Angle(view == "orbit" || view == "top" ? -90 : view == "ground" ?
        (grassExperiment ? -12 : -8) : -35, Units::DEGREES));
    viewpoint.setRange(Distance(view == "orbit" ? 20000000 : view == "distant" ? 6000 :
        view == "overview" ? (grassExperiment ? 120 : 1200) : view == "top" ?
        (grassExperiment ? 40 : 160) : (grassExperiment ? 8 : 24), Units::METERS));
    if (overrideRange) viewpoint.setRange(Distance(rangeOverride, Units::METERS));
    if (overridePitch) viewpoint.setPitch(Angle(pitchOverride,Units::DEGREES));

    auto layerState = layer->getOrCreateStateSet();
    auto debug = new osg::Uniform("oe_p2_debug", lodDebug);
    layerState->addUniform(debug);
    auto vp = VirtualProgram::getOrCreate(layerState);
    vp->setFunction("oe_p2_debug_vertex", R"(
        uint chonk_lod;
        flat out uint oe_p2_lod;
        void oe_p2_debug_vertex(inout vec4 vertex) { oe_p2_lod = chonk_lod; }
        )", VirtualProgram::LOCATION_VERTEX_MODEL, 0.1f);
    vp->setFunction("oe_p2_debug_fragment", R"(
        uniform bool oe_p2_debug;
        flat in uint oe_p2_lod;
        void oe_p2_debug_fragment(inout vec4 color) {
            if (oe_p2_debug) color.rgb = oe_p2_lod == 0u ? vec3(0.1,0.9,1.0) : vec3(1.0,0.4,0.05);
        })", VirtualProgram::LOCATION_FRAGMENT_LIGHTING, 1000000.0f);
    if (fullDetail)
        layerState->addUniform(new osg::Uniform("oe_lod_scale", osg::Vec4f(0,1,1,1)), osg::StateAttribute::OVERRIDE);
    if (fullDensity)
        layerState->addUniform(new osg::Uniform("oe_chonk_density_lod", osg::Vec3f(0,0,1)), osg::StateAttribute::OVERRIDE);

    osgViewer::Viewer viewer;
    viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    viewer.setRealizeOperation(new GL3RealizeOperation());
    auto manipulator = new osgEarth::Util::EarthManipulator();
    // Coarse mountain tiles can push a capture camera outward before refinement; keep the requested sampled view fixed.
    // Interactive navigation retains terrain avoidance.
    if (!capture.empty()) manipulator->getSettings()->setTerrainAvoidanceEnabled(false);
    viewer.setCameraManipulator(manipulator);
    auto root = new osg::Group();
    root->addChild(mapNode);
    auto controls = new GroupControls(layer->getNode(), debug, grassExperiment, assetGrass);
    for (auto group : controls->groups)
        if (group->getName() == disabled) group->setNodeMask(0u);
    controls->refresh();
    root->addChild(createLegend(controls->text));
    viewer.addEventHandler(controls);
    viewer.addEventHandler(new osgViewer::StatsHandler());
    viewer.addEventHandler(new LODTour(manipulator, viewpoint, lodTour && capture.empty(), grassExperiment));

    osg::ref_ptr<osg::GraphicsContext> context;
    if (!capture.empty())
    {
        auto traits = new osg::GraphicsContext::Traits(osg::DisplaySettings::instance());
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = 1440;
        traits->height = 960;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context)
        {
            std::cerr << "Cannot create offscreen graphics context.\n";
            return 1;
        }
        context->getState()->setUseVertexAttributeAliasing(true);
        context->getState()->setUseModelViewAndProjectionUniforms(true);
        viewer.getCamera()->setGraphicsContext(context);
        viewer.getCamera()->setViewport(0,0,1440,960);
        viewer.getCamera()->setDrawBuffer(GL_FRONT);
        viewer.getCamera()->setReadBuffer(GL_FRONT);
    }
    else
        viewer.setUpViewInWindow(60,60,1440,960);
    viewer.getCamera()->setClearColor(osg::Vec4(0.56f,0.68f,0.72f,1));
    viewer.getCamera()->setSmallFeatureCullingPixelSize(-1.0f);
    viewer.getCamera()->setProjectionMatrixAsPerspective(45.0, 1.5, 0.1, 100000.0);
    GLUtils::setGlobalDefaults(viewer.getCamera()->getOrCreateStateSet());
    // The globe's bounds otherwise push OSG's default near plane through the local vegetation.
    auto clip = new osgEarth::Util::AutoClipPlaneCullCallback(mapNode);
    clip->setMinNearFarRatio(0.000001);
    viewer.getCamera()->addCullCallback(clip);
    if (args.find("--prestige-sky") >= 0 || args.find("--shadows") >= 0)
        osgEarth::Util::MapNodeHelper().parse(mapNode, args, &viewer, root);
    if (overrideTime)
    {
        const auto date = Registry::instance()->getDateTime();
        Registry::instance()->setDateTime(DateTime(date.year(), date.month(), date.day(), utcHours));
    }
    if (frameStats)
    {
        viewer.getViewerStats()->collectStats("update", true);
        viewer.getCamera()->getStats()->collectStats("rendering", true);
        viewer.getCamera()->getStats()->collectStats("gpu", true);
    }
    viewer.setSceneData(root);
    manipulator->setViewpoint(viewpoint, 0.0);
    viewer.realize();
    if (!viewer.isRealized()) return 1;
    // Let the earth-file extension initialize its home view before applying an explicit demo/capture view.
    viewer.frame();
    manipulator->setViewpoint(viewpoint, 0.0);
    if (capture.empty()) return viewer.run();

    const unsigned stages = pagingTour ? 6u : 1u;
    bool pagingOK = true;
    std::ostringstream checkpointReport;
    for (unsigned stage = 0; stage < stages; ++stage)
    {
        Viewpoint target = viewpoint;
        const char* label = "Requested view";
        if (pagingTour)
        {
            const double longitudes[] = {longitude, 172.6, 179.9999, 25.0, longitude, longitude};
            const double latitudes[] = {latitude, -43.5, -17.0, 70.0, latitude, latitude};
            const char* labels[] = {"Home", "New Zealand", "Antimeridian", "High latitude", "Orbit", "Reload home"};
            const GeoPoint location(SpatialReference::get("wgs84"), longitudes[stage], latitudes[stage], 0.0);
            target.setFocalPoint(stage == 4u ? location : groundedFocus(mapNode->getMap(), location));
            target.setPitch(Angle(stage == 4u ? -90 : -8, Units::DEGREES));
            target.setRange(Distance(stage == 4u ? 20000000 : 24, Units::METERS));
            label = labels[stage];
        }
        manipulator->setViewpoint(target, 0.0);
        for (unsigned frame = 0; frame < frames; ++frame)
        {
            viewer.frame();
            // Give asynchronous paging a wall-clock window; this is a visual fixture, not a benchmark.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::cout << "Paging checkpoint: " << label << '\n';
        const auto total = reportPopulations(*controls);
        const bool expectEmpty = (pagingTour && stage == 4u) || view == "orbit";
        const auto assets = layer->getAssetResidency();
        checkpointReport << label << ": " << total << " instances, " << assets.assets <<
            " asset bundles, " << assets.bytes << " content bytes\n";
        if (expectEmpty ? (total != 0 || assets.assets != 0u || assets.bytes != 0u) :
            (total == 0 || assets.assets == 0u))
        {
            std::cerr << "Unexpected population residency at " << label << '\n';
            pagingOK = false;
        }
    }
    if (frameStats)
    {
        // Report OSG's recent-frame diagnostics, excluding our paging sleep and startup frames.
        for (const auto* attribute : {"Cull traversal time taken", "Draw traversal time taken", "GPU draw time taken"})
        {
            double seconds = 0.0;
            if (viewer.getCamera()->getStats()->getAveragedAttribute(attribute, seconds))
                std::cout << attribute << ": " << seconds * 1000.0 << " ms (OSG recent frames)\n";
        }
    }
    // Match the NVGL Inspector's resource count, separately from scene drawable/instance counts above.
    for (const auto& entry : GLObjectPool::getAll())
    {
        std::size_t objects = 0, bytes = 0;
        for (const auto& object : entry.second->objects())
            if (object->category() == "Chonk drawable") { ++objects; bytes += object->size(); }
        std::cout << "Context " << entry.first << ": " << objects << " Chonk drawable GPU objects, "
            << double(bytes) / 1048576.0 << " MiB allocated\n";
    }
    if (lodTour || canopyTour)
    {
        if (!sequence.empty() && !osgDB::makeDirectory(sequence)) return 1;
        for (unsigned frame = 0; frame < 360u; ++frame)
        {
            Viewpoint moving = tourViewpoint(viewpoint,double(frame)/359.0,grassExperiment);
            if (canopyTour)
            {
                // Continuous approach and reversal exercise both paging handovers; PNG frames are visual evidence only.
                moving = viewpoint;
                const double t = 0.5-0.5*std::cos(double(frame)/359.0*6.283185307179586);
                moving.setRange(Distance(16000.0*std::pow(2200.0/16000.0,t),Units::METERS));
                moving.setPitch(Angle(-45.0,Units::DEGREES));
            }
            manipulator->setViewpoint(moving,0.0);
            viewer.frame();
            if (!sequence.empty() && frame % 6u == 0u)
            {
                std::ostringstream filename;
                filename << sequence << "/frame" << std::setw(4) << std::setfill('0') << frame/6u << ".png";
                if (!writeFrame(context, filename.str())) return 1;
                std::ofstream cameraReport(filename.str()+".txt");
                cameraReport << manipulator->getViewpoint().getConfig().toJSON();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Allow source paging and expiry to settle again before the final return-home snapshot.
        for (unsigned frame = 0; frame < frames; ++frame)
        {
            viewer.frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::cout << "Completed moving-camera LOD tour\n";
        if (reportPopulations(*controls) == 0u) pagingOK = false;
    }
    const bool saved = writeFrame(context, capture);
    // Preserve reproducible camera/residency evidence even when a Windows GUI launch has no stdout console.
    std::ofstream report(capture + ".txt");
    const auto assets = layer->getAssetResidency();
    report << "Asset bundles: " << assets.assets << ", content bytes: " << assets.bytes << '/' << assets.budget <<
        ", load failures: " << assets.failedLoads << ", budget denials: " << assets.budgetDenials << '\n';
    report << "MSAA samples requested: " << osg::DisplaySettings::instance()->getNumMultiSamples() << '\n';
    report << checkpointReport.str();
    report << "Capture: " << capture << "\nFocal elevation: " << focus.z() << " m in map SRS\n"
        << "Viewpoint: " << manipulator->getViewpoint().getConfig().toJSON() << '\n'
        << "Procedural grass: " << (grassExperiment && !assetGrass ? "enabled" : "disabled") << '\n';
    reportPopulations(*controls, report);
    report << "Paging checks: " << (pagingOK ? "passed" : "FAILED") << '\n';
    context->makeCurrent();
    ChonkRenderBin::releaseSharedGLObjects(context->getState());
    context->releaseContext();
    std::cout << (saved ? "Captured " : "Failed to save ") << capture << '\n';
    return saved && pagingOK ? 0 : 1;
}
