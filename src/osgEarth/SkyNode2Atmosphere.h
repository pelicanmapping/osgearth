/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once

// Internal numerical model shared by construction, tests and benchmarks.
#include <osg/Image>
#include <osg/Vec3d>
#include <osg/Vec2d>
#include <algorithm>
#include <cmath>

namespace osgEarth { namespace Sky2Atmosphere
{
    constexpr double radius = 6360.0; // kilometers, ellipsoid-scaled Earth coordinates
    constexpr double top = 6460.0;
    constexpr unsigned width = 256, height = 64;

    //! Returns the forward exit from a sphere, or -1 for a miss. Direction must be unit length.
    inline double exitDistance(double r, double mu, double boundary)
    {
        double d = r*r*(mu*mu-1.0) + boundary*boundary;
        return d < 0.0 ? -1.0 : -r*mu + std::sqrt(d);
    }

    //! Tests occultation by solid Earth, excluding a tangent ray at the surface.
    inline bool occluded(double r, double mu)
    {
        return mu < 0.0 && r*r*(1.0-mu*mu) < radius*radius;
    }

    //! Evaluates extinction and scattering in inverse kilometers, including ozone absorption.
    inline void medium(double altitude, osg::Vec3d& extinction, osg::Vec3d& rayleigh, double& mie)
    {
        double h = std::max(0.0, altitude);
        rayleigh = osg::Vec3d(0.005802, 0.013558, 0.033100) * std::exp(-h/8.0);
        mie = 0.003996 * std::exp(-h/1.2);
        double ozone = std::max(0.0, 1.0-std::abs(h-25.0)/15.0);
        extinction = rayleigh + osg::Vec3d(1,1,1)*(mie/0.9) + osg::Vec3d(0.000650,0.001881,0.000085)*ozone;
    }

    //! Integrates Beer-Lambert transmittance without a LUT; used as an independent high-sample reference.
    inline osg::Vec3d integrate(double r, double mu, unsigned samples)
    {
        if (occluded(r, mu)) return osg::Vec3d();
        double distance = std::max(0.0, exitDistance(r, mu, top));
        osg::Vec3d opticalDepth;
        for (unsigned i=0; i<samples; ++i)
        {
            double t = distance*(i+0.5)/samples;
            double h = std::sqrt(r*r+t*t+2.0*r*mu*t)-radius;
            osg::Vec3d extinction, rayleigh;
            double mie;
            medium(h, extinction, rayleigh, mie);
            opticalDepth += extinction*(distance/samples);
        }
        return osg::Vec3d(std::exp(-opticalDepth.x()), std::exp(-opticalDepth.y()), std::exp(-opticalDepth.z()));
    }

    //! Decodes the horizon-resolving transmittance parameterization at unit-square coordinates.
    inline void decode(double u, double v, double& r, double& mu)
    {
        double H = std::sqrt(top*top-radius*radius), rho = H*v;
        r = std::sqrt(rho*rho+radius*radius);
        double d = top-r + u*(rho+H-(top-r));
        mu = d < 1e-8 ? 1.0 : std::max(-1.0, std::min(1.0, (H*H-rho*rho-d*d)/(2.0*r*d)));
    }

    //! Encodes an unoccluded atmospheric ray for a bilinear transmittance lookup.
    inline osg::Vec2d encode(double r, double mu)
    {
        r = std::max(radius, std::min(top, r));
        double H = std::sqrt(top*top-radius*radius), rho = std::sqrt(std::max(0.0, r*r-radius*radius));
        double d = std::max(0.0, exitDistance(r, mu, top));
        return osg::Vec2d((d-(top-r))/(rho+H-(top-r)), rho/H);
    }

    //! Builds an immutable RGB32F transmittance table; callers own the returned image.
    inline osg::ref_ptr<osg::Image> createTransmittance()
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(width, height, 1, GL_RGB, GL_FLOAT);
        for (unsigned y=0; y<height; ++y)
        for (unsigned x=0; x<width; ++x)
        {
            double r, mu;
            decode(double(x)/(width-1), double(y)/(height-1), r, mu);
            // The last column is tangent; avoid classifying roundoff as solid Earth.
            auto t = integrate(r, x == width-1 ? mu+1e-10 : mu, 128);
            auto p = reinterpret_cast<float*>(image->data(x,y));
            for (unsigned c=0; c<3; ++c) p[c] = float(t[c]);
        }
        return image;
    }

    //! Samples the production table on the CPU, with the same texel-center mapping as GLSL.
    inline osg::Vec3d sample(const osg::Image& image, double r, double mu)
    {
        if (occluded(r, mu)) return osg::Vec3d();
        auto uv = encode(r, mu);
        double x = std::max(0.0, std::min(double(width-1), uv.x()*(width-1)));
        double y = std::max(0.0, std::min(double(height-1), uv.y()*(height-1)));
        unsigned ix = unsigned(x), iy = unsigned(y);
        osg::Vec3d value;
        for (unsigned j=0; j<2; ++j)
        for (unsigned i=0; i<2; ++i)
        {
            auto p = reinterpret_cast<const float*>(image.data(std::min(ix+i,width-1), std::min(iy+j,height-1)));
            double weight = (i ? x-ix : 1.0-(x-ix))*(j ? y-iy : 1.0-(y-iy));
            value += osg::Vec3d(p[0],p[1],p[2])*weight;
        }
        return value;
    }

    //! Packs transmittance and isotropic multiple scattering into one immutable texture.
    // The geometric-series closure follows Hillaire (2020); this is a fresh implementation.
    inline osg::ref_ptr<osg::Image> createAtlas()
    {
        auto transmittance = createTransmittance();
        osg::ref_ptr<osg::Image> atlas = new osg::Image;
        atlas->allocateImage(width, height+32, 1, GL_RGB, GL_FLOAT);
        std::fill(reinterpret_cast<float*>(atlas->data()),
            reinterpret_cast<float*>(atlas->data())+width*(height+32)*3, 0.0f);
        std::copy(transmittance->data(), transmittance->data()+transmittance->getTotalSizeInBytes(), atlas->data());
        for (unsigned y=0; y<32; ++y)
        for (unsigned x=0; x<32; ++x)
        {
            double r = radius+0.01+(top-radius-0.02)*y/31.0;
            double sunMu = 2.0*x/31.0-1.0;
            osg::Vec3d sun(std::sqrt(std::max(0.0, 1.0-sunMu*sunMu)), 0.0, sunMu);
            osg::Vec3d total, feedback;
            for (unsigned d=0; d<32; ++d)
            {
                double z = 1.0-2.0*(d+0.5)/32.0, phi = d*2.399963229728653;
                double xy = std::sqrt(1.0-z*z);
                osg::Vec3d direction(xy*std::cos(phi), xy*std::sin(phi), z);
                bool ground = occluded(r, z);
                double distance = ground ? -r*z-std::sqrt(r*r*(z*z-1.0)+radius*radius) : exitDistance(r,z,top);
                osg::Vec3d throughput(1,1,1);
                for (unsigned s=0; s<24; ++s)
                {
                    double t = distance*(s+0.5)/24.0, ds = distance/24.0;
                    osg::Vec3d p = osg::Vec3d(0,0,r)+direction*t;
                    double pr = p.length();
                    osg::Vec3d extinction, rayleigh;
                    double mie;
                    medium(pr-radius, extinction, rayleigh, mie);
                    auto sunlight = sample(*transmittance, pr, p*sun/pr);
                    for (unsigned c=0; c<3; ++c)
                    {
                        double stepT = std::exp(-extinction[c]*ds);
                        double weight = throughput[c]*(1.0-stepT)/std::max(extinction[c],1e-9);
                        double scattering = rayleigh[c]+mie;
                        total[c] += weight*scattering*sunlight[c]/(4.0*osg::PI*32.0);
                        feedback[c] += weight*scattering/32.0;
                        throughput[c] *= stepT;
                    }
                }
                if (ground)
                {
                    osg::Vec3d p = osg::Vec3d(0,0,r)+direction*distance;
                    double mu = p*sun/radius;
                    auto sunlight = sample(*transmittance, radius+0.01, mu);
                    for (unsigned c=0; c<3; ++c)
                        total[c] += throughput[c]*sunlight[c]*std::max(0.0,mu)*0.1/(osg::PI*32.0);
                }
            }
            auto p = reinterpret_cast<float*>(atlas->data(x,y+height));
            for (unsigned c=0; c<3; ++c) p[c] = float(total[c]/std::max(0.05,1.0-feedback[c]));
        }
        return atlas;
    }
} }
