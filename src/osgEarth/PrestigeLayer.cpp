/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/PrestigeLayer>
#include <osgEarth/Map>
#include <osgEarth/JsonUtils>
#include <osgEarth/LineDrawable>
#include <osgEarth/PagedNode>
#include <osgEarth/Progress>
#include <osgEarth/Registry>
#include <osgEarth/ShaderGenerator>
#include <osgEarth/StringUtils>
#include <osgDB/ReadFile>
#include <osg/MatrixTransform>
#include <osgUtil/Optimizer>

#include <cmath>
#include <iomanip>
#include <vector>

using namespace osgEarth;
using namespace osgEarth::Util;

REGISTER_OSGEARTH_LAYER(Prestige, PrestigeLayer);

namespace
{
    // Demo hack: repeat this one generated tile at every requested tile location.
    constexpr unsigned PRESTIGE_DEMO_Z = 14u;
    constexpr unsigned PRESTIGE_DEMO_X = 4823u;
    constexpr unsigned PRESTIGE_DEMO_Y = 6160u;

    std::string makeTileStem(unsigned z, unsigned x, unsigned y)
    {
        return Stringify()
            << "tile_" << z
            << "_" << x
            << "_" << y;
    }

    std::string makeLOD2TileName(unsigned z, unsigned x, unsigned y)
    {
        return makeTileStem(z, x, y) + "_lod2.glb";
    }

    std::string makeDetailTileName(
        unsigned z,
        unsigned x,
        unsigned y,
        unsigned lod,
        unsigned cellX,
        unsigned cellZ)
    {
        return Stringify()
            << makeTileStem(z, x, y)
            << "_lod" << lod
            << "_x" << std::setfill('0') << std::setw(3) << cellX
            << "_y000"
            << "_z" << std::setfill('0') << std::setw(3) << cellZ
            << ".glb";
    }

    URI makeTileURI(const URI& base, const std::string& filename)
    {
        const std::string separator =
            endsWith(base.full(), "/") || endsWith(base.full(), "\\") ? "" : "/";

        return base.append(separator + filename);
    }

    osg::ref_ptr<osg::Node> readModel(
        const URI& uri,
        const osgDB::Options* readOptions,
        ProgressCallback* progress,
        std::shared_ptr<ChonkFactory> chonks = nullptr)
    {
        if (progress && progress->isCanceled())
            return {};

        osg::ref_ptr< osg::Node > node = osgDB::readNodeFile(uri.full(), readOptions);
        if (progress && progress->isCanceled())
            return {};
        return chonks && node ? ChonkFactory::convertExternalInstances(node.get(), chonks) : node;
    }

    bool readSidecarBounds(
        const URI& uri,
        const osgDB::Options* readOptions,
        ProgressCallback* progress,
        osg::BoundingSphered& output)
    {
        const ReadResult result = uri.readString(readOptions, progress);
        if (result.failed())
            return false;

        Json::Value document;
        Json::Reader reader;
        if (!reader.parse(result.getString(), document))
            return false;

        const Json::Value& values = document["cell_bounds_min_max"];
        if (!values.isArray() || values.size() != 6u)
            return false;

        double value[6];
        for (unsigned i = 0u; i < 6u; ++i)
        {
            if (!values[i].isNumeric())
                return false;

            value[i] = values[i].asDouble();
            if (!std::isfinite(value[i]))
                return false;
        }

        if (value[0] > value[3] || value[1] > value[4] || value[2] > value[5])
            return false;

        const std::string upAxis = document.get("up_axis", "Y").asString();
        osg::BoundingBoxd bounds;

        if (upAxis == "Y")
        {
            // osgEarth's glTF reader rotates Y-up glTF coordinates to Z-up:
            // (x, y, z) -> (x, -z, y).
            bounds.set(
                value[0], -value[5], value[1],
                value[3], -value[2], value[4]);
        }
        else if (upAxis == "Z")
        {
            bounds.set(
                value[0], value[1], value[2],
                value[3], value[4], value[5]);
        }
        else
        {
            return false;
        }

        output.set(bounds.center(), bounds.radius());
        return output.valid();
    }

    osg::ref_ptr<osg::Node> createWireframeSphere(const osg::BoundingSphered& bounds)
    {
        constexpr unsigned segments = 64u;
        constexpr double twoPi = 6.28318530717958647692;

        osg::ref_ptr<osg::MatrixTransform> transform =
            new osg::MatrixTransform(osg::Matrixd::translate(bounds.center()));
        transform->setName("Prestige LOD1 paging bound");

        for (unsigned axis = 0u; axis < 3u; ++axis)
        {
            osg::ref_ptr<LineDrawable> ring = new LineDrawable(GL_LINE_LOOP);
            ring->setColor(osg::Vec4(1.0f, 0.65f, 0.0f, 1.0f));
            ring->setLineWidth(2.0f);

            for (unsigned i = 0u; i < segments; ++i)
            {
                const double angle = twoPi * static_cast<double>(i) / segments;
                const double a = bounds.radius() * std::cos(angle);
                const double b = bounds.radius() * std::sin(angle);

                ring->pushVertex(
                    axis == 0u ? osg::Vec3d(0.0, a, b) :
                    axis == 1u ? osg::Vec3d(a, 0.0, b) :
                                 osg::Vec3d(a, b, 0.0));
            }

            ring->finish();
            transform->addChild(ring);
        }

        return transform;
    }

    void setBounds(PagedNode2* pagedNode, const osg::BoundingSphered& bounds)
    {
        if (bounds.valid())
        {
            pagedNode->setCenter(bounds.center());
            pagedNode->setRadius(bounds.radius());
        }
    }

    // Matches FeaturesToNodeFilter::computeLocalizers. Prestige exports an
    // East-North-Up plane centered on the Mercator tile, so this matrix puts
    // that local plane back into the map's world coordinate system.
    bool createLocalToWorld(
        const TileKey& key,
        const SpatialReference* mapSRS,
        osg::Matrixd& output)
    {
        if (!mapSRS)
            return false;

        const GeoExtent extent = key.getExtent();
        if (!extent.isValid())
            return false;

        if (mapSRS->isGeographic())
        {
            const SpatialReference* geographicSRS = mapSRS->getGeographicSRS();
            const SpatialReference* geocentricSRS = geographicSRS->getGeocentricSRS();
            const GeoExtent geographicExtent = extent.transform(geographicSRS);

            if (!geographicExtent.isValid() || geographicExtent.width() >= 180.0)
                return false;

            osg::Vec3d centroid;
            geographicExtent.getCentroid(centroid.x(), centroid.y());

            osg::Vec3d centroidECEF;
            if (!geographicSRS->transform(centroid, geocentricSRS, centroidECEF))
                return false;

            return geocentricSRS->createLocalToWorld(centroidECEF, output);
        }

        osg::Vec3d centroid;
        extent.getCentroid(centroid.x(), centroid.y());
        if (!extent.getSRS()->transform(centroid, mapSRS, centroid))
            return false;

        output.makeTranslate(centroid);
        return true;
    }

    // Creates a stable paging bound from a geographic grid cell instead of
    // deriving one from the cell's (potentially sparse) model contents.
    osg::BoundingSphered createLocalBounds(
        const GeoExtent& extent,
        const Profile* mapProfile,
        const osg::Matrixd& worldToLocal)
    {
        if (!mapProfile || !extent.isValid())
            return {};

        const GeoExtent workingExtent = mapProfile->clampAndTransformExtent(extent);
        if (!workingExtent.isValid())
            return {};

        osg::BoundingSphered bounds = workingExtent.createWorldBoundingSphere(0.0, 0.0);
        if (bounds.valid())
            bounds.center() = bounds.center() * worldToLocal;

        return bounds;
    }

    GeoExtent createGridCellExtent(
        const GeoExtent& tileExtent,
        unsigned gridSize,
        unsigned column,
        unsigned row)
    {
        const double width = tileExtent.width() / static_cast<double>(gridSize);
        const double height = tileExtent.height() / static_cast<double>(gridSize);

        const double west = tileExtent.xMin() + static_cast<double>(column) * width;
        const double east = column + 1u == gridSize ?
            tileExtent.xMax() : west + width;

        // Prestige's glTF Z axis points south, so its Z cell indices increase
        // from the north edge of the source tile.
        const double north = tileExtent.yMax() - static_cast<double>(row) * height;
        const double south = row + 1u == gridSize ?
            tileExtent.yMin() : north - height;

        return GeoExtent(tileExtent.getSRS(), west, south, east, north);
    }
}

void PrestigeLayer::Options::fromConfig(const Config& conf)
{
    conf.get("url", url());
    conf.get("invert_y", invertY());
    conf.get("split", split());
    conf.get("skip_lod1", skipLOD1());
    conf.get("grid_size", gridSize());
    conf.get("lod1_range", lod1Range());
    conf.get("lod0_range", lod0Range());
}

Config PrestigeLayer::Options::getConfig() const
{
    Config conf = TiledModelLayer::Options::getConfig();
    conf.set("url", url());
    conf.set("invert_y", invertY());
    conf.set("split", split());
    conf.set("skip_lod1", skipLOD1());
    conf.set("grid_size", gridSize());
    conf.set("lod1_range", lod1Range());
    conf.set("lod0_range", lod0Range());
    return conf;
}

OE_LAYER_PROPERTY_IMPL(PrestigeLayer, URI, URL, url);

void PrestigeLayer::setInvertY(bool value)
{
    options().invertY() = value;
}

bool PrestigeLayer::getInvertY() const
{
    return options().invertY().get();
}

void PrestigeLayer::setSplit(bool value)
{
    options().split() = value;
}

bool PrestigeLayer::getSplit() const
{
    return options().split().get();
}

void PrestigeLayer::setGridSize(unsigned value)
{
    options().gridSize() = value;
}

unsigned PrestigeLayer::getGridSize() const
{
    return options().gridSize().get();
}

void PrestigeLayer::setSkipLOD1(bool value)
{
    options().skipLOD1() = value;
}

bool PrestigeLayer::getSkipLOD1() const
{
    return options().skipLOD1().get();
}

void PrestigeLayer::setLOD1Range(float value)
{
    options().lod1Range() = value;
}

float PrestigeLayer::getLOD1Range() const
{
    return options().lod1Range().get();
}

void PrestigeLayer::setLOD0Range(float value)
{
    options().lod0Range() = value;
}

float PrestigeLayer::getLOD0Range() const
{
    return options().lod0Range().get();
}

PrestigeLayer::~PrestigeLayer()
{
    // nop
}

void PrestigeLayer::setProfile(const Profile* profile)
{
    _profile = profile;
    if (_profile.valid())
        options().profile() = profile->toProfileOptions();
}

Config PrestigeLayer::getConfig() const
{
    return TiledModelLayer::getConfig();
}

Status PrestigeLayer::openImplementation()
{
    Status parent = super::openImplementation();
    if (parent.isError())
        return parent;

    if (!options().url().isSet() || options().url()->empty())
        return Status(Status::ConfigurationError, "Missing required url");

    if (!options().profile().isSet())
        return Status(Status::ConfigurationError, "Missing required profile");

    _profile = Profile::create(*options().profile());
    if (!_profile.valid())
        return Status(Status::ConfigurationError, "Invalid profile");

    if (*options().gridSize() == 0u)
        return Status(Status::ConfigurationError, "grid_size must be greater than zero");

    if (*options().lod0Range() > *options().lod1Range())
        return Status(Status::ConfigurationError, "lod0_range must be less than or equal to lod1_range");

    return Status::NoError;
}

void PrestigeLayer::addedToMap(const Map* map)
{
    _readOptions = Registry::instance()->cloneOrCreateOptions(getReadOptions());
    _readOptions->setObjectCacheHint(osgDB::Options::CACHE_IMAGES);
    super::addedToMap(map);
    // Keep the factory alive with paged cells, and share prototypes/textures
    // across independent detail loads. TiledModelLayer owns the arena's update.
    _detailChonks = _textures.valid() ? std::make_shared<ChonkFactory>(_textures.get()) : nullptr;
}

osg::ref_ptr<osg::Node> PrestigeLayer::createTileImplementation(
    const TileKey& key,
    ProgressCallback* progress) const
{
    if (progress && progress->isCanceled())
        return {};

    unsigned x, y;
    key.getTileXY(x, y);

    if (*options().invertY())
    {
        unsigned cols = 0u, rows = 0u;
        key.getProfile()->getNumTiles(key.getLOD(), cols, rows);
        y = rows - y - 1u;
    }

    //x = PRESTIGE_DEMO_X;
    //y = PRESTIGE_DEMO_Y;
    const unsigned z = key.getLOD();
    const TileKey sourceKey(z, x, y, key.getProfile());
    const osg::ref_ptr<const Map> map = getMap();
    osg::Matrixd localToWorld;
    if (!createLocalToWorld(sourceKey, map.valid() ? map->getSRS() : nullptr, localToWorld))
        return {};

    const osg::Matrixd worldToLocal = osg::Matrixd::inverse(localToWorld);
    const osg::BoundingSphered tileBounds = createLocalBounds(
        sourceKey.getExtent(),
        map.valid() ? map->getProfile() : nullptr,
        worldToLocal);
    if (!tileBounds.valid())
        return {};

    const URI base = *options().url();
    const std::string lod2Name = makeLOD2TileName(z, x, y);
    const URI lod2URI = makeTileURI(base, lod2Name);

    osg::ref_ptr<osg::Node> lod2 = readModel(lod2URI, _readOptions.get(), progress);
    if (!lod2.valid())
        return {};

    //ImageUtils::compressAndMipmapTextures(lod2.get());




    osg::ref_ptr<PagedNode2> lod2Pager = new PagedNode2();
    lod2Pager->setName(lod2Name);
    lod2Pager->addChild(lod2);
    lod2Pager->setRefinePolicy(REFINE_REPLACE);
    lod2Pager->setLODMethod(LODMethod::CAMERA_DISTANCE);

    setBounds(lod2Pager, tileBounds);

    const float lod0Range = *options().lod0Range();
    const osg::ref_ptr<osgDB::Options> readOptions = _readOptions;
    const auto detailChonks = _detailChonks;

    if (!*options().split())
    {
        // Unsplit scheme: a single full-tile high-resolution gltf replaces
        // the LOD2 tile at the lod0 range.
        lod2Pager->setMaxRange(lod0Range);

        const URI fullURI = makeTileURI(base, makeTileStem(z, x, y) + ".gltf");

        lod2Pager->setLoadFunction(
            [fullURI, readOptions, detailChonks](Cancelable* cancelable)
            {
                osg::ref_ptr<ProgressCallback> progress = new ProgressCallback(cancelable);
                return readModel(fullURI, readOptions.get(), progress.get(), detailChonks);
            });
    }
    else
    {
        const unsigned gridSize = *options().gridSize();
        const bool skipLOD1 = *options().skipLOD1();
        std::vector<osg::BoundingSphered> cellBounds;
        cellBounds.reserve(static_cast<std::size_t>(gridSize) * gridSize);

        for (unsigned row = 0u; row < gridSize; ++row)
        {
            for (unsigned col = 0u; col < gridSize; ++col)
            {
                const GeoExtent cellExtent = createGridCellExtent(
                    sourceKey.getExtent(), gridSize, col, row);
                osg::BoundingSphered bounds = createLocalBounds(
                    cellExtent,
                    map->getProfile(),
                    worldToLocal);
                if (!bounds.valid())
                    return {};

                cellBounds.emplace_back(bounds);
            }
        }

        lod2Pager->setMaxRange(*options().lod1Range());

        lod2Pager->setLoadFunction(
        [base, z, x, y, lod0Range, gridSize, skipLOD1, cellBounds, readOptions, detailChonks](Cancelable* cancelable)
        {
            osg::ref_ptr<ProgressCallback> progress = new ProgressCallback(cancelable);
            osg::ref_ptr<osg::Group> group = new osg::Group();
            group->setName(Stringify() << "Prestige LOD1 group " << z << "/" << x << "/" << y);

            for (unsigned row = 0u; row < gridSize; ++row)
            {
                for (unsigned col = 0u; col < gridSize; ++col)
                {
                    if (progress->isCanceled())
                        return osg::ref_ptr<osg::Node>();

                    const std::string lod1Name =
                        makeDetailTileName(
                            z,
                            x,
                            y,
                            1u,
                            col,
                            row);
                    const std::string lod0Name =
                        makeDetailTileName(
                            z,
                            x,
                            y,
                            0u,
                            col,
                            row);
                    const URI lod1URI = makeTileURI(base, lod1Name);
                    const URI lod0URI = makeTileURI(base, lod0Name);
                    const URI sidecarURI = lod0URI.append(".json");

                    const std::size_t cellIndex =
                        static_cast<std::size_t>(row) * gridSize + col;
                    osg::BoundingSphered pagingBounds = cellBounds[cellIndex];
                    const bool hasSidecarBounds = readSidecarBounds(
                        sidecarURI,
                        readOptions.get(),
                        progress.get(),
                        pagingBounds);

                    if (progress->isCanceled())
                        return osg::ref_ptr<osg::Node>();

                    osg::ref_ptr<osg::Node> lod1;
                    if (skipLOD1)
                    {
                        if (!hasSidecarBounds)
                            continue;

                        lod1 = createWireframeSphere(pagingBounds);
                    }
                    else
                    {
                        lod1 = readModel(
                            lod1URI,
                            readOptions.get(),
                            progress.get());
                    }

                    if (!lod1.valid())
                        continue;

                    //ImageUtils::compressAndMipmapTextures(lod1.get());

                    osg::ref_ptr<PagedNode2> lod1Pager = new PagedNode2();
                    lod1Pager->setName(lod1Name);
                    lod1Pager->addChild(lod1);
                    lod1Pager->setRefinePolicy(REFINE_REPLACE);
                    lod1Pager->setLODMethod(LODMethod::CAMERA_DISTANCE);
                    lod1Pager->setMaxRange(lod0Range);
                    lod1Pager->setPreCompileGLObjects(false);
                    setBounds(lod1Pager, pagingBounds);

                    lod1Pager->setLoadFunction(
                        [lod0URI, readOptions, detailChonks](Cancelable* lod0Cancelable)
                        {
                            osg::ref_ptr<ProgressCallback> lod0Progress =
                                new ProgressCallback(lod0Cancelable);

                            osg::ref_ptr<osg::Node> lod0 = readModel(
                                lod0URI,
                                readOptions.get(),
                                lod0Progress.get(),
                                detailChonks);

                            if (lod0.valid())
                            {
                                //ImageUtils::compressAndMipmapTextures(lod0.get());
                            }

#if 0
                            if (lod0.valid())
                                Registry::shaderGenerator().run(lod0.get(), Registry::stateSetCache());
#endif
                            return lod0;
                        });

                    group->addChild(lod1Pager);
                }
            }

            if (group->getNumChildren() == 0u)
                return osg::ref_ptr<osg::Node>();

            //Registry::shaderGenerator().run(group.get(), Registry::stateSetCache());
            return osg::ref_ptr<osg::Node>(group.release());
        });
    }

    osg::ref_ptr<osg::MatrixTransform> localized = new osg::MatrixTransform(localToWorld);
    localized->setName(makeTileStem(z, x, y) + " local-to-world");
    localized->addChild(lod2Pager);
    return localized;
}
