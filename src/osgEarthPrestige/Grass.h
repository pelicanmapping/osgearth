/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarthPrestige/Scatter>
#include <osg/Node>
#include <osg/StateSet>

namespace osgEarthPrestige
{
    using namespace osgEarth;
    //! Builds shared blade topology and conservative proxy bounds, not CPU blade positions; invalid policies return null.
    //! Worker-safe. The returned geometry requires installGrassShader and Chonk's source/visibility tables.
    OSGEARTHPRESTIGE_EXPORT osg::ref_ptr<osg::Node> createGrassPatch(const ScatterGroup&, unsigned lod);

    //! Installs the opt-in patch expansion shader and immutable uniforms before publishing population state.
    //! Uses local_uv.y as a 24-bit seed. CPU instance transforms supply the sampled ground plane.
    OSGEARTHPRESTIGE_EXPORT void installGrassShader(osg::StateSet*, const ScatterGroup&);
}
