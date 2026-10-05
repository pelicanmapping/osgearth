/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarthPrestige/Export>
#include <osgEarth/Chonk>
#include <osgEarth/Progress>
#include <osgEarth/Status>

namespace osgEarthPrestige
{
    using namespace osgEarth;
    //! One accepted, terrain-clamped tree. Matrix maps tree space to the page frame with rotation and positive uniform scale.
    struct TreeCardPlacement
    {
        osg::Matrixd transform;
        unsigned model = 0;
    };

    //! One instanced group of a single species; offset addresses four vec4 records per tree in the page table.
    struct TreeCardCluster
    {
        osg::Matrixf transform;
        unsigned model = 0, offset = 0, count = 0;
    };

    //! Encodes count/offset and tier without enlarging instance records. Round(-UV.x) remains the member count;
    //! the exact quarter fraction tags far clusters for visualization, independent of GPU compaction order.
    OSGEARTHPRESTIGE_EXPORT osg::Vec2f treeCardInstanceUV(const TreeCardCluster&, bool far);

    //! Builds shared slot geometry from a single static coarse model (1..128 triangles), without stretching art.
    //! Bounds are a unit cube; the cluster shader expands vertices using independently clamped tree transforms.
    //! CPU-only; caller retains the source's accounting lease. Invalid input clears output.
    OSGEARTHPRESTIGE_EXPORT Status createTreeCardTemplate(const Chonk&, unsigned slots, Chonk::Ptr& output);

    //! Spatially partitions each species into groups of at most slots trees. Every placement appears exactly once.
    //! Models supply conservative authored bounds; no tree is moved, resized, or thinned. CPU-only, worker-safe.
    //! Produces a 64-byte affine transform and LOD sphere per tree; errors/cancellation clear both outputs.
    OSGEARTHPRESTIGE_EXPORT Status buildTreeCardClusters(const std::vector<TreeCardPlacement>&,
        const std::vector<Chonk::Ptr>& models, unsigned slots, std::vector<TreeCardCluster>&,
        std::vector<osg::Vec4f>& records, ProgressCallback* = nullptr);

    //! Installs shared expansion and member range/optional size fades; ordinary nonnegative UVs bypass it.
    //! Uses ChonkDrawable member-LOD policy even without GPU spatial culling, collapsing rejected cards before rasterization.
    OSGEARTHPRESTIGE_EXPORT void installTreeCardShader(osg::StateSet*);
}
