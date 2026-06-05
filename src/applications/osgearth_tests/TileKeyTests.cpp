/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgEarth/catch.hpp>

#include <osgEarth/TileKey>
#include <limits>

using namespace osgEarth;

TEST_CASE("TileKey string formatting")
{
    osg::ref_ptr<const Profile> profile = Profile::create(Profile::GLOBAL_GEODETIC);

    SECTION("valid tile keys")
    {
        REQUIRE(TileKey(0u, 0u, 0u, profile.get()).str() == "0/0/0");
        REQUIRE(TileKey(17u, 123456u, 654321u, profile.get()).str() == "17/123456/654321");
    }

    SECTION("max unsigned values")
    {
        const unsigned int maxValue = std::numeric_limits<unsigned int>::max();
        const std::string expected = std::to_string(maxValue) + "/" +
            std::to_string(maxValue) + "/" +
            std::to_string(maxValue);

        REQUIRE(TileKey(maxValue, maxValue, maxValue, profile.get()).str() == expected);
    }

    SECTION("invalid tile key")
    {
        REQUIRE(TileKey::INVALID.str() == "invalid");
    }
}
