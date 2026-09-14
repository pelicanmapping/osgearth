/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarth/Chonk>
#include <osgEarth/Capabilities>
#include <osgEarth/CullingUtils>
#include <osgEarth/ExternalNode>
#include <osgEarth/InstancedExternalNode>
#include <osgEarth/FileUtils>
#include <osgEarth/Registry>
#include <osgEarth/ShaderGenerator>
#include <osgEarth/VirtualProgram>
#include <osg/MatrixTransform>
#include <osg/GraphicsContext>
#include <osgViewer/Viewer>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgUtil/UpdateVisitor>
#include <cstdio>

namespace ChonkTest
{
    inline osg::ref_ptr<osg::Geometry> mesh(unsigned divisions = 1u)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry();
        auto vertices = new osg::Vec3Array();
        auto uv = new osg::Vec2Array();
        for (unsigned y = 0; y <= divisions; ++y)
            for (unsigned x = 0; x <= divisions; ++x)
            {
                const float u = float(x)/divisions, v = float(y)/divisions;
                vertices->push_back(osg::Vec3(4.0f*u - 2.0f, 4.0f*v - 2.0f, 0.0f));
                uv->push_back(osg::Vec2(u, v));
            }
        auto indices = new osg::DrawElementsUInt(GL_TRIANGLES);
        for (unsigned y = 0; y < divisions; ++y)
            for (unsigned x = 0; x < divisions; ++x)
            {
                const unsigned a = y*(divisions+1)+x, b = a+divisions+1;
                for (unsigned i : {a, a+1, b+1, a, b+1, b}) indices->push_back(i);
            }
        geometry->setVertexArray(vertices);
        geometry->setTexCoordArray(0, uv);
        auto normals = new osg::Vec3Array();
        normals->push_back(osg::Vec3(0,0,1));
        geometry->setNormalArray(normals, osg::Array::BIND_OVERALL);
        auto colors = new osg::Vec4Array();
        colors->push_back(osg::Vec4(0.2f,0.6f,1.0f,1.0f));
        geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
        geometry->addPrimitiveSet(indices);
        geometry->setUseDisplayList(false);
        geometry->setUseVertexBufferObjects(true);
        return geometry;
    }

    struct AssetFile
    {
        std::string path = osgEarth::Util::getTempName("chonk-", ".osgb");
        bool write(osg::Node* node)
        {
            if (node->asDrawable())
            {
                osg::ref_ptr<osg::Geode> geode = new osg::Geode();
                geode->addDrawable(node->asDrawable());
                return osgDB::writeNodeFile(*geode, path);
            }
            return osgDB::writeNodeFile(*node, path);
        }
        ~AssetFile() { std::remove(path.c_str()); }
    };

    struct FindDrawables : osg::NodeVisitor
    {
        std::vector<osg::ref_ptr<osgEarth::ChonkDrawable>> drawables;
        FindDrawables() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { }
        void apply(osg::Geometry& g) override
        {
            if (auto* chonk = dynamic_cast<osgEarth::ChonkDrawable*>(&g)) drawables.emplace_back(chonk);
        }
    };

    inline void update(osg::Node* node)
    {
        osgUtil::UpdateVisitor visitor;
        node->accept(visitor);
    }

    struct Renderer
    {
        osg::ref_ptr<osg::GraphicsContext> context;
        osgViewer::Viewer viewer;
        osg::ref_ptr<osg::Group> root = new osg::Group();
        osg::ref_ptr<osgEarth::TextureArena> textures = new osgEarth::TextureArena();
        std::shared_ptr<osgEarth::ChonkFactory> factory = std::make_shared<osgEarth::ChonkFactory>(textures.get());
        double time = 10.0;

        bool initialize()
        {
            osgEarth::Capabilities::get();
            auto traits = new osg::GraphicsContext::Traits(osg::DisplaySettings::instance());
            traits->readDISPLAY();
            traits->setUndefinedScreenDetailsToDefaultScreen();
            traits->width = traits->height = 256;
            traits->pbuffer = true;
            traits->doubleBuffer = false;
            context = osg::GraphicsContext::createGraphicsContext(traits);
            if (!context || !context->realize() || !context->makeCurrent()) return false;
            context->getState()->setUseVertexAttributeAliasing(true);
            context->getState()->setUseModelViewAndProjectionUniforms(true);
            viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
            auto* camera = viewer.getCamera();
            camera->setGraphicsContext(context);
            camera->setViewport(0, 0, 256, 256);
            camera->setDrawBuffer(GL_FRONT);
            camera->setReadBuffer(GL_FRONT);
            camera->setClearColor(osg::Vec4(0,0,0,1));
            camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
            camera->addCullCallback(new osgEarth::InstallCameraUniform());
            camera->setProjectionMatrixAsPerspective(45.0, 1.0, 1.0, 10000.0);
            camera->setViewMatrixAsLookAt(osg::Vec3d(0,0,100), osg::Vec3d(), osg::Vec3d(0,1,0));
            root->getOrCreateStateSet()->setAttribute(textures);
            root->getOrCreateStateSet()->addUniform(new osg::Uniform("oe_sse", 0.0f));
            root->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
            osgEarth::VirtualProgram::getOrCreate(root->getOrCreateStateSet())->setFunction(
                "chonk_test_color", "void chonk_test_color(inout vec4 c) { }",
                osgEarth::VirtualProgram::LOCATION_FRAGMENT_COLORING);
            viewer.setSceneData(root);
            viewer.realize();
            return true;
        }

        void setScene(osg::Node* node)
        {
            root->removeChildren(0, root->getNumChildren());
            root->addChild(node);
        }

        void frame()
        {
            viewer.frame(time);
            time += 1.0/60.0;
            context->makeCurrent();
            glFinish(); // include completed GPU work, not just command submission
        }

        osg::ref_ptr<osg::Image> pixels()
        {
            osg::ref_ptr<osg::Image> result = new osg::Image();
            result->readPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE);
            return result;
        }
    };
}
