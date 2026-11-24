/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarthPrestige/ClusterMeshLayer>
#include <osgEarth/ElevationLayer>
#include <osgEarth/ElevationPool>
#include <osgEarth/GeoData>
#include <osgEarth/Map>
#include <osgEarth/NodeUtils>
#include <osgEarth/PagedNode>
#include <osgEarth/Progress>
#include <osgEarth/Registry>
#include <osgEarth/SimplePager>
#include <osgEarth/SimplexNoise>
#include <osgDB/ReadFile>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

#define LC "[ClusterMeshLayer] \"" << getName() << "\": "

using namespace osgEarth;
using namespace osgEarth::Util;

namespace osgEarthPrestige
{
REGISTER_OSGEARTH_LAYER_FACTORY("prestige:clustermesh", ClusterMeshLayer);

namespace
{
    // Most placements one model may have in one tile, whatever the configured density.
    const unsigned MAX_PLACEMENTS_PER_TILE = 250000u;

    // The cluster meshes draw after the terrain and other opaque layers, whose depth then
    // hides instances from the occlusion test, and before the transparent bin.
    const int RENDER_BIN = 9;

    //! The independent objects of a loaded model: the children of the first group that has
    //! more than one, or the model itself. Megascans plant files hold one variant per node.
    std::vector<osg::Node*> findVariants(osg::Node* root)
    {
        osg::Group* group = root->asGroup();
        while (group && group->getNumChildren() == 1u && !group->getChild(0)->asGeode())
            group = group->getChild(0)->asGroup();

        std::vector<osg::Node*> result;
        if (group && group->getNumChildren() > 1u && !group->asGeode())
        {
            for (unsigned i = 0; i < group->getNumChildren(); ++i)
                result.push_back(group->getChild(i));
        }
        else
        {
            result.push_back(root);
        }
        return result;
    }

    //! One instance before it is clamped: where, of what, and how it is posed.
    struct Placement
    {
        osg::Vec3d point; // map coordinates; z is filled in by the elevation query
        ClusterMesh::Ptr mesh;
        float scale = 1.0f;
        float sink = 0.0f;
        double yaw = 0.0;       // radians about the vertical
        double tilt = 0.0;      // radians away from the vertical
        double tiltAxis = 0.0;  // radians; heading of the horizontal axis it leans about
    };

    //! Hermite step from 0 at a to 1 at b.
    double smoothstep(double a, double b, double x)
    {
        const double t = std::max(0.0, std::min(1.0, (x - a) / (b - a)));
        return t * t * (3.0 - 2.0 * t);
    }
}

Config
ClusterMeshLayer::Model::getConfig() const
{
    Config conf("model");
    conf.set("url", url);
    conf.set("density", density);
    conf.set("min_scale", minScale);
    conf.set("max_scale", maxScale);
    conf.set("sink", sink);
    conf.set("tilt", tilt);
    conf.set("clumping", clumping);
    conf.set("patch_size", patchSize);
    conf.set("level_offset", levelOffset);
    return conf;
}

void
ClusterMeshLayer::Model::fromConfig(const Config& conf)
{
    conf.get("url", url);
    conf.get("density", density);
    conf.get("min_scale", minScale);
    conf.get("max_scale", maxScale);
    conf.get("sink", sink);
    conf.get("tilt", tilt);
    conf.get("clumping", clumping);
    conf.get("patch_size", patchSize);
    conf.get("level_offset", levelOffset);
}

Config
ClusterMeshLayer::Options::getConfig() const
{
    Config conf = TiledModelLayer::Options::getConfig();
    conf.set("level", level());
    conf.set("range", range());
    conf.set("density", density());
    conf.set("seed", seed());
    conf.set("error_threshold", errorThreshold());
    conf.set("min_instance_pixels", minInstancePixels());
    conf.set("occlusion_culling", occlusionCulling());
    conf.set("max_drawn_clusters", maxDrawnClusters());
    conf.set("max_texture_size", maxTextureSize());
    conf.set("cluster_cache", clusterCache());
    if (!models().empty())
    {
        Config list("models");
        for (const auto& model : models())
            list.add(model.getConfig());
        conf.add(list);
    }
    return conf;
}

void
ClusterMeshLayer::Options::fromConfig(const Config& conf)
{
    conf.get("level", level());
    conf.get("range", range());
    conf.get("density", density());
    conf.get("seed", seed());
    conf.get("error_threshold", errorThreshold());
    conf.get("min_instance_pixels", minInstancePixels());
    conf.get("occlusion_culling", occlusionCulling());
    conf.get("max_drawn_clusters", maxDrawnClusters());
    conf.get("max_texture_size", maxTextureSize());
    conf.get("cluster_cache", clusterCache());
    models().clear();
    for (const auto& child : conf.child("models").children("model"))
    {
        Model model;
        model.fromConfig(child);
        models().push_back(model);
    }
}

void
ClusterMeshLayer::init()
{
    super::init();

    // Finer tiles add their models to what the coarser tiles already show.
    options().additive() = true;

    _clusterNode = new ClusterMeshNode();
    _clusterNode->setErrorThreshold(options().errorThreshold().get());
    _clusterNode->setMinInstancePixels(options().minInstancePixels().get());
    _clusterNode->setOcclusionCulling(options().occlusionCulling().get());
    _clusterNode->setMaxDrawnClusters(options().maxDrawnClusters().get());

    // TiledModelLayer puts its content in the transparent bin with blending forced on.
    // Cluster meshes are opaque and belong with the opaque geometry.
    auto* ss = _clusterNode->getOrCreateStateSet();
    ss->setRenderBinDetails(RENDER_BIN, "RenderBin");
    // Not nested: a bin inside the layer's transparent bin would draw after everything in it.
    ss->setNestRenderBins(false);
    ss->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED);
}

Status
ClusterMeshLayer::openImplementation()
{
    Status parent = super::openImplementation();
    if (parent.isError())
        return parent;

    if (options().models().empty())
        return Status(Status::ConfigurationError, "No models configured");

    for (const auto& model : options().models())
        if (!model.url.isSet() || model.url->empty())
            return Status(Status::ConfigurationError, "A model is missing its url");

    return Status::NoError;
}

void
ClusterMeshLayer::addedToMap(const Map* map)
{
    // The base class needs the profile to build its pager.
    _profile = map->getProfile();
    super::addedToMap(map);

    if (!_textures.valid())
    {
        setStatus(Status::ResourceUnavailable, "Cluster meshes need the bindless OpenGL 4.6 path; run with --nvgl");
        return;
    }
    _textures->setMaxTextureSize(options().maxTextureSize().get());
}

Status
ClusterMeshLayer::closeImplementation()
{
    {
        std::lock_guard<std::mutex> lock(_assetsMutex);
        _assets.reset();
    }
    // Tiles still held by a cache or a paging thread stop drawing along with the rest.
    if (_clusterNode.valid())
        _clusterNode->clear();
    return super::closeImplementation();
}

unsigned
ClusterMeshLayer::getMaxLevel() const
{
    unsigned offset = 0u;
    for (const auto& model : options().models())
        offset = std::max(offset, model.levelOffset.get());
    return getLevel() + offset;
}

void
ClusterMeshLayer::create()
{
    super::create();

    // The pager measures the distance to a tile from a bounding sphere that spans the
    // elevation range of the whole region, and loads within a multiple of that sphere's
    // radius. In mountains the sphere is kilometers across, and so is the loaded area.
    // Give the paged nodes that load this layer's tiles a sphere around the terrain that
    // is really there, and a range in meters. A node loads the tiles one level below it;
    // each finer level loads within half the distance. This runs on the paging threads.
    if (auto* pager = osgEarth::findTopMostNodeOfType<SimplePager>(_root.get()))
    {
        const unsigned level = getLevel();
        const float range = std::max(1.0f, getRange());
        osg::observer_ptr<const Map> weakMap(getMap().get());
        pager->setConfigurePagedNodeFunction([level, range, weakMap](const TileKey& key, PagedNode2* node)
            {
                const unsigned child = key.getLOD() + 1u;
                if (child < level)
                {
                    // An ancestor must be in range whenever the tiles under it are. Its
                    // generous default sphere guarantees that.
                    node->setMaxRange(std::max(node->getMaxRange(), range));
                    return;
                }

                osg::ref_ptr<const Map> map;
                if (weakMap.lock(map))
                {
                    const GeoExtent& extent = key.getExtent();
                    auto sample = map->getElevationPool()->getSample(extent.getCentroid(), nullptr);
                    if (sample.hasData())
                    {
                        // Allow for relief inside the tile in proportion to its size. This
                        // sphere also frustum-culls the tile, so it must not be too tight:
                        // the margin covers a 45 degree slope across the tile, and the models.
                        const double height = sample.elevation().as(Units::METERS);
                        const double margin = std::max(100.0, extent.width(Units::METERS));
                        const osg::BoundingSphered sphere = extent.createWorldBoundingSphere(height - margin,
                            height + margin);
                        node->setCenter(sphere.center());
                        node->setRadius(float(sphere.radius()));
                    }
                }
                node->setMaxRange(std::ldexp(range, -int(child - level)));
            });
    }

    if (_clusterNode.valid() && _root.valid() && !_root->containsNode(_clusterNode.get()))
        _root->addChild(_clusterNode.get());
}

void
ClusterMeshLayer::setRange(float meters)
{
    meters = std::max(meters, 1.0f);
    if (meters != getRange())
    {
        options().range() = meters;
        dirty();
    }
}

float
ClusterMeshLayer::getRange() const
{
    return options().range().get();
}

void
ClusterMeshLayer::setLevel(unsigned value)
{
    if (value != getLevel())
    {
        options().level() = value;
        dirty();
    }
}

unsigned
ClusterMeshLayer::getLevel() const
{
    return options().level().get();
}

void
ClusterMeshLayer::setDensity(float value)
{
    value = std::max(value, 0.0f);
    if (value != getDensity())
    {
        options().density() = value;
        dirty();
    }
}

float
ClusterMeshLayer::getDensity() const
{
    return options().density().get();
}

void
ClusterMeshLayer::setSeed(unsigned value)
{
    if (value != getSeed())
    {
        options().seed() = value;
        dirty();
    }
}

unsigned
ClusterMeshLayer::getSeed() const
{
    return options().seed().get();
}

void
ClusterMeshLayer::setErrorThreshold(float pixels)
{
    options().errorThreshold() = pixels;
    _clusterNode->setErrorThreshold(pixels);
}

float
ClusterMeshLayer::getErrorThreshold() const
{
    return options().errorThreshold().get();
}

void
ClusterMeshLayer::setMinInstancePixels(float pixels)
{
    options().minInstancePixels() = pixels;
    _clusterNode->setMinInstancePixels(pixels);
}

float
ClusterMeshLayer::getMinInstancePixels() const
{
    return options().minInstancePixels().get();
}

void
ClusterMeshLayer::setOcclusionCulling(bool value)
{
    options().occlusionCulling() = value;
    _clusterNode->setOcclusionCulling(value);
}

bool
ClusterMeshLayer::getOcclusionCulling() const
{
    return options().occlusionCulling().get();
}

std::shared_ptr<const std::vector<ClusterMeshLayer::Asset>>
ClusterMeshLayer::assets() const
{
    std::lock_guard<std::mutex> lock(_assetsMutex);
    if (_assets)
        return _assets;

    auto loaded = std::make_shared<std::vector<Asset>>();
    _assets = loaded;
    if (!_textures.valid())
        return _assets;

    osg::ref_ptr<osgDB::Options> readOptions = Registry::cloneOrCreateOptions(getReadOptions());
    const auto start = std::chrono::steady_clock::now();
    std::size_t meshes = 0u;
    std::uint64_t triangles = 0u;

    for (const auto& model : options().models())
    {
        const std::string file = model.url->full();
        osg::ref_ptr<osg::Node> source = osgDB::readRefNodeFile(file, readOptions.get());
        if (!source.valid())
        {
            OE_WARN << LC << "Could not read \"" << file << "\"" << std::endl;
            continue;
        }

        Asset asset;
        asset.model = model;

        // Convert one variant at a time by hiding its siblings from the converter, which
        // keeps the transforms and state above them in effect.
        const std::vector<osg::Node*> variants = findVariants(source.get());
        for (std::size_t v = 0; v < variants.size(); ++v)
        {
            for (std::size_t other = 0; other < variants.size(); ++other)
                variants[other]->setNodeMask(other == v ? ~0u : 0u);

            std::string cache;
            if (options().clusterCache().get() && !model.url->isRemote())
                cache = file + (variants.size() > 1u ? "." + std::to_string(v) : std::string()) + ".oecm";

            ClusterMesh::Ptr mesh = ClusterMesh::create(source.get(), _chonkFactory, cache);
            if (mesh)
            {
                asset.variants.push_back(mesh);
                triangles += mesh->numSourceTriangles();
                ++meshes;
            }
            else
            {
                OE_WARN << LC << "No usable geometry in \"" << file << "\" object " << v << std::endl;
            }
        }
        if (!asset.variants.empty())
            loaded->push_back(asset);
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    OE_INFO << LC << "Loaded " << loaded->size() << " models as " << meshes << " meshes, " << triangles
        << " source triangles, in " << seconds << " s" << std::endl;
    return _assets;
}

osg::ref_ptr<osg::Node>
ClusterMeshLayer::createTileImplementation(const TileKey& key, ProgressCallback* progress) const
{
    if (!_clusterNode.valid() || key.getLOD() < getLevel())
        return {};
    if (progress && progress->isCanceled())
        return {};

    osg::ref_ptr<const Map> map = getMap();
    if (!map.valid())
        return {};

    const auto assets = this->assets();
    if (assets->empty())
        return {};

    const GeoExtent& extent = key.getExtent();
    const SpatialReference* tileSRS = extent.getSRS();
    const SpatialReference* mapSRS = map->getSRS();
    const bool sameSRS = tileSRS->isHorizEquivalentTo(mapSRS);
    const double hectares = extent.width(Units::METERS) * extent.height(Units::METERS) * 1.0e-4;
    const double layerDensity = options().density().get();

    std::vector<Placement> placements;
    for (std::size_t a = 0; a < assets->size(); ++a)
    {
        const Asset& asset = (*assets)[a];
        const Model& model = asset.model;
        if (getLevel() + model.levelOffset.get() != key.getLOD())
            continue;

        // The tile's placements depend on nothing but its key, the seed, and the options.
        std::seed_seq sequence{ options().seed().get(), key.getLOD(), key.getTileX(), key.getTileY(), unsigned(a) };
        std::mt19937 random(sequence);
        std::uniform_real_distribution<double> unit(0.0, 1.0);

        // Round the expected count up or down at random, or sparse models would never appear.
        const double expected = std::max(0.0, double(model.density.get()) * layerDensity * hectares);
        unsigned count = unsigned(std::min(expected, double(MAX_PLACEMENTS_PER_TILE)));
        if (unit(random) < expected - std::floor(expected))
            ++count;

        // Noise in world coordinates is continuous across tile edges, so patches are too.
        const double clumping = std::max(0.0, std::min(1.0, double(model.clumping.get())));
        SimplexNoise noise;
        noise.setFrequency(1.0 / std::max(1.0, double(model.patchSize.get())));
        noise.setOctaves(2u);
        noise.setNormalize(true);
        noise.setRange(0.0, 1.0);
        // Shift each model's field so that the models do not all pile into the same patches.
        const double shift = 7919.0 * double(a);

        const double minScale = std::max(0.01, double(model.minScale.get()));
        const double maxScale = std::max(minScale, double(model.maxScale.get()));
        const double maxTilt = osg::DegreesToRadians(std::max(0.0, std::min(90.0, double(model.tilt.get()))));

        for (unsigned i = 0; i < count; ++i)
        {
            if ((i & 1023u) == 0u && progress && progress->isCanceled())
                return {};

            GeoPoint point(tileSRS, extent.xMin() + unit(random) * extent.width(),
                extent.yMin() + unit(random) * extent.height(), 0.0, ALTMODE_ABSOLUTE);
            if (!sameSRS && !point.transformInPlace(mapSRS))
                continue;

            Placement p;
            p.point = point.vec3d();
            p.mesh = asset.variants[std::min(asset.variants.size() - 1u,
                std::size_t(unit(random) * double(asset.variants.size())))];
            p.scale = float(minScale + unit(random) * (maxScale - minScale));
            p.sink = model.sink.get();
            p.yaw = unit(random) * 2.0 * osg::PI;
            p.tilt = unit(random) * maxTilt;
            p.tiltAxis = unit(random) * 2.0 * osg::PI;
            const double keep = unit(random);

            if (clumping > 0.0)
            {
                osg::Vec3d world;
                point.toWorld(world);
                const double threshold = 0.6 * clumping;
                const double value = noise.getValue(world.x() + shift, world.y(), world.z());
                if (keep > smoothstep(threshold - 0.08, threshold + 0.08, value))
                    continue;
            }
            placements.push_back(p);
        }
    }
    if (placements.empty())
        return {};

    // Stand every placement on the terrain. Without elevation data they stay on the ellipsoid.
    ElevationLayerVector elevationLayers;
    map->getOpenLayers(elevationLayers);
    if (!elevationLayers.empty())
    {
        std::vector<osg::Vec3d> points;
        points.reserve(placements.size());
        for (const auto& p : placements)
            points.push_back(p.point);

        ElevationPool::WorkingSet workingSet;
        map->getElevationPool()->sampleMapCoords(points.begin(), points.end(), Distance(1.0, Units::METERS),
            &workingSet, progress, 0.0f);
        if (progress && progress->isCanceled())
            return {};

        for (std::size_t i = 0; i < placements.size(); ++i)
            placements[i].point.z() = points[i].z();
    }

    // The batch's frame is the tangent plane at the middle of the tile. A tile is small
    // enough that its "up" serves as the vertical of every placement in it.
    double height = 0.0;
    for (const auto& p : placements)
        height += p.point.z();
    GeoPoint anchor = extent.getCentroid();
    if (!sameSRS && !anchor.transformInPlace(mapSRS))
        return {};
    anchor.z() = height / double(placements.size());
    anchor.altitudeMode() = ALTMODE_ABSOLUTE;
    osg::Matrixd localToWorld, worldToLocal;
    if (!anchor.createLocalToWorld(localToWorld) || !worldToLocal.invert(localToWorld))
        return {};

    std::vector<ClusterMeshNode::Instance> instances;
    instances.reserve(placements.size());
    for (const auto& p : placements)
    {
        osg::Vec3d world;
        if (!GeoPoint(mapSRS, p.point, ALTMODE_ABSOLUTE).toWorld(world))
            continue;
        const osg::Vec3d local = world * worldToLocal;

        // Rest the lowest point of the mesh on the ground, less the part that is buried.
        const auto& box = p.mesh->boundingBox();
        const double ground = double(box.zMin()) + double(p.sink) * double(box.zMax() - box.zMin());
        const osg::Vec3d axis(std::cos(p.tiltAxis), std::sin(p.tiltAxis), 0.0);

        ClusterMeshNode::Instance instance;
        instance.mesh = p.mesh;
        instance.transform = osg::Matrixf(
            osg::Matrixd::translate(0.0, 0.0, -ground) *
            osg::Matrixd::scale(p.scale, p.scale, p.scale) *
            osg::Matrixd::rotate(p.tilt, axis) *
            osg::Matrixd::rotate(p.yaw, osg::Vec3d(0, 0, 1)) *
            osg::Matrixd::translate(local));
        instances.push_back(instance);
    }

    return _clusterNode->createBatch(localToWorld, instances);
}

}
