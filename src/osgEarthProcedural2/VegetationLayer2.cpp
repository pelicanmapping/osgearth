/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarthProcedural2/VegetationLayer2>
#include "PlaceholderAssets.h"
#include "Grass.h"
#include "AssetLighting.h"
#include <osgEarthProcedural2/Canopy>
#include "CanopyTransition.h"
#include <osgEarth/Chonk>
#include <osgEarth/Capabilities>
#include <osgEarth/CameraUtils>
#include <osgEarth/ElevationPool>
#include <osgEarth/ElevationLayer>
#include <osgEarth/SimplePager>
#include <osgEarth/NodeUtils>
#include <osgEarth/VirtualProgram>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <map>
#include <mutex>
#include <set>
#include <cmath>
#include <algorithm>

using namespace osgEarth;
using namespace osgEarth::Procedural2;
using osgEarth::Util::SimplePager;

REGISTER_OSGEARTH_LAYER(Vegetation2, VegetationLayer2);

namespace
{
    //! Validates population policy and the placeholder catalog before any scene mutation.
    Status validateGroup(const ScatterGroup& group, const std::vector<ScatterAsset>& assets)
    {
        OE_RETURN_STATUS_ON_ERROR(group.validate());
        if (group.asset != "trees" && group.asset != "shrubs" && group.asset != "grass" &&
            group.asset != "undergrowth" && group.asset != "rocks")
            return Status(Status::ConfigurationError, "Unknown population type: " + group.asset);
        for (const auto& name : group.models)
        {
            auto found = std::find_if(assets.begin(), assets.end(), [&name](const ScatterAsset& value)
                { return value.name == name; });
            if (found == assets.end()) return Status(Status::ConfigurationError, "Unknown catalog asset: " + name);
        }
        return Status::NoError;
    }

    //! Rejects lossy aggregation of structured patterns and sources without a direct coverage pathway.
    Status validateCanopy(const ScatterGroup& group, const ScatterSource* source,
        bool configuredFeatures, const std::vector<CoverageRule>& rules)
    {
        if (!group.canopy) return Status::NoError;
        if (!configuredFeatures && (!source || !source->supportsCoverage()))
            return Status(Status::ConfigurationError, "Canopy requires a source with geographic coverage");
        for (const auto& rule : rules)
            if ((rule.group == "*" || rule.group == group.name) && rule.strategy != "scatter")
                return Status(Status::ConfigurationError, "Canopy does not yet represent structured region patterns");
        return Status::NoError;
    }

    //! Computes shadow paging from its current primary-view uniforms; no shared camera state or frame lag.
    bool referenceRefinement(PagedNode2* node, osg::NodeVisitor& nv, bool suggested)
    {
        auto cv = Culling::asCullVisitor(nv);
        auto camera = cv ? cv->getCurrentCamera() : nullptr;
        if (!camera || !CameraUtils::isShadowCamera(camera)) return suggested;
        const auto state = camera->getStateSet();
        const auto toPrimary = state ? state->getUniform("oe_shadowToPrimaryMatrix") : nullptr;
        const auto projection = state ? state->getUniform("oe_primaryProjectionMatrix") : nullptr;
        const auto viewport = state ? state->getUniform("oe_primaryViewport") : nullptr;
        const auto scale = state ? state->getUniform("oe_primaryLODScale") : nullptr;
        if (!toPrimary || !projection || !viewport || !scale) return suggested;
        osg::Matrixd view, proj;
        osg::Vec2 dimensions;
        float lodScale = 1.0f;
        if (!toPrimary->get(view) || !projection->get(proj) || !viewport->get(dimensions) || !scale->get(lodScale) ||
            lodScale <= 0.0f) return suggested;
        const auto& bound = node->getBound();
        const osg::Vec3d position = osg::Vec3d(bound.center())*(*cv->getModelViewMatrix())*view;
        if (node->getLODMethod() == LODMethod::CAMERA_DISTANCE)
        {
            const double distance = std::max(0.0, position.length()*lodScale-bound.radius());
            return distance >= node->getMinRange() && distance <= node->getMaxRange();
        }
        const double distance = ProjectionMatrix::isOrtho(proj) ? 1.0 : std::max(1.0,-position.z()-bound.radius());
        const double pixels = std::max(0.5,double(bound.radius()))*std::abs(proj(1,1))*dimensions.y()/distance/lodScale;
        osg::ref_ptr<PagingManager> manager;
        ObjectStorage::get(&nv, manager);
        const double sse = manager ? manager->sse() : 0.0;
        return pixels >= node->getMinPixels()+sse && pixels <= node->getMaxPixels()+sse;
    }

    //! Holds request-safe resources. Pagers/jobs own this snapshot; it never owns the layer or map.
    struct Runtime
    {
        osg::observer_ptr<const Map> map;
        std::shared_ptr<const ScatterSource> source;
        unsigned seed = 1;
        Distance resolution;
        std::shared_ptr<AssetCatalog> assets;

        //! Builds one spatial batch from unchanged source cells; failures never publish partial geometry.
        osg::ref_ptr<osg::Node> createBatch(const TileKey& key, const ScatterGroup& group, int bin,
            ProgressCallback* progress)
        {
            osg::ref_ptr<const Map> lockedMap;
            if (!map.lock(lockedMap)) return {};
            std::vector<ScatterPlacement> placements;
            Status status = source->generateBatch(key, group, seed, placements, progress);
            if (status.isError())
            {
                if (!progress || !progress->isCanceled())
                    OE_WARN << "[Vegetation2] " << status.toString() << std::endl;
                return {};
            }
            return createPlacements(key, group, bin, placements, progress);
        }

        //! Renders accepted placements, shared by detailed pages and isolated source points in aggregate pages.
        osg::ref_ptr<osg::Node> createPlacements(const TileKey& key, const ScatterGroup& group, int bin,
            const std::vector<ScatterPlacement>& placements, ProgressCallback* progress)
        {
            osg::ref_ptr<const Map> lockedMap;
            if (!map.lock(lockedMap)) return {};
            if (placements.empty()) return new osg::Group();
            std::vector<osg::Vec3d> points;
            points.reserve(placements.size());
            for (const auto& p : placements)
            {
                if (!std::isfinite(p.point.x()) || !std::isfinite(p.point.y()) || !std::isfinite(p.point.z()) ||
                    !std::isfinite(p.scale) || p.scale <= 0.0f || !std::isfinite(p.rotation)) return {};
                GeoPoint point(key.getProfile()->getSRS(), p.point);
                GeoPoint mapped;
                if (!point.transform(lockedMap->getSRS(), mapped)) return {};
                points.push_back(mapped.vec3d());
            }

            ElevationLayerVector elevationLayers;
            lockedMap->getOpenLayers(elevationLayers);
            const bool sampleSlope = group.proceduralGrass && !elevationLayers.empty();
            if (sampleSlope)
            {
                points.reserve(3u * placements.size());
                for (std::size_t i = 0; i < placements.size(); ++i)
                {
                    if ((i & 255u) == 0u && progress && progress->isCanceled()) return {};
                    osg::Matrixd frame;
                    if (!GeoPoint(lockedMap->getSRS(), points[i]).createLocalToWorld(frame)) return {};
                    const double span = std::max(0.25, double(group.grassRadius * placements[i].scale));
                    for (const auto& offset : {osg::Vec3d(span,0,0), osg::Vec3d(0,span,0)})
                    {
                        GeoPoint probe;
                        if (!probe.fromWorld(lockedMap->getSRS(), offset * frame)) return {};
                        points.push_back(probe.vec3d());
                    }
                }
            }
            if (!elevationLayers.empty())
            {
                ElevationPool::WorkingSet workingSet;
                lockedMap->getElevationPool()->sampleMapCoords(points.begin(), points.end(), resolution,
                    &workingSet, progress, NO_DATA_VALUE);
            }
            if (progress && progress->isCanceled()) return {};

            std::vector<Chonk::Ptr> models;
            if (!group.proceduralGrass)
            {
                for (const auto& name : group.models)
                {
                    auto model = assets->acquire(group, name, progress);
                    if (!model && (!progress || !progress->isCanceled())) model = assets->acquire(group, "", progress);
                    if (!model) return {};
                    models.push_back(model);
                }
            }
            if (models.empty()) models.push_back(assets->acquire(group, "", progress));
            if (!models.front()) return {};
            GeoPoint anchor = key.getExtent().getCentroid().transform(lockedMap->getSRS());
            osg::Matrixd localToWorld, worldToLocal;
            if (!anchor.createLocalToWorld(localToWorld) || !worldToLocal.invert(localToWorld)) return {};
            osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable(bin);
            drawable->setName(group.name);
            drawable->setFadeNearFar(group.maxRange * 0.8f, group.maxRange);

            for (std::size_t i = 0; i < placements.size(); ++i)
            {
                if ((i & 255u) == 0u && progress && progress->isCanceled()) return {};
                if (points[i].z() == NO_DATA_VALUE || !std::isfinite(points[i].z()))
                {
                    if (group.canopy) return {}; // keep the aggregate parent until all detail heights are available
                    continue;
                }
                osg::Matrixd pointToWorld;
                if (!GeoPoint(lockedMap->getSRS(), points[i]).createLocalToWorld(pointToWorld))
                {
                    if (group.canopy) return {};
                    continue;
                }
                osg::Matrixd slope;
                if (sampleSlope)
                {
                    const auto& east = points[placements.size() + 2u*i];
                    const auto& north = points[placements.size() + 2u*i + 1u];
                    if (east.z() == NO_DATA_VALUE || north.z() == NO_DATA_VALUE ||
                        !std::isfinite(east.z()) || !std::isfinite(north.z())) continue;
                    osg::Vec3d eastWorld, northWorld;
                    osg::Matrixd worldToPoint;
                    if (!worldToPoint.invert(pointToWorld) ||
                        !GeoPoint(lockedMap->getSRS(), east).toWorld(eastWorld) ||
                        !GeoPoint(lockedMap->getSRS(), north).toWorld(northWorld)) continue;
                    const osg::Vec3d normal = (eastWorld * worldToPoint) ^ (northWorld * worldToPoint);
                    if (!std::isfinite(normal.length2()) || std::abs(normal.z()) < 1e-8) continue;
                    const double dx = -normal.x() / normal.z(), dy = -normal.y() / normal.z();
                    // A local plane cannot represent cliffs. Skip these patches instead of stretching blades arbitrarily.
                    if (std::abs(dx) > 4.0 || std::abs(dy) > 4.0) continue;
                    slope(0,2) = dx;
                    slope(1,2) = dy;
                }
                const auto& p = placements[i];
                const osg::Matrixd placement = osg::Matrixd::scale(p.scale, p.scale, p.scale) *
                    osg::Matrixd::rotate(p.rotation, osg::Vec3d(0,0,1)) * slope * pointToWorld * worldToLocal;
                drawable->add(models[p.modelIndex(static_cast<unsigned>(models.size()))], osg::Matrixf(placement),
                    osg::Vec2f(p.densityRank(), group.proceduralGrass ? float(p.variationSeed()) : 0.0f));
            }
            if (drawable->empty()) return new osg::Group();
            auto transform = new osg::MatrixTransform(localToWorld);
            transform->addChild(drawable);
            // SimplePager's terminal payload is not range-limited, so enforce the group's range here too.
            auto lod = new osg::LOD();
            lod->addChild(transform, 0.0f, group.maxRange + transform->getBound().radius());
            return lod;
        }

        //! Builds one bounded aggregate page from coverage and elevations, without generating the fine forest.
        osg::ref_ptr<osg::Node> createCanopy(const TileKey& key, const ScatterGroup& group, int bin,
            ProgressCallback* progress)
        {
            osg::ref_ptr<const Map> lockedMap;
            if (!map.lock(lockedMap)) return {};
            std::shared_ptr<const PlacementField> field;
            auto status = source->queryField(key, group, seed, field, progress);
            if (status.isError())
            {
                if (!progress || !progress->isCanceled()) OE_WARN << "[Vegetation2] " << status.toString() << std::endl;
                return {};
            }
            ElevationLayerVector layers;
            lockedMap->getOpenLayers(layers);
            ElevationPool::WorkingSet workingSet;
            CanopyElevation sample = [&](std::vector<osg::Vec3d>& points) -> Status
            {
                if (layers.empty()) return Status::NoError;
                std::vector<osg::Vec3d> mapped;
                for (const auto& p : points)
                {
                    GeoPoint value;
                    if (!GeoPoint(key.getExtent().getSRS(), p).transform(lockedMap->getSRS(), value))
                        return Status(Status::ResourceUnavailable, "Cannot transform canopy elevation probe");
                    mapped.push_back(value.vec3d());
                }
                lockedMap->getElevationPool()->sampleMapCoords(mapped.begin(), mapped.end(), resolution,
                    &workingSet, progress, NO_DATA_VALUE);
                for (std::size_t i=0; i<mapped.size(); ++i)
                {
                    GeoPoint value;
                    if (mapped[i].z() == NO_DATA_VALUE ||
                        !GeoPoint(lockedMap->getSRS(), mapped[i]).transform(key.getExtent().getSRS(), value))
                        return Status(Status::ResourceUnavailable, "Canopy elevation unavailable");
                    points[i] = value.vec3d();
                }
                return Status::NoError;
            };
            std::vector<CanopyPatch> patches;
            status = buildCanopy(key, group, *field, lockedMap->getSRS(), sample, patches, progress);
            if (status.isError())
            {
                if (!progress || !progress->isCanceled()) OE_WARN << "[Vegetation2] " << status.toString() << std::endl;
                return {};
            }
            osg::ref_ptr<osg::Group> result = new osg::Group();
            double error = 1.0;
            if (!patches.empty())
            {
                osg::Matrixd frame, inverse;
                if (!key.getExtent().getCentroid().transform(lockedMap->getSRS()).createLocalToWorld(frame) ||
                    !inverse.invert(frame)) return {};
                osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable(bin);
                const bool far = key.getLOD()+2 == group.renderCellLevel;
                drawable->setName(group.name + (far ? "/canopy-far" : "/canopy-mid"));
                // The unculled Chonk path retains instancing and fragment coverage, without compute dispatches.
                drawable->setUseGPUCulling(far ? group.canopyFarGPUCulling : group.canopyMidGPUCulling);
                drawable->setFadeNearFar(group.maxRange*0.8f, group.maxRange);
                // Source clumps average below the alpha-test threshold in small mips; retain distant crown coverage.
                // This uses Chonk's existing mip compensation; authored coverage-preserving mips remain follow-up work.
                drawable->setAlphaCutoff(0.15f);
                std::map<unsigned, Chonk::Ptr> models;
                std::set<std::pair<double,double>> clumps;
                for (const auto& patch : patches)
                {
                    clumps.emplace(patch.artFootprint.xMin(),patch.artFootprint.yMin());
                    auto& model = models[patch.coverage*4u + patch.layout];
                    if (!model)
                    {
                        model = assets->acquireCanopy(group,patch.coverage,patch.layout,progress);
                        if (!model) return {};
                    }
                    drawable->add(model, osg::Matrixf(patch.localToWorld*inverse),
                        osg::Vec2f(-float(patch.mask),float(patch.clip)));
                    error = std::max(error, patch.error);
                }
                drawable->setUserValue("oe_p2_clumps",unsigned(clumps.size()));
                auto transform = new osg::MatrixTransform(frame);
                transform->addChild(drawable);
                auto lod = new osg::LOD();
                lod->addChild(transform, 0.0f, group.maxRange+transform->getBound().radius());
                result->addChild(lod);
            }
            // Mapped lone trees keep their real assets/identities; an aggregate never substitutes a grove for a point.
            std::vector<ScatterPlacement> points;
            for (const auto& point : field->points())
                if (!field->sample(point.point).excluded) points.push_back(point);
            if (points.size() > group.maxPerBatch) return {};
            if (!points.empty())
            {
                ScatterGroup explicitGroup = group;
                explicitGroup.name += "/mapped-points";
                auto explicitTrees = createPlacements(key, explicitGroup, bin, points, progress);
                if (!explicitTrees) return {};
                result->addChild(explicitTrees);
            }
            // Empty pages still need refinement: small forests may have been removed by conservative boundary tests.
            const auto& e = key.getExtent();
            const double span = e.getSRS()->isProjected() ?
                Units::convert(e.getSRS()->getUnits(), Units::METERS, e.height()) : e.height(Units::METERS);
            const unsigned levels = canopySubdivisionLevels(group,key.getLOD()+2u == group.renderCellLevel);
            result->setUserValue("oe_p2_canopy_error", std::max(error, span/(3.0*(1u<<levels))));
            return result;
        }
    };
}

void VegetationLayer2::Options::fromConfig(const Config& conf)
{
    conf.get("profile", profile());
    conf.get("seed", seed());
    conf.get("render_bin_number", renderBinNumber());
    conf.get("elevation_resolution", elevationResolution());
    conf.get("asset_budget_mb", assetBudgetMB());
    sources().clear();
    for (const auto& c : conf.child("sources").children("source")) sources().emplace_back(c);
    coverage().clear();
    for (const auto& c : conf.child("coverage").children("rule")) coverage().emplace_back(c);
    assets().clear();
    for (const auto& c : conf.child("assets").children("asset")) assets().emplace_back(c);
    if (conf.hasChild("groups"))
    {
        groups().clear();
        for (const auto& c : conf.child("groups").children("group"))
            groups().emplace_back(c);
    }
    else if (groups().empty())
    {
        const char* names[] = {"trees", "shrubs", "grass", "undergrowth", "rocks"};
        const double densities[] = {1200, 4000, 180000, 18000, 800};
        const float ranges[] = {3000, 700, 160, 260, 1000};
        const unsigned sourceLevels[] = {16, 18, 20, 19, 17};
        const unsigned renderLevels[] = {14, 16, 17, 17, 15};
        const float lodPixels[] = {32, 18, 8, 12, 20};
        for (unsigned i = 0; i < 5; ++i)
        {
            ScatterGroup group;
            group.name = group.asset = names[i];
            group.density = densities[i];
            group.maxRange = ranges[i];
            group.cellLevel = sourceLevels[i];
            group.renderCellLevel = renderLevels[i];
            group.lodPixels = lodPixels[i];
            groups().push_back(group);
        }
    }
}

Config VegetationLayer2::Options::getConfig() const
{
    Config conf = super::getConfig();
    conf.set("profile", profile());
    conf.set("seed", seed());
    conf.set("render_bin_number", renderBinNumber());
    conf.set("elevation_resolution", elevationResolution());
    conf.set("asset_budget_mb", assetBudgetMB());
    conf.remove("sources");
    Config inputs("sources");
    for (const auto& input : sources()) inputs.add(input.getConfig());
    conf.add(inputs);
    conf.remove("coverage");
    Config rules("coverage");
    for (const auto& rule : coverage()) rules.add(rule.getConfig());
    conf.add(rules);
    conf.remove("assets");
    Config catalog("assets");
    for (const auto& asset : assets()) catalog.add(asset.getConfig());
    conf.add(catalog);
    // Replace the original serialized collection so live edits survive an options round trip.
    conf.remove("groups");
    Config collection("groups");
    for (const auto& group : groups()) collection.add(group.getConfig());
    conf.add(collection);
    return conf;
}

void VegetationLayer2::init()
{
    super::init();
    _root = new osg::Group();
    _root->setName("Vegetation2");
    _root->setStateSet(getOrCreateStateSet());
    _strategies = defaultPlacementStrategies();
    _source = std::make_shared<UniformScatterSource>();
}

VegetationLayer2::~VegetationLayer2()
{
    clear();
}

bool VegetationLayer2::setSource(std::shared_ptr<const ScatterSource> source)
{
    if (isOpen() || !source) return false;
    if (!options().sources().empty()) return false;
    _customSource = true;
    _source = std::move(source);
    return true;
}

bool VegetationLayer2::registerPlacementStrategy(const std::string& name,
    std::shared_ptr<const RegionPlacementStrategy> strategy)
{
    if (isOpen() || name.empty() || name == "scatter" || !strategy) return false;
    _strategies[name] = std::move(strategy);
    return true;
}

AggregateResidency VegetationLayer2::getAggregateResidency(const std::string& population) const
{
    AggregateResidency result;
    for (const auto& pager : _pagers)
    {
        if (!pager || pager->getName() != population) continue;
        std::set<const ChonkDrawable*> visited;
        forEachNodeOfType<ChonkDrawable>(pager.get(),[&](ChonkDrawable* drawable)
        {
            if (!visited.insert(drawable).second) return;
            unsigned clumps = 0u;
            if (!drawable->getUserValue("oe_p2_clumps",clumps)) return;
            if (drawable->getName() == population+"/canopy-far")
            {
                result.farClumps += clumps; result.farPieces += drawable->getNumInstances();
            }
            else if (drawable->getName() == population+"/canopy-mid")
            {
                result.midClumps += clumps; result.midPieces += drawable->getNumInstances();
            }
        });
    }
    return result;
}

osg::Node* VegetationLayer2::getNode() const
{
    return _root.get();
}

Status VegetationLayer2::openImplementation()
{
    OE_RETURN_STATUS_ON_ERROR(super::openImplementation());
    if (!options().profile().isSet())
        return Status(Status::ConfigurationError, "Vegetation2 requires an explicit coverage profile");
    _profile = Profile::create(options().profile().get());
    if (!_profile.valid() || !_profile->isOK())
        return Status(Status::ConfigurationError, "Invalid Vegetation2 coverage profile");
    if (!std::isfinite(options().elevationResolution()->as(Units::METERS)) ||
        options().elevationResolution()->as(Units::METERS) <= 0.0)
        return Status(Status::ConfigurationError, "elevation_resolution must be positive");
    if (options().assetBudgetMB().get() < 1u || options().assetBudgetMB().get() > 4096u)
        return Status(Status::ConfigurationError, "asset_budget_mb must be between 1 and 4096");
    if (_customSource && !options().sources().empty())
        return Status(Status::ConfigurationError, "Choose a custom scatter source or configured feature inputs");
    if (options().sources().empty() != options().coverage().empty())
        return Status(Status::ConfigurationError, "Feature inputs and coverage rules must be configured together");
    std::set<std::string> inputNames;
    for (auto& input : options().sources())
    {
        if (input.name.empty() || !inputNames.insert(input.name).second || !input.features.isSet())
            return Status(Status::ConfigurationError, "Feature inputs require unique names and a feature-layer reference");
        OE_RETURN_STATUS_ON_ERROR(input.features.open(getReadOptions()));
    }
    std::set<std::string> ruleNames;
    for (const auto& rule : options().coverage())
    {
        OE_RETURN_STATUS_ON_ERROR(rule.validate());
        if (rule.strategy != "scatter")
        {
            if (!ruleNames.insert(rule.name).second)
                return Status(Status::ConfigurationError, "Region rule names must be unique");
            auto strategy = _strategies.find(rule.strategy);
            if (strategy == _strategies.end())
                return Status(Status::ConfigurationError, "Unregistered placement strategy: " + rule.strategy);
            OE_RETURN_STATUS_ON_ERROR(strategy->second->validate(rule.parameters));
        }
        if (rule.source != "*" && inputNames.count(rule.source) == 0u)
            return Status(Status::ConfigurationError, "Unknown coverage source: " + rule.source);
        if (rule.group != "*" && std::none_of(options().groups().begin(), options().groups().end(),
            [&rule](const ScatterGroup& group) { return group.name == rule.group; }))
            return Status(Status::ConfigurationError, "Unknown coverage population: " + rule.group);
    }
    std::set<std::string> assetNames;
    for (const auto& asset : options().assets())
    {
        if (asset.name.empty() || asset.nearModel.empty() || asset.coarseModel.empty() ||
            !assetNames.insert(asset.name).second)
            return Status(Status::ConfigurationError, "Catalog assets require unique names, near and coarse URIs");
    }
    std::set<std::string> names;
    for (const auto& group : options().groups())
    {
        OE_RETURN_STATUS_ON_ERROR(validateGroup(group, options().assets()));
        OE_RETURN_STATUS_ON_ERROR(validateCanopy(group, _source.get(), !options().sources().empty(), options().coverage()));
        if (!names.insert(group.name).second)
            return Status(Status::ConfigurationError, "Duplicate group name: " + group.name);
    }
    return Status::NoError;
}

Status VegetationLayer2::setGroup(const ScatterGroup& group)
{
    OE_RETURN_STATUS_ON_ERROR(validateGroup(group, options().assets()));
    OE_RETURN_STATUS_ON_ERROR(validateCanopy(group, _source.get(), !options().sources().empty(), options().coverage()));
    auto& groups = options().groups();
    auto found = std::find_if(groups.begin(), groups.end(), [&group](const ScatterGroup& value)
        { return value.name == group.name; });
    if (found == groups.end())
        return Status(Status::ConfigurationError, "Unknown population: " + group.name);
    const auto index = static_cast<unsigned>(std::distance(groups.begin(), found));
    if (_content.valid())
    {
        // Quality is a shared update-thread policy, not part of page content or asset identity.
        auto contentPolicy = group;
        contentPolicy.qualityOffset = found->qualityOffset;
        if (contentPolicy.getConfig().toJSON() == found->getConfig().toJSON() &&
            group.qualityOffset != found->qualityOffset)
        {
            if (_pagers[index])
                _pagers[index]->getOrCreateStateSet()->getUniform("oe_chonk_sse_adjust")->set(
                    osg::Vec2f(group.qualityOffset,1.0f/25.0f));
            *found = group;
            return Status::NoError;
        }
        // Construct first; workers keep the old runtime until their canceled requests finish.
        auto replacement = createPager(group, index);
        auto previous = _pagers[index];
        if (previous.valid())
        {
            previous->setDone();
            if (replacement.valid()) _content->replaceChild(previous, replacement);
            else _content->removeChild(previous);
        }
        else if (replacement.valid()) _content->addChild(replacement);
        _pagers[index] = replacement;
    }
    *found = group;
    return Status::NoError;
}

osg::ref_ptr<SimplePager> VegetationLayer2::createPager(const ScatterGroup& group, unsigned index)
{
    if (!group.enabled || group.density == 0.0) return {};
    osg::ref_ptr<const Map> map;
    if (!_map.lock(map)) return {};
    auto runtime = std::make_shared<Runtime>();
    runtime->map = map;
    runtime->source = _source;
    runtime->seed = options().seed().get();
    runtime->resolution = options().elevationResolution().get();
    runtime->assets = _assets;
    osg::ref_ptr<SimplePager> pager = new SimplePager(map, _profile);
    pager->setName(group.name);
    auto state = pager->getOrCreateStateSet();
    state->setAttribute(_assets->textures());
    state->setDefine("OE_CHONK_SSE_ADJUST");
    osg::ref_ptr<osg::Uniform> quality = new osg::Uniform("oe_chonk_sse_adjust",
        osg::Vec2f(group.qualityOffset,1.0f/25.0f));
    state->addUniform(quality);
    auto transitions = std::make_shared<CanopyTransitionStates>();
    installPopulationFadeShader(state);
    if (group.proceduralGrass) installGrassShader(state, group);
    else if (!group.models.empty())
        installAssetLighting(state);
    state->addUniform(new osg::Uniform("oe_chonk_lod_transition_factor", group.lodTransition));
    if (group.farDensity < 1.0f)
    {
        state->setDefine("OE_CHONK_DENSITY_LOD");
        state->addUniform(new osg::Uniform("oe_chonk_density_lod",
            osg::Vec3f(group.densityStart, group.densityEnd, group.farDensity)));
    }
    const unsigned firstLevel = group.canopy ? group.renderCellLevel-(group.canopyFar ? 2u : 1u) : group.renderCellLevel;
    pager->setMinLevel(firstLevel);
    pager->setMaxLevel(group.renderCellLevel);
    pager->setMaxRange(group.maxRange);
    // North-south span avoids using a polar tile's vanishing longitude width on a global profile.
    const auto extent = TileKey(group.renderCellLevel, 0, 0, _profile).getExtent();
    const double cellHeight = extent.getSRS()->isProjected() ?
        Units::convert(extent.getSRS()->getUnits(), Units::METERS, extent.height()) : extent.height(Units::METERS);
    pager->setRangeFactor(float(std::max(8.0, group.maxRange / std::max(1.0, cellHeight * 0.25))));
    pager->setTimeoutSeconds(2.0);
    pager->setCreateNodeFunction([runtime, group, index, quality, transitions](const TileKey& key, ProgressCallback* progress)
        {
            auto node = group.canopy && key.getLOD() < group.renderCellLevel ?
                runtime->createCanopy(key, group, static_cast<int>(index), progress) :
                runtime->createBatch(key, group, static_cast<int>(index), progress);
            if (!node && group.canopy && progress) progress->cancel();
            if (node && !group.canopy) installPopulationPageFade(node,quality,transitions);
            return node;
        });
    if (group.canopy) installCanopyShader(state);
    pager->setConfigurePagedNodeFunction([group, transitions, firstLevel, quality](const TileKey& key, PagedNode2* node)
    {
        // Conservative ancestor bounds can reject negligible subtrees before requesting their content.
        // The callback is owned by this node and retains no owning node reference.
        node->setRefinementFunction([node,quality](osg::NodeVisitor& nv, bool suggested)
        {
            return referenceRefinement(node,nv,suggested) &&
                populationVisibility(populationPixelSize(node->getBound(),nv),populationError(nv,quality)) > 0.0f;
        });
        if (!group.canopy || key.getLOD() < firstLevel) return;
        if (key.getLOD() == firstLevel) installPopulationPageFade(node,quality,transitions);
        double error = 1.0;
        if (node->getNumChildren() > 0) node->getChild(0)->getUserValue("oe_p2_canopy_error", error);
        // Keep load priorities in the same distance units as the coverage-only ancestors. Positive pixel
        // priorities otherwise starve initial coarse coverage behind negative distance-based requests.
        // The refinement callback below still selects representations by projected error.
        node->setLODMethod(LODMethod::CAMERA_DISTANCE);
        configureCanopyTransition(node,std::max(1.0,error),group,transitions,quality);
    });
    pager->build();
    return pager;
}

void VegetationLayer2::addedToMap(const Map* map)
{
    super::addedToMap(map);
    clear();
    if (!isOpen() || !_profile.valid()) return;
    _map = map;
    if (!options().sources().empty())
    {
        for (auto& input : options().sources())
        {
            input.features.addedToMap(map);
            auto* features = input.features.getLayer();
            if (!features || !features->isOpen() || !features->getFeatureProfile())
            {
                setStatus(Status(Status::ResourceUnavailable, "Cannot resolve feature input: " + input.name));
                return;
            }
        }
        _source = std::make_shared<FeatureScatterSource>(
            std::make_shared<MapFeatureProvider>(options().sources()), options().coverage(),
            std::make_shared<MixedPlacementStrategy>(_strategies));
    }
    _assets = std::make_shared<AssetCatalog>(options().assets(),
        std::size_t(options().assetBudgetMB().get()) * 1024u * 1024u, getReadOptions());
    _content = new osg::Group();
    auto assets = _assets;
    _content->addUpdateCallback(new LambdaCallback<>([assets](osg::NodeVisitor& nv)
        {
            assets->textures()->update(nv);
            return true;
        }));
    // Nest separate population bins under the layer to isolate policies and avoid Sky2's top-level bin 5.
    auto state = _content->getOrCreateStateSet();
    state->setRenderBinDetails(options().renderBinNumber().get(), "RenderBin");
    state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    state->setDefine("OE_CHONK_DITHER_FADE");
    // Inherit the map-wide SSE. Each population adds its own offset once in CPU paging and GPU LOD selection.
    const auto& groups = options().groups();
    _pagers.resize(groups.size());
    for (unsigned i = 0; i < groups.size(); ++i)
    {
        _pagers[i] = createPager(groups[i], i);
        if (_pagers[i].valid()) _content->addChild(_pagers[i]);
    }
    _root->addChild(_content);
}

void VegetationLayer2::clear()
{
    if (!_root.valid()) return;
    // Explicit ownership includes masked-out groups, which a scene visitor would skip.
    for (auto pager : _pagers) if (pager.valid()) pager->setDone();
    _pagers.clear();
    _root->removeChildren(0, _root->getNumChildren());
    _content = nullptr;
    _assets.reset();
    _map = nullptr;
}

void VegetationLayer2::removedFromMap(const Map* map)
{
    clear();
    for (auto& input : options().sources()) input.features.removedFromMap(map);
    if (!_customSource) _source = std::make_shared<UniformScatterSource>();
    super::removedFromMap(map);
}

void VegetationLayer2::prepareForRendering(TerrainEngine* engine)
{
    super::prepareForRendering(engine);
    const auto& caps = Capabilities::get();
    if (!caps.supportsGLSL(4.6f) || !caps.supportsNVGL())
    {
        clear();
        setStatus(Status(Status::ServiceUnavailable, "Vegetation2 currently requires NVIDIA GL 4.6 for Chonk"));
    }
}

Status VegetationLayer2::closeImplementation()
{
    clear();
    _profile = nullptr;
    return super::closeImplementation();
}

AssetResidency VegetationLayer2::getAssetResidency() const
{
    return _assets ? _assets->residency() : AssetResidency();
}
