/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarthProcedural2/Scatter>
#include <cmath>
#include <algorithm>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

namespace
{
    //! Fixed integer mixing avoids platform-dependent std::hash and random distributions.
    std::uint64_t mix(std::uint64_t value)
    {
        value += UINT64_C(0x9e3779b97f4a7c15);
        value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
        return value ^ (value >> 31);
    }

    //! Samples strictly inside (0,1), keeping points off shared cell boundaries.
    double unit(std::uint64_t value)
    {
        return (double(mix(value) >> 32) + 0.5) / 4294967296.0;
    }
}

ScatterGroup::ScatterGroup(const Config& conf)
{
    conf.get("name", name);
    conf.get("asset", asset);
    for (const auto& model : conf.child("models").children("model")) models.push_back(model.value());
    conf.get("enabled", enabled);
    conf.get("density", density);
    conf.get("row_density", rowDensity);
    conf.get("row_spacing_scale", rowSpacingScale);
    conf.get("plant_spacing_scale", plantSpacingScale);
    conf.get("max_range", maxRange);
    conf.get("cell_level", cellLevel);
    conf.get("render_cell_level", renderCellLevel);
    conf.get("max_per_cell", maxPerCell);
    conf.get("max_per_batch", maxPerBatch);
    conf.get("min_scale", minScale);
    conf.get("max_scale", maxScale);
    conf.get("lod_pixels", lodPixels);
    conf.get("min_pixels", minPixels);
    conf.get("lod_transition", lodTransition);
    conf.get("density_start", densityStart);
    conf.get("density_end", densityEnd);
    conf.get("far_density", farDensity);
    conf.get("canopy", canopy);
    conf.get("canopy_far", canopyFar);
    conf.get("canopy_mid_gpu_culling", canopyMidGPUCulling);
    conf.get("canopy_far_gpu_culling", canopyFarGPUCulling);
    conf.get("canopy_mid_cluster_size", canopyMidClusterSize);
    conf.get("canopy_far_cluster_size", canopyFarClusterSize);
    conf.get("canopy_mid_patch_scale", canopyMidPatchScale);
    conf.get("canopy_far_patch_scale", canopyFarPatchScale);
    conf.get("canopy_cover_scale", canopyCoverScale);
    // Legacy scenes specified an absolute canopy budget; preserve it at the default global SSE of 25px.
    float legacyPixels = 48.0f;
    if (conf.get("canopy_pixels", legacyPixels)) qualityOffset = legacyPixels - 25.0f;
    conf.get("quality_offset", qualityOffset);
    conf.get("canopy_height", canopyHeight);
    conf.get("canopy_transition", canopyTransition);
    conf.get("canopy_fade_seconds", canopyFadeSeconds);
    conf.get("procedural_grass", proceduralGrass);
    conf.get("grass_blades", grassBlades);
    conf.get("grass_radius", grassRadius);
    conf.get("grass_height", grassHeight);
    conf.get("grass_width", grassWidth);
    conf.get("grass_wind", grassWind);
}

Config ScatterGroup::getConfig() const
{
    Config conf("group");
    conf.set("name", name);
    conf.set("asset", asset);
    Config choices("models");
    for (const auto& model : models) choices.add("model", model);
    conf.add(choices);
    conf.set("enabled", enabled);
    conf.set("density", density);
    conf.set("row_density", rowDensity);
    conf.set("row_spacing_scale", rowSpacingScale);
    conf.set("plant_spacing_scale", plantSpacingScale);
    conf.set("max_range", maxRange);
    conf.set("cell_level", cellLevel);
    conf.set("render_cell_level", renderCellLevel);
    conf.set("max_per_cell", maxPerCell);
    conf.set("max_per_batch", maxPerBatch);
    conf.set("min_scale", minScale);
    conf.set("max_scale", maxScale);
    conf.set("lod_pixels", lodPixels);
    conf.set("min_pixels", minPixels);
    conf.set("lod_transition", lodTransition);
    conf.set("density_start", densityStart);
    conf.set("density_end", densityEnd);
    conf.set("far_density", farDensity);
    conf.set("canopy", canopy);
    conf.set("canopy_far", canopyFar);
    conf.set("canopy_mid_gpu_culling", canopyMidGPUCulling);
    conf.set("canopy_far_gpu_culling", canopyFarGPUCulling);
    conf.set("canopy_mid_cluster_size", canopyMidClusterSize);
    conf.set("canopy_far_cluster_size", canopyFarClusterSize);
    conf.set("canopy_mid_patch_scale", canopyMidPatchScale);
    conf.set("canopy_far_patch_scale", canopyFarPatchScale);
    conf.set("canopy_cover_scale", canopyCoverScale);
    conf.set("quality_offset", qualityOffset);
    conf.set("canopy_height", canopyHeight);
    conf.set("canopy_transition", canopyTransition);
    conf.set("canopy_fade_seconds", canopyFadeSeconds);
    conf.set("procedural_grass", proceduralGrass);
    conf.set("grass_blades", grassBlades);
    conf.set("grass_radius", grassRadius);
    conf.set("grass_height", grassHeight);
    conf.set("grass_width", grassWidth);
    conf.set("grass_wind", grassWind);
    return conf;
}

float ScatterGroup::effectiveError(float globalSSE) const
{
    return std::max(1.0f, globalSSE + qualityOffset);
}

Status ScatterGroup::validate() const
{
    if (name.empty() || asset.empty())
        return Status(Status::ConfigurationError, "Population name and asset are required");
    if (!std::isfinite(density) || density < 0.0)
        return Status(Status::ConfigurationError, "Density must be finite and nonnegative");
    if (!std::isfinite(rowDensity) || rowDensity < 0.0f || rowDensity > 1.0f ||
        !std::isfinite(rowSpacingScale) || rowSpacingScale < 0.1f || rowSpacingScale > 10.0f ||
        !std::isfinite(plantSpacingScale) || plantSpacingScale < 0.1f || plantSpacingScale > 10.0f)
        return Status(Status::ConfigurationError, "Rows need occupancy 0..1 and spacing scales 0.1..10");
    if (!std::isfinite(maxRange) || maxRange <= 0.0f)
        return Status(Status::ConfigurationError, "Maximum range must be finite and positive");
    if (cellLevel < 1u || cellLevel > 24u || renderCellLevel < 1u || renderCellLevel > cellLevel ||
        cellLevel - renderCellLevel > 6u)
        return Status(Status::ConfigurationError,
            "Levels must satisfy 1 <= render <= source <= 24, with at most six levels between them");
    if (maxPerCell == 0u || maxPerCell > 1000000u || maxPerBatch == 0u || maxPerBatch > 1000000u)
        return Status(Status::ConfigurationError, "Request limits must be between 1 and 1,000,000 instances");
    if (!std::isfinite(minScale) || !std::isfinite(maxScale) || minScale <= 0.0f || maxScale < minScale)
        return Status(Status::ConfigurationError, "Scales must be finite, with 0 < minimum <= maximum");
    if (!std::isfinite(lodPixels) || lodPixels < 0.0f || !std::isfinite(minPixels) || minPixels < 0.0f ||
        (lodPixels > 0.0f && lodPixels <= minPixels))
        return Status(Status::ConfigurationError,
            "Pixel cutoffs must be finite and nonnegative; coarse LOD must be zero or above the cull cutoff");
    if (!std::isfinite(lodTransition) || lodTransition < 0.0f || lodTransition > 0.5f)
        return Status(Status::ConfigurationError, "LOD transition must be between 0 and 0.5");
    if (!std::isfinite(farDensity) || farDensity < 0.0f || farDensity > 1.0f)
        return Status(Status::ConfigurationError, "Far density must be between 0 and 100 percent");
    if (!std::isfinite(densityStart) || densityStart < 0.0f || !std::isfinite(densityEnd) || densityEnd < 0.0f ||
        (farDensity < 1.0f && (densityEnd <= densityStart || densityEnd > maxRange)))
        return Status(Status::ConfigurationError,
            "Thinning needs 0 <= start < end <= maximum range when far density is below 100 percent");
    if (canopy && (asset != "trees" || renderCellLevel < 3u || farDensity != 1.0f))
        return Status(Status::ConfigurationError,
            "Canopy requires trees, render level >= 3, and disabled distance thinning");
    if (canopyMidClusterSize < 8u || canopyMidClusterSize > 256u ||
        canopyFarClusterSize < 8u || canopyFarClusterSize > 256u)
        return Status(Status::ConfigurationError, "Tree card clusters need 8..256 trees per group");
    for (float size : {canopyMidPatchScale,canopyFarPatchScale})
        if (size != 0.5f && size != 1.0f && size != 2.0f && size != 4.0f)
            return Status(Status::ConfigurationError, "Canopy footprint scales must be 0.5, 1, 2, or 4");
    if (!std::isfinite(canopyCoverScale) || canopyCoverScale < 0.25f || canopyCoverScale > 4.0f)
        return Status(Status::ConfigurationError, "Canopy cover scale must be between 0.25 and 4");
    if (!std::isfinite(canopyTransition) || canopyTransition < 0.0f || canopyTransition > 0.5f ||
        !std::isfinite(canopyFadeSeconds) || canopyFadeSeconds < 0.0f || canopyFadeSeconds > 5.0f)
        return Status(Status::ConfigurationError, "Canopy overlap needs 0..0.5 and arrival fade needs 0..5 seconds");
    if (!std::isfinite(qualityOffset) || qualityOffset < -4096.0f || qualityOffset > 4096.0f)
        return Status(Status::ConfigurationError, "Quality offset must be finite and between -4096 and 4096 pixels");
    if (!std::isfinite(canopyHeight) || canopyHeight < 1.0f || canopyHeight > 80.0f)
        return Status(Status::ConfigurationError, "Canopy height must be between 1 and 80 meters");
    if (proceduralGrass && asset != "grass")
        return Status(Status::ConfigurationError, "Procedural grass is only available for the grass asset");
    if (grassBlades < 4u || grassBlades > 128u)
        return Status(Status::ConfigurationError, "Grass blades per patch must be between 4 and 128");
    if (!std::isfinite(grassRadius) || grassRadius < 0.1f || grassRadius > 4.0f ||
        !std::isfinite(grassHeight) || grassHeight < 0.05f || grassHeight > 2.0f ||
        !std::isfinite(grassWidth) || grassWidth < 0.002f || grassWidth > 0.1f ||
        !std::isfinite(grassWind) || grassWind < 0.0f || grassWind > 1.0f)
        return Status(Status::ConfigurationError,
            "Grass needs radius 0.1..4m, height 0.05..2m, half-width 0.002..0.1m, and wind 0..1m");
    return Status::NoError;
}

float ScatterPlacement::densityRank() const
{
    // Use a separate hash stream: UniformScatterSource uses mix(id) for the cell-local X coordinate.
    // Reusing it here would thin every cell to its western strip. Keep placement IDs/coordinates unchanged.
    // The top 24 mixed bits are exactly representable as float, including for sequential source IDs.
    return float(mix(id ^ UINT64_C(0x6a09e667f3bcc909)) >> 40) / 16777216.0f;
}

unsigned ScatterPlacement::variationSeed() const
{
    return static_cast<unsigned>(mix(id ^ UINT64_C(0xbb67ae8584caa73b)) >> 40);
}

unsigned ScatterPlacement::modelIndex(unsigned count) const
{
    return count == 0u ? 0u : static_cast<unsigned>(mix(id ^ UINT64_C(0x3c6ef372fe94f82b)) % count);
}

Status ScatterSource::generateBatch(const TileKey& key, const ScatterGroup& group, unsigned seed,
    std::vector<ScatterPlacement>& output, ProgressCallback* progress) const
{
    output.clear();
    OE_RETURN_STATUS_ON_ERROR(group.validate());
    const unsigned firstLevel = group.canopy ? group.renderCellLevel-(group.canopyFar ? 2u : 1u) : group.renderCellLevel;
    if (!key.valid() || key.getLOD() < firstLevel || key.getLOD() > group.renderCellLevel)
        return Status(Status::ConfigurationError, "Invalid render cell for " + group.name);
    if (progress && progress->isCanceled())
        return Status(Status::ResourceUnavailable, "Scatter canceled");
    if (!group.enabled || group.density == 0.0) return Status::NoError;

    // Source keys are unchanged at every representation. Aggregate requests cover at most 65536 source cells.
    const unsigned side = 1u << (group.cellLevel - key.getLOD());
    std::vector<ScatterPlacement> result, cell;
    for (unsigned y = 0; y < side; ++y)
    for (unsigned x = 0; x < side; ++x)
    {
        if (progress && progress->isCanceled())
            return Status(Status::ResourceUnavailable, "Scatter canceled");
        const TileKey sourceKey(group.cellLevel, key.getTileX()*side + x, key.getTileY()*side + y, key.getProfile());
        cell.clear();
        OE_RETURN_STATUS_ON_ERROR(generate(sourceKey, group, seed, cell, progress));
        if (cell.size() > group.maxPerCell || cell.size() > group.maxPerBatch - result.size())
            return Status(Status::ConfigurationError, "Scatter batch exceeds its instance limits: " + group.name);
        result.insert(result.end(), cell.begin(), cell.end());
    }
    if (progress && progress->isCanceled())
        return Status(Status::ResourceUnavailable, "Scatter canceled");
    output.swap(result);
    return Status::NoError;
}

Status UniformScatterSource::generate(const TileKey& key, const ScatterGroup& group, unsigned seed,
    std::vector<ScatterPlacement>& output, ProgressCallback* progress) const
{
    output.clear();
    OE_RETURN_STATUS_ON_ERROR(group.validate());
    if (!key.valid())
        return Status(Status::ConfigurationError, "Invalid scatter cell");
    if (progress && progress->isCanceled())
        return Status(Status::ResourceUnavailable, "Scatter canceled");
    if (!group.enabled || group.density == 0.0)
        return Status::NoError;

    const auto& extent = key.getExtent();
    double area;
    if (extent.getSRS()->isGeographic())
    {
        // Spherical area includes longitude convergence. The product form stays stable in small polar cells.
        // This synthetic source approximates the ellipsoid; geographic source cells should remain small.
        const double radius = extent.getSRS()->getEllipsoid().getRadiusEquator();
        const double midLatitude = osg::DegreesToRadians(0.5 * (extent.yMin() + extent.yMax()));
        area = radius * radius * osg::DegreesToRadians(extent.width()) *
            2.0 * std::cos(midLatitude) * std::sin(osg::DegreesToRadians(extent.height()) * 0.5) * 1e-6;
    }
    else
    {
        const double width = Units::convert(extent.getSRS()->getUnits(), Units::METERS, extent.width());
        const double height = Units::convert(extent.getSRS()->getUnits(), Units::METERS, extent.height());
        area = width * height * 1e-6;
    }
    const double expected = area * group.density;
    if (!std::isfinite(expected) || expected < 0.0 || std::ceil(expected) > group.maxPerCell)
        return Status(Status::ConfigurationError, "Scatter cell exceeds max_per_cell: " + group.name);

    std::uint64_t salt = mix(seed);
    for (unsigned char c : group.name)
        salt = mix(salt ^ c);
    salt = mix(salt ^ key.getLOD());
    salt = mix(salt ^ key.getTileX());
    salt = mix(salt ^ (std::uint64_t(key.getTileY()) << 32));
    const unsigned count = unsigned(expected) + (unit(salt) < expected - std::floor(expected) ? 1u : 0u);
    std::vector<ScatterPlacement> result;
    result.reserve(count);
    for (unsigned i = 0; i < count; ++i)
    {
        if ((i & 255u) == 0u && progress && progress->isCanceled())
            return Status(Status::ResourceUnavailable, "Scatter canceled");
        ScatterPlacement p;
        p.id = mix(salt ^ mix(i));
        p.point.set(extent.xMin() + unit(p.id) * extent.width(),
            extent.yMin() + unit(p.id + 1u) * extent.height(), 0.0);
        p.rotation = float(unit(p.id + 2u) * 6.283185307179586);
        p.scale = group.minScale + float(unit(p.id + 3u)) * (group.maxScale - group.minScale);
        result.push_back(p);
    }
    output.swap(result);
    return Status::NoError;
}
