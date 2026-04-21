/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "BridgeLayer"
#include "RoadNetwork"

#include <osgEarth/GeometryCompiler>
#include <osgEarth/AltitudeFilter>
#include <osgEarth/TessellateOperator>
#include <osgEarth/FeatureStyleSorter>
#include <osgEarth/TerrainConstraintLayer>
#include <osgEarth/weemesh.h>
#include <osgEarth/Locators>

#include <unordered_set>

using namespace osgEarth;
using namespace osgEarth::Procedural;


#define LC "[BridgeLayer] "

#define OE_TEST OE_NULL

REGISTER_OSGEARTH_LAYER(bridges, BridgeLayer);

OSGEARTH_REGISTER_SIMPLE_SYMBOL_LAMBDA(
    bridge,
    [](const Config& c) { return new osgEarth::Procedural::BridgeSymbol(c); },
    [](const Config& c, class Style& s) { osgEarth::Procedural::BridgeSymbol::parseSLD(c, s); }
);

namespace
{
    //! Clamps all the endpoint junctions in the network and interpolates the midpoints
    //! based on their relation membership.
    inline void clampRoads(RoadNetwork& network, const Style& style, const GeoExtent& extent, ElevationPool* pool, ProgressCallback* prog)
    {
        ElevationPool::WorkingSet workingSet;
        GeoPoint p(extent.getSRS());

        Distance resolution(5.0, osgEarth::Units::METERS);
        const AltitudeSymbol* altitude = style.get<const AltitudeSymbol>();
        if (altitude)
        {
            resolution = *altitude->clampingResolution();
        }

        // First clamp all endpoint junctions. These are the ones where the bridge
        // touches the ground.
        std::vector<const RoadNetwork::Junction*> junctionEndPoints;
        std::vector< osg::Vec3d > junctionSamplePoints;

        for (auto& junction : network.junctions)
        {
            if (junction.is_endpoint())
            {
                p.x() = junction.x();
                p.y() = junction.y();
                junction._z = 10;
                junctionEndPoints.push_back(&junction);
                junctionSamplePoints.emplace_back(p.x(), p.y(), 0.0);
            }
        }

        // Convert all the junction points to the map SRS
        extent.getSRS()->transform(junctionSamplePoints, pool->getMapSRS());
        pool->sampleMapCoords(
            junctionSamplePoints.begin(),
            junctionSamplePoints.end(),
            resolution,
            &workingSet,
            prog
        );
        for (unsigned int i = 0; i < junctionEndPoints.size(); ++i)
        {
            if (junctionSamplePoints[i].z() != NO_DATA_VALUE)
            {
                junctionEndPoints[i]->_z = junctionSamplePoints[i].z();
            }
        }

        // Next, traverse all relations that start and end at endpoint junctions
        // (bridge touching the ground) and interpolate all midpoints junctions.
        for (auto& relation : network.relations)
        {
            auto* start = relation.ways.front()->start;
            auto* end = relation.ways.back()->end;

            if (start->is_endpoint() && end->is_endpoint())
            {
                double cummulative_length = 0.0;

                for(auto* way : relation.ways)
                {
                    cummulative_length += way->length;
                    if (way->end->is_midpoint())
                    {
                        way->end->_z = start->z() + (end->z() - start->z()) * (cummulative_length / relation.length);
                    }
                }
            }
        }

        // Next clamp all the midpoints that belong to a "floating" relation
        // (i.e., one where at least one terminating junction is not on the ground)
        for (auto& relation : network.relations)
        {
            auto* start = relation.ways.front()->start;
            auto* end = relation.ways.back()->end;

            if (start->is_midpoint() || end->is_midpoint())
            {
                double cummulative_length = 0.0;

                for (auto* way : relation.ways)
                {
                    cummulative_length += way->length;
                    if (way->end->is_midpoint() && way->end != end)
                    {
                        way->end->_z = start->z() + (end->z() - start->z()) * (cummulative_length / relation.length);
                    }
                }
            }
        }

        // Finally set the Z values for the actual feature geometry in each Way.
        for (auto& way : network.ways)
        {
            auto& start = way.start;
            auto& end = way.end;
            double delta = end->z() - start->z();
            for (auto& p : *way.geometry)
            {
                double t = distance2D(p, *start) / way.length;
                p.z() = start->z() + delta * t;
            }
        }
    }

    void extendEndpoints(RoadNetwork& network, const Distance& distance)
    {
        std::set<const RoadNetwork::Junction*> visited;

        for (auto& relation : network.relations)
        {
            for (auto* way : relation.ways)
            {
                if (way->start->is_endpoint() && visited.count(way->start) == 0)
                {
                    auto& p1 = way->geometry->at(1);
                    auto& p0 = way->geometry->at(0);
                    auto vec = p0 - p1;
                    vec.normalize();
                    p0 += vec * distance.as(Units::METERS);

                    RoadNetwork::Junction new_junction(p0, network.precision);
                    new_junction.ways = way->start->ways;
                    network.junctions.erase(*way->start);
                    auto iter = network.junctions.emplace(new_junction);
                    way->start = &*iter.first;

                    way->length += distance.as(Units::METERS);

                    visited.emplace(way->start); // mark as visited
                }

                if (way->end->is_endpoint() && visited.count(way->end) == 0)
                {
                    auto& p1 = way->geometry->at(way->geometry->size() - 2);
                    auto& p0 = way->geometry->at(way->geometry->size() - 1);
                    auto vec = p0 - p1;
                    vec.normalize();
                    p0 += vec * distance.as(Units::METERS);

                    RoadNetwork::Junction new_junction(p0, network.precision);
                    new_junction.ways = way->end->ways;
                    network.junctions.erase(*way->end);
                    auto iter = network.junctions.emplace(new_junction);
                    way->end = &*iter.first;

                    way->length += distance.as(Units::METERS);

                    visited.emplace(way->end);
                }
            }
        }
    }
}

//....................................................................

BridgeSymbol::BridgeSymbol(const Config& conf)
{
    mergeConfig(conf);
}

BridgeSymbol::BridgeSymbol(const BridgeSymbol& rhs, const osg::CopyOp& copyop) :
    Symbol(rhs, copyop),
    _deckSkin(rhs._deckSkin),
    _girderSkin(rhs._girderSkin),
    _railingSkin(rhs._railingSkin),
    _deckWidth(rhs._deckWidth),
    _girderHeight(rhs._girderHeight),
    _railingHeight(rhs._railingHeight),
    _spanLift(rhs._spanLift)
{
    //nop
}

Config
BridgeSymbol::getConfig() const
{
    Config conf = Symbol::getConfig();
    conf.set("deck_skin", deckSkin());
    conf.set("girder_skin", girderSkin());
    conf.set("railing_skin", railingSkin());
    conf.set("deck_width", deckWidth());
    conf.set("girder_height", girderHeight());
    conf.set("railing_height", railingHeight());
    conf.set("span_lift", spanLift());
    return conf;
}

void
BridgeSymbol::mergeConfig(const Config& conf)
{
    conf.get("deck_skin", deckSkin());
    conf.get("girder_skin", girderSkin());
    conf.get("railing_skin", railingSkin());
    conf.get("deck_width", deckWidth());
    conf.get("girder_height", girderHeight());
    conf.get("railing_height", railingHeight());
    conf.get("span_lift", spanLift());
}

void
BridgeSymbol::parseSLD(const Config& c, Style& style)
{
    if (match(c.key(), "library") && !c.value().empty())
        style.getOrCreate<BridgeSymbol>()->library() = Strings::unquote(c.value());
    else if (match(c.key(), "bridge-deck-skin"))
        style.getOrCreate<BridgeSymbol>()->deckSkin() = URI(Strings::unquote(c.value()), c.referrer());
    else if (match(c.key(), "bridge-girder-skin"))
        style.getOrCreate<BridgeSymbol>()->girderSkin() = URI(Strings::unquote(c.value()), c.referrer());
    else if (match(c.key(), "bridge-railing-skin"))
        style.getOrCreate<BridgeSymbol>()->railingSkin() = URI(Strings::unquote(c.value()), c.referrer());
    else if (match(c.key(), "bridge-deck-width") || match(c.key(), "bridge-width")) {
        style.getOrCreate<BridgeSymbol>()->deckWidth() = c.value();
        style.getOrCreate<BridgeSymbol>()->deckWidth()->setDefaultUnits(Units::METERS);
    }
    else if (match(c.key(), "bridge-girder-height")) {
        style.getOrCreate<BridgeSymbol>()->girderHeight() = Distance(c.value(), Units::METERS);
    }
    else if (match(c.key(), "bridge-railing-height")) {
        style.getOrCreate<BridgeSymbol>()->railingHeight() = Distance(c.value(), Units::METERS);
    }
    else if (match(c.key(), "bridge-span-lift")) {
        style.getOrCreate<BridgeSymbol>()->spanLift() = Distance(c.value(), Units::METERS);
    }
}

//....................................................................

void
BridgeLayer::Options::fromConfig(const Config& conf)
{
    conf.get("constraint_min_level", constraintMinLevel());
    conf.get("span_extend", spanExtend());
}

Config
BridgeLayer::Options::getConfig() const
{
    auto conf = super::getConfig();
    conf.set("constraint_min_level", constraintMinLevel());
    conf.set("span_extend", spanExtend());
    return conf;
}

//....................................................................

void
BridgeLayer::init()
{
    super::init();

    // some reasonable defaults
    options().minLevel().setDefault(14u);
    options().maxLevel().setDefault(14u);
    options().additive().setDefault(false);
}

void
BridgeLayer::addedToMap(const Map* map)
{
    super::addedToMap(map);

    // This will cause lines to join across the entire tile dataset.
    // The side-effect is that it will discard some features, losing their attributes.
    // Instead by default we join lines after they've been style-sorted.
    //_filters.emplace_back(new JoinLinesFilter());

    // create a terrain constraint layer that will 'clamp' the terrain to the end caps of our bridges.
    if (options().constraintMinLevel() > 0 && options().constraintMinLevel() <= 21)
    {
        auto* c = new TerrainConstraintLayer();
        c->setName(getName() + "_constraints");
        c->setRemoveInterior(false);
        c->setRemoveExterior(false);
        c->setHasElevation(true);
        c->setMinLevel(std::max(options().constraintMinLevel().value(), getMinLevel()));

        c->constraintCallback([this](const TileKey& key, MeshConstraint& result, FilterContext*, ProgressCallback* progress)
            {
                this->addConstraint(key, result, progress);
            });

        auto status = c->open(getReadOptions());
        if (!status.isOK())
        {
            OE_WARN << LC << "Failed to open constraint layer: " << status.message() << std::endl;
            return;
        }

        Map* mutableMap = const_cast<Map*>(map);
        mutableMap->addLayer(c);

        _constraintLayer = c;
    }
}

void
BridgeLayer::removedFromMap(const Map* map)
{
    if (_constraintLayer.valid())
    {
        Map* mutableMap = const_cast<Map*>(map);
        mutableMap->removeLayer(_constraintLayer.get());
        _constraintLayer = nullptr;
    }

    super::removedFromMap(map);    
}

namespace
{
    Geometry* line_to_polygon(const Geometry* geom, double width)
    {
        MultiGeometry* mg = new MultiGeometry();
        double halfWidth = 0.5 * width;

        ConstGeometryIterator i(geom, false);
        while (i.hasMore())
        {
            auto* line = i.next();
            if (line->size() < 2)
                continue;

            osgEarth::Polygon* poly = new osgEarth::Polygon();
            poly->resize(line->size() * 2);

            for (int i = 0; i < line->size(); ++i)
            {
                osg::Vec3d dir;
                if (i == 0)
                    dir = (*line)[i + 1] - (*line)[i];
                else if (i == line->size() - 1)
                    dir = (*line)[i] - (*line)[i - 1];
                else
                    dir = (*line)[i + 1] - (*line)[i - 1];
                dir.normalize();

                osg::Vec3d right = dir ^ osg::Vec3d(0, 0, 1);

                (*poly)[i] = (*line)[i] + right * halfWidth;
                (*poly)[line->size() * 2 - i - 1] = (*line)[i] - right * halfWidth;
            }

            mg->add(poly);
        }

        return mg;
    }

    Geometry* line_to_offset_curves(const Geometry* geom, double width, bool double_sided)
    {
        MultiGeometry* mg = new MultiGeometry();
        double halfWidth = 0.5 * width;

        ConstGeometryIterator i(geom, false);
        while (i.hasMore())
        {
            auto* line = i.next();
            if (line->size() < 2)
                continue;

            LineString* left = new LineString();
            left->resize(line->size());

            LineString* right = new LineString();
            right->resize(line->size());

            for (int i = 0; i < line->size(); ++i)
            {
                osg::Vec3d dir;
                if (i == 0)
                    dir = (*line)[i + 1] - (*line)[i];
                else if (i == line->size() - 1)
                    dir = (*line)[i] - (*line)[i - 1];
                else
                    dir = (*line)[i + 1] - (*line)[i - 1];

                osg::Vec3d right_vec = dir ^ osg::Vec3d(0, 0, 1);

                right_vec.normalize();

                (*right)[i] = (*line)[i] + right_vec * halfWidth;
                (*left)[i] = (*line)[i] - right_vec * halfWidth;
            }

            mg->add(left);
            mg->add(right);

            if (double_sided)
            {
                auto* left_rev = new LineString(*left);
                std::reverse(left_rev->begin(), left_rev->end());
                mg->add(left_rev);

                auto* right_rev = new LineString(*right);
                std::reverse(right_rev->begin(), right_rev->end());
                mg->add(right_rev);
            }
        }
        return mg;
    }

    Geometry* line_to_end_caps(const Geometry* geom, double width)
    {
        auto mg = new MultiGeometry();
        double halfWidth = 0.5 * width;

        ConstGeometryIterator i(geom, false);
        while (i.hasMore())
        {
            auto* line = i.next();
            if (line->size() < 2)
                continue;

            LineString* start = new LineString();
            start->resize(2);

            LineString* end = new LineString();
            end->resize(2);

            auto dir0 = (*line)[1] - (*line)[0];
            auto right0 = dir0 ^ osg::Vec3d(0, 0, 1);
            right0.normalize();
            (*start)[0] = (*line)[0] + right0 * halfWidth;
            (*start)[1] = (*line)[0] - right0 * halfWidth;

            auto dir1 = (*line)[line->size() - 1] - (*line)[line->size() - 2];
            auto right1 = dir1 ^ osg::Vec3d(0, 0, 1);
            right1.normalize();
            (*end)[0] = (*line)[line->size() - 1] + right1 * halfWidth;
            (*end)[1] = (*line)[line->size() - 1] - right1 * halfWidth;

            mg->add(start);
            mg->add(end);
        }
        return mg;
    }

    struct WayGeometry
    {
        RoadNetwork::Way* way = nullptr;
        osg::ref_ptr<Geometry> polygon;
        std::vector<weemesh::triangle_t*> triangles;
        std::unordered_map<unsigned, osg::Vec2f> uvs; // UV coordinates, indexed by the mesh_t vertex index
    };

    struct RelationGeometry
    {
        const RoadNetwork::Relation* relation = nullptr;
        std::vector<WayGeometry> wayGeometries;
    };

    struct NetworkGeometry
    {
        GeoExtent extent;
        weemesh::mesh_t mesh;
        std::vector<RelationGeometry> relationGeometries;
    };

#if 0
    void calculateRelationCenterline(const RoadNetwork& network, NetworkGeometry& network_geom)
    {
        for (auto& relation_geom : network_geom)
        {
            auto* relation = relation_geom.first;
            auto& relation_centerline = relation_geom.second.relation_centerline;
            relation_centerline = new LineString();

            int way_num = 0;
            for (auto* way : relation->ways)
            {
                for(unsigned i = (way_num == 0)? 0 : 1; i< way->geometry->size(); ++i)
                {
                    relation_centerline->push_back((*way->geometry)[i]);
                }
                ++way_num;
            }
        }
    }
#endif

    void buildNetworkData(const RoadNetwork& network, NetworkGeometry& network_geom, const Style& style, FilterContext& context)
    {
        network_geom.mesh._epsilon = 1e-3;

        auto* bridge = style.get<BridgeSymbol>();
        OE_SOFT_ASSERT_AND_RETURN(bridge, void());

        for (auto& relation : network.relations)
        {
            network_geom.relationGeometries.emplace_back();
            auto& relation_geom = network_geom.relationGeometries.back();
            relation_geom.relation = &relation;

            for (auto* way : relation.ways)
            {
                auto* geom = way->geometry;
                if (geom)
                {
                    auto width = bridge->deckWidth()->eval(way->feature, context).as(Units::METERS);
                    auto* poly = line_to_polygon(geom, width);
                    if (poly)
                    {
                        WayGeometry way_geom;
                        way_geom.way = way;
                        way_geom.polygon = poly;
                        relation_geom.wayGeometries.emplace_back(std::move(way_geom));
                    }
                }
            }
        }
    }

    void buildMeshFromNetworkGeometry(NetworkGeometry& network_geom)
    {
        OE_SOFT_ASSERT_AND_RETURN(network_geom.extent.isValid(), void());

        //mesh.set_boundary_marker(VERTEX_BOUNDARY);
        //mesh.set_constraint_marker(VERTEX_CONSTRAINT);
        //mesh.set_has_elevation_marker(VERTEX_HAS_ELEVATION);

        const unsigned tileSize = 65; //TODO
        network_geom.mesh.verts.reserve(tileSize * tileSize);

        GeoLocator locator(network_geom.extent);

        for (unsigned row = 0; row < tileSize; ++row)
        {
            double ny = (double)row / (double)(tileSize - 1);
            for (unsigned col = 0; col < tileSize; ++col)
            {
                double nx = (double)col / (double)(tileSize - 1);
                osg::Vec3d unit(nx, ny, 0.0);
                osg::Vec3d world;

                locator.unitToWorld(unit, world);

                int marker = 0;

                int i = network_geom.mesh.get_or_create_vertex(
                    weemesh::vert_t(world.x(), world.y(), world.z()),
                    marker);

                if (row > 0 && col > 0)
                {
                    network_geom.mesh.add_triangle(i, i - 1, i - tileSize - 1);
                    network_geom.mesh.add_triangle(i, i - tileSize - 1, i - tileSize);
                }
            }
        }
    }

    void addNetworkGeometryToMesh(NetworkGeometry& network_geom)
    {
        const int marker = 0;

        for (auto& relation_geom : network_geom.relationGeometries)
        {
            for(auto& way_geom : relation_geom.wayGeometries)
            {
                ConstGeometryIterator gi(way_geom.polygon.get());
                while (gi.hasMore())
                {
                    ConstSegmentIterator si(gi.next(), true);
                    while (si.hasMore())
                    {
                        auto& segment = si.next();
                        network_geom.mesh.insert({
                            {segment.first.x(), segment.first.y(), segment.first.z()},
                            {segment.second.x(), segment.second.y(), segment.second.z()} }, marker);
                    }
                }
            }
        }
    }

    void sortTrianglesIntoWayGeometries(NetworkGeometry& network_geom)
    {
        std::unordered_set<weemesh::UID> consumed;

        for (auto& relation_geom : network_geom.relationGeometries)
        {
            for (auto& way_geom : relation_geom.wayGeometries)
            {
                for (auto& tri_iter : network_geom.mesh.triangles)
                {
                    auto& uid = tri_iter.first;
                    auto& tri = tri_iter.second;

                    if (way_geom.polygon->contains2D(tri.centroid.x, tri.centroid.y) && consumed.count(uid) == 0)
                    {
                        way_geom.triangles.emplace_back(&tri);
                        consumed.insert(uid);
                    }
                }

                //OE_INFO << "Way " << (std::uintptr_t)&way_geom << " contains " << way_geom.triangles.size() << " triangles!" << std::endl;
            }
        }
    }

    void generateUVsForWayGeometries(NetworkGeometry& network_geom)
    {
        const double texture_width = 8.0; // TODO
        const double texture_length = 8.0; // TODO

        for (auto& relation_geom : network_geom.relationGeometries)
        {
            for (auto& way_geom : relation_geom.wayGeometries)
            {
                if (way_geom.triangles.size() == 0)
                    continue;

                auto* way_centerline = way_geom.way->geometry;

                // collect unique verts.
                // it might be faster to just run through them all including the duplicates,
                // benchmark this. TODO
                std::unordered_set<unsigned> vert_indices_unique;

                for (auto* tri : way_geom.triangles)
                {
                    vert_indices_unique.insert(tri->i0);
                    vert_indices_unique.insert(tri->i1);
                    vert_indices_unique.insert(tri->i2);
                }

                //auto wkt = GeometryUtils::geometryToWKT(way_centerline);
                //OE_INFO << "\nCenterline = " << wkt << std::endl;
                
                for(auto vert_index : vert_indices_unique)
                {
                    auto& vert = network_geom.mesh.verts[vert_index];
                    osg::Vec3d p(vert.x, vert.y, vert.z);
                    double cummulative_len = 0.0;

                    //OE_INFO << "  vert = " << std::setprecision(10) << p.x() << ", " << p.y() << std::endl;

                    for (unsigned i = 0; i < way_centerline->size() - 1; ++i)
                    {
                        auto& a = (*way_centerline)[i];
                        auto& b = (*way_centerline)[i + 1];

                        // project the point (in 2D) onto the segment at parameter t.
                        osg::Vec2d ab(b.x() - a.x(), b.y() - a.y());
                        osg::Vec2d ap(p.x() - a.x(), p.y() - a.y());
                        double t = (ap * ab) / (ab * ab);

                        if (equivalent(t, 0.0)) t = 0.0;
                        else if (equivalent(t, 1.0)) t = 1.0;

                        auto segment_len = sqrt(ab * ab);

                        //OE_INFO << "  ...t = " << t << std::endl;

                        // is t on the segment?
                        if (t == clamp(t, 0.0, 1.0))
                        {
                            auto partial_len = segment_len * t;

                            float v = (cummulative_len + partial_len) / texture_length;

                            auto p_projected = a + (b - a) * t;
                            auto distance = (p - p_projected).length();
                            float u = clamp(distance / texture_width, 0.0, 1.0);

                            way_geom.uvs.emplace(vert_index, osg::Vec2f{ u, v });

                            // inherit the Z value as well.
                            network_geom.mesh.get_vertex(vert_index).z = a.z() + (b.z() - a.z()) * t;

                            //OE_INFO << "OK!!!!, p=" << p.x() << ", " << p.y() << ": t = " << t << std::endl;

                            break;
                        }
                        else
                        {
                            //OE_INFO << "failed, p=" << p.x() << ", " << p.y() << ": t = " << t << std::endl;
                        }

                        cummulative_len += segment_len;
                    }
                }
            }
        }
    }

    osg::Node* createDrawablesFromWayGeometries(const NetworkGeometry& network_geom)
    {
        OE_SOFT_ASSERT_AND_RETURN(network_geom.extent.isValid(), nullptr);

        auto& mesh = network_geom.mesh;

        auto worldSRS = SpatialReference::get("wgs84")->getGeocentricSRS();
        osg::Matrix local2world, world2local;
        auto centroid = network_geom.extent.getCentroid();

        auto centroidWorld = centroid.transform(worldSRS);
        centroidWorld.createLocalToWorld(local2world);
        world2local.invert(local2world);

        osg::MatrixTransform* group = nullptr;

        for (auto& relation_geom : network_geom.relationGeometries)
        {
            for (auto& way_geom : relation_geom.wayGeometries)
            {
                auto num_verts = way_geom.uvs.size();
                if (num_verts < 3)
                    continue;

                osg::Geometry* geom = new osg::Geometry();
                geom->setUseVertexBufferObjects(true);
                geom->setUseDisplayList(false);

                auto verts = new osg::Vec3Array();
                verts->reserve(num_verts);
                geom->setVertexArray(verts);

                auto colors = new osg::Vec4Array(osg::Array::BIND_OVERALL, 1);
                (*colors)[0] = osg::Vec4(1, 1, 1, 1);
                geom->setColorArray(colors);

                auto normals = new osg::Vec3Array(osg::Array::BIND_OVERALL, 1);
                (*normals)[0] = osg::Vec3(0, 0, 1);
                geom->setNormalArray(normals);

                auto uvs = new osg::Vec2Array(osg::Array::BIND_PER_VERTEX);
                uvs->reserve(num_verts);
                geom->setTexCoordArray(0, uvs);

                auto elements = new osg::DrawElementsUShort(GL_TRIANGLES);
                elements->reserve(way_geom.triangles.size() * 3);
                geom->addPrimitiveSet(elements);

                std::unordered_map<unsigned, unsigned> vert_index_to_local_index;
                
                for (auto& uv_iter : way_geom.uvs)
                {
                    auto index = uv_iter.first;
                    auto& uv = uv_iter.second;

                    // map the index in the mesh to the index in the geometry we are creating
                    vert_index_to_local_index[index] = verts->size();

                    osg::Vec3d model(mesh.verts[index].x, mesh.verts[index].y, mesh.verts[index].z);

                    GeoPoint m(centroid.getSRS(), model);
                    m.transformInPlace(worldSRS);
                    model = m.vec3d() * world2local;
                    verts->push_back(model);

                    uvs->push_back(uv);
                }

                for (auto* tri : way_geom.triangles)
                {
                    elements->push_back(vert_index_to_local_index[tri->i0]);
                    elements->push_back(vert_index_to_local_index[tri->i1]);
                    elements->push_back(vert_index_to_local_index[tri->i2]);
                }

                if (!group)
                    group = new osg::MatrixTransform(local2world);

                group->addChild(geom);
            }
        }

        return group;
    }



    void addConstraints(const FeatureList& c_features, const Style in_style, FilterContext& context, MeshConstraint& constriant)
    {
        Style style(in_style);

        auto* bridge = style.get<BridgeSymbol>();
        OE_SOFT_ASSERT_AND_RETURN(bridge, void());

        osg::ref_ptr<const SpatialReference> localSRS = context.workingExtent()->getSRS()->createTangentPlaneSRS(
            context.workingExtent()->getCentroid().vec3d());

        // convert our lines to polygons.
        // TODO: handle multis
        for (auto& feature : c_features)
        {
            osg::ref_ptr<Feature> f = new Feature(*feature);
            f->transform(localSRS);

            auto deckWidth = bridge->deckWidth()->eval(f, context);

            // just a reminder to implement multi-geometries if necessary
            auto* geom = line_to_end_caps(f->getGeometry(), deckWidth.as(Units::METERS));
            if (geom)
            {
                f->setGeometry(geom);
                f->transform(feature->getSRS());
                constriant.features.emplace_back(f);
            }
        }
    }

    osg::ref_ptr<osg::Node> createDeck(const FeatureList& c_features, const Style& in_style, FilterContext& context)
    {
        Style style(in_style);

        auto* bridge = style.get<BridgeSymbol>();
        OE_SOFT_ASSERT_AND_RETURN(bridge, {});

        auto* line = style.getOrCreate<LineSymbol>();
        line->library() = bridge->library();
        line->uriContext() = bridge->uriContext();
        line->stroke()->color() = Color::White;
        line->stroke()->width() = bridge->deckWidth();
        line->stroke()->width()->setDefaultUnits(Units::METERS);
        line->imageURI() = bridge->deckSkin();

        auto* render = style.get<RenderSymbol>();
        if (render == nullptr || render->backfaceCulling() == true)
        {
            // backface culling on? generate double-sided polygons.
            line->doubleSided() = true;
        }
        else
        {
            // backface culling off? only generate one-sided polygons
            line->doubleSided() = false;
        }

        // clone the features
        FeatureList features;
        features.reserve(c_features.size());
        std::transform(c_features.begin(), c_features.end(), std::back_inserter(features), [](Feature* f) {
            return new Feature(*f); });

        auto node = GeometryCompiler().compile(features, style, context);
        return node;
    }

    osg::ref_ptr<osg::Node> createGirders(const FeatureList& c_features, const Style& in_style, FilterContext& context)
    {
        Style style(in_style);

        auto* bridge = style.get<BridgeSymbol>();
        OE_SOFT_ASSERT_AND_RETURN(bridge, {});

        Distance girderHeight = bridge->girderHeight().value();
        if (girderHeight.getValue() <= 0.0)
            return {};

        osg::ref_ptr<const SpatialReference> localSRS = context.workingExtent()->getSRS()->createTangentPlaneSRS(
            context.workingExtent()->getCentroid().vec3d());

        // convert our lines to polygons.
        // TODO: handle multis
        FeatureList features;
        for (auto& feature : c_features)
        {
            osg::ref_ptr<Feature> f = new Feature(*feature);
            f->transform(localSRS);

            auto deckWidth = bridge->deckWidth()->eval(feature, context);

            auto* geom = line_to_polygon(f->getGeometry(), deckWidth.as(Units::METERS) * 0.9);
            if (geom)
            {
                f->setGeometry(geom);
                f->transform(feature->getSRS());
                features.emplace_back(f);
            }
        }

        auto* extrude = style.getOrCreate<ExtrusionSymbol>();
        extrude->uriContext() = bridge->uriContext();
        extrude->library() = bridge->library();
        extrude->height() = girderHeight;
        extrude->direction() = extrude->DIRECTION_DOWN;
        extrude->flatten() = false;
        extrude->wallSkinName() = bridge->girderSkin()->base();
        extrude->roofSkinName() = bridge->girderSkin()->base();

        auto* render = style.getOrCreate<RenderSymbol>();
        render->backfaceCulling() = false;

        auto node = GeometryCompiler().compile(features, style, context);
        return node;
    }

    osg::ref_ptr<osg::Node> createRailings(const FeatureList& c_features, const Style& in_style, FilterContext& context)
    {
        OE_SOFT_ASSERT_AND_RETURN(context.workingExtent()->isValid(), {});

        Style style(in_style);

        auto* bridge = style.get<BridgeSymbol>();
        if (!bridge)
            return {};

        Distance railingHeight = bridge->railingHeight().value();
        if (railingHeight.getValue() <= 0.0)
            return {};

        osg::ref_ptr<const SpatialReference> localSRS = context.workingExtent()->getSRS()->createTangentPlaneSRS(
            context.workingExtent()->getCentroid().vec3d());

        bool double_sided_polys = true;
        auto* render = style.get<RenderSymbol>();

        if (render == nullptr || render->backfaceCulling() == true)
        {
            // backface culling on? generate double-sided polygons.
            double_sided_polys = true;
        }
        else
        {
            // backface culling off? only generate one-sided polygons
            double_sided_polys = false;
        }

        // convert our lines to offset lines.
        // TODO: handle multis
        FeatureList features;
        for (auto& feature : c_features)
        {
            osg::ref_ptr<Feature> f = new Feature(*feature);
            f->transform(localSRS);

            auto deckWidth = bridge->deckWidth()->eval(feature, context);

            auto* geom = line_to_offset_curves(f->getGeometry(), deckWidth.as(Units::METERS), double_sided_polys);
            if (geom)
            {
                f->setGeometry(geom);
                f->transform(feature->getSRS());
                features.emplace_back(f);
            }
        }

        auto* extrude = style.getOrCreate<ExtrusionSymbol>();
        extrude->uriContext() = bridge->uriContext();
        extrude->library() = bridge->library();
        extrude->height() = railingHeight;
        extrude->flatten() = false;
        extrude->wallSkinName() = bridge->railingSkin()->base();

        return GeometryCompiler().compile(features, style, context);
    }
}

osg::ref_ptr<osg::Node>
BridgeLayer::createTileImplementation(const TileKey& key, ProgressCallback* progress) const
{
    if (progress && progress->isCanceled())
        return nullptr;

    if (!getStatus().isOK() || !getFeatureSource() || !getFeatureProfile())
        return nullptr;

    osg::ref_ptr<FeatureSourceIndexNode> index;
    if (_featureIndex.valid())
    {
        index = new FeatureSourceIndexNode(_featureIndex.get());
    }

    // Assemble the network.
    RoadNetwork network;

    if (getFeatureProfile()->getSRS()->isGeographic())
        network.precision = 7; // 0.01 meters precision at the equator
    else
        network.precision = 3; // 0.001m precision

    // Functor decides which outgoing way to traverse when building a relation.
    network.nextWayInRelation = [&](const RoadNetwork::Junction& junction, RoadNetwork::Way* incoming, const std::vector<RoadNetwork::Way*>& exclusions)
        -> RoadNetwork::Way*
        {
            auto fid = incoming->feature->getFID();
            auto highway = incoming->feature->getString("highway");
            auto layer = incoming->feature->getInt("layer", -1);
            auto lanes = incoming->feature->getInt("lanes", -1);
            auto width = incoming->feature->getDouble("width", -1.0);

            RoadNetwork::Way* best = nullptr;
            int best_score = 0;

            for(auto* candidate : junction.ways)
            {
                if (std::find(exclusions.begin(), exclusions.end(), candidate) == exclusions.end())
                {
                    // matching non-zero fids? same feature, different tile.
                    if (fid != 00L && candidate->feature->getFID() == fid)
                        return candidate;

                    int score = 0;
                    if (candidate->feature->getInt("layer", -1) == layer)
                        score++;
                    if (candidate->feature->getString("highway") == highway)
                        score++;
                    if (candidate->feature->getInt("lanes", -1) == lanes)
                        score++;
                    if (candidate->feature->getDouble("width", -1.0) == width)
                        score++;

                    if (score > best_score)
                    {
                        best = candidate;
                        best_score = score;
                    }
                }                
            }

            return best;
        };

    // Functor decides whether two features are compatible enough to merge into one.
    network.canMerge = [&](const Feature* a, const Feature* b) -> bool
        {
            if ((a == b) || (a == nullptr) || (b == nullptr))
                return false;

            if (a->getString("highway") != b->getString("highway"))
                return false;

            if (a->getString("layer") != b->getString("layer"))
                return false;

            return true;
        };

    std::unordered_set<std::int64_t> featuresInExtent;

    // This functor assembles the Network and clamps our road data.
    auto preprocess = [&](FeatureList& features, ProgressCallback* progress)
        {
            if (!_session.valid())
                return;

            auto map = _session->getMap();
            if (!map || !map->getElevationPool())
                return;

            if (progress && progress->isCanceled())
                return;

            // build the network:
            for (auto& feature : features)
            {
                network.addFeature(feature);
            }

            // figure out which edges go together:
            network.buildRelations();

            if (options().spanExtend().isSet())
            {
                extendEndpoints(network, options().spanExtend().value());
            }

            // clamp ground-connection points to the terrain and interpolate midpoints:
           
            // cull out any relations whose center point is not in the current tile:
            network.getFeatures(key.getExtent(), featuresInExtent);

            // BTW: if you reenable this, it will BREAK the front/back endpoint map.
#if 0       // merge is a bit busted ... but do we really need it?
            FeatureList merged;
            network.merge_relations(merged);
            if (!merged.empty())
            {
                features.swap(merged);
            }
#endif
        };


    osg::ref_ptr<osg::Group> tileGroup;

    auto run = [&](const Style& in_style, FeatureList& features, ProgressCallback* progress)
        {    
            FilterContext context(_session.get(), key.getExtent(), index);

            // remove any features not culled to the extent:
            FeatureList passed;
            for (auto& feature : features)
                if (featuresInExtent.count(feature->getFID()) > 0)
                    passed.emplace_back(feature);
            features.swap(passed);

            if (features.empty())
                return;

            if (progress && progress->isCanceled())
                return;

            //OE_NOTICE << "Style name " << in_style.getName() << std::endl;

            // clamp ground-connection points to the terrain and interpolate midpoints:
            clampRoads(network, in_style, key.getExtent(), _session->getMap()->getElevationPool(), progress);

            // tessellate the lines:
            TessellateOperator filter;
            filter.setMaxPartitionSize(Distance(10, Units::METERS));
            context = filter.push(features, context);

            // wee hack to elevate the ends of the bridge just a bit.
            auto* bridge = in_style.get<BridgeSymbol>();
            Distance lift = bridge ? bridge->spanLift().value() : Distance(0.5, Units::METERS);
            double lift_m = lift.as(Units::METERS);
            for (auto& f : features)
            {
                auto* geom = f->getGeometry();
                GeometryIterator iter(geom, true);

                auto& is_endpoint = network.geometryEndpointFlags[geom];
                int start = is_endpoint.first ? 1 : 0;
                int end = is_endpoint.second ? 1 : 0;

                iter.forEach([&](auto* geom)
                    {
                        for (int i = start; i < geom->size() - end; ++i) {
                            (*geom)[i].z() += lift_m;
                        }
                    });
            }

            Style style(in_style);

            auto deck = createDeck(features, style, context);
            auto girders = createGirders(features, style, context);
            auto railings = createRailings(features, style, context);

            osg::Group* styleGroup = nullptr;

            if (deck.valid() || girders.valid() || railings.valid())
                styleGroup = new StyleGroup(style);

            if (deck.valid())
                styleGroup->addChild(deck.get());

            if (girders.valid())
                styleGroup->addChild(girders.get());

            if (railings.valid())
                styleGroup->addChild(railings.get());

            if (styleGroup)
            {
                if (!tileGroup.valid())
                {
                    tileGroup = new osg::Group();
                }

                tileGroup->addChild(styleGroup);
            }
        };

    Distance buffer(10.0, Units::METERS);
    FeatureStyleSorter().sort(key, buffer, _session.get(), _filters, preprocess, run, progress);

    if (index.valid())
    {
        index->addChild(tileGroup);
        tileGroup = index;
    }

    ShaderGenerator gen;
    gen.run(tileGroup.get());

    return tileGroup;
}

void
BridgeLayer::addConstraint(const TileKey& key, MeshConstraint& result, ProgressCallback* progress) const
{
    auto run = [&](const Style& in_style, FeatureList& features, ProgressCallback* progress)
        {
            FilterContext context(_session.get(), key.getExtent());

            // clamp the lines:
            AltitudeFilter clamper;
            auto* alt = clamper.getOrCreateSymbol();
            alt->clamping() = alt->CLAMP_TO_TERRAIN;
            alt->binding() = alt->BINDING_ENDPOINT;
            context = clamper.push(features, context);

            addConstraints(features, in_style, context, result);
        };

    FeatureStyleSorter().sort(key, Distance{}, _session.get(), _filters, nullptr, run, progress);
}
