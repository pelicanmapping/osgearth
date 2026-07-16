/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "TinyBVHLineSegmentIntersector"
#include "TinyBVHShape"

#include <cmath>

using namespace osgEarth::Util;

TinyBVHLineSegmentIntersector::TinyBVHLineSegmentIntersector(
    const osg::Vec3d& start,
    const osg::Vec3d& end) :
    osgUtil::LineSegmentIntersector(start, end)
{
}

TinyBVHLineSegmentIntersector::TinyBVHLineSegmentIntersector(
    CoordinateFrame cf,
    double x,
    double y) :
    osgUtil::LineSegmentIntersector(cf, x, y)
{
}

TinyBVHLineSegmentIntersector::TinyBVHLineSegmentIntersector(
    CoordinateFrame cf,
    const osg::Vec3d& start,
    const osg::Vec3d& end,
    osgUtil::LineSegmentIntersector* parent,
    IntersectionLimit limit) :
    osgUtil::LineSegmentIntersector(cf, start, end, parent, limit)
{
}

osgUtil::Intersector* TinyBVHLineSegmentIntersector::clone(osgUtil::IntersectionVisitor& iv)
{
    const osg::Matrix matrix = getTransformation(iv, _coordinateFrame);
    osg::ref_ptr<TinyBVHLineSegmentIntersector> intersector =
        new TinyBVHLineSegmentIntersector(
            MODEL,
            _start * matrix,
            _end * matrix,
            this,
            _intersectionLimit);
    intersector->setPrecisionHint(getPrecisionHint());
    return intersector.release();
}

void TinyBVHLineSegmentIntersector::intersect(
    osgUtil::IntersectionVisitor& iv,
    osg::Drawable* drawable)
{
    if (reachedLimit() || !drawable)
        return;

    osg::Vec3d start(_start);
    osg::Vec3d end(_end);
    if (drawable->isCullingActive() && !intersectAndClip(start, end, drawable->getBoundingBox()))
        return;
    if (iv.getDoDummyTraversal())
        return;

    intersect(iv, drawable, start, end);
}

void TinyBVHLineSegmentIntersector::intersect(
    osgUtil::IntersectionVisitor& iv,
    osg::Drawable* drawable,
    const osg::Vec3d& start,
    const osg::Vec3d& end)
{
    TinyBVHShape* shape =
        iv.getUseKdTreeWhenAvailable() && drawable ?
        dynamic_cast<TinyBVHShape*>(drawable->getShape()) : nullptr;

    if (!shape || !shape->valid())
    {
        osgUtil::LineSegmentIntersector::intersect(iv, drawable, start, end);
        return;
    }

    bool hitThisDrawable = false;
    shape->visitSegment(start, end, [&](unsigned primitiveIndex)
    {
        osg::Vec3d v0, v1, v2;
        unsigned i0 = 0u, i1 = 0u, i2 = 0u;
        if (shape->getTriangle(primitiveIndex, v0, v1, v2, i0, i1, i2))
        {
            hitThisDrawable = intersectTriangle(
                iv, drawable, start, end, primitiveIndex,
                v0, v1, v2, i0, i1, i2) || hitThisDrawable;
        }

        return !reachedLimit() &&
            !(getIntersectionLimit() == LIMIT_ONE_PER_DRAWABLE && hitThisDrawable);
    });
}

bool TinyBVHLineSegmentIntersector::intersectTriangle(
    osgUtil::IntersectionVisitor& iv,
    osg::Drawable* drawable,
    const osg::Vec3d& clippedStart,
    const osg::Vec3d& clippedEnd,
    unsigned primitiveIndex,
    const osg::Vec3d& v0,
    const osg::Vec3d& v1,
    const osg::Vec3d& v2,
    unsigned i0,
    unsigned i1,
    unsigned i2)
{
    osg::Vec3d direction = clippedEnd - clippedStart;
    const double segmentLength = direction.length();
    if (segmentLength <= 0.0)
        return false;
    direction /= segmentLength;

    const osg::Vec3d edge1 = v1 - v0;
    const osg::Vec3d edge2 = v2 - v0;
    const osg::Vec3d p = direction ^ edge2;
    const double determinant = edge1 * p;
    constexpr double epsilon = 1e-12;
    if (std::abs(determinant) <= epsilon)
        return false;

    const double inverseDeterminant = 1.0 / determinant;
    const osg::Vec3d tvec = clippedStart - v0;
    const double u = (tvec * p) * inverseDeterminant;
    if (u < 0.0 || u > 1.0)
        return false;

    const osg::Vec3d q = tvec ^ edge1;
    const double v = (direction * q) * inverseDeterminant;
    if (v < 0.0 || u + v > 1.0)
        return false;

    const double distance = (edge2 * q) * inverseDeterminant;
    if (distance < 0.0 || distance > segmentLength)
        return false;

    const double originalLength = (_end - _start).length();
    if (originalLength <= 0.0)
        return false;

    const double ratio = ((_start - clippedStart).length() + distance) / originalLength;
    const double w = 1.0 - u - v;

    Intersection hit;
    hit.ratio = ratio;
    hit.matrix = iv.getModelMatrix();
    hit.nodePath = iv.getNodePath();
    hit.drawable = drawable;
    hit.primitiveIndex = primitiveIndex;
    hit.localIntersectionPoint = clippedStart + direction * distance;
    hit.localIntersectionNormal = edge1 ^ edge2;
    hit.localIntersectionNormal.normalize();

    if (w != 0.0)
    {
        hit.indexList.push_back(i0);
        hit.ratioList.push_back(w);
    }
    if (u != 0.0)
    {
        hit.indexList.push_back(i1);
        hit.ratioList.push_back(u);
    }
    if (v != 0.0)
    {
        hit.indexList.push_back(i2);
        hit.ratioList.push_back(v);
    }

    insertIntersection(hit);
    return true;
}
