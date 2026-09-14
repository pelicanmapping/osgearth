/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include "ChonkTestUtils.h"
#include <osgEarth/PBRMaterial>
#include <osgEarth/Shaders>
#include <osg/Texture2D>

namespace ChonkTest
{
    // Make a constant 2x2 RGBA fixture so texture sampling is independent of UVs.
    inline osg::ref_ptr<osg::Texture2D> solidTexture(const osg::Vec4& color)
    {
        auto image = new osg::Image();
        image->allocateImage(2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned c = 0; c < 4; ++c)
                image->data()[4*i+c] = static_cast<unsigned char>(color[c]*255.0f);
        auto texture = new osg::Texture2D(image);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        return texture;
    }

    // Build a 4x4 grid of five-map materials shared by image tests and benchmarks.
    // divisions controls vertex work; extra maps are ignored by ordinary rendering.
    inline osg::ref_ptr<osg::Group> materialScene(unsigned divisions)
    {
        auto result = new osg::Group();
        for (unsigned i = 0; i < 16; ++i)
        {
            auto geometry = mesh(divisions);
            geometry->setUserValue(CHONK_HINT_LINEAR_COLOR, true);
            auto ss = geometry->getOrCreateStateSet();
            auto material = new osgEarth::PBRTexture();
            material->albedo = solidTexture(osg::Vec4(.2f+.04f*i,.8f,.4f,1));
            material->normal = solidTexture(osg::Vec4(.5f,.5f,1,1));
            material->pbr = solidTexture(osg::Vec4(.5f,.3f+.03f*i,.9f,.2f));
            ss->setTextureAttribute(0, material);
            for (int j = 0; j < 2; ++j)
            {
                auto texture = solidTexture(osg::Vec4(.25f,.5f,.75f,1));
                texture->setUserValue(CHONK_HINT_EXTENDED_MATERIAL_SLOT, j);
                osgEarth::ShaderGenerator::setIgnoreHint(texture, true);
                ss->setTextureAttribute(3+j, texture);
            }
            auto transform = new osg::MatrixTransform(osg::Matrix::translate(
                4.0f*(int(i%4)-1.5f), 4.0f*(int(i/4)-1.5f), 0));
            transform->addChild(geometry);
            result->addChild(transform);
        }
        return result;
    }

    // Install a diagnostic shader and camera that expose normal/PBR differences
    // when comparing ordinary OSG rendering with the packed Chonk path.
    inline void materialShader(Renderer& renderer)
    {
        auto vp = osgEarth::VirtualProgram::getOrCreate(renderer.root->getOrCreateStateSet());
        osgEarth::Util::Shaders shaders;
        shaders.load(vp, shaders.PBR);
        vp->setFunction("material_test_values", R"(
            struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;
            in vec3 vp_Normal;
            // Encode material and normal contributions into the comparison image.
            void material_test_values(inout vec4 c) {
                c.rgb *= vec3(oe_pbr.roughness, oe_pbr.ao, .5+.5*oe_pbr.metal);
                c.rgb *= .5+.5*abs(normalize(vp_Normal));
            })", osgEarth::VirtualProgram::LOCATION_FRAGMENT_LIGHTING);
        renderer.viewer.getCamera()->setViewMatrixAsLookAt(
            osg::Vec3d(0,0,25), osg::Vec3d(), osg::Vec3d(0,1,0));
    }
}
