/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/SlugFont>
#include <osgEarth/Notify>
#include <osg/Image>
#include <osg/Texture2D>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <mutex>
#include <cmath>
#include <algorithm>

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_RGBA_INTEGER
#define GL_RGBA_INTEGER 0x8D99
#endif
#ifndef GL_RGBA16UI
#define GL_RGBA16UI 0x8D76
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif
#ifndef GL_RGBA32F_ARB
#define GL_RGBA32F_ARB 0x8814
#endif

using namespace osgEarth;

namespace
{
    static std::mutex s_fontCacheMutex;
    static std::map<std::string, osg::observer_ptr<SlugFont>> s_fontCache;

    // FreeType outline decomposition callbacks
    struct OutlineContext
    {
        std::vector<SlugFont::CurveData>* curves;
        float scale;
        float lastX, lastY;
    };

    int ftMoveTo(const FT_Vector* to, void* user)
    {
        auto* ctx = static_cast<OutlineContext*>(user);
        ctx->lastX = to->x * ctx->scale;
        ctx->lastY = to->y * ctx->scale;
        return 0;
    }

    int ftLineTo(const FT_Vector* to, void* user)
    {
        auto* ctx = static_cast<OutlineContext*>(user);
        float x3 = to->x * ctx->scale;
        float y3 = to->y * ctx->scale;
        // Convert line to degenerate quadratic: control point at midpoint
        float mx = (ctx->lastX + x3) * 0.5f;
        float my = (ctx->lastY + y3) * 0.5f;
        SlugFont::CurveData c;
        c.x1 = ctx->lastX; c.y1 = ctx->lastY;
        c.x2 = mx; c.y2 = my;
        c.x3 = x3; c.y3 = y3;
        ctx->curves->push_back(c);
        ctx->lastX = x3;
        ctx->lastY = y3;
        return 0;
    }

    int ftConicTo(const FT_Vector* control, const FT_Vector* to, void* user)
    {
        auto* ctx = static_cast<OutlineContext*>(user);
        SlugFont::CurveData c;
        c.x1 = ctx->lastX; c.y1 = ctx->lastY;
        c.x2 = control->x * ctx->scale; c.y2 = control->y * ctx->scale;
        c.x3 = to->x * ctx->scale; c.y3 = to->y * ctx->scale;
        ctx->curves->push_back(c);
        ctx->lastX = c.x3;
        ctx->lastY = c.y3;
        return 0;
    }

    int ftCubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user)
    {
        // Approximate cubic Bezier with 2 quadratics
        auto* ctx = static_cast<OutlineContext*>(user);
        float p0x = ctx->lastX, p0y = ctx->lastY;
        float p1x = c1->x * ctx->scale, p1y = c1->y * ctx->scale;
        float p2x = c2->x * ctx->scale, p2y = c2->y * ctx->scale;
        float p3x = to->x * ctx->scale, p3y = to->y * ctx->scale;

        // Split at t=0.5 and compute quadratic control points
        float midx = (p0x + 3.0f * p1x + 3.0f * p2x + p3x) * 0.125f;
        float midy = (p0y + 3.0f * p1y + 3.0f * p2y + p3y) * 0.125f;

        // First quadratic: p0 -> q1 -> mid
        float q1x = (p0x + 3.0f * p1x) * 0.25f;
        float q1y = (p0y + 3.0f * p1y) * 0.25f;
        SlugFont::CurveData ca;
        ca.x1 = p0x; ca.y1 = p0y;
        ca.x2 = q1x; ca.y2 = q1y;
        ca.x3 = midx; ca.y3 = midy;
        ctx->curves->push_back(ca);

        // Second quadratic: mid -> q2 -> p3
        float q2x = (3.0f * p2x + p3x) * 0.25f;
        float q2y = (3.0f * p2y + p3y) * 0.25f;
        SlugFont::CurveData cb;
        cb.x1 = midx; cb.y1 = midy;
        cb.x2 = q2x; cb.y2 = q2y;
        cb.x3 = p3x; cb.y3 = p3y;
        ctx->curves->push_back(cb);

        ctx->lastX = p3x;
        ctx->lastY = p3y;
        return 0;
    }

    bool curveIntersectsBandY(const SlugFont::CurveData& c, float lo, float hi)
    {
        float minY = std::min({c.y1, c.y2, c.y3});
        float maxY = std::max({c.y1, c.y2, c.y3});
        return maxY >= lo && minY <= hi;
    }

    bool curveIntersectsBandX(const SlugFont::CurveData& c, float lo, float hi)
    {
        float minX = std::min({c.x1, c.x2, c.x3});
        float maxX = std::max({c.x1, c.x2, c.x3});
        return maxX >= lo && minX <= hi;
    }

    // Convert float to IEEE 754 half-precision
    uint16_t floatToHalf(float f)
    {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        uint32_t sign = (bits >> 16) & 0x8000;
        int32_t exponent = ((bits >> 23) & 0xFF) - 127 + 15;
        uint32_t mantissa = bits & 0x007FFFFF;
        if (exponent <= 0) {
            if (exponent < -10) return static_cast<uint16_t>(sign);
            mantissa |= 0x00800000;
            int shift = 1 - exponent;
            mantissa >>= shift;
            return static_cast<uint16_t>(sign | (mantissa >> 13));
        }
        if (exponent >= 31) {
            return static_cast<uint16_t>(sign | 0x7C00);
        }
        return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
    }
}

SlugFont::SlugFont(const std::string& fontPath) :
    _valid(false)
{
    load(fontPath);
}

SlugFont::~SlugFont()
{
}

osg::ref_ptr<SlugFont> SlugFont::get(const std::string& fontPath)
{
    std::lock_guard<std::mutex> lock(s_fontCacheMutex);
    auto it = s_fontCache.find(fontPath);
    if (it != s_fontCache.end())
    {
        osg::ref_ptr<SlugFont> font;
        if (it->second.lock(font))
            return font;
    }
    osg::ref_ptr<SlugFont> font = new SlugFont(fontPath);
    if (font->valid())
        s_fontCache[fontPath] = font;
    return font;
}

const SlugFont::GlyphData* SlugFont::getGlyph(uint32_t codepoint) const
{
    auto it = _glyphs.find(codepoint);
    return it != _glyphs.end() ? &it->second : nullptr;
}

void SlugFont::load(const std::string& fontPath)
{
    FT_Library library;
    if (FT_Init_FreeType(&library))
    {
        OE_WARN << "SlugFont: failed to initialize FreeType" << std::endl;
        return;
    }

    FT_Face face;
    if (FT_New_Face(library, fontPath.c_str(), 0, &face))
    {
        OE_WARN << "SlugFont: failed to load font: " << fontPath << std::endl;
        FT_Done_FreeType(library);
        return;
    }

    // Process printable ASCII glyphs
    for (uint32_t cp = 32; cp < 127; ++cp)
    {
        processGlyph(face, cp);
    }

    buildTextures();

    FT_Done_Face(face);
    FT_Done_FreeType(library);

    _valid = !_glyphs.empty();
}

void SlugFont::processGlyph(void* facePtr, uint32_t codepoint)
{
    FT_Face face = static_cast<FT_Face>(facePtr);

    FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
    if (glyphIndex == 0 && codepoint != 0)
        return;

    // FT_LOAD_NO_SCALE: outline points and metrics are in raw font units (not 26.6).
    // We normalize everything by dividing by units_per_EM to get em-space [0,1].
    if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_SCALE))
        return;

    float emScale = 1.0f / static_cast<float>(face->units_per_EM);

    GlyphBuildData build;
    // With NO_SCALE, metrics are in font units directly (no 26.6 encoding)
    build.metrics.bearingX = face->glyph->metrics.horiBearingX * emScale;
    build.metrics.bearingY = face->glyph->metrics.horiBearingY * emScale;
    build.metrics.width    = face->glyph->metrics.width * emScale;
    build.metrics.height   = face->glyph->metrics.height * emScale;
    build.metrics.advance  = face->glyph->metrics.horiAdvance * emScale;

    FT_Outline& outline = face->glyph->outline;
    if (outline.n_points == 0)
    {
        _glyphBuild[codepoint] = build;
        return;
    }

    // With NO_SCALE, outline points are in font units. Scale to em-space.
    OutlineContext ctx;
    ctx.curves = &build.curves;
    ctx.scale = emScale;
    ctx.lastX = ctx.lastY = 0.0f;

    FT_Outline_Funcs funcs;
    funcs.move_to = ftMoveTo;
    funcs.line_to = ftLineTo;
    funcs.conic_to = ftConicTo;
    funcs.cubic_to = ftCubicTo;
    funcs.shift = 0;
    funcs.delta = 0;

    FT_Outline_Decompose(&outline, &funcs, &ctx);

    if (build.curves.empty())
    {
        _glyphBuild[codepoint] = build;
        return;
    }

    // Build bands
    int numCurves = static_cast<int>(build.curves.size());
    int numBands = std::min(16, std::max(1, numCurves / 2));

    // Compute glyph bounding box from curve data
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (auto& c : build.curves)
    {
        minX = std::min({minX, c.x1, c.x2, c.x3});
        minY = std::min({minY, c.y1, c.y2, c.y3});
        maxX = std::max({maxX, c.x1, c.x2, c.x3});
        maxY = std::max({maxY, c.y1, c.y2, c.y3});
    }

    float rangeX = maxX - minX;
    float rangeY = maxY - minY;
    if (rangeX < 1e-9f) rangeX = 1e-9f;
    if (rangeY < 1e-9f) rangeY = 1e-9f;

    // Band transform: maps em-space coords to band index
    // bandIndex = coord * scale + offset
    build.metrics.bandScaleX = static_cast<float>(numBands) / rangeX;
    build.metrics.bandScaleY = static_cast<float>(numBands) / rangeY;
    build.metrics.bandOffsetX = -minX * build.metrics.bandScaleX;
    build.metrics.bandOffsetY = -minY * build.metrics.bandScaleY;

    build.metrics.bandMaxX = numBands - 1;
    build.metrics.bandMaxY = numBands - 1;

    // Build horizontal bands (indexed by y, curves sorted by descending max x)
    build.hbands.resize(numBands);
    float bandHeightY = rangeY / numBands;
    for (int b = 0; b < numBands; ++b)
    {
        float lo = minY + b * bandHeightY;
        float hi = lo + bandHeightY;
        auto& band = build.hbands[b];
        for (int ci = 0; ci < numCurves; ++ci)
        {
            if (curveIntersectsBandY(build.curves[ci], lo, hi))
                band.curveIndices.push_back(ci);
        }
        std::sort(band.curveIndices.begin(), band.curveIndices.end(),
            [&](int a, int b) {
                float maxA = std::max({build.curves[a].x1, build.curves[a].x2, build.curves[a].x3});
                float maxB = std::max({build.curves[b].x1, build.curves[b].x2, build.curves[b].x3});
                return maxA > maxB;
            });
        band.curveCount = static_cast<int>(band.curveIndices.size());
    }

    // Build vertical bands (indexed by x, curves sorted by descending max y)
    build.vbands.resize(numBands);
    float bandWidthX = rangeX / numBands;
    for (int b = 0; b < numBands; ++b)
    {
        float lo = minX + b * bandWidthX;
        float hi = lo + bandWidthX;
        auto& band = build.vbands[b];
        for (int ci = 0; ci < numCurves; ++ci)
        {
            if (curveIntersectsBandX(build.curves[ci], lo, hi))
                band.curveIndices.push_back(ci);
        }
        std::sort(band.curveIndices.begin(), band.curveIndices.end(),
            [&](int a, int b) {
                float maxA = std::max({build.curves[a].y1, build.curves[a].y2, build.curves[a].y3});
                float maxB = std::max({build.curves[b].y1, build.curves[b].y2, build.curves[b].y3});
                return maxA > maxB;
            });
        band.curveCount = static_cast<int>(band.curveIndices.size());
    }

    _glyphBuild[codepoint] = build;
}

void SlugFont::buildTextures()
{
    // First pass: compute total curve texels needed.
    // Each curve = 2 texels, but we may need padding to avoid row-spanning.
    int totalCurveTexels = 0;
    for (auto& kv : _glyphBuild)
    {
        // 2 texels per curve + potential 1 padding texel per curve for row alignment
        totalCurveTexels += static_cast<int>(kv.second.curves.size()) * 3;
    }

    int curveTexWidth = BAND_TEX_WIDTH;
    int curveTexHeight = std::max(1, (totalCurveTexels + curveTexWidth - 1) / curveTexWidth);

    // Use GL_RGBA + GL_FLOAT for curve texture (simpler, avoids half-float issues).
    // Internal format GL_RGBA32F ensures full precision on GPU.
    osg::ref_ptr<osg::Image> curveImage = new osg::Image();
    curveImage->allocateImage(curveTexWidth, curveTexHeight, 1, GL_RGBA, GL_FLOAT);
    curveImage->setInternalTextureFormat(GL_RGBA32F_ARB);
    memset(curveImage->data(), 0, curveImage->getImageSizeInBytes());

    // Band texture: headers + curve index lists
    int totalBandTexels = 0;
    for (auto& kv : _glyphBuild)
    {
        auto& g = kv.second;
        int numHeaders = static_cast<int>(g.hbands.size() + g.vbands.size());
        int numIndices = 0;
        for (auto& b : g.hbands) numIndices += static_cast<int>(b.curveIndices.size());
        for (auto& b : g.vbands) numIndices += static_cast<int>(b.curveIndices.size());
        totalBandTexels += numHeaders + numIndices;
    }

    int bandTexWidth = BAND_TEX_WIDTH;
    int bandTexHeight = std::max(1, (totalBandTexels + bandTexWidth - 1) / bandTexWidth);

    // Allocate with GL_RGBA + GL_UNSIGNED_SHORT for correct OSG buffer sizing,
    // then set pixel format to GL_RGBA_INTEGER for proper GPU upload as integer texture.
    osg::ref_ptr<osg::Image> bandImage = new osg::Image();
    bandImage->allocateImage(bandTexWidth, bandTexHeight, 1, GL_RGBA, GL_UNSIGNED_SHORT);
    memset(bandImage->data(), 0, bandImage->getImageSizeInBytes());
    bandImage->setInternalTextureFormat(GL_RGBA16UI);
    bandImage->setPixelFormat(GL_RGBA_INTEGER);

    // Helper: write 4 floats into curve texture at a texel position
    auto writeCurveTexel = [&](int texelIndex, float r, float g, float b, float a) {
        int x = texelIndex % curveTexWidth;
        int y = texelIndex / curveTexWidth;
        if (y >= curveTexHeight) return;
        float* ptr = reinterpret_cast<float*>(curveImage->data(x, y));
        ptr[0] = r;
        ptr[1] = g;
        ptr[2] = b;
        ptr[3] = a;
    };

    // Helper: write 4 uint16 values into band texture at a texel position
    auto writeBandTexel = [&](int texelIndex, uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
        int x = texelIndex % bandTexWidth;
        int y = texelIndex / bandTexWidth;
        if (y >= bandTexHeight) return;
        uint16_t* ptr = reinterpret_cast<uint16_t*>(bandImage->data(x, y));
        ptr[0] = r;
        ptr[1] = g;
        ptr[2] = b;
        ptr[3] = a;
    };

    int curveTexelOffset = 0;
    int bandTexelOffset = 0;

    for (auto& kv : _glyphBuild)
    {
        uint32_t codepoint = kv.first;
        auto& g = kv.second;

        // Map from local curve index to curve texture location (as 1D texel index)
        std::vector<int> curveLocs(g.curves.size());

        // Write curves to curve texture.
        // Each curve = 2 consecutive texels. Ensure both are on the same row
        // (the shader reads curveLoc.x+1 without row wrapping).
        for (int ci = 0; ci < static_cast<int>(g.curves.size()); ++ci)
        {
            // If the second texel would wrap to the next row, skip to next row
            int xPos = curveTexelOffset % curveTexWidth;
            if (xPos == curveTexWidth - 1)
            {
                curveTexelOffset++; // skip last texel on this row
            }

            auto& c = g.curves[ci];
            curveLocs[ci] = curveTexelOffset;
            // Texel 1: p1.xy, p2.xy
            writeCurveTexel(curveTexelOffset, c.x1, c.y1, c.x2, c.y2);
            curveTexelOffset++;
            // Texel 2: p3.xy, 0, 0
            writeCurveTexel(curveTexelOffset, c.x3, c.y3, 0.0f, 0.0f);
            curveTexelOffset++;
        }

        // Record glyph's band texture location
        GlyphData gd = g.metrics;
        gd.bandTexX = bandTexelOffset % bandTexWidth;
        gd.bandTexY = bandTexelOffset / bandTexWidth;

        int numHBands = static_cast<int>(g.hbands.size());
        int numVBands = static_cast<int>(g.vbands.size());
        int numHeaders = numHBands + numVBands;

        // Headers come first, then curve index lists.
        // Offset values in headers are relative to glyphLoc.
        int indexListOffset = numHeaders;

        // Write horizontal band headers
        for (int b = 0; b < numHBands; ++b)
        {
            auto& band = g.hbands[b];
            uint16_t count = static_cast<uint16_t>(band.curveCount);
            uint16_t offset = static_cast<uint16_t>(indexListOffset);
            writeBandTexel(bandTexelOffset + b, count, offset, 0, 0);
            indexListOffset += band.curveCount;
        }

        // Write vertical band headers (after horizontal headers)
        for (int b = 0; b < numVBands; ++b)
        {
            auto& band = g.vbands[b];
            uint16_t count = static_cast<uint16_t>(band.curveCount);
            uint16_t offset = static_cast<uint16_t>(indexListOffset);
            writeBandTexel(bandTexelOffset + numHBands + b, count, offset, 0, 0);
            indexListOffset += band.curveCount;
        }

        // Write curve index lists for horizontal bands
        int idxOff = numHeaders;
        for (int b = 0; b < numHBands; ++b)
        {
            for (int ci : g.hbands[b].curveIndices)
            {
                int curveLoc = curveLocs[ci];
                uint16_t cx = static_cast<uint16_t>(curveLoc % curveTexWidth);
                uint16_t cy = static_cast<uint16_t>(curveLoc / curveTexWidth);
                writeBandTexel(bandTexelOffset + idxOff, cx, cy, 0, 0);
                idxOff++;
            }
        }

        // Write curve index lists for vertical bands
        for (int b = 0; b < numVBands; ++b)
        {
            for (int ci : g.vbands[b].curveIndices)
            {
                int curveLoc = curveLocs[ci];
                uint16_t cx = static_cast<uint16_t>(curveLoc % curveTexWidth);
                uint16_t cy = static_cast<uint16_t>(curveLoc / curveTexWidth);
                writeBandTexel(bandTexelOffset + idxOff, cx, cy, 0, 0);
                idxOff++;
            }
        }

        bandTexelOffset += idxOff;

        _glyphs[codepoint] = gd;
    }

    // Create textures
    _curveTexture = new osg::Texture2D(curveImage.get());
    _curveTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
    _curveTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
    _curveTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    _curveTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    _curveTexture->setResizeNonPowerOfTwoHint(false);

    _bandTexture = new osg::Texture2D(bandImage.get());
    _bandTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
    _bandTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
    _bandTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    _bandTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    _bandTexture->setResizeNonPowerOfTwoHint(false);

    _glyphBuild.clear();
}
