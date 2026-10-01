/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarthProcedural2/FeaturePlacement>
#include <osgEarth/Query>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <locale>
#include <map>
#include <sstream>
#include <set>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

namespace
{
    //! Platform-stable mixing keeps field acceptance independent of placement and render thinning.
    std::uint64_t hash64(std::uint64_t x)
    {
        x += UINT64_C(0x9e3779b97f4a7c15);
        x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
        return x ^ (x >> 31);
    }

    //! Hashes UTF-8 bytes without implementation-defined std::hash behavior.
    std::uint64_t textHash(const std::string& text, std::uint64_t seed)
    {
        for (unsigned char c : text) seed = hash64(seed ^ c);
        return seed;
    }

    //! Returns a separate stable rank in [0,1) for fractional coverage.
    float coverageRank(std::uint64_t id)
    {
        return float(hash64(id ^ UINT64_C(0x510e527fade682d1)) >> 40) / 16777216.0f;
    }

    //! Assigns shared-edge points to exactly one cell; maximum outer edges are intentionally half-open.
    bool owns(const GeoExtent& e, const osg::Vec3d& p)
    {
        return p.x() >= e.xMin() && p.x() < e.xMax() && p.y() >= e.yMin() && p.y() < e.yMax();
    }

    //! Uses a feature-local projection for meter-based road buffers, independent of vegetation batching.
    osg::ref_ptr<const SpatialReference> metricFrame(const GeoExtent& extent)
    {
        if (extent.getSRS()->isProjected() && !extent.getSRS()->isMercator() &&
            extent.getSRS()->getUnits() == Units::METERS)
            return extent.getSRS();
        GeoPoint center = extent.getCentroid().transform(SpatialReference::get("wgs84"));
        if (!center.isValid()) return {};
        std::ostringstream proj;
        proj.precision(17);
        proj << "+proj=aeqd +lat_0=" << center.y() << " +lon_0=" << center.x()
             << " +datum=WGS84 +units=m +no_defs";
        return SpatialReference::create(proj.str());
    }

    //! Squared planar distance to a segment, including round end caps and degenerate segments.
    double segmentDistance2(const osg::Vec3d& p, const osg::Vec3d& a, const osg::Vec3d& b)
    {
        const double dx = b.x()-a.x(), dy = b.y()-a.y();
        const double length2 = dx*dx + dy*dy;
        const double t = length2 > 0.0 ? std::max(0.0, std::min(1.0,
            ((p.x()-a.x())*dx + (p.y()-a.y())*dy)/length2)) : 0.0;
        const double x = p.x()-a.x()-t*dx, y = p.y()-a.y()-t*dy;
        return x*x+y*y;
    }

    //! Tests a closed segment against a rectangle, including touching boundaries, without GEOS allocation.
    bool touches(const osg::Vec3d& a, const osg::Vec3d& b, const GeoExtent& box)
    {
        double lo = 0.0, hi = 1.0;
        for (unsigned axis = 0; axis < 2; ++axis)
        {
            const double minimum = axis == 0 ? box.xMin() : box.yMin();
            const double maximum = axis == 0 ? box.xMax() : box.yMax();
            const double delta = b[axis]-a[axis];
            if (std::abs(delta) < 1e-20)
            {
                if (a[axis] < minimum || a[axis] > maximum) return false;
            }
            else
            {
                double t0 = (minimum-a[axis])/delta, t1 = (maximum-a[axis])/delta;
                if (t0 > t1) std::swap(t0, t1);
                lo = std::max(lo, t0); hi = std::min(hi, t1);
                if (lo > hi) return false;
            }
        }
        return true;
    }

    //! A worker-local field indexes exact vector predicates; no GL objects or global raster are allocated.
    class VectorField : public PlacementField
    {
        struct Area
        {
            osg::ref_ptr<const Geometry> geometry;
            osg::ref_ptr<const SpatialReference> metric;
            osg::ref_ptr<const Geometry> line;
            GeoExtent extent;
            bool exclude = false;
            float density = 1.0f;
            int priority = 0;
            std::uint64_t pattern = 0;
            double radius = 0.0;
        };
        GeoExtent _extent;
        std::vector<Area> _areas;
        std::vector<std::vector<unsigned>> _bins;
        std::vector<ScatterPlacement> _points;
        std::vector<PlacementRegion> _regions;
        bool _hasDensity = false;
        static const unsigned SIDE = 32u;

        //! Converts a coordinate to a clamped broad-phase bin, never to a placement/coverage texel.
        unsigned index(double value, double minimum, double span) const
        {
            return unsigned(std::max(0.0, std::min(double(SIDE-1u), std::floor((value-minimum)/span*SIDE))));
        }

    public:
        //! Allocates a bounded acceleration grid for this request's SRS and extent.
        explicit VectorField(const TileKey& key) : _extent(key.getExtent()), _bins(SIDE*SIDE) { }

        //! Builds immutable predicates; copies source geometry before transforming it. Failure discards the field.
        Status compile(const std::vector<PlacementFeature>& features, const std::vector<CoverageRule>& rules,
            const ScatterGroup& group, unsigned seed, ProgressCallback* progress)
        {
            std::map<std::uint64_t, ScatterPlacement> points;
            std::map<std::uint64_t, unsigned> patterns;
            std::size_t vertices = 0, references = 0;
            for (const auto& record : features)
            {
                if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Coverage canceled");
                if (!record.feature || !record.feature->getGeometry() || !record.feature->getSRS()) continue;
                std::vector<const CoverageRule*> matches;
                for (const auto& rule : rules)
                    if (rule.matches(record.source, group, *record.feature)) matches.push_back(&rule);
                if (matches.empty()) continue;
                vertices += record.feature->getGeometry()->getTotalPointCount();
                if (vertices > 2000000u)
                    return Status(Status::ResourceUnavailable, "Coverage request exceeds 2,000,000 vertices");
                osg::ref_ptr<Feature> local = new Feature(*record.feature);
                GeometryIterator coordinates(local->getGeometry());
                while (coordinates.hasMore())
                {
                    Geometry* part = coordinates.next();
                    if (!local->getSRS()->transform(part->asVector(), _extent.getSRS()))
                        return Status(Status::ResourceUnavailable, "Cannot transform coverage geometry");
                    for (const auto& p : *part)
                        if (!std::isfinite(p.x()) || !std::isfinite(p.y()))
                            return Status(Status::ResourceUnavailable, "Nonfinite coverage geometry");
                }
                local->setSRS(_extent.getSRS());
                if (!local->getExtent().isValid())
                    return Status(Status::ResourceUnavailable, "Cannot transform coverage geometry");
                ConstGeometryIterator parts(local->getGeometry(), false);
                while (parts.hasMore())
                {
                    const Geometry* geometry = parts.next();
                    const auto type = geometry->getType();
                    for (const auto* rule : matches)
                    {
                        if (rule->action == "points" && (type == Geometry::TYPE_POINT || type == Geometry::TYPE_POINTSET))
                        {
                            for (const auto& location : *geometry)
                            {
                                if (!owns(_extent, location)) continue;
                                ScatterPlacement p;
                                p.id = textHash(group.name, textHash(record.source,
                                    hash64(std::uint64_t(record.feature->getFID()) ^ hash64(seed))));
                                if (record.feature->getFID() == 0 || type == Geometry::TYPE_POINTSET)
                                {
                                    osg::Vec3d geographic;
                                    if (!_extent.getSRS()->transform(location, SpatialReference::get("wgs84"), geographic))
                                        return Status(Status::ResourceUnavailable, "Cannot transform explicit point");
                                    p.id = hash64(p.id ^ std::uint64_t(std::llround(geographic.x()*1e7)));
                                    p.id = hash64(p.id ^ std::uint64_t(std::llround(geographic.y()*1e7)));
                                }
                                p.point = location;
                                p.point.z() = 0.0;
                                p.scale = group.minScale + coverageRank(hash64(p.id))*(group.maxScale-group.minScale);
                                p.rotation = float(6.283185307179586 * coverageRank(hash64(p.id+1)));
                                if (coverageRank(p.id) < rule->density) points.emplace(p.id, p);
                            }
                            continue;
                        }
                        if (rule->action == "points") continue;
                        const bool line = type == Geometry::TYPE_LINESTRING;
                        if (type != Geometry::TYPE_POLYGON && !(line && rule->action == "exclude" && rule->buffer > 0.0))
                            continue;
                        Area area;
                        area.geometry = geometry;
                        area.exclude = rule->action == "exclude";
                        area.density = rule->density;
                        area.priority = rule->priority;
                        if (rule->strategy != "scatter")
                        {
                            area.pattern = textHash(rule->name, textHash(group.name, textHash(record.source,
                                hash64(std::uint64_t(record.feature->getFID()) ^ hash64(seed)))));
                            if (area.pattern == 0u) area.pattern = 1u;
                            auto found = patterns.find(area.pattern);
                            if (found == patterns.end())
                            {
                                PlacementRegion region;
                                region.id = area.pattern;
                                region.strategy = rule->strategy;
                                region.parameters = rule->parameters;
                                region.extent = GeoExtent(_extent.getSRS(), geometry->getBounds());
                                region.fragments.push_back(record.feature);
                                patterns.emplace(region.id, unsigned(_regions.size()));
                                _regions.push_back(region);
                            }
                            else
                            {
                                auto& region = _regions[found->second];
                                region.extent.expandToInclude(GeoExtent(_extent.getSRS(), geometry->getBounds()));
                                if (region.fragments.back().get() != record.feature.get())
                                    region.fragments.push_back(record.feature);
                            }
                        }
                        area.extent = GeoExtent(_extent.getSRS(), geometry->getBounds());
                        if (line)
                        {
                            area.radius = rule->buffer;
                            area.metric = record.metric.valid() ? record.metric : metricFrame(record.feature->getExtent());
                            if (!area.metric) return Status(Status::ResourceUnavailable, "Cannot create road buffer frame");
                            osg::ref_ptr<Feature> projected = new Feature(geometry->clone(), _extent.getSRS());
                            if (!_extent.getSRS()->transform(projected->getGeometry()->asVector(), area.metric))
                                return Status(Status::ResourceUnavailable, "Cannot transform road buffer geometry");
                            projected->setSRS(area.metric);
                            area.line = projected->getGeometry();
                            area.extent.expand(Distance(2.0*area.radius, Units::METERS),
                                Distance(2.0*area.radius, Units::METERS));
                        }
                        if (!area.extent.intersects(_extent)) continue;
                        const unsigned id = static_cast<unsigned>(_areas.size());
                        _areas.push_back(area);
                        _hasDensity = _hasDensity || (!area.exclude && area.pattern == 0u && area.density > 0.0f);
                        // Wrapped halos can touch both longitude edges; broad-phase bins must not drop either side.
                        const unsigned xmin = area.extent.crossesAntimeridian() ? 0u :
                            index(area.extent.xMin(), _extent.xMin(), _extent.width());
                        const unsigned xmax = area.extent.crossesAntimeridian() ? SIDE-1u :
                            index(area.extent.xMax(), _extent.xMin(), _extent.width());
                        const unsigned ymin = index(area.extent.yMin(), _extent.yMin(), _extent.height());
                        const unsigned ymax = index(area.extent.yMax(), _extent.yMin(), _extent.height());
                        references += (xmax-xmin+1u)*(ymax-ymin+1u);
                        if (references > 2000000u)
                            return Status(Status::ResourceUnavailable,
                                "Coverage acceleration grid exceeds its reference limit");
                        for (unsigned y=ymin; y<=ymax; ++y)
                            for (unsigned x=xmin; x<=xmax; ++x) _bins[y*SIDE+x].push_back(id);
                    }
                }
            }
            for (const auto& entry : points) _points.push_back(entry.second);
            return Status::NoError;
        }

        //! Exact polygon/hole and buffered-line tests; overlapping inclusions use max rather than sum.
        CoverageSample sample(const osg::Vec3d& point) const override
        {
            CoverageSample result;
            bool selected = false;
            int priority = 0;
            const unsigned x=index(point.x(), _extent.xMin(), _extent.width());
            const unsigned y=index(point.y(), _extent.yMin(), _extent.height());
            for (unsigned id : _bins[y*SIDE+x])
            {
                const auto& area = _areas[id];
                if (!area.extent.contains(point.x(), point.y())) continue;
                bool inside = false;
                if (area.line)
                {
                    osg::Vec3d metric;
                    if (!_extent.getSRS()->transform(point, area.metric, metric))
                    {
                        result.excluded = true; // fail closed on a malformed exclusion transform
                        return result;
                    }
                    for (std::size_t i=1; i<area.line->size() && !inside; ++i)
                        inside = segmentDistance2(metric, (*area.line)[i-1], (*area.line)[i]) <= area.radius*area.radius;
                }
                else inside = area.geometry->contains2D(point.x(), point.y());
                if (!inside) continue;
                if (area.exclude) { result.excluded = true; return result; }
                // A structured land-use region replaces scatter, even if its density is zero.
                // Hard exclusions remain independent of this inclusion priority and always veto the result.
                const bool wins = !selected || area.priority > priority || (area.priority == priority &&
                    area.pattern != 0u && (result.pattern == 0u || area.pattern < result.pattern));
                if (wins)
                {
                    selected = true;
                    priority = area.priority;
                    result.pattern = area.pattern;
                    result.density = area.density;
                }
                else if (area.priority == priority && area.pattern == result.pattern)
                    result.density = std::max(result.density, area.density);
            }
            return result;
        }

        //! Proves constant vector coverage; boundaries and buffered road intersections remain mixed.
        bool uniform(const GeoExtent& box, CoverageSample& value) const override
        {
            if (box.crossesAntimeridian() || !box.getSRS()->isHorizEquivalentTo(_extent.getSRS())) return false;
            std::set<unsigned> candidates;
            const unsigned xmin = index(box.xMin(), _extent.xMin(), _extent.width());
            const unsigned xmax = index(box.xMax(), _extent.xMin(), _extent.width());
            const unsigned ymin = index(box.yMin(), _extent.yMin(), _extent.height());
            const unsigned ymax = index(box.yMax(), _extent.yMin(), _extent.height());
            for (unsigned y=ymin; y<=ymax; ++y)
                for (unsigned x=xmin; x<=xmax; ++x)
                    candidates.insert(_bins[y*SIDE+x].begin(), _bins[y*SIDE+x].end());
            for (unsigned id : candidates)
            {
                const auto& area = _areas[id];
                if (!area.extent.intersects(box)) continue;
                if (area.line)
                {
                    // Expand in the road's metric frame, not in latitude-dependent map units.
                    GeoExtent expanded = box.transform(area.metric);
                    if (!expanded.isValid()) return false;
                    expanded.expand(Distance(2.0*area.radius, Units::METERS),
                        Distance(2.0*area.radius, Units::METERS));
                    for (std::size_t i=1; i<area.line->size(); ++i)
                        if (touches((*area.line)[i-1], (*area.line)[i], expanded)) return false;
                }
                else
                {
                    ConstGeometryIterator rings(area.geometry.get()); // includes polygon holes
                    while (rings.hasMore())
                    {
                        const Geometry* ring = rings.next();
                        if (ring->empty()) continue;
                        for (std::size_t i=0; i<ring->size(); ++i)
                            if (touches((*ring)[i], (*ring)[(i+1)%ring->size()], box)) return false;
                    }
                }
            }
            value = sample(box.getCentroid().vec3d());
            return true;
        }

        //! Empty coverage avoids generating dense grass candidates in unmapped regions.
        bool hasDensity() const override { return _hasDensity; }

        //! Returns named layout regions with source attributes retained for registered placement algorithms.
        const std::vector<PlacementRegion>& regions() const override { return _regions; }

        //! Returns this request's de-duplicated source points in stable ID order.
        const std::vector<ScatterPlacement>& points() const override { return _points; }
    };
}

FeatureInput::FeatureInput(const Config& conf)
{
    conf.get("name", name);
    features.get(conf, "features");
}

Config FeatureInput::getConfig() const
{
    Config conf("source");
    conf.set("name", name);
    features.set(conf, "features");
    return conf;
}

CoverageRule::CoverageRule(const Config& conf)
{
    conf.get("source", source); conf.get("group", group); conf.get("key", key); conf.get("value", value);
    conf.get("except", exceptValue); conf.get("action", action); conf.get("buffer", buffer); conf.get("density", density);
    conf.get("strategy", strategy); conf.get("name", name); conf.get("priority", priority);
    parameters = conf.child("placement");
}

Config CoverageRule::getConfig() const
{
    Config conf("rule");
    conf.set("source", source); conf.set("group", group); conf.set("key", key); conf.set("value", value);
    conf.set("except", exceptValue); conf.set("action", action); conf.set("buffer", buffer); conf.set("density", density);
    conf.set("strategy", strategy); conf.set("name", name); conf.set("priority", priority);
    if (strategy != "scatter") { Config settings = parameters; settings.key() = "placement"; conf.add(settings); }
    return conf;
}

Status CoverageRule::validate() const
{
    if (key.empty() || source.empty() || group.empty() ||
        (action != "include" && action != "exclude" && action != "points"))
        return Status(Status::ConfigurationError, "Coverage rule needs a key, source/group, and include/exclude/points action");
    if (!std::isfinite(density) || density < 0.0f || density > 1.0f ||
        !std::isfinite(buffer) || buffer < 0.0 || buffer > 100.0 || (buffer > 0.0 && action != "exclude"))
        return Status(Status::ConfigurationError, "Coverage needs density 0..1 and exclusion buffer 0..100 meters");
    if (strategy.empty() || (strategy != "scatter" && (name.empty() || action != "include")))
        return Status(Status::ConfigurationError, "Region strategies require a name and polygon include action");
    return Status::NoError;
}

bool CoverageRule::matches(const std::string& input, const ScatterGroup& population, const Feature& feature) const
{
    if ((source != "*" && source != input) || (group != "*" && group != population.name) || !feature.isSet(key))
        return false;
    const auto v = feature.getString(key);
    return (value == "*" || v == value) && (exceptValue.empty() || v != exceptValue);
}

RowLayout::RowLayout(const Config& conf)
{
    conf.get("origin_mode", originMode);
    conf.get("origin_longitude", longitude); conf.get("origin_latitude", latitude);
    conf.get("heading", heading); conf.get("row_spacing", rowSpacing); conf.get("plant_spacing", plantSpacing);
    conf.get("longitude_attribute", longitudeAttribute); conf.get("latitude_attribute", latitudeAttribute);
    conf.get("heading_attribute", headingAttribute); conf.get("row_spacing_attribute", rowSpacingAttribute);
    conf.get("plant_spacing_attribute", plantSpacingAttribute);
}

Config RowLayout::getConfig() const
{
    Config conf("placement");
    conf.set("origin_mode", originMode);
    if (std::isfinite(longitude)) conf.set("origin_longitude", longitude);
    if (std::isfinite(latitude)) conf.set("origin_latitude", latitude);
    conf.set("heading", heading); conf.set("row_spacing", rowSpacing); conf.set("plant_spacing", plantSpacing);
    conf.set("longitude_attribute", longitudeAttribute); conf.set("latitude_attribute", latitudeAttribute);
    conf.set("heading_attribute", headingAttribute); conf.set("row_spacing_attribute", rowSpacingAttribute);
    conf.set("plant_spacing_attribute", plantSpacingAttribute);
    return conf;
}

Status RowLayout::validate() const
{
    if (originMode != "explicit" && originMode != "world")
        return Status(Status::ConfigurationError, "Row origin_mode must be explicit or world");
    if (originMode == "world" && (std::isfinite(longitude) || std::isfinite(latitude) ||
        !longitudeAttribute.empty() || !latitudeAttribute.empty()))
        return Status(Status::ConfigurationError, "World rows cannot also specify an explicit origin");
    if (originMode == "explicit" && ((longitudeAttribute.empty() && !std::isfinite(longitude)) ||
        (latitudeAttribute.empty() && !std::isfinite(latitude)) ||
        (std::isfinite(longitude) && std::abs(longitude) > 180.0) ||
        (std::isfinite(latitude) && std::abs(latitude) >= 90.0)))
        return Status(Status::ConfigurationError, "Rows need a stable geographic origin or origin attribute bindings");
    if (!std::isfinite(heading) || !std::isfinite(rowSpacing) || !std::isfinite(plantSpacing) ||
        rowSpacing < 0.1 || rowSpacing > 1000.0 || plantSpacing < 0.1 || plantSpacing > 1000.0)
        return Status(Status::ConfigurationError, "Rows need finite heading and spacing from 0.1 to 1000 meters");
    return Status::NoError;
}

Status RowLayout::resolve(const Feature& feature, const ScatterGroup& group, RowPattern& output, unsigned worldZone) const
{
    output = RowPattern();
    RowLayout resolved = *this;
    // Parse explicitly supplied attributes strictly; a malformed string must not silently become zero degrees/meters.
    auto read = [&feature](const std::string& attribute, double& value)
    {
        if (attribute.empty() || !feature.isSet(attribute)) return true;
        std::istringstream input(feature.getString(attribute));
        input.imbue(std::locale::classic());
        double parsed = 0.0;
        if (!(input >> parsed) || !std::isfinite(parsed)) return false;
        input >> std::ws;
        if (!input.eof()) return false;
        value = parsed;
        return true;
    };
    if (!read(longitudeAttribute, resolved.longitude) || !read(latitudeAttribute, resolved.latitude) ||
        !read(headingAttribute, resolved.heading) || !read(rowSpacingAttribute, resolved.rowSpacing) ||
        !read(plantSpacingAttribute, resolved.plantSpacing))
        return Status(Status::ConfigurationError, "Invalid numeric row-layout attribute");
    resolved.longitudeAttribute.clear(); resolved.latitudeAttribute.clear();
    OE_RETURN_STATUS_ON_ERROR(resolved.validate());
    std::ostringstream projection;
    projection.imbue(std::locale::classic()); projection.precision(17);
    if (originMode == "world")
    {
        if (worldZone < 1u || worldZone > 60u)
            return Status(Status::ConfigurationError, "World rows need a projection strip from 1 to 60");
        const double west = -180.0 + 6.0*(worldZone-1u);
        // UTM-width strips share an equatorial origin across hemispheres. Paging never chooses their phase.
        projection << "+proj=tmerc +lat_0=0 +lon_0=" << west+3.0 << " +k=0.9996";
        output.zone = worldZone;
        output.domain = GeoExtent(SpatialReference::get("wgs84"), west, -85.0511287798066, west+6.0, 85.0511287798066);
    }
    else
        projection << "+proj=aeqd +lat_0=" << resolved.latitude << " +lon_0=" << resolved.longitude;
    projection << " +datum=WGS84 +units=m +no_defs";
    output.frame = SpatialReference::create(projection.str());
    if (!output.frame) return Status(Status::ConfigurationError, "Cannot create row-layout metric frame");
    // Only explicit anchors need whole-fragment bounds. World mode clips the request to each fixed domain below.
    if (originMode == "explicit")
    {
        output.extent = feature.getExtent().transform(output.frame);
        if (!output.extent.isValid() || std::max(std::abs(output.extent.xMin()), std::abs(output.extent.xMax())) > 100000.0 ||
            std::max(std::abs(output.extent.yMin()), std::abs(output.extent.yMax())) > 100000.0)
            return Status(Status::ConfigurationError, "Row polygon must be within 100km of its stable origin");
    }
    output.heading = std::fmod(resolved.heading, 360.0) * 0.017453292519943295;
    output.rowSpacing = resolved.rowSpacing * group.rowSpacingScale;
    output.plantSpacing = resolved.plantSpacing * group.plantSpacingScale;
    return Status::NoError;
}

Status RowPlacementStrategy::validate(const Config& parameters) const
{
    return RowLayout(parameters).validate();
}

Status RowPlacementStrategy::generate(const TileKey& key, const ScatterGroup& group, unsigned,
    const PlacementRegion& region, std::vector<ScatterPlacement>& output, ProgressCallback* progress) const
{
    output.clear();
    OE_RETURN_STATUS_ON_ERROR(group.validate());
    if (!key.valid() || region.fragments.empty())
        return Status(Status::ConfigurationError, "Rows require a valid cell and source region");
    if (!group.enabled || group.density == 0.0 || group.rowDensity == 0.0f) return Status::NoError;
    RowLayout layout(region.parameters);
    OE_RETURN_STATUS_ON_ERROR(layout.validate());
    const auto clipped = key.getExtent().intersectionSameSRS(region.extent);
    if (!clipped.isValid()) return Status::NoError;
    const auto geographic = clipped.transform(SpatialReference::get("wgs84"));
    if (!geographic.isValid()) return Status(Status::ResourceUnavailable, "Cannot transform row domain");
    std::vector<ScatterPlacement> candidates;
    double candidateCount = 0.0;
    const bool world = layout.originMode == "world";
    const unsigned firstZone = !world ? 0u : geographic.crossesAntimeridian() ? 1u :
        unsigned(std::max(1.0, std::min(60.0, std::floor((geographic.xMin()+180.0)/6.0)+1.0)));
    const unsigned lastZone = !world ? 0u : geographic.crossesAntimeridian() ? 60u :
        unsigned(std::max(1.0, std::min(60.0, std::floor((geographic.xMax()+180.0)/6.0)+1.0)));
    for (unsigned zone = firstZone; zone <= lastZone; ++zone)
    {
        if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Rows canceled");
        GeoExtent domain = clipped;
        if (zone != 0u)
        {
            const double west = -180.0 + 6.0*(zone-1u);
            domain = geographic.intersectionSameSRS(
                GeoExtent(geographic.getSRS(), west, -85.0511287798066, west+6.0, 85.0511287798066));
            if (!domain.isValid() || domain.width() <= 0.0 || domain.height() <= 0.0) continue;
        }
        RowPattern pattern;
        OE_RETURN_STATUS_ON_ERROR(layout.resolve(*region.fragments.front(), group, pattern, zone));
        for (std::size_t i = 1; i < region.fragments.size(); ++i)
        {
            if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Rows canceled");
            RowPattern other;
            OE_RETURN_STATUS_ON_ERROR(layout.resolve(*region.fragments[i], group, other, zone));
            if (!pattern.frame->isHorizEquivalentTo(other.frame) || pattern.heading != other.heading ||
                pattern.rowSpacing != other.rowSpacing || pattern.plantSpacing != other.plantSpacing)
                return Status(Status::ConfigurationError, "Clipped row features disagree on origin or layout metadata");
        }
        const auto metric = domain.transform(pattern.frame);
        if (!metric.isValid()) return Status(Status::ResourceUnavailable, "Cannot transform row query");
        const double sine = std::sin(pattern.heading), cosine = std::cos(pattern.heading);
        double xmin = DBL_MAX, xmax = -DBL_MAX, ymin = DBL_MAX, ymax = -DBL_MAX;
        for (double x : {metric.xMin(), metric.xMax()})
        for (double y : {metric.yMin(), metric.yMax()})
        {
            const double across = x*cosine - y*sine, along = x*sine + y*cosine;
            xmin = std::min(xmin, across); xmax = std::max(xmax, across);
            ymin = std::min(ymin, along); ymax = std::max(ymax, along);
        }
        // One-slot halo handles transformed boundary roundoff; dispatcher checks exact ownership/coverage.
        const double col0 = std::floor(xmin/pattern.rowSpacing)-1.0, col1 = std::ceil(xmax/pattern.rowSpacing)+1.0;
        const double row0 = std::floor(ymin/pattern.plantSpacing)-1.0, row1 = std::ceil(ymax/pattern.plantSpacing)+1.0;
        candidateCount += (col1-col0+1.0)*(row1-row0+1.0);
        if (!std::isfinite(candidateCount) || candidateCount > 2000000.0 ||
            std::max(std::max(std::abs(col0), std::abs(col1)), std::max(std::abs(row0), std::abs(row1))) > 1e12)
            return Status(Status::ConfigurationError, "Row request exceeds 2,000,000 candidate slots");
        const auto patternID = zone == 0u ? region.id : hash64(region.id ^ hash64(zone));
        for (std::int64_t row = std::int64_t(row0); row <= std::int64_t(row1); ++row)
        for (std::int64_t col = std::int64_t(col0); col <= std::int64_t(col1); ++col)
        {
            if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Rows canceled");
            ScatterPlacement p;
            p.id = hash64(hash64(patternID ^ hash64(std::uint64_t(row))) ^ std::uint64_t(col));
            if (coverageRank(hash64(p.id)) >= group.rowDensity) continue;
            const double across = double(col)*pattern.rowSpacing, along = double(row)*pattern.plantSpacing;
            const osg::Vec3d metricPoint(across*cosine + along*sine, -across*sine + along*cosine, 0.0);
            if (pattern.domain.isValid())
            {
                osg::Vec3d geographicPoint;
                if (!pattern.frame->transform(metricPoint, pattern.domain.getSRS(), geographicPoint))
                    return Status(Status::ResourceUnavailable, "Cannot transform world row placement");
                if (!owns(pattern.domain, geographicPoint)) continue;
            }
            if (!pattern.frame->transform(metricPoint, key.getExtent().getSRS(), p.point))
                return Status(Status::ResourceUnavailable, "Cannot transform row placement");
            p.point.z() = 0.0;
            if (!owns(key.getExtent(), p.point)) continue;
            p.scale = group.minScale + coverageRank(hash64(p.id+1))*(group.maxScale-group.minScale);
            p.rotation = float(-pattern.heading); // counterclockwise local Z; world headings approximate grid north
            candidates.push_back(p);
        }
    }
    output.swap(candidates);
    return Status::NoError;
}

PlacementStrategies osgEarth::Procedural2::defaultPlacementStrategies()
{
    return {{"rows", std::make_shared<RowPlacementStrategy>()}};
}

MixedPlacementStrategy::MixedPlacementStrategy(const PlacementStrategies& strategies) : _strategies(strategies) { }

Status MixedPlacementStrategy::generate(const TileKey& key, const ScatterGroup& group, unsigned seed,
    const PlacementField& field, std::vector<ScatterPlacement>& output, ProgressCallback* progress) const
{
    output.clear();
    std::vector<ScatterPlacement> result, candidates;
    OE_RETURN_STATUS_ON_ERROR(NaturalPlacementStrategy().generate(key, group, seed, field, result, progress));
    if (!group.enabled || group.density == 0.0) return Status::NoError;
    std::size_t candidateCount = 0u;
    for (const auto& region : field.regions())
    {
        if (!region.extent.intersects(key.getExtent())) continue;
        auto algorithm = _strategies.find(region.strategy);
        if (algorithm == _strategies.end() || !algorithm->second)
            return Status(Status::ConfigurationError, "Unregistered placement strategy: " + region.strategy);
        OE_RETURN_STATUS_ON_ERROR(algorithm->second->validate(region.parameters));
        OE_RETURN_STATUS_ON_ERROR(algorithm->second->generate(key, group, seed, region, candidates, progress));
        candidateCount += candidates.size();
        if (candidateCount > 2000000u)
            return Status(Status::ConfigurationError, "Region request exceeds 2,000,000 candidates");
        for (const auto& p : candidates)
        {
            if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Placement canceled");
            if (!std::isfinite(p.point.x()) || !std::isfinite(p.point.y()) || !std::isfinite(p.point.z()) ||
                !std::isfinite(p.scale) || p.scale <= 0.0f || !std::isfinite(p.rotation))
                return Status(Status::ConfigurationError, "Placement strategy emitted an invalid instance");
            if (!owns(key.getExtent(), p.point)) continue;
            const auto value = field.sample(p.point);
            if (value.excluded || value.pattern != region.id || coverageRank(p.id) >= value.density) continue;
            if (result.size() >= group.maxPerCell)
                return Status(Status::ConfigurationError, "Region placement exceeds population instance limit");
            result.push_back(p);
        }
    }
    if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Placement canceled");
    output.swap(result);
    return Status::NoError;
}

Status PlacementFeatureProvider::queryFiltered(const TileKey& key, double buffer,
    const PlacementFeatureFilter& filter, std::vector<PlacementFeature>& output, ProgressCallback* progress) const
{
    output.clear();
    std::vector<PlacementFeature> result;
    OE_RETURN_STATUS_ON_ERROR(query(key, buffer, result, progress));
    for (const auto& record : result)
    {
        if (progress && progress->isCanceled())
        {
            output.clear();
            return Status(Status::ResourceUnavailable, "Feature selection canceled");
        }
        if (record.feature && (!filter || filter(record.source, *record.feature))) output.push_back(record);
    }
    if (progress && progress->isCanceled())
    {
        output.clear();
        return Status(Status::ResourceUnavailable, "Feature selection canceled");
    }
    return Status::NoError;
}

MapFeatureProvider::MapFeatureProvider(const std::vector<FeatureInput>& inputs) : _inputs(inputs) { }

Status MapFeatureProvider::query(const TileKey& key, double buffer, std::vector<PlacementFeature>& output,
    ProgressCallback* progress) const
{
    return queryFiltered(key, buffer, {}, output, progress);
}

Status MapFeatureProvider::queryFiltered(const TileKey& key, double buffer, const PlacementFeatureFilter& filter,
    std::vector<PlacementFeature>& output, ProgressCallback* progress) const
{
    output.clear();
    if (!key.valid() || !std::isfinite(buffer) || buffer < 0.0 || buffer > 100.0)
        return Status(Status::ConfigurationError, "Invalid geographic query key or buffer");
    std::vector<PlacementFeature> result;
    unsigned scanned = 0;
    GeoExtent extent = key.getExtent();
    extent.expand(Distance(2.0*buffer, Units::METERS), Distance(2.0*buffer, Units::METERS));
    for (const auto& input : _inputs)
    {
        auto* source = input.features.getLayer();
        if (!source || !source->isOpen() || !source->getFeatureProfile())
            return Status(Status::ResourceUnavailable, "Feature source is unavailable: " + input.name);
        const auto* fp = source->getFeatureProfile();
        if (!extent.intersects(fp->getExtent())) continue;
        std::vector<Query> queries;
        if (fp->isTiled())
        {
            const Profile* profile = fp->getTilingProfile();
            if (!profile || fp->getFirstLevel() < 0 || fp->getMaxLevel() < fp->getFirstLevel() || fp->getMaxLevel() > 30)
                return Status(Status::ConfigurationError, "Invalid native feature tiling levels: " + input.name);
            const unsigned lod = std::min(unsigned(std::max(0, fp->getMaxLevel())),
                std::max(unsigned(std::max(0, fp->getFirstLevel())),
                    profile->getEquivalentLOD(key.getProfile(), key.getLOD())));
            // A buffered request can wrap at the date line. Preflight each side rather than its global MBR.
            GeoExtent first, second;
            std::vector<GeoExtent> parts{extent};
            if (extent.splitAcrossAntimeridian(first, second)) parts = {first, second};
            std::set<TileKey> keys;
            for (const auto& part : parts)
            {
                const GeoExtent native = profile->clampAndTransformExtent(part);
                if (!native.isValid()) continue;
                double width = 0.0, height = 0.0;
                profile->getTileDimensions(lod, width, height);
                // Reject enormous requests BEFORE enumerating native keys.
                if (width <= 0.0 || height <= 0.0 ||
                    std::ceil(native.width()/width)*std::ceil(native.height()/height) > 64.0)
                    return Status(Status::ConfigurationError, "Coverage request exceeds 64 native source tiles: " + input.name);
                std::vector<TileKey> intersecting;
                profile->getIntersectingTiles(part, lod, intersecting);
                keys.insert(intersecting.begin(), intersecting.end());
                if (keys.size() > 64u)
                    return Status(Status::ConfigurationError, "Coverage request exceeds 64 native source tiles: " + input.name);
            }
            for (const auto& nativeKey : keys) queries.emplace_back(nativeKey);
        }
        else
        {
            GeoExtent local = extent.transform(fp->getSRS());
            if (!local.isValid()) return Status(Status::ResourceUnavailable, "Cannot transform feature query");
            Query query;
            query.bounds() = local.bounds();
            queries.push_back(query);
        }
        for (const auto& query : queries)
        {
            if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Feature query canceled");
            // A native tile shares one metric projection; avoid constructing one projection per road.
            osg::ref_ptr<const SpatialReference> metric;
            if (query.tileKey().isSet()) metric = metricFrame(query.tileKey()->getExtent());
            auto cursor = source->createFeatureCursor(query, {}, nullptr, progress);
            if (!cursor) return Status(Status::ResourceUnavailable, "Feature query failed: " + input.name);
            while (cursor->hasMore())
            {
                if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Feature query canceled");
                const Feature* feature = cursor->nextFeature();
                if (++scanned > 1000000u)
                    return Status(Status::ResourceUnavailable, "Coverage query exceeds 1,000,000 scanned features");
                if (!feature || !feature->getGeometry()) continue;
                if (filter && !filter(input.name, *feature)) continue;
                if (!feature->getExtent().intersects(extent)) continue;
                if (result.size() >= 100000u)
                    return Status(Status::ResourceUnavailable, "Coverage query exceeds 100,000 features");
                result.push_back({input.name, new Feature(*feature), metric});
            }
        }
    }
    output.swap(result);
    return Status::NoError;
}

Status NaturalPlacementStrategy::generate(const TileKey& key, const ScatterGroup& group, unsigned seed,
    const PlacementField& field, std::vector<ScatterPlacement>& output, ProgressCallback* progress) const
{
    output.clear();
    std::vector<ScatterPlacement> candidates, result;
    UniformScatterSource scatter;
    if (field.hasDensity())
    {
        OE_RETURN_STATUS_ON_ERROR(scatter.generate(key, group, seed, candidates, progress));
    }
    if (!group.enabled || group.density == 0.0) return Status::NoError;
    for (std::size_t i=0; i<candidates.size(); ++i)
    {
        if ((i & 255u) == 0u && progress && progress->isCanceled())
            return Status(Status::ResourceUnavailable, "Placement canceled");
        const auto value = field.sample(candidates[i].point);
        if (!value.excluded && value.pattern == 0u && coverageRank(candidates[i].id) < value.density)
            result.push_back(candidates[i]);
    }
    for (const auto& point : field.points())
    {
        if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Placement canceled");
        if (!owns(key.getExtent(), point.point) || field.sample(point.point).excluded) continue;
        if (result.size() >= group.maxPerCell)
            return Status(Status::ConfigurationError, "Explicit points exceed population instance limit");
        result.push_back(point);
    }
    if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Placement canceled");
    output.swap(result);
    return Status::NoError;
}

FeatureScatterSource::FeatureScatterSource(std::shared_ptr<const PlacementFeatureProvider> provider,
    const std::vector<CoverageRule>& rules, std::shared_ptr<const PlacementStrategy> strategy) :
    _provider(std::move(provider)), _strategy(std::move(strategy)), _rules(rules)
{
    if (!_strategy) _strategy = std::make_shared<MixedPlacementStrategy>();
}

Status FeatureScatterSource::generate(const TileKey& key, const ScatterGroup& group, unsigned seed,
    std::vector<ScatterPlacement>& output, ProgressCallback* progress) const
{
    return generateImpl(key, group, seed, false, output, progress);
}

Status FeatureScatterSource::generateBatch(const TileKey& key, const ScatterGroup& group, unsigned seed,
    std::vector<ScatterPlacement>& output, ProgressCallback* progress) const
{
    return generateImpl(key, group, seed, true, output, progress);
}

Status FeatureScatterSource::generateImpl(const TileKey& key, const ScatterGroup& group, unsigned seed, bool batch,
    std::vector<ScatterPlacement>& output, ProgressCallback* progress) const
{
    output.clear();
    OE_RETURN_STATUS_ON_ERROR(group.validate());
    const unsigned firstLevel = group.canopy ? group.renderCellLevel-(group.canopyFar ? 2u : 1u) : group.renderCellLevel;
    if (!_provider || !key.valid() || (batch && (key.getLOD() < firstLevel || key.getLOD() > group.renderCellLevel)))
        return Status(Status::ConfigurationError, "Invalid feature scatter request");
    if (!group.enabled || group.density == 0.0) return Status::NoError;
    std::shared_ptr<const PlacementField> field;
    OE_RETURN_STATUS_ON_ERROR(queryField(key, group, seed, field, progress));
    std::vector<ScatterPlacement> result, cell;
    const unsigned side = batch ? 1u << (group.cellLevel-key.getLOD()) : 1u;
    for (unsigned y=0; y<side; ++y)
    for (unsigned x=0; x<side; ++x)
    {
        if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Feature scatter canceled");
        const TileKey sourceKey = batch ? TileKey(group.cellLevel, key.getTileX()*side+x,
            key.getTileY()*side+y, key.getProfile()) : key;
        OE_RETURN_STATUS_ON_ERROR(_strategy->generate(sourceKey, group, seed, *field, cell, progress));
        if (cell.size() > group.maxPerCell || cell.size() > group.maxPerBatch-result.size())
            return Status(Status::ConfigurationError, "Feature scatter exceeds population instance limits");
        result.insert(result.end(), cell.begin(), cell.end());
    }
    output.swap(result);
    return Status::NoError;
}

Status FeatureScatterSource::queryField(const TileKey& key, const ScatterGroup& group, unsigned seed,
    std::shared_ptr<const PlacementField>& output, ProgressCallback* progress) const
{
    output.reset();
    OE_RETURN_STATUS_ON_ERROR(group.validate());
    if (!_provider || !key.valid()) return Status(Status::ConfigurationError, "Invalid coverage request");
    double buffer = 0.0;
    std::set<std::string> names;
    for (const auto& rule : _rules)
    {
        OE_RETURN_STATUS_ON_ERROR(rule.validate());
        if (rule.strategy != "scatter" && !names.insert(rule.name).second)
            return Status(Status::ConfigurationError, "Region rule names must be unique");
        buffer = std::max(buffer, rule.buffer);
    }
    std::vector<PlacementFeature> features;
    PlacementFeatureFilter filter = [&](const std::string& input, const Feature& feature)
    {
        for (const auto& rule : _rules)
            if (rule.matches(input, group, feature)) return true;
        return false;
    };
    OE_RETURN_STATUS_ON_ERROR(_provider->queryFiltered(key, buffer, filter, features, progress));
    auto field = std::make_shared<VectorField>(key);
    OE_RETURN_STATUS_ON_ERROR(field->compile(features, _rules, group, seed, progress));
    output = std::move(field);
    return Status::NoError;
}
