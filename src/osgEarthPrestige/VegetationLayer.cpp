/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarthPrestige/VegetationLayer>
#include <osg/Multisample>
#include "OverlayPager.h"
#include "PlaceholderAssets.h"
#include "Grass.h"
#include "AssetLighting.h"
#include <osgEarthPrestige/Canopy>
#include "CanopyTransition.h"
#include "TreeCards.h"
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
#include <array>

using namespace osgEarth;
using namespace osgEarthPrestige;
using osgEarth::Util::SimplePager;

REGISTER_OSGEARTH_LAYER_FACTORY("prestige:vegetation", VegetationLayer);

namespace
{
    //! Validates population policy and the placeholder catalog before any scene mutation.
    Status validateGroup(const ScatterGroup& group, const std::vector<ScatterAsset>& assets)
    {
        OE_RETURN_STATUS_ON_ERROR(group.validate());
        if (group.models.empty() && !group.proceduralGrass && !assets.empty())
            return Status(Status::ConfigurationError, "Select at least one model from the configured asset catalog");
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
        std::vector<ScatterAsset> catalog;
        std::shared_ptr<const AggregateCoverageStrategy> aggregateStrategy;
        //! Shared across per-page source snapshots; dimensions do not pin asset meshes or textures.
        struct ArtDescriptions
        {
            std::mutex mutex;
            std::array<std::vector<AggregateArt>,2> tiers;
        };
        std::shared_ptr<ArtDescriptions> artDescriptions = std::make_shared<ArtDescriptions>();

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
                    OE_WARN << "[Prestige vegetation] " << status.toString() << std::endl;
                return {};
            }
            return createPlacements(key, group, bin, placements, progress);
        }

        //! Renders accepted placements, shared by detailed pages and isolated source points in aggregate pages.
        osg::ref_ptr<osg::Node> createPlacements(const TileKey& key, const ScatterGroup& group, int bin,
            const std::vector<ScatterPlacement>& placements, ProgressCallback* progress,
            bool aggregate = false, bool stands = false)
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
                    auto model = stands ? assets->acquireStand(group,name,progress) :
                        aggregate ? assets->acquireImpostor(group, name, progress) :
                        assets->acquire(group, name, progress);
                    if (aggregate && !model) return {};
                    if (!model) return {};
                    models.push_back(model);
                }
            }
            if (models.empty()) models.push_back(stands ? assets->acquireStand(group,"",progress) :
                aggregate ? assets->acquireImpostor(group, "", progress) :
                assets->acquire(group, "", progress));
            if (!models.front()) return {};
            GeoPoint anchor = key.getExtent().getCentroid().transform(lockedMap->getSRS());
            osg::Matrixd localToWorld, worldToLocal;
            if (!anchor.createLocalToWorld(localToWorld) || !worldToLocal.invert(localToWorld)) return {};
            osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable(bin);
            const bool far = aggregate && key.getLOD()+2u == group.renderCellLevel;
            drawable->setName(group.name + (aggregate ? (far ? "/canopy-far" : "/canopy-mid") : ""));
            if (aggregate) drawable->setUseGPUCulling(far ? group.canopyFarGPUCulling : group.canopyMidGPUCulling);
            std::vector<TreeCardPlacement> trees;
            drawable->setFadeNearFar(group.maxRange * 0.8f, group.maxRange);
            // Match VegetationLayer's foliage mip compensation; otherwise thin leaves vanish before their impostors.
            drawable->setAlphaCutoff(group.asset == "trees" ? 0.75f : group.asset == "shrubs" ? 0.35f : 0.25f);

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
                const unsigned model = p.modelIndex(static_cast<unsigned>(models.size()));
                if (aggregate) trees.push_back({placement,model});
                else drawable->add(models[model], osg::Matrixf(placement),
                    osg::Vec2f(p.densityRank(), group.proceduralGrass ? float(p.variationSeed()) : 0.0f));
            }
            if (aggregate)
            {
                const unsigned slots = far ? group.canopyFarClusterSize : group.canopyMidClusterSize;
                std::vector<TreeCardCluster> clusters;
                std::vector<osg::Vec4f> records;
                const auto status = buildTreeCardClusters(trees,models,slots,clusters,records,progress);
                if (status.isError()) return {};
                std::vector<Chonk::Ptr> templates(models.size());
                for (const auto& cluster : clusters)
                {
                    auto& model = templates[cluster.model];
                    if (!model) model = assets->acquireTreeCards(group,
                        group.models.empty() ? "" : group.models[cluster.model],slots,progress,stands);
                    if (!model) return {};
                    drawable->add(model,cluster.transform,treeCardInstanceUV(cluster,far));
                }
                drawable->setAuxiliaryData(records);
                drawable->setMemberLOD(4u,3u,group.minPixels,group.lodPixels > 0.0f ? 1u : 0u);
                drawable->setUserValue("oe_p2_clumps",unsigned(clusters.size()));
                drawable->setUserValue("oe_p2_trees",unsigned(trees.size()));
            }
            if (drawable->empty()) return new osg::Group();
            auto transform = new osg::MatrixTransform(localToWorld);
            transform->addChild(drawable);
            // SimplePager's terminal payload is not range-limited, so enforce the group's range here too.
            auto lod = new osg::LOD();
            lod->addChild(transform, 0.0f, group.maxRange + transform->getBound().radius());
            return lod;
        }

        //! Builds a bounded coverage representation without visiting fine placement cells. Art stays at authored scale.
        osg::ref_ptr<osg::Node> createCoverage(const TileKey& key, const ScatterGroup& group, int bin,
            ProgressCallback* progress, float retention)
        {
            const bool far = key.getLOD()+2u == group.renderCellLevel;
            std::vector<AggregateArt> descriptions;
            std::vector<Chonk::Ptr> retained; // Keep loaded art alive through placement and template creation.
            {
                // This Runtime belongs to one immutable population policy. Cache dimensions after the first
                // successful load, so far-only pages need not repeatedly reload individual art to measure it.
                std::lock_guard<std::mutex> lock(artDescriptions->mutex);
                auto& cached = artDescriptions->tiers[far ? 1u : 0u];
                if (cached.empty())
                {
                    const auto names = group.models.empty() ? std::vector<std::string>{""} : group.models;
                    for (const auto& name : names)
                    {
                        auto model = far ? assets->acquireStand(group,name,progress) :
                            assets->acquireImpostor(group,name,progress);
                        if (!model) return {};
                        AggregateArt art;
                        const auto& box = model->_box;
                        const double x = std::max(std::abs(box.xMin()),std::abs(box.xMax()));
                        const double y = std::max(std::abs(box.yMin()),std::abs(box.yMax()));
                        art.radius = std::max(0.1,std::sqrt(x*x+y*y));
                        const auto entry = std::find_if(catalog.begin(),catalog.end(),[&name](const ScatterAsset& asset)
                            { return asset.name == name; });
                        if (far && entry != catalog.end() && !entry->canopyModel.empty())
                        {
                            // A tightly packed bake covers less ground than the same number of scattered trees.
                            // Weight density by physical footprint instead of the number of trees packed into the bake.
                            auto tree = assets->acquireImpostor(group,name,progress);
                            if (!tree) return {};
                            const auto& single = tree->_box;
                            art.trees = standCoverageWeight((single.xMax()-single.xMin())*(single.yMax()-single.yMin()),
                                (box.xMax()-box.xMin())*(box.yMax()-box.yMin()),entry->canopyTrees);
                            retained.push_back(tree);
                        }
                        descriptions.push_back(art); retained.push_back(model);
                    }
                    cached = descriptions;
                }
                else descriptions = cached;
            }
            double halo = 0.0;
            if (far) for (const auto& art : descriptions) halo = std::max(halo,art.radius*group.maxScale);
            std::shared_ptr<const PlacementField> field;
            auto status = source->queryFieldBuffered(key,group,seed,halo,field,progress);
            if (status.isOK() && field && !field->regions().empty())
            {
                // Preserve specialized land-use layouts until a strategy explicitly approximates their coverage.
                auto exact = group; exact.canopyMidStrategy = exact.canopyFarStrategy = "exact";
                return createCanopy(key,exact,bin,progress);
            }
            AggregateCoverageResult proxies;
            auto request = group; request.placementRetention = retention;
            if (status.isOK() && field)
                status = aggregateStrategy->generate(key,request,seed,*field,descriptions,far,proxies,progress);
            if (status.isOK()) status = retainAggregatePlacements(retention,proxies.trees,progress);
            if (status.isOK()) status = retainAggregatePlacements(retention,proxies.stands,progress);
            if (status.isError() || !field)
            {
                if (!progress || !progress->isCanceled())
                    OE_WARN << "[Prestige vegetation coverage] " << status.toString() << std::endl;
                return {};
            }
            osg::ref_ptr<osg::Group> result = new osg::Group();
            for (bool stands : {false,true})
            {
                const auto& placements = stands ? proxies.stands : proxies.trees;
                if (placements.empty()) continue;
                auto node = createPlacements(key,group,bin,placements,progress,true,stands);
                if (!node) return {};
                result->addChild(node);
            }
            result->setUserValue("oe_p2_canopy_error",canopyReferenceError(key));
            return result;
        }

        //! Groups the same accepted/clamped trees as detailed pages; shared slot meshes replace stretched canopy art.
        osg::ref_ptr<osg::Node> createCanopy(const TileKey& key, const ScatterGroup& group, int bin,
            ProgressCallback* progress)
        {
            const float retention = key.getLOD()+2u == group.renderCellLevel ?
                group.canopyFarRetention : group.canopyMidRetention;
            if (progress && progress->isCanceled()) return {};
            if (retention == 0.0f)
            {
                osg::ref_ptr<osg::Group> empty = new osg::Group();
                empty->setUserValue("oe_p2_canopy_error",canopyReferenceError(key));
                return empty; // a valid empty tier, not a failed page that would keep its parent forever
            }
            const auto& strategy = key.getLOD()+2u == group.renderCellLevel ?
                group.canopyFarStrategy : group.canopyMidStrategy;
            if (strategy == "coverage") return createCoverage(key,group,bin,progress,retention);
            std::vector<ScatterPlacement> placements;
            auto request = group; request.placementRetention = retention;
            auto status = source->generateBatch(key,request,seed,placements,progress);
            if (status.isOK()) status = retainAggregatePlacements(retention,placements,progress);
            if (status.isError())
            {
                if (!progress || !progress->isCanceled()) OE_WARN << "[Prestige vegetation] " << status.toString() << std::endl;
                return {};
            }
            auto result = createPlacements(key,group,bin,placements,progress,true);
            // Group size changes only culling granularity. Keep the nominal handover independent of that control.
            if (result) result->setUserValue("oe_p2_canopy_error",canopyReferenceError(key));
            return result;
        }
    };
}

void VegetationLayer::Options::fromConfig(const Config& conf)
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
    else if (assets().empty() && groups().empty())
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

Config VegetationLayer::Options::getConfig() const
{
    Config conf = super::getConfig();
    conf.set("profile", profile());
    conf.set("seed", seed());
    conf.set("render_bin_number", renderBinNumber());
    conf.set("elevation_resolution", elevationResolution());
    conf.set("asset_budget_mb", assetBudgetMB());
    conf.remove("demo"); // Retired option; placeholder selection follows the asset catalog.
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

void VegetationLayer::init()
{
    super::init();
    _root = new osg::Group();
    _root->setName("Prestige vegetation");
    _root->setStateSet(getOrCreateStateSet());
    getOrCreateStateSet()->addUniform(new osg::Uniform("oe_p2_cluster_debug",osg::Vec3f(0,1,1)));
    _strategies = defaultPlacementStrategies();
    _aggregateStrategy = std::make_shared<SampledAggregateCoverage>();
    _source = std::make_shared<UniformScatterSource>();
}

VegetationLayer::~VegetationLayer()
{
    clear();
}

bool VegetationLayer::setSource(std::shared_ptr<const ScatterSource> source)
{
    if (isOpen() || !source) return false;
    if (!options().sources().empty()) return false;
    _customSource = true;
    _source = std::move(source);
    return true;
}

bool VegetationLayer::registerPlacementStrategy(const std::string& name,
    std::shared_ptr<const RegionPlacementStrategy> strategy)
{
    if (isOpen() || name.empty() || name == "scatter" || !strategy) return false;
    _strategies[name] = std::move(strategy);
    return true;
}

bool VegetationLayer::setAggregateCoverageStrategy(std::shared_ptr<const AggregateCoverageStrategy> strategy)
{
    if (isOpen() || !strategy) return false;
    _aggregateStrategy = std::move(strategy);
    return true;
}

AggregateResidency VegetationLayer::getAggregateResidency(const std::string& population) const
{
    AggregateResidency result;
    for (const auto& pager : _pagers)
    {
        if (!pager || pager->getName() != population) continue;
        std::set<const ChonkDrawable*> visited;
        forEachNodeOfType<ChonkDrawable>(pager.get(),[&](ChonkDrawable* drawable)
        {
            if (!visited.insert(drawable).second) return;
            unsigned clumps = 0u, trees = 0u;
            if (!drawable->getUserValue("oe_p2_clumps",clumps)) return;
            drawable->getUserValue("oe_p2_trees",trees);
            if (drawable->getName() == population+"/canopy-far")
            {
                result.farClumps += clumps; result.farPieces += trees;
            }
            else if (drawable->getName() == population+"/canopy-mid")
            {
                result.midClumps += clumps; result.midPieces += trees;
            }
        });
    }
    return result;
}

osg::Node* VegetationLayer::getNode() const
{
    return _root.get();
}

Status VegetationLayer::openImplementation()
{
    OE_RETURN_STATUS_ON_ERROR(super::openImplementation());
    if (!options().profile().isSet())
        return Status(Status::ConfigurationError, "Prestige vegetation requires an explicit coverage profile");
    _profile = Profile::create(options().profile().get());
    if (!_profile.valid() || !_profile->isOK())
        return Status(Status::ConfigurationError, "Invalid Prestige vegetation coverage profile");
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
        if (input.name.empty() || !inputNames.insert(input.name).second || (bool(input.provider) == input.features.isSet()))
            return Status(Status::ConfigurationError, "Feature inputs require unique names and exactly one FeatureSource or provider");
        if (input.provider) { OE_RETURN_STATUS_ON_ERROR(input.provider->validate()); }
        else { OE_RETURN_STATUS_ON_ERROR(input.features.open(getReadOptions())); }
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
        if (asset.canopyTrees < 1u || asset.canopyTrees > 256u)
            return Status(Status::ConfigurationError,"Canopy assets must represent 1..256 trees");
        if (asset.name.empty() || asset.nearModel.empty() || asset.coarseModel.empty() ||
            !assetNames.insert(asset.name).second)
            return Status(Status::ConfigurationError, "Catalog assets require unique names, near and coarse URIs");
    }
    std::set<std::string> names;
    for (const auto& group : options().groups())
    {
        OE_RETURN_STATUS_ON_ERROR(validateGroup(group, options().assets()));
        if (group.usesCoverage() && options().sources().empty() && !_source->supportsCoverage())
            return Status(Status::ConfigurationError,"Coverage aggregation requires a geographic coverage source");

        if (!names.insert(group.name).second)
            return Status(Status::ConfigurationError, "Duplicate group name: " + group.name);
    }
    if (options().assets().empty() && std::any_of(options().groups().begin(), options().groups().end(),
        [](const ScatterGroup& group) { return group.enabled && !group.proceduralGrass; }))
        OE_NOTICE << "[Prestige vegetation] No asset catalog configured; using built-in placeholder vegetation." << std::endl;
    if (!_customSource && options().sources().empty() && !options().groups().empty())
        OE_NOTICE << "[Prestige vegetation] No geographic sources configured; using synthetic scatter over the layer profile."
            << std::endl;
    return Status::NoError;
}

void VegetationLayer::setClusterDebug(ClusterDebugMode mode, unsigned tiers)
{
    unsigned value = static_cast<unsigned>(mode);
    if (value > 2u) value = 0u;
    tiers &= 3u;
    getOrCreateStateSet()->getUniform("oe_p2_cluster_debug")->set(
        osg::Vec3f(float(value),(tiers & 1u) ? 1.0f : 0.0f,(tiers & 2u) ? 1.0f : 0.0f));
    _clusterDebug.store(value | (tiers << 2u));
}

ClusterDebugMode VegetationLayer::getClusterDebugMode() const
{
    return static_cast<ClusterDebugMode>(_clusterDebug.load() & 3u);
}

unsigned VegetationLayer::getClusterDebugTiers() const
{
    return _clusterDebug.load() >> 2u;
}

Status VegetationLayer::setGroup(const ScatterGroup& group)
{
    OE_RETURN_STATUS_ON_ERROR(validateGroup(group, options().assets()));

    if (group.usesCoverage() && options().sources().empty() && !_source->supportsCoverage())
        return Status(Status::ConfigurationError,"Coverage aggregation requires a geographic coverage source");
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

void VegetationLayer::updateSource()
{
    if (!_source || _source->revision() == _sourceRevision) return;
    auto current = _source->snapshot();
    if (!current) current = _source;
    std::vector<GeoExtent> changes;
    current->changesSince(_sourceSnapshot.get(), changes);
    for (unsigned i = 0; i < _pagers.size(); ++i)
    {
        auto* pager = dynamic_cast<OverlayPager*>(_pagers[i].get());
        if (!pager) continue;
        const auto& group = options().groups()[i];
        double buffer = 0.0;
        for (const auto& rule : options().coverage())
            if (rule.group == "*" || rule.group == group.name) buffer = std::max(buffer, rule.buffer);
        // Coverage stands may cross page edges; retain the existing conservative maximum art halo.
        if (group.usesCoverage()) buffer += 1000.0;
        auto regions = changes;
        for (auto& region : regions)
            region.expand(Distance(2.0*buffer+0.1, Units::METERS), Distance(2.0*buffer+0.1, Units::METERS));
        if (!regions.empty()) pager->invalidate(regions);
    }
    _sourceSnapshot = current;
    _sourceRevision = current->revision();
}

osg::ref_ptr<SimplePager> VegetationLayer::createPager(const ScatterGroup& group, unsigned index)
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
    runtime->catalog = options().assets();
    runtime->aggregateStrategy = _aggregateStrategy;
    osg::ref_ptr<SimplePager> pager = new OverlayPager(map, _profile);
    pager->setName(group.name);
    auto state = pager->getOrCreateStateSet();
    state->setAttribute(_assets->textures());
    state->setDefine("OE_CHONK_SSE_ADJUST");
    if (group.asset == "trees") state->setDefine("OE_CHONK_SSE_LOD_ONLY");
    else state->setDefine("OE_CHONK_SSE_PIXEL_CUTOFF");
    osg::ref_ptr<osg::Uniform> quality = new osg::Uniform("oe_chonk_sse_adjust",
        osg::Vec2f(group.qualityOffset,1.0f/25.0f));
    state->addUniform(quality);
    auto transitions = std::make_shared<CanopyTransitionStates>();
    installPopulationFadeShader(state);
    if (group.proceduralGrass) installGrassShader(state, group);
    else if (!group.models.empty())
        installAssetLighting(state);
    state->addUniform(new osg::Uniform("oe_chonk_lod_transition_factor", group.lodTransition));
    if (group.asset != "trees" && group.farDensity < 1.0f)
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
    pager->setCreateNodeFunction([runtime, group, index, quality, transitions]
        (const TileKey& key, ProgressCallback* progress)
        {
            Runtime page = *runtime;
            // Source adapters freeze their input composition when querying a field. Keep the original
            // generator here so application overrides of generateBatch/queryField remain in effect.
            auto node = group.canopy && key.getLOD() < group.renderCellLevel ?
                page.createCanopy(key, group, static_cast<int>(index), progress) :
                page.createBatch(key, group, static_cast<int>(index), progress);
            if (!node && group.canopy && progress) progress->cancel();
            if (node && !group.canopy && group.asset != "trees") installPopulationPageFade(node,quality,transitions);
            return node;
        });
    if (group.canopy) installTreeCardShader(state);
    pager->setConfigurePagedNodeFunction([group, transitions, firstLevel, quality](const TileKey& key, PagedNode2* node)
    {
        // Tree quality selects representations; only the independent maximum range removes distant forest.
        // Other populations can still reject negligible subtrees before requesting their content.
        // The callback is owned by this node and retains no owning node reference.
        node->setRefinementFunction([node,quality,group](osg::NodeVisitor& nv, bool suggested)
        {
            return referenceRefinement(node,nv,suggested) &&
                (group.asset == "trees" ||
                    populationVisibility(populationPixelSize(node->getBound(),nv),populationError(nv,quality)) > 0.0f);
        });
        if (!group.canopy || key.getLOD() < firstLevel) return;
        if (key.getLOD() == firstLevel && group.asset != "trees") installPopulationPageFade(node,quality,transitions);
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

void VegetationLayer::addedToMap(const Map* map)
{
    super::addedToMap(map);
    clear();
    if (!isOpen() || !_profile.valid()) return;
    _map = map;
    if (!options().sources().empty())
    {
        for (auto& input : options().sources())
        {
            if (input.provider) continue;
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
    _sourceSnapshot = _source->snapshot();
    if (!_sourceSnapshot) _sourceSnapshot = _source;
    _sourceRevision = _sourceSnapshot->revision();
    _assets = std::make_shared<AssetCatalog>(options().assets(),
        std::size_t(options().assetBudgetMB().get()) * 1024u * 1024u, getReadOptions());
    _content = new osg::Group();
    auto assets = _assets;
    osg::observer_ptr<VegetationLayer> weakLayer = this;
    _content->addUpdateCallback(new LambdaCallback<>([assets, weakLayer](osg::NodeVisitor& nv)
        {
            osg::ref_ptr<VegetationLayer> layer;
            if (weakLayer.lock(layer)) layer->updateSource();
            assets->textures()->update(nv);
            return true;
        }));
    // Nest separate population bins under the layer to isolate policies and avoid Sky2's top-level bin 5.
    auto state = _content->getOrCreateStateSet();
    state->setRenderBinDetails(options().renderBinNumber().get(), "RenderBin");
    state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    // Chonk checks the active framebuffer's sample count and retains hard cutouts in single-sample passes.
    // This also supports applications that enable MSAA after opening their map.
    // The Chonk-specific flag keeps VisibleLayer's ordinary opacity modulation active.
    state->setDefine("OE_CHONK_ALPHA_TO_COVERAGE");
    // Inherit application multisampling. A local ON enrolls this mode in OSG's state stack, whose fallback is
    // OFF; leaving vegetation would then disable MSAA for sibling layers and subsequent frames.
    state->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB, osg::StateAttribute::ON);
    // VisibleLayer installs ON|OVERRIDE blending on the parent during rendering preparation or opacity edits.
    // Unlike VegetationLayer's layer-level StateSet, this is a child: PROTECTED is required to keep A2C unblended.
    state->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
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

void VegetationLayer::clear()
{
    if (!_root.valid()) return;
    // Explicit ownership includes masked-out groups, which a scene visitor would skip.
    for (auto pager : _pagers) if (pager.valid()) pager->setDone();
    _pagers.clear();
    _root->removeChildren(0, _root->getNumChildren());
    _content = nullptr;
    _assets.reset();
    _sourceSnapshot.reset();
    _map = nullptr;
}

void VegetationLayer::removedFromMap(const Map* map)
{
    clear();
    for (auto& input : options().sources()) if (!input.provider) input.features.removedFromMap(map);
    if (!_customSource) _source = std::make_shared<UniformScatterSource>();
    super::removedFromMap(map);
}

void VegetationLayer::prepareForRendering(TerrainEngine* engine)
{
    super::prepareForRendering(engine);
    const auto& caps = Capabilities::get();
    if (!caps.supportsGLSL(4.6f) || !caps.supportsNVGL())
    {
        clear();
        setStatus(Status(Status::ServiceUnavailable, "Prestige vegetation currently requires NVIDIA GL 4.6 for Chonk"));
    }
}

Status VegetationLayer::closeImplementation()
{
    clear();
    _profile = nullptr;
    return super::closeImplementation();
}

AssetResidency VegetationLayer::getAssetResidency() const
{
    return _assets ? _assets->residency() : AssetResidency();
}
