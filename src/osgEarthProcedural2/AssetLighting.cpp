/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "AssetLighting.h"
#include <osgEarth/VirtualProgram>

void osgEarth::Procedural2::installAssetLighting(osg::StateSet* state)
{
    VirtualProgram::getOrCreate(state)->setFunction("oe_p2_leaf_normal", R"glsl(
        in vec3 vp_Normal;
        flat in uint oe_normal_technique;
        void oe_p2_leaf_normal(inout vec4 color)
        {
            // Only real leaf surfaces turn their normal on the back face. An impostor's
            // volume normals describe the crown, so turning them would point the crown downward.
            if (!gl_FrontFacing && oe_normal_technique == 0u) vp_Normal = -vp_Normal;
        }
    )glsl", VirtualProgram::LOCATION_FRAGMENT_LIGHTING, -1.0f);
}
