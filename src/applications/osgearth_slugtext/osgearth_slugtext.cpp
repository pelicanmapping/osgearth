/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

// Demo: side-by-side comparison of SlugText vs osgText::Text.
// Left column uses the Slug algorithm (resolution-independent curve rendering).
// Right column uses standard osgText (SDF glyph atlas).
//
// Usage: osgearth_slugtext [--font <path.ttf>] [--text <string>]

#include <osgViewer/Viewer>
#include <osgEarth/SlugText>
#include <osgEarth/GLUtils>
#include <osgText/Text>
#include <osgText/Font>
#include <osg/MatrixTransform>
#include <osg/Camera>
#include <iostream>

using namespace osgEarth;

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

osg::Geode* createOsgText(const std::string& str, const std::string& fontPath,
                           float size, const osg::Vec4& color)
{
    osgText::Text* text = new osgText::Text();
    text->setFont(fontPath);
    text->setCharacterSize(size);
    text->setColor(color);
    text->setText(str);
    text->setAxisAlignment(osgText::Text::XY_PLANE);
    text->setAlignment(osgText::Text::LEFT_BOTTOM);

    osg::Geode* geode = new osg::Geode();
    geode->addDrawable(text);
    return geode;
}

struct TextRow {
    std::string str;
    float size;
    osg::Vec4 color;
};

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc, argv);
    osgEarth::initialize(arguments);

    std::string fontPath;
    if (!arguments.read("--font", fontPath))
    {
#ifdef _WIN32
        fontPath = "C:/Windows/Fonts/arial.ttf";
#elif __APPLE__
        fontPath = "/Library/Fonts/Arial.ttf";
#else
        fontPath = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif
    }

    std::string userText;
    arguments.read("--text", userText);

    osgViewer::Viewer viewer(arguments);

    const int width = 1280, height = 720;
    const float colLeft = 50.0f;
    const float colRight = width * 0.5f + 30.0f;
    const float labelSize = 14.0f;

    osg::Group* root = new osg::Group();
    osg::Camera* hud = createHUDCamera(width, height);
    root->addChild(hud);

    // Column headers
    {
        auto* mt = new osg::MatrixTransform();
        mt->setMatrix(osg::Matrix::translate(colLeft, height - 30, 0));
        auto* st = new SlugText("SlugText (Bezier curves)", fontPath, 18.0f);
        st->setColor(osg::Vec4(1, 1, 0.5f, 1));
        mt->addChild(st);
        hud->addChild(mt);
    }
    {
        auto* mt = new osg::MatrixTransform();
        mt->setMatrix(osg::Matrix::translate(colRight, height - 30, 0));
        mt->addChild(createOsgText("osgText (SDF atlas)", fontPath, 18.0f,
                                   osg::Vec4(1, 1, 0.5f, 1)));
        hud->addChild(mt);
    }

    // Divider line (thin text approximation)
    {
        auto* mt = new osg::MatrixTransform();
        mt->setMatrix(osg::Matrix::translate(width * 0.5f - 5.0f, height - 40, 0));
        auto* st = new SlugText("|", fontPath, 12.0f);
        st->setColor(osg::Vec4(0.5f, 0.5f, 0.5f, 1));
        mt->addChild(st);
        hud->addChild(mt);
    }

    // Define text rows to compare
    std::vector<TextRow> rows = {
        { userText.empty() ? "Hello, Slug!" : userText, 48.0f, osg::Vec4(1, 1, 1, 1) },
        { "Resolution Independent", 32.0f, osg::Vec4(0.8f, 0.9f, 1.0f, 1.0f) },
        { "Pixel Perfect at Any Scale", 16.0f, osg::Vec4(1.0f, 0.9f, 0.7f, 1.0f) },
        { "Small 12px text sample", 12.0f, osg::Vec4(0.9f, 0.9f, 0.9f, 1.0f) },
        { "BIG", 120.0f, osg::Vec4(1.0f, 0.5f, 0.3f, 0.7f) },
    };

    float yPos = height - 60;

    for (auto& row : rows)
    {
        yPos -= row.size * 1.5f;

        // Left: SlugText
        {
            auto* mt = new osg::MatrixTransform();
            mt->setMatrix(osg::Matrix::translate(colLeft, yPos, 0));
            auto* st = new SlugText(row.str, fontPath, row.size);
            st->setColor(row.color);
            mt->addChild(st);
            hud->addChild(mt);
        }

        // Right: osgText
        {
            auto* mt = new osg::MatrixTransform();
            mt->setMatrix(osg::Matrix::translate(colRight, yPos, 0));
            mt->addChild(createOsgText(row.str, fontPath, row.size, row.color));
            hud->addChild(mt);
        }
    }

    // Rotated text comparison
    yPos -= 80;
    float rotAngle = osg::DegreesToRadians(15.0);
    {
        auto* mt = new osg::MatrixTransform();
        osg::Matrix m;
        m.makeRotate(rotAngle, osg::Vec3(0, 0, 1));
        m.setTrans(osg::Vec3(colLeft, yPos, 0));
        mt->setMatrix(m);
        auto* st = new SlugText("Rotated 15deg", fontPath, 28.0f);
        st->setColor(osg::Vec4(0.5f, 1.0f, 0.5f, 1.0f));
        mt->addChild(st);
        hud->addChild(mt);
    }
    {
        auto* mt = new osg::MatrixTransform();
        osg::Matrix m;
        m.makeRotate(rotAngle, osg::Vec3(0, 0, 1));
        m.setTrans(osg::Vec3(colRight, yPos, 0));
        mt->setMatrix(m);
        mt->addChild(createOsgText("Rotated 15deg", fontPath, 28.0f,
                                   osg::Vec4(0.5f, 1.0f, 0.5f, 1.0f)));
        hud->addChild(mt);
    }

    viewer.setSceneData(root);

#ifndef OSG_GL3_AVAILABLE
    viewer.setRealizeOperation(new osgEarth::GL3RealizeOperation());
#endif

    return viewer.run();
}
