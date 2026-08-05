/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */

#define LC "[osgearth_localize] "

#include <osgEarth/GeoData>
#include <osgEarth/ImageUtils>
#include <osgEarth/Notify>
#include <osgEarth/Profile>
#include <osgEarth/TileKey>

#include <osg/ArgumentParser>
#include <osg/Drawable>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/Texture>

#include <osgDB/Options>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>

#include <osgUtil/Optimizer>

#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>

using namespace osgEarth;
namespace fs = std::filesystem;

namespace
{
    constexpr float DEFAULT_LOD_RANGE_METERS = 3000.0f;
    constexpr float TEXTURE_MAX_ANISOTROPY = 4.0f;

    int
    usage(const char* executable, const std::string& message = {})
    {
        if (!message.empty())
        {
            OE_WARN << LC << message << std::endl;
        }

        std::cout
            << "Builds a geocentrically localized OSGB tile from high- and "
               "low-resolution OBJ files, or from only the low-resolution "
               "file.\n\n"
            << "Usage:\n"
            << "  " << executable
            << " <input.obj> <z> <x> <y> [--lod-range <meters>] "
               "[--low-lod-only]\n\n"
            << "The low-resolution input must be beside the source OBJ and "
               "named <input-stem>_lod0.obj.\n"
            << "The output is <input-stem>.osgb, with texture images embedded.\n"
            << "The input coordinate convention is X=east, Y=up, Z=north, "
               "centered on the Mercator tile.\n\n"
            << "Options:\n"
            << "  --lod-range <meters>  High-resolution LOD range "
            << "(default " << DEFAULT_LOD_RANGE_METERS << ")\n"
            << "  --low-lod-only         Write only <input-stem>_lod0.obj; "
               "do not load the high-resolution OBJ or create an osg::LOD\n"
            << "  --help, -h            Show this help\n";

        return message.empty() ? 0 : 1;
    }

    bool
    parseUnsigned(const char* text, unsigned& output)
    {
        if (!text || !*text || *text == '-')
        {
            return false;
        }

        try
        {
            std::size_t parsed = 0;
            const unsigned long value = std::stoul(text, &parsed, 10);
            if (text[parsed] != '\0' ||
                value > std::numeric_limits<unsigned>::max())
            {
                return false;
            }
            output = static_cast<unsigned>(value);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool
    toECEF(
        const SpatialReference* sourceSRS,
        const SpatialReference* ecefSRS,
        double x,
        double y,
        osg::Vec3d& output)
    {
        return sourceSRS &&
            ecefSRS &&
            sourceSRS->transform(osg::Vec3d(x, y, 0.0), ecefSRS, output);
    }

    bool
    createTileLocalToWorld(
        const TileKey& key,
        osg::Matrixd& output,
        double& eastScale,
        double& northScale)
    {
        const GeoExtent extent = key.getExtent();
        const SpatialReference* mercatorSRS = extent.getSRS();
        const SpatialReference* geographicSRS =
            mercatorSRS ? mercatorSRS->getGeographicSRS() : nullptr;
        const SpatialReference* ecefSRS =
            geographicSRS ? geographicSRS->getGeocentricSRS() : nullptr;
        if (!mercatorSRS || !ecefSRS || extent.width() <= 0.0 ||
            extent.height() <= 0.0)
        {
            return false;
        }

        const double centerX = 0.5 * (extent.xMin() + extent.xMax());
        const double centerY = 0.5 * (extent.yMin() + extent.yMax());

        osg::Vec3d centerECEF;
        osg::Vec3d westECEF;
        osg::Vec3d eastECEF;
        osg::Vec3d southECEF;
        osg::Vec3d northECEF;
        if (!toECEF(mercatorSRS, ecefSRS, centerX, centerY, centerECEF) ||
            !toECEF(
                mercatorSRS,
                ecefSRS,
                extent.xMin(),
                centerY,
                westECEF) ||
            !toECEF(
                mercatorSRS,
                ecefSRS,
                extent.xMax(),
                centerY,
                eastECEF) ||
            !toECEF(
                mercatorSRS,
                ecefSRS,
                centerX,
                extent.yMin(),
                southECEF) ||
            !toECEF(
                mercatorSRS,
                ecefSRS,
                centerX,
                extent.yMax(),
                northECEF))
        {
            return false;
        }

        osg::Matrixd localENUToWorld;
        if (!ecefSRS->createLocalToWorld(centerECEF, localENUToWorld))
        {
            return false;
        }

        // OBJ inputs occupy the full Web Mercator tile in projected metres.
        // Scale that projected footprint to the corresponding ECEF chord
        // lengths before placing it in the local tangent frame.
        eastScale =
            (eastECEF - westECEF).length() / extent.width();
        northScale =
            (northECEF - southECEF).length() / extent.height();
        if (!(eastScale > 0.0) || !(northScale > 0.0))
        {
            return false;
        }

        // osgEarth's tangent frame is X=east, Y=north, Z=up. OSG's OBJ reader
        // rotates the source OBJ from Y-up to Z-up as follows:
        //
        //   source OBJ (x, y, z) -> loaded OSG (x, -z, y)
        //
        // Our source OBJ uses X=east and Z=north, so loaded OSG Y points
        // south. Negate it while converting to the local ENU frame:
        //
        //   loaded OSG (x, y, z) -> ENU
        //       (x*eastScale, -y*northScale, z)
        const osg::Matrixd osgToENU =
            osg::Matrixd::scale(eastScale, -northScale, 1.0);

        output = osgToENU * localENUToWorld;
        return true;
    }

    class SetOverallWhiteColorVisitor : public osg::NodeVisitor
    {
    public:
        SetOverallWhiteColorVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
            _colors = new osg::Vec4Array();
            _colors->setBinding(osg::Array::BIND_OVERALL);
            _colors->push_back(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
        }

        void apply(osg::Geode& geode) override
        {
            for (unsigned i = 0; i < geode.getNumDrawables(); ++i)
            {
                osg::Geometry* geometry =
                    dynamic_cast<osg::Geometry*>(geode.getDrawable(i));
                if (geometry)
                {
                    geometry->setColorArray(
                        _colors.get(), osg::Array::BIND_OVERALL);
                    ++_geometryCount;
                }
            }
            traverse(geode);
        }

        std::size_t geometryCount() const
        {
            return _geometryCount;
        }

    private:
        osg::ref_ptr<osg::Vec4Array> _colors;
        std::size_t _geometryCount = 0;
    };

    struct RemovedTextureUnit
    {
        std::size_t stateSets = 0;
        std::size_t attributes = 0;
        std::size_t modes = 0;
        std::size_t coordinateArrays = 0;
    };

    void
    removeTextureUnitFromStateSet(
        osg::StateSet* stateSet,
        unsigned unit,
        RemovedTextureUnit& removed)
    {
        if (!stateSet)
        {
            return;
        }

        bool changed = false;
        osg::StateSet::TextureAttributeList& attributes =
            stateSet->getTextureAttributeList();
        if (unit < attributes.size() && !attributes[unit].empty())
        {
            removed.attributes += attributes[unit].size();
            attributes[unit].clear();
            changed = true;
        }

        osg::StateSet::TextureModeList& modes =
            stateSet->getTextureModeList();
        if (unit < modes.size() && !modes[unit].empty())
        {
            removed.modes += modes[unit].size();
            modes[unit].clear();
            changed = true;
        }

        if (changed)
        {
            ++removed.stateSets;
        }
    }

    void
    removeTextureUnit(
        osg::Node* node,
        unsigned unit,
        RemovedTextureUnit& removed)
    {
        if (!node)
        {
            return;
        }

        removeTextureUnitFromStateSet(node->getStateSet(), unit, removed);

        osg::Geode* geode = dynamic_cast<osg::Geode*>(node);
        if (geode)
        {
            for (unsigned i = 0; i < geode->getNumDrawables(); ++i)
            {
                osg::Drawable* drawable = geode->getDrawable(i);
                removeTextureUnitFromStateSet(
                    drawable->getStateSet(), unit, removed);

                osg::Geometry* geometry =
                    dynamic_cast<osg::Geometry*>(drawable);
                if (geometry && geometry->getTexCoordArray(unit))
                {
                    geometry->setTexCoordArray(unit, nullptr);
                    ++removed.coordinateArrays;
                }
            }
        }

        osg::Group* group = node->asGroup();
        if (group)
        {
            for (unsigned i = 0; i < group->getNumChildren(); ++i)
            {
                removeTextureUnit(group->getChild(i), unit, removed);
            }
        }
    }

    class EmbedImagesVisitor : public TextureAndImageVisitor
    {
    public:
        using TextureAndImageVisitor::apply;

        void apply(osg::Texture& texture) override
        {
            texture.setResizeNonPowerOfTwoHint(false);
            texture.setFilter(
                osg::Texture::MIN_FILTER,
                osg::Texture::LINEAR_MIPMAP_LINEAR);
            texture.setFilter(
                osg::Texture::MAG_FILTER,
                osg::Texture::LINEAR);
            texture.setUseHardwareMipMapGeneration(true);
            texture.setMaxAnisotropy(TEXTURE_MAX_ANISOTROPY);
            texture.setUnRefImageDataAfterApply(false);
            _textures.insert(&texture);
            TextureAndImageVisitor::apply(texture);
        }

        void apply(osg::Image& image) override
        {
            image.setWriteHint(osg::Image::STORE_INLINE);
            _images.insert(&image);
        }

        void apply(osg::Drawable& drawable) override
        {
            if (drawable.getStateSet())
            {
                TextureAndImageVisitor::apply(*drawable.getStateSet());
            }
        }

        std::size_t imageCount() const
        {
            return _images.size();
        }

        std::size_t textureCount() const
        {
            return _textures.size();
        }

    private:
        std::unordered_set<osg::Texture*> _textures;
        std::unordered_set<osg::Image*> _images;
    };
}

int
main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc, argv);

    if (arguments.read("--help") || arguments.read("-h"))
    {
        return usage(argv[0]);
    }

    const bool lowLodOnly = arguments.read("--low-lod-only");
    float lodRange = DEFAULT_LOD_RANGE_METERS;
    arguments.read("--lod-range", lodRange);

    if (arguments.argc() != 5)
    {
        return usage(
            argv[0],
            "Expected an input OBJ followed by Mercator z, x, and y.");
    }
    if (!(lodRange > 0.0f))
    {
        return usage(argv[0], "--lod-range must be greater than zero.");
    }

    unsigned zoom = 0;
    unsigned tileX = 0;
    unsigned tileY = 0;
    if (!parseUnsigned(arguments[2], zoom) ||
        !parseUnsigned(arguments[3], tileX) ||
        !parseUnsigned(arguments[4], tileY))
    {
        return usage(argv[0], "z, x, and y must be unsigned integers.");
    }
    if (zoom > 30u)
    {
        return usage(argv[0], "z must be 30 or less.");
    }

    const fs::path inputPath =
        fs::absolute(fs::path(arguments[1])).lexically_normal();
    if (inputPath.extension() != ".obj" && inputPath.extension() != ".OBJ")
    {
        return usage(argv[0], "The input filename must have an .obj extension.");
    }
    if (!fs::is_regular_file(inputPath))
    {
        return usage(
            argv[0], "Input OBJ does not exist: " + inputPath.string());
    }

    const fs::path lowResolutionPath =
        inputPath.parent_path() /
        (inputPath.stem().string() + "_lod0" +
         inputPath.extension().string());
    if (!fs::is_regular_file(lowResolutionPath))
    {
        return usage(
            argv[0],
            "Related low-resolution OBJ does not exist: " +
                lowResolutionPath.string());
    }

    fs::path outputPath = inputPath;
    outputPath.replace_extension(".osgb");

    osg::ref_ptr<const Profile> profile =
        Profile::create(Profile::SPHERICAL_MERCATOR);
    if (!profile.valid() || !profile->isOK())
    {
        OE_WARN << LC << "Failed to create the spherical Mercator profile."
                << std::endl;
        return 1;
    }

    unsigned tilesWide = 0;
    unsigned tilesHigh = 0;
    profile->getNumTiles(zoom, tilesWide, tilesHigh);
    if (tileX >= tilesWide || tileY >= tilesHigh)
    {
        OE_WARN << LC << "Tile " << zoom << "/" << tileX << "/" << tileY
                << " is outside the spherical Mercator profile ("
                << tilesWide << "x" << tilesHigh << " tiles at this level)."
                << std::endl;
        return 1;
    }

    const TileKey key(zoom, tileX, tileY, profile.get());
    if (!key.valid())
    {
        OE_WARN << LC << "Failed to create tile key." << std::endl;
        return 1;
    }

    osg::ref_ptr<osg::Node> lowResolution =
        osgDB::readRefNodeFile(lowResolutionPath.string());
    if (!lowResolution.valid())
    {
        OE_WARN << LC << "Failed to load " << lowResolutionPath << std::endl;
        return 1;
    }

    // Simplygon's low-resolution OBJ includes its normal map on texture unit
    // 1. osgEarth does not interpret that OBJ bump-map state correctly, so
    // retain only the diffuse texture on unit 0 in the low LOD.
    RemovedTextureUnit removedLowResolutionNormalMap;
    removeTextureUnit(
        lowResolution.get(), 1u, removedLowResolutionNormalMap);

    lowResolution->setName(
        lowResolutionPath.filename().string() + " low resolution");

    osg::ref_ptr<osg::Node> tileContent;
    osg::ref_ptr<osg::LOD> lod;
    if (lowLodOnly)
    {
        tileContent = lowResolution;
    }
    else
    {
        osg::ref_ptr<osg::Node> highResolution =
            osgDB::readRefNodeFile(inputPath.string());
        if (!highResolution.valid())
        {
            OE_WARN << LC << "Failed to load " << inputPath << std::endl;
            return 1;
        }

        highResolution->setName(
            inputPath.filename().string() + " high resolution");

        lod = new osg::LOD();
        lod->setName(key.str() + " building LOD");
        lod->setRangeMode(osg::LOD::DISTANCE_FROM_EYE_POINT);
        lod->addChild(highResolution.get(), 0.0f, lodRange);
        lod->addChild(
            lowResolution.get(),
            lodRange,
            std::numeric_limits<float>::max());
        tileContent = lod;
    }

    osg::Matrixd localToWorld;
    double eastScale = 1.0;
    double northScale = 1.0;
    if (!createTileLocalToWorld(
            key, localToWorld, eastScale, northScale))
    {
        OE_WARN << LC << "Failed to compute the geocentric localizer for "
                << key.str() << std::endl;
        return 1;
    }

    osg::ref_ptr<osg::MatrixTransform> localized =
        new osg::MatrixTransform(localToWorld);
    localized->setName(key.str() + " geocentric building tile");
    localized->addChild(tileContent.get());

    // Keep the double-precision localizer as a MatrixTransform. The default
    // optimizer mask includes FLATTEN_STATIC_TRANSFORMS, which would bake ECEF
    // coordinates into float vertex arrays and undo the localization.
    const unsigned optimizationMask =
        osgUtil::Optimizer::DEFAULT_OPTIMIZATIONS &
        ~osgUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS;
    osgUtil::Optimizer optimizer;
    optimizer.optimize(localized.get(), optimizationMask);

    // osgEarth's shaders expect a valid color array. Install one shared,
    // immutable white value on every optimized geometry so texturing remains
    // unchanged while the vertex-color binding is always well-defined.
    SetOverallWhiteColorVisitor setOverallWhiteColor;
    localized->accept(setOverallWhiteColor);

    EmbedImagesVisitor embedImages;
    localized->accept(embedImages);

    osg::ref_ptr<osgDB::Options> writeOptions = new osgDB::Options();
    writeOptions->setPluginStringData("WriteImageHint", "IncludeData");
    if (!osgDB::writeNodeFile(
            *localized, outputPath.string(), writeOptions.get()))
    {
        OE_WARN << LC << "Failed to write " << outputPath << std::endl;
        return 1;
    }

    const osg::Vec3d centerECEF =
        osg::Vec3d(0.0, 0.0, 0.0) * localToWorld;
    OE_NOTICE << LC << "Wrote " << outputPath << std::endl
              << "  tile: " << zoom << "/" << tileX << "/" << tileY
              << std::endl;
    if (lowLodOnly)
    {
        OE_NOTICE << "  content: low resolution only (no osg::LOD): "
                  << lowResolutionPath.filename() << std::endl;
    }
    else
    {
        OE_NOTICE << "  high resolution: " << inputPath.filename()
                  << " inside " << lodRange << " m" << std::endl
                  << "  low resolution: " << lowResolutionPath.filename()
                  << " outside " << lodRange << " m" << std::endl;
    }
    OE_NOTICE
              << "  low-resolution texture unit 1 removed from: "
              << removedLowResolutionNormalMap.stateSets << " state sets, "
              << removedLowResolutionNormalMap.coordinateArrays
              << " coordinate arrays" << std::endl
              << "  horizontal scale: east=" << eastScale
              << ", north=" << northScale << std::endl
              << "  ECEF center: " << centerECEF.x() << ", "
              << centerECEF.y() << ", " << centerECEF.z() << std::endl
              << "  white overall color arrays: "
              << setOverallWhiteColor.geometryCount() << std::endl
              << "  mipmapped NPOT textures ("
              << TEXTURE_MAX_ANISOTROPY << "x anisotropy): "
              << embedImages.textureCount() << std::endl
              << "  embedded images: " << embedImages.imageCount()
              << std::endl;

    return 0;
}
