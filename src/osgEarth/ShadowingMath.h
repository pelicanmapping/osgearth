/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarth/Math>
#include <osg/BoundingBox>
#include <array>
#include <cmath>

namespace osgEarth { namespace ShadowMath
{
    using Corners = std::array<osg::Vec3d,8>;

    //! Extracts an asymmetric perspective or orthographic slice in view space without heap allocation.
    //! Uses the requested receiver interval, including geometry exposed beyond nominal clip planes by log depth.
    //! Returns false for empty intervals or invalid projections; accepts standard and reverse-Z matrices.
    inline bool corners(const osg::Matrixd& projection, double nearRange, double farRange, Corners& out)
    {
        double l, r, b, t, n, f;
        bool ortho = ProjectionMatrix::isOrtho(projection);
        if (!(ortho ? ProjectionMatrix::getOrtho(projection,l,r,b,t,n,f) :
            ProjectionMatrix::getPerspective(projection,l,r,b,t,n,f))) return false;
        if (!std::isfinite(n) || (!ortho && n <= 0.0) || !std::isfinite(l+r+b+t)) return false;
        // The cull projection's clip planes can change later, and logarithmic depth can expose closer receivers.
        // Only its angular shape (or orthographic footprint) constrains the shadow coverage.
        if (!std::isfinite(nearRange+farRange) || nearRange < 0.0 || farRange <= nearRange) return false;
        for (unsigned i=0; i<8; ++i)
        {
            double z = i < 4 ? nearRange : farRange;
            double scale = ortho ? 1.0 : z/n;
            out[i].set((i&1 ? r : l)*scale,(i&2 ? t : b)*scale,-z);
        }
        return true;
    }

    struct Cascade
    {
        osg::Matrixd view, projection, viewToTexture;
        double texelSize = 0.0;
        double depthSpan = 0.0;
    };

    //! Fits a rotation-invariant sphere and snaps it to a world-anchored light texel grid in double precision.
    //! lightDirection points toward the sun; extrusion includes upstream casters beyond the receiver slice.
    inline bool fit(const Corners& points, const osg::Matrixd& inverseView, osg::Vec3d lightDirection,
        unsigned resolution, double extrusion, double filterRadius, Cascade& out)
    {
        double length = lightDirection.length();
        if (!std::isfinite(length) || length < 1e-12 || resolution < 16) return false;
        lightDirection /= length;
        osg::Vec3d up = std::abs(lightDirection.z()) < 0.95 ? osg::Vec3d(0,0,1) : osg::Vec3d(0,1,0);
        osg::Vec3d side = up ^ lightDirection;
        side.normalize();
        up = lightDirection ^ side;
        osg::Vec3d center;
        for (const auto& p : points) center += p;
        center *= 0.125;
        double radius = 0.0;
        for (const auto& p : points) radius = std::max(radius,(p-center).length());
        // Quantization and a filter guard keep the fit stable and all receivers inside the PCF footprint.
        radius = std::ceil(radius*16.0)/16.0;
        double guard = std::min(double(resolution)*0.25,std::max(1.0,filterRadius)+1.0);
        radius *= double(resolution)/(double(resolution)-2.0*guard);
        if (!std::isfinite(radius) || radius <= 0.0) return false;
        out.texelSize = 2.0*radius/resolution;
        osg::Vec3d worldCenter = center*inverseView;
        double x = worldCenter*side, y = worldCenter*up;
        worldCenter += side*(std::floor(x/out.texelSize+0.5)*out.texelSize-x);
        worldCenter += up*(std::floor(y/out.texelSize+0.5)*out.texelSize-y);
        out.view.makeLookAt(worldCenter,worldCenter-lightDirection,up);
        osg::Matrixd viewToLight = inverseView*out.view;
        osg::BoundingBoxd bounds;
        for (const auto& p : points) bounds.expandBy(p*viewToLight);
        double n = -bounds.zMax()-std::max(0.0,extrusion), f = -bounds.zMin()+out.texelSize;
        out.depthSpan = f-n;
        // Shadow textures always use conventional depth, independently of the main camera's depth convention.
        out.projection.makeOrtho(-radius,radius,-radius,radius,n,f);
        out.viewToTexture = viewToLight*out.projection*
            osg::Matrixd::translate(1,1,1)*osg::Matrixd::scale(0.5,0.5,0.5);
        return true;
    }
} }
