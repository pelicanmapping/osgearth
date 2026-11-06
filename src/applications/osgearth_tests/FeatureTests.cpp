/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgEarth/catch.hpp>

#include <osgEarth/Feature>
#include <osgEarth/Geometry>
#include <osgEarth/GeometryUtils>
#include <osgEarth/XYZFeatureSource>
#include <osgEarth/FileUtils>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/fstream>

using namespace osgEarth;

using Vec = std::vector<osg::Vec3d>;

namespace
{
    //! Owns temporary metadata and tiles; removes the directory after each offline TileJSON test.
    struct TileJSONFiles
    {
        std::string directory = osgDB::concatPaths(osgDB::getCurrentWorkingDirectory(),
            Util::getTempName("TileJSONTests_", ""));
        bool created = Util::makeDirectory(directory);

        //! Removes only this fixture's temporary directory.
        ~TileJSONFiles()
        {
            if (created)
                Util::removeDirectory(directory);
        }

        //! Writes a fixture resource and fails the test if the write fails.
        std::string write(const std::string& name, const std::string& data) const
        {
            REQUIRE(created);
            const auto path = osgDB::concatPaths(directory, name);
            osgDB::ofstream output(path.c_str(), std::ios::binary);
            output << data;
            output.close();
            REQUIRE(output.good());
            return path;
        }
    };
}

//! Checks metadata defaults, coverage versus tile-grid extent, and preservation of the original configuration.
TEST_CASE("XYZFeatures opens TileJSON without explicit tile settings", "[feature][tilejson]")
{
    TileJSONFiles files;
    const auto url = files.write("latest", R"({"tilejson":"3.0.0","tiles":["{z}_{x}_{y}.pbf"],
        "minzoom":2,"maxzoom":14,"attribution":"Test data","bounds":[-10,-20,30,40]})");
    osg::ref_ptr<XYZFeatureSource> source = new XYZFeatureSource();
    source->setURL(URI(url));
    REQUIRE(source->open().isOK());
    CHECK(source->getURL().full() == URI(url).full());
    CHECK(source->getFormat() == "pbf");
    CHECK(source->getMinLevel() == 2);
    CHECK(source->getMaxLevel() == 14);
    CHECK(source->getAttribution() == "Test data");
    CHECK_FALSE(source->isWritable());
    const auto fp = source->getFeatureProfile();
    REQUIRE(fp);
    CHECK(fp->getFirstLevel() == 2);
    CHECK(fp->getMaxLevel() == 14);
    CHECK(fp->getSRS()->isMercator());
    const auto coverage = fp->getExtent().transform(SpatialReference::get("wgs84"));
    CHECK(coverage.xMin() == Approx(-10));
    CHECK(coverage.yMax() == Approx(40));
    CHECK(fp->getTilingProfile()->getExtent().width() > fp->getExtent().width());
    const auto config = source->getConfig();
    CHECK(config.value("url") == url);
    CHECK_FALSE(config.hasChild("min_level"));
    CHECK_FALSE(config.hasChild("max_level"));
    CHECK_FALSE(config.hasChild("format"));
    CHECK_FALSE(config.hasChild("profile"));
    CHECK_FALSE(config.hasChild("attribution"));
}

//! Exercises actual cursor reads through relative XYZ/TMS endpoints, explicit format overrides, and overzooming.
TEST_CASE("XYZFeatures resolves TileJSON tile requests", "[feature][tilejson]")
{
    TileJSONFiles files;
    std::string scheme = "xyz";
    SECTION("XYZ") { }
    SECTION("TMS") { scheme = "tms"; }
    const auto url = files.write("tiles.json", std::string(R"({"tilejson":"2.2.0",
        "tiles":["{z}_{x}_{y}.json"],"format":"pbf","minzoom":1,"maxzoom":1,"scheme":")") + scheme + "\"}");
    files.write(scheme == "xyz" ? "1_0_0.json" : "1_0_1.json", R"({"type":"FeatureCollection","features":[
        {"type":"Feature","properties":{"marker":"correct tile"},"geometry":{"type":"Point","coordinates":[-90,45]}}
    ]})");
    osg::ref_ptr<XYZFeatureSource> source = new XYZFeatureSource();
    source->setURL(URI(url));
    source->setFormat("json");
    REQUIRE(source->open().isOK());
    const auto profile = source->getFeatureProfile()->getTilingProfile();
    CHECK(source->insert(TileKey(1, 0, 0, profile), {}, true).isError());
    for (unsigned level : { 1u, 2u })
    {
        auto cursor = source->createFeatureCursor(Query(TileKey(level, 0, 0, profile)));
        REQUIRE(cursor.valid());
        REQUIRE(cursor->hasMore());
        CHECK(cursor->nextFeature()->getString("marker") == "correct tile");
    }
}

//! Explicit earth-file settings must override metadata and remain serializable.
TEST_CASE("XYZFeatures retains explicit TileJSON overrides", "[feature][tilejson]")
{
    TileJSONFiles files;
    const auto url = files.write("tiles.json", R"({"tilejson":"3.0.0","tiles":["{z}_{x}_{y}.pbf"],
        "minzoom":0,"maxzoom":14,"attribution":"Metadata credit"})");
    XYZFeatureSource::Options options;
    options.url() = URI(url);
    options.minLevel() = 3;
    options.maxLevel() = 8;
    options.profile() = ProfileOptions("global-geodetic");
    options.attribution() = "Explicit credit";
    osg::ref_ptr<XYZFeatureSource> source = new XYZFeatureSource(options);
    REQUIRE(source->open().isOK());
    CHECK(source->getMinLevel() == 3);
    CHECK(source->getMaxLevel() == 8);
    CHECK(source->getFeatureProfile()->getSRS()->isGeographic());
    CHECK(source->getAttribution() == "Explicit credit");
    CHECK(source->getConfig().value<int>("max_level", -1) == 8);
}

//! Reopening must resolve a changed metadata endpoint without retaining old zoom limits or tile addresses.
TEST_CASE("XYZFeatures refreshes TileJSON on reopen", "[feature][tilejson]")
{
    TileJSONFiles files;
    const auto first = files.write("first.json", R"({"tilejson":"3.0.0","tiles":["old/{z}/{x}/{y}.pbf"],
        "minzoom":2,"maxzoom":6,"attribution":"Old credit"})");
    files.write("0_0_0.json", R"({"type":"FeatureCollection","features":[
        {"type":"Feature","properties":{"marker":"new tile"},"geometry":{"type":"Point","coordinates":[0,0]}}
    ]})");
    osg::ref_ptr<XYZFeatureSource> source = new XYZFeatureSource();
    source->setURL(URI(first));
    REQUIRE(source->open().isOK());
    source->close();
    files.write("first.json", R"({"tilejson":"3.0.0","tiles":["{z}_{x}_{y}.json"],
        "format":"json","maxzoom":1})");
    REQUIRE(source->open().isOK());
    CHECK(source->getMinLevel() == 0);
    CHECK(source->getMaxLevel() == 1);
    CHECK(source->getFeatureProfile()->getMaxLevel() == 1);
    CHECK(source->getAttribution().empty());
    auto cursor = source->createFeatureCursor(Query(TileKey(0, 0, 0, source->getFeatureProfile()->getTilingProfile())));
    REQUIRE(cursor.valid());
    REQUIRE(cursor->hasMore());
    CHECK(cursor->nextFeature()->getString("marker") == "new tile");
}

//! Optional missing or invalid values use TileJSON defaults instead of unsafe numeric conversions.
TEST_CASE("XYZFeatures uses TileJSON defaults", "[feature][tilejson]")
{
    TileJSONFiles files;
    const auto url = files.write("tiles.json", R"({"tilejson":"3.0.0","tiles":["{z}/{x}/{y}.pbf"],
        "minzoom":"bad","maxzoom":999999999999,"bounds":["bad",null,0,0],"attribution":42})");
    osg::ref_ptr<XYZFeatureSource> source = new XYZFeatureSource();
    source->setURL(URI(url));
    REQUIRE(source->open().isOK());
    CHECK(source->getMinLevel() == 0);
    CHECK(source->getMaxLevel() == 30);
    CHECK(source->getAttribution().empty());
}

//! Malformed manifests fail during open, before tile requests can run.
TEST_CASE("XYZFeatures rejects unusable TileJSON", "[feature][tilejson]")
{
    TileJSONFiles files;
    const std::vector<std::string> cases = {
        std::string("not json"), std::string("[]"), std::string("{}"),
        std::string(R"({"tilejson":"3.0.0","tiles":[]})"),
        std::string(R"({"tilejson":"3.0.0","tiles":[42]})"),
        std::string(R"({"tilejson":"3.0.0","tiles":["constant.pbf"]})"),
        std::string(R"({"tilejson":"3.0.0","tiles":["{z}/{x}/{y}.png"],"format":"png"})"),
        std::string(R"({"tilejson":"3.0.0","tiles":["{z}/{x}/{y}.pbf"],"minzoom":8,"maxzoom":2})") };
    osg::ref_ptr<XYZFeatureSource> source = new XYZFeatureSource();
    source->setURL(URI(osgDB::concatPaths(files.directory, "missing.json")));
    CHECK(source->open().isError());
    for (const auto& data : cases)
    {
        source = new XYZFeatureSource();
        source->setURL(URI(files.write("bad.json", data)));
        CHECK(source->open().isError());
    }
}

//! Direct templates keep their existing explicit-settings contract and writable local-file behavior.
TEST_CASE("XYZFeatures still supports direct templates", "[feature][tilejson]")
{
    TileJSONFiles files;
    osg::ref_ptr<XYZFeatureSource> source = new XYZFeatureSource();
    source->setURL(URI(osgDB::concatPaths(files.directory, "{z}_{x}_{y}.json")));
    CHECK(source->open().isError());
    source->options().profile() = ProfileOptions("spherical-mercator");
    source->setFormat("json");
    source->setMinLevel(0);
    source->setMaxLevel(1);
    REQUIRE(source->open().isOK());
    CHECK(source->isWritable());
}

namespace
{
    template<class A, class B>
    inline bool polygons_equivalent(const A& v1, const B& v2)
    {
        if (v1.size() != v2.size())
            return false;

        for (size_t off = 0; off < v2.size(); ++off)
        {
            bool matching = true;
            for (size_t i = 0; i < v1.size() && matching; ++i)
            {
                if (v1[i] != v2[(i + off) % v2.size()])
                {
                    matching = false;
                }
            }

            if (matching)
            {
                return true;
            }
        }

        return false;
    }
}

//TEST_CASE("Geometry::crop line against line")
//{
//    Vec input = { {-10, 0, 0 }, {10, 0, 0} };
//    Vec boundary = { {0, 0, 0}, {0, 1, 0} };
//    Vec output1 = { {-10, 0, 0}, {0, 0, 0} };
//    Vec output2 = { {0, 0, 0}, {10, 0, 0} };
//
//    LineString line(&input);
//    auto* geom = line.crop(boundary);
//    REQUIRE(geom);
//    REQUIRE(geom->isLineString());
//    REQUIRE(geom->asVector() == output1);
//    delete geom;
//
//    std::reverse(boundary.begin(), boundary.end());
//    geom = line.crop(boundary);
//    REQUIRE(geom);
//    REQUIRE(geom->isLineString());
//    REQUIRE(geom->asVector() == output2);
//    delete geom;
//}

TEST_CASE("polygons_equivalent works")
{
    Vec lhs = { {0,0,0}, {1,1,1}, {2,2,2}, {3,3,3} };
    
    Vec rhs_1 = { };
    REQUIRE(polygons_equivalent(lhs, rhs_1) == false);

    Vec rhs_2 = { {0,0,0}, {1,1,1}, {2,2,2} };
    REQUIRE(polygons_equivalent(lhs, rhs_2) == false);

    Vec rhs_3 = { {3,3,3}, {2,2,2}, {1,1,1}, {0,0,0} };
    REQUIRE(polygons_equivalent(lhs, rhs_3) == false);

    Vec rhs_4 = { {0,0,0}, {1,1,1}, {2,2,2}, {3,3,3} };
    REQUIRE(polygons_equivalent(lhs, rhs_4) == true);

    Vec rhs_5 = { {3,3,3}, {0,0,0}, {1,1,1}, {2,2,2} };
    REQUIRE(polygons_equivalent(lhs, rhs_5) == true);

    Vec rhs_6 = { {2,2,2}, {3,3,3}, {0,0,0}, {1,1,1} };
    REQUIRE(polygons_equivalent(lhs, rhs_6) == true);

    Vec rhs_7 = { {1,1,1}, {2,2,2}, {3,3,3}, {0,0,0} };
    REQUIRE(polygons_equivalent(lhs, rhs_7) == true);

}

TEST_CASE("Geometry::crop a polygon to another polygon")
{
    Vec input = { { 0,0,0 }, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}, {0, 0, 0} };
    Vec boundary{ {5, 5, 0}, {15, 5, 0}, {15, 15, 0}, {5, 15, 0}, {5, 5, 0} };
    Vec output = { {5, 10, 0}, {5, 5, 0}, {10, 5, 0}, {10, 10, 0} };

    Ring clip(&boundary);

    osgEarth::Polygon poly(&input);
    auto* result = poly.crop(&clip);

    REQUIRE(result);
    REQUIRE(result->isPolygon());
    REQUIRE(result->asVector() == output);
    delete result;
}

TEST_CASE("Geometry::crop a polygon and break it into 2 polygons")
{
    // crop a polygon resulting in two output polygons:
    Vec input_vec = { {0,0,0}, {10,0,0}, {10,10,0}, {0,10,0}, {0,8,0}, {6,8,0}, {6,2,0}, {0,2,0}, {0,0,0} };
    osgEarth::Polygon input(&input_vec);
    Vec boundary = { {5,-100,0}, {5,100,0}, {-100, 100, 0}, {-100, -100, 0} };
    Ring clip(&boundary);

    auto* result = input.crop(&clip);

    REQUIRE(result);
    REQUIRE(result->getType() == Geometry::TYPE_MULTI);
    auto* multi = dynamic_cast<MultiGeometry*>(result);
    REQUIRE(multi);
    REQUIRE(multi->getNumComponents() == 2);

    Vec part1_output = { {5,0,0}, {5,2,0}, {0,2,0}, {0,0,0} };
    auto part1 = multi->getComponents().at(0);
    REQUIRE(part1->getType() == Geometry::TYPE_POLYGON);
    REQUIRE(part1->size() == 4);
    REQUIRE(polygons_equivalent(part1->asVector(), part1_output));

    Vec part2_output = { {5,8,0}, {5,10,0}, {0,10,0}, {0,8,0}};
    auto part2 = multi->getComponents().at(1);
    REQUIRE(part2->getType() == Geometry::TYPE_POLYGON);
    REQUIRE(part2->size() == 4);
    REQUIRE(polygons_equivalent(part2->asVector(), part2_output));
}

TEST_CASE("Feature::splitAcrossDateLine doesn't modify features that don't cross the dateline")
{
    osg::ref_ptr<Feature> feature = new Feature(GeometryUtils::geometryFromWKT("POLYGON((-81 26, -40.5 45, -40.5 75.5, -81 60))"), osgEarth::SpatialReference::create("wgs84"));
    feature->splitAcrossAntimeridian();
    // We only have one feature in the list.
    REQUIRE(feature->getGeometry()->getType() != Geometry::TYPE_MULTI);
}

TEST_CASE("Feature::splitAcrossDateLine works")
{
    osg::ref_ptr< Feature > feature = new Feature(GeometryUtils::geometryFromWKT("POLYGON((170 26, 190 26, 190 56, 170 56))"), osgEarth::SpatialReference::create("wgs84"));
    feature->splitAcrossAntimeridian();
    auto* geom = feature->getGeometry();
    // We have two features in the list
    REQUIRE(geom->getType() == Geometry::TYPE_MULTI);
    REQUIRE(geom->getComponentType() == Geometry::TYPE_POLYGON);
}

TEST_CASE("Feature handles attributes correctly.")
{
    osg::ref_ptr< Feature > feature = new Feature(new Point(), osgEarth::SpatialReference::create("wgs84"));

    SECTION("Missing attributes get the correct default values.") {
        REQUIRE(feature->getDouble("foo") == 0.0);
        REQUIRE(feature->getBool("foo") == false);
        REQUIRE(feature->getString("foo") == "");
        REQUIRE(feature->getInt("foo") == 0);
    }

    SECTION("Setting attributes works") {
        feature->set("string", std::string("test"));
        REQUIRE(feature->getString("string") == "test");

        feature->set("double", 6.0);
        REQUIRE(feature->getDouble("double") == 6.0);

        feature->set("int", static_cast<std::int64_t>(8));
        REQUIRE(feature->getInt("int") == 8);

        feature->set("int64", static_cast<std::int64_t>(4549941524));
        REQUIRE(feature->getInt("int64") == 4549941524);

        feature->set("int64max", static_cast<std::int64_t>(INT64_MAX));
        REQUIRE(feature->getInt("int64max") == INT64_MAX);

        feature->set("bool", true);
        REQUIRE(feature->getBool("bool") == true);
    }

    SECTION("Null attributes get the correct default values") {
        // First we set the attribute to a value before we set it to null.

        feature->set("string", std::string("test"));
        REQUIRE(feature->isSet("string") == true);

        feature->setNull("string");
        REQUIRE(feature->isSet("string") == false);
        REQUIRE(feature->getString("string") == "");

        feature->set("double", 6.0);
        REQUIRE(feature->isSet("double") == true);
        feature->setNull("double");
        REQUIRE(feature->isSet("double") == false);
        REQUIRE(feature->getDouble("double") == 0.0);

        feature->set("int", std::int64_t(8));
        REQUIRE(feature->isSet("int") == true);
        feature->setNull("int");
        REQUIRE(feature->isSet("int") == false);
        REQUIRE(feature->getInt("int") == 0);

        feature->set("bool", true);
        REQUIRE(feature->isSet("bool") == true);
        feature->setNull("bool");
        REQUIRE(feature->isSet("bool") == false);
        REQUIRE(feature->getBool("bool") == false);
    }
}
