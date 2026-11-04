/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgEarth/catch.hpp>

#include <osgEarth/ImageLayer>
#include <osgEarth/Registry>
#include <osgEarth/GDAL>
#include <osgEarth/FeatureImageLayer>
#include <osgEarth/FeatureCursor>
#include <osgEarth/GeometryUtils>
#include <osgEarth/ImageUtils>
#include <osgEarth/Map>
#include <osgEarth/OGRFeatureSource>
#include <osgEarth/PolygonSymbol>

using namespace osgEarth;

TEST_CASE( "ImageLayers can be created" )
{
    GDALImageLayer* layer = new GDALImageLayer();
    layer->setName("World");
    layer->setURL("../data/world.tif");

    Status status = layer->open();
    REQUIRE( status.isOK() );

    SECTION("Profiles are correct")
    {
        const Profile* profile = layer->getProfile();
        REQUIRE(profile != nullptr);
        REQUIRE(profile->isEquivalentTo(Profile::create(Profile::GLOBAL_GEODETIC)));
    }

    SECTION("Images are read correctly")
    {
        TileKey key(0, 0, 0, layer->getProfile());
        GeoImage image = layer->createImage(key);
        REQUIRE(image.valid());
        REQUIRE(image.getImage()->s() == 256);
        REQUIRE(image.getImage()->t() == 256);
        REQUIRE(image.getExtent() == key.getExtent());
    }
}

TEST_CASE("Attribution works")
{
    std::string attribution = "Attribution test";

    GDALImageLayer* layer = new GDALImageLayer();
    layer->setURL("../data/world.tif");
    layer->setAttribution(attribution);

    Status status = layer->open();

    REQUIRE(status.isOK());
    REQUIRE(layer->getAttribution() == attribution);
}

// Verifies editable feature imagery refreshes cached tiles at multiple levels of detail.
TEST_CASE("FeatureImageLayer refreshes dynamic styles and geometry")
{
    OGRFeatureSource::Options sourceOptions;
    sourceOptions.l2CacheSize() = 8u;
    osg::ref_ptr<OGRFeatureSource> source = new OGRFeatureSource(sourceOptions);
    source->setGeometry(GeometryUtils::geometryFromWKT(
        "POLYGON((-160 10, -110 10, -110 60, -160 60, -160 10))"));
    REQUIRE(source->open().isOK());

    Style red;
    red.getOrCreate<PolygonSymbol>()->fill()->color() = Color::Red;
    osg::ref_ptr<StyleSheet> styles = new StyleSheet();
    styles->addStyle(red);

    FeatureImageLayer::Options options;
    options.l2CacheSize() = 8u;
    osg::ref_ptr<FeatureImageLayer> layer = new FeatureImageLayer(options);
    layer->setCachePolicy(CachePolicy::NO_CACHE);
    layer->setFeatureSource(source.get());
    layer->setStyleSheet(styles.get());

    osg::ref_ptr<Map> map = new Map();
    map->addLayer(layer.get());
    REQUIRE(layer->getStatus().isOK());

    TileKey keys[] = {
        TileKey(0, 0, 0, layer->getProfile()),
        TileKey(1, 0, 0, layer->getProfile())
    };

    // Samples safely inside the polygon, away from antialiased edges at either LOD.
    auto sample = [&](const TileKey& key)
    {
        GeoImage image = layer->createImage(key);
        REQUIRE(image.valid());
        ImageUtils::PixelReaderWithExtent<GeoExtent> reader(image.getImage(), image.getExtent());
        osg::Vec4f color;
        reader.readCoord(color, -135.0, 35.0);
        return color;
    };

    for (const auto& key : keys)
        REQUIRE(sample(key) == Color::Red);

    int revision = layer->getRevision();
    Color expected = Color::Blue;

    SECTION("Replacing the stylesheet invalidates tiles without an explicit dirty call")
    {
        Style blue;
        blue.getOrCreate<PolygonSymbol>()->fill()->color() = expected;
        osg::ref_ptr<StyleSheet> replacement = new StyleSheet();
        replacement->addStyle(blue);
        layer->setStyleSheet(replacement.get());
    }

    SECTION("In-place style edits refresh after dirty")
    {
        styles->getDefaultStyle()->getOrCreate<PolygonSymbol>()->fill()->color() = expected;
        layer->setStyleSheet(styles.get());
        REQUIRE(layer->getRevision() == revision);
        layer->dirty();
    }

    SECTION("Geometry edits refresh source caches and layer data extents")
    {
        Query query;
        query.tileKey() = keys[0];
        auto cursor = source->createFeatureCursor(query);
        REQUIRE(cursor.valid());
        REQUIRE(cursor->hasMore());
        REQUIRE(cursor->nextFeature()->getGeometry()->getBounds().xMin() == -160.0);
        int sourceRevision = source->getRevision();

        source->setGeometry(GeometryUtils::geometryFromWKT(
            "POLYGON((-80 10, -20 10, -20 60, -80 60, -80 10))"));
        layer->dirty();

        REQUIRE(source->getRevision() > sourceRevision);
        REQUIRE(layer->getDataExtentsUnion() == source->getExtent());
        cursor = source->createFeatureCursor(query);
        REQUIRE(cursor.valid());
        REQUIRE(cursor->hasMore());
        REQUIRE(cursor->nextFeature()->getGeometry()->getBounds().xMin() == -80.0);
        expected = Color::Transparent;
    }

    SECTION("Replacing the feature source refreshes cached tiles")
    {
        osg::ref_ptr<OGRFeatureSource> replacement = new OGRFeatureSource();
        replacement->setGeometry(GeometryUtils::geometryFromWKT(
            "POLYGON((-125 10, -110 10, -110 60, -125 60, -125 10))"));
        REQUIRE(replacement->open().isOK());
        layer->setFeatureSource(replacement.get());
        REQUIRE(layer->getDataExtentsUnion() == replacement->getExtent());
        expected = Color::Transparent;
    }

    REQUIRE(layer->getRevision() > revision);
    for (const auto& key : keys)
        REQUIRE(sample(key) == expected);
}