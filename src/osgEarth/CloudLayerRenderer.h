/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarth/CloudLayer>
#include <osgEarth/TerrainResources>
#include <osg/StateSet>
#include <osg/Uniform>
#include <osgUtil/CullVisitor>
#include <memory>

namespace osgEarth
{
    /** Internal adapter boundary. The host supplies physical lighting GLSL and camera coordinates;
     * the cloud renderer never modifies the host's camera, framebuffer, exposure, or scene graph.
     */
    class CloudLayerRenderer : public osg::Referenced
    {
    public:
        struct Frame
        {
            osg::Vec3 eye, sun;
            osg::Matrix3 basis;
            osg::Matrix3 viewToEarth, earthToView;
            osg::Matrixf projection, inverseProjection;
            float radius = 6360.0f; // Host's ellipsoid-scaled kilometers.
            float horizon = 1.5707963f;
            double time = 0.0;
            bool airScattering = false; // Host enables optional rays only when its air scattering is active.
        };
        //! Reserves two sampler units; optional rays reserve one more lazily. Host GLSL also supplies airSource.
        CloudLayerRenderer(CloudLayer*, TerrainResources*, const std::string& lightingGLSL);
        //! Reports whether GL compute and the required texture-unit reservations are available.
        bool valid() const;
        //! Schedules bounded compute work at order -102; returned state must enclose subsequent scene/environment draws.
        osg::StateSet* cull(osgUtil::CullVisitor&, const Frame&, osg::StateSet*& environmentState);
        //! Releases all off-graph resources and invalidates cached results for the specified context.
        void releaseGLObjects(osg::State*) const;
        //! Resizes per-context storage on owned off-graph drawables.
        void resizeGLObjectBuffers(unsigned);
    protected:
        //! Releases per-view resources and sampler reservations.
        ~CloudLayerRenderer() override;
    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
