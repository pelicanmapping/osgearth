/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include "SkyNode2TestScene.h"
#include <osgEarth/Chonk>
#include <osgEarth/Shadowing>
#include <osgEarth/CullingUtils>
#include <iostream>
#include <cstdlib>

namespace osgEarth { namespace Shadow46Tests
{
    //! Builds a reproducible ECEF city with many independently culled casters and ordinary ground geometry.
    struct Scene : Sky2Tests::Scene
    {
        osg::ref_ptr<Util::ShadowCaster> shadows;
        osg::ref_ptr<ChonkDrawable> buildings;
        osg::ref_ptr<TextureArena> textures;

        //! Populates retained GPU geometry; all instances share one local coordinate system and material arena.
        Scene(unsigned count = 16384, unsigned variants = 1, unsigned width = 800, unsigned height = 600) :
            Sky2Tests::Scene(new SkyNode2(flatOptions()),width,height)
        {
            if (std::getenv("OSGEARTH_SHADOW_TEST_DEBUG"))
            {
                using Callback = void(GL_APIENTRY *)(GLenum,GLenum,GLuint,GLenum,GLsizei,const GLchar*,const void*);
                void(GL_APIENTRY * installCallback)(Callback,const void*) = nullptr;
                osg::setGLExtensionFuncPtr(installCallback,"glDebugMessageCallback");
                if (installCallback)
                {
                    installCallback(debugMessage,nullptr);
                    glEnable(GL_DEBUG_OUTPUT);
                    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
                }
            }
            models->removeChildren(0,models->getNumChildren());
            textures = new TextureArena;
            ChonkFactory factory(textures);
            buildings = new ChonkDrawable;
            std::vector<osg::ref_ptr<osg::ShapeDrawable>> sources;
            std::vector<Chonk::Ptr> meshes;
            for (unsigned i=0; i<variants; ++i)
            {
                float h = 6.0f+float(i%7);
                osg::ref_ptr<osg::ShapeDrawable> box = new osg::ShapeDrawable(new osg::Box(osg::Vec3(0,0,h/2),2,2,h));
                box->setColor(osg::Vec4(0.7,0.7,0.7,1));
                box->setUseDisplayList(false);
                box->setUseVertexBufferObjects(true);
                sources.push_back(box);
                meshes.push_back(factory.getOrCreateChonk(box));
            }
            unsigned side = unsigned(std::ceil(std::sqrt(double(count))));
            for (unsigned i=0; i<count; ++i)
                buildings->add(meshes[i%variants],osg::Matrixf::translate(
                    (float(i%side)-side*0.5f)*5.0f,(float(i/side)-side*0.5f)*5.0f,0));
            buildings->setUseGPUCulling(true);
            models->addChild(buildings);
            models->getOrCreateStateSet()->setAttribute(textures);
            models->getOrCreateStateSet()->addUniform(new osg::Uniform("oe_sse",0.0f));
            osg::ref_ptr<osg::ShapeDrawable> floor = new osg::ShapeDrawable(new osg::Box(osg::Vec3(0,0,-1),2000,2000,2));
            floor->setColor(osg::Vec4(0.7,0.7,0.7,1));
            floor->setUseDisplayList(false);
            floor->setUseVertexBufferObjects(true);
            models->addChild(floor);
            shadows = new Util::ShadowCaster;
            if (std::getenv("OSGEARTH_SHADOW_TEST_CORE")) shadows->setSubmission(Util::ShadowCaster::CORE);
            shadows->setTextureSize(1024);
            shadows->setCascades(3,600);
            shadows->setLight(sky->getSunLight());
            shadows->getShadowCastingGroup()->addChild(models);
            sky->removeChild(models);
            shadows->addChild(models);
            sky->addChild(shadows);
            viewer->getCamera()->addCullCallback(new InstallCameraUniform);
            cityView();
        }

        //! Uses flat lighting to isolate shadow rendering from atmospheric lookup generation.
        static SkyNode2::Options flatOptions()
        {
            SkyNode2::Options result;
            result.preset = SkyNode2::FLAT;
            return result;
        }

        //! Reports driver errors without stack collection, keeping large regression fixtures practical to diagnose.
        static void GL_APIENTRY debugMessage(GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar* text, const void*)
        {
            if (type == GL_DEBUG_TYPE_ERROR) std::cerr << "Shadow test GL error: " << text << '\n';
        }

        //! Moves the camera across the city without changing sunlight or geometry.
        void cityView(double offset = 0.0)
        {
            viewer->getCamera()->setViewMatrixAsLookAt(osg::Vec3d(6378267,-110+offset,-160),
                osg::Vec3d(6378137,offset,0),osg::Vec3d(1,0,0));
        }
    };
} }
