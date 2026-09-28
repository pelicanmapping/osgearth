/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarthProcedural2/VegetationLayer2>
#include <osgEarth/Map>
#include <osgEarth/NodeUtils>
#include <osgEarth/SimplePager>
#include <atomic>
#include <cmath>
#include <set>
#include <limits>
#include <algorithm>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

namespace
{
    //! Uses a non-square, meter-based extent to catch unit and area mistakes.
    osg::ref_ptr<const Profile> coverage()
    {
        return Profile::create("epsg:32618", 500000,4500000,502000,4501000,"",1,1);
    }

    //! Compares the stable population, including source IDs and all per-instance variation.
    void equalPlacements(const std::vector<ScatterPlacement>& a, const std::vector<ScatterPlacement>& b)
    {
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            CHECK(a[i].id == b[i].id);
            CHECK(a[i].point == b[i].point);
            CHECK(a[i].scale == b[i].scale);
            CHECK(a[i].rotation == b[i].rotation);
        }
    }

    //! Serial test source that fails after producing the second cell to exercise atomic batch failure.
    struct FailingScatterSource : UniformScatterSource
    {
        mutable unsigned requests = 0;
        //! Simulates a late source error, including a partial output the batch must discard.
        Status generate(const TileKey& key, const ScatterGroup& group, unsigned seed,
            std::vector<ScatterPlacement>& output, ProgressCallback* progress) const override
        {
            Status status = UniformScatterSource::generate(key, group, seed, output, progress);
            if (++requests == 2u) return Status(Status::ResourceUnavailable, "Test source failure");
            return status;
        }
    };

    //! Counts source requests so opening a global layer can be checked without a graphics context.
    struct CountingScatterSource : UniformScatterSource
    {
        mutable std::atomic<unsigned> requests{0u};
        //! Records worker requests while retaining normal deterministic scatter behavior.
        Status generate(const TileKey& key, const ScatterGroup& group, unsigned seed,
            std::vector<ScatterPlacement>& output, ProgressCallback* progress) const override
        {
            ++requests;
            return UniformScatterSource::generate(key, group, seed, output, progress);
        }
    };

    //! Records immutable worker policies without creating assets or requiring a graphics context.
    struct PolicyScatterSource : ScatterSource
    {
        mutable std::vector<ScatterGroup> requests;
        //! Test calls are serial; an empty successful cell is enough to exercise the pager's captured policy.
        Status generate(const TileKey&, const ScatterGroup& group, unsigned,
            std::vector<ScatterPlacement>& output, ProgressCallback*) const override
        {
            requests.push_back(group);
            output.clear();
            return Status::NoError;
        }
    };

    //! Retains a named pager so replacement can be checked while simulating an old in-flight request.
    osg::ref_ptr<osgEarth::Util::SimplePager> populationPager(VegetationLayer2* layer, const std::string& name)
    {
        osg::ref_ptr<osgEarth::Util::SimplePager> result;
        forEachNodeOfType<osgEarth::Util::SimplePager>(layer->getNode(),
            [&result, &name](osgEarth::Util::SimplePager* pager)
            { if (pager->getName() == name) result = pager; });
        return result;
    }
}

//! Live edits replace only the selected pager and never mutate the snapshot retained by older paging jobs.
TEST_CASE("Procedural2 population edits isolate paging and reject invalid changes", "[procedural2]")
{
    VegetationLayer2::Options options;
    options.profile() = ProfileOptions("global-geodetic");
    auto source = std::make_shared<PolicyScatterSource>();
    osg::ref_ptr<VegetationLayer2> layer = new VegetationLayer2(options);
    REQUIRE(layer->setSource(source));
    auto tree = options.groups().front();
    tree.density = 2500;
    REQUIRE(layer->setGroup(tree).isOK()); // Closed-layer edit is retained until attachment.
    REQUIRE(layer->open().isOK());
    osg::ref_ptr<Map> map = new Map();
    map->addLayer(layer);
    auto root = layer->getNode();
    auto oldTrees = populationPager(layer, "trees");
    auto shrubs = populationPager(layer, "shrubs");
    auto grass = populationPager(layer, "grass");
    REQUIRE(oldTrees.valid());
    REQUIRE(shrubs.valid());
    REQUIRE(grass.valid());
    CHECK(source->requests.empty());

    auto profile = Profile::create("global-geodetic");
    const TileKey oldKey(tree.renderCellLevel, 0, 0, profile);
    oldTrees->createNode(oldKey, nullptr);
    REQUIRE_FALSE(source->requests.empty());
    CHECK(source->requests.back().density == 2500);

    tree.density = 5000;
    tree.renderCellLevel = 13;
    tree.lodPixels = 64;
    tree.maxRange = 6000;
    tree.densityStart = 500;
    tree.densityEnd = 1500;
    tree.farDensity = 0.5f;
    REQUIRE(layer->setGroup(tree).isOK());
    auto newTrees = populationPager(layer, "trees");
    REQUIRE(newTrees.valid());
    CHECK(newTrees != oldTrees);
    CHECK(layer->getNode() == root);
    CHECK(populationPager(layer, "shrubs") == shrubs);
    CHECK(populationPager(layer, "grass") == grass);
    CHECK(newTrees->getMinLevel() == 13u);
    CHECK(newTrees->getMaxRange() == 6000);
    newTrees->createNode(TileKey(13, 0, 0, profile), nullptr);
    CHECK(source->requests.back().density == 5000);
    CHECK(source->requests.back().lodPixels == 64);
    oldTrees->createNode(oldKey, nullptr); // Simulates a job that already captured the old callback.
    CHECK(source->requests.back().density == 2500);
    CHECK(source->requests.back().lodPixels == options.groups().front().lodPixels);

    // A quality-only edit updates the shared policy without canceling jobs or replacing resident pages.
    tree.qualityOffset = 50.0f;
    REQUIRE(layer->setGroup(tree).isOK());
    CHECK(populationPager(layer,"trees") == newTrees);
    CHECK(populationPager(layer,"grass") == grass);
    osg::Vec2f quality;
    REQUIRE(newTrees->getStateSet()->getUniform("oe_chonk_sse_adjust")->get(quality));
    CHECK(quality.x() == 50.0f);
    CHECK(quality.y() == Approx(1.0f/25.0f));
    REQUIRE(grass->getStateSet()->getUniform("oe_chonk_sse_adjust")->get(quality));
    CHECK(quality.x() == 0.0f);

    auto invalid = tree;
    invalid.renderCellLevel = invalid.cellLevel + 1;
    REQUIRE(layer->setGroup(invalid).isError());
    CHECK(populationPager(layer, "trees") == newTrees);
    CHECK(layer->options().groups().front().renderCellLevel == 13u);
    invalid = tree;
    invalid.asset = "missing";
    REQUIRE(layer->setGroup(invalid).isError());
    invalid = tree;
    invalid.name = "missing";
    REQUIRE(layer->setGroup(invalid).isError());
    CHECK(populationPager(layer, "trees") == newTrees);

    tree.enabled = false;
    REQUIRE(layer->setGroup(tree).isOK());
    CHECK_FALSE(populationPager(layer, "trees").valid());
    CHECK(populationPager(layer, "grass") == grass);
    tree.enabled = true;
    tree.density = 0;
    REQUIRE(layer->setGroup(tree).isOK());
    CHECK_FALSE(populationPager(layer, "trees").valid());
    tree.density = 5000;
    tree.cellLevel = 17;
    REQUIRE(layer->setGroup(tree).isOK());
    newTrees = populationPager(layer, "trees");
    REQUIRE(newTrees.valid());
    newTrees->createNode(TileKey(13, 0, 0, profile), nullptr);
    CHECK(source->requests.back().cellLevel == 17u);
    CHECK(populationPager(layer, "grass") == grass);

    VegetationLayer2::Options restored(layer->options().getConfig());
    CHECK(restored.groups().front().density == 5000);
    CHECK(restored.groups().front().cellLevel == 17u);
    CHECK(restored.groups().front().farDensity == 0.5f);
    map->removeLayer(layer);
    CHECK_FALSE(populationPager(layer, "trees").valid());
    REQUIRE(layer->setGroup(options.groups().front()).isOK());
    map->addLayer(layer);
    CHECK(populationPager(layer, "trees")->getMinLevel() == options.groups().front().renderCellLevel);
    map->removeLayer(layer);
    layer->close();
}

//! Render policies may change residency/visibility, but must never reshuffle or move the source population.
TEST_CASE("Procedural2 density and representation policies preserve placements", "[procedural2]")
{
    auto profile = coverage();
    ScatterGroup group;
    group.cellLevel = 3;
    group.renderCellLevel = 1;
    group.density = 200;
    UniformScatterSource source;
    std::vector<ScatterPlacement> original, adjusted;
    const TileKey key(1,0,0,profile);
    REQUIRE(source.generateBatch(key, group, 17, original).isOK());
    group.lodPixels = 12;
    group.minPixels = 0.5f;
    group.lodTransition = 0.2f;
    group.densityStart = 30;
    group.densityEnd = 180;
    group.farDensity = 0.25f;
    REQUIRE(source.generateBatch(key, group, 17, adjusted).isOK());
    equalPlacements(original, adjusted);
    ScatterGroup restored(group.getConfig());
    REQUIRE(restored.validate().isOK());
    CHECK(restored.lodPixels == group.lodPixels);
    CHECK(restored.minPixels == group.minPixels);
    CHECK(restored.lodTransition == group.lodTransition);
    CHECK(restored.densityStart == group.densityStart);
    CHECK(restored.densityEnd == group.densityEnd);
    CHECK(restored.farDensity == group.farDensity);

    // Sequential feature IDs must distribute ranks too; a source need not supply random IDs.
    unsigned quarter = 0, half = 0;
    for (std::uint64_t id = 0; id < 10000; ++id)
    {
        ScatterPlacement p;
        p.id = id;
        const float rank = p.densityRank();
        REQUIRE(rank >= 0.0f);
        REQUIRE(rank < 1.0f);
        const auto copy = p;
        REQUIRE(copy.densityRank() == rank);
        if (rank < 0.25f) ++quarter;
        if (rank < 0.5f) ++half;
    }
    CHECK(quarter > 2350);
    CHECK(quarter < 2650);
    CHECK(half > 4850);
    CHECK(half < 5150);
    restored.densityEnd = restored.densityStart;
    CHECK(restored.validate().isError());
    restored = group;
    restored.densityEnd = restored.maxRange + 1.0f;
    CHECK(restored.validate().isError());
    restored = group;
    restored.lodPixels = restored.minPixels;
    CHECK(restored.validate().isError());
    restored = group;
    restored.farDensity = -0.1f;
    CHECK(restored.validate().isError());
    restored = group;
    restored.lodTransition = std::numeric_limits<float>::quiet_NaN();
    CHECK(restored.validate().isError());
}

//! Thinned survivors must span each cell, not share a random stream with either placement coordinate.
TEST_CASE("Procedural2 density thinning has no cell coordinate bias", "[procedural2]")
{
    auto profile = coverage();
    ScatterGroup group;
    group.name = group.asset = "grass";
    group.cellLevel = 3;
    group.renderCellLevel = 1;
    group.density = 160000; // 5,000 placements per cell provide a meaningful spatial check.
    UniformScatterSource source;
    for (unsigned seed : {17u, 29u})
    for (unsigned x : {2u, 3u})
    {
        const TileKey key(3, x, 4, profile);
        const auto& extent = key.getExtent();
        std::vector<ScatterPlacement> placements;
        REQUIRE(source.generate(key, group, seed, placements).isOK());
        REQUIRE(placements.size() == 5000u);
        for (float retained : {0.2f, 0.5f})
        {
            unsigned total[16] = {}, survivors[16] = {};
            for (const auto& placement : placements)
            {
                const unsigned bx = std::min(3u, unsigned(4.0 *
                    (placement.point.x() - extent.xMin()) / extent.width()));
                const unsigned by = std::min(3u, unsigned(4.0 *
                    (placement.point.y() - extent.yMin()) / extent.height()));
                const unsigned bin = by * 4u + bx;
                ++total[bin];
                if (placement.densityRank() < retained) ++survivors[bin];
            }
            for (unsigned bin = 0; bin < 16u; ++bin)
            {
                CAPTURE(seed);
                CAPTURE(x);
                CAPTURE(retained);
                CAPTURE(bin);
                REQUIRE(total[bin] > 200u);
                const double fraction = double(survivors[bin]) / double(total[bin]);
                // Wide deterministic tolerance admits normal random variation but rejects spatial bands.
                CHECK(std::abs(fraction - retained) < 0.12);
            }
        }
    }
}

//! Global defaults must make bounded requests at multiple latitudes and on both sides of the longitude seam.
TEST_CASE("Procedural2 global scatter has physical density and finite seam cells", "[procedural2]")
{
    osg::ref_ptr<const Profile> profile = Profile::create("global-geodetic");
    UniformScatterSource source;
    VegetationLayer2::Options options;
    std::vector<ScatterPlacement> output, repeated;
    for (const auto& group : options.groups())
    {
        REQUIRE(group.validate().isOK());
        const auto equator = profile->createTileKey(0.0, 0.0, group.renderCellLevel);
        const auto north = profile->createTileKey(0.0, 60.0, group.renderCellLevel);
        REQUIRE(source.generateBatch(equator, group, 17, output).isOK());
        const auto equatorCount = output.size();
        REQUIRE(equatorCount > 0);
        REQUIRE(source.generateBatch(north, group, 17, output).isOK());
        // Equal angular cells near 60 degrees have approximately half the physical area of equatorial cells.
        CHECK(std::abs(double(output.size()) / double(equatorCount) - 0.5) < 0.06);
        for (const osg::Vec2d location : {osg::Vec2d(179.99999,-17), osg::Vec2d(-179.99999,-17),
            osg::Vec2d(25,70), osg::Vec2d(0,89.999999), osg::Vec2d(0,-89.999999)})
        {
            const auto key = profile->createTileKey(location.x(), location.y(), group.cellLevel);
            REQUIRE(source.generate(key, group, 17, output).isOK());
            REQUIRE(output.size() <= group.maxPerCell);
            const bool inside = std::all_of(output.begin(), output.end(), [&key](const ScatterPlacement& p)
                {
                    return std::isfinite(p.point.x()) && std::isfinite(p.point.y()) &&
                        p.point.x() > key.getExtent().xMin() && p.point.x() < key.getExtent().xMax() &&
                        p.point.y() > key.getExtent().yMin() && p.point.y() < key.getExtent().yMax();
                });
            REQUIRE(inside);
            REQUIRE(source.generate(key, group, 17, repeated).isOK());
            equalPlacements(output, repeated);
        }
    }
    ScatterGroup invalid;
    invalid.cellLevel = 25;
    invalid.renderCellLevel = 24;
    REQUIRE(invalid.validate().isError());
}

//! Opening whole-earth coverage creates only two unloaded roots per group, with no population requests.
TEST_CASE("Procedural2 global coverage starts unloaded", "[procedural2]")
{
    VegetationLayer2::Options options;
    options.profile() = ProfileOptions("global-geodetic");
    auto source = std::make_shared<CountingScatterSource>();
    osg::ref_ptr<VegetationLayer2> layer = new VegetationLayer2(options);
    REQUIRE(layer->setSource(source));
    REQUIRE(layer->open().isOK());
    osg::ref_ptr<Map> map = new Map();
    map->addLayer(layer);
    unsigned roots = 0;
    forEachNodeOfType<osgEarth::Util::PagedNode2>(layer->getNode(),
        [&roots](osgEarth::Util::PagedNode2*) { ++roots; });
    CHECK(roots == 10);
    CHECK(source->requests.load() == 0);
    map->removeLayer(layer);
}

//! Render batching must preserve every source placement, including when the batch subdivision changes.
TEST_CASE("Procedural2 render batches preserve source identity", "[procedural2]")
{
    auto profile = coverage();
    ScatterGroup group;
    group.density = 200;
    group.cellLevel = 3;
    group.renderCellLevel = 1;
    UniformScatterSource source;
    std::vector<ScatterPlacement> batch, expected, cell, subdivided;
    const TileKey key(1,0,0,profile);
    REQUIRE(source.generateBatch(key, group, 17, batch).isOK());
    for (unsigned y = 0; y < 4; ++y)
    for (unsigned x = 0; x < 4; ++x)
    {
        REQUIRE(source.generate(TileKey(3,x,y,profile), group, 17, cell).isOK());
        expected.insert(expected.end(), cell.begin(), cell.end());
    }
    REQUIRE_FALSE(batch.empty());
    equalPlacements(batch, expected);

    group.renderCellLevel = 2;
    for (unsigned i = 0; i < 4; ++i)
    {
        REQUIRE(source.generateBatch(key.createChildKey(i), group, 17, cell).isOK());
        subdivided.insert(subdivided.end(), cell.begin(), cell.end());
    }
    const auto byID = [](const ScatterPlacement& a, const ScatterPlacement& b) { return a.id < b.id; };
    std::sort(batch.begin(), batch.end(), byID);
    std::sort(subdivided.begin(), subdivided.end(), byID);
    equalPlacements(batch, subdivided);
}

//! Aggregate limits, invalid subdivision, cancellation, and late errors must never publish a partial population.
TEST_CASE("Procedural2 render batch failures are bounded and atomic", "[procedural2]")
{
    auto profile = coverage();
    const TileKey key(1,0,0,profile);
    ScatterGroup group;
    group.cellLevel = 3;
    group.renderCellLevel = 1;
    UniformScatterSource source;
    std::vector<ScatterPlacement> output(1);
    group.maxPerBatch = 1;
    REQUIRE(source.generateBatch(key, group, 17, output).isError());
    REQUIRE(output.empty());
    group.maxPerBatch = 262144;
    FailingScatterSource failing;
    REQUIRE(failing.generateBatch(key, group, 17, output).isError());
    REQUIRE(failing.requests == 2);
    REQUIRE(output.empty());
    osg::ref_ptr<ProgressCallback> progress = new ProgressCallback();
    progress->cancel();
    REQUIRE(source.generateBatch(key, group, 17, output, progress).isError());
    REQUIRE(output.empty());
    group.renderCellLevel = group.cellLevel + 1;
    REQUIRE(group.validate().isError());
    group.cellLevel = 16;
    group.renderCellLevel = 1;
    REQUIRE(group.validate().isError());
}

//! Reloads and unrelated group/cell requests must not perturb a deterministic population.
TEST_CASE("Procedural2 scatter is stable across requests and density changes", "[procedural2]")
{
    auto profile = coverage();
    TileKey key(1,0,0,profile);
    UniformScatterSource source;
    ScatterGroup group;
    group.density = 200;
    std::vector<ScatterPlacement> first, second, other;
    REQUIRE(source.generate(key, group, 17, first).isOK());
    REQUIRE(first.size() == 100); // 1km x 0.5km, at 200/km^2
    REQUIRE(source.generate(TileKey(1,1,0,profile), group, 17, other).isOK());
    REQUIRE(source.generate(key, group, 17, second).isOK());
    equalPlacements(first, second);
    group.density *= 2;
    REQUIRE(source.generate(key, group, 17, second).isOK());
    REQUIRE(second.size() == 200);
    second.resize(first.size());
    equalPlacements(first, second);
    group.name = "shrubs";
    REQUIRE(source.generate(key, group, 17, other).isOK());
    REQUIRE(other.front().id != first.front().id);
    REQUIRE(source.generate(key, group, 18, second).isOK());
    REQUIRE(other.front().id != second.front().id);
}

//! Cell ownership must be unambiguous and failures must not expose partial results.
TEST_CASE("Procedural2 scatter owns boundaries and honors cancellation and limits", "[procedural2]")
{
    auto profile = coverage();
    UniformScatterSource source;
    ScatterGroup group;
    std::set<std::uint64_t> ids;
    std::vector<ScatterPlacement> placements;
    for (unsigned x = 0; x < 2; ++x)
    {
        TileKey key(1,x,0,profile);
        REQUIRE(source.generate(key, group, 3, placements).isOK());
        for (const auto& p : placements)
        {
            REQUIRE(ids.insert(p.id).second);
            REQUIRE(p.point.x() > key.getExtent().xMin());
            REQUIRE(p.point.x() < key.getExtent().xMax());
            REQUIRE(p.point.y() > key.getExtent().yMin());
            REQUIRE(p.point.y() < key.getExtent().yMax());
        }
    }
    osg::ref_ptr<ProgressCallback> progress = new ProgressCallback();
    progress->cancel();
    REQUIRE(source.generate(TileKey(1,0,0,profile), group, 3, placements, progress).isError());
    REQUIRE(placements.empty());
    group.maxPerCell = 1;
    REQUIRE(source.generate(TileKey(1,0,0,profile), group, 3, placements).isError());
    REQUIRE(placements.empty());
    group.enabled = false;
    REQUIRE(source.generate(TileKey(1,0,0,profile), group, 3, placements).isOK());
    REQUIRE(placements.empty());
    group.density = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(group.validate().isError());
}

//! Serialization/registration and map lifecycle must preserve all independent groups without the legacy kit.
TEST_CASE("Procedural2 layer registers, round trips, and detaches its group pagers", "[procedural2]")
{
    VegetationLayer2::Options options;
    REQUIRE(options.groups().size() == 5);
    options.profile() = coverage()->toProfileOptions();
    options.groups()[2].enabled = false;
    options.groups()[1].maxRange = 321;
    options.groups()[1].renderCellLevel = 17;
    options.groups()[1].maxPerBatch = 4096;
    VegetationLayer2::Options restored(options.getConfig());
    REQUIRE(restored.groups().size() == 5);
    REQUIRE_FALSE(restored.groups()[2].enabled);
    REQUIRE(restored.groups()[1].maxRange == 321);
    REQUIRE(restored.groups()[1].renderCellLevel == 17);
    REQUIRE(restored.groups()[1].maxPerBatch == 4096);
    Config serialized = restored.getConfig();
    serialized.key() = "Vegetation2";
    auto layer = Layer::create_as<VegetationLayer2>(serialized);
    REQUIRE(layer.valid());
    REQUIRE(layer->open().isOK());
    REQUIRE_FALSE(layer->setSource(std::make_shared<UniformScatterSource>()));
    osg::ref_ptr<Map> map = new Map();
    map->addLayer(layer);
    std::set<std::string> groups;
    forEachNodeOfType<osgEarth::Util::SimplePager>(layer->getNode(),
        [&groups](osgEarth::Util::SimplePager* pager) { groups.insert(pager->getName()); });
    REQUIRE((groups == std::set<std::string>{"trees", "shrubs", "undergrowth", "rocks"}));
    map->removeLayer(layer);
    REQUIRE(layer->getNode()->asGroup()->getNumChildren() == 0);
    layer->close();
    REQUIRE(layer->setSource(std::make_shared<UniformScatterSource>()));
    REQUIRE(layer->open().isOK());
    map->addLayer(layer);
    REQUIRE(layer->getNode()->asGroup()->getNumChildren() == 1);
    map->removeLayer(layer);
}

//! A bounded source profile and unambiguous names are required before paging can start.
TEST_CASE("Procedural2 rejects missing profiles and duplicate groups", "[procedural2]")
{
    osg::ref_ptr<VegetationLayer2> missing = new VegetationLayer2();
    REQUIRE(missing->open().isError());
    VegetationLayer2::Options options;
    options.profile() = coverage()->toProfileOptions();
    options.groups().push_back(options.groups().front());
    osg::ref_ptr<VegetationLayer2> duplicate = new VegetationLayer2(options);
    REQUIRE(duplicate->open().isError());
}
