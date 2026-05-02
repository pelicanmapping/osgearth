/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgEarth/catch.hpp>

#include <osgEarth/FileUtils>
#include <osgEarth/StringUtils>

using namespace osgEarth;
using namespace osgEarth::Util;

TEST_CASE("FileUtils removes and extracts query parameters")
{
    const std::string url = "https://example.com/data/world.tif?token=abc&format=tif";

    REQUIRE(removeQueryParams(url) == "https://example.com/data/world.tif");
    REQUIRE(removeQueryParams("https://example.com/data/world.tif") == "https://example.com/data/world.tif");

    auto params = extractQueryParams(url);
    REQUIRE(params.size() == 2);
    REQUIRE(params["token"] == "abc");
    REQUIRE(params["format"] == "tif");
}

TEST_CASE("getFullPath resolves root-relative URL paths")
{
    const std::string path = getFullPath(
        "https://example.com/maps/world.earth",
        "/tiles/0/0/0.png");

    REQUIRE(path == "https://example.com/tiles/0/0/0.png");
}

TEST_CASE("getFullPath merges URL query parameters")
{
    const std::string path = getFullPath(
        "https://example.com/maps/world.earth?token=abc&z=9",
        "../tiles/tile.png?format=png&token=ignored");

    REQUIRE(path == "https://example.com/tiles/tile.png?format=png&token=abc&z=9");
}

TEST_CASE("StringTokenizer handles multi-character delimiters")
{
    auto tokens = StringTokenizer()
        .delim("::", true)
        .keepEmpties(false)
        .tokenize("alpha::beta::gamma");

    const StringVector expected{ "alpha", "::", "beta", "::", "gamma" };
    REQUIRE(tokens == expected);
}

TEST_CASE("StringUtils unquotes and parses booleans")
{
    REQUIRE(unquote("  '\"hello\"'  ") == "hello");

    REQUIRE(as<bool>("ON", false));
    REQUIRE_FALSE(as<bool>("off", true));
    REQUIRE(as<bool>("maybe", true));
    REQUIRE_FALSE(as<bool>("maybe", false));
}
