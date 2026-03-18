/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/SlugText>
#include <osgEarth/VirtualProgram>
#include <osgEarth/Shaders>
#include <osgEarth/Notify>
#include <osg/Geometry>
#include <osg/BlendFunc>

using namespace osgEarth;
using namespace osgEarth::Util;

SlugText::SlugText() :
    _fontSize(32.0f),
    _color(1, 1, 1, 1)
{
}

SlugText::SlugText(const std::string& text, const std::string& fontPath, float fontSize) :
    _text(text),
    _fontPath(fontPath),
    _fontSize(fontSize),
    _color(1, 1, 1, 1)
{
    compile();
}

SlugText::~SlugText()
{
}

void SlugText::setText(const std::string& text)
{
    _text = text;
    compile();
}

void SlugText::setFont(const std::string& fontPath)
{
    _fontPath = fontPath;
    _font = nullptr;
    compile();
}

void SlugText::setFontSize(float pixelsPerEm)
{
    _fontSize = pixelsPerEm;
    compile();
}

void SlugText::setColor(const osg::Vec4& color)
{
    _color = color;
    // Update the uniform without full recompile
    osg::StateSet* ss = getOrCreateStateSet();
    ss->getOrCreateUniform("slug_color", osg::Uniform::FLOAT_VEC4)->set(_color);
}

void SlugText::compile()
{
    // Clear existing drawables
    removeDrawables(0, getNumDrawables());

    if (_text.empty() || _fontPath.empty())
        return;

    // Get or create the font
    if (!_font)
    {
        _font = SlugFont::get(_fontPath);
        if (!_font || !_font->valid())
        {
            OE_WARN << "SlugText: failed to load font: " << _fontPath << std::endl;
            return;
        }
    }

    // Build geometry for text
    osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();
    geom->setUseDisplayList(false);
    geom->setUseVertexBufferObjects(true);

    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array();
    osg::ref_ptr<osg::Vec2Array> texcoords0 = new osg::Vec2Array();  // em-space sample coords
    osg::ref_ptr<osg::Vec4Array> texcoords1 = new osg::Vec4Array();  // band scale/offset
    osg::ref_ptr<osg::Vec4Array> texcoords2 = new osg::Vec4Array();  // glyph loc + band max

    osg::ref_ptr<osg::DrawElementsUShort> indices =
        new osg::DrawElementsUShort(GL_TRIANGLES);

    float cursorX = 0.0f;
    float scale = _fontSize;

    // Half-pixel expansion in em-space (for v1, expand quads slightly)
    float expand = 1.0f / scale;

    for (char ch : _text)
    {
        uint32_t codepoint = static_cast<uint32_t>(static_cast<unsigned char>(ch));
        const SlugFont::GlyphData* glyph = _font->getGlyph(codepoint);
        if (!glyph)
        {
            // Try space advance
            cursorX += 0.5f * scale;
            continue;
        }

        // Skip whitespace glyphs (no outline data)
        if (glyph->width < 1e-6f || glyph->height < 1e-6f)
        {
            cursorX += glyph->advance * scale;
            continue;
        }

        // Glyph quad in object space
        float x0 = cursorX + (glyph->bearingX - expand) * scale;
        float y0 = (glyph->bearingY - glyph->height - expand) * scale;
        float x1 = cursorX + (glyph->bearingX + glyph->width + expand) * scale;
        float y1 = (glyph->bearingY + expand) * scale;

        // Em-space sample coordinates (what the fragment shader uses to look up curves)
        float emX0 = glyph->bearingX - expand;
        float emY0 = glyph->bearingY - glyph->height - expand;
        float emX1 = glyph->bearingX + glyph->width + expand;
        float emY1 = glyph->bearingY + expand;

        unsigned short base = static_cast<unsigned short>(vertices->size());

        // 4 vertices per glyph quad
        vertices->push_back(osg::Vec3(x0, y0, 0.0f));
        vertices->push_back(osg::Vec3(x1, y0, 0.0f));
        vertices->push_back(osg::Vec3(x1, y1, 0.0f));
        vertices->push_back(osg::Vec3(x0, y1, 0.0f));

        // Em-space texture coordinates
        texcoords0->push_back(osg::Vec2(emX0, emY0));
        texcoords0->push_back(osg::Vec2(emX1, emY0));
        texcoords0->push_back(osg::Vec2(emX1, emY1));
        texcoords0->push_back(osg::Vec2(emX0, emY1));

        // Band transform (same for all 4 verts of this glyph)
        osg::Vec4 bandXform(glyph->bandScaleX, glyph->bandScaleY,
                            glyph->bandOffsetX, glyph->bandOffsetY);
        for (int i = 0; i < 4; ++i)
            texcoords1->push_back(bandXform);

        // Glyph data: band texture location + band maxes
        osg::Vec4 glyphData(
            static_cast<float>(glyph->bandTexX),
            static_cast<float>(glyph->bandTexY),
            static_cast<float>(glyph->bandMaxX),
            static_cast<float>(glyph->bandMaxY));
        for (int i = 0; i < 4; ++i)
            texcoords2->push_back(glyphData);

        // Two triangles
        indices->push_back(base + 0);
        indices->push_back(base + 1);
        indices->push_back(base + 2);
        indices->push_back(base + 0);
        indices->push_back(base + 2);
        indices->push_back(base + 3);

        cursorX += glyph->advance * scale;
    }

    if (vertices->empty())
        return;

    geom->setVertexArray(vertices.get());
    geom->setTexCoordArray(0, texcoords0.get());
    geom->setTexCoordArray(1, texcoords1.get());
    geom->setTexCoordArray(2, texcoords2.get());
    geom->addPrimitiveSet(indices.get());

    addDrawable(geom.get());

    // Setup state
    osg::StateSet* ss = getOrCreateStateSet();

    VirtualProgram* vp = VirtualProgram::getOrCreate(ss);
    vp->setName("SlugText");
    Shaders shaders;
    shaders.load(vp, shaders.SlugText);

    ss->addUniform(new osg::Uniform("slug_curveTexture", 0));
    ss->addUniform(new osg::Uniform("slug_bandTexture", 1));
    ss->getOrCreateUniform("slug_color", osg::Uniform::FLOAT_VEC4)->set(_color);

    ss->setTextureAttributeAndModes(0, _font->getCurveTexture(), osg::StateAttribute::ON);
    ss->setTextureAttributeAndModes(1, _font->getBandTexture(), osg::StateAttribute::ON);

    ss->setMode(GL_BLEND, osg::StateAttribute::ON);
    ss->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    ss->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
}
