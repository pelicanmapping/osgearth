/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osg/Node>
#include <string>

namespace osgEarthPrestige
{
    //! Builds original, meter-scaled diagnostic geometry rooted at ground level; unknown names return null.
    //! CPU only. Names: trees, shrubs, grass, undergrowth, rocks, canopy1..8[-0..3] (unit-footprint aggregates).
    //! These are placeholders, not final art. Canopy geometry remains inside [-0.5,0.5] in X/Y for exclusion safety.
    //! LOD 0 is the original mesh; LOD 1 is a cheaper silhouette or reduced ground-cover mesh.
    osg::ref_ptr<osg::Node> createPlaceholderAsset(const std::string& name, unsigned lod = 0u);
}
