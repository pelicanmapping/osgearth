/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/FeatureRasterizer>
#include <osgEarth/Metrics>
#include <osgEarth/Registry>
#include <osgEarth/StyleSheet>
#include <osgEarth/BuildConfig>

using namespace osgEarth;

#define LC "[FeatureRasterizer] : "

#ifdef OSGEARTH_HAVE_BLEND2D
#define USE_BLEND2D
#endif

#ifdef USE_BLEND2D
#ifdef BLEND2D_USE_SCOPED_INCLUDE
#include <blend2d/blend2d.h>
#else
#include <blend2d.h>
#endif
#endif

#include <osgEarth/AGG.h>
#include <osgEarth/BufferFilter>
#include <osgEarth/ResampleFilter>

#include <tuple>

namespace osgEarth {
    namespace FeatureImageLayerImpl
    {
        struct RenderFrame
        {
            double xmin, ymin, xmax, ymax;
            double xf, yf;
        };

        struct float32
        {
            float32() : value(NO_DATA_VALUE) { }
            float32(float v) : value(v) { }
            float value;
        };

        struct span_coverage32
        {
            static void render(unsigned char* ptr,
                int x,
                unsigned count,
                const unsigned char* covers,
                const float32& c)
            {
                unsigned char* p = ptr + (x << 2);
                float* f = (float*)p;
                do
                {
                    unsigned char cover = *covers++;
                    if (cover > 0)
                        *f = c.value;
                    f++;
                } while (--count);
            }

            static void hline(unsigned char* ptr,
                int x,
                unsigned count,
                const float32& c)
            {
                unsigned char* p = ptr + (x << 2);
                float* f = (float*)p;
                do {
                    *f++ = c.value;
                } while (--count);
            }

            static float32 get(unsigned char* ptr, int x)
            {
                unsigned char* p = ptr + (x << 2);
                float* f = (float*)p;
                return float32(*f);
            }
        };

        // rasterizes a geometry to color
        void rasterize_agglite(
            const Geometry* geometry,
            const osg::Vec4& color,
            RenderFrame& frame,
            agg::rasterizer& ras,
            agg::rendering_buffer& buffer)
        {
            unsigned a = (unsigned)(127.0f + (color.a()*255.0f) / 2.0f); // scale alpha up

            agg::rgba8 fgColor = agg::rgba8(
                (unsigned)(color.r()*255.0f),
                (unsigned)(color.g()*255.0f),
                (unsigned)(color.b()*255.0f),
                a);

            ConstGeometryIterator gi(geometry);
            while (gi.hasMore())
            {
                const Geometry* g = gi.next();

                for (Geometry::const_iterator p = g->begin(); p != g->end(); p++)
                {
                    const osg::Vec3d& p0 = *p;
                    double x0 = frame.xf*(p0.x() - frame.xmin);
                    double y0 = frame.yf*(p0.y() - frame.ymin);

                    if (p == g->begin())
                        ras.move_to_d(x0, y0);
                    else
                        ras.line_to_d(x0, y0);
                }
            }
            agg::renderer<agg::span_abgr32, agg::rgba8> ren(buffer);
            ras.render(ren, fgColor);

            ras.reset();
        }


        void rasterizeCoverage_agglite(
            const Geometry* geometry,
            float value,
            RenderFrame& frame,
            agg::rasterizer& ras,
            agg::rendering_buffer& buffer)
        {
            ConstGeometryIterator gi(geometry);
            while (gi.hasMore())
            {
                const Geometry* g = gi.next();

                for (Geometry::const_iterator p = g->begin(); p != g->end(); p++)
                {
                    const osg::Vec3d& p0 = *p;
                    double x0 = frame.xf*(p0.x() - frame.xmin);
                    double y0 = frame.yf*(p0.y() - frame.ymin);

                    if (p == g->begin())
                        ras.move_to_d(x0, y0);
                    else
                        ras.line_to_d(x0, y0);
                }
            }

            agg::renderer<span_coverage32, float32> ren(buffer);
            ras.render(ren, value);
            ras.reset();
        }

#ifdef USE_BLEND2D

        void rasterizePolygons_blend2d(
            const Geometry* geometry,
            const PolygonSymbol* symbol,
            RenderFrame& frame,
            BLContext& ctx)
        {
            OE_PROFILING_ZONE;

            if (!geometry->isPolygon())
            {
                return;
            }

            BLPath path;

            geometry->forEachPart([&](const Geometry* part)
            {
                for (Geometry::const_iterator p = part->begin(); p != part->end(); p++)
                {
                    const osg::Vec3d& p0 = *p;
                    double x = frame.xf*(p0.x() - frame.xmin);
                    double y = frame.yf*(p0.y() - frame.ymin);
#if BL_VERSION >= 5120
                    y = ctx.target_height() - y;
#else
                    y = ctx.targetHeight() - y;
#endif

                    if (p == part->begin())
#if BL_VERSION >= 5120
                        path.move_to(x, y);
#else
                        path.moveTo(x, y);
#endif
                    else
#if BL_VERSION >= 5120
                        path.line_to(x, y);
#else
                        path.lineTo(x, y);
#endif
                }
            });

            osg::Vec4 color = symbol->fill().isSet() ? symbol->fill()->color() : Color::White;
            // Fill the path
#if BL_VERSION >= 5120
            ctx.set_fill_style(BLRgba(color.r(), color.g(), color.b(), color.a()));
            ctx.fill_path(path);
#else
            ctx.setFillStyle(BLRgba(color.r(), color.g(), color.b(), color.a()));
            ctx.fillPath(path);
#endif

            // Also stroke a 1 pixel path around the polygon with the same color to help cover up any gaps between any adjoining features.
#if BL_VERSION >= 5120
            ctx.set_stroke_style(BLRgba(color.r(), color.g(), color.b(), color.a()));
            ctx.set_stroke_width(1.0);
            ctx.stroke_path(path);
#else
            ctx.setStrokeStyle(BLRgba(color.r(), color.g(), color.b(), color.a()));
            ctx.setStrokeWidth(1.0);
            ctx.strokePath(path);
#endif
        }

        void lineSymbolToBLContext(const LineSymbol* line, BLContext& ctx)
        {
            auto cap = BL_STROKE_CAP_ROUND;
            auto join = BL_STROKE_JOIN_ROUND;

            if (line->stroke().isSet())
            {
                if (line->stroke()->lineCap().isSet())
                {
                    cap =
                        line->stroke()->lineCap() == Stroke::LINECAP_FLAT ? BL_STROKE_CAP_BUTT :
                        line->stroke()->lineCap() == Stroke::LINECAP_SQUARE ? BL_STROKE_CAP_SQUARE :
                        BL_STROKE_CAP_ROUND;
                }

                if (line->stroke()->lineJoin().isSet())
                {
                    join =
                        line->stroke()->lineJoin() == Stroke::LINEJOIN_MITRE ? BL_STROKE_JOIN_MITER_BEVEL :
                        BL_STROKE_JOIN_ROUND;
                }
            }

#if BL_VERSION >= 5120
            ctx.set_stroke_caps(cap);
            ctx.set_stroke_join(join);
#else
            ctx.setStrokeCaps(cap);
            ctx.setStrokeJoin(join);
#endif
        }

        void rasterizeLines(
            const Geometry* geometry,
            const Color& color,
            double lineWidth_px,
            RenderFrame& frame,
            BLContext& ctx)
        {
            OE_HARD_ASSERT(geometry != nullptr);

            if (!geometry->isPolygon() && !geometry->isLinear())
            {
                return;
            }

            OE_PROFILING_ZONE;

            BLPath path;

            geometry->forEachPart(true, [&](const Geometry* part)
            {
                for (Geometry::const_iterator p = part->begin(); p != part->end(); p++)
                {
                    const osg::Vec3d& p0 = *p;
                    double x = frame.xf*(p0.x() - frame.xmin);
                    double y = frame.yf*(p0.y() - frame.ymin);
#if BL_VERSION >= 5120
                    y = ctx.target_height() - y;
#else
                    y = ctx.targetHeight() - y;
#endif

                    if (p == part->begin())
#if BL_VERSION >= 5120
                        path.move_to(x, y);
#else
                        path.moveTo(x, y);
#endif
                    else
#if BL_VERSION >= 5120
                        path.line_to(x, y);
#else
                        path.lineTo(x, y);
#endif
                }

                if ((part->getType() == Geometry::TYPE_RING || part->getType() == Geometry::TYPE_POLYGON) &&
                    (part->front() != part->back()))
                {
                    const osg::Vec3d& p0 = part->front();
                    double x = frame.xf*(p0.x() - frame.xmin);
                    double y = frame.yf*(p0.y() - frame.ymin);
#if BL_VERSION >= 5120
                    y = ctx.target_height() - y;

                    path.line_to(x, y);
#else
                    y = ctx.targetHeight() - y;

                    path.lineTo(x, y);
#endif
                }
            });

#if BL_VERSION >= 5120
            ctx.set_stroke_style(BLRgba(color.r(), color.g(), color.b(), color.a()));
            ctx.set_stroke_width(lineWidth_px);
            ctx.stroke_path(path);
#else
            ctx.setStrokeStyle(BLRgba(color.r(), color.g(), color.b(), color.a()));
            ctx.setStrokeWidth(lineWidth_px);
            ctx.strokePath(path);
#endif
        }

        static const BLFontFace& getOrCreateFontFace()
        {
            // TODO:  Proper font support
            static BLFontFace fontFace;
            static std::mutex fontMutex;
#if BL_VERSION >= 5120
            if (fontFace.is_empty())
#else
            if (fontFace.empty())
#endif
            {
                std::lock_guard<std::mutex> lock(fontMutex);
                auto defaultFont = osgEarth::Registry::instance()->getDefaultFont();
#if BL_VERSION >= 5120
                fontFace.create_from_file(defaultFont->getFileName().c_str());
#else
                fontFace.createFromFile(defaultFont->getFileName().c_str());
#endif
            }
            return fontFace;
        }

        std::string templateReplace(const Feature* feature, const std::string& expression)
        {
            std::string result = expression;
            auto start = result.find('{');
            auto end = result.find('}');
            if (start != std::string::npos && end != std::string::npos)
            {
                std::string attribute = result.substr(start + 1, end - start - 1);
                if (feature->hasAttr(attribute))
                {
                    std::string value = feature->getString(attribute);
                    std::string replaceText = Stringify() << "{" << attribute << "}";
                    osgEarth::replaceIn(result, replaceText, value);
                }
            }

            if (result.find("{") != std::string::npos || result.find("}") != std::string::npos)
            {
                OE_INFO << LC << "Failed to replace attributes in template " << expression << std::endl;
                result = "";
            }
            return result;
        }

        void renderMapboxText(BLContext& ctx, float x, float y, const std::string& text, const TextSymbol* textSymbol, MapboxGLGlyphManager* glyphManager, float textScale, FeatureRasterizer::SymbolBoundingBoxes& symbolBounds)
        {
            if (!glyphManager)
            {
                return;
            }

#if 0
            // Draw the label position
            ctx.setFillStyle(BLRgba(0.0, 1.0, 0.0, 1.0));
            ctx.fillCircle(x, y, 4);
#endif

            float fontSize = textSymbol->size()->literal() * textScale;

            const float ONE_EM = 24.0;
            float scale = fontSize / ONE_EM;
            float baselineOffset = 7.0f;
            float GLYPH_PADDING = 3.0f;

            // Collect a list of glyphs for the text
            std::vector< osg::ref_ptr< MapboxGLGlyphManager::Glyph > > textGlyphs;
            glyphManager->getGlyphs(text, textSymbol->font().get(), textGlyphs);

            float cursorX = x;
            float cursorY = y;

            float lineWidth = 0.0f;
            float lineHeight = 0.0f;

            osg::BoundingBox bounds;

            // Compute the line width and line height
            for (auto& g : textGlyphs)
            {
                if (g.valid())
                {
                    float minX = cursorX + g->left * scale;
                    float minY = cursorY - g->top * scale;
                    float maxX = minX + g->width * scale;
                    float maxY = minY + g->height * scale;
                    bounds.expandBy(minX, minY, 0.0f);
                    bounds.expandBy(maxX, minY, 0.0f);
                    bounds.expandBy(maxX, maxY, 0.0f);
                    bounds.expandBy(minX, maxY, 0.0f);
                    cursorX += g->advance * scale;
                    //ctx.blitImage(BLPoint((double)g->left, (double)(-g->top)), sprite, glyphRect);
                    //lineWidth += g->advance * scale;
                    //lineHeight = std::max(lineHeight, (g->height) * scale);
                }
            }

            lineWidth = bounds.xMax() - bounds.xMin();
            lineHeight = bounds.yMax() - bounds.yMin();

            cursorX = x;
            cursorY = y;

            // Adjust the cursor based on the alignment
            auto alignment = textSymbol->alignment().get();

            switch (alignment)
            {
            case TextSymbol::ALIGN_CENTER_CENTER:
                cursorX -= lineWidth / 2.0;
                cursorY -= (ONE_EM * scale) * 0.5;
                break;
            case TextSymbol::ALIGN_LEFT_CENTER:
                cursorY -= (ONE_EM * scale) * 0.5;
                break;
            case TextSymbol::ALIGN_RIGHT_CENTER:
                cursorX -= lineWidth;
                cursorY -= (ONE_EM * scale) * 0.5;
                break;
            case TextSymbol::ALIGN_CENTER_TOP:
                cursorX -= lineWidth / 2.0;
                break;
            case TextSymbol::ALIGN_CENTER_BOTTOM:
                cursorX -= lineWidth / 2.0;
                cursorY -= (ONE_EM * scale);
                break;
            case TextSymbol::ALIGN_LEFT_TOP:
                // default
                break;
            case TextSymbol::ALIGN_RIGHT_TOP:
                cursorX -= lineWidth;
                break;
            case TextSymbol::ALIGN_LEFT_BOTTOM:
                cursorY -= (ONE_EM * scale);
                break;
            case TextSymbol::ALIGN_RIGHT_BOTTOM:
                cursorX -= lineWidth;
                cursorY -= (ONE_EM * scale);
                break;
            default:
                break;
            }            

            float offsetX = x - cursorX;
            float offsetY = y - cursorY;
            bounds.xMin() -= offsetX;
            bounds.yMin() -= offsetY;
            bounds.xMax() -= offsetX;
            bounds.yMax() -= offsetY;

            // Don't draw the text if it intersects an existing label.
            bool intersects = false;
            if (symbolBounds.intersects2d(bounds))
            {
                return;             
            }

            // Render each glyph
            for (unsigned int index = 0; index < textGlyphs.size(); ++index)
            {
                MapboxGLGlyphManager::Glyph* g = textGlyphs[index].get();

                if (!g)
                {                
                    continue;
                }

                //Write each glyph into the output image.
                unsigned glyphWidth = g->width + GLYPH_PADDING*2.0;
                unsigned glyphHeight = g->height + GLYPH_PADDING * 2.0;

                if (g->bitmap.size() > 0)
                {
                    auto textColor = textSymbol->fill()->color();

                    unsigned int numPixels = glyphWidth * glyphHeight;

                    unsigned char* glyphData = new unsigned char[g->bitmap.size() * 4]{ 0u };

                    for (unsigned int i = 0; i < numPixels; ++i)
                    {
                        unsigned char* base = &glyphData[i * 4];
                        unsigned char value = g->bitmap[i];
                        float maxValue = 192.0; // alpha 1
                        float minValue = 180.0;  // alpha 0
                        float alpha = osg::clampBetween(((value - minValue) / (maxValue - minValue)), 0.0f, 1.0f);
                        alpha *= textColor.a();

                        base[0] = (unsigned char)(textColor.b() * alpha * 255.0f);
                        base[1] = (unsigned char)(textColor.g() * alpha * 255.0f);
                        base[2] = (unsigned char)(textColor.r() * alpha * 255.0f);
                        base[3] = (unsigned char)(alpha * 255.0f);
                    }

                    BLImage sprite;
#if BL_VERSION >= 5120
                    sprite.create_from_data(glyphWidth, glyphHeight, BL_FORMAT_PRGB32, glyphData, glyphWidth * 4);
#else
                    sprite.createFromData(glyphWidth, glyphHeight, BL_FORMAT_PRGB32, glyphData, glyphWidth * 4);
#endif

                    BLRectI glyphRect(0.0, 0.0, (double)glyphWidth, (double)glyphHeight);

                    ctx.translate(cursorX, cursorY);
                    ctx.scale(scale);
#if BL_VERSION >= 5120
                    ctx.blit_image(BLPoint((double)g->left - GLYPH_PADDING, (double)(-g->top) - GLYPH_PADDING), sprite, glyphRect);
                    //ctx.blit_image(BLPoint(0, 0), sprite, glyphRect);
#else
                    ctx.blitImage(BLPoint((double)g->left - GLYPH_PADDING, (double)(-g->top) - GLYPH_PADDING), sprite, glyphRect);
                    //ctx.blitImage(BLPoint(0, 0), sprite, glyphRect);
#endif

#if BL_VERSION >= 5120
                    ctx.reset_transform();
#elif BL_VERSION >= 2820
                    ctx.resetTransform();
#else
                    ctx.resetMatrix();
#endif

#if 0
                    // Draw the text bounding box
                    ctx.setStrokeStyle(BLRgba(1.0, 0.0, 0.0, 1.0));
                    ctx.strokeBox(BLBoxI(bounds.xMin(), bounds.yMin(), bounds.xMax(), bounds.yMax()));
#endif

                    delete[] glyphData;
                }

                // Advance the cursor
                cursorX += (float)g->advance * scale;
            }

            symbolBounds._bounds.push_back(bounds);
        }

        void rasterizeSymbols(
            const Feature* feature,
            const StyleSheet* styleSheet,
            const TextSymbol* textSymbol,
            const SkinSymbol* skinSymbol,
            RenderFrame& frame,
            BLContext& ctx,
            MapboxGLGlyphManager* glyphManager,
            float scale,
            FeatureRasterizer::SymbolBoundingBoxes& symbolBounds)
        {
            OE_HARD_ASSERT(feature != nullptr);

            OE_PROFILING_ZONE;

            BLFont font;

            Session* session = nullptr;

            // Disable symbols for non-linear features until we have a better labeling strategy.
            if (!feature->getGeometry()->isPointSet())
            {
                return;
            }

            if (styleSheet && skinSymbol && skinSymbol->name().isSet())
            {
                if (skinSymbol->library().isSet())
                {
                    osg::ref_ptr< ResourceLibrary > library = styleSheet->getResourceLibrary(skinSymbol->library().get());

                    if (library.valid())
                    {
                        std::string iconName = templateReplace(feature, skinSymbol->name()->expression());

                        auto skin = library->getSkin(iconName);

                        if (skin)
                        {
                            osg::ref_ptr< osg::Image > image = skin->image().get();
                            if (!image.valid())
                            {
                                image = skin->createColorImage(nullptr);
                            }
                            if (image.valid())
                            {
                                BLRectI iconRect(*skin->imageBiasS() * image->s(), *skin->imageBiasT() * image->t(), *skin->imageScaleS() * image->s(), *skin->imageScaleT() * image->t());

                                BLImage sprite;
#if BL_VERSION >= 5120
                                sprite.create_from_data(image->s(), image->t(), BL_FORMAT_PRGB32, image->data(), image->s() * 4);

                                ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
#else
                                sprite.createFromData(image->s(), image->t(), BL_FORMAT_PRGB32, image->data(), image->s() * 4);

                                ctx.setCompOp(BL_COMP_OP_SRC_OVER);
#endif

                                feature->getGeometry()->forEachPart([&](const Geometry* part)
                                    {
                                        // Only label points for now
                                        for (Geometry::const_iterator p = part->begin(); p != part->end(); p++)
                                        {
                                            const osg::Vec3d& p0 = *p;
                                            double x = frame.xf * (p0.x() - frame.xmin);
                                            double y = frame.yf * (p0.y() - frame.ymin);
#if BL_VERSION >= 5120
                                            y = ctx.target_height() - y;
#else
                                            y = ctx.targetHeight() - y;
#endif

                                            ctx.translate(x, y);
                                            ctx.scale(scale);
#if BL_VERSION >= 5120
                                            ctx.blit_image(BLPoint(-iconRect.w / 2.0, -iconRect.h / 2.0), sprite, iconRect);
#else
                                            ctx.blitImage(BLPoint(-iconRect.w / 2.0, -iconRect.h / 2.0), sprite, iconRect);
#endif

#if BL_VERSION >= 5120
                                            ctx.reset_transform();
#elif BL_VERSION >= 2820
                                            ctx.resetTransform();
#else
                                            ctx.resetMatrix();
#endif
                                        }
                                    });
                            }
                        }
                        else
                        {
                            //OE_WARN << "Failed to get skin for " << iconName << std::endl;
                        }
                    }
                }
            }

            if (textSymbol)
            {
                std::string fontSizeText = templateReplace(feature, textSymbol->size()->expression());

                float fontSize = as<float>(fontSizeText, 12) * scale;//feature->eval(fontSizeExpression, session);
#if BL_VERSION >= 5120
                font.create_from_face(getOrCreateFontFace(), fontSize);
#else
                font.createFromFace(getOrCreateFontFace(), fontSize);
#endif
                std::string text = templateReplace(feature, textSymbol->content()->expression());

                feature->getGeometry()->forEachPart([&](const Geometry* part)
                    {
                        for (Geometry::const_iterator p = part->begin(); p != part->end(); p++)
                        {
                            const osg::Vec3d& p0 = *p;
                            double x = frame.xf * (p0.x() - frame.xmin);
                            double y = frame.yf * (p0.y() - frame.ymin);
#if BL_VERSION >= 5120
                            y = ctx.target_height() - y;
#else
                            y = ctx.targetHeight() - y;
#endif

                            if (glyphManager)
                            {
                                // Use the mapboxgl font to render the text.
                                renderMapboxText(ctx, x, y, text, textSymbol, glyphManager, scale, symbolBounds);
                            }
                            else
                            {
                                // Just use Blend's default font rendering
                                if (textSymbol->fill().isSet())
                                {
                                    osgEarth::Color fillColor = textSymbol->fill()->color();
#if BL_VERSION >= 5120
                                    ctx.set_fill_style(BLRgba(fillColor.r(), fillColor.g(), fillColor.b(), fillColor.a()));
                                    ctx.fill_utf8_text(BLPoint(x, y), font, text.c_str());
#else
                                    ctx.setFillStyle(BLRgba(fillColor.r(), fillColor.g(), fillColor.b(), fillColor.a()));
                                    ctx.fillUtf8Text(BLPoint(x, y), font, text.c_str());
#endif
                                }

                                if (textSymbol->halo().isSet())
                                {
                                    osgEarth::Color haloColor = textSymbol->halo()->color();
#if BL_VERSION >= 5120
                                    ctx.set_stroke_style(BLRgba(haloColor.r(), haloColor.g(), haloColor.b(), haloColor.a()));
                                    ctx.set_stroke_width(1);
                                    ctx.stroke_utf8_text(BLPoint(x, y), font, text.c_str());
#else
                                    ctx.setStrokeStyle(BLRgba(haloColor.r(), haloColor.g(), haloColor.b(), haloColor.a()));
                                    ctx.setStrokeWidth(1);
                                    ctx.strokeUtf8Text(BLPoint(x, y), font, text.c_str());
#endif
                                }
                            }
                        }
                    });
            }
        }
#endif
    }
}


using namespace osgEarth::FeatureImageLayerImpl;


FeatureRasterizer::FeatureRasterizer(
    unsigned width, unsigned height, 
    const GeoExtent& extent, 
    const Color& backgroundColor) :

    _width(width),
    _height(height),
    _originalExtent(extent)
{
    unsigned imageWidth = width;
    unsigned imageHeight = height;

    if (_bufferPixels > 0)
    {
        imageWidth = width + 2 * _bufferPixels;
        imageHeight = height + 2 * _bufferPixels;

        double pixelSizeS = extent.width() / (double)width;
        double pixelSizeT = extent.height() / (double)height;

        _bounds = Bounds(
            extent.xMin() - pixelSizeS * (double)_bufferPixels,
            extent.yMin() - pixelSizeT * (double)_bufferPixels,
            0.0,
            extent.xMax() + pixelSizeS * (double)_bufferPixels,
            extent.yMax() + pixelSizeT * (double)_bufferPixels,
            0.0);
    }
    else
    {
        _bounds = _originalExtent.bounds();
    }

    // Allocate the image and initialize it to the background color
    _image = new osg::Image();
    //_image->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    _image->allocateImage(imageWidth, imageHeight, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    ImageUtils::PixelWriter write(_image.get());

#ifdef USE_BLEND2D
    osg::Vec4 bg(backgroundColor.b(), backgroundColor.g(), backgroundColor.r(), backgroundColor.a());
    _implPixelFormat = RF_BGRA;
    _inverted = true;
#else
    osg::Vec4 bg(backgroundColor.a(), backgroundColor.b(), backgroundColor.g(), backgroundColor.r());
    _implPixelFormat = RF_ABGR;
    _inverted = false;
#endif

    write.assign(bg);    
}


FeatureRasterizer::FeatureRasterizer(osg::Image* image, const GeoExtent& extent) :
    _image(image),
    _originalExtent(extent),
    _width(image ? image->s() : 0),
    _height(image ? image->t() : 0)
{
    // disable buffering for a re-allocated image.
    _bufferPixels = 0;
    _bounds = extent.bounds();
}

MapboxGLGlyphManager* FeatureRasterizer::getGlyphManager() const
{
    return _glyphManager.get();
}

void FeatureRasterizer::setGlyphManager(MapboxGLGlyphManager* glyphManager)
{
    _glyphManager = glyphManager;
}

float FeatureRasterizer::getPixelScale() const
{
    return _pixelScale;
}

void FeatureRasterizer::setPixelScale(float pixelScale)
{
    _pixelScale = pixelScale;
}

void
FeatureRasterizer::render_blend2d(
    const FeatureList& features,
    const Style& style,
    FilterContext& context)
{
#ifdef USE_BLEND2D

    // agglite renders in this format:
    _implPixelFormat = RF_BGRA;
    _inverted = true;

    // find the symbology:
    const PointSymbol* masterPoint = style.get<PointSymbol>();
    const LineSymbol* masterLine = style.getSymbol<LineSymbol>();
    const PolygonSymbol* masterPoly = style.getSymbol<PolygonSymbol>();
    const CoverageSymbol* masterCov = style.getSymbol<CoverageSymbol>();
    const TextSymbol* masterText = style.getSymbol<TextSymbol>();
    const SkinSymbol* masterSkin = style.getSymbol<SkinSymbol>();

    // Converts coordinates to image space (double s, t).
    // Blend2D uses a coordinate system where (0,0) is the top-left corner of the image (on the actual corner,
    // not the center up the upper-left pixel) and (s,t) is the bottom-right corner of the bottom-right
    // pixel.  The y-axis is positive down.

    RenderFrame frame;
    frame.xmin = _bounds.xMin();
    frame.ymin = _bounds.yMin();
    frame.xmax = _bounds.xMax();
    frame.ymax = _bounds.yMax();
    frame.xf = (double)_image->s() / (_bounds.xMax() - _bounds.xMin());
    frame.yf = (double)_image->t() / (_bounds.yMax() - _bounds.yMin());

    // set up the render target:
    BLImage buf;
#if BL_VERSION >= 5120
    buf.create_from_data(_image->s(), _image->t(), BL_FORMAT_PRGB32, _image->data(), _image->s() * 4);
#else
    buf.createFromData(_image->s(), _image->t(), BL_FORMAT_PRGB32, _image->data(), _image->s() * 4);
#endif

    BLContext ctx(buf);
#if BL_VERSION >= 5120
    ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
#else
    ctx.setCompOp(BL_COMP_OP_SRC_OVER);
#endif

    // render polygons:
    if (masterPoly)
    {
        for (const auto& feature : features)
        {
            if (feature->getGeometry())
            {
                rasterizePolygons_blend2d(feature->getGeometry(), masterPoly, frame, ctx);
            }
        }
    }

    if (masterLine)
    {
        lineSymbolToBLContext(masterLine, ctx);

        auto& widthExpr = masterLine->stroke()->width();
        auto& outlineWidthExpr = masterLine->stroke()->outlineWidth();

        for (auto& feature : features)
        {
            if (feature->getGeometry() == nullptr)
                continue;

            double lineWidth_px = 1.0f;
            double outlineWidth_px = 0.0f;

            // Calculate the line width in pixels:
            if (widthExpr.isSet())
            {
                Distance lineWidth = widthExpr->eval(feature, context);

                if (lineWidth.getUnits() != Units::PIXELS)
                {
                    // NOTE. If the layer is projected (e.g. spherical meractor) but the map
                    // is geographic, the line width will be inaccurate; this is because the
                    // meractor image will be reprojected and the line widths will shrink
                    // by varying degrees depending on location on the globe. Someday we will
                    // address this but not today.

                    double lineWidth_map_south = _originalExtent.getSRS()->transformDistance(
                        lineWidth,
                        _originalExtent.getSRS()->getUnits(),
                        _originalExtent.yMin());

                    double lineWidth_map_north = _originalExtent.getSRS()->transformDistance(
                        lineWidth,
                        _originalExtent.getSRS()->getUnits(),
                        _originalExtent.yMax());

                    double lineWidth_map = std::min(lineWidth_map_south, lineWidth_map_north);

                    double pixelSize_map = _originalExtent.height() / (double)_image->t();

                    lineWidth_px = (lineWidth_map / pixelSize_map);

                    // enfore a minimum width of one pixel.
                    double minPixels = masterLine->stroke()->minPixels().getOrUse(1.0);
                    lineWidth_px = osg::clampAbove(lineWidth_px, minPixels);
                }

                else // pixels already
                {
                    lineWidth_px = lineWidth.getValue();
                }
            }

            if (outlineWidthExpr.isSet())
            {
                Distance outlineWidth = outlineWidthExpr->eval(feature, context);

                if (outlineWidth.getUnits() != Units::PIXELS)
                {
                    double lineWidth_map_south = _originalExtent.getSRS()->transformDistance(
                        outlineWidth,
                        _originalExtent.getSRS()->getUnits(),
                        _originalExtent.yMin());

                    double lineWidth_map_north = _originalExtent.getSRS()->transformDistance(
                        outlineWidth,
                        _originalExtent.getSRS()->getUnits(),
                        _originalExtent.yMax());

                    double lineWidth_map = std::min(lineWidth_map_south, lineWidth_map_north);
                    double pixelSize_map = _originalExtent.height() / (double)_image->t();
                    outlineWidth_px = (lineWidth_map / pixelSize_map);

                    // enfore a minimum width of one pixel.
                    double minPixels = masterLine->stroke()->minPixels().getOrUse(1.0);
                    outlineWidth_px = std::max(outlineWidth_px, minPixels);
                }

                else // pixels already
                {
                    outlineWidth_px = outlineWidth.getValue();
                }
            }

            // Rasterize the outlines:
            if (outlineWidth_px > 0.0f)
            {
                rasterizeLines(
                    feature->getGeometry(),
                    masterLine->stroke()->outlineColor().get(),
                    outlineWidth_px,
                    frame, ctx);
            }

            rasterizeLines(
                feature->getGeometry(),
                masterLine->stroke()->color(),
                lineWidth_px,
                frame, ctx);
        }
    }

    if (masterPoint)
    {
        float width = masterPoint->size().value();

#if BL_VERSION >= 5120
        ctx.set_fill_style(BLRgba(
#else
        ctx.setFillStyle(BLRgba(
#endif
            masterPoint->fill()->color().r(), masterPoint->fill()->color().g(), masterPoint->fill()->color().b(), masterPoint->fill()->color().a()));

        for (const auto& feature : features)
        {
            feature->getGeometry()->forEachPart([&](const Geometry* part)
                {
                    for (auto& p : *part)
                    {
                        double x = frame.xf * (p.x() - frame.xmin);
                        double y = frame.yf * (p.y() - frame.ymin);
#if BL_VERSION >= 5120
                        y = ctx.target_height() - y;
                        ctx.fill_circle(x, y, width / 2.0);
#else
                        y = ctx.targetHeight() - y;
                        ctx.fillCircle(x, y, width / 2.0);
#endif
                    }
                });
        }
    }

    if (masterText || masterSkin)
    {   
        // Sort the features based their location.  We do this to ensure that features collected in a metatiling
        // fashion will always be rendered in the same order when rendered in multiple neighboring tiles
        // so the decluttering algorithm will work consistently across tiles.
        FeatureList sortedFeatures(features);
        std::sort(sortedFeatures.begin(), sortedFeatures.end(), [](auto& a, auto& b) {
            auto centerA = a->getGeometry()->getBounds().center();
            auto centerB = b->getGeometry()->getBounds().center();
            if (centerA.x() < centerB.x()) return true;
            if (centerA.x() > centerB.x()) return false;
            return centerA.y() < centerB.y();
            });

        // Rasterize the symbols:
        auto* sheet = context.session() ? context.session()->styles() : nullptr;

        for (const auto& feature : sortedFeatures)
        {
            if (feature->getGeometry())
            {
                rasterizeSymbols(feature.get(), sheet, masterText, masterSkin, frame, ctx, _glyphManager.get(), getPixelScale(), _symbolBoundingBoxes);
            }
        }
    }

    ctx.end();

#if 0
    ImageUtils::PixelReader read(_image.get());
    ImageUtils::PixelWriter write(_image.get());
    for (unsigned int y = 0; y < _image->t(); ++y)
    {
        for (unsigned int x = 0; x < _image->s(); ++x)
        {
            osg::Vec4 pixel = read(x, y);
            if (pixel.a() > 0.0f)
            {
                // unpremultiply the alpha
                pixel.r() = pixel.r() / pixel.a();
                pixel.g() = pixel.g() / pixel.a();
                pixel.b() = pixel.b() / pixel.a();
            }
            write(pixel, x, y);
        }
    }
#endif

#endif // USE_BLEND2D
}

void
FeatureRasterizer::render_agglite(
    const FeatureList& features,
    const Style& style,
    FilterContext& context)
{
    auto* featureProfile = context.featureProfile();
    auto* sheet = context.session() ? context.session()->styles() : nullptr;

    // agglite renders in this format:
    _implPixelFormat = RF_ABGR;
    _inverted = false;

    // find the symbology:
    const LineSymbol* globalLineSymbol = style.getSymbol<LineSymbol>();
    const PolygonSymbol* globalPolySymbol = style.getSymbol<PolygonSymbol>();
    const CoverageSymbol* globalCovSymbol = style.getSymbol<CoverageSymbol>();

    // Converts coordinates to image space (s,t):
    RenderFrame frame;
    frame.xmin = _bounds.xMin();
    frame.ymin = _bounds.yMin();
    frame.xmax = _bounds.xMax();
    frame.ymax = _bounds.yMax();
    frame.xf = (double)_image->s() / (_bounds.xMax() - _bounds.xMin());
    frame.yf = (double)_image->t() / (_bounds.yMax() - _bounds.yMin());

    // sort into bins, making a copy for lines that require buffering.
    FeatureList polygons;
    FeatureList lines;

    //FilterContext context;
    const SpatialReference* featureSRS = features.front()->getSRS();

    OE_SOFT_ASSERT_AND_RETURN(featureSRS != nullptr, void());

    for(auto& f : features)
    {
        const LineSymbol* lineSymbol = globalLineSymbol;
        const PolygonSymbol* polySymbol = globalPolySymbol;
        const CoverageSymbol* covSymbol = globalCovSymbol;

        if (f->getGeometry())
        {
            bool addPolygon = false;
            bool addLineOrOutline = false;

            // first see if the feature has overriding symbols:
            if (f->style())
            {
                if (f->style()->has<PolygonSymbol>())
                    polySymbol = f->style()->get<PolygonSymbol>();

                if (f->style()->has<LineSymbol>())
                    lineSymbol = f->style()->get<LineSymbol>();

                if (f->style()->has<CoverageSymbol>())
                    covSymbol = f->style()->get<CoverageSymbol>();
            }

            // if it's a polygon and we have a polygon symbol, queue it
            if (f->getGeometry()->isPolygon())
            {
                if (polySymbol || covSymbol)
                {
                    addPolygon = true;
                }

                else if (lineSymbol || covSymbol)
                {
                    addLineOrOutline = true;
                }
            }

            // polygons can use the line symbol for an outline, so polygons AND lines 
            // can get queued as lines:
            else if (f->getGeometry()->isLinear())
            {
                if (lineSymbol || covSymbol)
                {
                    addLineOrOutline = true;
                }
            }

            if (addPolygon)
            {
                polygons.push_back(f);
            }

            if (addLineOrOutline)
            {
                // Use the GeometryIterator to get all the geometries so we can
                // clone the lines as rings
                GeometryIterator gi(f->getGeometry());
                while (gi.hasMore())
                {
                    Geometry* geom = gi.next();
                    Feature* newFeature = new Feature(*f.get());
                    newFeature->setGeometry(geom);
                    if (!newFeature->getGeometry()->isLinear())
                    {
                        newFeature->setGeometry(newFeature->getGeometry()->cloneAs(Geometry::TYPE_RING));
                    }

                    lines.push_back(newFeature);
                }
            }

            //// if there are no geometry symbols but there is a coverage symbol, default to polygons.
            //if (!hasLine && !hasPoly && covSymbol != nullptr)
            //{
            //    polygons.push_back(f);
            //}
        }
    }

    if (lines.size() > 0)
    {
        // We are buffering in the features native extent, so we need to use the
        // transformed extent to get the proper "resolution" for the image
        GeoExtent transformedExtent = _originalExtent.transform(featureSRS);

        double trans_xf = (double)_image->s() / transformedExtent.width();
        double trans_yf = (double)_image->t() / transformedExtent.height();

        // resolution of the image (pixel extents):
        double xres = 1.0 / trans_xf;
        double yres = 1.0 / trans_yf;

        // downsample the line data so that it is no higher resolution than to image to which
        // we intend to rasterize it. If you don't do this, you run the risk of the buffer
        // operation taking forever on very high-res input data.
        ResampleFilter resample;
        resample.minLength() = osg::minimum(xres, yres);
        context = resample.push(lines, context);

        // now run the buffer operation on all lines:
        BufferFilter buffer;

        GeoExtent imageExtentInFeatureSRS = _originalExtent.transform(featureSRS);
        double pixelWidth = imageExtentInFeatureSRS.width() / (double)_image->s();
        double lineWidth_px = pixelWidth;

        if (globalLineSymbol)
        {
            buffer.capStyle() = globalLineSymbol->stroke()->lineCap().value();

            if (globalLineSymbol->stroke()->width().isSet())
            {
                Distance lineWidth = globalLineSymbol->stroke()->width()->literal();

                // if the width units are specified, process them:
                if (lineWidth.getUnits() != Units::PIXELS)
                {
                    auto& featureUnits = featureSRS->getUnits();

                    // if the units are different than those of the feature data, we need to
                    // do a units conversion.
                    if (featureUnits != lineWidth.getUnits())
                    {
                        if (Units::canConvert(lineWidth.getUnits(), featureUnits))
                        {
                            // linear to linear, no problem
                            lineWidth = lineWidth.to(featureUnits);
                        }
                        else if (lineWidth.getUnits().isLinear() && featureUnits.isAngular())
                        {
                            // linear to angular? approximate degrees per meter at the
                            // latitude of the tile's centroid.
                            double lineWidthM = lineWidth.as(Units::METERS);
                            double mPerDegAtEquatorInv = 360.0 / (featureSRS->getEllipsoid().getRadiusEquator() * 2.0 * osg::PI);
                            GeoPoint ll = _originalExtent.getCentroid();
                            lineWidth_px = lineWidthM * mPerDegAtEquatorInv * cos(osg::DegreesToRadians(ll.y()));
                        }
                    }

                    // enfore a minimum width of one pixel.
                    float minPixels = globalLineSymbol->stroke()->minPixels().getOrUse(1.0f);
                    lineWidth_px = osg::clampAbove(lineWidth.getValue(), pixelWidth*minPixels);
                }

                else // pixels
                {
                    lineWidth_px *= pixelWidth;
                }
            }
        }

        buffer.distance() = lineWidth_px * 0.5;   // since the distance is for one side
        buffer.push(lines, context);
    }

    // Transform the features into the map's SRS:
    for (auto& polygon : polygons)
        polygon->transform(_originalExtent.getSRS());

    for (auto& line : lines)
        line->transform(_originalExtent.getSRS());

    // set up the AGG renderer:
    agg::rendering_buffer rbuf(
        _image->data(),
        _image->s(), _image->t(),
        _image->s() * 4);

    // Create the renderer and the rasterizer
    agg::rasterizer ras;
    ras.filling_rule(agg::fill_even_odd);

    // construct an extent for cropping the geometry to our tile.
    // extend just outside the actual extents so we don't get edge artifacts:
    GeoExtent cropExtent = GeoExtent(_originalExtent);
    cropExtent.scale(1.1, 1.1);
    double cropXMin, cropYMin, cropXMax, cropYMax;
    cropExtent.getBounds(cropXMin, cropYMin, cropXMax, cropYMax);

    // GEOS crop won't abide by weird extents, so if we're in geographic space
    // we must clamp the scaled extent back to a legal range.
    if (cropExtent.crossesAntimeridian())
    {
        GeoPoint centroid = _originalExtent.getCentroid();
        if (centroid.x() < 0.0) // tile is east of antimeridian
        {
            cropXMin = -180.0;
            cropXMax = cropExtent.east();
        }
        else
        {
            cropXMin = cropExtent.west();
            cropXMax = 180.0;
        }
    }

    osgEarth::Polygon cropPoly(4);
    cropPoly.push_back(osg::Vec3d(cropXMin, cropYMin, 0));
    cropPoly.push_back(osg::Vec3d(cropXMax, cropYMin, 0));
    cropPoly.push_back(osg::Vec3d(cropXMax, cropYMax, 0));
    cropPoly.push_back(osg::Vec3d(cropXMin, cropYMax, 0));

    // If there's a coverage symbol, make a copy of the expressions so we can evaluate them
    //optional<NumericExpression> covValue;
    const CoverageSymbol* covSymbol = style.get<CoverageSymbol>();
    //if (covSymbol && covSymbol->value().isSet())
    //{
    //    covValue = covSymbol->value().get();
    //}

    // render the polygons
    for (auto& feature : polygons)
    {
        Geometry* geometry = feature->getGeometry();

        osg::ref_ptr<Geometry> cropped = geometry->crop(&cropPoly);
        if (cropped.valid())
        {
            if (covSymbol)
            {
                float value = covSymbol->value()->eval(feature, context);
                rasterizeCoverage_agglite(cropped.get(), value, frame, ras, rbuf);
            }
            else
            {
                const PolygonSymbol* poly =
                    feature->style() && feature->style()->has<PolygonSymbol>() ? feature->style()->get<PolygonSymbol>() :
                    globalPolySymbol;

                Color color = poly ? poly->fill()->color() : Color::White;
                rasterize_agglite(cropped.get(), color, frame, ras, rbuf);
            }
        }
    }

    for (auto& feature : lines)
    {
        Geometry* geometry = feature->getGeometry();

        osg::ref_ptr<Geometry> cropped = geometry->crop(&cropPoly);
        if (cropped.valid())
        {
            if (covSymbol)
            {
                float value = covSymbol->value()->eval(feature, context);
                rasterizeCoverage_agglite(cropped.get(), value, frame, ras, rbuf);
            }
            else
            {
                const LineSymbol* line =
                    feature->style() && feature->style()->has<LineSymbol>() ? feature->style()->get<LineSymbol>() :
                    globalLineSymbol;

                osg::Vec4f color = line ? static_cast<osg::Vec4>(line->stroke()->color()) : osg::Vec4(1, 1, 1, 1);
                rasterize_agglite(cropped.get(), color, frame, ras, rbuf);
            }
        }
    }
}

void
FeatureRasterizer::render(const FeatureList& features, const Style& style, FilterContext& context)
{
    if (features.empty())
        return;

    OE_PROFILING_ZONE;

    const SpatialReference* featureSRS = features.front()->getSRS();
    OE_SOFT_ASSERT_AND_RETURN(featureSRS != nullptr, void());

    // Transform to map SRS:
    if (!featureSRS->isHorizEquivalentTo(_originalExtent.getSRS()))
    {
        OE_PROFILING_ZONE_NAMED("Transform");
        for (auto& feature : features)
            feature->transform(_originalExtent.getSRS());
    }

#ifdef USE_BLEND2D
    if (style.get<CoverageSymbol>())
        render_agglite(features, style, context);
    else
        render_blend2d(features, style, context);
#else
    render_agglite(features, style, context);
#endif
}

GeoImage
FeatureRasterizer::finalize()
{
    if (_image->getPixelSizeInBits() == 32 &&
        _image->getDataType() == GL_UNSIGNED_BYTE)
    {
        unsigned int totalSizeInBytes = _image->getTotalSizeInBytes();
        unsigned char* pixel = _image->data();

        if (_implPixelFormat == RF_BGRA)
        {
            //convert from BGRA to RGBA
            for (unsigned i = 0; i < totalSizeInBytes; i += 4, pixel += 4)
            {
                std::swap(pixel[0], pixel[2]);
            }
        }
        else if (_implPixelFormat == RF_ABGR)
        {
            //convert from ABGR to RGBA
            for (unsigned i = 0; i < totalSizeInBytes; i += 4, pixel += 4)
            {
                std::swap(pixel[0], pixel[3]);
                std::swap(pixel[1], pixel[2]);
            }
        }
    }

    // unbuffer
    if (_bufferPixels > 0)
    {
        auto finalImage = new osg::Image();
        finalImage->allocateImage(_width, _height, 1, _image->getPixelFormat(), _image->getDataType());
        ImageUtils::PixelReader read(_image.get());
        ImageUtils::PixelWriter write(finalImage);
        
        osg::Vec4 pixel;
        ImageUtils::ImageIterator iter(finalImage);
        iter.forEachPixel([&](auto& i) {
            read(pixel, i.s() + _bufferPixels, i.t() + _bufferPixels, i.r());
            write(pixel, i.s(), i.t(), i.r());
        });

        _image = finalImage;
    }

    if (_inverted)
    {
        _image->flipVertical();
    }

    return GeoImage(_image.release(), _originalExtent);
}
