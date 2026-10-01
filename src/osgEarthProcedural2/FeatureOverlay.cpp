/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarthProcedural2/FeatureOverlay>
#include <osgEarth/GeometryUtils>
#include <osgEarth/JsonUtils>
#include <osgEarth/FileUtils>
#include <osgDB/FileNameUtils>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <cstdio>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace osgEarth;
using namespace osgEarth::Procedural2;
namespace Json = osgEarth::Util::Json;

namespace
{
    constexpr std::size_t maxFeatures = 4096, maxVertices = 2048;
    //! Returns an authoring error without changing the live document.
    Status invalid(const std::string& message) { return Status(Status::ConfigurationError, message); }
    //! Oriented planar triangle area; local geographic authoring excludes date-line crossings.
    double turn(const osg::Vec3d& a, const osg::Vec3d& b, const osg::Vec3d& c)
    {
        return (b.x()-a.x())*(c.y()-a.y()) - (b.y()-a.y())*(c.x()-a.x());
    }
    //! Rejects touching or crossing non-neighbor edges, including collinear overlaps.
    bool intersects(const osg::Vec3d& a, const osg::Vec3d& b, const osg::Vec3d& c, const osg::Vec3d& d)
    {
        if (std::max(a.x(),b.x()) < std::min(c.x(),d.x()) || std::max(c.x(),d.x()) < std::min(a.x(),b.x()) ||
            std::max(a.y(),b.y()) < std::min(c.y(),d.y()) || std::max(c.y(),d.y()) < std::min(a.y(),b.y())) return false;
        return turn(a,b,c)*turn(a,b,d) <= 0.0 && turn(c,d,a)*turn(c,d,b) <= 0.0;
    }
    //! Replaces a local file after a successful temporary write; no existing data is removed on error.
    bool replaceFile(const std::string& temporary, const std::string& destination)
    {
#ifdef _WIN32
        // osgEarth's file paths are UTF-8; keep non-ASCII project directories usable on Windows.
        auto wide = [](const std::string& value)
        {
            const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0);
            std::wstring result(size > 0 ? size : 0, L'\0');
            if (size > 0) MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &result[0], size);
            return result;
        };
        const auto from = wide(temporary), to = wide(destination);
        return !from.empty() && !to.empty() && MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
        return std::rename(temporary.c_str(), destination.c_str()) == 0;
#endif
    }
}

OverlayType::OverlayType(const Config& conf)
{
    conf.get("name", name); conf.get("label", label); conf.get("geometry", geometry);
    for (const auto& attr : conf.children("attribute"))
        attributes[attr.value("key")] = attr.value("value");
}

Config OverlayType::getConfig() const
{
    Config conf("type");
    conf.set("name", name); conf.set("label", label); conf.set("geometry", geometry);
    for (const auto& attr : attributes)
    {
        Config value("attribute"); value.set("key", attr.first); value.set("value", attr.second); conf.add(value);
    }
    return conf;
}

FeatureOverlay::FeatureOverlay(const std::vector<OverlayType>& types, const std::string& source) :
    _types(types), _source(source), _current(std::make_shared<OverlaySnapshot>()) { }

const OverlayType* FeatureOverlay::findType(const std::string& name) const
{
    for (const auto& type : _types) if (type.name == name) return &type;
    return nullptr;
}

std::shared_ptr<const OverlaySnapshot> FeatureOverlay::snapshot() const { return std::atomic_load(&_current); }

Status FeatureOverlay::validateCatalog(const std::vector<CoverageRule>& rules, const std::vector<ScatterGroup>& groups) const
{
    if (_source.empty() || _source == "*") return invalid("Overlay requires a named source namespace");
    std::set<std::string> names;
    for (const auto& type : _types)
    {
        if (type.name.empty() || !names.insert(type.name).second ||
            (type.geometry != "polygon" && type.geometry != "line") || type.attributes.empty())
            return invalid("Overlay types require unique names, polygon/line geometry, and attributes");
        osg::ref_ptr<Feature> feature = new Feature(nullptr, SpatialReference::get("wgs84"));
        for (const auto& attr : type.attributes)
        {
            if (attr.first.empty() || attr.first == "p2:type") return invalid("Invalid overlay catalog attribute");
            feature->set(attr.first, attr.second);
        }
        bool matched = false;
        for (const auto& rule : rules)
        for (const auto& group : groups)
            if (rule.action == "exclude" && (type.geometry == "polygon" || rule.buffer > 0.0) &&
                rule.matches(_source, group, *feature)) matched = true;
        if (!matched) return invalid("Overlay type has no applicable exclusion rule: " + type.name);
    }
    return Status::NoError;
}

Status FeatureOverlay::prepare(const std::string& typeName, const Geometry* input, const SpatialReference* srs,
    FeatureID id, osg::ref_ptr<const Feature>& output) const
{
    const auto* type = findType(typeName);
    if (!type || !input || !srs || id <= 0) return invalid("Unknown overlay type, missing geometry/SRS, or invalid ID");
    const bool polygon = type->geometry == "polygon";
    if (input->getType() != (polygon ? Geometry::TYPE_POLYGON : Geometry::TYPE_LINESTRING) ||
        (polygon && !static_cast<const Polygon*>(input)->getHoles().empty()))
        return invalid("This editor accepts a simple polygon or polyline matching the catalog type");
    if (input->size() > maxVertices) return invalid("Overlay feature exceeds 2048 vertices");
    osg::ref_ptr<Geometry> geometry = input->clone();
    for (auto& point : *geometry)
        if (!srs->transform(point, SpatialReference::get("wgs84"), point)) return invalid("Cannot transform overlay to WGS84");
    if (polygon) geometry->open();
    if (geometry->size() < (polygon ? 3u : 2u)) return invalid("Add more distinct points before finishing");
    for (std::size_t i = 0; i < geometry->size(); ++i)
    {
        auto& p = (*geometry)[i];
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || std::abs(p.x()) > 180.0 || std::abs(p.y()) > 85.0)
            return invalid("Local overlay coordinates must be finite WGS84 positions between 85S and 85N");
        p.z() = 0.0;
        if (i && p == (*geometry)[i-1]) return invalid("Remove duplicate consecutive points");
    }
    const auto bounds = geometry->getBounds();
    if (bounds.xMax()-bounds.xMin() > 2.0 || bounds.yMax()-bounds.yMin() > 2.0)
        return invalid("Local edits are limited to two degrees and cannot cross the date line yet");
    if (polygon)
    {
        double area = 0.0;
        const auto n = geometry->size();
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto& a = (*geometry)[i]; const auto& b = (*geometry)[(i+1)%n];
            area += turn((*geometry)[0], a, b);
            for (std::size_t j = i+2; j < n; ++j)
                if ((j+1)%n != i && intersects(a,b,(*geometry)[j],(*geometry)[(j+1)%n]))
                    return invalid("Polygon edges cross or touch; redraw a simple outline");
        }
        if (std::abs(area) < 1e-14) return invalid("Polygon has no usable area");
        geometry->rewind(Geometry::ORIENTATION_CCW);
    }
    osg::ref_ptr<Feature> feature = new Feature(geometry, SpatialReference::get("wgs84"), Style(), id);
    feature->set("p2:type", typeName);
    for (const auto& attr : type->attributes) feature->set(attr.first, attr.second);
    // Populate the lazy extent before sharing this feature with concurrent paging workers.
    feature->getExtent();
    output = feature;
    return Status::NoError;
}

void FeatureOverlay::publish(std::shared_ptr<OverlaySnapshot> next)
{
    auto previous = snapshot();
    _undo.push_back(previous); if (_undo.size() > 32u) _undo.pop_front();
    _redo.clear(); next->revision = previous->revision+1;
    std::atomic_store(&_current, std::shared_ptr<const OverlaySnapshot>(std::move(next)));
}

Status FeatureOverlay::put(const std::string& type, const Geometry* geometry, const SpatialReference* srs, FeatureID& id)
{
    auto previous = snapshot();
    auto next = std::make_shared<OverlaySnapshot>(*previous);
    auto found = std::find_if(next->features.begin(), next->features.end(),
        [id](const osg::ref_ptr<const Feature>& f) { return f->getFID() == id; });
    if (id != 0 && found == next->features.end()) return invalid("Unknown overlay feature ID");
    if (id == 0 && next->features.size() >= maxFeatures) return invalid("Local overlay exceeds 4096 features");
    if (_nextID == std::numeric_limits<FeatureID>::max()) return invalid("Overlay feature IDs exhausted");
    osg::ref_ptr<const Feature> feature;
    const auto assigned = id == 0 ? _nextID : id;
    OE_RETURN_STATUS_ON_ERROR(prepare(type, geometry, srs, assigned, feature));
    if (found != next->features.end()) *found = feature;
    else next->features.push_back(feature);
    if (id == 0) ++_nextID;
    id = assigned; publish(next);
    return Status::NoError;
}

Status FeatureOverlay::erase(FeatureID id)
{
    auto next = std::make_shared<OverlaySnapshot>(*snapshot());
    auto found = std::find_if(next->features.begin(), next->features.end(),
        [id](const osg::ref_ptr<const Feature>& f) { return f->getFID() == id; });
    if (found == next->features.end()) return invalid("Unknown overlay feature ID");
    next->features.erase(found); publish(next);
    return Status::NoError;
}

Status FeatureOverlay::replace(const OverlaySnapshot& input)
{
    if (input.features.size() > maxFeatures) return invalid("Local overlay exceeds 4096 features");
    auto next = std::make_shared<OverlaySnapshot>();
    std::set<FeatureID> ids;
    auto nextID = _nextID;
    for (const auto& feature : input.features)
    {
        if (!feature || feature->getFID() <= 0 || feature->getFID() == std::numeric_limits<FeatureID>::max() ||
            !ids.insert(feature->getFID()).second) return invalid("Invalid or duplicate overlay feature ID");
        osg::ref_ptr<const Feature> prepared;
        OE_RETURN_STATUS_ON_ERROR(prepare(feature->getString("p2:type"), feature->getGeometry(),
            feature->getSRS(), feature->getFID(), prepared));
        // Do not silently accept hand-edited tags whose effects contradict the selected catalog type.
        if (feature->getAttrs().size() != prepared->getAttrs().size()) return invalid("Overlay attributes differ from catalog");
        for (const auto& attr : prepared->getAttrs())
            if (feature->getString(attr.first) != attr.second.getString())
                return invalid("Overlay attributes differ from catalog: " + attr.first);
        next->features.push_back(prepared);
        nextID = std::max(nextID, feature->getFID()+1);
    }
    _nextID = nextID; publish(next);
    return Status::NoError;
}

Status FeatureOverlay::undo()
{
    if (_undo.empty()) return invalid("Nothing to undo");
    auto previous = snapshot(); _redo.push_back(previous);
    auto next = std::make_shared<OverlaySnapshot>(*_undo.back()); _undo.pop_back();
    next->revision = previous->revision+1;
    std::atomic_store(&_current, std::shared_ptr<const OverlaySnapshot>(next));
    return Status::NoError;
}

Status FeatureOverlay::redo()
{
    if (_redo.empty()) return invalid("Nothing to redo");
    auto previous = snapshot(); _undo.push_back(previous);
    auto next = std::make_shared<OverlaySnapshot>(*_redo.back()); _redo.pop_back();
    next->revision = previous->revision+1;
    std::atomic_store(&_current, std::shared_ptr<const OverlaySnapshot>(next));
    return Status::NoError;
}

Status GeoJSONOverlayStorage::read(const std::string& path, OverlaySnapshot& output) const
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream || stream.tellg() < 0 || stream.tellg() > 16*1024*1024)
        return Status(Status::ResourceUnavailable, "Cannot read overlay, or file exceeds 16 MiB: " + path);
    stream.seekg(0);
    Json::Value root; Json::Reader reader;
    if (!reader.parse(stream, root) || !root.isObject() || !root["type"].isString() ||
        root["type"].asString() != "FeatureCollection" || !root["features"].isArray())
        return invalid("Expected a GeoJSON FeatureCollection");
    if (root["features"].size() > maxFeatures) return invalid("Local overlay exceeds 4096 features");
    OverlaySnapshot result;
    for (unsigned i = 0; i < root["features"].size(); ++i)
    {
        const auto& value = root["features"][i];
        if (!value.isObject() || !value["type"].isString() || value["type"].asString() != "Feature" ||
            !value["properties"].isObject() || !value["id"].isString() || !value["geometry"].isObject())
            return invalid("Overlay features need string IDs and catalog properties");
        FeatureID id = 0; std::istringstream input(value["id"].asString());
        if (!(input >> id) || !input.eof() || id <= 0) return invalid("Invalid overlay feature ID");
        auto geometry = GeometryUtils::geometryFromGeoJSON(Json::FastWriter().write(value["geometry"]));
        if (!geometry) return invalid("Invalid GeoJSON geometry");
        osg::ref_ptr<Feature> feature = new Feature(geometry, SpatialReference::get("wgs84"), Style(), id);
        for (const auto& name : value["properties"].getMemberNames())
        {
            if (!value["properties"][name].isString()) return invalid("Catalog properties must be strings");
            feature->set(name, value["properties"][name].asString());
        }
        result.features.push_back(feature);
    }
    output = std::move(result);
    return Status::NoError;
}

Status GeoJSONOverlayStorage::write(const std::string& path, const OverlaySnapshot& input) const
{
    if (path.empty() || input.features.size() > maxFeatures) return invalid("Invalid overlay path or feature count");
    Json::Value root(Json::objectValue); root["type"] = "FeatureCollection";
    root["features"] = Json::Value(Json::arrayValue);
    Json::Reader reader;
    for (const auto& feature : input.features)
    {
        if (!feature) return invalid("Null overlay feature");
        Json::Value value;
        if (!reader.parse(feature->getGeoJSON(), value)) return invalid("Cannot serialize overlay feature");
        // osgEarth stores open rings internally; GeoJSON requires the final coordinate to repeat the first.
        auto& geometry = value["geometry"];
        if (geometry["type"].asString() == "Polygon")
            for (unsigned i = 0; i < geometry["coordinates"].size(); ++i)
            {
                auto& ring = geometry["coordinates"][i];
                if (!ring.empty() && ring[0u] != ring[ring.size()-1u])
                { const auto first = ring[0u]; ring.append(first); }
            }
        value["id"] = std::to_string(feature->getFID()); root["features"].append(value);
    }
    // Keep the temporary on the same filesystem; unique names also isolate simultaneous application saves.
    const std::string temporary = path + "." + osgDB::getSimpleFileName(getTempName("p2-",".tmp"));
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return Status(Status::ResourceUnavailable, "Cannot write overlay: " + path);
    stream << Json::StyledWriter().write(root); stream.close();
    if (!stream || !replaceFile(temporary,path))
    {
        std::remove(temporary.c_str());
        return Status(Status::ResourceUnavailable, "Cannot finish overlay save: " + path);
    }
    return Status::NoError;
}

OverlayFeatureProvider::OverlayFeatureProvider(std::shared_ptr<const PlacementFeatureProvider> base,
    std::shared_ptr<const OverlaySnapshot> snapshot, const std::string& source) :
    _base(std::move(base)), _snapshot(std::move(snapshot)), _source(source) { }

Status OverlayFeatureProvider::query(const TileKey& key, double buffer, std::vector<PlacementFeature>& output,
    ProgressCallback* progress) const { return queryFiltered(key, buffer, {}, output, progress); }

Status OverlayFeatureProvider::queryFiltered(const TileKey& key, double buffer, const PlacementFeatureFilter& filter,
    std::vector<PlacementFeature>& output, ProgressCallback* progress) const
{
    output.clear();
    if (!key.valid() || !std::isfinite(buffer) || buffer < 0.0 || buffer > 100.0) return invalid("Invalid overlay query");
    std::vector<PlacementFeature> result;
    if (_base) OE_RETURN_STATUS_ON_ERROR(_base->queryFiltered(key, buffer, filter, result, progress));
    auto extent = key.getExtent();
    extent.expand(Distance(2.0*buffer, Units::METERS), Distance(2.0*buffer, Units::METERS));
    if (_snapshot) for (const auto& feature : _snapshot->features)
    {
        if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Overlay query canceled");
        if (feature->getExtent().intersects(extent) && (!filter || filter(_source,*feature)))
            result.push_back({_source,feature,{}});
    }
    output.swap(result);
    return Status::NoError;
}
