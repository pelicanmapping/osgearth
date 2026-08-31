/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include <osgEarth/catch.hpp>
#include <osgEarth/Config>

#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

using namespace osgEarth;

namespace
{
    bool near(double lhs, double rhs, double epsilon = 1e-6)
    {
        return std::fabs(lhs - rhs) <= epsilon;
    }

    struct ConfigurableValue
    {
        int value = 0;

        ConfigurableValue() = default;
        explicit ConfigurableValue(int in_value) : value(in_value) { }
        explicit ConfigurableValue(const Config& conf)
        {
            conf.get("value", value);
        }

        Config getConfig() const
        {
            Config conf("configurable");
            conf.set("value", value);
            return conf;
        }
    };

    struct ReferencedConfigurableValue : public osg::Referenced
    {
        int value = 0;

        explicit ReferencedConfigurableValue(int in_value = 0) : value(in_value) { }
        explicit ReferencedConfigurableValue(const Config& conf)
        {
            conf.get("value", value);
        }

        Config getConfig() const
        {
            Config conf("referenced");
            conf.set("value", value);
            return conf;
        }
    };
}

OSGEARTH_SPECIALIZE_CONFIG(ConfigurableValue)

TEST_CASE("Config construction, copying, moving, and value typing")
{
    Config empty;
    REQUIRE(empty.empty());
    REQUIRE_FALSE(empty.isSimple());

    Config keyOnly("node");
    REQUIRE_FALSE(keyOnly.empty());
    REQUIRE_FALSE(keyOnly.isSimple());

    Config integer("integer", 42);
    REQUIRE(integer.isSimple());
    REQUIRE(integer.isNumber());
    REQUIRE(integer.key() == "integer");
    REQUIRE(integer.value() == "42");
    REQUIRE(integer.valueAs<int>(-1) == 42);

    Config floatingPoint("floating_point", 1.25);
    REQUIRE(floatingPoint.isNumber());
    REQUIRE(near(floatingPoint.valueAs<double>(-1.0), 1.25));

    Config boolean("boolean", true);
    REQUIRE(boolean.value() == "true");
    REQUIRE_FALSE(boolean.isNumber());

    Config text("text", std::string("hello"));
    REQUIRE(text.value() == "hello");
    REQUIRE_FALSE(text.isNumber());

    Config source("source", "value");
    source.setExternalRef("external.earth");
    source.setIsLocation(true);
    source.add("child", 7);

    Config copy(source);
    REQUIRE(copy.key() == "source");
    REQUIRE(copy.value() == "value");
    REQUIRE(copy.externalRef() == "external.earth");
    REQUIRE(copy.isLocation());
    REQUIRE(copy.child("child").valueAs<int>(-1) == 7);

    Config moved(std::move(source));
    REQUIRE(source.empty());
    REQUIRE(moved.key() == "source");
    REQUIRE(moved.externalRef() == "external.earth");
    REQUIRE(moved.isLocation());
    REQUIRE(moved.child("child").valueAs<int>(-1) == 7);

    Config assigned;
    assigned = std::move(moved);
    REQUIRE(moved.empty());
    REQUIRE(assigned.key() == "source");
    REQUIRE(assigned.child("child").valueAs<int>(-1) == 7);
}

TEST_CASE("Config child lookup and mutation are case-insensitive")
{
    Config root("root");
    root.add("Layer", "first");
    root.add("layer", "second");

    REQUIRE(root.hasChild("LAYER"));
    REQUIRE(root.children("lAyEr").size() == 2u);
    REQUIRE(root.child("LAYER").value() == "first");
    REQUIRE(root.child_ptr("missing") == nullptr);
    REQUIRE(root.child("missing").empty());

    Config* first = root.mutable_child("layer");
    REQUIRE(first != nullptr);
    first->setValue("changed");
    REQUIRE(root.child("LAYER").value() == "changed");

    root.set("LAYER", "replacement");
    REQUIRE(root.children("layer").size() == 1u);
    REQUIRE(root.value("layer") == "replacement");
    REQUIRE(root.hasValue("LaYeR"));

    root.set("blank", " \t\r\n");
    REQUIRE_FALSE(root.hasValue("blank"));
    REQUIRE(root.value("blank").empty());

    Config branch("original");
    branch.add("leaf", 9);
    Config& renamed = root.add("renamed", branch);
    REQUIRE(renamed.key() == "renamed");
    REQUIRE(renamed.child("leaf").valueAs<int>(-1) == 9);
    REQUIRE(branch.key() == "original");

    root.set_with_function("not_added", [](Config&) { });
    REQUIRE_FALSE(root.hasChild("not_added"));

    root.set_with_function("generated", [](Config& conf) {
        conf.set("enabled", true);
    });
    REQUIRE(root.child("generated").child("enabled").valueAs<bool>(false));

    root.remove("lAyEr");
    REQUIRE_FALSE(root.hasChild("layer"));
}

TEST_CASE("Config find, merge, and difference preserve tree semantics")
{
    Config root("root");
    root.add("keep", "original");
    root.add("target", "direct");
    Config& branch = root.add("branch");
    branch.add("target", "nested");
    branch.add("leaf", "value");

    REQUIRE(root.find("ROOT") == &root);
    REQUIRE(root.find("root", false) == nullptr);
    REQUIRE(root.find("TARGET") == root.child_ptr("target"));
    REQUIRE(root.find("leaf") == branch.child_ptr("leaf"));
    REQUIRE(root.find("missing") == nullptr);

    Config overlay("overlay");
    overlay.add("target", "replacement-one");
    overlay.add("TARGET", "replacement-two");
    overlay.add("added", "new");

    root.merge(overlay);
    REQUIRE(root.value("keep") == "original");
    REQUIRE(root.children("target").size() == 2u);
    REQUIRE(root.children("target")[0].value() == "replacement-one");
    REQUIRE(root.children("target")[1].value() == "replacement-two");
    REQUIRE(root.value("added") == "new");

    Config subtract("subtract");
    subtract.add("TARGET", "ignored");
    subtract.add("keep", "ignored");
    Config difference = root - subtract;
    REQUIRE_FALSE(difference.hasChild("target"));
    REQUIRE_FALSE(difference.hasChild("keep"));
    REQUIRE(difference.value("added") == "new");
    REQUIRE(root.hasChild("target"));
}

TEST_CASE("Config primitive access handles values, fallbacks, and boundaries")
{
    Config config("root");
    config.set("boolean", true);
    config.set("short", (std::numeric_limits<short>::min)());
    config.set("unsigned_short", (std::numeric_limits<unsigned short>::max)());
    config.set("integer", (std::numeric_limits<int>::min)());
    config.set("unsigned_integer", (std::numeric_limits<unsigned int>::max)());
    config.set("long", (std::numeric_limits<long>::min)());
    config.set("unsigned_long", (std::numeric_limits<unsigned long>::max)());
    config.set("float", 12.5f);
    config.set("double", 1.25e100);
    config.set("blank", " \t\n");
    config.set("invalid", "not-a-number");

    bool boolean = false;
    REQUIRE(config.get("boolean", boolean));
    REQUIRE(boolean);

    short signedShort = 0;
    unsigned short unsignedShort = 0;
    int integer = 0;
    unsigned int unsignedInteger = 0;
    long signedLong = 0;
    unsigned long unsignedLong = 0;
    float floatingPoint = 0.0f;
    double doublePrecision = 0.0;

    REQUIRE(config.get("short", signedShort));
    REQUIRE(config.get("unsigned_short", unsignedShort));
    REQUIRE(config.get("integer", integer));
    REQUIRE(config.get("unsigned_integer", unsignedInteger));
    REQUIRE(config.get("long", signedLong));
    REQUIRE(config.get("unsigned_long", unsignedLong));
    REQUIRE(config.get("float", floatingPoint));
    REQUIRE(config.get("double", doublePrecision));

    REQUIRE(signedShort == (std::numeric_limits<short>::min)());
    REQUIRE(unsignedShort == (std::numeric_limits<unsigned short>::max)());
    REQUIRE(integer == (std::numeric_limits<int>::min)());
    REQUIRE(unsignedInteger == (std::numeric_limits<unsigned int>::max)());
    REQUIRE(signedLong == (std::numeric_limits<long>::min)());
    REQUIRE(unsignedLong == (std::numeric_limits<unsigned long>::max)());
    REQUIRE(near(floatingPoint, 12.5));
    REQUIRE(near(doublePrecision / 1.25e100, 1.0));

    REQUIRE(config.value<int>("integer", 7) == (std::numeric_limits<int>::min)());
    REQUIRE(config.value<int>("missing", 7) == 7);
    REQUIRE(config.value<std::string>("missing", "fallback") == "fallback");
    REQUIRE_FALSE(config.get("blank", integer));
    REQUIRE(integer == (std::numeric_limits<int>::min)());

    doublePrecision = 19.0;
    REQUIRE(config.get("invalid", doublePrecision));
    REQUIRE(doublePrecision == 19.0);
    REQUIRE_FALSE(config.get("missing", doublePrecision));
    REQUIRE(doublePrecision == 19.0);
}

TEST_CASE("Config optional, percentage, and enumeration overloads round-trip")
{
    Config config("root");

    optional<int> optionalInteger;
    config.set("optional", optionalInteger);
    REQUIRE_FALSE(config.hasChild("optional"));

    optionalInteger = 17;
    config.set("optional", optionalInteger);
    optional<int> parsedInteger(3);
    REQUIRE(config.get("optional", parsedInteger));
    REQUIRE(parsedInteger.isSetTo(17));

    optionalInteger.clear();
    config.set("optional", optionalInteger);
    REQUIRE_FALSE(config.hasChild("optional"));

    optional<double> absolute;
    optional<double> percentage;
    config.set("amount", "25%");
    REQUIRE(config.get("amount", absolute, percentage));
    REQUIRE_FALSE(absolute.isSet());
    REQUIRE(percentage.isSet());
    REQUIRE(near(percentage.get(), 0.25));

    config.set("amount", "0.75");
    REQUIRE(config.get("amount", absolute, percentage));
    REQUIRE(absolute.isSet());
    REQUIRE(near(absolute.get(), 0.75));
    REQUIRE_FALSE(percentage.isSet());

    optional<float> floatValue(0.0f);
    optional<bool> floatIsPercentage(false);
    config.set("float_percentage", "12.5%");
    REQUIRE(config.get("float_percentage", floatValue, floatIsPercentage));
    REQUIRE(floatValue.isSet());
    REQUIRE(near(floatValue.get(), 12.5));
    REQUIRE(floatIsPercentage.isSetTo(true));

    optional<double> doubleValue;
    doubleValue = 25.0;
    optional<bool> doubleIsPercentage;
    doubleIsPercentage = true;
    config.set("encoded_percentage", doubleValue, doubleIsPercentage);
    REQUIRE(config.value("encoded_percentage") == "25%");

    optional<double> noAbsolute;
    optional<double> normalizedPercentage;
    normalizedPercentage = 0.125;
    config.set("normalized_percentage", noAbsolute, normalizedPercentage);
    REQUIRE(config.value("normalized_percentage") == "12.500000%");

    config.set("mode", "fast");
    int enumValue = 0;
    REQUIRE(config.get("mode", "fast", enumValue, 2));
    REQUIRE(enumValue == 2);
    REQUIRE_FALSE(config.get("mode", "slow", enumValue, 3));
    REQUIRE(enumValue == 2);

    optional<int> optionalEnum;
    REQUIRE(config.get("mode", "fast", optionalEnum, 4));
    REQUIRE(optionalEnum.isSetTo(4));
}

TEST_CASE("Config specializations support nested configs, vectors, and configurable objects")
{
    Config root("root");

    Config nested("source_name");
    nested.set("value", 11);
    root.set("nested", nested);
    REQUIRE(root.child("nested").key() == "nested");
    REQUIRE(root.child("nested").child("value").valueAs<int>(-1) == 11);

    optional<Config> optionalConfig;
    optionalConfig = nested;
    root.set("optional_config", optionalConfig);
    optional<Config> parsedConfig;
    REQUIRE(root.get("optional_config", parsedConfig));
    REQUIRE(parsedConfig.isSet());
    REQUIRE(parsedConfig->child("value").valueAs<int>(-1) == 11);

    std::vector<std::string> strings{ "alpha", "beta,gamma", "delta" };
    root.set("strings", strings);
    std::vector<std::string> parsedStrings;
    REQUIRE(root.get("strings", parsedStrings));
    REQUIRE(parsedStrings == strings);

    root.set("strings", std::vector<std::string>());
    REQUIRE_FALSE(root.hasChild("strings"));

    ConfigurableValue input(23);
    root.set("object", input);
    ConfigurableValue output;
    REQUIRE(root.get("object", output));
    REQUIRE(output.value == 23);

    root.add("added_object", ConfigurableValue(29));
    REQUIRE(root.child("added_object").child("value").valueAs<int>(-1) == 29);

    osg::ref_ptr<ReferencedConfigurableValue> referenced = new ReferencedConfigurableValue(31);
    root.set("referenced", referenced);
    osg::ref_ptr<ReferencedConfigurableValue> parsedReferenced;
    REQUIRE(root.get("referenced", parsedReferenced));
    REQUIRE(parsedReferenced.valid());
    REQUIRE(parsedReferenced->value == 31);
}

TEST_CASE("Config OSG vector specializations preserve components")
{
    Config root("root");

    optional<osg::Vec2f> vec2f;
    optional<osg::Vec2d> vec2d;
    optional<osg::Vec3f> vec3f;
    optional<osg::Vec3d> vec3d;
    vec2f = osg::Vec2f(1.25f, -2.5f);
    vec2d = osg::Vec2d(1.25e20, -2.5e-20);
    vec3f = osg::Vec3f(1.0f, 2.0f, 3.0f);
    vec3d = osg::Vec3d(1.0, 2.0, 3.0);

    root.set("vec2f", vec2f);
    root.set("vec2d", vec2d);
    root.set("vec3f", vec3f);
    root.set("vec3d", vec3d);

    optional<osg::Vec2f> parsed2f;
    optional<osg::Vec2d> parsed2d;
    optional<osg::Vec3f> parsed3f;
    optional<osg::Vec3d> parsed3d;
    REQUIRE(root.get("vec2f", parsed2f));
    REQUIRE(root.get("vec2d", parsed2d));
    REQUIRE(root.get("vec3f", parsed3f));
    REQUIRE(root.get("vec3d", parsed3d));

    REQUIRE(near(parsed2f->x(), 1.25));
    REQUIRE(near(parsed2f->y(), -2.5));
    REQUIRE(near(parsed2d->x() / 1.25e20, 1.0));
    REQUIRE(near(parsed2d->y() / -2.5e-20, 1.0));
    REQUIRE(near(parsed3f->x(), 1.0));
    REQUIRE(near(parsed3f->y(), 2.0));
    REQUIRE(near(parsed3f->z(), 3.0));
    REQUIRE(near(parsed3d->x(), 1.0));
    REQUIRE(near(parsed3d->y(), 2.0));
    REQUIRE(near(parsed3d->z(), 3.0));
}

TEST_CASE("Config JSON parsing preserves scalar types and rejects invalid input")
{
    Config parsed;
    REQUIRE(parsed.fromJSON(R"({"root":{"integer":7,"floating":1.25,"boolean":true,"text":"hello"}})"));
    REQUIRE(parsed.key() == "root");
    REQUIRE(parsed.child("integer").valueAs<int>(-1) == 7);
    REQUIRE(parsed.child("integer").isNumber());
    REQUIRE(near(parsed.child("floating").valueAs<double>(-1.0), 1.25));
    REQUIRE(parsed.child("floating").isNumber());
    REQUIRE(parsed.child("boolean").valueAs<bool>(false));
    REQUIRE_FALSE(parsed.child("boolean").isNumber());
    REQUIRE(parsed.value("text") == "hello");

    Config unchanged("original");
    unchanged.set("value", 9);
    REQUIRE_FALSE(unchanged.fromJSON("{not valid JSON"));
    REQUIRE(unchanged.key() == "original");
    REQUIRE(unchanged.child("value").valueAs<int>(-1) == 9);

    Config read = Config::readJSON(R"({"read":{"value":3}})");
    REQUIRE(read.key() == "read");
    REQUIRE(read.child("value").valueAs<int>(-1) == 3);
}

TEST_CASE("Config JSON serialization round-trips nested and repeated children")
{
    Config original("root");
    original.set("integer", 7);
    original.set("text", "hello");
    Config& nested = original.add("nested");
    nested.set("enabled", true);

    Config first("item");
    first.set("name", "first");
    Config second("item");
    second.set("name", "second");
    original.add(first);
    original.add(second);

    const std::string compact = original.toJSON(false);
    const std::string pretty = original.toJSON(true);
    REQUIRE_FALSE(compact.empty());
    REQUIRE_FALSE(pretty.empty());

    Config compactRoundTrip;
    Config prettyRoundTrip;
    REQUIRE(compactRoundTrip.fromJSON(compact));
    REQUIRE(prettyRoundTrip.fromJSON(pretty));

    for (const Config* roundTrip : { &compactRoundTrip, &prettyRoundTrip })
    {
        REQUIRE(roundTrip->key() == "root");
        REQUIRE(roundTrip->child("integer").valueAs<int>(-1) == 7);
        REQUIRE(roundTrip->value("text") == "hello");
        REQUIRE(roundTrip->child("nested").child("enabled").valueAs<bool>(false));
        const ConfigSet items = roundTrip->children("item");
        REQUIRE(items.size() == 2u);
        REQUIRE(items[0].value("name") == "first");
        REQUIRE(items[1].value("name") == "second");
    }

    Config repeatedSimple("root");
    repeatedSimple.add("item", "one");
    repeatedSimple.add("item", "two");
    Config repeatedRoundTrip;
    REQUIRE(repeatedRoundTrip.fromJSON(repeatedSimple.toJSON()));
    REQUIRE(repeatedRoundTrip.key() == "root");
    const ConfigSet repeatedItems = repeatedRoundTrip.children("item");
    REQUIRE(repeatedItems.size() == 2u);
    REQUIRE(repeatedItems[0].value() == "one");
    REQUIRE(repeatedItems[1].value() == "two");
}

TEST_CASE("Config XML parsing preserves elements, attributes, text, and failures")
{
    std::istringstream xml(
        "<Map NAME=\"demo\">"
        "  <Layer enabled=\"true\"> imagery </Layer>"
        "  <Layer enabled=\"false\"> elevation </Layer>"
        "</Map>");

    Config document;
    REQUIRE(document.fromXML(xml));
    REQUIRE(document.key() == "Document");
    REQUIRE(document.children().size() == 1u);

    const Config& map = document.child("map");
    REQUIRE(map.key() == "map");
    REQUIRE(map.value("name") == "demo");
    const ConfigSet layers = map.children("layer");
    REQUIRE(layers.size() == 2u);
    REQUIRE(layers[0].value() == "imagery");
    REQUIRE(layers[0].child("enabled").valueAs<bool>(false));
    REQUIRE(layers[1].value() == "elevation");
    REQUIRE_FALSE(layers[1].child("enabled").valueAs<bool>(true));

    Config unchanged("original");
    std::istringstream invalid("<root><unclosed></root>");
    REQUIRE_FALSE(unchanged.fromXML(invalid));
    REQUIRE(unchanged.key() == "original");
}

TEST_CASE("Config metadata and referrers propagate without overwriting existing context")
{
    Config root("root");
    root.add("before", "value");
    root.setReferrer("https://example.test/config/root.earth");

    REQUIRE(root.referrer() == "https://example.test/config/root.earth");
    REQUIRE(root.child("before").referrer() == root.referrer());

    Config& after = root.add("after", "value");
    REQUIRE(after.referrer() == root.referrer());

    root.setReferrer("https://example.test/config/other.earth");
    REQUIRE(root.referrer() == "https://example.test/config/root.earth");
    REQUIRE(root.child("before").referrer() == root.referrer());

    root.setExternalRef("included.earth");
    root.setIsLocation(true);
    REQUIRE(root.externalRef() == "included.earth");
    REQUIRE(root.isLocation());
    REQUIRE(root.referrer("after") == root.referrer());
}

TEST_CASE("ConfigOptions and DriverConfigOptions preserve and merge configuration")
{
    Config base("options");
    base.set("first", 1);
    ConfigOptions options(base);
    REQUIRE(options.getConfig().child("first").valueAs<int>(-1) == 1);

    Config overlayConfig("options");
    overlayConfig.set("first", 2);
    overlayConfig.set("second", 3);
    ConfigOptions overlay(overlayConfig);
    options.merge(overlay);
    REQUIRE(options.getConfig().child("first").valueAs<int>(-1) == 2);
    REQUIRE(options.getConfig().child("second").valueAs<int>(-1) == 3);

    Config legacyDriver("driver_options");
    legacyDriver.set("type", "legacy");
    DriverConfigOptions driver(legacyDriver);
    REQUIRE(driver.getDriver() == "legacy");
    driver.setDriver("modern");
    REQUIRE(driver.getConfig().value("driver") == "modern");
}
