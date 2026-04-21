/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#define LC "[osgearth_conv] "

#include <osgEarth/Notify>
#include <osgEarth/Profile>
#include <osgEarth/TileSource>
#include <osgEarth/TileHandler>
#include <osgEarth/TileVisitor>
#include <osgEarth/ImageLayer>
#include <osgEarth/ElevationLayer>
#include <osgEarth/MapNode>
#include <osgEarth/OGRFeatureSource>
#include <osgEarth/ImageUtils>
#include <osgEarth/TileEstimator>
#include <osgEarth/GLUtils>
#include <osgEarth/TileRasterizer>
#include <osgEarth/NodeUtils>
#include <osgEarth/weejobs.h>

#include <osg/ArgumentParser>
#include <osg/Timer>
#include <osgDB/ReadFile>

#include <osgViewer/Viewer>

#include <iomanip>

using namespace osgEarth;

// documentation
int usage(char** argv)
{
    std::cout
        << "Converts tiles from one format to another.\n\n"
        << argv[0]
        << "\n    --in [prop_name] [prop_value]       : set an input property (instead of using --in-earth)"
        << "\n    --in-earth [earthfile]              : earth file from which to load input layer (instead of using --in)"
        << "\n    --in-layer [layer name]             : with --in-earth, name of layer to convert"
        << "\n    --out [prop_name] [prop_value]      : set an output property"
        << "\n    --profile [profile def]             : set an output profile (optional; default = same as input)"
        << "\n    --min-level [int]                   : minimum level of detail"
        << "\n    --max-level [int]                   : maximum level of detail"
        << "\n    --osg-options [OSG options string]  : options to pass to OSG readers/writers"
        << "\n    --extents [minLat] [minLong] [maxLat] [maxLong] : Lat/Long extends to copy"
        << "\n    --no-overwrite                      : skip tiles that already exist in the destination"
        << "\n    --threads [int]                     : go faster by using [n] working threads"
        << "\n    --threaded-writer                   : write to the output layer in a separate thread (good for MBTiles)"
        << std::endl;

    return 0;
}

// Visitor that converts image tiles
struct ImageLayerTileCopy : public TileHandler
{
    ImageLayerTileCopy(ImageLayer* source, ImageLayer* dest, bool overwrite, bool compress, bool threaded_writer)
        : _source(source), _dest(dest), _overwrite(overwrite), _compress(compress), _threaded_writer(threaded_writer)
    {
        if (_threaded_writer)
        {
            _write_context.pool = jobs::get_pool("image_writer", 1);
            _write_context.group = jobs::jobgroup::create();
        }
    }

    ~ImageLayerTileCopy()
    {
        if (_threaded_writer)
        {
            std::cout << "Stand by while I finish writing..." << std::endl;
            _write_context.group->join();
        }
    }

    bool handleTile(const TileKey& key, const TileVisitor& tv) override
    {
        bool ok = false;

        // if overwriting is disabled, check to see whether the destination
        // already has data for the key
        if (_overwrite == false)
        {
            if (_dest->createImage(key).valid())
            {
                return true;
            }
        }

        GeoImage image = _source->createImage(key);
        if (image.valid())
        {
            osg::ref_ptr<const osg::Image> imageToWrite = image.getImage();
            if (_compress)
                imageToWrite = ImageUtils::compressImage(image.getImage(), "cpu");

            if (_threaded_writer)
            {
#if 1
                if (_dest->isEncodingSupported())
                {
                    auto encoded = _dest->encodeImage(key, imageToWrite.get(), nullptr);
                    if (encoded.isOK())
                        imageToWrite = encoded.value();
                }
#endif

                jobs::dispatch([=]()
                    {
                        Status status = _dest->writeImage(key, imageToWrite.get(), 0L);
                    },
                    _write_context);
            }
            else
            {
                Status status = _dest->writeImage(key, imageToWrite.get(), 0L);
                ok = status.isOK();
                if (!ok)
                {
                    OE_WARN << key.str() << ": " << status.message() << std::endl;
                }
            }
        }

        return ok;
    }

    bool hasData(const TileKey& key) const override
    {
        return _source->mayHaveData(key);
    }

    unsigned getEstimatedTileCount(
        const std::vector<GeoExtent>& extents,
        unsigned minLevel,
        unsigned maxLevel) const override
    {
        TileEstimator e;
        e.setMinLevel(minLevel);
        e.setMaxLevel(maxLevel);
        e.setProfile(_dest->getProfile());
        for (auto& src_extent : extents)
        {
            auto dest_extent = _dest->getProfile()->clampAndTransformExtent(src_extent);
            e.addExtent(dest_extent);
        }
        return e.getNumTiles();
    }

    osg::ref_ptr<ImageLayer> _source;
    osg::ref_ptr<ImageLayer> _dest;
    bool _overwrite;
    bool _compress;  
    bool _threaded_writer = false;
    jobs::context _write_context;

};

// Visitor that converts elevation tiles
struct ElevationLayerTileCopy : public TileHandler
{
    ElevationLayerTileCopy(ElevationLayer* source, ElevationLayer* dest, bool overwrite, bool threaded_writer)
        : _source(source), _dest(dest), _overwrite(overwrite), _threaded_writer(threaded_writer)
    {
        if (_threaded_writer)
        {
            _write_context.pool = jobs::get_pool("image_writer", 1);
            _write_context.group = jobs::jobgroup::create();
        }
    }

    ~ElevationLayerTileCopy()
    {
        if (_threaded_writer)
        {
            std::cout << "Stand by while I finish writing..." << std::endl;
            _write_context.group->join();
        }
    }

    bool handleTile(const TileKey& key, const TileVisitor& tv) override
    {
        bool ok = false;

        // if overwriting is disabled, check to see whether the destination
        // already has data for the key
        if (_overwrite == false)
        {
            if (_dest->createHeightField(key).valid())
            {
                return true;
            }
        }

        GeoHeightField hf = _source->createHeightField(key, 0L);
        if ( hf.valid() )
        {
            if (_threaded_writer)
            {
                jobs::dispatch([=]()
                    {
                        Status s = _dest->writeHeightField(key, hf.getHeightField(), 0L);
                    },
                    _write_context);
            }
            else
            {
                Status s = _dest->writeHeightField(key, hf.getHeightField(), 0L);
                ok = s.isOK();
                if (!ok)
                {
                    OE_WARN << key.str() << ": " << s.message() << std::endl;
                }
            }
        }
        else
        {
            //OE_WARN << key.str() << " : " << hf.getStatus().message() << std::endl;
        }
        return ok;
    }

    bool hasData(const TileKey& key) const override
    {
        return _source->mayHaveData(key);
    }

    unsigned getEstimatedTileCount(
        const std::vector<GeoExtent>& extents,
        unsigned minLevel,
        unsigned maxLevel) const override
    {
        TileEstimator e;
        e.setMinLevel(minLevel);
        e.setMaxLevel(maxLevel);
        e.setProfile(_dest->getProfile());
        for (auto& src_extent : extents)
        {
            auto dest_extent = _dest->getProfile()->clampAndTransformExtent(src_extent);
            e.addExtent(dest_extent);
        }
        return e.getNumTiles();
    }

    osg::ref_ptr<ElevationLayer> _source;
    osg::ref_ptr<ElevationLayer> _dest;
    bool _overwrite;
    bool _threaded_writer = false;
    jobs::context _write_context;
};


// Custom progress reporter
struct ProgressReporter : public osgEarth::ProgressCallback
{
    ProgressReporter() : _first(true), _start(0) { }

    bool reportProgress(double             current,
                        double             total,
                        unsigned           currentStage,
                        unsigned           totalStages,
                        const std::string& msg )
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_first)
        {
            _first = false;
            _start = osg::Timer::instance()->tick();
        }
        osg::Timer_t now = osg::Timer::instance()->tick();

        if (total > 0.0f)
        {
            float percentage = current / total;

            double timeSoFar = osg::Timer::instance()->delta_s(_start, now);
            double projectedTotalTime = timeSoFar / percentage;
            double timeToGo = projectedTotalTime - timeSoFar;
            
            constexpr double secondsPerMinute = 60.0;
            constexpr double secondsPerHour = secondsPerMinute * 60.0;
            constexpr double secondsPerDay = secondsPerHour * 24.0;
            
            double timeToGoRemainder = timeToGo;
            double daysToGo = timeToGoRemainder / secondsPerDay;
            timeToGoRemainder = fmod(timeToGoRemainder, secondsPerDay);
            double hoursToGo = timeToGoRemainder / secondsPerHour;
            timeToGoRemainder = fmod(timeToGoRemainder, secondsPerHour);
            double minsToGo = timeToGoRemainder / secondsPerMinute;
            double secsToGo = fmod(timeToGoRemainder, secondsPerMinute);
            
            double projectedTotalTimeRemainder = projectedTotalTime;
            double daysTotal = projectedTotalTimeRemainder / secondsPerDay;
            projectedTotalTimeRemainder = fmod(projectedTotalTimeRemainder, secondsPerDay);
            double hoursTotal = projectedTotalTimeRemainder / secondsPerHour;
            projectedTotalTimeRemainder = fmod(projectedTotalTimeRemainder, secondsPerHour);
            double minsTotal = projectedTotalTimeRemainder / secondsPerMinute;
            double secsTotal = fmod(projectedTotalTimeRemainder, secondsPerMinute);
            
            std::cout
                << std::fixed
                << std::setprecision(1) << "\r"
                << (int)current << "/" << (int)total
                << " " << int(100.0f*percentage) << "% complete, "
                << (int)daysTotal << "d" << (int)hoursTotal << "h" << (int)minsTotal << "m" << (int)secsTotal << "s projected, "
                << (int)daysToGo << "d" << (int)hoursToGo << "h" << (int)minsToGo << "m" << (int)secsToGo << "s remaining          "
                << std::flush;

            if (percentage >= 100.0f)
                std::cout << std::endl;
        }
        else
        {

            double timeSoFar = osg::Timer::instance()->delta_s(_start, now);

            std::cout
                << std::fixed
                << std::setprecision(1) << "\r"
                << (int)current << "/" << (int)total
                << " " << timeSoFar << "s elapsed"
                << std::flush;
        }

        return false;
    }

    std::mutex _mutex;
    bool _first;
    osg::Timer_t _start;
};


/**
 * Command-line tool that copies the contents of one TileSource
 * to another. All arguments are Config name/value pairs, so you need
 * to look in the header file for each driver's Options structure for
 * options :)
 *
 * Example #2: copy a GDAL file to an MBTiles repo using --in:
 *
 *   osgearth_conv
 *      --in driver gdalimage
 *      --in url world.tif
 *      --out driver mbtilesimage
 *      --out format image/png
 *      --out filename world.mbtiles
 *
 *   The "in" properties come from the GDALOptions getConfig method. The
 *   "out" properties come from the MBTilesOptions getConfig method.
 *
 * Example #2: copy an earth file layer to an MBTiles repo using --in-earth:
 *
 *   osgearth_conv
 *      --in-earth myfile.earth
 *      --in-layer my_image_layer_name
 *      --out driver mbtilesimage
 *      --out format image/png
 *      --out filename world.mbtiles
 *
 * Other arguments:
 *
 *      --profile [profile]   : reproject to the target profile, e.g. "wgs84"
 *      --min-level [int]     : min level of detail to copy
 *      --max-level [int]     : max level of detail to copy
 *      --extents [minLat] [minLong] [maxLat] [maxLong] : Lat/Long extends to copy (*)
 *      --no-overwrite        : don't overwrite data that already exists
 *      --threads [int]       : number of threads to launch
 *
 * OSG arguments:
 *
 *      -O <string>           : OSG Options string (plugin options)
 *
 * Of course, the output layer must support writing.
 * Of course, the output layer must support writing.
 */
int
main(int argc, char** argv)
{
    osg::ArgumentParser args(&argc, argv);

    if (argc == 1)
        return usage(argv);

    osgDB::readCommandLine(args);

    if (args.read("--pause"))
    {
        std::cout << "Press enter to continue" << std::endl;
        getchar();
    }

    osgEarth::initialize();

    // plugin options, if the user passed them in:
    osg::ref_ptr<osgDB::Options> dbo = new osgDB::Options();
    std::string str;
    while (args.read("--osg-options", str) || args.read("-O", str))
    {
        dbo->setOptionString(str);
    }

    typedef std::unordered_map<std::string, std::string> KeyValue;
    std::string key, value;

    // There are two paths. Either the user defines a source layer on
    // the command line using "--in" options, or the user specifies
    // an earth file and layer name.
    osg::ref_ptr<TileLayer> input;
    Config inConf;

    // earth file option:
    std::string earthFile;
    osg::ref_ptr<MapNode> mapNode;
    osg::ref_ptr<const Map> map;
    if (args.read("--in-earth", earthFile))
    {
        std::string layerName;
        if (!args.read("--in-layer", layerName))
        {
            OE_WARN << "Missing required argument --in-layer" << std::endl;
            return -1;
        }

        osg::ref_ptr<osg::Node> node = osgDB::readRefNodeFile(earthFile, dbo.get());
        mapNode = MapNode::get(node.get());
        if (mapNode.valid())
        {
            mapNode->open();
            map = mapNode->getMap();
        }

        input = map->getLayerByName<TileLayer>(layerName);
        if (!input.valid())
        {
            OE_WARN << "Layer \"" << layerName << "\" not found in the earth file" << std::endl;
            return -1;
        }

        inConf = input->getConfig();
    }

    // command line input option:
    else
    {
        // collect input configuration:
        while (args.read("--in", key, value))
            inConf.set(key, value);

        inConf.key() = inConf.value("driver");

        auto layer = Layer::create(ConfigOptions(inConf));
        input = dynamic_cast<TileLayer*>(layer.get());
        if (!input)
        {
            OE_WARN << LC << "Failed to open input for " << inConf.toJSON(false) << std::endl;
            return -1;
        }
    }

    // Assign a custom tile size to the input source, if possible:
    unsigned tileSize = input->getTileSize();
    if (args.read("--tile-size", tileSize))
    {
        input->setTileSize(tileSize);
    }

    // Open the input layer:
    input->setReadOptions(dbo.get());
    Status inputStatus = input->open();
    if (inputStatus.isError())
    {
        OE_WARN << LC << "Error initializing input: " << inputStatus.message() << std::endl;
        return -1;
    }


    // are we changing profiles?
    osg::ref_ptr<const Profile> outputProfile = input->getProfile();
    std::string profileString;
    bool isSameProfile = true;

    if (args.read("--profile", profileString))
    {
        outputProfile = Profile::create(profileString);
        if (!outputProfile.valid() || !outputProfile->isOK())
        {
            OE_WARN << LC << "Output profile is not recognized" << std::endl;
            return -1;
        }
        isSameProfile = outputProfile->isHorizEquivalentTo(input->getProfile());
    }


    // set the manual extents, if specified:
    double minlat, minlon, maxlat, maxlon;
    optional<GeoExtent> userExtent;
    while (args.read("--extents", minlat, minlon, maxlat, maxlon))
    {
        userExtent = GeoExtent(SpatialReference::get("wgs84"), minlon, minlat, maxlon, maxlat);
        if (userExtent->isInvalid())
            userExtent.clear();
    }




    // Set the level limits:
    unsigned minLevel = ~0;
    bool minLevelSet = args.read("--min-level", minLevel);

    unsigned maxLevel = 0;
    bool maxLevelSet = args.read("--max-level", maxLevel);


    std::vector<DataExtent> extentsToVisit;
    optional<unsigned> minLevelToVisit;
    optional<unsigned> maxLevelToVisit;

    // figure out the max source level:
    if (!minLevelSet || !maxLevelSet)
    {
        DataExtentList dataExtents;
        input->getDataExtents(dataExtents);
        for (auto& de : dataExtents)
        {
            if (!maxLevelSet && de.maxLevel().isSet() && de.maxLevel().get() > maxLevel)
                maxLevel = de.maxLevel().get();

            if (!minLevelSet && de.minLevel().isSet() && de.minLevel().get() < minLevel)
                minLevel = de.minLevel().get();

            if (!userExtent.isSet())
            {
                extentsToVisit.emplace_back(de);
            }
        }
    }

    if (minLevel < ~0)
    {
        minLevelToVisit = minLevel;
    }

    if (maxLevel > 0)
    {
        maxLevel = outputProfile->getEquivalentLOD(input->getProfile(), maxLevel);
        maxLevelToVisit = maxLevel;
    }









    // collect output configuration:
    bool compress = false;
    Config outConf;
    while (args.read("--out", key, value))
    {
        outConf.set(key, value);

        // special case: turn on compression when using dds at the output format
        // and disable the built-in image flipping logic
        if (key == "format" && value == "dds")
        {
            compress = true;
        }
    }
    outConf.key() = outConf.value("driver");

    // set the output profile.
    ProfileOptions profileOptions = outputProfile->toProfileOptions();
    outConf.add("profile", profileOptions.getConfig());

    if (maxLevelToVisit.isSet())
    {
        outConf.set("max_data_level", maxLevelToVisit.value());
    }

    // open the output tile source:
    auto layer = Layer::create(ConfigOptions(outConf));
    osg::ref_ptr<TileLayer> output = dynamic_cast<TileLayer*>(layer.get());
    if (!output)
    {
        OE_WARN << LC << "Failed to create output layer" << std::endl;
        return -1;
    }

    output->setReadOptions(dbo.get());
    Status outputStatus = output->openForWriting();
    if (outputStatus.isError())
    {
        OE_WARN << LC << "Error initializing output: " << outputStatus.message() << std::endl;
        return -1;
    }

    // Dump out some stuff...
    OE_NOTICE << LC << "FROM:\n"
        << inConf.toJSON(true)
        << std::endl;

    OE_NOTICE << LC << "TO:\n"
        << outConf.toJSON(true)
        << std::endl;





    // create the visitor.
    osg::ref_ptr<TileVisitor> visitor;

    unsigned numThreads = 4;
    args.read("--threads", numThreads);
    MultithreadedTileVisitor* mtv = new MultithreadedTileVisitor();
    mtv->setNumThreads(numThreads < 1 ? 1 : numThreads);
    visitor = mtv;

    bool overwrite = true;
    if (args.read("--no-overwrite"))
        overwrite = false;

    bool threaded_writer = args.read("--threaded-writer");

    if (dynamic_cast<ImageLayer*>(input.get()) && dynamic_cast<ImageLayer*>(output.get()))
    {
        visitor->setTileHandler(new ImageLayerTileCopy(
            dynamic_cast<ImageLayer*>(input.get()),
            dynamic_cast<ImageLayer*>(output.get()),
            overwrite,
            compress,
            threaded_writer));
    }
    else if (dynamic_cast<ElevationLayer*>(input.get()) && dynamic_cast<ElevationLayer*>(output.get()))
    {
        visitor->setTileHandler(new ElevationLayerTileCopy(
            dynamic_cast<ElevationLayer*>(input.get()),
            dynamic_cast<ElevationLayer*>(output.get()),
            overwrite,
            threaded_writer));
    }

    // Read in an index shapefile to drive where to tile
    std::string index;

    while (args.read("--index", index))
    {
        osg::ref_ptr< OGRFeatureSource > indexFeatures = new OGRFeatureSource;
        indexFeatures->setURL(index);
        if (indexFeatures->open().isError())
        {
            OE_WARN << "Failed to open index " << index << ": " << indexFeatures->getStatus().toString() << std::endl;
            return -1;
        }

        osg::ref_ptr< FeatureCursor > cursor = indexFeatures->createFeatureCursor();
        while (cursor.valid() && cursor->hasMore())
        {
            osg::ref_ptr< Feature > feature = cursor->nextFeature();
            osgEarth::Bounds featureBounds = feature->getGeometry()->getBounds();
            GeoExtent ext(feature->getSRS(), featureBounds);
            ext = input->getProfile()->clampAndTransformExtent(ext);

            visitor->addExtentToDataIndex(ext);
        }
    }



    if (userExtent.isSet())
    {
        visitor->addExtentToVisit(userExtent.value());
    }
    else
    {
        for (auto& de : extentsToVisit)
            visitor->addExtentToVisit(de);
    }

    if (minLevelToVisit.isSet())
    {
        visitor->setMinLevel(minLevelToVisit.get());
    }

    if (maxLevelToVisit.isSet())
    {
        visitor->setMaxLevel(maxLevelToVisit.get());
        OE_NOTICE << LC << "Calculated max level = " << maxLevel << std::endl;
    }



    // If we've not added any extents to visit just add the entire input extent
    if (visitor->getExtentsToVisit().empty())
    {
        visitor->addExtentToVisit(input->getProfile()->getExtent());
    }

    DataExtentList outputExtents;
    if (!userExtent.isSet())
    {
        // Transform and copy over the data extents to the output datasource.
        DataExtentList inputExtents;
        input->getDataExtents(inputExtents);

        for (DataExtentList::const_iterator itr = inputExtents.begin(); itr != inputExtents.end(); ++itr)
        {
            // Convert the data extent to the profile that is actually used by the output tile source
            const DataExtent& inputExtent = *itr;
            GeoExtent outputExtent = outputProfile->clampAndTransformExtent(inputExtent);
            DataExtent result(outputExtent, 0, outputProfile->getEquivalentLOD(input->getProfile(), inputExtent.maxLevel().get()));
            outputExtents.push_back(result);
        }
    }
    else
    {
        // Use the user set extents
        GeoExtent outputExtent = outputProfile->clampAndTransformExtent(userExtent.value());
        DataExtent result(outputExtent, outputProfile->getEquivalentLOD(input->getProfile(), minLevel), outputProfile->getEquivalentLOD(input->getProfile(), maxLevel));
        outputExtents.push_back(result);
    }

    if (outputExtents.empty())
    {
        // The input has no data extents and the user didn't specify an extent, so use the entire output profile's extent
        DataExtent result(outputProfile->getExtent(),
                          outputProfile->getEquivalentLOD(input->getProfile(), minLevel),
                          outputProfile->getEquivalentLOD(input->getProfile(), maxLevel));
        outputExtents.push_back(result);
    }


    if (!outputExtents.empty())
    {
        output->setDataExtents(outputExtents);
    }

    // if the input layer is using a tile rasterizer, we need a frame loop!
    auto* rasterizer = osgEarth::findTopMostNodeOfType<TileRasterizer>(input->getNode());
    if (rasterizer)
        std::cout << "Found a tile rasterizer; going to run a headless frame loop." << std::endl;

    // Ready!!!
    std::cout << "Working..." << std::endl;

    visitor->setProgressCallback( new ProgressReporter() );

    osg::Timer_t t0 = osg::Timer::instance()->tick();

    if (rasterizer)
    {
        // run the tile visitor in a job.
        jobs::future<bool> visitorDone = jobs::dispatch([&](auto cancelable)
            {
                visitor->run(outputProfile.get());
                return true;
            });

        // configure the fastest possible frame loop.
        GL3RealizeOperation* realizer = new GL3RealizeOperation();
        realizer->setSyncToVBlank(false);

        osgViewer::Viewer viewer;
        viewer.setRealizeOperation(realizer);
        viewer.setThreadingModel(viewer.SingleThreaded);

        // configure for headless rendering.
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits();
        traits->x = 0;
        traits->y = 0;
        traits->width = 1;
        traits->height = 1;
        traits->windowDecoration = false;
        traits->pbuffer = true;
        traits->doubleBuffer = true;
        traits->sharedContext = nullptr;

        osg::ref_ptr<osg::GraphicsContext> gc = osg::GraphicsContext::createGraphicsContext(traits.get());
        auto* camera = viewer.getCamera();
        camera->setGraphicsContext(gc.get());
        camera->setDrawBuffer(GL_BACK);
        camera->setReadBuffer(GL_BACK);
        camera->setViewport(new osg::Viewport(0, 0, traits->width, traits->height));

        // make the rasterizer faster? Maybe?
        rasterizer->setNumRenderersPerGraphicsContext(64);
        rasterizer->setNumJobsToDispatchPerFrame(64);

        // disable the kdtrees for performance boost
        osgDB::Registry::instance()->setKdTreeBuilder(nullptr);

        viewer.setSceneData(rasterizer);

        // run until the visitor is done.
        while(!visitorDone.available())
        {
            viewer.frame();
        }
    }
    else
    {
        visitor->run(outputProfile.get());
    }

    osg::Timer_t t1 = osg::Timer::instance()->tick();

    std::cout
        << std::endl
        << "Done. Time = "
        << std::fixed
        << std::setprecision(1)
        << osg::Timer::instance()->delta_s(t0, t1)
        << " seconds." << std::endl;

    return 0;
}
