/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <osgEarthImGui/ImGuiApp>
#include <osgEarth/EarthManipulator>
#include <osgEarth/NavigationManipulators>
#include <osgEarth/ExampleResources>
#include <osgEarth/GLUtils>
#include <osgViewer/Viewer>
#include <osgGA/TrackballManipulator>

#include <osgEarthImGui/LayersGUI>
#include <osgEarthImGui/ContentBrowserGUI>
#include <osgEarthImGui/NetworkMonitorGUI>
#include <osgEarthImGui/SceneGraphGUI>
#include <osgEarthImGui/TextureInspectorGUI>
#include <osgEarthImGui/ViewpointsGUI>
#include <osgEarthImGui/LiveCamerasGUI>
#include <osgEarthImGui/SystemGUI>
#include <osgEarthImGui/EnvironmentGUI>
#include <osgEarthImGui/ExternalAssetsGUI>
#include <osgEarthImGui/TerrainGUI>
#include <osgEarthImGui/ShaderGUI>
#include <osgEarthImGui/CameraGUI>
#include <osgEarthImGui/RenderingGUI>
#include <osgEarthImGui/AnnotationsGUI>
#include <osgEarthImGui/PickerGUI>
#include <osgEarthImGui/OpenEarthFileGUI>
#include <osgEarthImGui/ResourceLibraryGUI>
#include <osgEarthImGui/DecalsGUI>

#ifdef OSGEARTH_HAVE_PRESTIGE_NODEKIT
#include <osgEarthImGui/PrestigeVegetationLayerGUI>
#include <osgEarthImGui/PrestigeGrimeLayerGUI>
#include <osgEarthImGui/PrestigeAssetsGUI>
#endif

#ifdef OSGEARTH_HAVE_GEOCODER
#include <osgEarthImGui/SearchGUI>
#endif

#ifdef OSGEARTH_HAVE_PROCEDURAL_NODEKIT
#include <osgEarthImGui/LifeMapLayerGUI>
#include <osgEarthImGui/TerrainEditGUI>
#include <osgEarthImGui/TextureSplattingLayerGUI>
#include <osgEarthImGui/VegetationLayerGUI>
#include <osgEarthImGui/NodeGraphGUI>
#endif

#ifdef OSGEARTH_HAVE_CESIUM_NODEKIT
#include <osgEarthImGui/CesiumIonGUI>
#endif

#include <osgEarth/SelectExtentTool>

#include <osgEarthImGui/FeatureEditGUI>

#define LC "[imgui] "

using namespace osgEarth;
using namespace osgEarth::Util;

namespace
{
    // A one-shot menu action; scene changes run between frames in this single-threaded viewer.
    class ReloadEarthFileGUI : public ImGuiPanel
    {
    public:
        //! Create the reload action without requiring the optional file-dialog library.
        ReloadEarthFileGUI() : ImGuiPanel("Reload Earth File") { }

        //! Enable reload only for a map read from an earth file.
        bool isAvailable(osg::RenderInfo& ri) const override
        {
            auto* v = view(ri);
            auto* mapNode = v ? MapNode::get(v->getSceneData()) : nullptr;
            std::string source;
            return mapNode && mapNode->getUserValue("osgEarth.earthFile", source) && !source.empty();
        }

        //! Queue a single reload and immediately reset the menu action.
        void draw(osg::RenderInfo&) override
        {
            _requested = true;
            setVisible(false);
        }

        //! Report a failed reload while leaving the existing scene available.
        void update(osg::RenderInfo&) override
        {
            if (!_error.empty())
                ImGui::OpenPopup("Reload failed");
            if (ImGui::BeginPopupModal("Reload failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted(_error.c_str());
                if (ImGui::Button("OK"))
                {
                    _error.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        //! Reload from disk between frames; retain the old scene on read/open failure.
        bool reload(osgViewer::View& view)
        {
            if (!_requested)
                return false;
            _requested = false;

            auto* previous = MapNode::get(view.getSceneData());
            std::string source;
            if (!previous || !previous->getUserValue("osgEarth.earthFile", source) || source.empty())
                return false;

            // Read a fresh graph, bypassing OSG's node cache.
            osg::ref_ptr<osgDB::Options> options = new osgDB::Options();
            options->setObjectCacheHint(osgDB::Options::CACHE_NONE);
            osg::ref_ptr<osg::Node> replacement = osgDB::readRefNodeFile(source, options.get());
            auto* mapNode = MapNode::get(replacement);
            if (!mapNode || !mapNode->open())
            {
                _error = "Could not reload " + source + "\nThe current map has been kept.";
                OE_WARN << LC << _error << std::endl;
                return false;
            }

            auto* manipulator = view.getCameraManipulator();
            osg::Matrixd pose;
            Viewpoint viewpoint;
            auto* earth = dynamic_cast<EarthManipulator*>(manipulator);
            if (manipulator)
                pose = manipulator->getMatrix();
            if (earth)
            {
                viewpoint = earth->getViewpoint();
                // A tether target belongs to the graph being discarded.
                viewpoint.setNode(nullptr);
            }

            for (auto& extension : previous->getExtensions())
                if (auto* viewIF = ExtensionInterface<osg::View>::get(extension.get()))
                    viewIF->disconnect(&view);

            // Reset inactive controllers as well, so switching modes cannot retain the old map.
            for (auto& handler : view.getEventHandlers())
            {
                if (auto* modes = dynamic_cast<NavigationModeHandler*>(handler.get()))
                {
                    for (int mode = NavigationModeHandler::MAP; mode <= NavigationModeHandler::DRIVE; ++mode)
                    {
                        auto* controller = modes->getManipulator(static_cast<NavigationModeHandler::Mode>(mode));
                        if (controller)
                        {
                            controller->setNode(nullptr);
                            controller->setNode(replacement.get());
                        }
                    }
                }
            }
            if (manipulator)
                manipulator->setNode(nullptr);
            view.setSceneData(replacement.get());
            if (earth)
                earth->setViewpoint(viewpoint, 0.0);
            else if (manipulator)
                manipulator->setByMatrix(pose);

            for (auto& extension : mapNode->getExtensions())
                if (auto* viewIF = ExtensionInterface<osg::View>::get(extension.get()))
                    viewIF->connect(&view);

            return true;
        }

    private:
        bool _requested = false;
        std::string _error;
    };

    //! Bind a new extent selector to the current map; the view owns the handler.
    void installSelectTool(osgViewer::View& viewer, ImGuiAppEngine* ui, MapNode* mapNode,
        osg::ref_ptr<Contrib::SelectExtentTool>& selectTool)
    {
        if (selectTool.valid())
            viewer.removeEventHandler(selectTool.get());
        ui->setSelectedExtent(GeoExtent());

        selectTool = new Contrib::SelectExtentTool(mapNode);
        selectTool->getStyle().getOrCreateSymbol<LineSymbol>()->stroke()->color() = Color::Red;
        selectTool->setModKeyMask(osgGA::GUIEventAdapter::MODKEY_SHIFT);
        selectTool->onSelect([ui](const GeoExtent& extent) { ui->setSelectedExtent(extent); });
        viewer.getEventHandlers().push_front(selectTool.get());
    }
}

int
usage(const char* name)
{
    OE_NOTICE
        << "\nUsage: " << name << " file.earth" << std::endl
        << "  Windows XInput controllers are detected automatically; toggle in Tools > Camera" << std::endl
        << "  Camera modes: Map, Fly, Walk, Drive; WASD moves, right mouse drag looks." << std::endl
        << MapNodeHelper().usage() << std::endl;
    return 0;
}

int
main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc, argv);
    if (arguments.read("--help"))
        return usage(argv[0]);

    bool extras = arguments.read("--extras");

    osgEarth::initialize(arguments);

    // Handle ICO here so OSG's argument parser does not install its unprotected implementation.
    bool useICO = arguments.read("--ico");

    // Set up the viewer and input handler:
    osgViewer::Viewer viewer(arguments);
    if (useICO)
    {
        viewer.setIncrementalCompileOperation(GLUtils::createIncrementalCompileOperation());
        OE_NOTICE << LC << "ICO vertex-array/index-buffer protection enabled" << std::endl;
    }
    viewer.setThreadingModel(viewer.SingleThreaded);
    // Let the frame thread use the process's allowed CPUs instead of OSG pinning it to CPU 0.
    viewer.setUseConfigureAffinity(false);
    viewer.setProcessorAffinity(OpenThreads::Affinity());
    viewer.setCameraManipulator(new EarthManipulator(arguments));

    // Load the earth file.
    osg::ref_ptr<osg::Node> node = MapNodeHelper().load(arguments, &viewer);
    if (node.valid())
    {
        // Call this to add the GUI. 
        auto ui = new ImGuiAppEngine(arguments);

#ifdef OSGEARTH_HAVE_OPEN_EARTH_FILE_GUI
        ui->add("File", new OpenEarthFileGUI());
#endif
        auto* reload = new ReloadEarthFileGUI();
        ui->add("File", reload);
        ui->add("File", new ImGuiDemoWindowGUI());
        ui->add("File", new SeparatorGUI());
        ui->add("File", new QuitGUI());

        ui->add("Tools", new CameraGUI());
        ui->add("Tools", new ContentBrowserGUI());
        ui->add("Tools", new DecalsGUI());
        ui->add("Tools", new EnvironmentGUI("Sky", false, true));
        ui->add("Tools", new ExternalAssetsGUI());
        ui->add("Tools", new NetworkMonitorGUI());
        ui->add("Tools", new NVGLInspectorGUI());
        ui->add("Tools", new AnnotationsGUI());
        ui->add("Tools", new LayersGUI());
        ui->add("Tools", new PickerGUI());
        ui->add("Tools", new RenderingGUI());
        ui->add("Tools", new ResourceLibraryGUI());
        ui->add("Tools", new SceneGraphGUI());
#ifdef OSGEARTH_HAVE_GEOCODER
        ui->add("Tools", new SearchGUI());
#endif
        ui->add("Tools", new ShaderGUI(&arguments));
        ui->add("Tools", new SystemGUI());
        ui->add("Tools", new TerrainGUI());
        ui->add("Tools", new TextureInspectorGUI());
        ui->add("Tools", new ViewpointsGUI());
        ui->add("Tools", new LiveCamerasGUI());

#ifdef OSGEARTH_HAVE_PRESTIGE_NODEKIT
        ui->add("Prestige", new osgEarthPrestige::VegetationLayerGUI());
        ui->add("Prestige", new EnvironmentGUI("Sky & Clouds", true));
        ui->add("Prestige", new osgEarthPrestige::GrimeLayerGUI());
        ui->add("Prestige", new PrestigeAssetsGUI());
#endif

#ifdef OSGEARTH_HAVE_CESIUM_NODEKIT
        ui->add("Cesium", new osgEarth::Cesium::CesiumIonGUI());
#endif

#ifdef OSGEARTH_HAVE_PROCEDURAL_NODEKIT
        ui->add("Procedural", new osgEarth::Procedural::LifeMapLayerGUI());
        ui->add("Procedural", new osgEarth::Procedural::TerrainEditGUI);
        ui->add("Procedural", new osgEarth::Procedural::TextureSplattingLayerGUI());
        ui->add("Procedural", new osgEarth::Procedural::VegetationLayerGUI());
        ui->add("Procedural", new osgEarth::Procedural::NodeGraphGUI());
#endif

        if (extras)
        {
            ui->add("Extras", new osgEarth::FeatureEditGUI());
        }

        ui->onStartup = []()
        {
            ImGui::GetIO().FontAllowUserScaling = true;
        };

        // Put it on the front of the list so events don't filter through to other handlers.
        viewer.getEventHandlers().push_front(ui);

        osg::ref_ptr<Contrib::SelectExtentTool> selectTool;
        auto mapNode = MapNode::get(node);
        if (mapNode)
        {
            installSelectTool(viewer, ui, mapNode, selectTool);
        }
        else
        {
            viewer.setCameraManipulator(new osgGA::TrackballManipulator());
        }
        viewer.setSceneData(node);
        if (mapNode)
        {
            // Switching and input updates execute on event traversal, before the active manipulator.
            viewer.addEventHandler(new NavigationModeHandler(
                dynamic_cast<EarthManipulator*>(viewer.getCameraManipulator())));
            viewer.addEventHandler(new GamepadHandler());
            viewer.setRunFrameScheme(osgViewer::ViewerBase::CONTINUOUS);
        }
        // Let the view own the graph so a reload can release the previous scene.
        node = nullptr;
        viewer.realize();
        while (!viewer.done())
        {
            viewer.frame();
            if (reload->reload(viewer))
                installSelectTool(viewer, ui, MapNode::get(viewer.getSceneData()), selectTool);
        }
        return 0;
    }
    else
    {
        return usage(argv[0]);
    }
}
