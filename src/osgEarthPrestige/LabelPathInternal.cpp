/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#include "LabelEngineInternal.h"
#include <osg/Vec4d>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace osgEarthPrestige::Labeling;

bool osgEarthPrestige::Labeling::placePath(const Record& record, const CameraSnapshot& camera, Placement& output)
{
    if (!record.path || !camera.valid()) return false;
    const auto& path = *record.path;
    const auto matrix = camera.worldToView * camera.projection;
    const auto eye = osg::Matrixd::inverse(camera.worldToView).getTrans();
    struct Vertex { osg::Vec3d world; osg::Vec2d pixel; double w; };
    std::vector<Vertex> vertices;
    vertices.reserve(path.world.size());
    for (const auto& world : path.world)
    {
        const auto clip = osg::Vec4d(world, 1) * matrix;
        const osg::Vec3d normal(world.x(), world.y(), world.z() * 1.006739496742);
        if (clip.w() <= 0 || !std::isfinite(clip.w()) || (camera.geocentric && normal * (eye - world) <= 0))
            return false;
        osg::Vec2d pixel(camera.x + (clip.x() / clip.w() + 1) * camera.width * 0.5,
            camera.y + (clip.y() / clip.w() + 1) * camera.height * 0.5);
        if (!std::isfinite(pixel.x()) || !std::isfinite(pixel.y())) return false;
        if (vertices.empty() || (pixel - vertices.back().pixel).length2() > 0.0001)
            vertices.push_back({world, pixel, clip.w()});
    }
    if (vertices.size() < 2) return false;
    std::vector<double> lengths(vertices.size(), 0);
    //! Rebuilds the small arc-length table after an upright direction reversal.
    auto measure = [&]()
    {
        for (std::size_t i = 1; i < vertices.size(); ++i)
            lengths[i] = lengths[i - 1] + (vertices[i].pixel - vertices[i - 1].pixel).length();
    };
    measure();
    const double width = path.width * camera.devicePixelRatio;
    const double margin = 4 * camera.devicePixelRatio;
    if (lengths.back() < width + 2 * margin) return false;
    if (!std::isfinite(lengths.back()) || !std::isfinite(width) || !std::isfinite(margin)) return false;
    const double start = (lengths.back() - width) * 0.5;
    osg::Vec2d sampledSegment;
    //! Samples the polyline in screen arc length, retaining its segment for perspective-correct world interpolation.
    auto sample = [&](double distance, osg::Vec2d& pixel, osg::Vec2d& direction, PathPlacement::Glyph& glyph)
    {
        if (distance < margin || distance > lengths.back() - margin) return false;
        auto end = std::lower_bound(lengths.begin() + 1, lengths.end(), distance);
        const auto i = std::min<std::size_t>(end - lengths.begin(), vertices.size() - 1);
        const auto& a = vertices[i - 1]; const auto& b = vertices[i];
        const double t = (distance - lengths[i - 1]) / (lengths[i] - lengths[i - 1]);
        const double u = t * a.w / ((1 - t) * b.w + t * a.w);
        pixel = a.pixel + (b.pixel - a.pixel) * t;
        direction = b.pixel - a.pixel; direction.normalize();
        sampledSegment = direction;
        if (path.gpu)
        {
            auto left = direction, right = direction;
            if (i > 1)
            {
                auto before = a.pixel - vertices[i - 2].pixel; before.normalize();
                if (before * direction > -0.99) { left += before; left.normalize(); }
            }
            if (i + 1 < vertices.size())
            {
                auto after = vertices[i + 1].pixel - b.pixel; after.normalize();
                if (after * direction > -0.99) { right += after; right.normalize(); }
            }
            direction = left * (1 - t) + right * t; direction.normalize();
        }
        glyph.world = a.world + (b.world - a.world) * u;
        glyph.tangent = b.world - a.world; glyph.tangent.normalize();
        return true;
    };
    osg::Vec2d center, direction;
    PathPlacement::Glyph unused;
    if (!sample(lengths.back() * 0.5, center, direction, unused)) return false;
    if (sampledSegment.x() < -0.01 || (std::abs(sampledSegment.x()) <= 0.01 && sampledSegment.y() < 0))
    { std::reverse(vertices.begin(), vertices.end()); measure(); }
    auto placed = std::make_shared<PathPlacement>();
    placed->glyphs.reserve(path.glyphs.size());
    osg::Vec2d lower(std::numeric_limits<double>::max(), std::numeric_limits<double>::max()), upper(-lower.x(), -lower.y());
    osg::Vec2d previous, first;
    for (const auto& item : path.glyphs)
    {
        osg::Vec2d pixel, tangent;
        PathPlacement::Glyph glyph;
        if (!sample(start + item.distance * camera.devicePixelRatio, pixel, tangent, glyph)) return false;
        // Keep the whole shaped run, or reject it: never truncate a road name at a bend or tile boundary.
        if (!placed->glyphs.empty() &&
            (previous * sampledSegment < 0.866025403784 || first * sampledSegment < 0.5)) return false;
        if (placed->glyphs.empty()) first = sampledSegment;
        previous = sampledSegment;
        for (unsigned corner = 0; corner < 4; ++corner)
        {
            const double x = (corner & 1 ? item.upperRight.x() : item.lowerLeft.x()) * camera.devicePixelRatio;
            const double y = (corner & 2 ? item.upperRight.y() : item.lowerLeft.y()) * camera.devicePixelRatio;
            const osg::Vec2d p = pixel + osg::Vec2d(tangent.x() * x - tangent.y() * y,
                tangent.y() * x + tangent.x() * y);
            for (unsigned axis = 0; axis < 2; ++axis)
            { lower[axis] = std::min(lower[axis], p[axis]); upper[axis] = std::max(upper[axis], p[axis]); }
        }
        placed->glyphs.push_back(glyph);
    }
    if (lower.x() < camera.x || lower.y() < camera.y || upper.x() > camera.x + camera.width ||
        upper.y() > camera.y + camera.height) return false;
    const auto anchor = osg::Vec4d(record.worldAnchor, 1) * matrix;
    if (anchor.w() <= 0) return false;
    output.anchorPixels.set(camera.x + (anchor.x() / anchor.w() + 1) * camera.width * 0.5,
        camera.y + (anchor.y() / anchor.w() + 1) * camera.height * 0.5);
    output.lowerLeft = lower; output.upperRight = upper; output.path = std::move(placed);
    return true;
}
