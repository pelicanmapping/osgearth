/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/TinyBVHLineSegmentIntersector>
#include <osgEarth/TinyBVHShape>

#include <osg/Geode>
#include <osg/Group>
#include <osg/LineWidth>
#include <osg/MatrixTransform>
#include <osg/ShapeDrawable>
#include <osg/io_utils>
#include <osg/observer_ptr>
#include <osgGA/GUIEventHandler>
#include <osgGA/TrackballManipulator>
#include <osgUtil/IntersectionVisitor>
#include <osgViewer/Viewer>
#include <osgEarth/Notify>

#include <algorithm>
#include <cmath>

using namespace osgEarth::Util;

namespace
{
    constexpr unsigned TERRAIN_MASK = 0x1u;
    constexpr unsigned ANNOTATION_MASK = 0x2u;

    osg::ref_ptr<osg::Geometry> createGrid(unsigned gridSize, bool useDrawArrays)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry();
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array();
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array();
        osg::ref_ptr<osg::DrawElementsUInt> indices =
            new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES);

        const unsigned rowSize = gridSize + 1u;
        vertices->reserve(static_cast<std::size_t>(rowSize) * rowSize);
        colors->reserve(static_cast<std::size_t>(rowSize) * rowSize);
        indices->reserve(static_cast<std::size_t>(gridSize) * gridSize * 6u);

        const float halfSize = static_cast<float>(gridSize) * 0.5f;
        for (unsigned y = 0u; y <= gridSize; ++y)
        {
            for (unsigned x = 0u; x <= gridSize; ++x)
            {
                const float px = static_cast<float>(x) - halfSize;
                const float py = static_cast<float>(y) - halfSize;
                const float pz = 4.0f * std::sin(px * 0.08f) * std::cos(py * 0.08f);
                vertices->push_back(osg::Vec3(px, py, pz));

                const float normalizedHeight = 0.5f + pz / 8.0f;
                colors->push_back(osg::Vec4(
                    0.1f + normalizedHeight * 0.3f,
                    0.35f + normalizedHeight * 0.45f,
                    0.15f,
                    1.0f));
            }
        }

        for (unsigned y = 0u; y < gridSize; ++y)
        {
            for (unsigned x = 0u; x < gridSize; ++x)
            {
                const unsigned lowerLeft = y * rowSize + x;
                const unsigned lowerRight = lowerLeft + 1u;
                const unsigned upperLeft = lowerLeft + rowSize;
                const unsigned upperRight = upperLeft + 1u;
                indices->push_back(lowerLeft);
                indices->push_back(lowerRight);
                indices->push_back(upperRight);
                indices->push_back(lowerLeft);
                indices->push_back(upperRight);
                indices->push_back(upperLeft);
            }
        }

        if (useDrawArrays)
        {
            osg::ref_ptr<osg::Vec3Array> expandedVertices = new osg::Vec3Array();
            osg::ref_ptr<osg::Vec4Array> expandedColors = new osg::Vec4Array();
            expandedVertices->reserve(indices->size() + 1u);
            expandedColors->reserve(indices->size() + 1u);
            // Leave an unused leading vertex to exercise DrawArrays::first.
            expandedVertices->push_back(osg::Vec3());
            expandedColors->push_back(osg::Vec4());
            for (unsigned index : *indices)
            {
                expandedVertices->push_back((*vertices)[index]);
                expandedColors->push_back((*colors)[index]);
            }

            geometry->setVertexArray(expandedVertices.get());
            geometry->setColorArray(expandedColors.get(), osg::Array::BIND_PER_VERTEX);
            geometry->addPrimitiveSet(new osg::DrawArrays(
                osg::PrimitiveSet::TRIANGLES,
                1,
                static_cast<GLsizei>(expandedVertices->size() - 1u)));
        }
        else
        {
            geometry->setVertexArray(vertices.get());
            geometry->setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX);
            geometry->addPrimitiveSet(indices.get());
        }
        geometry->setUseVertexBufferObjects(true);
        geometry->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        return geometry;
    }

    osg::ref_ptr<osg::Node> createIntersectionGraphic(
        const osg::Vec3d& start,
        const osg::Vec3d& end,
        const osg::Vec3d& hitPoint)
    {
        osg::ref_ptr<osg::Group> group = new osg::Group();
        group->setNodeMask(ANNOTATION_MASK);

        osg::ref_ptr<osg::Geometry> line = new osg::Geometry();
        osg::ref_ptr<osg::Vec3Array> lineVertices = new osg::Vec3Array();
        lineVertices->push_back(osg::Vec3(
            static_cast<float>(start.x()),
            static_cast<float>(start.y()),
            static_cast<float>(start.z())));
        lineVertices->push_back(osg::Vec3(
            static_cast<float>(end.x()),
            static_cast<float>(end.y()),
            static_cast<float>(end.z())));
        line->setVertexArray(lineVertices.get());
        line->addPrimitiveSet(new osg::DrawArrays(GL_LINES, 0, 2));
        osg::ref_ptr<osg::Vec4Array> lineColor = new osg::Vec4Array();
        lineColor->push_back(osg::Vec4(1.0f, 0.1f, 0.1f, 1.0f));
        line->setColorArray(lineColor.get(), osg::Array::BIND_OVERALL);
        line->getOrCreateStateSet()->setAttribute(new osg::LineWidth(3.0f));
        line->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);

        osg::ref_ptr<osg::Geode> lineGeode = new osg::Geode();
        lineGeode->addDrawable(line.get());
        group->addChild(lineGeode.get());

        osg::ref_ptr<osg::ShapeDrawable> marker =
            new osg::ShapeDrawable(new osg::Sphere(
                osg::Vec3(
                    static_cast<float>(hitPoint.x()),
                    static_cast<float>(hitPoint.y()),
                    static_cast<float>(hitPoint.z())),
                1.0f));
        marker->setColor(osg::Vec4(1.0f, 0.9f, 0.1f, 1.0f));
        osg::ref_ptr<osg::Geode> markerGeode = new osg::Geode();
        markerGeode->addDrawable(marker.get());
        group->addChild(markerGeode.get());
        return group;
    }

    osg::ref_ptr<osg::MatrixTransform> createPickMarker()
    {
        osg::ref_ptr<osg::MatrixTransform> marker = new osg::MatrixTransform();
        marker->setDataVariance(osg::Object::DYNAMIC);
        marker->setNodeMask(0u);

        osg::ref_ptr<osg::ShapeDrawable> sphere =
            new osg::ShapeDrawable(new osg::Sphere(osg::Vec3(), 1.25f));
        sphere->setColor(osg::Vec4(0.1f, 1.0f, 1.0f, 1.0f));

        osg::ref_ptr<osg::Geode> geode = new osg::Geode();
        geode->addDrawable(sphere.get());
        geode->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        marker->addChild(geode.get());
        return marker;
    }

    osg::Vec4 getDepthColor(unsigned depth, bool leaf)
    {
        const float phase = static_cast<float>(depth % 6u) / 6.0f;
        const float r = 0.5f + 0.5f * std::sin((phase + 0.00f) * 6.2831853f);
        const float g = 0.5f + 0.5f * std::sin((phase + 0.33f) * 6.2831853f);
        const float b = 0.5f + 0.5f * std::sin((phase + 0.67f) * 6.2831853f);
        return osg::Vec4(r, g, b, leaf ? 0.9f : 0.28f);
    }

    osg::ref_ptr<osg::Node> createBVHGraphic(const TinyBVHShape& bvh)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry();
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array();
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array();
        osg::ref_ptr<osg::DrawElementsUInt> indices =
            new osg::DrawElementsUInt(GL_LINES);
        vertices->reserve(bvh.getNodeCount() * 8u);
        colors->reserve(bvh.getNodeCount() * 8u);
        indices->reserve(bvh.getNodeCount() * 24u);

        static const unsigned edges[] = {
            0u, 1u, 1u, 3u, 3u, 2u, 2u, 0u,
            4u, 5u, 5u, 7u, 7u, 6u, 6u, 4u,
            0u, 4u, 1u, 5u, 2u, 6u, 3u, 7u
        };

        bvh.visitNodes([&](const osg::BoundingBox& bounds, bool leaf, unsigned depth, unsigned)
        {
            const unsigned first = static_cast<unsigned>(vertices->size());
            const osg::Vec4 color = getDepthColor(depth, leaf);
            for (unsigned corner = 0u; corner < 8u; ++corner)
            {
                vertices->push_back(bounds.corner(corner));
                colors->push_back(color);
            }
            for (unsigned edge : edges)
                indices->push_back(first + edge);
            return true;
        });

        geometry->setVertexArray(vertices.get());
        geometry->setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX);
        geometry->addPrimitiveSet(indices.get());
        geometry->setUseVertexBufferObjects(true);

        osg::StateSet* stateSet = geometry->getOrCreateStateSet();
        stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        stateSet->setMode(GL_BLEND, osg::StateAttribute::ON);
        stateSet->setAttribute(new osg::LineWidth(1.0f));
        stateSet->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);

        osg::ref_ptr<osg::Geode> geode = new osg::Geode();
        geode->addDrawable(geometry.get());
        return geode;
    }

    class BVHDebugHandler : public osgGA::GUIEventHandler
    {
    public:
        BVHDebugHandler(TinyBVHShape* bvh, osg::Group* overlay, bool initiallyVisible) :
            _bvh(bvh),
            _overlay(overlay)
        {
            setVisible(initiallyVisible);
        }

        bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&) override
        {
            if (ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN &&
                (ea.getKey() == 'd' || ea.getKey() == 'D'))
            {
                setVisible(!_visible);
                return true;
            }
            return false;
        }

    private:
        void setVisible(bool value)
        {
            if (!_bvh.valid() || !_overlay.valid())
                return;
            if (value && _overlay->getNumChildren() == 0u)
                _overlay->addChild(createBVHGraphic(*_bvh).get());

            _visible = value;
            _overlay->setNodeMask(_visible ? ANNOTATION_MASK : 0u);
            OSG_NOTICE << "TinyBVH visualization " << (_visible ? "enabled" : "disabled")
                << " (press D to toggle)" << std::endl;
        }

        osg::ref_ptr<TinyBVHShape> _bvh;
        osg::observer_ptr<osg::Group> _overlay;
        bool _visible = false;
    };

    class PickHandler : public osgGA::GUIEventHandler
    {
    public:
        explicit PickHandler(osg::MatrixTransform* marker) :
            _marker(marker)
        {
        }

        bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override
        {
            if ((ea.getEventType() != osgGA::GUIEventAdapter::MOVE &&
                 ea.getEventType() != osgGA::GUIEventAdapter::DRAG) ||
                !_marker.valid())
            {
                return false;
            }

            osgViewer::View* view = dynamic_cast<osgViewer::View*>(&aa);
            if (!view)
                return false;

            osg::ref_ptr<TinyBVHLineSegmentIntersector> intersector =
                new TinyBVHLineSegmentIntersector(osgUtil::Intersector::WINDOW, ea.getX(), ea.getY());
            intersector->setIntersectionLimit(osgUtil::Intersector::LIMIT_NEAREST);
            osgUtil::IntersectionVisitor visitor(intersector.get());
            visitor.setTraversalMask(TERRAIN_MASK);
            view->getCamera()->accept(visitor);

            if (intersector->containsIntersections())
            {
                const auto hit = intersector->getFirstIntersection();
                const osg::Vec3d hitPoint = hit.getWorldIntersectPoint();
                _marker->setMatrix(osg::Matrix::translate(hitPoint));
                _marker->setNodeMask(ANNOTATION_MASK);
            }
            else
                _marker->setNodeMask(0u);

            return false;
        }

    private:
        osg::observer_ptr<osg::MatrixTransform> _marker;
    };
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc, argv);
    const bool noViewer = arguments.read("--no-viewer");
    const bool useDrawArrays = arguments.read("--draw-arrays");
    const bool showBVH = arguments.read("--show-bvh");
    unsigned gridSize = 128u;
    unsigned leafSize = TinyBVHShape::DEFAULT_MAX_PRIMITIVES_PER_LEAF;
    arguments.read("--size", gridSize);
    arguments.read("--leaf-size", leafSize);
    gridSize = std::max(2u, gridSize);
    leafSize = std::max(1u, leafSize);

    osg::ref_ptr<osg::Geometry> geometry = createGrid(gridSize, useDrawArrays);
    osg::ref_ptr<TinyBVHShape> bvh = new TinyBVHShape(geometry.get(), leafSize);
    if (!bvh->valid())
    {
        OSG_WARN << "Failed to build TinyBVH shape" << std::endl;
        return 1;
    }
    geometry->setShape(bvh.get());

    OSG_NOTICE << "Built TinyBVH with " << bvh->getBuilderName()
        << " from " << (useDrawArrays ? "osg::DrawArrays" : "osg::DrawElementsUInt")
        << ": triangles=" << bvh->getPrimitiveCount()
        << ", nodes=" << bvh->getNodeCount()
        << ", max-leaf-size=" << bvh->getMaxPrimitivesPerLeaf()
        << ", bytes=" << bvh->getMemoryUsage() << std::endl;

    osg::ref_ptr<osg::Geode> terrain = new osg::Geode();
    terrain->setNodeMask(TERRAIN_MASK);
    terrain->addDrawable(geometry.get());

    const osg::Vec3d segmentStart(0.0, 0.0, 30.0);
    const osg::Vec3d segmentEnd(0.0, 0.0, -30.0);
    osg::ref_ptr<TinyBVHLineSegmentIntersector> intersector =
        new TinyBVHLineSegmentIntersector(segmentStart, segmentEnd);
    intersector->setIntersectionLimit(osgUtil::Intersector::LIMIT_NEAREST);
    osgUtil::IntersectionVisitor visitor(intersector.get());
    visitor.setTraversalMask(TERRAIN_MASK);
    terrain->accept(visitor);

    osg::ref_ptr<TinyBVHLineSegmentIntersector> referenceIntersector =
        new TinyBVHLineSegmentIntersector(segmentStart, segmentEnd);
    referenceIntersector->setIntersectionLimit(osgUtil::Intersector::LIMIT_NEAREST);
    osgUtil::IntersectionVisitor referenceVisitor(referenceIntersector.get());
    referenceVisitor.setTraversalMask(TERRAIN_MASK);
    referenceVisitor.setUseKdTreeWhenAvailable(false);
    terrain->accept(referenceVisitor);

    const bool resultsMatch =
        intersector->containsIntersections() == referenceIntersector->containsIntersections() &&
        (!intersector->containsIntersections() ||
            (intersector->getFirstIntersection().primitiveIndex ==
                referenceIntersector->getFirstIntersection().primitiveIndex &&
             (intersector->getFirstIntersection().getWorldIntersectPoint() -
                referenceIntersector->getFirstIntersection().getWorldIntersectPoint()).length() < 1e-6));
    if (!resultsMatch)
    {
        OSG_WARN << "TinyBVH result did not match unaccelerated triangle traversal" << std::endl;
        return 2;
    }
    OSG_NOTICE << "TinyBVH result matches unaccelerated triangle traversal" << std::endl;

    osg::ref_ptr<osg::Group> root = new osg::Group();
    root->addChild(terrain.get());
    osg::ref_ptr<osg::MatrixTransform> pickMarker = createPickMarker();
    root->addChild(pickMarker.get());
    osg::ref_ptr<osg::Group> bvhOverlay = new osg::Group();
    bvhOverlay->setNodeMask(0u);
    root->addChild(bvhOverlay.get());
    osg::ref_ptr<BVHDebugHandler> bvhDebugHandler =
        new BVHDebugHandler(bvh.get(), bvhOverlay.get(), showBVH);
    if (intersector->containsIntersections())
    {
        const osg::Vec3d hitPoint = intersector->getFirstIntersection().getWorldIntersectPoint();
        root->addChild(createIntersectionGraphic(segmentStart, segmentEnd, hitPoint).get());
    }
    else
    {
        OSG_WARN << "The demonstration segment did not intersect the grid" << std::endl;
    }

    if (noViewer)
        return 0;

    osgViewer::Viewer viewer(arguments);
    viewer.setSceneData(root.get());
    viewer.setCameraManipulator(new osgGA::TrackballManipulator());
    viewer.addEventHandler(new PickHandler(pickMarker.get()));
    viewer.addEventHandler(bvhDebugHandler.get());
    viewer.getCamera()->setClearColor(osg::Vec4(0.08f, 0.1f, 0.14f, 1.0f));
    viewer.setUpViewInWindow(100, 100, 1280, 720);
    return viewer.run();
}
