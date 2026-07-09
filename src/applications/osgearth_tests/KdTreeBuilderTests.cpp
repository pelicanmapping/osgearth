/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <osgEarth/catch.hpp>
#include <osgEarth/KdTreeBuilder>

#include <osg/Geometry>
#include <osg/Geode>
#include <osg/KdTree>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>

#include <random>

namespace
{
    // Builds a terrain-like grid of indexed triangles, dim x dim vertices.
    osg::ref_ptr<osg::Geometry> makeGrid(unsigned dim, bool useUShortIndices = false)
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array();
        verts->reserve(dim * dim);
        for (unsigned r = 0; r < dim; ++r)
        {
            for (unsigned c = 0; c < dim; ++c)
            {
                float z = sinf((float)r * 0.35f) * cosf((float)c * 0.25f) * 3.0f;
                verts->push_back(osg::Vec3((float)c, (float)r, z));
            }
        }
        geom->setVertexArray(verts.get());

        if (useUShortIndices)
        {
            osg::ref_ptr<osg::DrawElementsUShort> de = new osg::DrawElementsUShort(GL_TRIANGLES);
            de->reserve((dim - 1) * (dim - 1) * 6);
            for (unsigned r = 0; r < dim - 1; ++r)
            {
                for (unsigned c = 0; c < dim - 1; ++c)
                {
                    unsigned i0 = r * dim + c, i1 = i0 + 1, i2 = i0 + dim, i3 = i2 + 1;
                    de->push_back(i0); de->push_back(i1); de->push_back(i2);
                    de->push_back(i1); de->push_back(i3); de->push_back(i2);
                }
            }
            geom->addPrimitiveSet(de.get());
        }
        else
        {
            osg::ref_ptr<osg::DrawElementsUInt> de = new osg::DrawElementsUInt(GL_TRIANGLES);
            de->reserve((dim - 1) * (dim - 1) * 6);
            for (unsigned r = 0; r < dim - 1; ++r)
            {
                for (unsigned c = 0; c < dim - 1; ++c)
                {
                    unsigned i0 = r * dim + c, i1 = i0 + 1, i2 = i0 + dim, i3 = i2 + 1;
                    de->push_back(i0); de->push_back(i1); de->push_back(i2);
                    de->push_back(i1); de->push_back(i3); de->push_back(i2);
                }
            }
            geom->addPrimitiveSet(de.get());
        }
        return geom;
    }

    // Random triangle soup as a non-indexed DrawArrays.
    osg::ref_ptr<osg::Geometry> makeTriangleSoup(unsigned numTris, unsigned seed)
    {
        std::minstd_rand rng(seed);
        std::uniform_real_distribution<float> dist(-100.0f, 100.0f);

        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array();
        verts->reserve(numTris * 3);
        for (unsigned i = 0; i < numTris * 3; ++i)
        {
            verts->push_back(osg::Vec3(dist(rng), dist(rng), dist(rng)));
        }
        geom->setVertexArray(verts.get());
        geom->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, numTris * 3));
        return geom;
    }

    // Geometry with several primitive sets of different types, including
    // degenerate primitives that the builder must discard.
    osg::ref_ptr<osg::Geometry> makeMixedGeometry()
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array();

        std::minstd_rand rng(1234);
        std::uniform_real_distribution<float> dist(-50.0f, 50.0f);
        for (unsigned i = 0; i < 400; ++i)
        {
            verts->push_back(osg::Vec3(dist(rng), dist(rng), dist(rng)));
        }
        // duplicate some vertices so indexed prims can be degenerate
        (*verts)[10] = (*verts)[11];
        (*verts)[100] = (*verts)[101];
        geom->setVertexArray(verts.get());

        // indexed triangles including degenerates (10,11 identical)
        osg::ref_ptr<osg::DrawElementsUInt> tris = new osg::DrawElementsUInt(GL_TRIANGLES);
        for (unsigned i = 0; i < 120; i += 3)
        {
            tris->push_back(i); tris->push_back(i + 1); tris->push_back(i + 2);
        }
        geom->addPrimitiveSet(tris.get());

        // a triangle strip
        geom->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_STRIP, 120, 30));
        // a triangle fan
        geom->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_FAN, 150, 20));
        // quads
        geom->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 170, 40));
        // quad strip
        geom->addPrimitiveSet(new osg::DrawArrays(GL_QUAD_STRIP, 210, 20));
        // polygon
        geom->addPrimitiveSet(new osg::DrawArrays(GL_POLYGON, 230, 12));
        // lines (includes a degenerate at 100/101)
        osg::ref_ptr<osg::DrawElementsUShort> lines = new osg::DrawElementsUShort(GL_LINES);
        for (unsigned i = 96; i < 116; i += 2)
        {
            lines->push_back(i); lines->push_back(i + 1);
        }
        geom->addPrimitiveSet(lines.get());
        // line strip and loop
        geom->addPrimitiveSet(new osg::DrawArrays(GL_LINE_STRIP, 242, 10));
        geom->addPrimitiveSet(new osg::DrawArrays(GL_LINE_LOOP, 252, 8));
        // points
        geom->addPrimitiveSet(new osg::DrawArrays(GL_POINTS, 260, 25));

        return geom;
    }

    bool boxesIdentical(const osg::BoundingBox& a, const osg::BoundingBox& b)
    {
        return a._min == b._min && a._max == b._max;
    }

    // Builds the geometry's kdtree with both the stock osg builder and the
    // osgEarth builder (single- and multi-threaded) and requires bit-identical
    // results plus exact-sized vectors from the osgEarth builder.
    void compareBuilders(osg::Geometry* geom, bool expectBuilt = true)
    {
        osg::ref_ptr<osg::KdTree> stock = new osg::KdTree();
        osg::KdTree::BuildOptions stockOptions;
        bool stockBuilt = stock->build(stockOptions, geom);
        REQUIRE(stockBuilt == expectBuilt);

        for (unsigned int numThreads : { 1u, 4u })
        {
            osg::ref_ptr<osg::KdTree> fast = new osg::KdTree();
            osg::KdTree::BuildOptions fastOptions;
            bool fastBuilt = osgEarth::Util::KdTreeBuilder::build(*fast, fastOptions, geom, numThreads);

            REQUIRE(stockBuilt == fastBuilt);
            if (!stockBuilt) continue;

            REQUIRE(stockOptions._numVerticesProcessed == fastOptions._numVerticesProcessed);
            REQUIRE(stock->_degenerateCount == fast->_degenerateCount);
            REQUIRE(stock->getVertices() == fast->getVertices());
            REQUIRE(stock->getVertexIndices() == fast->getVertexIndices());
            REQUIRE(stock->getPrimitiveIndices() == fast->getPrimitiveIndices());

            REQUIRE(stock->getNodes().size() == fast->getNodes().size());
            for (size_t i = 0; i < stock->getNodes().size(); ++i)
            {
                const osg::KdTree::KdNode& a = stock->getNodes()[i];
                const osg::KdTree::KdNode& b = fast->getNodes()[i];
                REQUIRE(a.first == b.first);
                REQUIRE(a.second == b.second);
                REQUIRE(boxesIdentical(a.bb, b.bb));
            }

            // the osgEarth builder must allocate exactly - no wasted capacity.
            REQUIRE(fast->getVertexIndices().capacity() == fast->getVertexIndices().size());
            REQUIRE(fast->getPrimitiveIndices().capacity() == fast->getPrimitiveIndices().size());
            REQUIRE(fast->getNodes().capacity() == fast->getNodes().size());
        }
    }

    // Collects (primitiveIndex, intersection point) pairs for a line segment.
    std::vector<std::pair<unsigned, osg::Vec3d>> intersect(
        osg::Node* node, const osg::Vec3d& start, const osg::Vec3d& end)
    {
        osg::ref_ptr<osgUtil::LineSegmentIntersector> lsi =
            new osgUtil::LineSegmentIntersector(start, end);
        osgUtil::IntersectionVisitor iv(lsi.get());
        node->accept(iv);

        std::vector<std::pair<unsigned, osg::Vec3d>> result;
        for (auto& hit : lsi->getIntersections())
        {
            result.emplace_back(hit.primitiveIndex, hit.getWorldIntersectPoint());
        }
        return result;
    }
}

TEST_CASE("KdTreeBuilder produces identical trees for grid meshes")
{
    compareBuilders(makeGrid(9).get());
    compareBuilders(makeGrid(33).get());
    compareBuilders(makeGrid(128).get());
    compareBuilders(makeGrid(33, true).get()); // ushort indices

    // large enough to engage the parallel collection and subdivision paths
    compareBuilders(makeGrid(257).get());
}

TEST_CASE("KdTreeBuilder produces identical trees for every primitive mode")
{
    GLenum modes[] = { GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_LINE_LOOP,
                       GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN,
                       GL_QUADS, GL_QUAD_STRIP, GL_POLYGON };

    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);

    // n is divisible by 12 so GL_TRIANGLES/GL_LINES/GL_QUADS index counts are
    // well formed; osg's own functor reads out of bounds otherwise.
    for (GLenum mode : modes)
    {
        for (unsigned n : { 48u, 60000u })
        {
            for (int variant = 0; variant < 3; ++variant)
            {
                std::minstd_rand rng(mode * 31 + n + variant);
                osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();
                osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array();
                for (unsigned i = 0; i < n; ++i)
                    verts->push_back(osg::Vec3(dist(rng), dist(rng), dist(rng)));

                // sprinkle duplicate vertices to produce degenerate primitives
                if (variant == 2)
                    for (unsigned i = 5; i + 1 < n; i += 97)
                        (*verts)[i + 1] = (*verts)[i];

                geom->setVertexArray(verts.get());

                if (variant == 0)
                {
                    geom->addPrimitiveSet(new osg::DrawArrays(mode, 0, n));
                }
                else
                {
                    osg::ref_ptr<osg::DrawElementsUShort> de = new osg::DrawElementsUShort(mode);
                    for (unsigned i = 0; i < n; ++i)
                        de->push_back((unsigned short)(rng() % n));
                    geom->addPrimitiveSet(de.get());
                }

                compareBuilders(geom.get());
            }
        }
    }
}

TEST_CASE("KdTreeBuilder produces identical trees for triangle soups")
{
    compareBuilders(makeTriangleSoup(10, 1).get());
    compareBuilders(makeTriangleSoup(1000, 2).get());
    compareBuilders(makeTriangleSoup(5000, 3).get());
}

TEST_CASE("KdTreeBuilder produces identical trees for mixed primitive types")
{
    compareBuilders(makeMixedGeometry().get());
}

TEST_CASE("KdTreeBuilder handles empty and unsupported geometry like osg")
{
    // no vertex array
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();
        compareBuilders(geom.get(), false);
    }

    // Vec3dArray is not supported by either builder
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();
        osg::ref_ptr<osg::Vec3dArray> verts = new osg::Vec3dArray();
        for (unsigned i = 0; i < 100; ++i)
            verts->push_back(osg::Vec3d(i, i, i));
        geom->setVertexArray(verts.get());
        geom->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 99));
        compareBuilders(geom.get(), false);
    }

    // too few vertices to bother
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array();
        for (unsigned i = 0; i < 4; ++i)
            verts->push_back(osg::Vec3(i, 0, 0));
        geom->setVertexArray(verts.get());
        geom->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        compareBuilders(geom.get(), false);
    }

    // vertices but no primitive sets
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array();
        for (unsigned i = 0; i < 100; ++i)
            verts->push_back(osg::Vec3(i, i, 0));
        geom->setVertexArray(verts.get());
        compareBuilders(geom.get());
    }

    // all-degenerate triangles
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array();
        for (unsigned i = 0; i < 30; ++i)
            verts->push_back(osg::Vec3(1, 2, 3));
        geom->setVertexArray(verts.get());
        geom->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 30));
        compareBuilders(geom.get());
    }
}

TEST_CASE("KdTreeBuilder trees return the same intersections")
{
    osg::ref_ptr<osg::Geometry> geom = makeGrid(65);

    osg::ref_ptr<osg::KdTree> stock = new osg::KdTree();
    osg::KdTree::BuildOptions stockOptions;
    REQUIRE(stock->build(stockOptions, geom.get()));

    osg::ref_ptr<osg::KdTree> fast = new osg::KdTree();
    osg::KdTree::BuildOptions fastOptions;
    REQUIRE(osgEarth::Util::KdTreeBuilder::build(*fast, fastOptions, geom.get()));

    osg::ref_ptr<osg::Geode> geode = new osg::Geode();
    geode->addDrawable(geom.get());

    // fire a bundle of rays down through the grid and require identical hits
    for (unsigned i = 0; i < 64; ++i)
    {
        double x = 1.0 + (double)i;
        osg::Vec3d start(x, 32.0, 100.0);
        osg::Vec3d end(x, 32.0, -100.0);

        geom->setShape(stock.get());
        auto stockHits = intersect(geode.get(), start, end);

        geom->setShape(fast.get());
        auto fastHits = intersect(geode.get(), start, end);

        REQUIRE(stockHits.size() == fastHits.size());
        for (size_t h = 0; h < stockHits.size(); ++h)
        {
            REQUIRE(stockHits[h].first == fastHits[h].first);
            REQUIRE(stockHits[h].second == fastHits[h].second);
        }
        REQUIRE(!stockHits.empty());
    }
}

TEST_CASE("KdTreeBuilder visitor applies kdtrees to a scene graph")
{
    osg::ref_ptr<osg::Geode> geode = new osg::Geode();
    osg::ref_ptr<osg::Geometry> geom = makeGrid(17);
    geode->addDrawable(geom.get());

    osg::ref_ptr<osgEarth::Util::KdTreeBuilder> builder = new osgEarth::Util::KdTreeBuilder();
    geode->accept(*builder);

    osg::KdTree* kdTree = dynamic_cast<osg::KdTree*>(geom->getShape());
    REQUIRE(kdTree != nullptr);
    REQUIRE(!kdTree->getNodes().empty());

    // visiting again must not replace the existing tree
    osg::ref_ptr<osgEarth::Util::KdTreeBuilder> builder2 = new osgEarth::Util::KdTreeBuilder();
    geode->accept(*builder2);
    REQUIRE(geom->getShape() == kdTree);

    // clone() must return the osgEarth builder type
    osg::ref_ptr<osg::KdTreeBuilder> cloned = builder->clone();
    REQUIRE(dynamic_cast<osgEarth::Util::KdTreeBuilder*>(cloned.get()) != nullptr);
}
