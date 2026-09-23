/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include "SkyNode2TestScene.h"

namespace osgEarth { namespace Sky2Tests
{
    //! Displays actual ground-shadow transmission over a fixed equatorial square; scene owns the returned state.
    inline osg::StateSet* groundShadowProbe(Scene& scene, unsigned width, unsigned height, float halfWidthKm)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(osg::Vec3(-1,-1,0));
        vertices->push_back(osg::Vec3(3,-1,0));
        vertices->push_back(osg::Vec3(-1,3,0));
        geometry->setVertexArray(vertices);
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
        geometry->setUseDisplayList(false);
        geometry->setUseVertexBufferObjects(true);
        geometry->setCullingActive(false);
        auto state = geometry->getOrCreateStateSet();
        state->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS));
        state->addUniform(new osg::Uniform("cloudTestArea",osg::Vec3(float(width),float(height),halfWidthKm)));
        ShaderLoader::load(VirtualProgram::getOrCreate(state),R"(
            #pragma vp_function cloudGroundClip, vertex_clip, 0.9
            in vec4 osg_Vertex;
            // Covers the framebuffer independently of camera position, orientation, and atmosphere.
            void cloudGroundClip(inout vec4 vertex) { vertex=vec4(osg_Vertex.xy,0,1); }
            [break]
            #pragma vp_function cloudGroundOutput, fragment_output, 0.99
            uniform vec3 cloudTestArea;
            uniform vec4 oe_cloud_shell;
            layout(location=0) out vec4 cloudGroundResult;
            float oe_cloud_shadow(vec3 position);
            // Receiver positions stay fixed in the world during camera and wind stability tests.
            void cloudGroundOutput(inout vec4 color)
            {
                vec2 xy=(gl_FragCoord.xy/cloudTestArea.xy*2.0-1.0)*cloudTestArea.z;
                vec3 p=normalize(vec3(oe_cloud_shell.x,xy))*(oe_cloud_shell.x+0.002);
                cloudGroundResult=color=vec4(vec3(oe_cloud_shadow(p)),1);
            }
        )");
        scene.sky->addChild(geometry);
        return state;
    }
} }
