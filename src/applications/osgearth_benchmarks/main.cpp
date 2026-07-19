/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <benchmark/benchmark.h>

#include <osgEarth/ElevationPool>
#include <osgEarth/GeoData>
#include <osgEarth/HeightFieldUtils>
#include <osgEarth/Map>
#include <osgEarth/SpatialReference>
#include <osgEarth/StringUtils>
#include <osgEarth/Cache>
#include <osgEarth/ImageUtils>
#include <osgEarth/InstanceBuilder>
#include <osgEarth/Kit>
#include <osgEarth/MBTiles>
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osgDB/Options>
#include <osgDB/ReadFile>
#include <filesystem>
#include <unordered_set>

using namespace osgEarth;
namespace fs = std::filesystem;

namespace
{
    class ConstantElevationLayer : public ElevationLayer
    {
    public:
        META_LayerNoOptions(osgEarth, ConstantElevationLayer, ElevationLayer, constant_elevation);

        void init() override
        {
            ElevationLayer::init();
            setProfile(Profile::create(Profile::GLOBAL_GEODETIC));
            setMaxDataLevel(12u);
            options().tileSize() = 9u;
        }

    protected:
        GeoHeightField createHeightFieldImplementation(
            const TileKey& key,
            ProgressCallback* progress) const override
        {
            osg::ref_ptr<osg::HeightField> hf = HeightFieldUtils::createReferenceHeightField(
                key.getExtent(),
                getTileSize(),
                getTileSize(),
                0u,
                false,
                42.0f);

            return GeoHeightField(hf.get(), key.getExtent());
        }

        virtual ~ConstantElevationLayer() { }
    };

    osg::ref_ptr<Map> createElevationBenchmarkMap(unsigned dataExtentCount)
    {
        osg::ref_ptr<Map> map = new Map();
        map->setProfile(Profile::create(Profile::GLOBAL_GEODETIC));

        osg::ref_ptr<ConstantElevationLayer> layer = new ConstantElevationLayer();

        DataExtentList dataExtents;
        dataExtents.reserve(dataExtentCount);
        for (unsigned i = 0; i < dataExtentCount; ++i)
        {
            dataExtents.emplace_back(map->getProfile()->getExtent(), 0u, 12u);
        }

        layer->setDataExtents(dataExtents);
        map->addLayer(layer.get());
        map->getElevationPool()->setMap(map.get());

        return map;
    }

    struct MBTilesReadBenchmarkData
    {
        MBTiles::Driver driver;
        osg::ref_ptr<const Profile> profile;
        TileKey key = TileKey::INVALID;
        bool ready = false;
        std::string error;

        MBTilesReadBenchmarkData()
        {
            MBTiles::Options options;
            options.url() = "../data/world_countries.mbtiles";

            DataExtentList dataExtents;
            Status status = driver.open(
                "world_countries",
                options,
                false,
                options.format(),
                profile,
                dataExtents,
                nullptr);

            if (status.isError())
            {
                error = status.toString();
                return;
            }

            if (!profile.valid())
            {
                error = "MBTiles benchmark failed to establish a profile";
                return;
            }

            // Largest PNG tile in the bundled fixture: z=3, x=4, MBTiles row=4.
            key = TileKey(3u, 4u, 3u, profile.get());

            ReadResult warmup = driver.read(key, nullptr, nullptr);
            if (!warmup.succeeded())
            {
                error = "MBTiles benchmark failed to read warmup tile";
                return;
            }

            ready = true;
        }
    };

    MBTilesReadBenchmarkData& getMBTilesReadBenchmarkData()
    {
        static MBTilesReadBenchmarkData data;
        return data;
    }

    osg::Node* createKitBenchmarkModel()
    {
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array();
        vertices->push_back(osg::Vec3f(-0.5f, -0.5f, 0.0f));
        vertices->push_back(osg::Vec3f(0.5f, -0.5f, 0.0f));
        vertices->push_back(osg::Vec3f(0.0f, 0.5f, 1.0f));

        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry();
        geometry->setVertexArray(vertices.get());
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));

        osg::ref_ptr<osg::Geode> geode = new osg::Geode();
        geode->addDrawable(geometry.get());
        return geode.release();
    }

    struct InstanceArrayMemoryVisitor : public osg::NodeVisitor
    {
        InstanceArrayMemoryVisitor() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { }

        void apply(osg::Geode& geode) override
        {
            for (unsigned i = 0u; i < geode.getNumDrawables(); ++i)
            {
                osg::Geometry* geometry = geode.getDrawable(i)->asGeometry();
                if (!geometry)
                    continue;
                const osg::BufferData* buffer =
                    InstanceBuilder::getInstanceBuffer(geometry);
                if (buffer && buffers.insert(buffer).second)
                    bytes += buffer->getTotalDataSize();
            }
            traverse(geode);
        }

        std::unordered_set<const osg::BufferData*> buffers;
        std::uint64_t bytes = 0u;
    };
}

static void BM_ExpandedModelSceneBuild(benchmark::State& state)
{
    osg::ref_ptr<osg::Node> model = createKitBenchmarkModel();
    const int count = static_cast<int>(state.range(0));

    for (auto _ : state)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group();
        for (int i = 0; i < count; ++i)
        {
            osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform(
                osg::Matrix::translate(static_cast<float>(i % 100), static_cast<float>(i / 100), 0.0f));
            transform->addChild(model.get());
            root->addChild(transform.get());
        }
        benchmark::DoNotOptimize(root.get());
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.counters["draw_submissions"] = static_cast<double>(count);
}
BENCHMARK(BM_ExpandedModelSceneBuild)
    ->Arg(5000)
    ->Arg(100000)
    ->Unit(benchmark::kMicrosecond);

static void BM_KitInstancedSceneBuild(benchmark::State& state)
{
    osg::ref_ptr<Kit> kit = new Kit();
    kit->addModel("unit", createKitBenchmarkModel());
    const int count = static_cast<int>(state.range(0));

    for (auto _ : state)
    {
        osg::ref_ptr<KitNode> source = new KitNode();
        source->reserveInstances(count);
        for (int i = 0; i < count; ++i)
        {
            source->addInstance(
                "unit",
                osg::Vec3f(static_cast<float>(i % 100), static_cast<float>(i / 100), 0.0f));
        }
        osg::ref_ptr<osg::Group> result = kit->createInstancedNode(source.get());
        benchmark::DoNotOptimize(result.get());
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.counters["draw_submissions"] = 1.0;
}
BENCHMARK(BM_KitInstancedSceneBuild)
    ->Arg(5000)
    ->Arg(100000)
    ->Arg(250000)
    ->Arg(1000000)
    ->Unit(benchmark::kMicrosecond);

// Measures the scene-layout penalty that Kit-level collection is intended to
// remove. The total instance count and model are identical; only the number of
// separately compiled city/tile graphs changes.
static void BM_KitInstancedSceneLayout(benchmark::State& state)
{
    osg::ref_ptr<Kit> kit = new Kit();
    kit->addModel("unit", createKitBenchmarkModel());
    constexpr int totalInstances = 1000;
    const int groups = static_cast<int>(state.range(0));
    unsigned drawSubmissions = 0u;

    for (auto _ : state)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group();
        root->addChild(kit->getRenderNode());
        for (int group = 0; group < groups; ++group)
        {
            osg::ref_ptr<KitNode> source = new KitNode();
            const int begin = group * totalInstances / groups;
            const int end = (group + 1) * totalInstances / groups;
            source->reserveInstances(end - begin);
            for (int i = begin; i < end; ++i)
            {
                source->addInstance(
                    "unit",
                    osg::Vec3f(static_cast<float>(i % 100),
                        static_cast<float>(i / 100), 0.0f));
            }

            Kit::BuildStats stats;
            root->addChild(kit->createInstancedNode(source.get(), &stats));
        }
        drawSubmissions = kit->getNumRenderDrawables();
        benchmark::DoNotOptimize(root.get());
    }

    state.SetItemsProcessed(state.iterations() * totalInstances);
    state.counters["compiled_graphs"] = static_cast<double>(groups);
    state.counters["draw_submissions"] = static_cast<double>(drawSubmissions);
}
BENCHMARK(BM_KitInstancedSceneLayout)
    ->Arg(1)
    ->Arg(4)
    ->Unit(benchmark::kMicrosecond);

static void BM_KitInstancedCityDestroy(benchmark::State& state)
{
    constexpr int modelCount = 10;
    const int count = static_cast<int>(state.range(0));
    osg::ref_ptr<Kit> kit = new Kit();
    kit->setInstanceChunkSize(128.0f);
    for (int model = 0; model < modelCount; ++model)
        kit->addModel("unit" + std::to_string(model), createKitBenchmarkModel());

    osg::ref_ptr<KitNode> source = new KitNode();
    source->reserveInstances(count);
    for (int i = 0; i < count; ++i)
    {
        source->addInstance(
            "unit" + std::to_string(i % modelCount),
            osg::Vec3f(
                static_cast<float>((i * 37) % 2048),
                static_cast<float>((i * 101) % 2048),
                static_cast<float>(i % 20)));
    }

    Kit::BuildStats sampleStats;
    osg::ref_ptr<osg::Group> sample =
        kit->createInstancedNode(source.get(), &sampleStats);
    InstanceArrayMemoryVisitor sampleMemory;
    sample->accept(sampleMemory);
    state.counters["batches"] = static_cast<double>(sampleStats.batches);
    state.counters["instance_buffers"] =
        static_cast<double>(sampleMemory.buffers.size());
    state.counters["old_instance_arrays"] =
        static_cast<double>(sampleStats.drawables * 3u);
    sample = nullptr;

    for (auto _ : state)
    {
        state.PauseTiming();
        osg::ref_ptr<osg::Group> result = kit->createInstancedNode(source.get());
        benchmark::DoNotOptimize(result.get());
        state.ResumeTiming();

        result = nullptr;
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_KitInstancedCityDestroy)
    ->Arg(100000)
    ->Arg(250000)
    ->Unit(benchmark::kMicrosecond);

static void BM_KitBinaryCityRead(benchmark::State& state)
{
    const fs::path cityPath = "../data/kit/cities/downtown_dense.kitcityb";
    if (!fs::is_regular_file(cityPath))
    {
        state.SkipWithError("Run this benchmark from the tests directory");
        return;
    }

    osg::ref_ptr<osgDB::Options> options = new osgDB::Options();
    options->setObjectCacheHint(osgDB::Options::CACHE_NONE);
    for (auto _ : state)
    {
        osg::ref_ptr<osg::Node> city = osgDB::readRefNodeFile(cityPath.string(), options.get());
        benchmark::DoNotOptimize(city.get());
        auto* kitNode = dynamic_cast<KitNode*>(city.get());
        if (!kitNode || kitNode->getNumInstances() != 2377247u)
        {
            state.SkipWithError("Binary city did not load with the expected instance count");
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * 2377247u);
}
BENCHMARK(BM_KitBinaryCityRead)
    ->Iterations(3)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

static void BM_KitBinaryCityGather(benchmark::State& state)
{
    const fs::path cityPath = "../data/kit/cities/downtown_dense.kitcityb";
    const fs::path kitPath = "../data/kit/buildings.kit";
    osg::ref_ptr<osgDB::Options> options = new osgDB::Options();
    options->setObjectCacheHint(osgDB::Options::CACHE_NONE);
    osg::ref_ptr<osg::Node> city = osgDB::readRefNodeFile(cityPath.string(), options.get());
    osg::ref_ptr<Kit> kit = new Kit();
    if (!city.valid() || !kit->load(kitPath.string(), options.get()))
    {
        state.SkipWithError("Failed to load the Kit city benchmark fixtures");
        return;
    }
    kit->setInstanceChunkSize(512.0f);

    for (auto _ : state)
    {
        Kit::BuildStats stats;
        osg::ref_ptr<osg::Group> result = kit->createInstancedNode(city.get(), &stats);
        benchmark::DoNotOptimize(result.get());
        if (stats.instances != 2377247u || stats.missingModels != 0u)
        {
            state.SkipWithError("Gather changed the city instance count");
            break;
        }
        InstanceArrayMemoryVisitor memory;
        result->accept(memory);
        state.counters["instance_buffer_MiB"] =
            static_cast<double>(memory.bytes) / (1024.0 * 1024.0);
        state.counters["instance_buffers"] =
            static_cast<double>(memory.buffers.size());
    }
    state.SetItemsProcessed(state.iterations() * 2377247u);
}
BENCHMARK(BM_KitBinaryCityGather)
    ->Iterations(3)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

static void BM_InstanceBoundingBoxTraversal(benchmark::State& state)
{
    constexpr unsigned count = 250000u;
    osg::ref_ptr<osg::Geometry> geometry = InstanceBuilder::createGeometry();
    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array();
    vertices->push_back(osg::Vec3f(-0.5f, -0.5f, 0.0f));
    vertices->push_back(osg::Vec3f(0.5f, -0.5f, 0.0f));
    vertices->push_back(osg::Vec3f(0.5f, 0.5f, 1.0f));
    vertices->push_back(osg::Vec3f(-0.5f, 0.5f, 1.0f));
    geometry->setVertexArray(vertices.get());
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));

    osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array();
    osg::ref_ptr<osg::Vec4Array> rotations = new osg::Vec4Array();
    osg::ref_ptr<osg::Vec3Array> scales = new osg::Vec3Array();
    positions->reserve(count);
    rotations->reserve(count);
    scales->reserve(count);
    for (unsigned i = 0u; i < count; ++i)
    {
        positions->push_back(osg::Vec3f(
            static_cast<float>(i % 500u),
            static_cast<float>(i / 500u),
            static_cast<float>(i % 20u)));
        const float angle = static_cast<float>(i % 32u) * 0.09817477f;
        rotations->push_back(osg::Vec4f(0.0f, 0.0f, std::sin(angle), std::cos(angle)));
        scales->push_back(osg::Vec3f(1.0f + static_cast<float>(i % 4u), 2.0f, 3.0f));
    }

    InstanceBuilder builder;
    builder.setPositions(positions.get());
    builder.setRotations(rotations.get());
    builder.setScales(scales.get());
    builder.installInstancing(geometry.get());

    for (auto _ : state)
    {
        geometry->dirtyBound();
        const osg::BoundingBox& bounds = geometry->getBoundingBox();
        benchmark::DoNotOptimize(bounds);
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_InstanceBoundingBoxTraversal)
    ->Iterations(3)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

static void BM_GeoPointTransform(benchmark::State& state)
{
    auto wgs84 = osgEarth::SpatialReference::get("wgs84");
    auto mercator = osgEarth::SpatialReference::get("spherical-mercator");
    osgEarth::GeoPoint point(wgs84, -73.935242, 40.730610, 0.0);

    for (auto _ : state)
    {
        osgEarth::GeoPoint output;
        point.transform(mercator, output);
        benchmark::DoNotOptimize(output);
    }
}
BENCHMARK(BM_GeoPointTransform);

static void BM_GeoExtentContains(benchmark::State& state)
{
    auto srs = osgEarth::SpatialReference::get("wgs84");
    osgEarth::GeoExtent extent(srs, -180.0, -90.0, 180.0, 90.0);

    for (auto _ : state)
    {
        bool result = extent.contains(45.0, 45.0);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_GeoExtentContains);

static void BM_GeoExtentIntersects(benchmark::State& state)
{
    auto srs = osgEarth::SpatialReference::get("wgs84");
    osgEarth::GeoExtent a(srs, -10.0, -10.0, 10.0, 10.0);
    osgEarth::GeoExtent b(srs, 5.0, 5.0, 20.0, 20.0);

    for (auto _ : state)
    {
        bool result = a.intersects(b);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_GeoExtentIntersects);

static void BM_ElevationPoolSampleMapCoordsFixedResolution(benchmark::State& state)
{
    osg::ref_ptr<Map> map = createElevationBenchmarkMap(256u);
    osg::ref_ptr<ElevationPool> pool = map->getElevationPool();

    std::vector<osg::Vec3d> points;
    points.reserve(state.range(0));
    for (int i = 0; i < state.range(0); ++i)
    {
        double x = -1.0 + 2.0 * static_cast<double>(i % 64) / 63.0;
        double y = -1.0 + 2.0 * static_cast<double>((i / 64) % 64) / 63.0;
        points.emplace_back(x, y, 0.0);
    }

    const Distance resolution(10000000.0, Units::METERS);

    int warmupCount = pool->sampleMapCoords(
        points.begin(),
        points.end(),
        resolution,
        nullptr,
        nullptr);

    if (warmupCount != static_cast<int>(points.size()))
    {
        state.SkipWithError("ElevationPool failed to sample all benchmark points");
        return;
    }

    for (auto _ : state)
    {
        int count = pool->sampleMapCoords(
            points.begin(),
            points.end(),
            resolution,
            nullptr,
            nullptr);

        if (count != static_cast<int>(points.size()))
        {
            state.SkipWithError("ElevationPool failed to sample all benchmark points");
            return;
        }

        benchmark::DoNotOptimize(count);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ElevationPoolSampleMapCoordsFixedResolution)->Arg(4096);

static void BM_MBTilesImageRead_PNG(benchmark::State& state)
{
    MBTilesReadBenchmarkData& data = getMBTilesReadBenchmarkData();
    if (!data.ready)
    {
        state.SkipWithError(data.error.c_str());
        return;
    }

    for (auto _ : state)
    {
        ReadResult result = data.driver.read(data.key, nullptr, nullptr);
        if (!result.succeeded())
        {
            state.SkipWithError("MBTiles benchmark failed to read tile");
            return;
        }

        benchmark::DoNotOptimize(result.getImage());
    }
}
BENCHMARK(BM_MBTilesImageRead_PNG)->ThreadRange(1, 8)->UseRealTime()->Unit(benchmark::kMicrosecond);



const int NUM_CACHE_IMAGES = 1000;
const std::string CACHE_IMAGE = "../data/readymap_tile.jpg";
const std::string CACHE_PATH = "cache";

static void BM_FileSystemSingleThreadedRead(benchmark::State& state)
{
    Config config;
    config.fromJSON("{ \"path\": \"" + CACHE_PATH + "\" }");
    CacheOptions cacheOptions(config);
    cacheOptions.setDriver("filesystem");

    // Fill the cache
    osg::ref_ptr<Cache> cache = CacheFactory::create(cacheOptions);
    osg::ref_ptr<CacheBin> cacheBin = cache->getOrCreateDefaultBin();
    osg::ref_ptr< osg::Image > image = osgDB::readRefImageFile(CACHE_IMAGE);
    for (unsigned int i = 0; i < NUM_CACHE_IMAGES; ++i)
    {
        std::string key = "image_" + std::to_string(i);
        cacheBin->write(key, image.get(), nullptr);
    }

    // Delete the cache to finish writing
    cache = nullptr;

    for (auto _ : state)
    {
        // Recreate the path at the same location.
        osg::ref_ptr<Cache> cache = CacheFactory::create(cacheOptions);
        osg::ref_ptr<CacheBin> cacheBin = cache->getOrCreateDefaultBin();

        // Read all the images back
        for (unsigned int i = 0; i < NUM_CACHE_IMAGES; ++i)
        {
            std::string key = "image_" + std::to_string(i);
            osg::ref_ptr< osg::Image > image = cacheBin->readImage(key, nullptr).getImage();
            benchmark::DoNotOptimize(image);
        }
    }

    // Remove the CACHE_PATH directory after the benchmark to clean up the generated files
    fs::remove_all(CACHE_PATH);
}
BENCHMARK(BM_FileSystemSingleThreadedRead)->Iterations(1);

static void BM_FileSystemSingleThreadedWrite(benchmark::State& state)
{
    for (auto _ : state)
    {
        Config config;
        config.fromJSON("{ \"path\": \"" + CACHE_PATH + "\" }");
        CacheOptions cacheOptions(config);
        cacheOptions.setDriver("filesystem");
        osg::ref_ptr<Cache> cache = CacheFactory::create(cacheOptions);

        osg::ref_ptr<CacheBin> cacheBin = cache->getOrCreateDefaultBin();

        osg::ref_ptr< osg::Image > image = osgDB::readRefImageFile(CACHE_IMAGE);

        for (unsigned int i = 0; i < NUM_CACHE_IMAGES; ++i)
        {
            std::string key = "image_" + std::to_string(i);
            bool result = cacheBin->write(key, image.get(), nullptr);
            benchmark::DoNotOptimize(result);
        }
    }

    // Remove the CACHE_PATH directory after the benchmark to clean up the generated files
    fs::remove_all(CACHE_PATH);
}

BENCHMARK(BM_FileSystemSingleThreadedWrite)->Iterations(1);

static void BM_SQLite3SingleThreadedRead(benchmark::State& state)
{
    Config config;
    config.fromJSON("{ \"path\": \"" + CACHE_PATH + "\" }");
    CacheOptions cacheOptions(config);
    cacheOptions.setDriver("filesystem");

    osg::ref_ptr<Cache> cache = CacheFactory::create(cacheOptions);
    osg::ref_ptr<CacheBin> cacheBin = cache->getOrCreateDefaultBin();

    osg::ref_ptr< osg::Image > image = osgDB::readRefImageFile(CACHE_IMAGE);
    for (unsigned int i = 0; i < NUM_CACHE_IMAGES; ++i)
    {
        std::string key = "image_" + std::to_string(i);
        cacheBin->write(key, image.get(), nullptr);
    }

    // Delete the cache to finish writing
    cache = nullptr;

    for (auto _ : state)
    {
        // Recreate the path at the same location.
        osg::ref_ptr<Cache> cache = CacheFactory::create(cacheOptions);
        osg::ref_ptr<CacheBin> cacheBin = cache->getOrCreateDefaultBin();

        for (unsigned int i = 0; i < NUM_CACHE_IMAGES; ++i)
        {
            std::string key = "image_" + std::to_string(i);
            osg::ref_ptr< osg::Image > image = cacheBin->readImage(key, nullptr).getImage();
            benchmark::DoNotOptimize(image);
        }
    }

    // Remove the CACHE_PATH directory after the benchmark to clean up the generated files
    fs::remove_all(CACHE_PATH);
}
BENCHMARK(BM_SQLite3SingleThreadedRead)->Iterations(1);

static void BM_SQLite3SystemSingleThreadedWrite(benchmark::State& state)
{
    for (auto _ : state)
    {
        Config config;
        config.fromJSON("{ \"path\": \"" + CACHE_PATH + "\" }");
        CacheOptions cacheOptions(config);
        cacheOptions.setDriver("sqlite3");
        osg::ref_ptr<Cache> cache = CacheFactory::create(cacheOptions);

        osg::ref_ptr<CacheBin> cacheBin = cache->getOrCreateDefaultBin();

        osg::ref_ptr< osg::Image > image = osgDB::readRefImageFile(CACHE_IMAGE);

        for (unsigned int i = 0; i < NUM_CACHE_IMAGES; ++i)
        {
            std::string key = "image_" + std::to_string(i);
            bool result = cacheBin->write(key, image.get(), nullptr);
            benchmark::DoNotOptimize(result);
        }
    }

    // Remove the CACHE_PATH directory after the benchmark to clean up the generated files
    fs::remove_all(CACHE_PATH);
}

BENCHMARK(BM_SQLite3SystemSingleThreadedWrite)->Iterations(1);

static void BM_CompressImage_FastDXT(benchmark::State& state)
{
    std::string driver = "fastdxt";
    osg::ref_ptr<osg::Image> image = osgDB::readRefImageFile(CACHE_IMAGE);
    // Preload the processor so it's ready before timing.
    osgDB::ImageProcessor* ip = osgDB::Registry::instance()->getImageProcessorForExtension(driver);

    for (auto _ : state)
    {
        osg::ref_ptr<const osg::Image> compressed = ImageUtils::compressImage(image.get(), driver);
        benchmark::DoNotOptimize(compressed);
    }
}
BENCHMARK(BM_CompressImage_FastDXT);

static void BM_CompressImage_STBDXT(benchmark::State& state)
{
    std::string driver = "stbdxt";

    osg::ref_ptr<osg::Image> image = osgDB::readRefImageFile(CACHE_IMAGE);
    // Preload the processor so it's ready before timing.
    osgDB::ImageProcessor* ip = osgDB::Registry::instance()->getImageProcessorForExtension(driver);

    for (auto _ : state)
    {
        osg::ref_ptr<const osg::Image> compressed = ImageUtils::compressImage(image.get(), driver);
        benchmark::DoNotOptimize(compressed);
    }
}
BENCHMARK(BM_CompressImage_STBDXT);

static osg::ref_ptr<osg::Image> createResizeBenchmarkImage(unsigned int width, unsigned int height)
{
    osg::ref_ptr<osg::Image> image = new osg::Image();
    image->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);

    for (unsigned int t = 0; t < height; ++t)
    {
        for (unsigned int s = 0; s < width; ++s)
        {
            unsigned char* pixel = image->data(s, t);
            pixel[0] = static_cast<unsigned char>((s * 3 + t) & 0xff);
            pixel[1] = static_cast<unsigned char>((s + t * 5) & 0xff);
            pixel[2] = static_cast<unsigned char>((s * 7 + t * 11) & 0xff);
            pixel[3] = static_cast<unsigned char>(255 - ((s + t) & 0x7f));
        }
    }

    return image;
}

static void BM_ResizeImage_BilinearRGBA8(benchmark::State& state)
{
    osg::ref_ptr<osg::Image> image = createResizeBenchmarkImage(1024, 1024);
    osg::ref_ptr<osg::Image> output = new osg::Image();
    output->allocateImage(
        static_cast<int>(state.range(0)),
        static_cast<int>(state.range(1)),
        1,
        GL_RGBA,
        GL_UNSIGNED_BYTE);

    for (auto _ : state)
    {
        bool result = ImageUtils::resizeImage(
            image.get(),
            static_cast<unsigned int>(state.range(0)),
            static_cast<unsigned int>(state.range(1)),
            output,
            0,
            true);
        benchmark::DoNotOptimize(result);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ResizeImage_BilinearRGBA8)->Args({768, 768})->Unit(benchmark::kMillisecond);

static void BM_MipmapImage_RGBA8(benchmark::State& state)
{
    osg::ref_ptr<osg::Image> image = createResizeBenchmarkImage(
        static_cast<unsigned int>(state.range(0)),
        static_cast<unsigned int>(state.range(1)));

    for (auto _ : state)
    {
        osg::ref_ptr<const osg::Image> mipmapped = ImageUtils::mipmapImage(image.get(), 4);
        benchmark::DoNotOptimize(mipmapped.get());
        benchmark::DoNotOptimize(mipmapped->getNumMipmapLevels());
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_MipmapImage_RGBA8)->Args({1024, 1024})->Args({2048, 2048})->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
