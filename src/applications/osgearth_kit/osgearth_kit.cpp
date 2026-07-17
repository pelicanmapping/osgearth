/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/EarthManipulator>
#include <osgEarth/ExampleResources>
#include <osgEarth/GLUtils>
#include <osgEarth/GeoData>
#include <osgEarth/Kit>
#include <osgEarth/LogarithmicDepthBuffer>
#include <osgEarth/Map>
#include <osgEarth/MapNode>
#include <osgEarth/Notify>
#include <osgEarth/PagedNode>
#include <osgEarth/Profile>
#include <osgEarth/Registry>
#include <osgEarth/SimplePager>
#include <osgEarth/Sky>
#include <osgEarth/SelectExtentTool>
#include <osgEarth/XYZ>
#include <osgEarth/MetadataNode>

#include <osgEarthImGui/ImGuiApp>
#include <osgEarthImGui/AnnotationsGUI>
#include <osgEarthImGui/CameraGUI>
#include <osgEarthImGui/ContentBrowserGUI>
#include <osgEarthImGui/DecalsGUI>
#include <osgEarthImGui/EnvironmentGUI>
#include <osgEarthImGui/FeatureEditGUI>
#include <osgEarthImGui/LayersGUI>
#include <osgEarthImGui/LiveCamerasGUI>
#include <osgEarthImGui/NetworkMonitorGUI>
#include <osgEarthImGui/PickerGUI>
#include <osgEarthImGui/RenderingGUI>
#include <osgEarthImGui/ResourceLibraryGUI>
#include <osgEarthImGui/SceneGraphGUI>
#include <osgEarthImGui/ShaderGUI>
#include <osgEarthImGui/SystemGUI>
#include <osgEarthImGui/TerrainGUI>
#include <osgEarthImGui/TextureInspectorGUI>
#include <osgEarthImGui/ViewpointsGUI>

#ifdef OSGEARTH_HAVE_OPEN_EARTH_FILE_GUI
#include <osgEarthImGui/OpenEarthFileGUI>
#endif

#ifdef OSGEARTH_HAVE_GEOCODER
#include <osgEarthImGui/SearchGUI>
#endif

#ifdef OSGEARTH_HAVE_PROCEDURAL_NODEKIT
#include <osgEarthImGui/LifeMapLayerGUI>
#include <osgEarthImGui/NodeGraphGUI>
#include <osgEarthImGui/TerrainEditGUI>
#include <osgEarthImGui/TextureSplattingLayerGUI>
#include <osgEarthImGui/VegetationLayerGUI>
#endif

#ifdef OSGEARTH_HAVE_CESIUM_NODEKIT
#include <osgEarthImGui/CesiumIonGUI>
#endif
#include <osgEarth/TMS>

#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osgDB/Options>
#include <osgDB/ReadFile>
#include <osgDB/Registry>
#include <osgViewer/Viewer>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace osgEarth;
using namespace osgEarth::Util;
namespace fs = std::filesystem;

#define LC "[osgearth_kit] "

namespace
{
    constexpr unsigned CITY_LEVEL = 14u;
    constexpr double PROTOTYPE_SIZE_METERS = 2445.98490512564;

    struct CityPrototype
    {
        std::string source;
        std::string impostorSource;
    };

    struct LoadedCity
    {
        osg::ref_ptr<osg::Group> batches;
        osg::ref_ptr<osg::Node> impostor;
        Kit::BuildStats stats;
        double loadMilliseconds = 0.0;
        double buildMilliseconds = 0.0;
        double impostorLoadMilliseconds = 0.0;
    };

    bool loadCity(
        const CityPrototype& city,
        Kit* kit,
        bool impostorsOnly,
        LoadedCity& loaded)
    {
        loaded = LoadedCity();

        // Explicitly bypass osgDB's object cache. Every tile must own the
        // complete city graph so compile visitors and GL-object setup never
        // touch a graph that another tile is traversing.
        osg::ref_ptr<osgDB::Options> options = new osgDB::Options();
        options->setObjectCacheHint(osgDB::Options::CACHE_NONE);

        if (!impostorsOnly)
        {
            if (!kit)
                return false;

            const auto loadStart = std::chrono::steady_clock::now();
            osg::ref_ptr<osg::Node> lightweight =
                osgDB::readRefNodeFile(city.source, options.get());
            const auto loadEnd = std::chrono::steady_clock::now();
            if (!lightweight.valid())
            {
                OE_WARN << LC << "Failed to load " << city.source << std::endl;
                return false;
            }

            loaded.loadMilliseconds = std::chrono::duration<double, std::milli>(
                loadEnd - loadStart).count();
            const auto buildStart = std::chrono::steady_clock::now();
            loaded.batches = kit->createInstancedNode(lightweight.get(), &loaded.stats);
            const auto buildEnd = std::chrono::steady_clock::now();
            loaded.buildMilliseconds = std::chrono::duration<double, std::milli>(
                buildEnd - buildStart).count();

            // The parsed KitNode graph is only an input to batch compilation.
            // Release its source positions before loading the resident impostor so
            // a tile does not carry both transient inputs at once.
            lightweight = nullptr;

            if (!loaded.batches.valid() || loaded.batches->getNumChildren() == 0u ||
                loaded.stats.missingModels != 0u)
            {
                OE_WARN << LC << "Failed to compile " << city.source << std::endl;
                return false;
            }
        }

        const auto impostorStart = std::chrono::steady_clock::now();
        loaded.impostor = osgDB::readRefNodeFile(city.impostorSource, options.get());
        const auto impostorEnd = std::chrono::steady_clock::now();
        loaded.impostorLoadMilliseconds = std::chrono::duration<double, std::milli>(
            impostorEnd - impostorStart).count();

        if (!loaded.impostor.valid())
        {
            OE_WARN << LC << "Failed to load " << city.impostorSource << std::endl;
            return false;
        }

        Registry::shaderGenerator().run(loaded.impostor.get());
        return true;
    }

    class KitPager : public SimplePager
    {
    public:
        KitPager(
            const Map* map,
            std::vector<CityPrototype> cities,
            Kit* kit,
            float highDetailRange,
            bool impostorsOnly) :
            SimplePager(map, Profile::create(Profile::GLOBAL_MERCATOR)),
            _cities(std::move(cities)),
            _kit(kit),
            _highDetailRange(highDetailRange),
            _impostorsOnly(impostorsOnly),
            _worldSRS(map ? map->getWorldSRS() : nullptr)
        {
            setName("Level 14 Kit city pager");
            setMinLevel(CITY_LEVEL);
            setMaxLevel(CITY_LEVEL);
            setAdditive(false);
            setRangeFactor(12.0f);
            setUsePayloadBoundsForChildren(false);
            setCreatePagedNodeFunction([](const TileKey&)
            {
                osg::ref_ptr<PagedNode2> node = new PagedNode2();
                // Each tile owns large, previously unseen instance VBOs. Do
                // not hold the whole four-child result out of the scene while
                // an asynchronous compile visitor uploads all of them.
                node->setPreCompileGLObjects(false);
                return node.release();
            });
        }

        unsigned getCreatedTileCount() const
        {
            return _createdTiles.load(std::memory_order_relaxed);
        }

        osg::ref_ptr<osg::Node> createNode(
            const TileKey& key,
            ProgressCallback* progress) override
        {
            if (key.getLOD() != CITY_LEVEL || _cities.empty() || (progress && progress->canceled()))
                return {};

            const std::size_t index =
                (static_cast<std::size_t>(key.getTileX()) * 73856093u ^
                 static_cast<std::size_t>(key.getTileY()) * 19349663u) % _cities.size();
            const CityPrototype& city = _cities[index];

            const GeoExtent extent = key.getExtent();
            const double centerX = 0.5 * (extent.xMin() + extent.xMax());
            const double centerY = 0.5 * (extent.yMin() + extent.yMax());
            const SpatialReference* srs = extent.getSRS();

            if (!_worldSRS.valid())
                return {};

            GeoPoint center(srs, centerX, centerY, 1.5, ALTMODE_ABSOLUTE);
            GeoPoint west(srs, extent.xMin(), centerY, 1.5, ALTMODE_ABSOLUTE);
            GeoPoint east(srs, extent.xMax(), centerY, 1.5, ALTMODE_ABSOLUTE);
            GeoPoint south(srs, centerX, extent.yMin(), 1.5, ALTMODE_ABSOLUTE);
            GeoPoint north(srs, centerX, extent.yMax(), 1.5, ALTMODE_ABSOLUTE);

            // Tile keys are Mercator, but the map's world can be geocentric
            // (the default) or projected. Build the placement in that actual
            // world SRS; using the Mercator point directly would put a city at
            // planar X/Y coordinates even when the terrain is a globe.
            const GeoPoint centerWorld = center.transform(_worldSRS.get());
            const GeoPoint westWorld = west.transform(_worldSRS.get());
            const GeoPoint eastWorld = east.transform(_worldSRS.get());
            const GeoPoint southWorld = south.transform(_worldSRS.get());
            const GeoPoint northWorld = north.transform(_worldSRS.get());
            if (!centerWorld.isValid() || !westWorld.isValid() || !eastWorld.isValid() ||
                !southWorld.isValid() || !northWorld.isValid())
            {
                return {};
            }

            osg::Matrixd localToWorld;
            if (!centerWorld.createLocalToWorld(localToWorld))
            {
                return {};
            }

            // The on-disk prototypes occupy one equatorial level-14 tile.
            // Rescale them to this tile's world-space footprint, then place the
            // result in its local east/north/up frame (or projected XY frame).
            const double scaleX =
                (eastWorld.vec3d() - westWorld.vec3d()).length() / PROTOTYPE_SIZE_METERS;
            const double scaleY =
                (northWorld.vec3d() - southWorld.vec3d()).length() / PROTOTYPE_SIZE_METERS;
            osg::ref_ptr<osg::MatrixTransform> placement = new osg::MatrixTransform(
                osg::Matrixd::scale(scaleX, scaleY, 1.0) * localToWorld);
            placement->setName(key.str() + " Kit city");

            LoadedCity loaded;
            {
                // Kit model prototypes and osgDB's global registries are shared
                // construction inputs. Serialize tile construction, while still
                // giving every returned tile independent nodes, arrays, states,
                // primitive sets, and impostor geometry.
                //std::lock_guard<std::mutex> lock(_loadMutex);
                if ((progress && progress->canceled()) ||
                    !loadCity(city, _kit.get(), _impostorsOnly, loaded))
                {
                    return {};
                }
            }

            if (_impostorsOnly)
            {
                // Diagnostic mode: the binary city and Kit models were never
                // loaded, so there is no hidden high-detail graph to destroy.
                placement->addChild(loaded.impostor.get());
            }
            else
            {
                // Both children remain resident. LOD only prevents traversal and
                // drawing of the expensive instance batches beyond this range.
                osg::ref_ptr<osg::LOD> lod = new osg::LOD();
                lod->setName(key.str() + " Kit city LOD");
                lod->setRangeMode(osg::LOD::DISTANCE_FROM_EYE_POINT);
                lod->addChild(loaded.batches.get(), 0.0f, _highDetailRange);
                lod->addChild(
                    loaded.impostor.get(), _highDetailRange,
                    std::numeric_limits<float>::max());
                placement->addChild(lod.get());
            }

            _createdTiles.fetch_add(1u, std::memory_order_relaxed);

            if (!_reported.exchange(true))
            {
                if (_impostorsOnly)
                {
                    OE_NOTICE << LC << "Paged " << key.str()
                        << " in impostors-only mode from "
                        << fs::path(city.impostorSource).filename().string()
                        << "; load " << loaded.impostorLoadMilliseconds << " ms"
                        << std::endl;
                }
                else
                {
                    OE_NOTICE << LC
                        << "Paged " << key.str() << " from " << fs::path(city.source).filename().string()
                        << ": " << loaded.stats.instances << " instances in " << loaded.stats.batches
                        << " model batches / " << loaded.stats.drawables
                        << " tile-owned instanced drawables; load "
                        << loaded.loadMilliseconds << " ms, batch "
                        << loaded.buildMilliseconds << " ms, impostor "
                        << loaded.impostorLoadMilliseconds << " ms; impostor beyond "
                        << _highDetailRange << " m" << std::endl;
                }
            }

            return placement.release();
        }

    private:
        std::vector<CityPrototype> _cities;
        osg::ref_ptr<Kit> _kit;
        float _highDetailRange;
        bool _impostorsOnly;
        osg::ref_ptr<const SpatialReference> _worldSRS;
        std::mutex _loadMutex;
        std::atomic<unsigned> _createdTiles{ 0u };
        std::atomic<bool> _reported{ false };
    };

    int usage(const char* name, const char* error = nullptr)
    {
        if (error)
            std::cerr << "Error: " << error << "\n\n";
        std::cerr
            << "Usage: " << name << " [options]\n"
            << "  --kit <file>       Kit manifest (default data/kit/buildings.kit)\n"
            << "  --city-dir <dir>   Directory containing .kitcityb/.kitcity files\n"
            << "  --earth <file>     Optional base .earth file\n"
            << "  --no-imagery       Use an empty globe instead of OpenStreetMap\n"
            << "  --validate-only    Load and batch one city without opening a viewer\n"
            << "  --direct-tile      Load the startup tile directly (diagnostic)\n"
            << "  --frames <count>   Render a fixed number of frames, then exit (smoke test)\n"
            << "  --stress-paging    Move across adjacent L14 tiles during a fixed-frame test\n"
            << "  --impostors-only   Load and display only low-poly city impostors\n"
            << "  --lod-range <m>    High-detail instance range (default 3000 metres)\n"
            << "  --chunk-size <m>   Spatial Kit batch chunk size, 0 disables (default 512 metres)\n"
            << "  --sky              Use the default sky quality preset\n"
            << "  --sky-low          Use the low-quality sky preset\n"
            << "  --sky-medium       Use the medium-quality sky preset\n"
            << "  --sky-high         Use the high-quality sky preset\n"
            << "  --sky-best         Use the best-quality sky preset\n"
            << "  --extras           Add experimental ImGui tools\n"
            << "  --nogui            Hide the ImGui interface at startup\n";
        return error ? 1 : 0;
    }

    void installGUI(
        osg::ArgumentParser& arguments,
        osgViewer::Viewer& viewer,
        MapNode* mapNode,
        bool extras)
    {
        auto* ui = new ImGuiAppEngine(arguments);

#ifdef OSGEARTH_HAVE_OPEN_EARTH_FILE_GUI
        ui->add("File", new OpenEarthFileGUI());
#endif
        ui->add("File", new ImGuiDemoWindowGUI());
        ui->add("File", new SeparatorGUI());
        ui->add("File", new QuitGUI());

        ui->add("Tools", new CameraGUI());
        ui->add("Tools", new ContentBrowserGUI());
        ui->add("Tools", new DecalsGUI());
        ui->add("Tools", new EnvironmentGUI(), true);
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

#ifdef OSGEARTH_HAVE_CESIUM_NODEKIT
        ui->add("Cesium", new osgEarth::Cesium::CesiumIonGUI());
#endif

#ifdef OSGEARTH_HAVE_PROCEDURAL_NODEKIT
        ui->add("Procedural", new osgEarth::Procedural::LifeMapLayerGUI());
        ui->add("Procedural", new osgEarth::Procedural::TerrainEditGUI());
        ui->add("Procedural", new osgEarth::Procedural::TextureSplattingLayerGUI());
        ui->add("Procedural", new osgEarth::Procedural::VegetationLayerGUI());
        ui->add("Procedural", new osgEarth::Procedural::NodeGraphGUI());
#endif

        if (extras)
            ui->add("Extras", new FeatureEditGUI());

        ui->onStartup = []()
        {
            ImGui::GetIO().FontAllowUserScaling = true;
        };

        // Put ImGui first so mouse and keyboard events captured by a panel do
        // not fall through to the earth manipulator.
        viewer.getEventHandlers().push_front(ui);

        // Match osgearth_imgui's shared extent-selection facility.
        auto* selectTool = new Contrib::SelectExtentTool(mapNode);
        selectTool->getStyle().getOrCreateSymbol<LineSymbol>()->stroke()->color() = Color::Red;
        selectTool->setModKeyMask(osgGA::GUIEventAdapter::MODKEY_SHIFT);
        selectTool->onSelect([ui](const GeoExtent& extent)
        {
            ui->setSelectedExtent(extent);
        });
        viewer.getEventHandlers().push_front(selectTool);

    }

    std::vector<std::string> findCities(const std::string& directory)
    {
        // Keep text fixtures available for inspection, but select the binary
        // sibling whenever both formats exist for a prototype.
        std::map<std::string, std::string> selected;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(directory, error))
        {
            if (!entry.is_regular_file())
                continue;
            const std::string extension = entry.path().extension().string();
            const std::string stem = entry.path().stem().string();
            if (extension == ".kitcityb")
                selected[stem] = entry.path().string();
            else if (extension == ".kitcity" && selected.find(stem) == selected.end())
                selected[stem] = entry.path().string();
        }
        std::vector<std::string> result;
        result.reserve(selected.size());
        for (const auto& entry : selected)
            result.push_back(entry.second);
        return result;
    }

    std::vector<CityPrototype> collectCities(const std::vector<std::string>& files)
    {
        std::vector<CityPrototype> result;
        result.reserve(files.size());
        for (const auto& file : files)
        {
            const fs::path sourcePath(file);
            const fs::path impostorPath = sourcePath.parent_path() /
                (sourcePath.stem().string() + "_impostor.osgb");
            if (!fs::is_regular_file(impostorPath))
            {
                OE_WARN << LC << "Ignoring " << file << " because "
                    << impostorPath.string() << " is missing" << std::endl;
                continue;
            }

            CityPrototype city;
            city.source = file;
            city.impostorSource = impostorPath.string();
            result.push_back(std::move(city));
        }
        return result;
    }
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc, argv);
    osgEarth::initialize(arguments);

    if (arguments.read("--help") || arguments.read("-h"))
        return usage(argv[0]);

    std::string kitFile = "data/kit/buildings.kit";
    std::string cityDirectory = "data/kit/cities";
    std::string earthFile;
    int frameLimit = 0;
    double highDetailRange = 3000.0;
    double instanceChunkSize = 512.0;
    arguments.read("--kit", kitFile);
    arguments.read("--city-dir", cityDirectory);
    arguments.read("--earth", earthFile);
    arguments.read("--frames", frameLimit);
    arguments.read("--lod-range", highDetailRange);
    arguments.read("--chunk-size", instanceChunkSize);
    if (highDetailRange <= 0.0)
        return usage(argv[0], "--lod-range must be greater than zero");
    if (instanceChunkSize < 0.0)
        return usage(argv[0], "--chunk-size must be zero or greater");
    const bool noImagery = arguments.read("--no-imagery");
    const bool validateOnly = arguments.read("--validate-only");
    const bool stressPaging = arguments.read("--stress-paging");
    bool impostorsOnly = arguments.read("--impostors-only");
    // Accept the common alternate spelling without leaving an unconsumed
    // command-line argument when both spellings are present.
    impostorsOnly = arguments.read("--imposters-only") || impostorsOnly;
    const bool extras = arguments.read("--extras");
    SkyOptions skyOptions;
    const SkyOptions::Quality skyQuality = SkyOptions::parseQuality(arguments);
    if (skyQuality != SkyOptions::QUALITY_UNSET)
        skyOptions.quality() = skyQuality;
    if (stressPaging && frameLimit <= 0)
        frameLimit = 1500;
    const bool directTile =
        arguments.read("--direct-tile") || (frameLimit > 0 && !stressPaging);

    osg::ref_ptr<Kit> kit;
    if (!impostorsOnly)
    {
        kit = new Kit();
        if (!kit->load(kitFile))
            return usage(argv[0], kit->getLastError().c_str());
        kit->setInstanceChunkSize(static_cast<float>(instanceChunkSize));
    }

    const std::vector<std::string> cityFiles = findCities(cityDirectory);
    if (cityFiles.empty())
        return usage(argv[0], ("No Kit city files found in " + cityDirectory).c_str());
    const std::vector<CityPrototype> cities = collectCities(cityFiles);
    if (cities.empty())
        return usage(argv[0], "No Kit city prototypes have matching impostors");

    if (impostorsOnly)
    {
        OE_NOTICE << LC << "Impostors-only mode: skipped Kit model and city instance loading; found "
            << cities.size() << " low-poly city prototypes" << std::endl;
    }
    else
    {
        OE_NOTICE << LC << "Loaded " << kit->getNumModels()
            << " named models and found " << cities.size()
            << " tile-owned city prototypes; Kit chunk size "
            << kit->getInstanceChunkSize() << " m" << std::endl;
    }

    if (validateOnly)
    {
        LoadedCity loaded;
        if (!loadCity(cities.front(), kit.get(), impostorsOnly, loaded))
            return usage(argv[0], "Failed to load the validation city");
        if (impostorsOnly)
        {
            std::cout << "Validated an impostor-only city; Kit models and instance geometry were not loaded\n";
            return 0;
        }
        std::cout << "Validated " << loaded.stats.instances << " instances, "
            << loaded.stats.batches << " named-model batches, " << loaded.stats.drawables
            << " instanced drawables, " << loaded.stats.missingModels
            << " missing models, and a resident city impostor; LOD range "
            << highDetailRange << " m, chunk size "
            << kit->getInstanceChunkSize() << " m\n";
        return loaded.stats.instances > 0u && loaded.stats.missingModels == 0u ? 0 : 2;
    }

    osg::ref_ptr<osg::Group> root = new osg::Group();
    osg::ref_ptr<MapNode> mapNode;

    if (!earthFile.empty())
    {
        osg::ref_ptr<osg::Node> earth = osgDB::readRefNodeFile(earthFile);
        mapNode = MapNode::get(earth);
        if (!mapNode.valid())
            return usage(argv[0], "The --earth file did not contain a MapNode");
        root->addChild(earth.get());
    }
    else
    {
        osg::ref_ptr<Map> map = new Map();
        if (!noImagery)
        {
#if 0
            osg::ref_ptr<XYZImageLayer> osm = new XYZImageLayer();
            osm->setName("OpenStreetMap");
            osm->setURL("https://tile.openstreetmap.org/{z}/{x}/{y}.png");
            osm->setProfile(Profile::create(Profile::SPHERICAL_MERCATOR));
            map->addLayer(osm.get());
#else
            TMSImageLayer* imagery = new TMSImageLayer();
            imagery->setURL("http://readymap.org/readymap/tiles/1.0.0/22/");
            map->addLayer(imagery);
#endif
        }
        mapNode = new MapNode(map.get());
        root->addChild(mapNode.get());
    }

    osg::ref_ptr<KitPager> pager = new KitPager(
        mapNode->getMap(), cities, kit.get(), static_cast<float>(highDetailRange),
        impostorsOnly);
    if (directTile)
    {
        GeoPoint startup(SpatialReference::get("wgs84"), -74.0060, 40.7128, 0.0, ALTMODE_ABSOLUTE);
        GeoPoint mercator = startup.transform(pager->getProfile()->getSRS());
        const TileKey key = pager->getProfile()->createTileKey(mercator.x(), mercator.y(), CITY_LEVEL);
        osg::ref_ptr<osg::Node> node = pager->createNode(key, nullptr);
        if (!node.valid())
            return usage(argv[0], "Failed to create the startup level-14 tile");
        root->addChild(node.get());
    }
    else
    {
        pager->build();
        root->addChild(pager.get());
    }

    osgViewer::Viewer viewer(arguments);
    // ImGui and its panels share state between the event, update, and draw
    // traversals, so use the same threading model as osgearth_imgui.
    viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);

    // The simple sky's default moon texture ships in osgEarth's data folder.
    // This example already resolves its default Kit assets relative to the
    // repository root, so make the sibling sky assets discoverable as well.
    const fs::path dataPath = fs::absolute(fs::path(kitFile).parent_path().parent_path());
    if (fs::is_directory(dataPath))
    {
        auto& searchPaths = osgDB::Registry::instance()->getDataFilePathList();
        const std::string dataPathString = dataPath.string();
        if (std::find(searchPaths.begin(), searchPaths.end(), dataPathString) == searchPaths.end())
            searchPaths.push_back(dataPathString);
    }

    osg::ref_ptr<SkyNode> sky = SkyNode::create(skyOptions);
    if (!sky.valid())
        return usage(argv[0], "Failed to create the osgEarth SkyNode");
    sky->setName("Kit city sky");
    sky->addChild(root.get());
    sky->attach(&viewer);

    viewer.setSceneData(sky.get());
    viewer.getCamera()->setSmallFeatureCullingPixelSize(-1.0f);

    osg::ref_ptr<EarthManipulator> manipulator = new EarthManipulator(arguments);
    viewer.setCameraManipulator(manipulator.get());
    manipulator->setViewpoint(Viewpoint(
        "Kit city",
        -74.0060, 40.7128, 0.0,
        -28.0, -48.0, 2100.0),
        0.0);

    LogarithmicDepthBuffer depthBuffer;
    depthBuffer.install(viewer.getCamera());

#ifndef OSG_GL3_AVAILABLE
    viewer.setRealizeOperation(new GL3RealizeOperation());
#endif

    MapNodeHelper().configureView(&viewer);
    installGUI(arguments, viewer, mapNode.get(), extras);
    if (frameLimit > 0)
    {
        viewer.realize();
        viewer.setDone(false);
        for (int frame = 0; frame < frameLimit; ++frame)
        {
            if (stressPaging && frame % 300 == 0)
            {
                static const std::array<std::pair<double, double>, 5> offsets = {{
                    { 0.000, 0.000 },
                    { 0.026, 0.000 },
                    { 0.052, 0.018 },
                    { 0.026, 0.036 },
                    { 0.000, 0.018 }
                }};
                const auto& offset = offsets[(frame / 300) % offsets.size()];
                manipulator->setViewpoint(Viewpoint(
                    "Kit paging stress",
                    -74.0060 + offset.first, 40.7128 + offset.second, 0.0,
                    -28.0, -48.0, 2100.0), 0.0);
            }
            viewer.frame();
            if (stressPaging)
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        if (stressPaging)
            std::cout << "Stress-paged " << pager->getCreatedTileCount()
                << " level-14 Kit tiles\n";
        return 0;
    }
    return viewer.run();
}
