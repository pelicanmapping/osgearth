/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgEarth/catch.hpp>
#include <osgEarth/StringUtils>

using namespace osgEarth;
using namespace osgEarth::Util;

TEST_CASE("replaceIn preserves replacement semantics")
{
    SECTION("Empty pattern is a no-op")
    {
        std::string value = "abc";
        REQUIRE(&Strings::replaceIn(value, "", "x") == &value);
        REQUIRE(value == "abc");
    }

    SECTION("No match is a no-op")
    {
        std::string value = "abc";
        REQUIRE(&Strings::replaceIn(value, "z", "x") == &value);
        REQUIRE(value == "abc");
    }

    SECTION("Single match expands")
    {
        std::string value = "a ${x} b";
        REQUIRE(&Strings::replaceIn(value, "${x}", "expanded") == &value);
        REQUIRE(value == "a expanded b");
    }

    SECTION("Multiple matches expand")
    {
        std::string value = "${x}/${x}/${x}";
        REQUIRE(&Strings::replaceIn(value, "${x}", "expanded") == &value);
        REQUIRE(value == "expanded/expanded/expanded");
    }

    SECTION("Multiple matches shrink")
    {
        std::string value = "prefix--middle--suffix";
        REQUIRE(&Strings::replaceIn(value, "--", "-") == &value);
        REQUIRE(value == "prefix-middle-suffix");
    }

    SECTION("Same-size replacements still replace all matches")
    {
        std::string value = "abc abc abc";
        REQUIRE(&Strings::replaceIn(value, "abc", "xyz") == &value);
        REQUIRE(value == "xyz xyz xyz");
    }

    SECTION("Matches remain non-overlapping")
    {
        std::string value = "aaa";
        REQUIRE(&Strings::replaceIn(value, "aa", "a") == &value);
        REQUIRE(value == "aa");
    }
}
