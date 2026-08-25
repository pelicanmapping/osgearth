/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "FeatureImageLayerSlug.h"

#include <osgEarth/Notify>
#include <osgEarth/PointSymbol>
#include <osgEarth/LineSymbol>
#include <osgEarth/PolygonSymbol>

#include <osg/Image>
#include <osg/Texture2D>

#include <slughorn/canvas.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif

using namespace osgEarth;
using namespace osgEarth::Util;

namespace
{
    constexpr unsigned MAX_SLUG_LAYERS = 64u;
    constexpr size_t MAX_SOURCE_VERTICES_PER_SHAPE = 1024u;
    constexpr unsigned METADATA_HEADER_TEXELS = 3u;
    constexpr unsigned METADATA_TEXELS_PER_LAYER = 4u;
    std::once_flag s_unsupportedSymbolWarning;
    std::atomic_uint s_curveOverflowWarnings{ 0u };

    int bandCountForCurveCount(std::size_t curveCount)
    {
        // Retain Slughorn's inexpensive default for simple shapes. Complex
        // shapes use the complete 32-cell indirection grid; every resulting
        // 1/32 band is nested inside an old 1/16 band, so its curve candidate
        // list cannot grow.
        const auto automaticCount = std::max<std::size_t>(1u, curveCount / 2u);
        const auto count = automaticCount > 16u ?
            std::size_t{ slughorn::Atlas::INDIRECTION_SIZE } : automaticCount;
        return static_cast<int>(count);
    }

    std::pair<std::vector<slughorn::slug_t>, std::vector<slughorn::slug_t>>
    complexityAwareBandSplits(const slughorn::Atlas::Curves& curves)
    {
        const int bandCount = bandCountForCurveCount(curves.size());
        return slughorn::Atlas::computeUniformSplits(
            curves, bandCount, bandCount);
    }

    slughorn::Color toSlugColor(const Color& value)
    {
        return {
            static_cast<float>(value.r()),
            static_cast<float>(value.g()),
            static_cast<float>(value.b()),
            static_cast<float>(value.a())
        };
    }

    bool sameColor(const Color& lhs, const Color& rhs)
    {
        return lhs == rhs;
    }

    slughorn::canvas::LineJoin toSlugJoin(Stroke::LineJoinStyle value)
    {
        return value == Stroke::LINEJOIN_MITRE ?
            slughorn::canvas::LineJoin::Miter :
            slughorn::canvas::LineJoin::Round;
    }

    slughorn::canvas::LineCap toSlugCap(Stroke::LineCapStyle value)
    {
        return
            value == Stroke::LINECAP_SQUARE ? slughorn::canvas::LineCap::Square :
            value == Stroke::LINECAP_ROUND ? slughorn::canvas::LineCap::Round :
            slughorn::canvas::LineCap::Butt;
    }
}

struct FeatureImageLayerSlug::Impl
{
    struct StrokeGroup
    {
        slughorn::canvas::Path path;
        Color color = Color::White;
        double widthPixels = 1.0;
        Stroke::LineJoinStyle join = Stroke::LINEJOIN_ROUND;
        Stroke::LineCapStyle cap = Stroke::LINECAP_FLAT;
        size_t sourceVertexCount = 0u;
    };

    struct FillGroup
    {
        slughorn::canvas::Path path;
        Color color = Color::White;
        size_t sourceVertexCount = 0u;
    };

    Impl(
        unsigned tileSize_,
        const GeoExtent& extent_,
        const Color& backgroundColor_,
        unsigned atlasTextureWidth) :
        tileSize(tileSize_),
        extent(extent_),
        backgroundColor(backgroundColor_),
        atlas(atlasTextureWidth),
        canvas(atlas, slughorn::KeyIterator("osgearth"))
    {
        canvas.setSplitStrategy(complexityAwareBandSplits);
    }

    unsigned tileSize;
    GeoExtent extent;
    Color backgroundColor;
    slughorn::Atlas atlas;
    slughorn::canvas::Canvas canvas;
    bool finalized = false;
    bool warnedLayerLimit = false;
    size_t droppedCurveCount = 0u;
    size_t truncatedShapeCount = 0u;

    float normalizeX(double x) const
    {
        if (extent.crossesAntimeridian() && x < extent.xMin())
            x += 360.0;
        return static_cast<float>((x - extent.xMin()) / extent.width());
    }

    float normalizeY(double y) const
    {
        return static_cast<float>((y - extent.yMin()) / extent.height());
    }

    bool appendGeometry(
        slughorn::canvas::Path& path,
        const Geometry* geometry,
        bool closeRings) const
    {
        if (!geometry)
            return false;

        bool added = false;
        geometry->forEachPart(true, [&](const Geometry* part)
        {
            if (!part || part->empty() || part->isPointSet())
                return;

            auto i = part->begin();
            path.moveTo(normalizeX(i->x()), normalizeY(i->y()));
            ++i;

            for (; i != part->end(); ++i)
                path.lineTo(normalizeX(i->x()), normalizeY(i->y()));

            if (closeRings || part->getType() == Geometry::TYPE_RING ||
                part->getType() == Geometry::TYPE_POLYGON)
            {
                path.closePath();
            }

            added = true;
        });
        return added;
    }

    size_t countVertices(const Geometry* geometry) const
    {
        size_t result = 0u;
        if (geometry)
        {
            geometry->forEachPart(true, [&](const Geometry* part)
            {
                if (part)
                    result += part->size();
            });
        }
        return result;
    }

    double toPixels(
        const optional<Expression<Distance>>& expression,
        Feature* feature,
        FilterContext& context,
        double fallback,
        double minPixels) const
    {
        if (!expression.isSet())
            return fallback;

        Distance width = expression->eval(feature, context);
        if (width.getUnits() == Units::PIXELS)
            return width.getValue();

        const double south = extent.getSRS()->transformDistance(
            width, extent.getSRS()->getUnits(), extent.yMin());
        const double north = extent.getSRS()->transformDistance(
            width, extent.getSRS()->getUnits(), extent.yMax());
        const double mapWidth = std::min(south, north);
        const double pixelSize = extent.height() / static_cast<double>(tileSize);
        return std::max(mapWidth / pixelSize, minPixels);
    }

    bool canAddLayer()
    {
        if (canvas.layerCount() < MAX_SLUG_LAYERS)
            return true;

        if (!warnedLayerLimit)
        {
            OE_WARN << "[FeatureImageLayer/Slug] Tile exceeded " << MAX_SLUG_LAYERS
                << " style layers; remaining style passes will be omitted" << std::endl;
            warnedLayerLimit = true;
        }
        return false;
    }

    void enforceAtlasRowLimit(const slughorn::Layer& layer)
    {
        const auto shape = atlas.getShape(layer.key);
        if (!shape || shape->curves.empty())
            return;

        const size_t rowWidth = atlas.getTextureWidth();
        const auto splits = complexityAwareBandSplits(shape->curves);
        const size_t numBandsX = splits.first.size() + 1u;
        const size_t numBandsY = splits.second.size() + 1u;

        const float minX = shape->bearingX;
        const float maxX = shape->bearingX + shape->width;
        const float minY = shape->bearingY - shape->height;
        const float maxY = shape->bearingY;
        const float rangeX = std::max(maxX - minX, 1e-6f);
        const float rangeY = std::max(maxY - minY, 1e-6f);

        auto makeBoundaries = [](float minimum, float range,
                                 const std::vector<slughorn::slug_t>& values)
        {
            std::vector<float> result(values.size() + 2u);
            result.front() = minimum;
            result.back() = minimum + range;
            for (size_t i = 0u; i < values.size(); ++i)
            {
                const float snapped = std::round(
                    static_cast<float>(values[i]) *
                    slughorn::Atlas::INDIRECTION_SIZE) /
                    slughorn::Atlas::INDIRECTION_SIZE;
                result[i + 1u] = minimum + snapped * range;
            }
            return result;
        };

        const auto xBoundaries = makeBoundaries(minX, rangeX, splits.first);
        const auto yBoundaries = makeBoundaries(minY, rangeY, splits.second);
        std::vector<size_t> xCounts(numBandsX, 0u);
        std::vector<size_t> yCounts(numBandsY, 0u);
        std::vector<size_t> xBands;
        std::vector<size_t> yBands;
        slughorn::Atlas::Curves kept;
        kept.reserve(shape->curves.size());

        std::vector<size_t> contourStarts;
        const bool hasContourStarts = !shape->contourStarts.empty();
        size_t nextContour = hasContourStarts ? 1u : 0u;
        bool needsContourStart = hasContourStarts;

        for (size_t curveIndex = 0u;
             curveIndex < shape->curves.size(); ++curveIndex)
        {
            while (hasContourStarts && nextContour < shape->contourStarts.size() &&
                   curveIndex >= shape->contourStarts[nextContour])
            {
                needsContourStart = true;
                ++nextContour;
            }

            const auto& curve = shape->curves[curveIndex];
            const float curveMinX = std::min({ curve.x1, curve.x2, curve.x3 });
            const float curveMaxX = std::max({ curve.x1, curve.x2, curve.x3 });
            const float curveMinY = std::min({ curve.y1, curve.y2, curve.y3 });
            const float curveMaxY = std::max({ curve.y1, curve.y2, curve.y3 });

            xBands.clear();
            yBands.clear();
            for (size_t band = 0u; band < numBandsX; ++band)
            {
                if (curveMaxX >= xBoundaries[band] &&
                    curveMinX <= xBoundaries[band + 1u])
                {
                    xBands.push_back(band);
                }
            }
            for (size_t band = 0u; band < numBandsY; ++band)
            {
                if (curveMaxY >= yBoundaries[band] &&
                    curveMinY <= yBoundaries[band + 1u])
                {
                    yBands.push_back(band);
                }
            }

            const bool fitsX = std::all_of(
                xBands.begin(), xBands.end(),
                [&](size_t band) { return xCounts[band] < rowWidth; });
            const bool fitsY = std::all_of(
                yBands.begin(), yBands.end(),
                [&](size_t band) { return yCounts[band] < rowWidth; });
            if (!fitsX || !fitsY)
                continue;

            if (needsContourStart)
            {
                if (contourStarts.empty() || contourStarts.back() != kept.size())
                    contourStarts.push_back(kept.size());
                needsContourStart = false;
            }
            kept.push_back(curve);
            for (const auto band : xBands)
                ++xCounts[band];
            for (const auto band : yBands)
                ++yCounts[band];
        }

        const size_t dropped = shape->curves.size() - kept.size();
        if (dropped == 0u)
            return;

        slughorn::Atlas::ShapeInfo replacement;
        replacement.curves = std::move(kept);
        replacement.contourStarts = std::move(contourStarts);
        replacement.autoMetrics = false;
        replacement.bearingX = shape->bearingX;
        replacement.bearingY = shape->bearingY;
        replacement.width = shape->width;
        replacement.height = shape->height;
        replacement.advance = shape->advance;
        replacement.splitsX = splits.first;
        replacement.splitsY = splits.second;
        replacement.origin = shape->origin;
        atlas.addShape(layer.key, replacement);

        droppedCurveCount += dropped;
        ++truncatedShapeCount;
    }

    void commitFill(slughorn::canvas::Path& path, const Color& color)
    {
        if (canAddLayer() && path.hasPendingPath())
            enforceAtlasRowLimit(canvas.fill(path, toSlugColor(color)));
    }

    void commitStroke(const StrokeGroup& group)
    {
        if (!canAddLayer() || !group.path.hasPendingPath() || group.widthPixels <= 0.0)
            return;

        const float normalizedWidth = static_cast<float>(
            group.widthPixels / static_cast<double>(tileSize));

        enforceAtlasRowLimit(canvas.stroke(
            group.path,
            normalizedWidth,
            toSlugColor(group.color),
            1.0f,
            {},
            toSlugJoin(group.join),
            toSlugCap(group.cap)));
    }

    StrokeGroup& getStrokeGroup(
        std::vector<StrokeGroup>& groups,
        const Color& color,
        double widthPixels,
        Stroke::LineJoinStyle join,
        Stroke::LineCapStyle cap,
        size_t sourceVertexCount)
    {
        if (!groups.empty())
        {
            auto& group = groups.back();
            if (sameColor(group.color, color) &&
                std::abs(group.widthPixels - widthPixels) < 1e-4 &&
                group.join == join && group.cap == cap &&
                group.sourceVertexCount + sourceVertexCount <=
                    MAX_SOURCE_VERTICES_PER_SHAPE)
            {
                group.sourceVertexCount += sourceVertexCount;
                return group;
            }
        }

        groups.emplace_back();
        auto& result = groups.back();
        result.color = color;
        result.widthPixels = widthPixels;
        result.join = join;
        result.cap = cap;
        result.sourceVertexCount = sourceVertexCount;
        return result;
    }

    FillGroup& getFillGroup(
        std::vector<FillGroup>& groups,
        const Color& color,
        size_t sourceVertexCount)
    {
        if (!groups.empty() && sameColor(groups.back().color, color) &&
            groups.back().sourceVertexCount + sourceVertexCount <=
                MAX_SOURCE_VERTICES_PER_SHAPE)
        {
            groups.back().sourceVertexCount += sourceVertexCount;
            return groups.back();
        }

        groups.emplace_back();
        groups.back().color = color;
        groups.back().sourceVertexCount = sourceVertexCount;
        return groups.back();
    }

    void render(const FeatureList& features, const Style& style, FilterContext& context)
    {
        if (features.empty() || finalized)
            return;

        const SpatialReference* featureSRS = features.front()->getSRS();
        if (!featureSRS)
            return;

        if (!featureSRS->isHorizEquivalentTo(extent.getSRS()))
        {
            for (auto& feature : features)
                feature->transform(extent.getSRS());
        }

        const PolygonSymbol* masterPolygon = style.getSymbol<PolygonSymbol>();
        const LineSymbol* masterLine = style.getSymbol<LineSymbol>();
        const PointSymbol* masterPoint = style.getSymbol<PointSymbol>();

        if (style.getSymbol<TextSymbol>() || style.getSymbol<SkinSymbol>() ||
            style.getSymbol<CoverageSymbol>())
        {
            std::call_once(s_unsupportedSymbolWarning, []()
            {
                OE_WARN << "[FeatureImageLayer/Slug] Text, skin/icon, and coverage "
                    "symbols are not supported by the prototype" << std::endl;
            });
        }

        // Polygon fills. Consecutive equal colors share a Slug shape; starting a
        // new group on each style change preserves feature draw order.
        if (masterPolygon)
        {
            std::vector<FillGroup> groups;
            for (const auto& feature : features)
            {
                if (!feature->getGeometry() || !feature->getGeometry()->isPolygon())
                    continue;

                const PolygonSymbol* polygon = masterPolygon;
                if (feature->style() && feature->style()->has<PolygonSymbol>())
                    polygon = feature->style()->get<PolygonSymbol>();

                const Color color = polygon->fill()->color();
                appendGeometry(
                    getFillGroup(
                        groups, color, countVertices(feature->getGeometry())).path,
                    feature->getGeometry(),
                    true);
            }

            for (auto& group : groups)
                commitFill(group.path, group.color);
        }

        // Line strings and polygon outlines. Width expressions are evaluated per
        // feature and equal results share one Slug shape.
        if (masterLine)
        {
            std::vector<StrokeGroup> outlines;
            std::vector<StrokeGroup> strokes;

            for (const auto& feature : features)
            {
                const Geometry* geometry = feature->getGeometry();
                if (!geometry || (!geometry->isLinear() && !geometry->isPolygon()))
                    continue;

                const LineSymbol* line = masterLine;
                if (feature->style() && feature->style()->has<LineSymbol>())
                    line = feature->style()->get<LineSymbol>();

                const PolygonSymbol* polygon = masterPolygon;
                if (feature->style() && feature->style()->has<PolygonSymbol>())
                    polygon = feature->style()->get<PolygonSymbol>();
                if (geometry->isPolygon() && polygon && !polygon->outline().get())
                    continue;

                const Stroke& stroke = line->stroke().get();
                const auto join = stroke.lineJoin().get();
                const auto cap = stroke.lineCap().get();
                const size_t sourceVertexCount = countVertices(geometry);
                const double widthPixels = toPixels(
                    stroke.width(), feature.get(), context, 1.0,
                    stroke.minPixels().getOrUse(1.0f));

                auto& mainGroup = getStrokeGroup(
                    strokes, stroke.color(), widthPixels, join, cap,
                    sourceVertexCount);
                appendGeometry(mainGroup.path, geometry, false);

                if (stroke.outlineWidth().isSet())
                {
                    const double outlinePixels = toPixels(
                        stroke.outlineWidth(), feature.get(), context, 0.0,
                        stroke.minPixels().getOrUse(1.0f));
                    auto& outlineGroup = getStrokeGroup(
                        outlines, stroke.outlineColor().get(), outlinePixels, join, cap,
                        sourceVertexCount);
                    appendGeometry(outlineGroup.path, geometry, false);
                }
            }

            for (const auto& group : outlines)
                commitStroke(group);
            for (const auto& group : strokes)
                commitStroke(group);
        }

        // Point symbols become analytic circles in tile coordinates.
        if (masterPoint)
        {
            std::vector<FillGroup> groups;
            for (const auto& feature : features)
            {
                const Geometry* geometry = feature->getGeometry();
                if (!geometry || !geometry->isPointSet())
                    continue;

                const PointSymbol* point = masterPoint;
                if (feature->style() && feature->style()->has<PointSymbol>())
                    point = feature->style()->get<PointSymbol>();

                auto& group = getFillGroup(
                    groups,
                    point->fill()->color(),
                    countVertices(geometry));
                const float radius = point->size().getOrUse(1.0f) /
                    (2.0f * static_cast<float>(tileSize));

                geometry->forEachPart([&](const Geometry* part)
                {
                    for (const auto& position : *part)
                        group.path.circle(
                            normalizeX(position.x()),
                            normalizeY(position.y()),
                            radius);
                });
            }

            for (auto& group : groups)
                commitFill(group.path, group.color);
        }
    }

    TextureWindow finalize()
    {
        if (finalized)
            return {};
        finalized = true;

        if (droppedCurveCount > 0u)
        {
            const unsigned warningIndex = s_curveOverflowWarnings.fetch_add(1u);
            if (warningIndex < 10u)
            {
                OE_WARN << "[FeatureImageLayer/Slug] " << atlas.getTextureWidth()
                    << "-texel atlas rows omitted "
                    << droppedCurveCount << " curves from " << truncatedShapeCount
                    << " shapes in one tile"
                    << (warningIndex == 9u ? "; further warnings suppressed" : "")
                    << std::endl;
            }
        }

        slughorn::CompositeShape composite = canvas.finalize();

        try
        {
            if (!composite.empty())
                atlas.build();
        }
        catch (const std::exception& e)
        {
            OE_WARN << "[FeatureImageLayer/Slug] Slughorn atlas build failed: "
                << e.what() << std::endl;
            return {};
        }

        const unsigned layerCount = static_cast<unsigned>(composite.layers.size());
        const unsigned atlasWidth = composite.empty() ?
            4u : atlas.getTextureWidth();
        const unsigned metadataTexels = METADATA_HEADER_TEXELS +
            layerCount * METADATA_TEXELS_PER_LAYER;
        const unsigned metadataRows = std::max(
            1u, (metadataTexels + atlasWidth - 1u) / atlasWidth);

        const auto& curveData = atlas.getCurveTextureData();
        const auto& bandData = atlas.getBandTextureData();
        const unsigned curveRows = composite.empty() ? 0u : curveData.height;
        const unsigned bandRows = composite.empty() ? 0u : bandData.height;
        const unsigned curveRowOffset = metadataRows;
        const unsigned bandRowOffset = curveRowOffset + curveRows;
        const unsigned textureHeight = bandRowOffset + bandRows;

        const std::size_t expectedCurveBytes =
            static_cast<std::size_t>(curveData.width) * curveData.height *
            4u * sizeof(float);
        const std::size_t expectedBandBytes =
            static_cast<std::size_t>(bandData.width) * bandData.height *
            4u * sizeof(std::uint16_t);
        if (!composite.empty() &&
            (curveData.width != atlasWidth || bandData.width != atlasWidth ||
             curveData.bytes.size() != expectedCurveBytes ||
             bandData.bytes.size() != expectedBandBytes))
        {
            OE_WARN << "[FeatureImageLayer/Slug] Slughorn returned invalid atlas data"
                << std::endl;
            return {};
        }

        osg::ref_ptr<osg::Image> image = new osg::Image();
        image->allocateImage(
            static_cast<int>(atlasWidth),
            static_cast<int>(textureHeight),
            1,
            GL_RGBA,
            GL_FLOAT);
        image->setInternalTextureFormat(GL_RGBA32F);
        std::memset(image->data(), 0, image->getTotalSizeInBytes());

        float* pixels = reinterpret_cast<float*>(image->data());
        auto writeTexel = [&](unsigned index, float x, float y, float z, float w)
        {
            float* out = pixels + static_cast<size_t>(index) * 4u;
            out[0] = x;
            out[1] = y;
            out[2] = z;
            out[3] = w;
        };

        unsigned log2Width = 0u;
        for (unsigned value = atlasWidth; value > 1u; value >>= 1u)
            ++log2Width;

        writeTexel(0u, 1.0f, static_cast<float>(layerCount),
            static_cast<float>(log2Width), static_cast<float>(atlasWidth));
        writeTexel(1u, static_cast<float>(curveRowOffset),
            static_cast<float>(bandRowOffset), static_cast<float>(curveRows),
            static_cast<float>(bandRows));
        writeTexel(2u, backgroundColor.r(), backgroundColor.g(),
            backgroundColor.b(), backgroundColor.a());

        if (!composite.empty())
        {
            std::memcpy(
                pixels + static_cast<size_t>(curveRowOffset) * atlasWidth * 4u,
                curveData.bytes.data(),
                curveData.bytes.size());

            float* targetBand = pixels +
                static_cast<size_t>(bandRowOffset) * atlasWidth * 4u;
            const size_t bandValues =
                bandData.bytes.size() / sizeof(std::uint16_t);
            for (size_t i = 0; i < bandValues; ++i)
            {
                std::uint16_t value;
                std::memcpy(&value, bandData.bytes.data() +
                    i * sizeof(std::uint16_t), sizeof(value));
                targetBand[i] = static_cast<float>(value);
            }

            unsigned record = METADATA_HEADER_TEXELS;
            for (const auto& layer : composite.layers)
            {
                const auto shape = atlas.getShape(layer.key);
                if (!shape)
                    continue;

                writeTexel(record++, shape->bandScaleX, shape->bandScaleY,
                    shape->bandOffsetX, shape->bandOffsetY);
                writeTexel(record++, static_cast<float>(shape->bandTexX),
                    static_cast<float>(shape->bandTexY),
                    static_cast<float>(shape->bandMaxX),
                    static_cast<float>(shape->bandMaxY));
                writeTexel(record++, layer.transform.x, layer.transform.y,
                    layer.scale, 0.0f);
                writeTexel(record++, layer.color.r, layer.color.g,
                    layer.color.b, layer.color.a);
            }
        }

        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image.get());
        texture->setInternalFormat(GL_RGBA32F);
        texture->setSourceFormat(GL_RGBA);
        texture->setSourceType(GL_FLOAT);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        texture->setResizeNonPowerOfTwoHint(false);
        texture->setUnRefImageDataAfterApply(false);

        // Keep the normal texture matrix so terrain parent-tile fallback can pass its
        // scale/bias through to the Slug hook.
        osg::Matrixf matrix;
        matrix.makeIdentity();

        return TextureWindow(texture.release(), matrix);
    }
};

FeatureImageLayerSlug::FeatureImageLayerSlug(
    unsigned tileSize,
    const GeoExtent& extent,
    const Color& backgroundColor,
    unsigned atlasTextureWidth) :
    _impl(new Impl(tileSize, extent, backgroundColor, atlasTextureWidth))
{
}

FeatureImageLayerSlug::~FeatureImageLayerSlug() = default;

void
FeatureImageLayerSlug::render(
    const FeatureList& features,
    const Style& style,
    FilterContext& context)
{
    _impl->render(features, style, context);
}

TextureWindow
FeatureImageLayerSlug::finalize()
{
    return _impl->finalize();
}
