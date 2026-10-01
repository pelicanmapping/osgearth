/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarthProcedural2/Canopy>
#include <algorithm>
#include <cmath>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

namespace
{
    //! Derives repeatable appearance/layout variation from a footprint, independent of paging order and density.
    ScatterPlacement variationFor(const GeoExtent& e)
    {
        ScatterPlacement variation;
        variation.id = std::uint64_t(std::llround(e.xMin()*1e7)) ^
            (std::uint64_t(std::llround(e.yMin()*1e7))*UINT64_C(0x9e3779b97f4a7c15)) ^
            (std::uint64_t(std::llround(e.width()*1e7))*UINT64_C(0x94d049bb133111eb)) ^
            (std::uint64_t(std::llround(e.height()*1e7))*UINT64_C(0xbf58476d1ce4e5b9));
        return variation;
    }

    //! Partitions a rectangle exactly; optional unequal splits hide the patch lattice without overlaps or gaps.
    void subdivide(const GeoExtent& e, std::vector<GeoExtent>& output, bool irregular = false)
    {
        const auto variation = variationFor(e);
        const double dx = irregular ? 0.45+0.1*variation.densityRank() : 0.5;
        const double dy = irregular ? 0.45+0.1*variation.variationSeed()/16777216.0 : 0.5;
        const double xs[] = {e.xMin(), e.xMin()+e.width()*dx, e.xMax()};
        const double ys[] = {e.yMin(), e.yMin()+e.height()*dy, e.yMax()};
        for (unsigned y=0; y<2; ++y)
            for (unsigned x=0; x<2; ++x)
                output.emplace_back(e.getSRS(), xs[x], ys[y], xs[x+1], ys[y+1]);
    }
}

unsigned osgEarth::Procedural2::canopySubdivisionLevels(const ScatterGroup& group, bool far)
{
    const float scale = far ? group.canopyFarPatchScale : group.canopyMidPatchScale;
    return scale == 0.5f ? 5u : scale == 1.0f ? 4u : scale == 2.0f ? 3u : 2u;
}

double osgEarth::Procedural2::canopyReferenceError(const TileKey& key)
{
    const auto& e = key.getExtent();
    // Global-geodetic cells are angular squares; their north/south span bounds horizontal extent at any latitude.
    const double span = e.getSRS()->isProjected() ?
        Units::convert(e.getSRS()->getUnits(),Units::METERS,std::max(e.width(),e.height())) : e.height(Units::METERS);
    return span/48.0;
}

const std::array<CanopyCrown,9>& osgEarth::Procedural2::canopyTemplate(unsigned coverage, unsigned layout)
{
    //! Initializes the bounded catalog once; occupancy changes size without changing positions or random consumption.
    static const auto catalog = []()
    {
        std::array<std::array<CanopyCrown,9>,32> result;
        for (unsigned level=1; level<=8; ++level)
            for (unsigned variant=0; variant<4; ++variant)
            {
                auto& crowns = result[(level-1)*4+variant];
                ScatterPlacement random;
                random.id = UINT64_C(0xd1310ba698dfb5ac)+variant*65537u;
                //! Supplies portable, deterministic samples for best-candidate placement and appearance.
                auto unit = [&random]() { ++random.id; return random.densityRank(); };
                const float occupancy = std::sqrt(float(level)/8.0f);
                for (unsigned i=0; i<crowns.size(); ++i)
                {
                    float best = -1.0f;
                    for (unsigned candidate=0; candidate<24; ++candidate)
                    {
                        const float x = (unit()-0.5f)*0.50f;
                        const float y = (unit()-0.5f)*0.50f;
                        const osg::Vec3 point(x,y,0);
                        float nearest = 2.0f;
                        for (unsigned j=0; j<i; ++j)
                        {
                            const auto delta = point-crowns[j].center;
                            nearest = std::min(nearest,delta.x()*delta.x()+delta.y()*delta.y());
                        }
                        if (nearest > best) { best = nearest; crowns[i].center = point; }
                    }
                    auto& c = crowns[i];
                    // Overlap crown interiors instead of shrinking edge crowns into isolated dots.
                    // Keep the same certified unit square, material sharing, and nine-piece draw recipe.
                    c.size.x() = std::min(0.5f-std::abs(c.center.x()),0.26f+0.08f*unit())*occupancy;
                    c.size.y() = std::min(0.5f-std::abs(c.center.y()),0.26f+0.08f*unit())*occupancy;
                    c.size.z() = 0.25f+0.15f*unit();
                    const float tone = unit();
                    c.center.z() = 0.52f+0.16f*unit();
                    c.color.set(0.18f+0.04f*tone,0.30f+0.04f*tone,0.14f,1);
                }
            }
        return result;
    }();
    return catalog[(std::max(1u,std::min(8u,coverage))-1u)*4u+std::min(3u,layout)];
}

GeoExtent osgEarth::Procedural2::canopyCrownFootprint(const GeoExtent& e, unsigned coverage,
    unsigned layout, unsigned turn, unsigned crown)
{
    const auto& c = canopyTemplate(coverage,layout)[std::min(8u,crown)];
    double x=c.center.x(), y=c.center.y(), rx=c.size.x(), ry=c.size.y();
    for (unsigned i=0; i<(turn%4u); ++i)
    {
        const double previous = x; x = -y; y = previous;
        std::swap(rx,ry);
    }
    return GeoExtent(e.getSRS(),e.xMin()+(0.5+x-rx)*e.width(),e.yMin()+(0.5+y-ry)*e.height(),
        e.xMin()+(0.5+x+rx)*e.width(),e.yMin()+(0.5+y+ry)*e.height());
}

GeoExtent osgEarth::Procedural2::canopyPatchCrownFootprint(const CanopyPatch& patch, unsigned crown)
{
    return canopyCrownFootprint(patch.artFootprint,patch.coverage,patch.layout,patch.turn,crown)
        .intersectionSameSRS(patch.footprint);
}

unsigned osgEarth::Procedural2::canopyClipCode(const CanopyPatch& patch)
{
    const auto& art = patch.artFootprint;
    const auto& cell = patch.footprint;
    unsigned side = unsigned(std::llround(16.0*cell.width()/art.width()));
    if (side == 16u) return 0u;
    unsigned x = unsigned(std::llround(16.0*(cell.xMin()-art.xMin())/art.width()));
    unsigned y = unsigned(std::llround(16.0*(cell.yMin()-art.yMin())/art.height()));
    // Art rotates before terrain fitting; clipping must undo that turn to stay in the original template frame.
    for (unsigned i=0; i<(patch.turn%4u); ++i)
    {
        const unsigned previous = x; x = y; y = 16u-previous-side;
    }
    return 1u+x+16u*y+256u*(side-1u);
}

Status osgEarth::Procedural2::buildCanopy(const TileKey& key, const ScatterGroup& group,
    const PlacementField& field, const SpatialReference* worldSRS, const CanopyElevation& sample,
    std::vector<CanopyPatch>& output, ProgressCallback* progress)
{
    output.clear();
    OE_RETURN_STATUS_ON_ERROR(group.validate());
    if (!key.valid() || !worldSRS || !sample)
        return Status(Status::ConfigurationError, "Invalid canopy request");
    if (!group.enabled || group.density == 0.0 || !field.hasDensity()) return Status::NoError;
    const auto& extent = key.getExtent();
    if (extent.crossesAntimeridian())
        return Status(Status::ConfigurationError, "Canopy cells must not cross the antimeridian");
    std::vector<GeoExtent> cells{extent};
    const unsigned levels = canopySubdivisionLevels(group,key.getLOD()+2u == group.renderCellLevel);
    // Larger art must not erase forest at roads or polygon edges. Keep the terminal coverage/terrain sampling
    // resolution at least as fine as the default footprint, while retaining the selected art size and layout.
    const unsigned refinements = std::max(2u,6u-levels);
    // Vary the final two splits: widths stay within 0.81..1.21 of nominal, independently of the selected base size.
    // Each page remains an exact partition; subsequent coverage/terrain refinement preserves the source art scale.
    for (unsigned level=0; level<levels; ++level)
    {
        std::vector<GeoExtent> next;
        for (const auto& cell : cells) subdivide(cell, next, level+2u >= levels);
        cells.swap(next);
    }
    std::vector<CanopyPatch> pending, result;
    for (const auto& cell : cells)
    {
        CanopyPatch patch;
        patch.footprint = patch.artFootprint = cell;
        const auto variation = variationFor(cell);
        patch.layout = variation.modelIndex(4u);
        patch.turn = variation.variationSeed()%4u;
        pending.push_back(patch);
    }
    //! Refines coverage and the terrain plane while retaining the source clump's layout and physical size.
    auto refine = [](const CanopyPatch& parent, std::vector<CanopyPatch>& next)
    {
        std::vector<GeoExtent> children;
        subdivide(parent.footprint,children);
        for (const auto& cell : children)
        {
            CanopyPatch child = parent;
            child.footprint = cell;
            next.push_back(child);
        }
    };
    for (unsigned depth=0; depth<=refinements; ++depth)
    {
        std::vector<CanopyPatch> next;
        std::vector<GeoExtent> accepted;
        std::vector<CanopyPatch> descriptions;
        std::vector<osg::Vec3d> probes;
        for (auto description : pending)
        {
            const auto& cell = description.footprint;
            if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Canopy canceled");
            CoverageSample value;
            const bool uniform = field.uniform(cell, value);
            if (!uniform && depth < refinements) { refine(description,next); continue; }
            description.coverage = 8u;
            if (!uniform)
            {
                // Find an occupancy candidate, then certify complete crowns below. Point samples never authorize art.
                value.density = 0.0f; value.excluded = false; value.pattern = 0u;
                for (unsigned crown=0; crown<9; ++crown)
                {
                    const auto box = canopyPatchCrownFootprint(description,crown);
                    if (!box.isValid() || box.width() <= 0.0 || box.height() <= 0.0) continue;
                    const auto at = field.sample(box.getCentroid().vec3d());
                    if (!at.excluded && at.pattern == 0u) value.density = std::max(value.density,at.density);
                }
            }
            if (value.excluded || value.pattern != 0u || value.density <= 0.0f) continue;
            const double scale = 0.5*(group.minScale+group.maxScale);
            // Approximate crown projection from the configured stand height instead of the old 50m2 placeholder.
            // Fullness calibrates the shared art; it never changes tree placement or the tier's handover scale.
            const double diameter = 0.8*group.canopyHeight;
            const double area = 0.7853981633974483*diameter*diameter*group.canopyCoverScale;
            const double coverage = 1.0-std::exp(-group.density*value.density*area*1e-6*scale*scale);
            description.coverage = unsigned(std::max(1.0,std::min(8.0,std::ceil(coverage*8.0))));
            // Reject invisible pieces even in uniform cells; only clipped crown rectangles authorize coverage.
            {
                description.mask = 0u;
                for (unsigned crown=0; crown<9; ++crown)
                {
                    CoverageSample at;
                    const auto box = canopyPatchCrownFootprint(description,crown);
                    if (!box.isValid() || box.width() <= 0.0 || box.height() <= 0.0) continue;
                    if (uniform || (field.uniform(box,at) && !at.excluded && at.pattern == 0u && at.density == value.density))
                        description.mask |= 1u<<crown;
                }
                if (description.mask == 0u) continue;
            }
            accepted.push_back(cell);
            descriptions.push_back(description);
            probes.emplace_back(cell.xMin(), cell.yMin(), 0);
            probes.emplace_back(cell.xMax(), cell.yMin(), 0);
            probes.emplace_back(cell.xMin(), cell.yMax(), 0);
            probes.emplace_back(cell.xMax(), cell.yMax(), 0);
            probes.push_back(cell.getCentroid().vec3d());
        }
        if (!probes.empty()) OE_RETURN_STATUS_ON_ERROR(sample(probes));
        if (probes.size() != accepted.size()*5u)
            return Status(Status::ResourceUnavailable, "Elevation adapter changed canopy probe count");
        for (std::size_t i=0; i<accepted.size(); ++i)
        {
            if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Canopy canceled");
            osg::Vec3d world[5];
            for (unsigned j=0; j<5; ++j)
            {
                const auto& p = probes[5*i+j];
                if (!std::isfinite(p.z()) || p.z() == NO_DATA_VALUE ||
                    !GeoPoint(extent.getSRS(), p).transform(worldSRS).toWorld(world[j]))
                    return Status(Status::ResourceUnavailable, "Canopy elevation unavailable");
            }
            osg::Matrixd frame;
            if (!GeoPoint(extent.getSRS(), probes[5*i+4]).transform(worldSRS).createLocalToWorld(frame))
                return Status(Status::ResourceUnavailable, "Cannot create canopy frame");
            const osg::Vec3d up = osg::Matrixd::transform3x3(osg::Vec3d(0,0,1), frame);
            const osg::Vec3d center = (world[0]+world[1]+world[2]+world[3])*0.25;
            const osg::Vec3d east = (world[1]-world[0]+world[3]-world[2])*0.5;
            const osg::Vec3d north = (world[2]-world[0]+world[3]-world[1])*0.5;
            const double residual = std::max(std::abs((world[4]-center)*up),
                std::abs((world[0]+world[3]-world[1]-world[2])*up)*0.25);
            if (residual > 2.0 && depth < refinements)
            {
                refine(descriptions[i], next);
                continue;
            }
            CanopyPatch patch = descriptions[i];
            const double scale = 0.5*(group.minScale+group.maxScale);
            const osg::Vec3d crown = up*(group.canopyHeight*scale);
            const auto& art = patch.artFootprint;
            const auto& cell = patch.footprint;
            const osg::Vec3d artEast = east*(art.width()/cell.width());
            const osg::Vec3d artNorth = north*(art.height()/cell.height());
            const osg::Vec3d artCenter = center+
                east*((art.getCentroid().x()-cell.getCentroid().x())/cell.width())+
                north*((art.getCentroid().y()-cell.getCentroid().y())/cell.height());
            patch.localToWorld.set(artEast.x(), artEast.y(), artEast.z(), 0,
                artNorth.x(), artNorth.y(), artNorth.z(), 0, crown.x(), crown.y(), crown.z(), 0,
                artCenter.x(), artCenter.y(), artCenter.z(), 1);
            patch.clip = canopyClipCode(patch);
            // Quarter turns preserve the certified square template footprint before terrain fitting.
            patch.localToWorld = osg::Matrixd::rotate(patch.turn*1.5707963267948966,
                osg::Vec3d(0,0,1))*patch.localToWorld;
            patch.error = canopyReferenceError(key) + residual;
            result.push_back(patch);
        }
        pending.swap(next);
    }
    if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Canopy canceled");
    output.swap(result);
    return Status::NoError;
}
