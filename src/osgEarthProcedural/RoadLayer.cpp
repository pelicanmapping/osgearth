#include "RoadLayer"
#include <osgEarth/SimplePager>
#include <osgEarth/CropFilter>
#include <osgEarth/rtree.h>
#include <osgEarth/FeatureSDFLayer>
#include <osgEarth/TiledModelLayer>
#include <osgEarth/TerrainConstraintLayer>
#include <osgEarth/TerrainEngineNode>
#include <osgEarth/TerrainResources>
#include <osgEarth/ShaderLoader>
#include <osgEarth/Shaders>
#include <osgEarth/ResampleFilter>
#include <osgEarth/TextureArena>
#include <osgEarth/Math>
#include <osgDB/ReadFile>
#include <osgUtil/SmoothingVisitor>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/PolygonOffset>
#include <array>

using namespace osgEarth;
using namespace osgEarth::Procedural;

REGISTER_OSGEARTH_LAYER(roads, RoadLayer);

RoadLayerArt::RoadLayerArt(const Config& conf)
{
    conf.child("substrate").get("material", substrateMaterial());
    conf.child("substrate").get("buffer", substrateBuffer());
    conf.child("lanes").get("material", lanesMaterial());
    conf.child("crossings").get("material", crossingsMaterial());
    conf.child("intersections").get("material", intersectionsMaterial());
}

Config
RoadLayerArt::getConfig() const
{
    Config conf("art");
    Config& sub = conf.add("substrate");
    sub.set("material", substrateMaterial());
    sub.set("buffer", substrateBuffer());
    Config& lanes = conf.add("lanes");
    lanes.set("material", lanesMaterial());
    Config& crossings = conf.add("crossings");
    crossings.set("material", crossingsMaterial());
    Config& intersections = conf.add("intersections");
    intersections.set("material", intersectionsMaterial());
    return conf;
}


namespace
{
    Distance LANE_WIDTH(10.0f, Units::FEET);

    Distance DEFAULT_ROAD_WIDTH = LANE_WIDTH * 2.0;

    using index_t = uint32_t;
    using rank_t = uint8_t;
    using point_t = osg::Vec3d;
    using vec_t = osg::Vec3d;
    using FID = uint8_t;
    std::atomic<uint32_t> s_uidgen;
    const double EPSILON = 1.0; // meters

    vec_t cross(const vec_t& a, const vec_t& b)
    {
        return a ^ b;
    }

    double cross_2d(const vec_t& a, const vec_t& b)
    {
        return a.x() * b.y() - b.x() * a.y();
    }

    double length_squared_2d(const vec_t& v)
    {
        return v.x() * v.x() + v.y() * v.y();
    }

    vec_t normalize(const vec_t& v1)
    {
        double L = v1.length();
        return L > 0.0 ? v1 / L : v1;
    }

    double dot(const vec_t& v1, const vec_t& v2)
    {
        return v1.x() * v2.x() + v1.y() * v2.y() + v1.z() * v2.z();
    }

    std::string str(const vec_t& v)
    {
        return std::to_string(v.x()) + "," + std::to_string(v.y()) + "," + std::to_string(v.z());
    }

    
    inline std::int64_t hashed_value(double a, double mult)
    {
        auto i = (std::int64_t)(a * 1000.0);
        auto m = (std::int64_t)(mult * 1000.0);
        return ((i + m - 1) / m) * m;
    }

    struct edge_t;

    struct part_art_t
    {
        osg::ref_ptr<osg::StateSet> stateset;
        float width = 1.0f; // m
        float length = 1.0f; // m
        bool tiled_along_width = true;
        bool tiled_along_length = true;
    };

    struct properties_t
    {
        std::string kind;
        part_art_t surface;
        part_art_t lanes;
        part_art_t crossing;
        part_art_t intersection;
        Distance default_width = Distance(20, Units::FEET);
        //float default_width = 5.0f;
        rank_t rank = 0;
        uint8_t num_lanes = 2;
    };

    struct intersection_t
    {
        enum Type { NONE, GENERIC, MERGE, T, Y, X } type = NONE;
        float backoff_length = 0.0f;
        vec_t orientation = vec_t(1, 0, 0);
    };

    struct node_t
    {
        const UID uid;
        point_t p;
        mutable std::vector<edge_t*> edges;
        intersection_t intersection;
        bool has_crossing = false;
        int subgraph_id = -1;
        const properties_t* props = nullptr;

        node_t() : uid(s_uidgen++) {
        }
        node_t(double x, double y) : uid(s_uidgen++), p(x, y, 0) {
        }
        node_t(double x, double y, double z) : uid(s_uidgen++), p(x, y, z) {
        }

        // equality function for unordered_set/map
        // accurate to EPSILON
        bool operator == (const node_t& rhs) const {
            return
                hashed_value(p.x(), EPSILON) == hashed_value(rhs.p.x(), EPSILON) &&
                hashed_value(p.y(), EPSILON) == hashed_value(rhs.p.y(), EPSILON);
        }

        bool operator != (const node_t& rhs) const {
            return !operator==(rhs);
        }

        // hash function for unordered_set/map
        std::size_t operator()(const node_t& me) const {
            return hash_value_unsigned(hashed_value(me.p.x(), EPSILON), hashed_value(me.p.y(), EPSILON));
        }

        // only used for geospatial comparison (not for a set/map)
        bool geo_less_than(const node_t& rhs) const {
            if (p.x() < rhs.p.x()) return true;
            if (p.x() > rhs.p.x()) return false;
            return p.y() < rhs.p.y();
        }
    };

    // A graph edge connecting two nodes.
    // 
    // The "left" and "right" sides of the edge are relative to the direction
    // of the edge (from "node1" to "node2").
    // The 2 nodes are sorted in geospatial order from west to east;
    // the western-most node is always node1. If the edge is oriented
    // straight north or south, then the southern-most node is node1.
    struct edge_t
    {
        const UID uid;
        const node_t& node1; // first endpoint
        const node_t& node2; // second endpoint
        FID fid; // unique source id (feature id)
        float width; // m
        const properties_t* props = nullptr;
        int order = 0; // render order

        float angle = 0.0f; // relative to +x axis, used for sorting

        // end points, extruded to width.
        point_t node1_left, node1_right;
        point_t node2_left, node2_right;

        // constructor keeps the nodes sorted in geospatial order.
        edge_t(const node_t& a, const node_t& b, float w, const properties_t* d) :
            uid(s_uidgen++),
            node1(a.geo_less_than(b) ? a : b),
            node2(a.geo_less_than(b) ? b : a),
            width(w),
            props(d)
        {
            auto dir = node2.p - node1.p;
            angle = atan2(dir.y(), dir.x());
            if (w <= 0.0f && props != nullptr)
                width = props->default_width.as(Units::METERS);
        }

        vec_t direction() const {
            return normalize(node2.p - node1.p);
        }

        const node_t& other_node(const node_t& n) const {
            return n == node1 ? node2 : node1;
        }

        // get the node that the two edges share, or null if they don't
        const node_t* node_shared_with(const edge_t& rhs) const {
            if (node1 == rhs.node1 || node1 == rhs.node2) return &node1;
            else if (node2 == rhs.node1 || node2 == rhs.node2) return &node2;
            else return nullptr;
        }
        // get the node that is NOT shared between the 2 edges
        const node_t* node_not_shared_with(const edge_t& rhs) const {
            if (node1 != rhs.node1 && node1 != rhs.node2) return &node1;
            else if (node2 != rhs.node1 && node2 != rhs.node2) return &node2;
            else return nullptr;
        }

        // dot product of the two vectors forming the 2 edges and
        // extending from their shared node
        double cos_of_angle_with(const edge_t& rhs) const {
            auto shared = node_shared_with(rhs);
            if (!shared) return 0;
            auto n1 = node_shared_with(rhs);
            if (!n1) return 0;
            auto n2 = rhs.node_not_shared_with(*this);
            if (!n2) return 0;
            auto n3 = node_not_shared_with(rhs);
            if (!n3) return 0;
            auto v2 = normalize(n2->p - n1->p);
            auto v3 = normalize(n3->p - n1->p);
            return dot(v2, v3);
        }

        // true if they intersect, and returns the point in i.
        bool intersects(const edge_t rhs, point_t& i) const {
            if (node_shared_with(rhs)) return false;
            auto d1 = node2.p - node1.p;
            auto d2 = rhs.node2.p - rhs.node1.p;
            auto determinant = cross_2d(d1, d2);
            if (equivalent(determinant, 0.0))
                return false;
            double u = cross_2d(rhs.node1.p - node1.p, d2) / determinant;
            double v = cross_2d(rhs.node1.p - node1.p, d1) / determinant;
            i = node1.p + d1 * u;
            return (u > 0.0 && u < 1.0 && v > 0.0 && v < 1.0);

        }

        bool operator == (const edge_t& rhs) const {
            return
                uid == rhs.uid ||
                (node1 == rhs.node1 && node2 == rhs.node2);
        }
        bool operator != (const edge_t& rhs) const {
            return !operator==(rhs);
        }
        bool operator < (const edge_t& rhs) const {
            return uid < rhs.uid;
        }
        std::size_t operator()(const edge_t& me) const {
            return me.uid;
        }
    };

    struct angle_sort
    {
        angle_sort(const node_t& node) : _node(node)
        {
            _dreference = vec_t(node.p.x() + 1, node.p.y(), 0);
        }
        bool operator()(const edge_t* edge_a, const edge_t* edge_b) const
        {
            if (edge_a == edge_b)
                return false;

            OE_SOFT_ASSERT_AND_RETURN(edge_a, false);
            OE_SOFT_ASSERT_AND_RETURN(edge_b, false);
            OE_SOFT_ASSERT_AND_RETURN(edge_a != edge_b, false);

            auto* a = edge_a->node_not_shared_with(*edge_b);
            auto* b = edge_b->node_not_shared_with(*edge_a);

            const vec_t da = a->p - _node.p, db = b->p - _node.p;
            const double detb = cross_2d(_dreference, db);

            // nothing is less than zero degrees
            if (detb == 0 && db.x() * _dreference.x() + db.y() * _dreference.y() >= 0)
                return false;

            const double deta = cross_2d(_dreference, da);

            // zero degrees is less than anything else
            if (deta == 0 && da.x() * _dreference.x() + da.y() * _dreference.y() >= 0)
                return true;

            if (deta * detb >= 0) {
                // both on same side of reference, compare to each other
                return cross_2d(da, db) > 0;
            }

            // vectors "less than" zero degrees are actually large, near 2 pi
            return deta > 0;
        }
        const node_t& _node;
        vec_t _dreference;
    };

    struct graph_t
    {
        int num_subgraphs = -1;
        std::unordered_set<node_t, node_t> nodes;
        std::set<edge_t> edges;

        // sets a node's id and then propagates it to all neighbors
        void set_subgraph_id(node_t& n, int id) {
            n.subgraph_id = id;
            for (auto& edge : n.edges) {
                auto& other_node = edge->other_node(n);
                if (other_node.subgraph_id != id)
                    set_subgraph_id(const_cast<node_t&>(other_node), id);
            }
        }

        node_t* add_node(double x1, double y1, double z1, const properties_t* props) {
            const auto iter = nodes.emplace(x1, y1, z1).first;
            auto node = const_cast<node_t*>(&(*iter));
            node->props = props;
            return node;
        }

        edge_t* add_edge(double x1, double y1, double x2, double y2, double z, float width, const properties_t* props) {
            const auto node1 = nodes.emplace(x1, y1, z).first;
            const auto node2 = nodes.emplace(x2, y2, z).first;
            if (node1->uid == node2->uid) // safety catch; shouldn't happen
                return nullptr;
            const auto edge_emplace_result = edges.emplace(*node1, *node2, width, props);
            auto edge = const_cast<edge_t*>(&(*edge_emplace_result.first));
            if (!edge_emplace_result.second)
                return edge; // dupe

            bool found = false;
            for (auto a : node1->edges)
                if (*a == *edge) {
                    found = true; break;
                }
            if (!found) node1->edges.emplace_back(edge);

            found = false;
            for(auto a : node2->edges)
                if (*a == *edge) {
                    found = true; break;
                }
            if (!found) node2->edges.emplace_back(edge);

            auto& n1 = const_cast<node_t&>(*node1);
            auto& n2 = const_cast<node_t&>(*node2);

            // propagate the subgraph ids.
            int new_id =
                n1.subgraph_id < 0 && n2.subgraph_id < 0 ? ++num_subgraphs :
                n1.subgraph_id < 0 ? n2.subgraph_id :
                n2.subgraph_id < 0 ? n1.subgraph_id :
                std::min(n1.subgraph_id, n2.subgraph_id);

            set_subgraph_id(n1, new_id); // will set n2 as well

            return edge;
        }
        // find crossing segments and break them up.
        void split_intersecting_edges()
        {
            bool restart = true;

            while (restart)
            {
                restart = false;
                restart = false;
                for (auto lhs_iter = edges.begin(); lhs_iter != edges.end() && !restart; ++lhs_iter)
                {
                    auto& lhs = *lhs_iter;
                    auto rhs_iter = lhs_iter;
                    for (++rhs_iter; rhs_iter != edges.end() && !restart;)
                    {
                        auto& rhs = *rhs_iter;
                        if (lhs != rhs)
                        {
                            point_t i;
                            if (lhs.intersects(rhs, i))
                            {
                                float z = 0.0f;
                                add_edge(lhs.node1.p.x(), lhs.node1.p.y(), i.x(), i.y(), z, lhs.width, lhs.props);
                                add_edge(lhs.node2.p.x(), lhs.node2.p.y(), i.x(), i.y(), z, lhs.width, lhs.props);
                                add_edge(rhs.node1.p.x(), rhs.node1.p.y(), i.x(), i.y(), z, rhs.width, rhs.props);
                                add_edge(rhs.node2.p.x(), rhs.node2.p.y(), i.x(), i.y(), z, rhs.width, rhs.props);
                                edges.erase(lhs_iter);
                                edges.erase(rhs_iter);
                                restart = true;
                            }
                        }
                        if (restart) break;
                        else  ++rhs_iter;
                    }
                    if (restart) break;
                    //else  ++lhs_iter;
                }
            }
        }
    };

    struct edge_tree_t
    {
        using index_t = RTree<edge_t*, double, 2>;
        index_t _index;

        edge_tree_t(graph_t& graph)
        {
            for (auto& edge : graph.edges)
            {
                double min[2] = { std::min(edge.node1.p.x(), edge.node2.p.x()), std::min(edge.node1.p.y(), edge.node2.p.y()) };
                double max[2] = { std::max(edge.node1.p.x(), edge.node2.p.x()), std::max(edge.node1.p.y(), edge.node2.p.y()) };
                _index.Insert(min, max, const_cast<edge_t*>(&edge));
            }
        }

        point_t closestPointOnEdgeTo(double x, double y) const
        {
            osg::Vec2d point(x, y);
            double closest2 = DBL_MAX;
            osg::Vec3d closest_point(0, 0, NO_DATA_VALUE);

            auto function = [&](const edge_t* edge) -> bool
                {
                    osg::Vec2d e1(edge->node1.p.x(), edge->node1.p.y());
                    osg::Vec2d e2(edge->node2.p.x(), edge->node2.p.y());
                    osg::Vec2d result;

                    osg::Vec2d qp = e2 - e1;
                    osg::Vec2d xp = point - e1;
                    double u = (xp * qp) / (qp * qp);
                    if (u < 0.0) result = e1;
                    else if (u > 1.0) result = e2;
                    else result = e1 + qp * u;

                    double dist2 = (result - point).length2();
                    if (dist2 < closest2)
                    {
                        closest2 = dist2;
                        closest_point.set(result.x(), result.y(),
                            edge->node1.p.z() + u * (edge->node2.p.z() - edge->node1.p.z()));
                    }

                    return true; // true to continue searching
                };

            const double radius = 1000.0;
            double min[2] = { x - radius, y - radius }, max[2] = { x + radius, y + radius };
            _index.Search(min, max, function);
            return closest_point;
        }
    };


    struct art_t
    {
        properties_t road_2_lane;

        void load(const RoadLayerArt& art)
        {
            // basic 2 lane road
            {
                osg::ref_ptr<osg::StateSet> lanes_ss;
                auto lanes_tex = new PBRTexture();
                lanes_tex->load(art.lanesMaterial().value());
                if (lanes_tex->status.isOK())
                {
                    lanes_tex->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
                    lanes_ss = new osg::StateSet();
                    lanes_ss->setTextureAttribute(0, lanes_tex);
                }
                else
                {
                    OE_WARN << "Failed to load lanes texture: " << lanes_tex->status.message() << std::endl;
                }
                //lines_ss->setTextureAttribute(0, lines_tex->albedo);
                //lines_ss->setTextureAttribute(1, lines_tex->normal);
                //lines_ss->setTextureAttribute(2, lines_tex->pbr);

                osg::ref_ptr<osg::StateSet> crossing_ss;
                auto crossings_tex = new PBRTexture();
                crossings_tex->load(art.crossingsMaterial().value());
                if (crossings_tex->status.isOK())
                {
                    crossings_tex->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
                    crossing_ss = new osg::StateSet();
                    crossing_ss->setTextureAttribute(0, crossings_tex);
                }
                else
                {
                    OE_WARN << "Failed to load crossings texture: " << crossings_tex->status.message() << std::endl;
                }

                osg::ref_ptr<osg::StateSet> intersection_ss;
                auto intersection_tex = new PBRTexture();
                intersection_tex->load(art.intersectionsMaterial().value());
                intersection_tex->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
                intersection_tex->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
                if (intersection_tex->status.isOK())
                {
                    intersection_ss = new osg::StateSet();
                    intersection_ss->setTextureAttribute(0, intersection_tex);
                }
                else
                {
                    OE_WARN << "Failed to load intersection texture: " << intersection_tex->status.message() << std::endl;
                }

                road_2_lane = {
                    "road",
                    { {}, 9, 0, true, true }, // surface - UNUSED
                    { lanes_ss, 5, 5, true, false }, // lines art, width, length, tiled_along_length, tiled_along_width
                    { crossing_ss, 0.5, 1.5, false, true }, // crossing art, width, length, tiled_along_length, tiled_along_width
                    { intersection_ss, 1.0f, 1.0f, true, true}, // intersection art, width, length, tiled_along_length, tiled_along_width
                    DEFAULT_ROAD_WIDTH, // default road width
                    1, // rank
                    2  // lanes
                };
            }

#if 0
            // skinny one land road
            {
                auto crossing = new osg::Texture2D(osgDB::readRefImageFile("D:/data/textures/road_crossing_simple.png"));
                crossing->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
                crossing->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
                crossing->setMaxAnisotropy(4.0f);

                road_one_lane = road_basic;
                road_one_lane.default_width = 5.0f;
                road_one_lane.lines.texture = nullptr; // no lines
                road_one_lane.crossing.texture = crossing;
                road_one_lane.crossing.length = 3.0f;
            }
#endif
            //secondary = { { texture, 10, 10 }, 20.0f, 4, 4 };
        }
    };

    void compile_3way_intersection(node_t& node)
    {
        // 3 edges in play:
        auto& a = *node.edges[0];
        auto& b = *node.edges[1];
        auto& c = *node.edges[2];

        // direction vectors
        vec_t da = a.direction();
        vec_t db = b.direction();
        vec_t dc = c.direction();

        // dot products
        double ab = dot(da, db);
        double bc = dot(db, dc);
        double ac = dot(da, dc);

        vec_t ori;

        if (ab > ac && ab > bc) // c is the t-edge
        {
            ori = (a.direction() + b.direction()) * 0.5;
        }
        else if (bc > ab && bc > ac) // a is the t-edge
        {
            ori = (b.direction() + c.direction()) * 0.5;
        }
        else // b is the t-edge
        {
            ori = (a.direction() + c.direction()) * 0.5;
        }

        node.intersection.orientation = ori;
    }

    // figure out the settings for a node crossing, if applicable.
    void compile_intersection(node_t& node)
    {
        node.intersection.backoff_length = 0.0f;

        if (node.edges.size() < 3)
        {
            node.intersection.type = intersection_t::NONE;
        }
        else
        {
            // generic for starters.
            node.intersection.type = intersection_t::GENERIC;

            // compute backoff distance for each edge.
            for(unsigned i = 0; i<=node.edges.size(); ++i)
            {
                auto& A = i < node.edges.size() ? *node.edges[i] : *node.edges.front();
                auto& B = (i == 0) ? *node.edges.back() : *node.edges[i - 1];
                float angle = 0.5 * acos(A.cos_of_angle_with(B)); // bisecting angle
                float half_width = 0.5 * (std::max(A.width, B.width));
                float backoff = half_width / tan(angle);
                node.intersection.backoff_length = std::max(node.intersection.backoff_length, backoff);
            }

            // if this node doesn't have its own properties, inherit them from the edges.
            if (node.props == nullptr)
            {
                node.props = node.edges.front()->props;
            }

            // compute the orientation of the crossing geometry
            if (node.edges.size() == 3)
            {
                compile_3way_intersection(node);
            }
        }
    }

    // squares off an unconnected edge node
    void compile_end_cap(edge_t& e, const node_t& n, bool use_backoff)
    {
        vec_t dir = e.direction();
        vec_t left = cross(vec_t(0, 0, 1), dir);
        vec_t extrusion = (left * e.width * 0.5);

        // backoff cannot be longer than 1/2 the edge length:
        float len = (e.node1.p - e.node2.p).length();
        float max_backoff = 0.5f * len;
        double backoff_len = std::min(n.intersection.backoff_length, max_backoff);

        vec_t backoff = (dir * backoff_len * (use_backoff? 1.0 : 0.0));

        if (e.node2 == n)
        {
            e.node2_left = e.node2.p + extrusion - backoff;
            e.node2_right = e.node2.p - extrusion - backoff;
        }
        else
        {
            e.node1_left = e.node1.p + extrusion + backoff;
            e.node1_right = e.node1.p - extrusion + backoff;
        }
    }

    void compile_2way_join(edge_t& A, edge_t& B, const node_t& n)
    {
        // too tight a turn? cap it
        double cosangle = A.cos_of_angle_with(B);
        if (cosangle > 0.25)
        {
            compile_end_cap(A, n, true);
            return;
        }

        auto other_end_of_A = A.node_not_shared_with(B);
        OE_SOFT_ASSERT_AND_RETURN(other_end_of_A, void());

        auto other_end_of_B = B.node_not_shared_with(A);
        OE_SOFT_ASSERT_AND_RETURN(other_end_of_B, void());

        vec_t in_A = normalize(other_end_of_A->p - n.p);
        vec_t out_B = normalize(n.p - other_end_of_B->p);

        vec_t left_A = cross(vec_t(0, 0, 1), in_A);
        vec_t left_B = cross(vec_t(0, 0, 1), out_B);

        vec_t median = (left_A + left_B) * 0.5;
        double median_len = (A.width * 0.5) / dot(left_B, median);

        if (A.node1 == n)
        {
            A.node1_left = n.p + median * median_len;
            A.node1_right = n.p - median * median_len;
        }
        else
        {
            A.node2_left = n.p - median * median_len;
            A.node2_right = n.p + median * median_len;
        }
    }

    // joins two edges together with a miter.
    void compile_2way_join(edge_t& A, const node_t& n)
    {
        edge_t& B = *n.edges[0] == A ? *n.edges[1] : *n.edges[0];
        compile_2way_join(A, B, n);
    }

    // joins a "third" edge to two edges that are joined with a simple miter.
    void compile_t_edge(edge_t& t_edge, const node_t& n)
    {
        // decrement the order of the T-edge so it draws "under" the main edges:
        t_edge.order--;

        // temporary:
        compile_end_cap(t_edge, n, false); // no backoff!
        return;

        edge_t* a;
        edge_t* b;

        if (*n.edges[0] == t_edge)
            a = (n.edges[1]), b = (n.edges[2]);
        else if (*n.edges[1] == t_edge)
            a = (n.edges[0]), b = (n.edges[2]);
        else
            a = (n.edges[0]), b = (n.edges[1]);
    }

    // 3-ways can be T, Y, or MERGE.
    void compile_3way_join(edge_t& e, const node_t& n)
    {
        // 3 edges in play:
        auto& a = *n.edges[0];
        auto& b = *n.edges[1];
        auto& c = *n.edges[2];

        // direction vectors
        vec_t da = normalize(a.node2.p - a.node1.p);
        vec_t db = normalize(b.node2.p - b.node1.p);
        vec_t dc = normalize(c.node2.p - c.node1.p);

        // dot products
        double ab = fabs(dot(da, db));
        double bc = fabs(dot(db, dc));
        double ac = fabs(dot(da, dc));

        // whichever pair of edges are closest in direction
        // (i.e have the highest dot products), the 3rd edge
        // is the "T" edge.
        auto& t_edge =
            (ab > ac && ab > bc) ? c :
            (ac > ab && ac > bc) ? b :
            a;

        // TEMPORARY:
        // ignore the T-junction and make a generic intersection.
        //compile_end_cap(e, n);
        //return;


        if (t_edge == e)
        {
            compile_t_edge(e, n);
        }
        else
        {
            if (t_edge == a) {
                if (e == b)
                    compile_2way_join(e, c, n);
                else
                    compile_2way_join(e, b, n);
            }
            else if (t_edge == b) {
                if (e == a)
                    compile_2way_join(e, c, n);
                else
                    compile_2way_join(e, a, n);
            }
            else { // t_edge == c
                if (e == a)
                    compile_2way_join(e, b, n);
                else
                    compile_2way_join(e, a, n);
            }
        }
    }

    // Calculates all the values needed to render the graph
    void compile(graph_t& g)
    {
        // sort each node's edges by angle.
        for (auto& n : g.nodes)
        {
            auto& node = const_cast<node_t&>(n);
            std::sort(node.edges.begin(), node.edges.end(), angle_sort(node));
        }

        // calculate crossing types and backoff distances
        for (auto& n : g.nodes)
        {
            auto& node = const_cast<node_t&>(n);
            compile_intersection(node);
        }

        // calculate the join points
        for (auto& e : g.edges)
        {
            auto& edge = const_cast<edge_t&>(e);

            for (const node_t* node : { &edge.node1, &edge.node2 })
            {
                if (node->edges.size() == 1)
                {
                    compile_end_cap(edge, *node, false); // no backoff needed
                }
                else if (node->edges.size() == 2)
                {
                    compile_2way_join(edge, *node);
                }
                else if (node->edges.size() == 3)
                {
                    compile_3way_join(edge, *node);
                }
                else
                {
                    // TODO? Just render the intersection separately in this case.
                    compile_end_cap(edge, *node, true);
                }
            }
        }
    }

    // clamps edge polygon points to the terrain.
    void clamp(graph_t& g, const SpatialReference* working_srs, ElevationPool* pool, ProgressCallback* progress)
    {
        ElevationPool::WorkingSet ws;

        for (auto& const_node : g.nodes)
        {
            auto& node = const_cast<node_t&>(const_node);
            GeoPoint p(working_srs, node.p.x(), node.p.y(), node.p.z(), ALTMODE_ABSOLUTE);
            auto e = pool->getSample(p, &ws, progress);
            if (e.hasData())
            {
                node.p.z() += e.elevation().as(Units::METERS);
            }
        }

        for (auto& const_edge : g.edges)
        {
            auto& edge = const_cast<edge_t&>(const_edge);
            edge.node1_left.z() = edge.node1.p.z();
            edge.node1_right.z() = edge.node1.p.z();
            edge.node2_left.z() = edge.node2.p.z();
            edge.node2_right.z() = edge.node2.p.z();
        }
    }

#if 0
    osg::Node* tessellate_edge(const edge_t& edge, std::function<osg::Vec3(const osg::Vec3d&)>& transform)
    {
        auto verts = new osg::Vec3Array();
        auto colors = new osg::Vec4Array(osg::Array::BIND_OVERALL);
        auto uvs = new osg::Vec2Array(osg::Array::BIND_PER_VERTEX);

        verts->push_back(transform(edge.node1_right));
        verts->push_back(transform(edge.node2_right));
        verts->push_back(transform(edge.node2_left));
        verts->push_back(transform(edge.node1_left));

        colors->push_back(osg::Vec4(1, 1, 1, 1));

        // rotate the segment onto the x-axis to get the correct repeating
        osg::Quat q;
        q.makeRotate(normalize(edge.node2.p - edge.node1.p), vec_t(1, 0, 0));
        float s = edge.width / edge.props->surface.width;
        if (edge.props->surface.tiled_along_width == false)
        //if (edge.props->surface.texture->getWrap(osg::Texture::WRAP_S) != osg::Texture::REPEAT)
            s = std::ceil(s);
        float t;
        t = (q * (*verts)[0]).x() / edge.props->surface.length;
        uvs->push_back(osg::Vec2f(0, t));
        t = (q * (*verts)[1]).x() / edge.props->surface.length;
        uvs->push_back(osg::Vec2f(0, t));
        t = (q * (*verts)[2]).x() / edge.props->surface.length;
        uvs->push_back(osg::Vec2f(s, t));
        t = (q * (*verts)[3]).x() / edge.props->surface.length;
        uvs->push_back(osg::Vec2f(s, t));

        const GLushort indices[] = { 0, 1, 2, 0, 2, 3 };
        auto prim = new osg::DrawElementsUShort(GL_TRIANGLES, 6, indices);

        auto geom = new osg::Geometry();
        geom->setUseVertexBufferObjects(true);
        geom->setUseDisplayList(false);
        geom->setName("roads:tessellate_edge");
        geom->setVertexArray(verts);
        geom->setColorArray(colors);
        geom->setTexCoordArray(0, uvs);
        geom->addPrimitiveSet(prim);

        geom->setStateSet(edge.props->surface.stateset);

        return geom;
    }

    osg::Node* tessellate_edge_center_line(const edge_t& edge, std::function<osg::Vec3(const osg::Vec3d&)>& transform)
    {
        auto verts = new osg::Vec3Array();
        auto colors = new osg::Vec4Array(osg::Array::BIND_OVERALL);

        verts->push_back(transform(edge.node1.p));
        verts->push_back(transform(edge.node2.p));

        colors->push_back(osg::Vec4(1, 1, 0, 1));

        const GLushort indices[] = { 0, 1 };
        auto prim = new osg::DrawElementsUShort(GL_LINES, 2, indices);

        auto geom = new osg::Geometry();
        geom->setUseVertexBufferObjects(true);
        geom->setUseDisplayList(false);
        geom->setName("roads:tessellate_edge_center_line");
        geom->setVertexArray(verts);
        geom->setColorArray(colors);
        geom->addPrimitiveSet(prim);

        return geom;
    }
#endif

    template<class XFORM>
    osg::Node* tessellate_lanes(const edge_t& edge, XFORM&& transform)
    {
        if (edge.props->lanes.stateset == nullptr)
            return new osg::Group();

        auto verts = new osg::Vec3Array();
        auto colors = new osg::Vec4Array(osg::Array::BIND_OVERALL);
        auto normals = new osg::Vec3Array(osg::Array::BIND_OVERALL);
        auto uvs = new osg::Vec2Array(osg::Array::BIND_PER_VERTEX);

        verts->push_back(transform(edge.node1_right));
        verts->push_back(transform(edge.node2_right));
        verts->push_back(transform(edge.node2_left));
        verts->push_back(transform(edge.node1_left));

        colors->push_back(osg::Vec4(1, 1, 1, 0.85));
        normals->push_back(osg::Vec3(0, 0, 1));

        // rotate the segment onto the x-axis to get the correct repeating
        osg::Quat q;
        q.makeRotate(normalize(edge.node2.p - edge.node1.p), vec_t(1, 0, 0));

        float s = 1.0f; // stretch to whatever road we're using
        //float s = edge.width / edge.props->lanes.width;

        float t;
        t = (q * (*verts)[0]).x() / edge.props->lanes.length;
        uvs->push_back(osg::Vec2f(0, t));
        t = (q * (*verts)[1]).x() / edge.props->lanes.length;
        uvs->push_back(osg::Vec2f(0, t));
        t = (q * (*verts)[2]).x() / edge.props->lanes.length;
        uvs->push_back(osg::Vec2f(s, t));
        t = (q * (*verts)[3]).x() / edge.props->lanes.length;
        uvs->push_back(osg::Vec2f(s, t));

        const GLushort indices[] = { 0, 1, 2, 0, 2, 3 };
        auto prim = new osg::DrawElementsUShort(GL_TRIANGLES, 6, indices);

        auto geom = new osg::Geometry();
        geom->setName("roads:tessellate_lane_lines");
        geom->setUseVertexBufferObjects(true);
        geom->setUseDisplayList(false);
        geom->setVertexArray(verts);
        geom->setNormalArray(normals);
        geom->setColorArray(colors);
        geom->setTexCoordArray(0, uvs);
        geom->addPrimitiveSet(prim);

        geom->setStateSet(edge.props->lanes.stateset);

        return geom;
    }

    template<class XFORM>
    osg::Node* tessellate_crossing(const node_t& node, XFORM&& transform)
    {
        if (node.props == nullptr ||
            node.props->crossing.stateset == nullptr ||
            node.edges.size() != 2)
        {
            return new osg::Group();
        }

        const node_t& node2 = node.edges[0]->other_node(node);
        const node_t& node3 = node.edges[1]->other_node(node);

        vec_t d2 = (node2.p - node.p);
        vec_t d3 = (node.p - node3.p);
        vec_t orientation = normalize(d2 + d3);

        osg::Quat q;
        q.makeRotate(osg::Vec3d(0, 1, 0), orientation);
        float road_width = node.edges.front()->width; // road width
        float crossing_width = node.props->crossing.width;
        float hw = road_width * 0.5;
        float hl = node.props->crossing.length * 0.5;
        float s = std::ceil(road_width / crossing_width);

        auto geom = new osg::Geometry();
        geom->setName("roads:tessellate_crossing");
        geom->setUseVertexBufferObjects(true);
        geom->setUseDisplayList(false);
        geom->setUseVertexArrayObject(true);

        auto LL = node.p + q * osg::Vec3d(-hw, -hl, 0);
        auto LR = node.p + q * osg::Vec3d(hw, -hl, 0);
        auto UR = node.p + q * osg::Vec3d(hw, hl, 0);
        auto UL = node.p + q * osg::Vec3d(-hw, hl, 0);

        auto verts = new osg::Vec3Array();
        verts->reserve(4);
        verts->push_back(transform(LL));
        verts->push_back(transform(LR));
        verts->push_back(transform(UR));
        verts->push_back(transform(UL));
        geom->setVertexArray(verts);

        const GLubyte index_data[] = { 0, 1, 2, 0, 2, 3 };
        geom->addPrimitiveSet(new osg::DrawElementsUByte(GL_TRIANGLES, 6, index_data));

        const osg::Vec4 color_data[] = { { 1, 1, 1, 1 } };
        geom->setColorArray(new osg::Vec4Array(osg::Array::BIND_OVERALL, 1, color_data));

        const osg::Vec2 uv_data[] = { { 0,0 }, {s, 0}, {s, 1}, {0, 1} };
        geom->setTexCoordArray(0, new osg::Vec2Array(osg::Array::BIND_PER_VERTEX, 4, uv_data));

        geom->setStateSet(node.props->crossing.stateset);

        return geom;
    }

    template<class XFORM>
    osg::Node* tessellate_intersections(const graph_t& g, XFORM&& transform)
    {
        if (g.nodes.empty())
            return new osg::Group();

        // Note: this depends on each node's edges being sorted by angle,
        // which happens in the compile() function.

        // TODO: later, we will sort these by properties so we get all the right
        // textures in the right places. For now, just use the props of the first
        // node:
        const node_t* first_node = nullptr;

        unsigned num_points = 0;
        for (auto& node : g.nodes)
        {
            if (node.intersection.type != node.intersection.NONE && node.edges.size() >= 4)
            {
                if (!first_node)
                    first_node = &node;

                num_points += node.edges.size() * 2;
            }
        }

        if (num_points < 2)
            return new osg::Group();

        auto geom = new osg::Geometry();
        geom->setUseDisplayList(false);
        geom->setUseVertexBufferObjects(true);

        std::vector<vec_t> points;
        points.reserve(num_points);

        auto indices = new osg::DrawElementsUInt(GL_TRIANGLES);
        indices->reserve(3 * (num_points - 2));
        geom->addPrimitiveSet(indices);

        auto uvs = new osg::Vec2Array(osg::Array::BIND_PER_VERTEX);
        uvs->reserve(num_points);

        for (auto& node : g.nodes)
        {
            if (node.intersection.type != node.intersection.NONE && node.edges.size() == 4)
            {
                GLushort base = points.size();
                float length = 0.0f;
                Bounds b;

                for (auto& edge : node.edges)
                {
                    if (edge->node1 == node)
                    {
                        points.push_back(edge->node1_right);
                        points.push_back(edge->node1_left);
                    }
                    else
                    {
                        points.push_back(edge->node2_left);
                        points.push_back(edge->node2_right);
                    }

                    b.expandBy(points[points.size() - 2]);
                    b.expandBy(points[points.size() - 1]);

                    length += (points[points.size() - 2] - points[points.size() - 1]).length();

                    if (points.size()-base > 2)
                    {
                        indices->push_back(base);
                        indices->push_back(points.size() - 3);
                        indices->push_back(points.size() - 2);
                        indices->push_back(base);
                        indices->push_back(points.size() - 2);
                        indices->push_back(points.size() - 1);

                        length += (points[points.size() - 3] - points[points.size() - 2]).length();
                    }
                }

                // generate uvs.
                float sm = node.props->intersection.width;
                float tm = node.props->intersection.length;
                for (auto& p : points)
                {
                    float s = (p.x()-b.xMin())/(b.xMax()-b.xMin());
                    float t = (p.y()-b.yMin())/(b.yMax()-b.yMin());
                    uvs->push_back(osg::Vec2(s * sm, t * tm));
                }
            }
        }

        geom->setTexCoordArray(0, uvs);

        auto verts = new osg::Vec3Array();
        verts->reserve(points.size());
        for (auto& p : points) {
            verts->push_back(transform(p));
        }
        geom->setVertexArray(verts);

        auto normals = new osg::Vec3Array(osg::Array::BIND_OVERALL);
        normals->push_back(osg::Vec3(0, 0, 1));
        geom->setNormalArray(normals);

        auto colors = new osg::Vec4Array(osg::Array::BIND_OVERALL);
        colors->push_back(osg::Vec4(1, 1, 1, 1));
        geom->setColorArray(colors);


        // texture:
        if (first_node && first_node->props && first_node->props->intersection.stateset.valid())
        {
            geom->setStateSet(first_node->props->intersection.stateset);
        }

        return geom;
    }

    template<class XFORM>
    osg::Node* tessellate_decals(const graph_t& g, XFORM&& transform)
    {
        auto group = new osg::Group();

        // first sort the edges by render order.
        std::vector<const edge_t*> sorted_edges;
        sorted_edges.reserve(g.edges.size());
        for(auto& edge : g.edges)
            sorted_edges.push_back(&edge);
        std::sort(sorted_edges.begin(), sorted_edges.end(), [](const edge_t* a, const edge_t* b) {
            return a->order < b->order;
        });        

        for (auto& edge : sorted_edges)
        {
            group->addChild(tessellate_lanes(*edge, transform));
        }
        for (auto& node : g.nodes)
        {
            if (node.props && node.has_crossing)
            {
                group->addChild(tessellate_crossing(node, transform));
            }
        }

        group->addChild(tessellate_intersections(g, transform));

        return group;
    }

    void create_feature_polygons_from_decals(const graph_t& g, MultiGeometry* mg)
    {
        for (auto& edge : g.edges)
        {
            auto t1 = new osgEarth::Polygon();
            t1->push_back(edge.node1_left);
            t1->push_back(edge.node1_right);
            t1->push_back(edge.node2_left);
            mg->add(t1);

            auto t2 = new osgEarth::Polygon();
            t2->push_back(edge.node2_left);
            t2->push_back(edge.node1_right);
            t2->push_back(edge.node2_right);
            mg->add(t2);
        }
    }

    template<class XFORM>
    Geometry* create_lane_polygon(const edge_t& edge, XFORM&& transform)
    {
        auto poly = new osgEarth::Polygon();
        poly->reserve(4);
        poly->push_back(transform(edge.node1_right));
        poly->push_back(transform(edge.node2_right));
        poly->push_back(transform(edge.node2_left));
        poly->push_back(transform(edge.node1_left));
        return poly;
    }

    template<class XFORM>
    osg::Node* tessellate_decals_to_grid(const TileKey& key, const SpatialReference* working_srs, const graph_t& g, XFORM&& transform)
    {
        // first sort the edges by render order.
        std::vector<const edge_t*> sorted_edges;
        sorted_edges.reserve(g.edges.size());
        for (auto& edge : g.edges)
            sorted_edges.push_back(&edge);
        std::sort(sorted_edges.begin(), sorted_edges.end(), [](const edge_t* a, const edge_t* b) {
            return a->order < b->order;
            });

        MeshConstraint mc;
        mc.hasElevation = true;
        mc.removeExterior = true;

        for (auto& edge : sorted_edges)
        {
            auto poly = create_lane_polygon(*edge, transform);
            mc.features.emplace_back(new Feature(poly, working_srs));
        }

        TileMesher mesher;
        auto mesh = mesher.createMesh(key, { mc }, nullptr);
        
        auto geom = new osg::Geometry();
        geom->setUseVertexBufferObjects(true);
        geom->setUseDisplayList(false);
        geom->setVertexArray(mesh.verts);
        geom->setNormalArray(mesh.normals, osg::Array::BIND_PER_VERTEX);
        geom->setTexCoordArray(0, mesh.uvs);
        geom->addPrimitiveSet(mesh.indices);

        auto mt = new osg::MatrixTransform();
        mt->setMatrix(mesh.localToWorld);
        mt->addChild(geom);

        return mt;
    }
}

//#define DEFAULT_WIDTH 5.0

#ifdef OSGEARTH_HAVE_CLIPPER2
#include <clipper2/clipper.h>
namespace
{
    class Clipper2OffsetFilter : public FeatureFilter
    {
    public:
        FilterContext push(FeatureList& input, FilterContext& context) override
        {
            if (input.empty())
                return context;

            // Notes:
            // You will notice that this code separately inflates each line string.
            // This sounds wierd but it is an order of magnitude faster than inflating
            // the entire collection of linestrings at once.

            FeatureList output;

            Clipper2Lib::PathsD solution;
            Clipper2Lib::PathsD temp = { Clipper2Lib::PathD(2) };

            for (auto& feature : input)
            {
                if (feature->getGeometry()->getComponentType() == Geometry::TYPE_POLYGON)
                {
                    output.emplace_back(feature.get());
                }
                else
                {
                    //double width = DEFAULT_WIDTH;
                    //if (feature->hasAttr("width"))
                    //    width = feature->getDouble("width", width);
                    //else if (feature->hasAttr("lanes"))
                    //    width = feature->getDouble("lanes", 1.0) * DEFAULT_WIDTH;

                    ConstGeometryIterator iter(feature->getGeometry());
                    while (iter.hasMore())
                    {
                        auto* part = iter.next();
                        if (part->isLinear())
                        {
                            temp[0].clear();
                            temp[0].reserve(part->size());
                            for (auto& p : *part)
                            {
                                temp[0].emplace_back(p.x(), p.y());
                            }

                            // If we are using this for SDF, we need the inflation and the SDF-ing to
                            // total the road width.
                            auto inflate_solution = Clipper2Lib::InflatePaths(
                                temp,
                                0.5, // per side
                                Clipper2Lib::JoinType::Round,
                                Clipper2Lib::EndType::Round);

                            for (auto& s : inflate_solution)
                            {
                                solution.emplace_back(std::move(s));
                            }
                        }
                    }
                }
            }

            // do we need this? nah
            //Clipper2Lib::RamerDouglasPeucker(solution, EPSILON);

            // now union all the inflated linestrings:
            solution = Clipper2Lib::Union(solution, Clipper2Lib::FillRule::NonZero);

            // Annoyingly, there is no guarantee as to the order in which
            // clipper will output polygons and holes, so we need to sort each
            // hole into the corresponding polygon.
            std::vector<osg::ref_ptr<osgEarth::Polygon>> polygons;
            std::vector<Clipper2Lib::PathD*> polygon_paths;

            // find the outer ring of each polygon:
            for (auto& path : solution)
            {
                if (Clipper2Lib::IsPositive(path))
                {
                    polygon_paths.push_back(&path);
                    auto poly = new osgEarth::Polygon();
                    polygons.push_back(poly);
                    poly->reserve(path.size());
                    for (auto& point : path)
                    {
                        poly->push_back(point.x, point.y);
                    }
                }
            }

            // find the holes and sort them into the correct polygons
            // by testing the first point
            for (auto& path : solution)
            {
                if (!Clipper2Lib::IsPositive(path) && path.size() > 0) // holes
                {
                    auto& p = path.front();
                    for (int i = 0; i < polygon_paths.size(); ++i)
                    {
                        auto& polygon_path = *polygon_paths[i];
                        if (Clipper2Lib::PointInPolygon(p, polygon_path) == Clipper2Lib::PointInPolygonResult::IsInside)
                        {
                            auto ring = new osgEarth::Ring();
                            polygons[i]->getHoles().push_back(ring);
                            ring->reserve(path.size());
                            for (auto& point : path)
                            {
                                ring->push_back(point.x, point.y);
                            }
                            break;
                        }
                    }
                }
            }

            // Combine all the polygons into a single geometry.
            osg::ref_ptr<osgEarth::Geometry> geom;
            if (polygons.size() == 1)
            {
                geom = polygons[0];
            }
            else
            {
                auto multi = new osgEarth::MultiGeometry();
                multi->getComponents().reserve(polygons.size());
                for (auto& polygon : polygons)
                {
                    multi->add(polygon.get());
                }
                geom = multi;
            }

            output.emplace_back(new Feature(geom.get(), input.front()->getSRS()));
            input.swap(output);

            return context;
        }
    };
}
#endif // OSGEARTH_HAVE_CLIPPER2

namespace
{
    const char* substrate_shaders = R"(
        #pragma vp_function oe_road_substrate_vs, vertex_view
        vec2 oe_terrain_scaleCoordsToRefLOD(in vec2 tc, in float refLOD);
        out vec4 oe_layer_tilec;
        out vec2 oe_road_substrate_uv;
        void oe_road_substrate_vs(inout vec4 vert)
        {
            oe_road_substrate_uv = oe_terrain_scaleCoordsToRefLOD(oe_layer_tilec.st, 19);
        }

        [break]

        #pragma vp_function oe_road_substrate_fs, fragment
        #pragma include PBRMaterial.glsl
        #pragma import_defines(OE_ROAD_HAS_PBR_TEX, OE_ROAD_HAS_OCCLUSION_TEX)
        uniform sampler2D road_tex_albedo;
        uniform sampler2D road_tex_normal;
        uniform sampler2D road_tex_pbr;
        uniform sampler2D road_tex_occlusion;
        uniform vec4 road_layoutAndFactors;
        in vec2 oe_road_substrate_uv;
        in float oe_layer_opacity;
        vec3 vp_Normal;
        mat3 oe_normalMapTBN;
        uniform float oe_normal_boost = 1.0f;
        struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;

        float height_and_effect_mix(in float h1, in float a1, in float h2, in float a2)
        {
            // https://tinyurl.com/y5nkw2l9
            float ma = max(h1 + a1, h2 + a2) - 0.25; //displacement_depth;
            float b1 = max(h1 + a1 - ma, 0.0);
            float b2 = max(h2 + a2 - ma, 0.0);
            return b2 / (b1 + b2);
        }
                
        void oe_road_substrate_fs(inout vec4 color)
        {                    
            vec4 albedo = texture(road_tex_albedo, oe_road_substrate_uv);
            vec4 nn = texture(road_tex_normal, oe_road_substrate_uv);
            vec4 pbr = oe_pbr_default(road_layoutAndFactors.x);
          #ifdef OE_ROAD_HAS_PBR_TEX
            pbr = texture(road_tex_pbr, oe_road_substrate_uv);
          #endif
            float ao = -1.0;
          #ifdef OE_ROAD_HAS_OCCLUSION_TEX
            ao = texture(road_tex_occlusion, oe_road_substrate_uv).r;
          #endif
            pbr = oe_pbr_decode(pbr, road_layoutAndFactors, ao);
                    
            const float min_h = 0.5;
            float a = 1.0-color.r;
            float h = pbr[0];

            color = albedo;
            color.a *= height_and_effect_mix(1, 0, max(h,min_h), a) * oe_layer_opacity;
                    
            oe_pbr.displacement = pbr[0];
            oe_pbr.roughness = pbr[1];
            oe_pbr.ao = pbr[2];
            oe_pbr.metal = pbr[3];
                    
            vec3 N = nn.xyz * 2.0 - 1.0;
            N.z = 1.0 - abs(N.x)*oe_normal_boost - abs(N.y)*oe_normal_boost;
            vp_Normal = normalize(vp_Normal + oe_normalMapTBN * N);
        }
    )";


    const char* decal_shaders = R"(
        #pragma vp_function pull_clip, vertex_clip, last
        uniform float clipz_offset = 0.001;
        out float ndc_depth;
        void pull_clip(inout vec4 vert)
        {
            ndc_depth = (vert.z/vert.w) - clipz_offset;
        }

        [break]

        #pragma vp_function pull_fs, fragment, last
        in float ndc_depth;
        void pull_fs(inout vec4 color)
        {
            //if (color.a < 0.15) discard;
            gl_FragDepth = ((gl_DepthRange.diff*ndc_depth)+gl_DepthRange.near+gl_DepthRange.far)*0.5;
        }
    )";
}



namespace
{
    class SubstrateLayer : public FeatureSDFLayer
    {
    public:
        class Options : public FeatureSDFLayer::Options {
            META_LayerOptions(osgEarthProcedural, Options, FeatureSDFLayer::Options);
            void fromConfig(const Config&) { }
        };
        META_Layer(osgEarthProcedural, SubstrateLayer, Options, FeatureSDFLayer, _roads_surface);

    public:
        
        const RoadLayerArt* _artConfig = nullptr;
        PBRMaterial _material;
        std::array<TextureImageUnitReservation, 4> _units;

        void init() override
        {
            super::init();
            options().minLevel() = 18u;
        }            

        Status openImplementation() override
        {
            auto s = super::openImplementation();
            if (s.isError())
                return s;

            float width = DEFAULT_ROAD_WIDTH.as(Units::METERS);
            float buffered_width = width + (2.0 * _artConfig->substrateBuffer()->as(Units::METERS));

            Style style;
            auto render = style.getOrCreate<RenderSymbol>();
            render->sdfMinDistance() = Distance(0.5 * width, Units::METERS);
            render->sdfMaxDistance() = Distance(0.5 * buffered_width, Units::METERS);
            
            auto styles = new StyleSheet();
            styles->addStyle(style);
            options().styleSheet().setLayer(styles);

#ifdef OSGEARTH_HAVE_CLIPPER2
            // add the offseter if available
            _filterChain.push_back(new Clipper2OffsetFilter());
#endif

            return s;
        }

        void prepareForRendering(TerrainEngine* engine) override
        {
            super::prepareForRendering(engine);

            PBRTexture textures;
            const auto status = textures.load(_material, getReadOptions());
            if (status.isError())
            {
                setStatus(status);
                return;
            }

            if (!engine->getResources()->reserveTextureImageUnitForLayer(_units[0], this) ||
                !engine->getResources()->reserveTextureImageUnitForLayer(_units[1], this) ||
                !engine->getResources()->reserveTextureImageUnitForLayer(_units[2], this) ||
                (textures.occlusion && !engine->getResources()->reserveTextureImageUnitForLayer(_units[3], this)))
            {
                setStatus(Status::ResourceUnavailable, "Failed to reserve texture image units");
                return;
            }

            auto ss = getOrCreateStateSet();

            ss->setTextureAttribute(_units[0].unit(), textures.albedo.get());
            ss->getOrCreateUniform("road_tex_albedo", osg::Uniform::SAMPLER_2D)->set(_units[0].unit());

            ss->setTextureAttribute(_units[1].unit(), textures.normal.get());
            ss->getOrCreateUniform("road_tex_normal", osg::Uniform::SAMPLER_2D)->set(_units[1].unit());

            ss->getOrCreateUniform("road_layoutAndFactors", osg::Uniform::FLOAT_VEC4)->set(textures.layoutAndFactors);
            if (textures.pbr)
            {
                ss->setTextureAttribute(_units[2].unit(), textures.pbr.get());
                ss->getOrCreateUniform("road_tex_pbr", osg::Uniform::SAMPLER_2D)->set(_units[2].unit());
                ss->setDefine("OE_ROAD_HAS_PBR_TEX");
            }
            if (textures.occlusion)
            {
                ss->setTextureAttribute(_units[3].unit(), textures.occlusion.get());
                ss->getOrCreateUniform("road_tex_occlusion", osg::Uniform::SAMPLER_2D)->set(_units[3].unit());
                ss->setDefine("OE_ROAD_HAS_OCCLUSION_TEX");
            }

            // and the shader itself:
            auto vp = VirtualProgram::getOrCreate(getOrCreateStateSet());
            vp->setName("Road Surface");
            osgEarth::Util::Shaders shaders;
            shaders.add("RoadSubstrate.glsl", substrate_shaders);
            ShaderLoader::load(vp, "RoadSubstrate.glsl", shaders);
        }
    };


    class RoadDecalsLayer : public TiledModelLayer
    {
    public:
        class Options : public TiledModelLayer::Options {
            META_LayerOptions(osgEarthProcedural, Options, TiledModelLayer::Options);
        };
        META_Layer(osgEarthProcedural, RoadDecalsLayer, Options, TiledModelLayer, _roads_markings);

        const RoadLayerArt* _artConfig = nullptr;
        LayerReference<FeatureSource> _features;
        osg::ref_ptr<StyleSheet> _stylesheet;
        osg::ref_ptr<Session> _session;
        FeatureFilterChain _filterChain; // not needed, use filteredfeaturesource
        art_t _art;
        bool _drawRoadsAtopTerrainSkin = false;

        void init() override
        {
            super::init();
            setMinLevel(14);
            setMaxLevel(14);
            options().nvgl() = GLUtils::useNVGL();
            
            _filterChain.push_back(new ResampleFilter(0.0, 25.0));
        }

        Status openImplementation() override
        {
            auto s = super::openImplementation();
            if (s.isError())
                return s;

            _art.load(*_artConfig);

            _features.open(getReadOptions());
            return s;
        }

        void addedToMap(const Map* map)
        {
            super::addedToMap(map);

            _features.addedToMap(map);

            auto fs = _features.getLayer();
            if (!fs)
            {
                setStatus(Status::ConfigurationError, "RoadMarkingsDecalLayer: Missing required feature source");
                return;
            }

            if (!fs->getFeatureProfile())
            {
                setStatus(Status::ConfigurationError, "RoadMarkingsDecalLayer: Feature source has no feature profile");
                return;
            }

            _session = new Session(map);

            auto sheet = new StyleSheet();
            _session->setStyles(sheet);

            Style style;
            auto render = style.getOrCreate<RenderSymbol>();
            render->order().mutable_value().setLiteral(818);
            sheet->addStyle(style);
        }

        void removedFromMap(const Map* map)
        {
            super::removedFromMap(map);
            _features.removedFromMap(map);
        }

        void prepareForRendering(TerrainEngine* engine) override
        {
            super::prepareForRendering(engine);

            // blending on, backface culling on
            auto ss = getOrCreateStateSet();
            ss->setMode(GL_BLEND, 1);
            ss->setAttributeAndModes(new osg::CullFace(osg::CullFace::BACK), 1);
            ss->setAttributeAndModes(new osg::PolygonOffset(-1, -1), 1);

            // if the roads aren't cut in, we need some depth magic.
            if (_drawRoadsAtopTerrainSkin)
            {
                auto vp = VirtualProgram::getOrCreate(getOrCreateStateSet());
                ShaderLoader::load(vp, decal_shaders);
                vp->setName("Road Decals");

                ss->setAttributeAndModes(new osg::Depth(osg::Depth::LEQUAL, 0, 1, false), 1);
            }
        }

        const Profile* getProfile() const override
        {
            return Profile::create(Profile::GLOBAL_GEODETIC);

            auto fs = _features.getLayer();

            if (fs && fs->getFeatureProfile())
                return fs->getFeatureProfile()->getTilingProfile();
            else
                return nullptr;
        }

        osg::ref_ptr<osg::Node> createTileImplementation(const TileKey& key, ProgressCallback* progress) const override
        {
            // take local refs to isolate this method from the member objects
            auto featureSource(_features.getLayer());

            OE_SOFT_ASSERT_AND_RETURN(featureSource, nullptr);
            OE_SOFT_ASSERT_AND_RETURN(featureSource->getStatus().isOK(), nullptr);

            auto featureProfile = featureSource->getFeatureProfile();
            OE_SOFT_ASSERT_AND_RETURN(featureProfile, nullptr);

            auto featureSRS = featureProfile->getSRS();
            OE_SOFT_ASSERT_AND_RETURN(featureSRS, nullptr);

            // Fetch the set of features to render
            FilterContext context(_session.get());
            context.setWorkingExtent(key.getExtent().transform(featureSRS));

            auto cursor = featureSource->createFeatureCursor(key, _filterChain, &context, progress);
            if (!cursor.valid())
                return nullptr;

            FeatureList features;
            if (cursor->fill(features) == 0)
                return nullptr;

#if 0
            CropFilter crop(CropFilter::METHOD_BBOX);
            context.workingExtent() = key.getExtent().transform(featureSRS);
            context = crop.push(features, context);
#endif

            osg::Group* root = new osg::Group();

            graph_t graph;
            osg::Vec3d origin(0, 0, 0);

            auto centroid = key.getExtent().getCentroid();
            //auto working_srs = key.getExtent().getSRS()->createTangentPlaneSRS(centroid.vec3d());
            auto working_srs = SpatialReference::get("spherical-mercator");
            auto working_extent = key.getExtent().transform(working_srs);
            auto working_bounds = working_extent.bounds();

            for (auto& feature : features)
            {
                if (!feature.valid())
                    continue;

                auto geom = feature->getGeometry();
                if (!geom)
                    continue;

                int layer = feature->getInt("layer", 0);
                // for now, skip features that are underground
                if (layer < 0)
                    continue;

                //TODO: use this to render overpasses, etc. We will need to "bin" them separately
                // because of the depth writes being disabled on the bottom layer.
                //float z = layer * 3.0f;
                float z = 0.0;

                feature->transform(working_srs);

                // trivial reject-o
                if (!feature->getExtent().intersects(working_extent))
                    continue;

                auto highway = feature->getString("highway");
                auto width = feature->getDouble("width", 0);
                auto lanes = feature->getInt("lanes", 0);

                auto* art = &_art.road_2_lane;

                // todo: select art based on number of lanes

                if (width == 0.0 && lanes > 0)
                {
                    width = lanes * LANE_WIDTH.as(Units::METERS);
                }

                if (highway == "crossing")
                {
                    if (feature->getString("crossing") != "unmarked" && !geom->empty())
                    {
                        auto pos = (*geom)[0] - origin;
                        if (working_extent.contains(osg::Vec3d(pos.x(), pos.y(), z)))
                        {
                            auto node = graph.add_node(pos.x(), pos.y(), z, art);
                            node->has_crossing = true;
                        }
                    }
                }
                else
                {
                    ConstGeometryIterator iter(geom);
                    while (iter.hasMore())
                    {
                        auto part = iter.next();
                        if (part)
                        {
                            for (int i = 0; i < part->size() - 1; ++i)
                            {
                                auto p1 = (*part)[i] - origin;
                                auto p2 = (*part)[i + 1] - origin;

                                Bounds bounds;
                                bounds.expandBy(p1);
                                bounds.expandBy(p2);

                                if (intersects2d(working_bounds, bounds))
                                {
                                    // skip segments that don't meet the minimum length requirement
                                    //while ((p1 - p2).length() <= EPSILON && i < part->size() - 2)
                                    //{
                                    //    ++i;
                                    //    p2 = (*part)[i + 1] - origin;
                                    //}
                                    //if (i < part->size() - 1)
                                    {
                                        auto* edge = graph.add_edge(p1.x(), p1.y(), p2.x(), p2.y(), z, width, art);

                                        // higher layers draw later
                                        if (edge)
                                        {
                                            edge->order += layer;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (graph.edges.empty())
            {
                return {};
            }

            // OSM data should be properly noded. Any intersecting edges
            // are likely layered data (e.g. bridges, overpasses, tunnels).
            //graph.split_intersecting_edges();

            // Compile the 3D geometry:
            compile(graph);

            // Clamp to the terrain:
            auto pool = _session->getMap()->getElevationPool();
            clamp(graph, working_srs, pool, progress);

            // Tessellate into OSG:
            osg::Matrix world2local;
            centroid.createWorldToLocal(world2local);

            auto xform = [&](const osg::Vec3d& p)
                {
                    GeoPoint point(working_srs, p, ALTMODE_ABSOLUTE);
                    point.transformInPlace(working_srs->getGeographicSRS());                    
                    osg::Vec3d world;
                    point.toWorld(world);
                    return world * world2local;
                };

            // make the decal geometry
            root->addChild(tessellate_decals(graph, xform));

            // localization transform for this tile:
            auto mt = new osg::MatrixTransform();
            osg::Matrixd local2world;
            local2world.invert(world2local);
            mt->setMatrix(local2world);
            mt->addChild(root);
            root = mt;

            // render support:
            auto sheet = _stylesheet.get();
            auto style = sheet ? sheet->getDefaultStyle() : nullptr;
            auto render = style ? style->get<RenderSymbol>() : nullptr;
            if (render) render->applyTo(root);

            // embed the source features:
            auto* embedded = new MultiGeometry();
            create_feature_polygons_from_decals(graph, embedded);
            osg::ref_ptr<Feature> feature = new Feature(embedded, working_srs);
            auto object = new WrapperObject<osg::ref_ptr<Feature>>("features", feature);
            root->getOrCreateUserDataContainer()->addUserObject(object);

            return root;
        }
    };
}



void
RoadLayer::Options::fromConfig(const Config& conf)
{
    features().get(conf, "features");
    conf.get("art", art());
    conf.get("use_constraints", useConstraints());
    conf.get("substrate_min_level", substrateMinLevel());
    conf.get("constraints_min_level", constraintsMinLevel());
}

Config
RoadLayer::Options::getConfig() const
{
    Config conf = super::getConfig();
    features().set(conf, "features");
    conf.set("art", art());
    conf.set("use_constraints", useConstraints());
    conf.set("substrate_min_level", substrateMinLevel());
    conf.set("constraints_min_level", constraintsMinLevel());
    return conf;
}

void
RoadLayer::init()
{
    super::init();

    // pass along visibility changes
    auto update = [&](const VisibleLayer* layer)
        {
            if (_substrateLayer)
            {
                _substrateLayer->setVisible(layer->getVisible());
                _substrateLayer->setOpacity(layer->getOpacity());
            }
            if (_decalsLayer)
            {
                _decalsLayer->setVisible(layer->getVisible());
                _decalsLayer->setOpacity(layer->getOpacity());
            }
            if (_constraintsLayer)
            {
                _constraintsLayer->setVisible(layer->getVisible());
            }
        };

    onVisibleChanged(update);
    onOpacityChanged(update);
}

Status
RoadLayer::openImplementation()
{
    if (options().art()->substrateMaterial().isSet())
    {
        auto substrate = new SubstrateLayer();
        substrate->setName("..Roads - substrate");
        substrate->setMinLevel(std::max(options().substrateMinLevel().value(), getMinLevel()));
        substrate->setMaxDataLevel(19u);
        substrate->options().features() = options().features();
        substrate->_artConfig = &options().art().value();
        substrate->_material = options().art()->substrateMaterial().value();
        substrate->setUserProperty("show_in_ui", "true");
        substrate->options().cachePolicy() = options().cachePolicy();
        substrate->options().visible() = options().visible();
        substrate->options().opacity() = options().opacity();
        Status s = substrate->open(getReadOptions());
        if (s.isError())
            return s;
        _substrateLayer = substrate;
        _sublayers.emplace_back(_substrateLayer);
    }

    // Layer that renders the road marking decal geometries
    RoadDecalsLayer* decals = nullptr;
    if (options().art()->lanesMaterial().isSet())
    {
        decals = new RoadDecalsLayer();
        decals->setName("..Roads - decals");
        decals->setMinLevel(getMinLevel());
        decals->_features = options().features();
        decals->_artConfig = &options().art().value();
        decals->_drawRoadsAtopTerrainSkin = options().useConstraints() == false;
        decals->setUserProperty("show_in_ui", "true");
        decals->options().cachePolicy() = options().cachePolicy();
        decals->options().visible() = options().visible();
        decals->options().opacity() = options().opacity();
        Status s2 = decals->open(getReadOptions());
        if (s2.isError())
            return s2;
        _decalsLayer = decals;
        _sublayers.emplace_back(decals);
    }

    // Layer that cuts the road geometries into the terrain
    if (options().useConstraints() == true)
    {
        auto* con = new TerrainConstraintLayer();
        con->setName("..Roads - constraints");
        con->setUserProperty("show_in_ui", "true");
        con->setModelLayer(decals);
        con->setHasElevation(true);
        con->setRemoveInterior(true);
        con->setMinLevel(std::max(options().constraintsMinLevel().value(), getMinLevel()));
        con->options().cachePolicy() = options().cachePolicy();
        con->options().visible() = options().visible();
        con->options().opacity() = options().opacity();
        Status s3 = con->open(getReadOptions());
        if (s3.isError())
            return s3;
        _constraintsLayer = con;
        _sublayers.emplace_back(con);
    }

    return super::openImplementation();
}
