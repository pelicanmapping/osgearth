/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarthPrestige/AggregateCoverage>
#include "PlacementLimits.h"
#include <algorithm>
#include <cmath>

using namespace osgEarth;
using namespace osgEarthPrestige;

namespace
{
    //! Stable independent streams for boundary representatives; no mutable random generator or camera input.
    std::uint64_t hash(std::uint64_t value)
    {
        value += UINT64_C(0x9e3779b97f4a7c15);
        value = (value^(value>>30))*UINT64_C(0xbf58476d1ce4e5b9);
        value = (value^(value>>27))*UINT64_C(0x94d049bb133111eb);
        return value^(value>>31);
    }

    //! Returns a stable half-open uniform variate independent of placement/species selection streams.
    double unit(std::uint64_t value) { return double(hash(value)>>11)*(1.0/9007199254740992.0); }

    //! Converts a meter footprint to request coordinates; geographic expansion uses osgEarth's SRS-aware extent API.
    GeoExtent footprint(const SpatialReference* srs, const osg::Vec3d& point, double radius)
    {
        GeoExtent result(srs,point.x(),point.y(),point.x(),point.y());
        result.expand(Distance(2.0*radius,Units::METERS),Distance(2.0*radius,Units::METERS));
        return result;
    }
}

double osgEarthPrestige::standCoverageWeight(double treeArea, double standArea, unsigned authoredTrees)
{
    if (!std::isfinite(treeArea) || !std::isfinite(standArea) || treeArea <= 0.0 || standArea <= 0.0 ||
        authoredTrees < 1u || authoredTrees > 256u) return 1.0;
    return std::max(1.0,std::min(double(authoredTrees),standArea/treeArea));
}

Status SampledAggregateCoverage::generate(const TileKey& key, const ScatterGroup& group, unsigned seed,
    const PlacementField& field, const std::vector<AggregateArt>& models, bool far,
    AggregateCoverageResult& output, ProgressCallback* progress) const
{
    output = AggregateCoverageResult();
    osg::ref_ptr<PlacementWorkProgress> budget = new PlacementWorkProgress(progress, group.maxWorkPerRequest);
    progress = budget.get();
    OE_RETURN_STATUS_ON_ERROR(group.validate());
    if (!key.valid() || models.empty()) return Status(Status::ConfigurationError,"Missing coverage key or art");
    double minimumTrees = 256.0;
    for (const auto& model : models)
    {
        if (!std::isfinite(model.radius) || model.radius <= 0.0 || model.radius > 500.0 ||
            !std::isfinite(model.trees) || model.trees < 1.0 || model.trees > 256.0 || model.radius*group.maxScale > 1000.0)
            return Status(Status::ConfigurationError,
                "Coverage art needs radius 0..500m, scaled radius <= 1000m, and 1..256 trees");
        minimumTrees = std::min(minimumTrees,far ? model.trees : 1.0);
    }
    if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable,"Coverage canceled");
    if (!group.enabled || group.density <= 0.0) return Status::NoError;
    struct Proxy : ScatterPlacement { bool stand = false; };
    PlacementSelection<Proxy> selected(group.maxPerBatch);
    // Both representations share one accepted cap, after their footprint/point exclusions and tier retention.
    auto accept = [&](const ScatterPlacement& placement, bool stand)
    {
        if (!retainPlacement(placement, group.placementRetention)) return;
        Proxy proxy; static_cast<ScatterPlacement&>(proxy) = placement; proxy.stand = stand;
        selected.add(proxy);
    };
    std::size_t work = 0;
    std::vector<ScatterPlacement> candidates;
    if (field.hasDensity())
    {
        // Work scales with coarse representatives, never the detailed source's cell level or region algorithms.
        ScatterGroup coarse = group;
        coarse.density /= minimumTrees;
        OE_RETURN_STATUS_ON_ERROR(UniformScatterSource().generateCandidates(key,coarse,seed^0x51ed270bu,candidates,progress));
    }
    OE_RETURN_STATUS_ON_ERROR(consumePlacementWork(field.points().size(), progress));
    work = candidates.size() + field.points().size();
    if (work > group.maxCandidatesPerCell)
        return Status(Status::ConfigurationError, "Coverage exceeds candidate work limit");
    const auto& extent = key.getExtent();
    for (const auto& p : candidates)
    {
        if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable,"Coverage canceled");
        const auto& art = models[p.modelIndex(unsigned(models.size()))];
        if (far && unit(p.id^UINT64_C(0x6a09e667)) >= double(minimumTrees)/art.trees) continue;
        if (!far)
        {
            const auto at = field.sample(p.point);
            if (!at.excluded && p.densityRank() < at.density) accept(p, false);
        }
        else
        {
            const auto box = footprint(extent.getSRS(),p.point,art.radius*p.scale);
            if (!box.isValid()) return Status(Status::ConfigurationError,"Invalid stand footprint");
            CoverageSample whole;
            if (field.uniform(box,whole))
            {
                if (!whole.excluded && p.densityRank() < whole.density) accept(p, true);
            }
            else
            {
                // Approximate only the uncertain patch with smaller cards. These are independently sampled roots,
                // not reconstructed detailed placements. The halo field applies the same exclusions across page edges.
                const unsigned count = unsigned(art.trees) +
                    (unit(p.id^UINT64_C(0xf82b5903)) < art.trees-std::floor(art.trees) ? 1u : 0u);
                OE_RETURN_STATUS_ON_ERROR(consumePlacementWork(count, progress));
                work += count;
                if (work > group.maxCandidatesPerCell)
                    return Status(Status::ConfigurationError, "Coverage boundary exceeds candidate work limit");
                for (unsigned i=0; i<count; ++i)
                {
                    auto tree = p;
                    tree.id = hash(p.id^hash(i));
                    const double angle = unit(tree.id)*6.283185307179586;
                    const double radius = std::sqrt(unit(tree.id+1u));
                    tree.point.x() += std::cos(angle)*radius*box.width()*0.5;
                    tree.point.y() += std::sin(angle)*radius*box.height()*0.5;
                    if (extent.getSRS()->isGeographic())
                    {
                        if (std::abs(tree.point.y()) > 90.0) continue;
                        if (tree.point.x() > 180.0) tree.point.x() -= 360.0;
                        if (tree.point.x() < -180.0) tree.point.x() += 360.0;
                    }
                    // The stand center owns these representatives, even when a root lies across a page edge.
                    const auto sample = field.sample(tree.point);
                    if (!sample.excluded && tree.densityRank() < sample.density) accept(tree, false);
                }
            }
        }

    }
    for (const auto& point : field.points())
    {
        if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable,"Coverage canceled");
        if (point.point.x() >= extent.xMin() && point.point.x() < extent.xMax() &&
            point.point.y() >= extent.yMin() && point.point.y() < extent.yMax() && !field.sampleExplicit(point.point).excluded)
            accept(point, false);

    }
    std::vector<Proxy> accepted;
    OE_RETURN_STATUS_ON_ERROR(selected.finish(accepted, progress, "coverage batch"));
    for (const auto& proxy : accepted)
        (proxy.stand ? output.stands : output.trees).push_back(proxy);
    return Status::NoError;
}
