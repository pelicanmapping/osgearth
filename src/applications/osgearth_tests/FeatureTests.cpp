/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgEarth/catch.hpp>

#include <osgEarth/Feature>
#include <osgEarth/Geometry>
#include <osgEarth/GeometryUtils>

using namespace osgEarth;

using Vec = std::vector<osg::Vec3d>;

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
