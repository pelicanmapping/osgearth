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
#include <osg/KdTree>
#include <osg/Geometry>
#include <osgEarth/TinyBVHShape>
#include <osgEarth/TinyBVHLineSegmentIntersector>
#include <osg/Geode>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <utility>
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

static osg::ref_ptr<osg::Geometry> createTriangleGeometry(unsigned triangleCount)
{
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry();
    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array();
    osg::ref_ptr<osg::DrawElementsUInt> indices =
        new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES);

    vertices->reserve(static_cast<std::size_t>(triangleCount) * 3u);
    indices->reserve(static_cast<std::size_t>(triangleCount) * 3u);

    for (unsigned i = 0u; i < triangleCount; ++i)
    {
        const unsigned x = i % 256u;
        const unsigned y = i / 256u;
        const float z = static_cast<float>((i * 17u) % 31u) * 0.01f;
        const unsigned firstVertex = static_cast<unsigned>(vertices->size());
        vertices->push_back(osg::Vec3(static_cast<float>(x), static_cast<float>(y), z));
        vertices->push_back(osg::Vec3(static_cast<float>(x) + 0.9f, static_cast<float>(y), z + 0.02f));
        vertices->push_back(osg::Vec3(static_cast<float>(x), static_cast<float>(y) + 0.9f, z - 0.01f));
        indices->push_back(firstVertex);
        indices->push_back(firstVertex + 1u);
        indices->push_back(firstVertex + 2u);
    }

    geometry->setVertexArray(vertices.get());
    geometry->addPrimitiveSet(indices.get());
    return geometry;
}

static std::size_t getKdTreeAllocatedMemoryUsage(const osg::KdTree& tree)
{
    return tree.getNodes().capacity() * sizeof(osg::KdTree::KdNode) +
        tree.getPrimitiveIndices().capacity() * sizeof(osg::KdTree::Indices::value_type) +
        tree.getVertexIndices().capacity() * sizeof(osg::KdTree::Indices::value_type);
}

static std::size_t getKdTreeUsedMemoryUsage(const osg::KdTree& tree)
{
    return tree.getNodes().size() * sizeof(osg::KdTree::KdNode) +
        tree.getPrimitiveIndices().size() * sizeof(osg::KdTree::Indices::value_type) +
        tree.getVertexIndices().size() * sizeof(osg::KdTree::Indices::value_type);
}

static void BM_BuildOsgKdTree(benchmark::State& state)
{
    const unsigned triangleCount = static_cast<unsigned>(state.range(0));
    osg::ref_ptr<osg::Geometry> geometry = createTriangleGeometry(triangleCount);

    osg::ref_ptr<osg::KdTree> sample = new osg::KdTree();
    osg::KdTree::BuildOptions sampleOptions;
    if (!sample->build(sampleOptions, geometry.get()))
    {
        state.SkipWithError("osg::KdTree build failed");
        return;
    }
    state.counters["allocated_bytes"] = static_cast<double>(getKdTreeAllocatedMemoryUsage(*sample));
    state.counters["used_bytes"] = static_cast<double>(getKdTreeUsedMemoryUsage(*sample));
    state.counters["nodes"] = static_cast<double>(sample->getNodes().size());
    sample = nullptr;

    for (auto _ : state)
    {
        osg::ref_ptr<osg::KdTree> tree = new osg::KdTree();
        osg::KdTree::BuildOptions options;
        const bool built = tree->build(options, geometry.get());
        benchmark::DoNotOptimize(tree.get());
        benchmark::DoNotOptimize(built);

        state.PauseTiming();
        tree = nullptr;
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * triangleCount);
}

#ifdef OSGEARTH_TINYBVH_USE_BUILD_QUICK
static void BM_BuildTinyBVHQuick(benchmark::State& state)
#else
static void BM_BuildTinyBVHAVX(benchmark::State& state)
#endif
{
    const unsigned triangleCount = static_cast<unsigned>(state.range(0));
    osg::ref_ptr<osg::Geometry> geometry = createTriangleGeometry(triangleCount);

    osg::ref_ptr<osgEarth::Util::TinyBVHShape> sample =
        new osgEarth::Util::TinyBVHShape(geometry.get());
    if (!sample->valid())
    {
        state.SkipWithError("TinyBVH shape build failed");
        return;
    }
    state.counters["allocated_bytes"] = static_cast<double>(sample->getMemoryUsage());
    state.counters["used_bytes"] = static_cast<double>(sample->getMemoryUsage());
    state.counters["nodes"] = static_cast<double>(sample->getNodeCount());
    sample = nullptr;

    for (auto _ : state)
    {
        osg::ref_ptr<osgEarth::Util::TinyBVHShape> shape =
            new osgEarth::Util::TinyBVHShape(geometry.get());
        benchmark::DoNotOptimize(shape.get());
        benchmark::DoNotOptimize(shape->valid());

        state.PauseTiming();
        shape = nullptr;
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * triangleCount);
}

static void BM_BuildTinyBVHLeafSize(benchmark::State& state)
{
    constexpr unsigned triangleCount = 16384u;
    const unsigned leafSize = static_cast<unsigned>(state.range(0));
    osg::ref_ptr<osg::Geometry> geometry = createTriangleGeometry(triangleCount);

    osg::ref_ptr<osgEarth::Util::TinyBVHShape> sample =
        new osgEarth::Util::TinyBVHShape(geometry.get(), leafSize);
    state.counters["bytes"] = static_cast<double>(sample->getMemoryUsage());
    state.counters["nodes"] = static_cast<double>(sample->getNodeCount());

    for (auto _ : state)
    {
        osg::ref_ptr<osgEarth::Util::TinyBVHShape> shape =
            new osgEarth::Util::TinyBVHShape(geometry.get(), leafSize);
        benchmark::DoNotOptimize(shape.get());

        state.PauseTiming();
        shape = nullptr;
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * triangleCount);
}

static bool intersectsTriangle(
    const osg::Vec3d& start,
    const osg::Vec3d& end,
    const osg::Vec3d& v0,
    const osg::Vec3d& v1,
    const osg::Vec3d& v2)
{
    const osg::Vec3d direction = end - start;
    const osg::Vec3d edge1 = v1 - v0;
    const osg::Vec3d edge2 = v2 - v0;
    const osg::Vec3d p = direction ^ edge2;
    const double determinant = edge1 * p;
    if (std::abs(determinant) <= 1e-12)
        return false;

    const double inverseDeterminant = 1.0 / determinant;
    const osg::Vec3d tvec = start - v0;
    const double u = (tvec * p) * inverseDeterminant;
    if (u < 0.0 || u > 1.0)
        return false;

    const osg::Vec3d q = tvec ^ edge1;
    const double v = (direction * q) * inverseDeterminant;
    if (v < 0.0 || u + v > 1.0)
        return false;

    const double ratio = (edge2 * q) * inverseDeterminant;
    return ratio >= 0.0 && ratio <= 1.0;
}

static void BM_IntersectTinyBVHLeafSize(benchmark::State& state)
{
    constexpr unsigned triangleCount = 16384u;
    constexpr unsigned rayCount = 256u;
    const unsigned leafSize = static_cast<unsigned>(state.range(0));
    osg::ref_ptr<osg::Geometry> geometry = createTriangleGeometry(triangleCount);
    osg::ref_ptr<osgEarth::Util::TinyBVHShape> shape =
        new osgEarth::Util::TinyBVHShape(geometry.get(), leafSize);

    std::vector<std::pair<osg::Vec3d, osg::Vec3d>> rays;
    rays.reserve(rayCount);
    for (unsigned r = 0u; r < rayCount; ++r)
    {
        const unsigned primitiveIndex = (r * 61u) % triangleCount;
        const double x = static_cast<double>(primitiveIndex % 256u) + 0.2;
        const double y = static_cast<double>(primitiveIndex / 256u) + 0.2;
        rays.emplace_back(osg::Vec3d(x, y, 10.0), osg::Vec3d(x, y, -10.0));
    }

    std::uint64_t candidates = 0u;
    for (const auto& ray : rays)
        shape->visitSegment(ray.first, ray.second, [&](unsigned) { ++candidates; return true; });
    state.counters["candidates_per_ray"] = static_cast<double>(candidates) / rayCount;
    state.counters["bytes"] = static_cast<double>(shape->getMemoryUsage());

    for (auto _ : state)
    {
        unsigned hits = 0u;
        for (const auto& ray : rays)
        {
            shape->visitSegment(ray.first, ray.second, [&](unsigned primitiveIndex)
            {
                osg::Vec3d v0, v1, v2;
                unsigned i0, i1, i2;
                if (shape->getTriangle(primitiveIndex, v0, v1, v2, i0, i1, i2) &&
                    intersectsTriangle(ray.first, ray.second, v0, v1, v2))
                {
                    ++hits;
                }
                return true;
            });
        }
        benchmark::DoNotOptimize(hits);
    }
    state.SetItemsProcessed(state.iterations() * rayCount);
}

struct IntersectionBenchmarkResult
{
    bool hit = false;
    unsigned primitiveIndex = 0u;
    osg::Vec3d point;
};

struct IntersectionBenchmarkData
{
    explicit IntersectionBenchmarkData(unsigned triangleCount, bool hitRays)
    {
        osg::ref_ptr<osg::Geometry> osgGeometry = createTriangleGeometry(triangleCount);
        osg::ref_ptr<osg::KdTree> kdTree = new osg::KdTree();
        osg::KdTree::BuildOptions options;
        valid = kdTree->build(options, osgGeometry.get());
        if (valid)
        {
            osgStructureBytes = getKdTreeUsedMemoryUsage(*kdTree);
            osgGeometry->setShape(kdTree.get());
            osgGeode = new osg::Geode();
            osgGeode->addDrawable(osgGeometry.get());
        }

        osg::ref_ptr<osg::Geometry> tinyGeometry = createTriangleGeometry(triangleCount);
        osg::ref_ptr<osgEarth::Util::TinyBVHShape> tinyShape =
            new osgEarth::Util::TinyBVHShape(tinyGeometry.get());
        valid = valid && tinyShape->valid();
        if (tinyShape->valid())
        {
            tinyStructureBytes = tinyShape->getMemoryUsage();
            tinyGeometry->setShape(tinyShape.get());
            tinyGeode = new osg::Geode();
            tinyGeode->addDrawable(tinyGeometry.get());
        }

        constexpr unsigned rayCount = 256u;
        rays.reserve(rayCount);
        for (unsigned r = 0u; r < rayCount; ++r)
        {
            const unsigned primitiveIndex = (r * 61u) % triangleCount;
            const double offset = hitRays ? 0.2 : 0.8;
            const double x = static_cast<double>(primitiveIndex % 256u) + offset;
            const double y = static_cast<double>(primitiveIndex / 256u) + offset;
            rays.emplace_back(osg::Vec3d(x, y, 10.0), osg::Vec3d(x, y, -10.0));
        }
    }

    static IntersectionBenchmarkResult intersectOsg(
        osg::Geode* geode,
        const std::pair<osg::Vec3d, osg::Vec3d>& ray)
    {
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector =
            new osgUtil::LineSegmentIntersector(ray.first, ray.second);
        intersector->setIntersectionLimit(osgUtil::Intersector::LIMIT_NEAREST);
        osgUtil::IntersectionVisitor visitor(intersector.get());
        visitor.setUseKdTreeWhenAvailable(true);
        geode->accept(visitor);

        IntersectionBenchmarkResult result;
        result.hit = intersector->containsIntersections();
        if (result.hit)
        {
            const auto& intersection = intersector->getFirstIntersection();
            result.primitiveIndex = intersection.primitiveIndex;
            result.point = intersection.getWorldIntersectPoint();
        }
        return result;
    }

    static IntersectionBenchmarkResult intersectTiny(
        osg::Geode* geode,
        const std::pair<osg::Vec3d, osg::Vec3d>& ray)
    {
        osg::ref_ptr<osgEarth::Util::TinyBVHLineSegmentIntersector> intersector =
            new osgEarth::Util::TinyBVHLineSegmentIntersector(ray.first, ray.second);
        intersector->setIntersectionLimit(osgUtil::Intersector::LIMIT_NEAREST);
        osgUtil::IntersectionVisitor visitor(intersector.get());
        visitor.setUseKdTreeWhenAvailable(true);
        geode->accept(visitor);

        IntersectionBenchmarkResult result;
        result.hit = intersector->containsIntersections();
        if (result.hit)
        {
            const auto& intersection = intersector->getFirstIntersection();
            result.primitiveIndex = intersection.primitiveIndex;
            result.point = intersection.getWorldIntersectPoint();
        }
        return result;
    }

    bool validate() const
    {
        if (!valid)
            return false;

        for (const auto& ray : rays)
        {
            const IntersectionBenchmarkResult osgResult = intersectOsg(osgGeode.get(), ray);
            const IntersectionBenchmarkResult tinyResult = intersectTiny(tinyGeode.get(), ray);
            if (osgResult.hit != tinyResult.hit ||
                (osgResult.hit &&
                    (osgResult.primitiveIndex != tinyResult.primitiveIndex ||
                     (osgResult.point - tinyResult.point).length() > 1e-5)))
            {
                return false;
            }
        }
        return true;
    }

    bool valid = false;
    std::size_t osgStructureBytes = 0u;
    std::size_t tinyStructureBytes = 0u;
    osg::ref_ptr<osg::Geode> osgGeode;
    osg::ref_ptr<osg::Geode> tinyGeode;
    std::vector<std::pair<osg::Vec3d, osg::Vec3d>> rays;
};

static void benchmarkIntersectionVisitor(
    benchmark::State& state,
    bool useTinyBVH,
    bool hitRays)
{
    const unsigned triangleCount = static_cast<unsigned>(state.range(0));
    IntersectionBenchmarkData data(triangleCount, hitRays);
    if (!data.validate())
    {
        state.SkipWithError("OSG KdTree and TinyBVH intersection results differ");
        return;
    }

    state.counters["structure_bytes"] = static_cast<double>(
        useTinyBVH ? data.tinyStructureBytes : data.osgStructureBytes);

    for (auto _ : state)
    {
        unsigned intersections = 0u;
        for (const auto& ray : data.rays)
        {
            const IntersectionBenchmarkResult result = useTinyBVH ?
                IntersectionBenchmarkData::intersectTiny(data.tinyGeode.get(), ray) :
                IntersectionBenchmarkData::intersectOsg(data.osgGeode.get(), ray);
            intersections += result.hit ? 1u : 0u;
        }
        benchmark::DoNotOptimize(intersections);
    }
    state.SetItemsProcessed(state.iterations() * data.rays.size());
}

static void BM_IntersectOsgKdTree_Hit(benchmark::State& state)
{
    benchmarkIntersectionVisitor(state, false, true);
}

static void BM_IntersectTinyBVH_Hit(benchmark::State& state)
{
    benchmarkIntersectionVisitor(state, true, true);
}

static void BM_IntersectOsgKdTree_Miss(benchmark::State& state)
{
    benchmarkIntersectionVisitor(state, false, false);
}

static void BM_IntersectTinyBVH_Miss(benchmark::State& state)
{
    benchmarkIntersectionVisitor(state, true, false);
}

BENCHMARK(BM_BuildOsgKdTree)
    ->Arg(1024)
    ->Arg(16384)
    ->Arg(65536)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

#ifdef OSGEARTH_TINYBVH_USE_BUILD_QUICK
BENCHMARK(BM_BuildTinyBVHQuick)
#else
BENCHMARK(BM_BuildTinyBVHAVX)
#endif
    ->Arg(1024)
    ->Arg(16384)
    ->Arg(65536)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_BuildTinyBVHLeafSize)
    ->Arg(4)
    ->Arg(8)
    ->Arg(16)
    ->Arg(32)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_IntersectTinyBVHLeafSize)
    ->Arg(4)
    ->Arg(8)
    ->Arg(16)
    ->Arg(32)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_IntersectOsgKdTree_Hit)
    ->Arg(1024)
    ->Arg(16384)
    ->Arg(65536)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_IntersectTinyBVH_Hit)
    ->Arg(1024)
    ->Arg(16384)
    ->Arg(65536)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_IntersectOsgKdTree_Miss)
    ->Arg(1024)
    ->Arg(16384)
    ->Arg(65536)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_IntersectTinyBVH_Miss)
    ->Arg(1024)
    ->Arg(16384)
    ->Arg(65536)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
