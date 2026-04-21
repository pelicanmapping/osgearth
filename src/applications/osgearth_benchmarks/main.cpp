/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <benchmark/benchmark.h>

#include <osgEarth/GeoData>
#include <osgEarth/SpatialReference>
#include <osgEarth/StringUtils>
#include <osgEarth/Cache>
#include <osgEarth/ImageUtils>
#include <osgDB/ReadFile>
#include <filesystem>

using namespace osgEarth;
namespace fs = std::filesystem;

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

BENCHMARK_MAIN();
