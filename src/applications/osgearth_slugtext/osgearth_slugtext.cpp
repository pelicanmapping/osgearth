/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

// Demo application for SlugText rendering.
// Renders text using the Slug algorithm (Lengyel 2017) which produces
// pixel-accurate results at any scale, rotation, or perspective.
//
// Usage: osgearth_slugtext [--font <path>] [--text <string>]

#include <osgViewer/Viewer>
#include <osgEarth/SlugText>
#include <osgEarth/GLUtils>
#include <osg/MatrixTransform>
#include <osg/Camera>
#include <iostream>

using namespace osgEarth;

int usage(const char* name, const char* msg)
{
    std::cerr << "Error: " << msg << std::endl;
    std::cerr << "Usage: " << name << " [--font <path.ttf>] [--text <string>]" << std::endl;
    return -1;
}

osg::Camera* createHUDCamera(int width, int height)
{
    osg::Camera* camera = new osg::Camera();
    camera->setProjectionMatrix(osg::Matrix::ortho2D(0, width, 0, height));
    camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
    camera->setViewMatrix(osg::Matrix::identity());
    camera->setClearMask(GL_DEPTH_BUFFER_BIT);
    camera->setRenderOrder(osg::Camera::POST_RENDER);
    camera->setAllowEventFocus(false);
    return camera;
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc, argv);
    osgEarth::initialize(arguments);

    std::string fontPath;
    if (!arguments.read("--font", fontPath))
    {
        // Try common font paths
#ifdef _WIN32
        fontPath = "C:/Windows/Fonts/arial.ttf";
#elif __APPLE__
        fontPath = "/Library/Fonts/Arial.ttf";
#else
        fontPath = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif
    }

    std::string text;
    if (!arguments.read("--text", text))
    {
        text = "Hello, Slug!";
    }

    osgViewer::Viewer viewer(arguments);

    int width = 1280, height = 720;
    osg::Group* root = new osg::Group();

    // Create an orthographic HUD camera for 2D text display
    osg::Camera* hud = createHUDCamera(width, height);
    root->addChild(hud);

    // Large text
    {
        auto* mt = new osg::MatrixTransform();
        mt->setMatrix(osg::Matrix::translate(50, height - 100, 0));
        auto* st = new SlugText(text, fontPath, 48.0f);
        st->setColor(osg::Vec4(1, 1, 1, 1));
        mt->addChild(st);
        hud->addChild(mt);
    }

    // Medium text
    {
        auto* mt = new osg::MatrixTransform();
        mt->setMatrix(osg::Matrix::translate(50, height - 180, 0));
        auto* st = new SlugText("Resolution Independent Text", fontPath, 32.0f);
        st->setColor(osg::Vec4(0.8f, 0.9f, 1.0f, 1.0f));
        mt->addChild(st);
        hud->addChild(mt);
    }

    // Small text
    {
        auto* mt = new osg::MatrixTransform();
        mt->setMatrix(osg::Matrix::translate(50, height - 240, 0));
        auto* st = new SlugText("Pixel Perfect at Any Scale", fontPath, 16.0f);
        st->setColor(osg::Vec4(1.0f, 0.9f, 0.7f, 1.0f));
        mt->addChild(st);
        hud->addChild(mt);
    }

    // 3D perspective text (rotated)
    {
        auto* mt = new osg::MatrixTransform();
        osg::Matrix m;
        m.makeRotate(osg::DegreesToRadians(15.0), osg::Vec3(0, 0, 1));
        m.setTrans(osg::Vec3(50, height - 350, 0));
        mt->setMatrix(m);
        auto* st = new SlugText("Rotated Text", fontPath, 36.0f);
        st->setColor(osg::Vec4(0.5f, 1.0f, 0.5f, 1.0f));
        mt->addChild(st);
        hud->addChild(mt);
    }

    // Very large text to show resolution independence
    {
        auto* mt = new osg::MatrixTransform();
        mt->setMatrix(osg::Matrix::translate(50, 80, 0));
        auto* st = new SlugText("BIG", fontPath, 200.0f);
        st->setColor(osg::Vec4(1.0f, 0.5f, 0.3f, 0.5f));
        mt->addChild(st);
        hud->addChild(mt);
    }

    viewer.setSceneData(root);

#ifndef OSG_GL3_AVAILABLE
    viewer.setRealizeOperation(new osgEarth::GL3RealizeOperation());
#endif

    return viewer.run();
}
