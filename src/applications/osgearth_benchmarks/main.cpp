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
#include <osgEarth/TileKey>
#include <osgEarth/Cache>
#include <osgEarth/ImageUtils>
#include <osgDB/ReadFile>
#include <filesystem>

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
}

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


static void BM_TileKeyString(benchmark::State& state)
{
    osg::ref_ptr<const Profile> profile = Profile::create(Profile::GLOBAL_GEODETIC);

    std::vector<TileKey> keys;
    keys.reserve(state.range(0));
    for (int i = 0; i < state.range(0); ++i)
    {
        const unsigned lod = 8u + static_cast<unsigned>(i % 16);
        keys.emplace_back(
            lod,
            12345u + static_cast<unsigned>(i * 37u),
            67890u + static_cast<unsigned>(i * 53u),
            profile.get());
    }

    if (TileKey(17u, 123456u, 654321u, profile.get()).str() != "17/123456/654321")
    {
        state.SkipWithError("TileKey string formatting changed unexpectedly");
        return;
    }

    for (auto _ : state)
    {
        for (const auto& key : keys)
        {
            std::string formatted = key.str();
            benchmark::DoNotOptimize(formatted);
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(keys.size()));
}
BENCHMARK(BM_TileKeyString)->Arg(4096);



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

BENCHMARK_MAIN();
