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
#include <osgEarth/MBTiles>
#include <osgDB/ReadFile>
#include "../../osgEarth/FeatureImageLayerSlugPacking.h"
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

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

static std::vector<std::uint16_t> createSlugBandBenchmarkData(std::size_t texelCount)
{
    std::vector<std::uint16_t> result(texelCount * 4u);
    for (std::size_t i = 0u; i < result.size(); ++i)
        result[i] = static_cast<std::uint16_t>((i * 7919u) & 0xffffu);
    return result;
}

// Reference implementation used before the packed RG optimization.
static void BM_SlugBandAtlasExpandRGBA32F(benchmark::State& state)
{
    const std::size_t texelCount = static_cast<std::size_t>(state.range(0));
    const auto source = createSlugBandBenchmarkData(texelCount);
    std::vector<float> destination(texelCount * 4u);

    for (auto _ : state)
    {
        for (std::size_t i = 0u; i < source.size(); ++i)
            destination[i] = static_cast<float>(source[i]);
        benchmark::DoNotOptimize(destination.data());
        benchmark::ClobberMemory();
    }

    state.SetBytesProcessed(
        state.iterations() * static_cast<std::int64_t>(
            source.size() * sizeof(source[0]) +
            destination.size() * sizeof(destination[0])));
}
BENCHMARK(BM_SlugBandAtlasExpandRGBA32F)
    ->Arg(262144)
    ->Unit(benchmark::kMicrosecond);

static void BM_SlugBandAtlasPackRG32F(benchmark::State& state)
{
    using namespace osgEarth::Util::detail;
    const std::size_t texelCount = static_cast<std::size_t>(state.range(0));
    const auto source = createSlugBandBenchmarkData(texelCount);
    std::vector<float> destination(slugPackedBandTexelCount(texelCount) * 4u);

    for (auto _ : state)
    {
        slugPackBandRG(source.data(), texelCount, destination.data());
        benchmark::DoNotOptimize(destination.data());
        benchmark::ClobberMemory();
    }

    state.SetBytesProcessed(
        state.iterations() * static_cast<std::int64_t>(
            texelCount * 2u * sizeof(source[0]) +
            destination.size() * sizeof(destination[0])));
}
BENCHMARK(BM_SlugBandAtlasPackRG32F)
    ->Arg(262144)
    ->Unit(benchmark::kMicrosecond);

struct SlugSolverVec2
{
    float x;
    float y;
};

struct SlugSolverVec4
{
    float x;
    float y;
    float z;
    float w;
};

#ifdef _MSC_VER
#define OE_BENCHMARK_NOINLINE __declspec(noinline)
#else
#define OE_BENCHMARK_NOINLINE __attribute__((noinline))
#endif

// Mirrors the original Slughorn shader: quadratic work occurs before the
// straight-curve fallback overwrites its result.
static OE_BENCHMARK_NOINLINE SlugSolverVec2 slugSolveHorizontalLegacy(
    const SlugSolverVec4& p12,
    const SlugSolverVec2& p3)
{
    const SlugSolverVec2 a = {
        p12.x - p12.z * 2.0f + p3.x,
        p12.y - p12.w * 2.0f + p3.y
    };
    const SlugSolverVec2 b = { p12.x - p12.z, p12.y - p12.w };
    const float reciprocal = 1.0f / a.y;
    const float linearReciprocal = 0.5f / b.y;
    const float discriminant = std::sqrt(std::max(
        b.y * b.y - a.y * p12.y, 0.0f));
    float t1 = (b.y - discriminant) * reciprocal;
    float t2 = (b.y + discriminant) * reciprocal;
    if (std::abs(a.y) < 1.0f / 65536.0f)
    {
        t1 = p12.y * linearReciprocal;
        t2 = t1;
    }
    return {
        (a.x * t1 - b.x * 2.0f) * t1 + p12.x,
        (a.x * t2 - b.x * 2.0f) * t2 + p12.x
    };
}

static OE_BENCHMARK_NOINLINE SlugSolverVec2 slugSolveHorizontalFast(
    const SlugSolverVec4& p12,
    const SlugSolverVec2& p3)
{
    const SlugSolverVec2 a = {
        p12.x - p12.z * 2.0f + p3.x,
        p12.y - p12.w * 2.0f + p3.y
    };
    const SlugSolverVec2 b = { p12.x - p12.z, p12.y - p12.w };
    float t1;
    float t2;
    if (std::abs(a.y) < 1.0f / 65536.0f)
    {
        t1 = p12.y * (0.5f / b.y);
        t2 = t1;
    }
    else
    {
        const float reciprocal = 1.0f / a.y;
        const float discriminant = std::sqrt(std::max(
            b.y * b.y - a.y * p12.y, 0.0f));
        t1 = (b.y - discriminant) * reciprocal;
        t2 = (b.y + discriminant) * reciprocal;
    }
    return {
        (a.x * t1 - b.x * 2.0f) * t1 + p12.x,
        (a.x * t2 - b.x * 2.0f) * t2 + p12.x
    };
}

template<typename Solver>
static void BM_SlugStraightCurveSolver(benchmark::State& state, Solver solver)
{
    constexpr std::size_t curveCount = 16384u;
    std::vector<SlugSolverVec4> p12(curveCount);
    std::vector<SlugSolverVec2> p3(curveCount);
    for (std::size_t i = 0u; i < curveCount; ++i)
    {
        const float offset = static_cast<float>(i & 255u) / 1024.0f;
        p12[i] = { -0.4f + offset, -0.25f, offset, 0.0f };
        p3[i] = { 0.4f + offset, 0.25f };
    }

    // The optimized branch must remain algebraically identical on the path
    // this benchmark is intended to accelerate.
    const auto reference = slugSolveHorizontalLegacy(p12[0], p3[0]);
    const auto optimized = slugSolveHorizontalFast(p12[0], p3[0]);
    if (reference.x != optimized.x || reference.y != optimized.y)
    {
        state.SkipWithError("Slug straight-curve solver result changed");
        return;
    }

    for (auto _ : state)
    {
        SlugSolverVec2 accumulated = {};
        for (std::size_t i = 0u; i < curveCount; ++i)
        {
            const auto result = solver(p12[i], p3[i]);
            accumulated.x += result.x;
            accumulated.y += result.y;
        }
        benchmark::DoNotOptimize(accumulated);
    }
    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(curveCount));
}

static void BM_SlugStraightCurveSolverLegacy(benchmark::State& state)
{
    BM_SlugStraightCurveSolver(state, slugSolveHorizontalLegacy);
}
BENCHMARK(BM_SlugStraightCurveSolverLegacy)->Unit(benchmark::kMicrosecond);

static void BM_SlugStraightCurveSolverFast(benchmark::State& state)
{
    BM_SlugStraightCurveSolver(state, slugSolveHorizontalFast);
}
BENCHMARK(BM_SlugStraightCurveSolverFast)->Unit(benchmark::kMicrosecond);

#undef OE_BENCHMARK_NOINLINE

BENCHMARK_MAIN();
