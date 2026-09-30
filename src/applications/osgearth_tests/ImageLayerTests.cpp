/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgEarth/catch.hpp>

#include <osgEarth/ImageLayer>
#include <osgEarth/ImageUtils>
#include <osgEarth/Math>
#include <osgEarth/Registry>
#include <osgEarth/GDAL>

#include <algorithm>
#include <cmath>

using namespace osgEarth;

namespace
{
    osg::ref_ptr<osg::Image> createReprojectionTestImage(unsigned int width, unsigned int height)
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
                pixel[3] = 255u;
            }
        }

        return image;
    }

    class ReprojectionTestImageLayer : public ImageLayer
    {
    public:
        META_LayerNoOptions(osgEarth, ReprojectionTestImageLayer, ImageLayer, reprojection_test_image);

        void init() override
        {
            ImageLayer::init();
            setProfile(Profile::create(Profile::GLOBAL_GEODETIC));
            setTileSize(64u);
            setMaxDataLevel(18u);
            setCachePolicy(CachePolicy::NO_CACHE);
        }

        Status openImplementation() override
        {
            Status parent = ImageLayer::openImplementation();
            if (parent.isError())
                return parent;

            _image = createReprojectionTestImage(getTileSize(), getTileSize());
            return parent;
        }

        GeoImage createImageImplementation(const TileKey& key, ProgressCallback*) const override
        {
            return GeoImage(_image.get(), key.getExtent());
        }

    protected:
        virtual ~ReprojectionTestImageLayer() { }

    private:
        osg::ref_ptr<osg::Image> _image;
    };

    GeoImage createExactAssembledImage(ReprojectionTestImageLayer* layer, const TileKey& key)
    {
        unsigned targetLOD = layer->getProfile()->getEquivalentLOD(key.getProfile(), key.getLOD());

        std::vector<TileKey> intersectingKeys;
        layer->getProfile()->getIntersectingTiles(key, intersectingKeys);

        using KeyedImage = std::pair<TileKey, GeoImage>;
        std::vector<KeyedImage> sources;

        for (auto& intersectingKey : intersectingKeys)
        {
            GeoImage subTile = layer->createImageImplementation(intersectingKey, nullptr);
            if (subTile.valid() && intersectingKey.getLOD() == targetLOD)
            {
                sources.emplace_back(intersectingKey, subTile);
            }
        }

        REQUIRE(!sources.empty());

        std::sort(
            sources.begin(), sources.end(),
            [](const KeyedImage& lhs, const KeyedImage& rhs) {
                return lhs.first.getLOD() > rhs.first.getLOD();
            });

        auto* proto = sources.front().second.getImage();
        unsigned cols = layer->getTileSize();
        unsigned rows = layer->getTileSize();
        unsigned layers = proto->r();

        osg::ref_ptr<osg::Image> mosaic = new osg::Image();
        mosaic->allocateImage(cols, rows, layers, proto->getPixelFormat(), proto->getDataType());
        mosaic->setInternalTextureFormat(proto->getInternalTextureFormat());

        std::vector<osg::Vec3d> points(cols * rows);

        double minx, miny, maxx, maxy;
        key.getExtent().getBounds(minx, miny, maxx, maxy);
        double dx = (maxx - minx) / static_cast<double>(cols);
        double dy = (maxy - miny) / static_cast<double>(rows);

        for (unsigned t = 0; t < rows; ++t)
        {
            double y = miny + (0.5 * dy) + (dy * static_cast<double>(t));
            for (unsigned s = 0; s < cols; ++s)
            {
                double x = minx + (0.5 * dx) + (dx * static_cast<double>(s));
                points[t * cols + s] = { x, y, 0.0 };
            }
        }

        auto* key_srs = key.getExtent().getSRS();
        auto* source_srs = sources.front().second.getSRS();

        Bounds sourceBounds;
        source_srs->getBounds(sourceBounds);

        if (source_srs && key_srs)
        {
            key_srs->transform(points, source_srs);

            if (sourceBounds.valid())
            {
                for (auto& point : points)
                {
                    point.x() = clamp(point.x(), sourceBounds.xMin(), sourceBounds.xMax());
                    point.y() = clamp(point.y(), sourceBounds.yMin(), sourceBounds.yMax());
                }
            }
        }

        std::vector<GeoImagePixelReader> readers;
        for (auto& source : sources)
        {
            readers.emplace_back(source.second);
            readers.back().setBilinear(!layer->isCoverage());
        }

        ImageUtils::PixelWriter write_mosaic(mosaic.get());
        osg::Vec4f pixel;

        write_mosaic.forEachPixel([&](auto& iter)
        {
            unsigned i = iter.t() * cols + iter.s();
            pixel.set(0, 0, 0, 0);

            for (unsigned k = 0; k < sources.size() && pixel.a() == 0.0f; ++k)
            {
                readers[k].readCoordWithoutClamping(pixel, points[i].x(), points[i].y(), iter.r());
            }

            write_mosaic(pixel, iter);
        });

        return GeoImage(mosaic.get(), key.getExtent());
    }
}

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

static void checkImageLayerReprojectionAssemblyMatchesExact(bool coverage)
{
    osg::ref_ptr<ReprojectionTestImageLayer> layer = new ReprojectionTestImageLayer();
    layer->setCoverage(coverage);
    Status status = layer->open();
    REQUIRE(status.isOK());

    osg::ref_ptr<const Profile> outputProfile = Profile::create(Profile::GLOBAL_MERCATOR);
    TileKey key(5u, 16u, 11u, outputProfile.get());

    GeoImage actual = layer->createImage(key, nullptr);
    GeoImage expected = createExactAssembledImage(layer.get(), key);

    REQUIRE(actual.valid());
    REQUIRE(expected.valid());
    REQUIRE(actual.getImage()->s() == expected.getImage()->s());
    REQUIRE(actual.getImage()->t() == expected.getImage()->t());

    ImageUtils::PixelReader readActual(actual.getImage());
    ImageUtils::PixelReader readExpected(expected.getImage());

    osg::Vec4f actualPixel;
    osg::Vec4f expectedPixel;
    const float tolerance = coverage ? 0.0f : 3.0f / 255.0f;

    for (int t = 0; t < actual.getImage()->t(); ++t)
    {
        for (int s = 0; s < actual.getImage()->s(); ++s)
        {
            readActual(actualPixel, s, t);
            readExpected(expectedPixel, s, t);

            for (int c = 0; c < 4; ++c)
            {
                CAPTURE(s);
                CAPTURE(t);
                CAPTURE(c);
                CAPTURE(actualPixel[c]);
                CAPTURE(expectedPixel[c]);
                REQUIRE(std::abs(actualPixel[c] - expectedPixel[c]) <= tolerance);
            }
        }
    }
}

TEST_CASE("ImageLayer reprojection assembly matches exact sampling")
{
    SECTION("imagery")
    {
        checkImageLayerReprojectionAssemblyMatchesExact(false);
    }

    SECTION("coverage")
    {
        checkImageLayerReprojectionAssemblyMatchesExact(true);
    }
}
