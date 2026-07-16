/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */

#include <osgEarth/catch.hpp>
#include <osgEarth/BuildConfig>
#include <osgEarth/DrawInstanced>
#include <osgEarth/TinyBVHLineSegmentIntersector>
#include <osgEarth/TinyBVHShape>

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/observer_ptr>
#include <osgUtil/IntersectionVisitor>

using namespace osgEarth::Util;

TEST_CASE("TinyBVHShape retains temporary proxy data and intersects multiple primitive sets")
{
    osg::ref_ptr<TinyBVHShape> shape;
    osg::observer_ptr<osg::Vec3Array> observedVertices;
    osg::observer_ptr<osg::PrimitiveSet> observedIndexedSet;
    osg::observer_ptr<osg::PrimitiveSet> observedDrawArrays;

    {
        osg::ref_ptr<osg::Geometry> temporaryGeometry = new osg::Geometry();
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array();
        vertices->push_back(osg::Vec3(0.0f, 0.0f, 0.0f));
        vertices->push_back(osg::Vec3(1.0f, 0.0f, 0.0f));
        vertices->push_back(osg::Vec3(0.0f, 1.0f, 0.0f));
        vertices->push_back(osg::Vec3(2.0f, 0.0f, 0.0f));
        vertices->push_back(osg::Vec3(3.0f, 0.0f, 0.0f));
        vertices->push_back(osg::Vec3(2.0f, 1.0f, 0.0f));
        osg::ref_ptr<osg::DrawElementsUInt> indexed =
            new osg::DrawElementsUInt(GL_TRIANGLES, 0);
        indexed->push_back(0u);
        indexed->push_back(1u);
        indexed->push_back(2u);
        osg::ref_ptr<osg::DrawArrays> drawArrays =
            new osg::DrawArrays(GL_TRIANGLES, 3, 3);

        temporaryGeometry->setVertexArray(vertices.get());
        temporaryGeometry->addPrimitiveSet(indexed.get());
        temporaryGeometry->addPrimitiveSet(drawArrays.get());

        osg::ref_ptr<TinyBVHBuilder> builder = new TinyBVHBuilder();
        temporaryGeometry->accept(*builder);
        shape = dynamic_cast<TinyBVHShape*>(temporaryGeometry->getShape());

        REQUIRE(shape.valid());
        REQUIRE(shape->valid());
        CHECK(shape->getPrimitiveCount() == 2u);

        observedVertices = vertices.get();
        observedIndexedSet = indexed.get();
        observedDrawArrays = drawArrays.get();
    }

    // The temporary Geometry is gone. The shape must keep only the source
    // arrays needed to resolve candidate primitive indices during traversal.
    REQUIRE(observedVertices.valid());
    REQUIRE(observedIndexedSet.valid());
    REQUIRE(observedDrawArrays.valid());

    osg::ref_ptr<osg::Geometry> proxyDrawable = new osg::Geometry();
    proxyDrawable->setShape(shape.get());
    proxyDrawable->setInitialBound(osg::BoundingBox(-1.0f, -1.0f, -1.0f, 4.0f, 2.0f, 1.0f));

    osg::ref_ptr<osg::Geode> geode = new osg::Geode();
    geode->addDrawable(proxyDrawable.get());

    osg::ref_ptr<TinyBVHLineSegmentIntersector> intersector =
        new TinyBVHLineSegmentIntersector(
            osg::Vec3d(2.2, 0.2, 1.0),
            osg::Vec3d(2.2, 0.2, -1.0));
    intersector->setIntersectionLimit(osgUtil::Intersector::LIMIT_NEAREST);
    osgUtil::IntersectionVisitor visitor(intersector.get());
    geode->accept(visitor);

    REQUIRE(intersector->containsIntersections());
    const auto hit = intersector->getFirstIntersection();
    CHECK(hit.primitiveIndex == 1u);
    CHECK((hit.getWorldIntersectPoint() - osg::Vec3d(2.2, 0.2, 0.0)).length() < 1e-6);
}

#ifdef OSGEARTH_USE_TINYBVH
TEST_CASE("DrawInstanced proxy geometry builds and uses a TinyBVHShape")
{
    osg::ref_ptr<DrawInstanced::InstanceGeometry> geometry =
        new DrawInstanced::InstanceGeometry();
    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array();
    vertices->push_back(osg::Vec3(0.0f, 0.0f, 0.0f));
    vertices->push_back(osg::Vec3(1.0f, 0.0f, 0.0f));
    vertices->push_back(osg::Vec3(0.0f, 1.0f, 0.0f));
    osg::ref_ptr<osg::DrawArrays> triangles =
        new osg::DrawArrays(GL_TRIANGLES, 0, 3);
    geometry->setVertexArray(vertices.get());
    geometry->addPrimitiveSet(triangles.get());

    std::vector<osg::Matrixf> matrices;
    matrices.push_back(osg::Matrixf::identity());
    matrices.push_back(osg::Matrixf::translate(2.0f, 0.0f, 0.0f));
    geometry->setMatrices(matrices);

    REQUIRE(geometry->getProxyGeometry() != nullptr);
    TinyBVHShape* shape = dynamic_cast<TinyBVHShape*>(geometry->getShape());
    REQUIRE(shape != nullptr);
    REQUIRE(shape->valid());
    CHECK(shape->getPrimitiveCount() == 2u);

    osg::ref_ptr<osg::Geode> geode = new osg::Geode();
    geode->addDrawable(geometry.get());
    osg::ref_ptr<TinyBVHLineSegmentIntersector> intersector =
        new TinyBVHLineSegmentIntersector(
            osg::Vec3d(2.2, 0.2, 1.0),
            osg::Vec3d(2.2, 0.2, -1.0));
    osgUtil::IntersectionVisitor visitor(intersector.get());
    geode->accept(visitor);

    REQUIRE(intersector->containsIntersections());
    CHECK(intersector->getFirstIntersection().primitiveIndex == 1u);
}
#endif
