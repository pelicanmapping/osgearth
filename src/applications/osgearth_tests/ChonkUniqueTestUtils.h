/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include "ChonkTestUtils.h"
#include <cmath>

namespace ChonkTest
{
    // Count actual rendering leaves, including masked fallback branches. Packed
    // geometry must disappear from the ordinary OSG drawable population.
    struct DrawCounts : osg::NodeVisitor
    {
        std::size_t ordinary = 0, chonks = 0, batches = 0, placements = 0;
        std::size_t ordinaryDraws = 0;
        // Inspect all structural leaves without changing traversal/render state.
        DrawCounts() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { setNodeMaskOverride(~0u); }
        // Each Chonk contributes one multi-draw leaf, irrespective of mesh count.
        void apply(osg::Geometry& geometry) override
        {
            if (auto* drawable = dynamic_cast<osgEarth::ChonkDrawable*>(&geometry))
            {
                ++chonks;
                batches += drawable->getNumBatches();
                placements += drawable->getNumInstances();
            }
            else
            {
                ++ordinary;
                ordinaryDraws += geometry.getNumPrimitiveSets();
            }
        }
    };

    // Deterministic, genuinely different meshes with disjoint placements. Keep
    // the original graph alive so the legacy factory cache has stable keys.
    struct UniqueScene
    {
        osg::ref_ptr<osg::Group> root = new osg::Group();
        std::vector<osg::ref_ptr<osg::Geometry>> meshes;
        std::vector<osg::Matrixf> transforms;
        float width;

        // Construct a grid in cell-local coordinates, varying vertex data per mesh.
        UniqueScene(unsigned count, unsigned divisions = 2)
        {
            const unsigned side = unsigned(std::ceil(std::sqrt(double(count))));
            width = float(side) * 8.0f;
            for (unsigned i = 0; i < count; ++i)
            {
                auto geometry = mesh(divisions);
                auto* vertices = static_cast<osg::Vec3Array*>(geometry->getVertexArray());
                const float scale = 0.7f + 0.3f * float(i + 1) / float(count);
                for (auto& v : *vertices) v.x() *= scale;
                osg::Matrixf transform = osg::Matrixf::translate(
                    float(i % side)*8.0f - width*0.5f + 4.0f,
                    float(i / side)*8.0f - width*0.5f + 4.0f, 0.0f);
                osg::ref_ptr<osg::MatrixTransform> placement = new osg::MatrixTransform(transform);
                placement->addChild(geometry);
                root->addChild(placement);
                meshes.push_back(geometry);
                transforms.push_back(transform);
            }
        }
    };

    // Compare rendered RGB values, tolerating only 8-bit rounding differences.
    inline unsigned differentPixels(const osg::Image& lhs, const osg::Image& rhs)
    {
        if (lhs.s() != rhs.s() || lhs.t() != rhs.t()) return ~0u;
        unsigned count = 0;
        for (int y = 0; y < lhs.t(); ++y)
            for (int x = 0; x < lhs.s(); ++x)
                for (unsigned c = 0; c < 3; ++c)
                    if (std::abs(int(lhs.data(x,y)[c]) - int(rhs.data(x,y)[c])) > 2)
                    { ++count; break; }
        return count;
    }
}
