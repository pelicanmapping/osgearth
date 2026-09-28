/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarthProcedural2/FeaturePlacement>
#include <osgEarthProcedural2/Canopy>
#include <osgEarthProcedural2/CanopyTransition.h>
#include <osgEarth/SimplePager>
#include <osgEarthProcedural2/VegetationLayer2>
#include <osgEarth/OGRFeatureSource>
#include <osgEarth/XYZFeatureSource>
#include <osgEarth/Query>
#include <osgEarth/NodeUtils>
#include <osgEarth/CameraUtils>
#include <osg/Texture2D>
#include <limits>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>
#include <algorithm>
#include <future>
#include <iostream>
#include <map>
#include <cmath>
#include <set>
#include <osg/io_utils>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

namespace
{
    //! Supplies immutable geometry without network or graphics dependencies.
    struct FixtureProvider : PlacementFeatureProvider
    {
        std::vector<PlacementFeature> features;
        bool fail = false;
        //! Failure is atomic, matching the adapter contract; test data is already query-local.
        Status query(const TileKey&, double, std::vector<PlacementFeature>& output, ProgressCallback* progress) const override
        {
            output.clear();
            if (fail || (progress && progress->isCanceled()))
                return Status(Status::ResourceUnavailable, "Fixture canceled or unavailable");
            output = features;
            return Status::NoError;
        }
    };

    //! Uses XYZ metadata with an empty in-memory cursor; records native requests without any network activity.
    struct RecordingFeatureSource : XYZFeatureSource
    {
        mutable std::vector<TileKey> requested;
        //! Test queries are serial. An empty successful tile exercises profile translation and halo ownership.
        FeatureCursor* createFeatureCursorImplementation(const Query& query, ProgressCallback*) const override
        {
            requested.push_back(query.tileKey().get());
            return new FeatureListCursor(FeatureList());
        }
    };

    //! Uses a metric profile to make narrow exclusion widths directly checkable.
    osg::ref_ptr<const Profile> featureProfile()
    {
        return Profile::create("epsg:32618", 500000, 4500000, 500400, 4500400, "", 1, 1);
    }

    //! Creates an axis-aligned polygon in the fixture's metric frame.
    osg::ref_ptr<Polygon> rectangle(double xmin, double ymin, double xmax, double ymax)
    {
        auto p = new Polygon();
        p->push_back({xmin,ymin,0}); p->push_back({xmax,ymin,0});
        p->push_back({xmax,ymax,0}); p->push_back({xmin,ymax,0});
        return p;
    }

    //! Tags one fixture geometry while retaining stable IDs and source namespace.
    PlacementFeature tagged(Geometry* geometry, const char* key, const char* value, FeatureID id = 1)
    {
        osg::ref_ptr<Feature> f = new Feature(geometry, featureProfile()->getSRS(), Style(), id);
        f->set(key, value);
        return {"fixture", f};
    }

    //! Makes a generic attribute rule; no OSM-specific logic lives in the placement algorithm.
    CoverageRule rule(const char* key, const char* value, const char* action = "include", double buffer = 0.0)
    {
        CoverageRule r;
        r.key = key; r.value = value; r.action = action; r.buffer = buffer;
        return r;
    }

    //! Returns a policy spanning sixteen source cells in one batch.
    ScatterGroup population()
    {
        ScatterGroup group;
        group.cellLevel = 3; group.renderCellLevel = 1; group.density = 200000;
        return group;
    }

    //! Compares identity and position without depending on batch enumeration order.
    void samePopulation(const std::vector<ScatterPlacement>& a, const std::vector<ScatterPlacement>& b)
    {
        REQUIRE(a.size() == b.size());
        std::map<std::uint64_t, osg::Vec3d> positions;
        for (const auto& p : a) REQUIRE(positions.emplace(p.id, p.point).second);
        for (const auto& p : b)
        {
            auto found = positions.find(p.id);
            REQUIRE(found != positions.end());
            CHECK(found->second == p.point);
        }
    }
    //! Anchors a rotated layout at the metric fixture origin; no page/fragment centroid participates.
    CoverageRule rowRule()
    {
        auto r = rule("landuse", "vineyard");
        r.name = "vines"; r.strategy = "rows";
        const auto origin = GeoPoint(featureProfile()->getSRS(), 500000, 4500000, 0).transform(
            SpatialReference::get("wgs84"));
        RowLayout layout;
        layout.longitude = origin.x(); layout.latitude = origin.y();
        layout.heading = 27.0; layout.rowSpacing = 8.0; layout.plantSpacing = 4.0;
        r.parameters = layout.getConfig();
        return r;
    }

    //! A deliberately unrelated application algorithm proves dispatch is open beyond built-in land-use types.
    struct FixedLayout : RegionPlacementStrategy
    {
        //! Reads strategy-owned config without requiring a change to the generic CoverageRule schema.
        Status validate(const Config& config) const override
        {
            return config.value("fixture", false) ? Status::NoError :
                Status(Status::ConfigurationError, "Missing fixed-layout fixture setting");
        }
        //! Emits source-space slots; shared policy must discard the excluded and out-of-region candidates.
        Status generate(const TileKey&, const ScatterGroup&, unsigned, const PlacementRegion& region,
            std::vector<ScatterPlacement>& output, ProgressCallback*) const override
        {
            output.clear();
            for (unsigned i = 0; i < 4; ++i)
            {
                ScatterPlacement p;
                p.id = region.id + i;
                p.point = osg::Vec3d(500050 + 50*i, 4500300, 0);
                output.push_back(p);
            }
            return Status::NoError;
        }
    };
}

//! Tests holes, hard exclusions, sub-grid road widths, and stable batches against independent geometric predicates.
TEST_CASE("Procedural2 geographic coverage preserves holes and narrow exclusions", "[procedural2][procedural2-features]")
{
    auto provider = std::make_shared<FixtureProvider>();
    auto forest = rectangle(500000,4500000,500400,4500400);
    auto hole = rectangle(500070,4500250,500100,4500280);
    forest->getHoles().push_back(hole);
    auto building = rectangle(500120,4500300,500160,4500340);
    auto road = new LineString();
    road->push_back({500030,4500000,0}); road->push_back({500030,4500400,0});
    provider->features = {tagged(forest,"natural","wood"), tagged(building,"building","yes",2),
        tagged(road,"highway","track",3)};
    std::vector<CoverageRule> rules{rule("natural","wood"), rule("building","*","exclude"),
        rule("highway","*","exclude",1.5)};
    FeatureScatterSource source(provider, rules);
    auto group = population();
    auto profile = featureProfile();
    TileKey key(1,0,0,profile);
    std::vector<ScatterPlacement> a,b,raw;
    REQUIRE(source.generateBatch(key,group,9,a).isOK());
    REQUIRE(a.size() > 500u);
    REQUIRE(UniformScatterSource().generateBatch(key,group,9,raw).isOK());
    std::vector<ScatterPlacement> expected;
    for (const auto& p : raw)
        if (!hole->contains2D(p.point.x(),p.point.y()) && !building->contains2D(p.point.x(),p.point.y()) &&
            std::abs(p.point.x()-500030) > 1.5) expected.push_back(p);
    samePopulation(a,expected);
    group.renderCellLevel = 2;
    for (unsigned y=0;y<2;++y)
    for (unsigned x=0;x<2;++x)
    {
        std::vector<ScatterPlacement> cell;
        REQUIRE(source.generateBatch(TileKey(2,x,y,profile),group,9,cell).isOK());
        b.insert(b.end(),cell.begin(),cell.end());
    }
    samePopulation(a,b);
    CHECK(forest->getHoles().size() == 1u); // worker transforms must not mutate provider-owned geometry
}

//! Tests union semantics and a stable fractional-density subset instead of multiplying overlapping forest coverage.
TEST_CASE("Procedural2 coverage composition preserves density identity", "[procedural2][procedural2-features]")
{
    auto provider = std::make_shared<FixtureProvider>();
    provider->features.push_back(tagged(rectangle(500000,4500000,500400,4500400),"natural","wood"));
    auto include = rule("natural","wood");
    auto group = population();
    TileKey key(1,0,0,featureProfile());
    std::vector<ScatterPlacement> full,overlap,half,reloaded;
    REQUIRE(FeatureScatterSource(provider,{include}).generateBatch(key,group,17,full).isOK());
    provider->features.push_back(provider->features.front());
    provider->features.back().source = "another-source";
    REQUIRE(FeatureScatterSource(provider,{include}).generateBatch(key,group,17,overlap).isOK());
    samePopulation(full,overlap);
    include.density = 0.5f;
    FeatureScatterSource reduced(provider,{include});
    REQUIRE(reduced.generateBatch(key,group,17,half).isOK());
    CHECK(half.size() > full.size()*0.4);
    CHECK(half.size() < full.size()*0.6);
    std::set<std::uint64_t> ids;
    for (const auto& p : full) ids.insert(p.id);
    for (const auto& p : half) REQUIRE(ids.count(p.id) == 1u);
    REQUIRE(reduced.generateBatch(key,group,17,reloaded).isOK());
    samePopulation(half,reloaded);
    auto worker = std::async(std::launch::async, [&]()
        {
            std::vector<ScatterPlacement> result;
            reduced.generateBatch(key,group,17,result);
            return result;
        });
    samePopulation(half,worker.get());
}

//! Tests explicit points outside polygons, duplicated input records, and ownership exactly on cell borders.
TEST_CASE("Procedural2 explicit trees obey exclusions and cell ownership", "[procedural2][procedural2-features]")
{
    auto provider = std::make_shared<FixtureProvider>();
    auto points = new PointSet();
    points->push_back({500100,4500300,0}); points->push_back({500120,4500300,0});
    provider->features.push_back(tagged(points,"natural","tree",55));
    provider->features.push_back(provider->features.front());
    provider->features.push_back(tagged(rectangle(500115,4500295,500125,4500305),"natural","water",56));
    auto group=population(); auto profile=featureProfile();
    FeatureScatterSource source(provider,{rule("natural","tree","points"),rule("natural","water","exclude")});
    std::vector<ScatterPlacement> batch,cells;
    REQUIRE(source.generateBatch(TileKey(1,0,0,profile),group,5,batch).isOK());
    REQUIRE(batch.size() == 1u);
    CHECK(batch[0].point == osg::Vec3d(500100,4500300,0));
    for (unsigned y=0;y<4;++y)
    for (unsigned x=0;x<4;++x)
    {
        std::vector<ScatterPlacement> cell;
        REQUIRE(source.generate(TileKey(3,x,y,profile),group,5,cell).isOK());
        cells.insert(cells.end(),cell.begin(),cell.end());
    }
    samePopulation(batch,cells);
}

//! Tests atomic failure, cancellation, unknown source references, and rule serialization without graphics.
TEST_CASE("Procedural2 feature configuration and failures are explicit", "[procedural2][procedural2-features]")
{
    auto provider=std::make_shared<FixtureProvider>();
    auto policy=rule("highway","*","exclude",4.0);
    policy.exceptValue="no"; policy.source="osm"; policy.group="trees";
    auto copy=CoverageRule(policy.getConfig());
    CHECK(copy.buffer==4.0); CHECK(copy.exceptValue=="no"); CHECK(copy.source=="osm");
    REQUIRE(copy.validate().isOK());
    osg::ref_ptr<Feature> nullable = new Feature(rectangle(0,0,10,10), featureProfile()->getSRS());
    nullable->setNull("highway");
    CHECK_FALSE(copy.matches("osm", population(), *nullable));
    nullable->set("highway", "no");
    CHECK_FALSE(copy.matches("osm", population(), *nullable));
    nullable->set("highway", "track");
    CHECK(copy.matches("osm", population(), *nullable));
    copy.density=2.0f; CHECK(copy.validate().isError());
    std::vector<ScatterPlacement> output(2);
    provider->fail=true;
    FeatureScatterSource source(provider,{rule("natural","wood")});
    REQUIRE(source.generateBatch(TileKey(1,0,0,featureProfile()),population(),1,output).isError());
    CHECK(output.empty());
    provider->fail=false;
    osg::ref_ptr<ProgressCallback> canceled=new ProgressCallback(); canceled->cancel();
    REQUIRE(source.generateBatch(TileKey(1,0,0,featureProfile()),population(),1,output,canceled).isError());
    CHECK(output.empty());
    VegetationLayer2::Options options;
    options.profile()=ProfileOptions("global-geodetic");
    options.coverage().push_back(policy);
    osg::ref_ptr<VegetationLayer2> layer=new VegetationLayer2(options);
    CHECK(layer->open().isError());
}

//! Exercises the real non-tiled FeatureSource adapter with a checked-in GeoJSON source rather than only a mock.
TEST_CASE("Procedural2 reads local feature sources through the shared adapter", "[procedural2][procedural2-features]")
{
    osg::ref_ptr<OGRFeatureSource> features=new OGRFeatureSource();
    features->setURL(URI("../data/procedural2/coverage/fixture.geojson"));
    REQUIRE(features->open().isOK());
    FeatureInput input; input.name="fixture"; input.features.setLayer(features);
    auto provider=std::make_shared<MapFeatureProvider>(std::vector<FeatureInput>{input});
    osg::ref_ptr<const Profile> profile=Profile::create("global-geodetic");
    auto key=profile->createTileKey(-75.0,40.65,16);
    std::vector<PlacementFeature> records;
    REQUIRE(provider->query(key,5,records,nullptr).isOK());
    CHECK(records.size() >= 3u);
    auto group=population(); group.cellLevel=group.renderCellLevel=16;
    FeatureScatterSource source(provider,{rule("natural","wood"),rule("natural","water","exclude"),
        rule("building","*","exclude"),rule("highway","*","exclude",4.0)});
    std::vector<ScatterPlacement> placements;
    REQUIRE(source.generateBatch(key,group,17,placements).isOK());
    CHECK(!placements.empty());
}

//! Rejects an accidental world-scale native query before allocating or requesting millions of source tiles.
TEST_CASE("Procedural2 bounds native tile queries before enumeration", "[procedural2][procedural2-features]")
{
    osg::ref_ptr<XYZFeatureSource> features=new XYZFeatureSource();
    features->setURL(URI("../data/procedural2/coverage/missing/{z}/{x}/{y}.pbf"));
    features->setFormat("pbf");
    features->options().profile()=ProfileOptions("spherical-mercator");
    features->options().minLevel()=14; features->options().maxLevel()=14;
    REQUIRE(features->open().isOK());
    FeatureInput input; input.name="bounded"; input.features.setLayer(features);
    MapFeatureProvider provider({input});
    osg::ref_ptr<const Profile> profile=Profile::create("global-geodetic");
    std::vector<PlacementFeature> records;
    const auto status=provider.query(TileKey(1,0,0,profile),4,records,nullptr);
    CHECK(status.isError());
    CHECK(status.message().find("64 native source tiles") != std::string::npos);
    CHECK(records.empty());
}

//! A small date-line halo must request both native edges without expanding to a global query.
TEST_CASE("Procedural2 source halos cross the antimeridian", "[procedural2][procedural2-features]")
{
    osg::ref_ptr<RecordingFeatureSource> features=new RecordingFeatureSource();
    features->setURL(URI("unused/{z}/{x}/{y}.pbf")); features->setFormat("pbf");
    features->options().profile()=ProfileOptions("spherical-mercator");
    features->options().minLevel()=14; features->options().maxLevel()=14;
    REQUIRE(features->open().isOK());
    FeatureInput input; input.name="seam"; input.features.setLayer(features);
    MapFeatureProvider provider({input});
    osg::ref_ptr<const Profile> profile=Profile::create("global-geodetic");
    std::vector<PlacementFeature> records;
    const auto status=provider.query(profile->createTileKey(179.99999,60.0,18),8,records,nullptr);
    INFO(status.toString());
    REQUIRE(status.isOK());
    bool west=false, east=false;
    for (const auto& key : features->requested)
    {
        CHECK(key.getLOD()==14u);
        west=west || key.getTileX()==0u;
        east=east || key.getTileX()==16383u;
    }
    CHECK(west); CHECK(east); CHECK(features->requested.size()<=4u);
}

//! Opt-in network check of the exact OSM endpoint/profile used by tests/osm.earth; excluded from offline runs.
TEST_CASE("Procedural2 OSM FeatureSource live smoke", "[.][procedural2-osm]")
{
    osg::ref_ptr<XYZFeatureSource> features=new XYZFeatureSource();
    features->setURL(URI("https://readymap.org/readymap/mbtiles/osm/{z}/{x}/{-y}.pbf"));
    features->setFormat("pbf");
    features->setFIDAttribute("@id");
    features->options().profile()=ProfileOptions("spherical-mercator");
    features->options().minLevel()=14; features->options().maxLevel()=14;
    REQUIRE(features->open().isOK());
    FeatureInput input; input.name="osm"; input.features.setLayer(features);
    MapFeatureProvider provider({input});
    osg::ref_ptr<const Profile> profile=Profile::create("global-geodetic");
    std::vector<PlacementFeature> records;
    REQUIRE(provider.query(profile->createTileKey(24.92,60.22,14),8,records,nullptr).isOK());
    REQUIRE(!records.empty());
    unsigned woods=0, trees=0;
    for (const auto& r : records)
    {
        const auto& f=*r.feature;
        if (f.getString("natural")=="wood" || f.getString("landuse")=="forest")
        {
            ++woods;
            if (woods<=4) std::cout << "OSM woodland center: " << f.getExtent().getCentroid().transform(
                SpatialReference::get("wgs84")).vec3d() << std::endl;
        }
        if (f.getString("natural")=="tree") ++trees;
    }
    std::cout << "OSM features: " << records.size() << ", woodland parts: " << woods << ", trees: " << trees << std::endl;
    REQUIRE(woods>0u);
    auto placementProvider=std::make_shared<MapFeatureProvider>(std::vector<FeatureInput>{input});
    FeatureScatterSource source(placementProvider,{rule("natural","wood"),rule("landuse","forest"),
        rule("natural","tree","points"),rule("building","*","exclude"),
        rule("natural","water","exclude"),rule("highway","*","exclude",4.0)});
    auto group=population(); group.density=2000; group.cellLevel=16; group.renderCellLevel=14;
    std::vector<ScatterPlacement> placements;
    const auto status=source.generateBatch(profile->createTileKey(24.92,60.22,14),group,17,placements);
    INFO(status.toString());
    REQUIRE(status.isOK());
    std::cout << "OSM accepted tree placements: " << placements.size() << std::endl;
    REQUIRE(!placements.empty());
}

//! Rows retain metric spacing, IDs, exclusions, and phase across source/render levels and clipped native fragments.
TEST_CASE("Procedural2 rows cross page and feature boundaries without restarting", "[procedural2][procedural2-rows]")
{
    auto provider = std::make_shared<FixtureProvider>();
    auto polygon = rectangle(500000,4500000,500400,4500400);
    auto hole = rectangle(500040,4500280,500075,4500310);
    polygon->getHoles().push_back(hole);
    auto road = new LineString();
    road->push_back({500130,4500000,0}); road->push_back({500130,4500400,0});
    provider->features = {tagged(polygon,"landuse","vineyard",42), tagged(road,"highway","track",9)};
    const auto rows = rowRule();
    const std::vector<CoverageRule> rules{rows,rule("highway","*","exclude",3)};
    FeatureScatterSource source(provider,rules);
    auto profile = featureProfile(); auto group = population();
    std::vector<ScatterPlacement> full,cells,clipped;
    REQUIRE(source.generateBatch(TileKey(1,0,0,profile),group,17,full).isOK());
    REQUIRE(full.size() > 800u);
    RowPattern pattern;
    REQUIRE(RowLayout(rows.parameters).resolve(*provider->features[0].feature,group,pattern).isOK());
    for (const auto& p : full)
    {
        osg::Vec3d metric;
        REQUIRE(profile->getSRS()->transform(p.point,pattern.frame,metric));
        const double across = (metric.x()*std::cos(pattern.heading)-metric.y()*std::sin(pattern.heading))/pattern.rowSpacing;
        const double along = (metric.x()*std::sin(pattern.heading)+metric.y()*std::cos(pattern.heading))/pattern.plantSpacing;
        CHECK(std::abs(across-std::round(across)) < 1e-6);
        CHECK(std::abs(along-std::round(along)) < 1e-6);
        CHECK(!hole->contains2D(p.point.x(),p.point.y()));
        CHECK(std::abs(p.point.x()-500130) > 3.0);
    }
    group.cellLevel = 4; group.renderCellLevel = 2;
    for (unsigned y = 0; y < 2; ++y)
    for (unsigned x = 0; x < 2; ++x)
    {
        std::vector<ScatterPlacement> batch;
        REQUIRE(source.generateBatch(TileKey(2,x,y,profile),group,17,batch).isOK());
        cells.insert(cells.end(),batch.begin(),batch.end());
    }
    samePopulation(full,cells);
    // Simulate native tile clipping: identity and explicit origin survive, although each fragment's centroid changes.
    auto left = rectangle(500000,4500000,500100,4500400);
    left->getHoles().push_back(hole);
    provider->features[0] = tagged(left,"landuse","vineyard",42);
    provider->features.push_back(tagged(rectangle(500100,4500000,500400,4500400),"landuse","vineyard",42));
    provider->features.push_back(provider->features.back()); // duplicate native records must not double rows
    group = population();
    REQUIRE(source.generateBatch(TileKey(1,0,0,profile),group,17,clipped).isOK());
    samePopulation(full,clipped);
    std::reverse(provider->features.begin(),provider->features.end());
    REQUIRE(source.generateBatch(TileKey(1,0,0,profile),group,17,cells).isOK());
    samePopulation(full,cells);
    auto concurrent = std::async(std::launch::async, [&]()
        {
            std::vector<ScatterPlacement> result;
            if (source.generateBatch(TileKey(1,0,0,profile),group,17,result).isError()) result.clear();
            return result;
        });
    REQUIRE(source.generateBatch(TileKey(1,0,0,profile),group,17,cells).isOK());
    samePopulation(cells,concurrent.get());
}

//! Structured occupancy changes remove stable slots; priority replaces natural scatter without softening exclusions.
TEST_CASE("Procedural2 rows compose with scatter and stable occupancy", "[procedural2][procedural2-rows]")
{
    auto provider = std::make_shared<FixtureProvider>();
    provider->features = {tagged(rectangle(500000,4500000,500400,4500400),"landuse","vineyard",42),
        tagged(rectangle(500000,4500000,500400,4500400),"natural","wood",43)};
    auto rows = rowRule(); auto forest = rule("natural","wood");
    auto group = population(); TileKey key(1,0,0,featureProfile());
    FeatureScatterSource source(provider,{forest,rows});
    std::vector<ScatterPlacement> full,half,restored,expected;
    REQUIRE(source.generateBatch(key,group,3,full).isOK());
    REQUIRE(FeatureScatterSource(provider,{rows}).generateBatch(key,group,3,expected).isOK());
    samePopulation(full,expected); // rows win equal priority instead of layering random plants into them
    group.rowDensity = 0.5f;
    REQUIRE(source.generateBatch(key,group,3,half).isOK());
    CHECK(half.size() > full.size()*0.4); CHECK(half.size() < full.size()*0.6);
    std::map<std::uint64_t,osg::Vec3d> positions;
    for (const auto& p : full) positions.emplace(p.id,p.point);
    for (const auto& p : half) { REQUIRE(positions.count(p.id)==1u); CHECK(positions.at(p.id)==p.point); }
    group.rowDensity = 1.0f;
    REQUIRE(source.generateBatch(key,group,3,restored).isOK()); samePopulation(full,restored);
    group.density *= 2.0;
    REQUIRE(source.generateBatch(key,group,3,restored).isOK()); samePopulation(full,restored);
    group.rowDensity = 0.0f;
    REQUIRE(source.generateBatch(key,group,3,restored).isOK()); CHECK(restored.empty());
    group = population(); forest.priority = 10;
    REQUIRE(FeatureScatterSource(provider,{forest,rows}).generateBatch(key,group,3,restored).isOK());
    REQUIRE(FeatureScatterSource(provider,{forest}).generateBatch(key,group,3,expected).isOK());
    samePopulation(restored,expected);
}

//! Bad layout metadata and oversized requests fail atomically; attributes override fallback values deterministically.
TEST_CASE("Procedural2 row metadata and request limits are explicit", "[procedural2][procedural2-rows]")
{
    auto provider = std::make_shared<FixtureProvider>();
    auto feature = tagged(rectangle(500000,4500000,500400,4500400),"landuse","vineyard",42);
    auto rows = rowRule();
    RowLayout layout(rows.parameters); layout.headingAttribute = "orientation";
    auto mutableFeature = new Feature(*feature.feature); mutableFeature->set("orientation", "90");
    feature.feature = mutableFeature; provider->features = {feature};
    RowPattern pattern;
    REQUIRE(layout.resolve(*feature.feature,population(),pattern).isOK());
    CHECK(std::abs(pattern.heading-1.5707963267948966)<1e-12);
    rows.parameters = layout.getConfig();
    CoverageRule copy(rows.getConfig());
    CHECK(copy.strategy == "rows"); CHECK(copy.name == rows.name);
    CHECK(RowLayout(copy.parameters).headingAttribute == "orientation");
    CHECK(RowLayout().validate().isError()); // no implicit centroid fallback for clipped OSM polygons
    std::vector<ScatterPlacement> output(3);
    auto group = population(); TileKey key(1,0,0,featureProfile());
    mutableFeature->set("orientation", "east-ish");
    CHECK(FeatureScatterSource(provider,{rows}).generateBatch(key,group,1,output).isError()); CHECK(output.empty());
    mutableFeature->set("orientation", "90"); group.maxPerCell = 1;
    CHECK(FeatureScatterSource(provider,{rows}).generateBatch(key,group,1,output).isError()); CHECK(output.empty());
    group = population(); group.cellLevel=group.renderCellLevel=1;
    layout.rowSpacing=layout.plantSpacing=0.1; rows.parameters=layout.getConfig();
    CHECK(FeatureScatterSource(provider,{rows}).generateBatch(key,group,1,output).isError()); CHECK(output.empty());
    rows = rowRule();
    osg::ref_ptr<ProgressCallback> canceled = new ProgressCallback(); canceled->cancel();
    CHECK(FeatureScatterSource(provider,{rows}).generateBatch(key,group,1,output,canceled).isError());
    CHECK(output.empty());
    // One clipped copy of the same field cannot supply a different orientation without invalidating the request.
    layout = RowLayout(rowRule().parameters); layout.headingAttribute="orientation"; rows.parameters=layout.getConfig();
    osg::ref_ptr<Feature> conflicting = new Feature(*mutableFeature); conflicting->set("orientation","45");
    provider->features.push_back({"fixture",conflicting});
    group=population();
    CHECK(FeatureScatterSource(provider,{rows}).generateBatch(key,group,1,output).isError()); CHECK(output.empty());
    group.rowDensity=0.4f; group.rowSpacingScale=1.2f; group.plantSpacingScale=0.8f;
    ScatterGroup restored(group.getConfig());
    CHECK(restored.rowDensity==group.rowDensity); CHECK(restored.rowSpacingScale==group.rowSpacingScale);
    CHECK(restored.plantSpacingScale==group.plantSpacingScale);
    group.rowDensity=2.0f; CHECK(group.validate().isError());
}

//! Registration demonstrates an application-specific land-use pathway without modifying source decoding or rendering.
TEST_CASE("Procedural2 application strategies share exclusions and ownership", "[procedural2][procedural2-rows]")
{
    auto provider = std::make_shared<FixtureProvider>();
    provider->features = {tagged(rectangle(500000,4500200,500190,4500400),"leisure","playground",77),
        tagged(rectangle(500090,4500290,500110,4500310),"building","yes",78)};
    auto policy = rule("leisure","playground");
    policy.name="custom-playground"; policy.strategy="fixture-layout"; policy.parameters.set("fixture",true);
    PlacementStrategies strategies = defaultPlacementStrategies();
    strategies["fixture-layout"] = std::make_shared<FixedLayout>();
    auto dispatch = std::make_shared<MixedPlacementStrategy>(strategies);
    FeatureScatterSource source(provider,{policy,rule("building","*","exclude")},dispatch);
    auto group=population(); auto profile=featureProfile();
    std::vector<ScatterPlacement> output,cells;
    REQUIRE(source.generateBatch(TileKey(1,0,0,profile),group,5,output).isOK());
    REQUIRE(output.size()==2u);
    for (const auto& p : output) CHECK((p.point.x()==500050 || p.point.x()==500150));
    group.renderCellLevel=2;
    for (unsigned x=0;x<2;++x) for (unsigned y=0;y<2;++y)
    {
        std::vector<ScatterPlacement> part;
        REQUIRE(source.generateBatch(TileKey(2,x,y,profile),group,5,part).isOK());
        cells.insert(cells.end(),part.begin(),part.end());
    }
    samePopulation(output,cells);
    CHECK(FeatureScatterSource(provider,{policy}).generateBatch(TileKey(2,0,0,profile),group,5,output).isError());
    CHECK(output.empty());
    osg::ref_ptr<VegetationLayer2> layer = new VegetationLayer2();
    CHECK(layer->registerPlacementStrategy("fixture-layout",strategies["fixture-layout"]));
    CHECK(!layer->registerPlacementStrategy("scatter",strategies["fixture-layout"]));
    CHECK(!layer->registerPlacementStrategy("empty",{}));
    osg::ref_ptr<OGRFeatureSource> features = new OGRFeatureSource();
    features->setURL(URI("../data/procedural2/rows/fields.geojson"));
    FeatureInput input; input.name="fixture"; input.features.setLayer(features);
    VegetationLayer2::Options options;
    options.profile()=ProfileOptions("global-geodetic");
    options.sources().push_back(input); options.coverage().push_back(policy);
    layer = new VegetationLayer2(options);
    CHECK(layer->open().isError()); // unknown plugins fail before worker jobs begin
    layer = new VegetationLayer2(options);
    REQUIRE(layer->registerPlacementStrategy("fixture-layout",strategies["fixture-layout"]));
    REQUIRE(layer->open().isOK());
    CHECK(!layer->registerPlacementStrategy("fixture-layout",strategies["fixture-layout"]));
}


//! Missing anchor metadata uses fixed metric strips; fragmented geometry and paging must not change layout or identity.
TEST_CASE("Procedural2 world rows survive native clipping and projection boundaries", "[procedural2][procedural2-rows]")
{
    RowLayout layout; layout.originMode="world"; layout.rowSpacing=20; layout.plantSpacing=10;
    layout.heading=31; layout.headingAttribute="vine_row_orientation";
    REQUIRE(layout.validate().isOK());
    CHECK(RowLayout(layout.getConfig()).originMode=="world");
    auto policy=rule("landuse","vineyard"); policy.name="world-vines"; policy.strategy="rows";
    policy.parameters=layout.getConfig();
    // Include a fixed projection boundary, the equator, both hemispheres, and the last longitude strip.
    for (const auto& center : std::vector<osg::Vec2d>{{7.35,46.23},{0.0,46.23},{-69.0,0.0},{179.98,-45.0}})
    {
        INFO(center.x() << ", " << center.y());
        const double west=center.x()-0.004, east=center.x()+0.004;
        const double south=center.y()-0.004, north=center.y()+0.004;
        osg::ref_ptr<const Profile> profile=Profile::create("wgs84",west,south-0.008,east+0.008,north,"",1,1);
        osg::ref_ptr<Feature> field=new Feature(rectangle(west,south,east,north),profile->getSRS(),Style(),42);
        field->set("landuse","vineyard"); field->set("vine_row_orientation","67.5");
        auto provider=std::make_shared<FixtureProvider>(); provider->features={{"osm",field}};
        FeatureScatterSource source(provider,{policy});
        auto group=population(); group.cellLevel=group.renderCellLevel=1;
        std::vector<ScatterPlacement> full,parts,clipped;
        REQUIRE(source.generateBatch(TileKey(1,0,0,profile),group,17,full).isOK());
        REQUIRE(full.size()>100u);
        for (const auto& p : full)
        {
            CHECK(p.point.x()>=west); CHECK(p.point.x()<east);
            CHECK(p.point.y()>=south); CHECK(p.point.y()<north);
            const unsigned zone=unsigned(std::floor((p.point.x()+180.0)/6.0))+1u;
            RowPattern pattern;
            REQUIRE(layout.resolve(*field,group,pattern,zone).isOK());
            CHECK(std::abs(pattern.heading-67.5*0.017453292519943295)<1e-12);
            osg::Vec3d metric;
            REQUIRE(profile->getSRS()->transform(p.point,pattern.frame,metric));
            const double across=(metric.x()*std::cos(pattern.heading)-metric.y()*std::sin(pattern.heading))/20;
            const double along=(metric.x()*std::sin(pattern.heading)+metric.y()*std::cos(pattern.heading))/10;
            CHECK(std::abs(across-std::round(across))<1e-6);
            CHECK(std::abs(along-std::round(along))<1e-6);
        }
        group.cellLevel=3; group.renderCellLevel=2;
        for (unsigned y=0;y<2;++y) for (unsigned x=0;x<2;++x)
        {
            TileKey key(2,x,y,profile);
            const auto& e=key.getExtent();
            // Each query sees only its own clipped fragment, with the same ID and no origin metadata.
            osg::ref_ptr<Feature> fragment=new Feature(*field);
            fragment->setGeometry(rectangle(e.xMin(),e.yMin(),e.xMax(),e.yMax()));
            provider->features={{"osm",fragment},{"osm",fragment}};
            REQUIRE(source.generateBatch(key,group,17,clipped).isOK());
            parts.insert(parts.end(),clipped.begin(),clipped.end());
        }
        samePopulation(full,parts);
        provider->features={{"osm",field}}; group.cellLevel=group.renderCellLevel=1;
        group.rowDensity=0.5f;
        REQUIRE(source.generateBatch(TileKey(1,0,0,profile),group,17,clipped).isOK());
        CHECK(clipped.size()<full.size()); CHECK(clipped.size()>full.size()/3u);
        group.rowDensity=1.0f;
        REQUIRE(source.generateBatch(TileKey(1,0,0,profile),group,17,clipped).isOK());
        samePopulation(full,clipped);
    }
    RowPattern pattern;
    const auto field=tagged(rectangle(500000,4500000,500400,4500400),"landuse","vineyard",42);
    REQUIRE(layout.resolve(*field.feature,population(),pattern,18).isOK()); // missing heading falls back to rule
    CHECK(std::abs(pattern.heading-31*0.017453292519943295)<1e-12);
    CHECK(layout.resolve(*field.feature,population(),pattern).isError());
    CHECK(layout.resolve(*field.feature,population(),pattern,61).isError());
    layout.longitude=0; CHECK(layout.validate().isError());
    layout.longitude=std::numeric_limits<double>::quiet_NaN(); layout.originMode="guess";
    CHECK(layout.validate().isError());
}

//! Network-only validation uses real agricultural tags from the same service as the main demo, without saved parcels.
TEST_CASE("Procedural2 OSM agricultural row placement live smoke", "[.][procedural2-osm-rows]")
{
    osg::ref_ptr<XYZFeatureSource> features=new XYZFeatureSource();
    features->setURL(URI("https://readymap.org/readymap/mbtiles/osm/{z}/{x}/{-y}.pbf"));
    features->setFormat("pbf"); features->setFIDAttribute("@id");
    features->options().profile()=ProfileOptions("spherical-mercator");
    features->options().minLevel()=14; features->options().maxLevel()=14;
    REQUIRE(features->open().isOK());
    FeatureInput input; input.name="osm"; input.features.setLayer(features);
    auto provider=std::make_shared<MapFeatureProvider>(std::vector<FeatureInput>{input});
    osg::ref_ptr<const Profile> profile=Profile::create("global-geodetic");
    const char* types[]={"vineyard","orchard","farmland"};
    const osg::Vec2d centers[]={{7.34835,46.23788},{7.34327,46.21723},{7.32757,46.21682}};
    for (unsigned i=0;i<3;++i)
    {
        INFO(types[i]);
        auto policy=rule("landuse",types[i]); policy.name=types[i]; policy.strategy="rows";
        RowLayout layout; layout.originMode="world"; layout.rowSpacing=6; layout.plantSpacing=3;
        if (i==0) layout.headingAttribute="vine_row_orientation";
        policy.parameters=layout.getConfig();
        FeatureScatterSource source(provider,{policy,rule("building","*","exclude"),
            rule("natural","water","exclude"),rule("highway","*","exclude",4)});
        auto group=population(); group.cellLevel=18; group.renderCellLevel=17;
        std::vector<ScatterPlacement> output;
        const auto status=source.generateBatch(profile->createTileKey(centers[i].x(),centers[i].y(),17),group,17,output);
        INFO(status.toString()); REQUIRE(status.isOK()); REQUIRE(!output.empty());
        std::cout << "OSM " << types[i] << " accepted placements: " << output.size() << std::endl;
    }
}

//! Checks coarse coverage without generating fine trees, including holes narrower than the initial canopy grid.
TEST_CASE("Procedural2 canopy preserves exclusions with bounded shared patches", "[procedural2][canopy]")
{
    auto provider = std::make_shared<FixtureProvider>();
    auto forest = rectangle(499900,4499900,500500,4500500);
    forest->getHoles().push_back(rectangle(500198,4500198,500202,4500202));
    provider->features.push_back(tagged(forest, "natural", "wood"));
    osg::ref_ptr<LineString> road = new LineString();
    road->push_back({500101,4500000,0}); road->push_back({500101,4500400,0});
    provider->features.push_back(tagged(road, "highway", "service", 2));
    FeatureScatterSource source(provider, {rule("natural","wood"), rule("highway","*","exclude",1.0)});
    ScatterGroup group;
    group.canopy = true; group.cellLevel = group.renderCellLevel = 3;
    group.density = 100000000; // deliberately impossible to enumerate under maxPerCell
    const TileKey key(0,0,0,featureProfile());
    std::shared_ptr<const PlacementField> field;
    REQUIRE(source.queryField(key,group,1,field).isOK());
    CoverageSample value;
    CHECK_FALSE(field->uniform(GeoExtent(key.getProfile()->getSRS(),500190,4500190,500210,4500210),value));
    CHECK_FALSE(field->uniform(GeoExtent(key.getProfile()->getSRS(),500095,4500100,500105,4500110),value));
    unsigned batches = 0, probes = 0;
    CanopyElevation slope = [&](std::vector<osg::Vec3d>& points)
    {
        ++batches; probes += unsigned(points.size());
        for (auto& p : points) p.z() = 0.2*(p.x()-500000)+0.1*(p.y()-4500000);
        return Status::NoError;
    };
    std::vector<CanopyPatch> patches;
    REQUIRE(buildCanopy(key,group,*field,key.getProfile()->getSRS(),slope,patches).isOK());
    REQUIRE_FALSE(patches.empty());
    CHECK(patches.size() <= 4096);
    CHECK(batches <= 3);
    CHECK(probes <= 5*5376);
    std::set<unsigned> layouts;
    unsigned partial = 0u;
    for (const auto& patch : patches)
    {
        layouts.insert(patch.layout);
        CHECK(patch.layout < 4u);
        CHECK(patch.mask > 0u);
        CHECK(patch.mask <= 511u);
        if (!field->uniform(patch.footprint,value)) ++partial;
        for (unsigned crown=0; crown<9; ++crown)
        {
            if (!(patch.mask & (1u<<crown))) continue;
            const auto box = canopyPatchCrownFootprint(patch,crown);
            REQUIRE(field->uniform(box,value));
            CHECK_FALSE(value.excluded);
            CHECK(value.density == 1.0f);
        }
        CHECK(patch.coverage == 8);
        const auto center = osg::Vec3d(0,0,0)*patch.localToWorld;
        CHECK(std::abs(center.z() - (0.2*(center.x()-500000)+0.1*(center.y()-4500000))) < 1e-6);
        const auto vertical = osg::Matrixd::transform3x3(osg::Vec3d(0,0,1),patch.localToWorld);
        CHECK(std::abs(vertical.x()) < 1e-9);
        CHECK(std::abs(vertical.y()) < 1e-9);
    }
    CHECK(layouts.size() == 4u);
    CHECK(partial > 0u);
    auto first = patches;
    CanopyElevation ridge = [](std::vector<osg::Vec3d>& points)
    {
        for (auto& p : points) p.z() = 100.0*std::sin((p.x()-500000)*0.07);
        return Status::NoError;
    };
    REQUIRE(buildCanopy(key,group,*field,key.getProfile()->getSRS(),ridge,patches).isOK());
    CHECK(patches.size() > first.size());
    CHECK(patches.size() <= 4096);
    REQUIRE(buildCanopy(key,group,*field,key.getProfile()->getSRS(),slope,patches).isOK());
    REQUIRE(first.size() == patches.size());
    for (std::size_t i=0; i<first.size(); ++i)
    {
        CHECK(first[i].localToWorld == patches[i].localToWorld);
        CHECK(first[i].layout == patches[i].layout);
        CHECK(first[i].mask == patches[i].mask);
        CHECK(first[i].turn == patches[i].turn);
    }
    CanopyElevation unavailable = [](std::vector<osg::Vec3d>&)
    { return Status(Status::ResourceUnavailable, "Unavailable fixture elevation"); };
    CHECK(buildCanopy(key,group,*field,key.getProfile()->getSRS(),unavailable,patches).isError());
    CHECK(patches.empty());
    osg::ref_ptr<ProgressCallback> canceled = new ProgressCallback();
    canceled->cancel();
    CHECK(buildCanopy(key,group,*field,key.getProfile()->getSRS(),slope,patches,canceled).isError());
    CHECK(patches.empty());
    ScatterGroup restored(group.getConfig());
    CHECK(restored.canopy);
    CHECK(restored.qualityOffset == group.qualityOffset);
    CHECK(restored.canopyTransition == group.canopyTransition);
    CHECK(restored.canopyFadeSeconds == group.canopyFadeSeconds);
    restored.farDensity = 0.5f;
    CHECK(restored.validate().isError());
}

//! Unequal patch sizes must not create artificial empty strips or overlapping coverage inside uniform forest.
TEST_CASE("Procedural2 irregular canopy partitions cover a page exactly", "[procedural2][canopy]")
{
    auto provider = std::make_shared<FixtureProvider>();
    provider->features.push_back(tagged(rectangle(499900,4499900,500500,4500500), "natural", "wood"));
    FeatureScatterSource source(provider, {rule("natural","wood")});
    ScatterGroup group;
    group.canopy = true; group.cellLevel = group.renderCellLevel = 3;
    const TileKey key(0,0,0,featureProfile());
    std::shared_ptr<const PlacementField> field;
    REQUIRE(source.queryField(key,group,1,field).isOK());
    //! Supplies a flat surface to isolate partition ownership from terrain-driven subdivision.
    CanopyElevation flat = [](std::vector<osg::Vec3d>& points)
    {
        for (auto& p : points) p.z() = 0.0;
        return Status::NoError;
    };
    std::vector<CanopyPatch> patches;
    REQUIRE(buildCanopy(key,group,*field,key.getProfile()->getSRS(),flat,patches).isOK());
    REQUIRE(patches.size() == 256u);
    const auto& page = key.getExtent();
    double area = 0.0;
    std::set<double> widths;
    for (std::size_t i=0; i<patches.size(); ++i)
    {
        const auto& e = patches[i].footprint;
        widths.insert(e.width());
        area += e.width()*e.height();
        CHECK(e.xMin() >= page.xMin()); CHECK(e.xMax() <= page.xMax());
        CHECK(e.yMin() >= page.yMin()); CHECK(e.yMax() <= page.yMax());
        CHECK(e.width() >= 0.81*page.width()/16.0-1e-8);
        CHECK(e.width() <= 1.21*page.width()/16.0+1e-8);
        for (std::size_t j=0; j<i; ++j)
        {
            const auto& other = patches[j].footprint;
            const double dx = std::min(e.xMax(),other.xMax())-std::max(e.xMin(),other.xMin());
            const double dy = std::min(e.yMax(),other.yMax())-std::max(e.yMin(),other.yMin());
            CHECK_FALSE((dx > 1e-8 && dy > 1e-8));
        }
        for (double x : {-0.5,0.5})
            for (double y : {-0.5,0.5})
            {
                const auto corner = osg::Vec3d(x,y,0.0)*patches[i].localToWorld;
                CHECK(corner.x() >= e.xMin()-1e-8); CHECK(corner.x() <= e.xMax()+1e-8);
                CHECK(corner.y() >= e.yMin()-1e-8); CHECK(corner.y() <= e.yMax()+1e-8);
            }
    }
    CHECK(std::abs(area-page.width()*page.height()) < 1e-6);
    CHECK(widths.size() > 100u);
}

//! Artificial coverage refinement must preserve the original forest, rather than fitting more, smaller trees.
TEST_CASE("Procedural2 canopy refinement preserves source art scale and coverage", "[procedural2][canopy]")
{
    auto provider = std::make_shared<FixtureProvider>();
    provider->features.push_back(tagged(rectangle(499900,4499900,500500,4500500), "natural", "wood"));
    FeatureScatterSource source(provider, {rule("natural","wood")});
    ScatterGroup group;
    group.canopy = true; group.cellLevel = group.renderCellLevel = 3;
    group.density = 1000000;
    const TileKey key(0,0,0,featureProfile());
    std::shared_ptr<const PlacementField> field;
    REQUIRE(source.queryField(key,group,1,field).isOK());
    //! Conservatively requests smaller coverage regions without changing any density or exclusion predicates.
    struct RefinedField : PlacementField
    {
        const PlacementField& source;
        //! Borrows an immutable field for this synchronous test.
        explicit RefinedField(const PlacementField& value) : source(value) { }
        //! The actual forest is unchanged by the artificial subdivision request.
        CoverageSample sample(const osg::Vec3d& p) const override { return source.sample(p); }
        //! Forces two splits before certifying the same constant forest.
        bool uniform(const GeoExtent& e, CoverageSample& value) const override
        { return e.width() < 8.0 && e.height() < 8.0 && source.uniform(e,value); }
        //! This test contains continuous forest.
        bool hasDensity() const override { return source.hasDensity(); }
        //! Retains the underlying source's explicit point set.
        const std::vector<ScatterPlacement>& points() const override { return source.points(); }
    } refined(*field);
    //! A planar slope must reconstruct exactly even when evaluated in smaller pieces.
    CanopyElevation slope = [](std::vector<osg::Vec3d>& points)
    {
        for (auto& p : points) p.z() = 0.2*(p.x()-500000)+0.1*(p.y()-4500000);
        return Status::NoError;
    };
    std::vector<CanopyPatch> original, pieces;
    REQUIRE(buildCanopy(key,group,*field,key.getProfile()->getSRS(),slope,original).isOK());
    REQUIRE(buildCanopy(key,group,refined,key.getProfile()->getSRS(),slope,pieces).isOK());
    REQUIRE(original.size() == 256u);
    CHECK(pieces.size() > original.size()); CHECK(pieces.size() <= 4096u);
    std::vector<std::array<double,9>> area(original.size());
    std::set<unsigned> turns;
    for (const auto& piece : pieces)
    {
        auto found = std::find_if(original.begin(),original.end(),[&](const CanopyPatch& root)
            { return root.artFootprint == piece.artFootprint; });
        REQUIRE(found != original.end());
        CHECK(piece.coverage == found->coverage);
        CHECK(piece.layout == found->layout);
        CHECK(piece.turn == found->turn);
        CHECK(piece.clip != 0u);
        turns.insert(piece.turn);
        for (unsigned row=0; row<4; ++row)
            for (unsigned col=0; col<4; ++col)
                CHECK(std::abs(piece.localToWorld(row,col)-found->localToWorld(row,col)) < 1e-7);
        for (unsigned crown=0; crown<9; ++crown)
            if (piece.mask & (1u<<crown))
            {
                const auto box = canopyPatchCrownFootprint(piece,crown);
                REQUIRE(box.isValid());
                area[found-original.begin()][crown] += box.width()*box.height();
            }
        // Decode the GPU window and rotate it back to map axes; CPU certification must cover that exact region.
        const unsigned code = piece.clip-1u, side = (code >> 4u)+1u;
        unsigned x = code & 3u, y = (code >> 2u) & 3u;
        for (unsigned i=0; i<piece.turn; ++i)
        {
            const unsigned previous = x; x = 4u-y-side; y = previous;
        }
        CHECK(std::abs(piece.footprint.xMin()-(piece.artFootprint.xMin()+x*piece.artFootprint.width()/4.0)) < 1e-8);
        CHECK(std::abs(piece.footprint.yMin()-(piece.artFootprint.yMin()+y*piece.artFootprint.height()/4.0)) < 1e-8);
    }
    CHECK(turns.size() == 4u);
    for (std::size_t i=0; i<original.size(); ++i)
        for (unsigned crown=0; crown<9; ++crown)
        {
            const auto box = canopyPatchCrownFootprint(original[i],crown);
            CHECK(std::abs(area[i][crown]-box.width()*box.height()) < 1e-6);
        }
}

//! A failed child cannot publish siblings as a partial replacement of the still-valid parent forest.
TEST_CASE("Procedural2 hierarchy rejects partial page replacements", "[procedural2][canopy]")
{
    osg::ref_ptr<Map> map = new Map();
    osg::ref_ptr<Util::SimplePager> pager = new Util::SimplePager(map,featureProfile());
    pager->setMinLevel(1); pager->setMaxLevel(1);
    unsigned calls = 0;
    pager->setCreateNodeFunction([&](const TileKey&, ProgressCallback* progress) -> osg::ref_ptr<osg::Node>
    {
        if (++calls == 2) { progress->cancel(); return {}; }
        return new osg::Group(); // a successful empty page is distinct from failure
    });
    osg::ref_ptr<ProgressCallback> progress = new ProgressCallback();
    CHECK_FALSE(pager->createPagedChildrenOf(TileKey(0,0,0,featureProfile()),progress).valid());
    CHECK(calls == 2);
    progress->reset();
    auto empty = pager->createPagedChildrenOf(TileKey(0,0,0,featureProfile()),progress);
    REQUIRE(empty.valid());
    CHECK(empty->asGroup()->getNumChildren() == 4);
}

//! Checks that per-page settings follow defaults and the optional cull decision hook actually governs refinement.
TEST_CASE("Procedural2 can configure hierarchical refinement without replacing SimplePager", "[procedural2][canopy]")
{
    osg::ref_ptr<Map> map = new Map();
    osg::ref_ptr<Util::SimplePager> pager = new Util::SimplePager(map,featureProfile());
    pager->setMinLevel(1); pager->setMaxLevel(2);
    unsigned configured = 0, culled = 0;
    pager->setCreateNodeFunction([](const TileKey&, ProgressCallback*) -> osg::ref_ptr<osg::Node>
        { return new osg::Group(); });
    pager->setConfigurePagedNodeFunction([&](const TileKey& key, Util::PagedNode2* node)
    {
        CHECK(key.getLOD() == 1);
        ++configured;
        node->setMinPixels(333.0f);
        node->setRefinementFunction([&](osg::NodeVisitor&, bool suggested)
        {
            ++culled;
            CHECK(suggested);
            return false; // don't launch an asynchronous job during this CPU-only test
        });
    });
    auto children = pager->createPagedChildrenOf(TileKey(0,0,0,featureProfile()),nullptr);
    REQUIRE(children.valid());
    CHECK(configured == 4);
    auto child = dynamic_cast<Util::PagedNode2*>(children->asGroup()->getChild(0));
    REQUIRE(child != nullptr);
    CHECK(child->getMinPixels() == 333.0f);
    osg::NodeVisitor nv(osg::NodeVisitor::CULL_VISITOR,osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);
    child->traverse(nv);
    CHECK(culled == 1);
}

//! Distance reversal and late page arrival must blend continuously; nested intervals retain exactly one representation.
TEST_CASE("Procedural2 canopy transitions preserve complementary coverage", "[procedural2][canopy]")
{
    CHECK(canopyBlend(74,100,0.25f,10,0.4f) == 0.0f);
    CHECK(canopyBlend(126,100,0.25f,10,0.4f) == 1.0f);
    CHECK(canopyBlend(100,100,0.25f,10,0.4f) == 0.5f);
    CHECK(canopyBlend(150,100,0.25f,0,0.4f) == 0.0f);
    CHECK(canopyBlend(150,100,0.25f,-1,0.4f) == 0.0f);
    CHECK(std::abs(canopyBlend(150,100,0.25f,0.2,0.4f)-0.5f) < 1e-6f);
    CHECK(canopyBlend(150,100,0.0f,0,0.0f) == 1.0f);
    float previous = 0.0f;
    for (unsigned i=0; i<=100; ++i)
    {
        const float weight = canopyBlend(75+i*0.5,100,0.25f,10,0.4f);
        CHECK(weight >= previous);
        CHECK(weight-previous < 0.016f);
        CHECK(std::abs(weight+canopyBlend(125-i*0.5,100,0.25f,10,0.4f)-1.0f) < 1e-6f);
        previous = weight;
        const unsigned split = canopySplit(0,64,weight);
        for (unsigned j=0; j<=64; ++j)
        {
            const unsigned nested = canopySplit(split,64,j/64.0f);
            CHECK(nested >= split);
            CHECK(nested <= 64u);
            for (unsigned pixel=0; pixel<64; ++pixel)
            {
                const unsigned owners = unsigned(pixel < split)+unsigned(pixel >= split && pixel < nested)+
                    unsigned(pixel >= nested);
                REQUIRE(owners == 1u);
            }
        }
    }
    ScatterGroup invalid;
    invalid.canopyTransition = 0.6f;
    CHECK(invalid.validate().isError());
    invalid.canopyTransition = 0.25f; invalid.canopyFadeSeconds = -1.0f;
    CHECK(invalid.validate().isError());
}

//! Tier footprint controls must partition the same coverage, scale counts predictably, and leave fine placement stable.
TEST_CASE("Procedural2 independent canopy footprints preserve coverage and placement", "[procedural2][canopy]")
{
    auto provider = std::make_shared<FixtureProvider>();
    provider->features.push_back(tagged(rectangle(499900,4499900,500500,4500500), "natural", "wood"));
    FeatureScatterSource source(provider, {rule("natural","wood")});
    ScatterGroup group;
    group.canopy = true; group.cellLevel = group.renderCellLevel = 3;
    group.density = 200000;
    const auto profile = featureProfile();
    const TileKey fineKey(3,0,0,profile);
    std::vector<ScatterPlacement> before, after;
    REQUIRE(source.generateBatch(fineKey,group,1,before).isOK());
    REQUIRE_FALSE(before.empty());
    //! Flat terrain isolates nominal partitioning from adaptive subdivision.
    CanopyElevation flat = [](std::vector<osg::Vec3d>& points)
    {
        for (auto& p : points) p.z() = 0.0;
        return Status::NoError;
    };
    for (bool far : {false,true})
    {
        const TileKey key(far ? 1u : 2u,0,0,profile);
        std::shared_ptr<const PlacementField> field;
        REQUIRE(source.queryField(key,group,1,field).isOK());
        for (float scale : {0.5f,1.0f,2.0f,4.0f})
        {
            // Give the other tier a different size to catch accidentally shared controls.
            group.canopyMidPatchScale = far ? 4.0f : scale;
            group.canopyFarPatchScale = far ? scale : 0.5f;
            std::vector<CanopyPatch> patches;
            REQUIRE(buildCanopy(key,group,*field,profile->getSRS(),flat,patches).isOK());
            const unsigned side = unsigned(16.0f/scale);
            REQUIRE(patches.size() == side*side);
            double area = 0.0;
            for (const auto& patch : patches)
            {
                const auto& e = patch.footprint;
                CHECK(patch.artFootprint == e);
                CHECK(e.width() >= 0.81*key.getExtent().width()/side-1e-8);
                CHECK(e.width() <= 1.21*key.getExtent().width()/side+1e-8);
                area += e.width()*e.height();
            }
            CHECK(std::abs(area-key.getExtent().width()*key.getExtent().height()) < 1e-6);
            REQUIRE(source.generateBatch(fineKey,group,1,after).isOK());
            samePopulation(before,after);
            const ScatterGroup restored(group.getConfig());
            CHECK(restored.canopyMidPatchScale == group.canopyMidPatchScale);
            CHECK(restored.canopyFarPatchScale == group.canopyFarPatchScale);
        }
    }
    group.canopyFar = false;
    CHECK_FALSE(ScatterGroup(group.getConfig()).canopyFar);
    for (float invalid : {0.0f,1.5f,8.0f,std::numeric_limits<float>::quiet_NaN()})
    {
        auto changed = group;
        changed.canopyMidPatchScale = invalid;
        CHECK(changed.validate().isError());
        changed = group;
        changed.canopyFarPatchScale = invalid;
        CHECK(changed.validate().isError());
    }
}

//! Live tier changes retain the viewing range, reach medium-only pages, and report only attached aggregate content.
TEST_CASE("Procedural2 canopy modes rebuild the hierarchy and report resident clumps", "[procedural2][canopy]")
{
    auto provider = std::make_shared<FixtureProvider>();
    provider->features.push_back(tagged(rectangle(499900,4499900,500500,4500500), "natural", "wood"));
    auto point = new PointSet(); point->push_back({500025,4500375,0});
    provider->features.push_back(tagged(point,"natural","tree",99));
    auto source = std::make_shared<FeatureScatterSource>(provider,
        std::vector<CoverageRule>{rule("natural","wood"),rule("natural","tree","points")});
    const auto profile = featureProfile();
    VegetationLayer2::Options options;
    options.profile() = profile->toProfileOptions();
    ScatterGroup group;
    group.canopy = true; group.cellLevel = group.renderCellLevel = 3;
    group.canopyMidPatchScale = 2.0f; group.canopyFarPatchScale = 4.0f;
    options.groups() = {group};
    osg::ref_ptr<VegetationLayer2> layer = new VegetationLayer2(options);
    REQUIRE(layer->setSource(source));
    REQUIRE(layer->open().isOK());
    osg::ref_ptr<Map> map = new Map();
    map->addLayer(layer);
    //! Finds the single active population pager, holding it through this synchronous test.
    auto pager = [&]()
    {
        osg::ref_ptr<Util::SimplePager> result;
        forEachNodeOfType<Util::SimplePager>(layer->getNode(),[&](Util::SimplePager* node) { result = node; });
        return result;
    };
    auto both = pager();
    REQUIRE(both.valid());
    CHECK(both->getMinLevel() == 1u);
    const auto far = both->createNode(TileKey(1,0,0,profile),nullptr);
    const auto mid = both->createNode(TileKey(2,0,0,profile),nullptr);
    REQUIRE(far.valid()); REQUIRE(mid.valid());
    both->addChild(far); both->addChild(mid);
    const auto counts = layer->getAggregateResidency(group.name);
    CHECK(counts.farClumps == 16u); CHECK(counts.farPieces == 16u);
    CHECK(counts.midClumps == 64u); CHECK(counts.midPieces == 64u);
    CHECK(layer->getAggregateResidency("unknown").midClumps == 0u);

    // Both flags default on; all four combinations must affect only their matching aggregate drawables.
    CHECK(group.canopyMidGPUCulling); CHECK(group.canopyFarGPUCulling);
    for (bool midCull : {false,true})
    for (bool farCull : {false,true})
    {
        INFO("mid GPU culling=" << midCull << " far GPU culling=" << farCull);
        group.canopyMidGPUCulling = midCull;
        group.canopyFarGPUCulling = farCull;
        const ScatterGroup restored(group.getConfig());
        CHECK(restored.canopyMidGPUCulling == midCull);
        CHECK(restored.canopyFarGPUCulling == farCull);
        REQUIRE(layer->setGroup(restored).isOK());
        auto current = pager();
        REQUIRE(current.valid());
        CHECK(current->getMaxRange() == group.maxRange);
        for (unsigned level=1; level<=3; ++level)
        {
            auto page = current->createNode(TileKey(level,0,0,profile),nullptr);
            REQUIRE(page.valid());
            unsigned aggregates = 0, individuals = 0;
            forEachNodeOfType<ChonkDrawable>(page.get(),[&](ChonkDrawable* drawable)
            {
                const bool farTier = drawable->getName() == group.name+"/canopy-far";
                const bool midTier = drawable->getName() == group.name+"/canopy-mid";
                CHECK(drawable->getCullingActive()); // ordinary scene-graph bounds still participate
                if (farTier || midTier)
                {
                    ++aggregates;
                    CHECK(drawable->getUseGPUCulling() == (farTier ? farCull : midCull));
                    CHECK(drawable->getNumInstances() == (farTier ? counts.farPieces : counts.midPieces));
                }
                else
                {
                    ++individuals;
                    CHECK(drawable->getUseGPUCulling()); // includes mapped points carried by aggregate pages
                }
            });
            CHECK(aggregates == (level < 3u ? 1u : 0u));
            CHECK(individuals > 0u);
        }
    }

    group.canopyFar = false;
    REQUIRE(layer->setGroup(group).isOK());
    auto mediumOnly = pager();
    REQUIRE(mediumOnly.valid());
    CHECK(mediumOnly != both);
    CHECK(mediumOnly->getMinLevel() == 2u);
    CHECK(mediumOnly->getMaxLevel() == 3u);
    CHECK(mediumOnly->getMaxRange() == group.maxRange);
    CHECK(layer->getAggregateResidency(group.name).farClumps == 0u);
    CHECK(layer->getAggregateResidency(group.name).midClumps == 0u);
    // Empty level-one ancestors must use distance refinement to reach the first enabled tier throughout max_range.
    auto ancestors = mediumOnly->createPagedChildrenOf(TileKey(0,0,0,profile),nullptr);
    REQUIRE(ancestors.valid());
    unsigned ancestorCount = 0u;
    forEachNodeOfType<Util::PagedNode2>(ancestors.get(),[&](Util::PagedNode2* node)
    {
        ++ancestorCount;
        CHECK(node->getNumChildren() == 0u);
        CHECK(node->getLODMethod() == Util::LODMethod::CAMERA_DISTANCE);
        CHECK(node->getMinPixels() == mediumOnly->getMinPixels());
        CHECK(node->getMaxRange() == group.maxRange);
    });
    CHECK(ancestorCount == 4u);
    auto children = mediumOnly->createPagedChildrenOf(TileKey(1,0,0,profile),nullptr);
    REQUIRE(children.valid());
    mediumOnly->addChild(children);
    CHECK(layer->getAggregateResidency(group.name).midClumps == 4u*64u);
    CHECK(layer->getAggregateResidency(group.name).farClumps == 0u);

    group.canopy = false;
    REQUIRE(layer->setGroup(group).isOK());
    auto individuals = pager();
    REQUIRE(individuals.valid());
    CHECK(individuals->getMinLevel() == 3u);
    CHECK(individuals->getMaxRange() == group.maxRange);
    CHECK(layer->getAggregateResidency(group.name).midClumps == 0u);
    CHECK(layer->getAggregateResidency(group.name).farClumps == 0u);
    map->removeLayer(layer);
    CHECK(layer->getAggregateResidency(group.name).midPieces == 0u);
    layer->close();
}

//! Global and local controls share pixel units, preserve legacy scene intent, and degrade page visibility monotonically.
TEST_CASE("Procedural2 quality combines global and population error once", "[procedural2][canopy]")
{
    ScatterGroup group;
    CHECK(group.qualityOffset == 0.0f);
    CHECK(group.effectiveError(25) == 25.0f);
    group.qualityOffset = 23;
    CHECK(group.effectiveError(25) == 48.0f);
    CHECK(group.effectiveError(100) == 123.0f);
    CHECK(ScatterGroup(group.getConfig()).qualityOffset == 23.0f);
    osg::ref_ptr<osg::Uniform> adjustment = new osg::Uniform("oe_chonk_sse_adjust",osg::Vec2f(23,1.0f/25.0f));
    osg::NodeVisitor visitor;
    visitor.setUserValue("oe_sse",25.0f);
    CHECK(populationError(visitor,adjustment) == 48.0f);
    visitor.setUserValue("oe_sse",48.0f);
    adjustment->set(osg::Vec2f(0,1.0f/25.0f));
    CHECK(populationError(visitor,adjustment) == 48.0f);
    group.qualityOffset = -100;
    CHECK(group.effectiveError(25) == 1.0f);
    adjustment->set(osg::Vec2f(-100,1.0f/25.0f));
    CHECK(populationError(visitor,adjustment) == 1.0f);
    for (float invalid : {4097.0f,-4097.0f,std::numeric_limits<float>::quiet_NaN()})
    {
        group.qualityOffset = invalid;
        CHECK(group.validate().isError());
    }
    Config legacy("group"); legacy.set("canopy_pixels",48.0f);
    CHECK(ScatterGroup(legacy).effectiveError(25) == 48.0f);
    legacy.set("quality_offset",0.0f);
    CHECK(ScatterGroup(legacy).effectiveError(25) == 25.0f);
    float previous = 1.0f;
    for (float budget = 1; budget <= 300; budget += 1)
    {
        const float visible = populationVisibility(100,budget);
        CHECK(visible <= previous); CHECK(visible >= 0.0f);
        previous = visible;
    }
    CHECK(populationVisibility(100,25) == 1.0f);
    CHECK(populationVisibility(100,100) == Approx(0.5f));
    CHECK(populationVisibility(100,200) == 0.0f);
    CHECK(populationVisibility(-1,200) == 1.0f);
}

//! Page visibility and refinement must use primary-view pixels even when a shadow cascade has a different projection.
TEST_CASE("Procedural2 page error follows primary camera and orthographic projection", "[procedural2][canopy]")
{
    osg::ref_ptr<osg::Camera> camera = new osg::Camera();
    osg::ref_ptr<osgUtil::RenderStage> stage = new osgUtil::RenderStage();
    stage->setCamera(camera);
    osg::ref_ptr<osgUtil::CullVisitor> retainedVisitor = new osgUtil::CullVisitor();
    auto& visitor = *retainedVisitor;
    visitor.setRenderStage(stage);
    visitor.setLODScale(1.0f);
    const osg::BoundingSphere bound(osg::Vec3(),10.0f);
    const osg::Matrixd projection = osg::Matrixd::perspective(45.0,1.0,1.0,10000.0);
    visitor.pushViewport(new osg::Viewport(0,0,256,256));
    visitor.pushProjectionMatrix(new osg::RefMatrix(projection));
    visitor.pushModelViewMatrix(new osg::RefMatrix(osg::Matrixd::translate(0,0,-100)),osg::Transform::ABSOLUTE_RF);
    const double primary = populationPixelSize(bound,visitor);
    CHECK(primary == Approx(10.0*projection(1,1)*256.0/90.0));
    visitor.popModelViewMatrix(); visitor.popProjectionMatrix(); visitor.popViewport();
    camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
    camera->attach(osg::Camera::DEPTH_BUFFER,new osg::Texture2D());
    auto* state = camera->getOrCreateStateSet();
    state->setDefine("OE_IS_SHADOW_CAMERA");
    REQUIRE(CameraUtils::isShadowCamera(camera));
    state->addUniform(new osg::Uniform("oe_shadowToPrimaryMatrix",osg::Matrixf::translate(0,0,900)));
    state->addUniform(new osg::Uniform("oe_primaryProjectionMatrix",osg::Matrixf(projection)));
    state->addUniform(new osg::Uniform("oe_primaryViewport",osg::Vec2f(256,256)));
    state->addUniform(new osg::Uniform("oe_primaryLODScale",1.0f));
    visitor.pushViewport(new osg::Viewport(0,0,2048,2048));
    visitor.pushProjectionMatrix(new osg::RefMatrix(osg::Matrixd::ortho(-500,500,-500,500,1,10000)));
    visitor.pushModelViewMatrix(new osg::RefMatrix(osg::Matrixd::translate(0,0,-1000)),osg::Transform::ABSOLUTE_RF);
    CHECK(populationPixelSize(bound,visitor) == Approx(primary));
    state->getUniform("oe_primaryLODScale")->set(2.0f);
    CHECK(populationPixelSize(bound,visitor) == Approx(primary/2.0));
    state->getUniform("oe_primaryProjectionMatrix")->set(osg::Matrixf::ortho(-50,50,-50,50,1,10000));
    state->getUniform("oe_primaryLODScale")->set(1.0f);
    CHECK(populationPixelSize(bound,visitor) == Approx(51.2));
    state->removeUniform("oe_primaryViewport");
    CHECK(populationPixelSize(bound,visitor) == -1.0); // missing reference conservatively keeps the page
    visitor.popModelViewMatrix(); visitor.popProjectionMatrix(); visitor.popViewport();
}
