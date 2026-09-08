/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <benchmark/benchmark.h>

#include <osgEarth/ElevationPool>
#include <osgEarth/GeoData>
#include <osgEarth/HeightFieldUtils>
#include <osgEarth/ImageLayer>
#include <osgEarth/Map>
#include <osgEarth/SpatialReference>
#include <osgEarth/StringUtils>
#include <osgEarth/Cache>
#include <osgEarth/Config>
#include <osgEarth/Coverage>
#include <osgEarth/ImageUtils>
#include <osgEarth/MBTiles>
#include <osgEarth/FileUtils>
#include <osgDB/ReadFile>

using namespace osgEarth;

namespace
{
    struct BenchmarkCoverageValue
    {
        BenchmarkCoverageValue() = default;
        explicit BenchmarkCoverageValue(const Config&) { }

        bool valid() const { return true; }
        bool operator<(const BenchmarkCoverageValue&) const { return false; }
        Config getConfig() const { return Config("value"); }
    };

    std::string makeDelimitedInput(std::size_t count)
    {
        std::string input;
        input.reserve(count * 16u);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (i > 0u)
                input.push_back(',');
            input += (i % 8u == 0u) ? "\"quoted,value\"" : "plain_value";
        }
        return input;
    }

    Config makeCoverageConfig(unsigned width, unsigned height, unsigned runLength)
    {
        Config config("coverage");
        config.set("width", width);
        config.set("height", height);

        std::ostringstream pixels;
        const unsigned size = width * height;
        for (unsigned offset = 0u; offset < size; offset += runLength)
        {
            if (offset > 0u)
                pixels << ' ';
            pixels << std::min(runLength, size - offset) << ' ' << ((offset / runLength) % 4u);
        }
        config.add("pixels", pixels.str());
        return config;
    }

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

    osg::ref_ptr<osg::Image> createReprojectionBenchmarkImage(unsigned int width, unsigned int height)
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
                pixel[3] = 255u;
            }
        }

        return image;
    }

    class ReprojectionBenchmarkImageLayer : public ImageLayer
    {
    public:
        META_LayerNoOptions(osgEarth, ReprojectionBenchmarkImageLayer, ImageLayer, reprojection_benchmark_image);

        void init() override
        {
            ImageLayer::init();
            setProfile(Profile::create(Profile::GLOBAL_GEODETIC));
            setTileSize(256u);
            setMaxDataLevel(18u);
            setCachePolicy(CachePolicy::NO_CACHE);
        }

        Status openImplementation() override
        {
            Status parent = ImageLayer::openImplementation();
            if (parent.isError())
                return parent;

            _image = createReprojectionBenchmarkImage(256u, 256u);
            return parent;
        }

        GeoImage createImageImplementation(const TileKey& key, ProgressCallback*) const override
        {
            return GeoImage(_image.get(), key.getExtent());
        }

    protected:
        virtual ~ReprojectionBenchmarkImageLayer() { }

    private:
        osg::ref_ptr<osg::Image> _image;
    };
}

static void BM_StringTokenizerDelimited(benchmark::State& state)
{
    const std::string input = makeDelimitedInput(static_cast<std::size_t>(state.range(0)));
    const StringTokenizer tokenizer = StringTokenizer().delim(",").standardQuotes();

    for (auto _ : state)
    {
        auto output = tokenizer.tokenize(input);
        benchmark::DoNotOptimize(output.data());
        benchmark::DoNotOptimize(output.size());
    }
}
BENCHMARK(BM_StringTokenizerDelimited)->Arg(4096);

static void BM_ConfigGetNumeric(benchmark::State& state)
{
    Config config;
    config.set("integer", 123456789);
    config.set("single", 12345.625f);
    config.set("double", 123456789.125);

    for (auto _ : state)
    {
        int integer = 0;
        float single = 0.0f;
        double doubleValue = 0.0;
        config.get("integer", integer);
        config.get("single", single);
        config.get("double", doubleValue);
        benchmark::DoNotOptimize(integer);
        benchmark::DoNotOptimize(single);
        benchmark::DoNotOptimize(doubleValue);
    }
}
BENCHMARK(BM_ConfigGetNumeric);

static void BM_CoverageSetConfig(benchmark::State& state)
{
    const Config config = makeCoverageConfig(256u, 256u, static_cast<unsigned>(state.range(0)));

    for (auto _ : state)
    {
        auto coverage = Coverage<BenchmarkCoverageValue>::create();
        coverage->setConfig(config);
        benchmark::DoNotOptimize(coverage.get());
        benchmark::DoNotOptimize(coverage->nodataCount());
    }
}
BENCHMARK(BM_CoverageSetConfig)->Arg(1)->Arg(8)->Arg(256);

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

static void BM_ImageLayerAssembleImage_Reproject(benchmark::State& state)
{
    osg::ref_ptr<ReprojectionBenchmarkImageLayer> layer = new ReprojectionBenchmarkImageLayer();
    Status status = layer->open();
    if (status.isError())
    {
        state.SkipWithError(status.toString().c_str());
        return;
    }

    osg::ref_ptr<const Profile> outputProfile = Profile::create(Profile::GLOBAL_MERCATOR);
    TileKey key(4u, 8u, 5u, outputProfile.get());

    GeoImage warmup = layer->createImage(key, nullptr);
    if (!warmup.valid())
    {
        state.SkipWithError("ImageLayer reprojection benchmark failed to create warmup tile");
        return;
    }

    for (auto _ : state)
    {
        GeoImage image = layer->createImage(key, nullptr);
        if (!image.valid())
        {
            state.SkipWithError("ImageLayer reprojection benchmark failed to create tile");
            return;
        }

        benchmark::DoNotOptimize(image.getImage());
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ImageLayerAssembleImage_Reproject)->Unit(benchmark::kMillisecond);



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
    Util::removeDirectory(CACHE_PATH);
}
// Superseded by the controlled Cache/* benchmark matrix in CacheBenchmarks.cpp.

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
    Util::removeDirectory(CACHE_PATH);
}


static void BM_SQLite3SingleThreadedRead(benchmark::State& state)
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
    Util::removeDirectory(CACHE_PATH);
}

namespace
{
    struct SQLite3ConcurrentReadFixture
    {
        SQLite3ConcurrentReadFixture()
        {
            path = "sqlite_concurrent_read_cache";
            Util::removeDirectory(path);

            Config config;
            config.fromJSON("{ \"path\": \"" + path + "\" }");
            CacheOptions cacheOptions(config);
            cacheOptions.setDriver("sqlite3");

            osg::ref_ptr<Cache> writerCache = CacheFactory::create(cacheOptions);
            osg::ref_ptr<CacheBin> writerBin = writerCache->getOrCreateDefaultBin();
            osg::ref_ptr<osg::Image> image = osgDB::readRefImageFile(CACHE_IMAGE);
            for (unsigned i = 0u; i < NUM_CACHE_IMAGES; ++i)
            {
                writerBin->write("image_" + std::to_string(i), image.get(), nullptr);
            }

            writerBin = nullptr;
            writerCache = nullptr; // drain all writes before opening the read fixture

            cache = CacheFactory::create(cacheOptions);
            bin = cache->getOrCreateDefaultBin();
        }

        ~SQLite3ConcurrentReadFixture()
        {
            bin = nullptr;
            cache = nullptr;
            Util::removeDirectory(path);
        }

        std::string path;
        osg::ref_ptr<Cache> cache;
        osg::ref_ptr<CacheBin> bin;
    };

    SQLite3ConcurrentReadFixture& sqlite3ConcurrentReadFixture()
    {
        static SQLite3ConcurrentReadFixture s_fixture;
        return s_fixture;
    }
}

static void BM_SQLite3ConcurrentRead(benchmark::State& state)
{
    auto& fixture = sqlite3ConcurrentReadFixture();
    std::uint64_t index = static_cast<std::uint64_t>(state.thread_index());

    for (auto _ : state)
    {
        const std::string key = "image_" + std::to_string(index++ % NUM_CACHE_IMAGES);
        ReadResult result = fixture.bin->readImage(key, nullptr);
        if (!result.succeeded())
        {
            state.SkipWithError("SQLite3 benchmark failed to read cached image");
            return;
        }
        benchmark::DoNotOptimize(result.getImage());
    }
}

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
    Util::removeDirectory(CACHE_PATH);
}


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
