/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <osgEarthImGui/ImGuiApp>
#include <osgEarth/EarthManipulator>
#include <osgEarth/ExampleResources>
#include <osgViewer/Viewer>

#include <osgEarthImGui/LayersGUI>
#include <osgEarthImGui/ContentBrowserGUI>
#include <osgEarthImGui/NetworkMonitorGUI>
#include <osgEarthImGui/SceneGraphGUI>
#include <osgEarthImGui/TextureInspectorGUI>
#include <osgEarthImGui/ViewpointsGUI>
#include <osgEarthImGui/LiveCamerasGUI>
#include <osgEarthImGui/SystemGUI>
#include <osgEarthImGui/HeapHotspotsGUI>
#include <osgEarthImGui/EnvironmentGUI>
#include <osgEarthImGui/TerrainGUI>
#include <osgEarthImGui/ShaderGUI>
#include <osgEarthImGui/CameraGUI>
#include <osgEarthImGui/RenderingGUI>
#include <osgEarthImGui/AnnotationsGUI>
#include <osgEarthImGui/PickerGUI>
#include <osgEarthImGui/OpenEarthFileGUI>
#include <osgEarthImGui/ResourceLibraryGUI>
#include <osgEarthImGui/DecalsGUI>

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

#include "HeapHotSpots.h"

#define LC "[imgui] "

using namespace osgEarth;
using namespace osgEarth::Util;

int
usage(const char* name)
{
    OE_NOTICE
        << "\nUsage: " << name << " file.earth" << std::endl
        << MapNodeHelper().usage() << std::endl;
    return 0;
}

int
main(int argc, char** argv)
{
    OE_NOTICE << "Installing HeapHotSpots" << std::endl;
    HeapHotspots::install();
    OE_NOTICE << "Installed HeapHotSpots" << std::endl;

    osg::ArgumentParser arguments(&argc, argv);
    if (arguments.read("--help"))
        return usage(argv[0]);

    bool extras = arguments.read("--extras");

    osgEarth::initialize(arguments);

    // Set up the viewer and input handler:
    osgViewer::Viewer viewer(arguments);
    viewer.setThreadingModel(viewer.SingleThreaded);
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
        ui->add("File", new ImGuiDemoWindowGUI());
        ui->add("File", new SeparatorGUI());
        ui->add("File", new QuitGUI());

        ui->add("Tools", new CameraGUI());
        ui->add("Tools", new ContentBrowserGUI());
        ui->add("Tools", new DecalsGUI());
        ui->add("Tools", new EnvironmentGUI());
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
        ui->add("Tools", new HeapHotspotsGUI(
            []()
            {
                return HeapHotspots::capture();
            },
            [](const HeapHotspotReport& report, const std::string& filename)
            {
                return HeapHotspots::write(report, filename.c_str());
            }));
        ui->add("Tools", new TerrainGUI());
        ui->add("Tools", new TextureInspectorGUI());
        ui->add("Tools", new ViewpointsGUI());
        ui->add("Tools", new LiveCamerasGUI());

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

        // Install a select-extent tool that panels can access.
        auto selectTool = new Contrib::SelectExtentTool(MapNode::get(node));
        selectTool->getStyle().getOrCreateSymbol<LineSymbol>()->stroke()->color() = Color::Red;
        selectTool->setModKeyMask(osgGA::GUIEventAdapter::MODKEY_SHIFT);
        selectTool->onSelect([ui](const osgEarth::GeoExtent& extent)
            {
                ui->setSelectedExtent(extent);
            });

        viewer.getEventHandlers().push_front(selectTool);

        viewer.setSceneData(node);
        return viewer.run();
    }
    else
    {
        return usage(argv[0]);
    }
}
