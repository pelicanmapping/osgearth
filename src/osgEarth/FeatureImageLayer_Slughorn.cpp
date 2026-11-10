// osgEarth - Copyright 2026 Pelican Mapping - MIT License
// This translation unit alone uses C++20. No Slughorn types enter the public API.
#include <osgEarth/FeatureImageLayer>
#include <osgEarth/FeatureStyleSorter>
#include <osgEarth/Session>
#include <osgEarth/Progress>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/PolygonSymbol>
#include <osgEarth/LineSymbol>
#include <osgEarth/PointSymbol>
#include <osgEarth/StyleSelector>
#include <slughorn/canvas.hpp>
#include "FeatureImageLayerMaterials"
#include <cmath>
#include <cstring>
#include <stdexcept>

using namespace osgEarth;

namespace
{
    constexpr unsigned atlasWidth = 1024u;
    constexpr unsigned gridSize = 64u;
    constexpr unsigned coarseGridSize = 16u;
    constexpr float atlasMagic = -73821.0f;
    using slughorn::Atlas;
    using slughorn::canvas::Canvas;
    using slughorn::canvas::Path;

    // Creates an immutable nearest-filtered data texture; CPU data survives context recreation.
    osg::ref_ptr<osg::Texture2D> dataTexture(osg::Image* image)
    {
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
        // TextureArena's bindless uploader otherwise defaults RGBA images to RGBA8.
        if (image->getDataType() == GL_FLOAT && image->getPixelFormat() == GL_RGBA)
            texture->setInternalFormat(GL_RGBA32F_ARB);
        texture->setResizeNonPowerOfTwoHint(false);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        texture->setUnRefImageDataAfterApply(false);
        return texture;
    }

    // Converts a style color without changing its alpha or color space.
    slughorn::Color slugColor(const Color& c)
    {
        return { c.r(), c.g(), c.b(), c.a() };
    }

    // Converts map distances to nominal tile pixels, matching FeatureRasterizer's width policy.
    double pixelWidth(const Distance& width, const GeoExtent& extent, unsigned tileSize)
    {
        if (width.getUnits() == Units::PIXELS)
            return width.getValue();
        double south = extent.getSRS()->transformDistance(width, extent.getSRS()->getUnits(), extent.yMin());
        double north = extent.getSRS()->transformDistance(width, extent.getSRS()->getUnits(), extent.yMax());
        return std::min(south, north) * double(tileSize) / extent.height();
    }

    // Adds a contour in tile UV space, subtracting the double-precision origin before narrowing.
    // Exterior/hole winding is normalized for Slug's nonzero fill rule. Input geometry is never changed.
    void contour(Path& path, const Geometry& geom, const GeoExtent& extent, bool closed, bool hole = false)
    {
        if (geom.size() < (closed ? 3u : 2u))
            return;
        bool reverse = closed && ((geom.getOrientation() == Geometry::ORIENTATION_CCW) == hole);
        for (std::size_t i = 0; i < geom.size(); ++i)
        {
            const auto& p = geom[reverse ? geom.size() - 1u - i : i];
            float x = float((p.x() - extent.xMin()) / extent.width());
            float y = float((p.y() - extent.yMin()) / extent.height());
            if (!std::isfinite(x) || !std::isfinite(y))
                throw std::runtime_error("non-finite transformed geometry");
            if (i == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        if (closed) path.closePath();
    }

    // Reads one RG16UI SDK band texel without imposing alignment on its byte storage.
    // Malformed addresses throw and send this tile through the existing raster fallback.
    osg::Vec2ui readBand(const Atlas::TextureData& bands, std::size_t address)
    {
        if (address >= bands.bytes.size() / (2u * sizeof(std::uint16_t)))
            throw std::runtime_error("invalid Slughorn band address");
        std::uint16_t pair[2];
        std::memcpy(pair, bands.bytes.data() + address * sizeof(pair), sizeof(pair));
        return osg::Vec2ui(pair[0], pair[1]);
    }

    // Replaces SDK indirection with direct headers and packs four linear curve addresses per texel.
    // Curve ordering and all curve coordinates are preserved exactly; addresses stay below 2^24.
    unsigned appendBands(std::vector<osg::Vec4f>& data, const Atlas::TextureData& bands,
        unsigned glyph, unsigned horizontalCount, unsigned verticalCount, unsigned curveBase, unsigned curveCount)
    {
        const unsigned indirectionSize = 32u;
        unsigned base = unsigned(data.size());
        data.resize(data.size() + indirectionSize);
        std::vector<osg::Vec2f> headers(horizontalCount + verticalCount);
        for (unsigned i = 0; i < headers.size(); ++i)
        {
            auto source = readBand(bands, glyph + 2u * indirectionSize + i);
            unsigned listBase = unsigned(data.size());
            data.resize(data.size() + (source.x() + 3u) / 4u);
            for (unsigned j = 0; j < source.x(); ++j)
            {
                auto curve = readBand(bands, glyph + source.y() + j);
                unsigned index = curve.y() * atlasWidth + curve.x();
                if (index + 1u >= curveCount)
                    throw std::runtime_error("invalid Slughorn curve address");
                data[listBase + j / 4u][j % 4u] = float(curveBase + index);
            }
            headers[i] = osg::Vec2f(float(source.x()), float(listBase));
        }
        for (unsigned q = 0; q < indirectionSize; ++q)
        {
            unsigned h = readBand(bands, glyph + q).x();
            unsigned v = readBand(bands, glyph + indirectionSize + q).x();
            if (h >= horizontalCount || v >= verticalCount)
                throw std::runtime_error("invalid Slughorn band index");
            const auto& horizontal = headers[h];
            const auto& vertical = headers[horizontalCount + v];
            data[base + q] = osg::Vec4f(horizontal.x(), horizontal.y(), vertical.x(), vertical.y());
        }
        return base;
    }

    // Tile-local builder, used by one paging request only. It owns all SDK state until publication.
    struct VectorTile
    {
        Atlas atlas{ atlasWidth };
        Canvas canvas{ atlas, slughorn::KeyIterator{ "osgearth" } };
        // A paint operation is either an SDK curve shape or a round stroke over shared centerline segments.
        struct Draw : slughorn::Layer
        {
            unsigned first = 0u, count = 0u;
            float radius = 0.0f;
            osg::Vec4f bounds;
            osg::Vec4f widths;
            unsigned material = 0u;
            float period = 1.0f;

            // Creates an unpopulated centerline record; zero segments means there is nothing to draw.
            Draw() = default;

            // Retains SDK shape metadata for fills and strokes requiring curve expansion.
            Draw(const slughorn::Layer& layer) : slughorn::Layer(layer) { }
        };
        std::vector<Draw> layers;
        std::vector<osg::Vec4f> segments;
        GeoExtent extent;
        unsigned tileSize;
        GeoExtent sourceExtent;
        bool mercator = false;
        double gridHeight = 1.0, pixelScale = 1.0;
        osg::Vec4f projection, projectionExtra;
        static constexpr double earthRadius = 6378137.0;
        static constexpr double mercatorLimit = 85.0511287798066;

        FeatureImageLayerMaterials* materials = nullptr;

        // Establishes the tile-local working plane and source pixel scale before choosing per-shape SDK bands.
        // Tiles beyond the Mercator domain throw to the raster reprojection fallback.
        VectorTile(const TileKey& key, const FeatureProfile* features, unsigned size) :
            extent(key.getExtent()), tileSize(size), sourceExtent(key.getExtent())
        {
            const auto mercatorSRS = SpatialReference::get("spherical-mercator");
            mercator = sourceExtent.getSRS()->isGeographic() && features &&
                features->getSRS()->isHorizEquivalentTo(mercatorSRS);
            if (mercator)
            {
                // Mercator has no polar continuation. Let the existing raster reprojection handle those tiles.
                if (sourceExtent.yMin() <= -mercatorLimit || sourceExtent.yMax() >= mercatorLimit)
                    throw std::runtime_error("Mercator vector tile exceeds projection domain");
                const double phi = osg::DegreesToRadians(sourceExtent.yMin());
                const double latitudeSpan = osg::DegreesToRadians(sourceExtent.height());
                const double longitudeSpan = osg::DegreesToRadians(sourceExtent.width());
                const double width = earthRadius * longitudeSpan;
                const double x = earthRadius * osg::DegreesToRadians(sourceExtent.xMin());
                const double y = earthRadius * std::asinh(std::tan(phi));
                const double north = earthRadius * std::asinh(std::tan(phi + latitudeSpan));
                gridHeight = (north - y) / width;
                // A square working extent gives x and y the SAME projected unit. The grid uses the real height.
                extent = GeoExtent(mercatorSRS, x, y, x + width, y + width);
                projection.set(float(std::sin(phi)), float(std::cos(phi)), float(latitudeSpan), float(1.0 / longitudeSpan));
                projectionExtra.set(float(1.0 / gridHeight), float(phi), 0.0f, 0.0f);
                // Pixel styles refer to the source profile's tile resolution, not the geographic terrain tile.
                if (const auto profile = features->getTilingProfile())
                {
                    unsigned lod = profile->getEquivalentLOD(key.getProfile(), key.getLOD());
                    if (features->getMaxLevel() >= 0) lod = std::min(lod, unsigned(features->getMaxLevel()));
                    double w, h;
                    profile->getTileDimensions(lod, w, h);
                    pixelScale = w / width;
                }
            }
            atlas.setCurveTextureFormat(Atlas::TextureData::Format::RGBA32F);
            canvas.setTolerance(slughorn::TOLERANCE_FINE);
            canvas.setSplitStrategy([](const Atlas::Curves& curves)
            {
                // Bound per-fragment curve searches without exceeding the SDK indirection grid.
                int count = int(std::min<std::size_t>(32u, std::max<std::size_t>(1u, curves.size() / 2u)));
                return Atlas::computeUniformSplits(curves, count, count);
            });
        }

        // Records a committed shape in paint order. The pinned SDK checks actual 16-bit band capacity in build().
        void append(const Draw& layer)
        {
            if (layer.count == 0u)
            {
                auto shape = atlas.getShape(layer.key);
                if (!shape || shape->curves.empty()) return;
            }
            else if (layer.radius <= 0.0f)
                return;
            if (layers.size() >= 65535u)
                throw std::runtime_error("tile exceeds Slug shape capacity");
            layers.push_back(layer);
        }

        // Packs one path once for both outline and core; repeated vertices produce no zero-length segments.
        // Coordinates are rebased in double precision before narrowing. Invalid input requests raster fallback.
        Draw centerline(const Geometry& geometry)
        {
            Draw draw;
            if (geometry.size() < (geometry.isRing() ? 3u : 2u)) return draw;
            draw.first = unsigned(segments.size());
            osg::Vec2f first, previous;
            for (std::size_t i = 0; i < geometry.size(); ++i)
            {
                const auto& vertex = geometry[i];
                osg::Vec2f point(float((vertex.x() - extent.xMin()) / extent.width()),
                    float((vertex.y() - extent.yMin()) / extent.height()));
                if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
                    throw std::runtime_error("non-finite centerline geometry");
                if (i == 0u)
                {
                    first = point;
                    draw.bounds = osg::Vec4f(point.x(), point.y(), point.x(), point.y());
                }
                else if (point != previous)
                    segments.emplace_back(previous.x(), previous.y(), point.x(), point.y());
                draw.bounds.x() = std::min(draw.bounds.x(), point.x());
                draw.bounds.y() = std::min(draw.bounds.y(), point.y());
                draw.bounds.z() = std::max(draw.bounds.z(), point.x());
                draw.bounds.w() = std::max(draw.bounds.w(), point.y());
                previous = point;
            }
            if (geometry.isRing() && previous != first)
                segments.emplace_back(previous.x(), previous.y(), first.x(), first.y());
            draw.count = unsigned(segments.size()) - draw.first;
            return draw;
        }

        // Converts a radius contribution to (minimum projected width, ground-meter width), in atlas units.
        // Ground radii receive latitude-dependent Mercator scale in the shader; angular units request raster fallback.
        osg::Vec2f widthComponents(const Distance& width, double minimum, double factor) const
        {
            if (width.getUnits() == Units::PIXELS)
                return osg::Vec2f(float(width.getValue() * pixelScale * factor / tileSize), 0.0f);
            if (!width.getUnits().isLinear()) throw std::runtime_error("angular Mercator stroke width is unsupported");
            return osg::Vec2f(float(minimum * pixelScale * factor / tileSize),
                float(width.as(Units::METERS) * factor / extent.width()));
        }

        // Builds a round paint with a conservative bound for all supported Mercator latitudes.
        // The fixed bound is only for lookup; actual ground width uses each fragment's latitude.
        Draw mercatorStroke(Draw draw, const osg::Vec4f& widths, const Color& color) const
        {
            const float maxScale = float(std::cosh(osg::PI));
            draw.widths = widths;
            draw.radius = std::max(widths.x(), widths.y() * maxScale) + std::max(widths.z(), widths.w() * maxScale);
            if (!std::isfinite(draw.radius)) throw std::runtime_error("non-finite Mercator stroke width");
            draw.color = slugColor(color);
            draw.bounds += osg::Vec4f(-draw.radius, -draw.radius, draw.radius, draw.radius);
            return draw;
        }

        // Creates a separate paint operation over the same segments, expanding its bounds by the stroke radius.
        Draw roundStroke(Draw draw, double width, const Color& color) const
        {
            draw.radius = float(width * 0.5 / tileSize);
            if (!std::isfinite(draw.radius)) throw std::runtime_error("non-finite stroke width");
            draw.color = slugColor(color);
            draw.bounds += osg::Vec4f(-draw.radius, -draw.radius, draw.radius, draw.radius);
            for (unsigned i = 0; i < 4u; ++i)
                if (!std::isfinite(draw.bounds[i])) throw std::runtime_error("non-finite stroke bounds");
            return draw;
        }

        // Adds the supported 2D style passes in rasterizer order; unsupported symbols request raster fallback.
        void render(const Style& style, const FeatureList& features, const FilterContext& context)
        {
            auto poly = style.get<PolygonSymbol>();
            auto line = style.get<LineSymbol>();
            auto point = style.get<PointSymbol>();
            for (const auto& symbol : style.symbols())
            {
                // FeatureStyleSorter has already evaluated selectors; they do not draw anything.
                if (dynamic_cast<const SelectorSymbol*>(symbol.get())) continue;
                if (symbol.get() != poly && symbol.get() != line && symbol.get() != point)
                    throw std::runtime_error(std::string("unsupported vector symbol: ") + symbol->className());
            }
            if (poly && poly->material().isSet())
                throw std::runtime_error("material fills require raster rendering");
            if (line && line->stroke()->stipplePattern().get() != 0xffffu)
                throw std::runtime_error("stippled strokes require raster rendering");

            FeatureList local;
            for (const auto& feature : features)
            {
                if (!feature->getGeometry()) continue;
                osg::ref_ptr<Feature> copy = new Feature(*feature);
                // Buffered Web Mercator tiles can extend past the date line. Clip in the source CRS before
                // projection wraps those vertices to the opposite longitude and folds polygon rings.
                if (copy->getSRS() && sourceExtent.getSRS()->isGeographic() &&
                    copy->getSRS()->isHorizEquivalentTo(SpatialReference::get("spherical-mercator")))
                {
                    Bounds world;
                    const Bounds bounds = copy->getGeometry()->getBounds();
                    if (copy->getSRS()->getBounds(world) &&
                        (bounds.xMin() < world.xMin() || bounds.xMax() > world.xMax()))
                    {
                        // Latitude does not wrap; preserve the tile's north/south buffer.
                        world.yMin() = std::min(world.yMin(), bounds.yMin());
                        world.yMax() = std::max(world.yMax(), bounds.yMax());
#ifdef OSGEARTH_HAVE_GEOS
                        osg::ref_ptr<Geometry> cropped = copy->getGeometry()->crop(world);
                        if (!cropped.valid())
                            throw std::runtime_error("cannot clip buffered Mercator geometry");
                        copy->setGeometry(cropped.get());
#else
                        throw std::runtime_error("Mercator date-line clipping requires GEOS");
#endif
                    }
                }
                copy->transform(extent.getSRS());
                if (!copy->getSRS()->isHorizEquivalentTo(extent.getSRS()))
                    throw std::runtime_error("feature reprojection failed");
                local.push_back(copy);
            }

            // Complete all fills before strokes, as FeatureRasterizer does for a style bucket.
            if (poly)
            {
                for (const auto& feature : local)
                {
                    ConstGeometryIterator iter(feature->getGeometry(), false);
                    while (iter.hasMore())
                    {
                        const Geometry* g = iter.next();
                        if (!g->isRing() || g->size() < 3u) continue;
                        Path path;
                        contour(path, *g, extent, true);
                        auto polygon = dynamic_cast<const Polygon*>(g);
                        if (polygon)
                            for (const auto& hole : polygon->getHoles())
                                contour(path, *hole, extent, true, true);
                        append(canvas.fill(path, slugColor(poly->fill()->color())));
                    }
                }
            }
            if (line)
            {
                const Stroke& stroke = line->stroke().get();
                const float period = line->materialPeriod().get();
                if (!std::isfinite(period) || period <= 0.0f)
                    throw std::runtime_error("stroke-material-period must be finite and positive");
                if (!materials && (!line->material()->empty() || !line->outlineMaterial()->empty()))
                    throw std::runtime_error("stroke material requires a FIL materials palette");
                const unsigned coreSlot = materials ? materials->slot(line->material().get()) : 0u;
                const unsigned outlineSlot = materials ? materials->slot(line->outlineMaterial().get()) : 0u;
                // Attach application-owned paint metadata without modifying SDK geometry or band data.
                auto paint = [&](Draw draw, unsigned slot) { draw.material = slot; draw.period = period; return draw; };
                // Defer core layers so every outline in this style stays below every road interior.
                std::vector<Draw> strokes;
                if (stroke.outlineWidth().isSet()) strokes.reserve(local.size());
                auto cap = stroke.lineCap() == Stroke::LINECAP_FLAT ? slughorn::canvas::LineCap::Butt :
                    stroke.lineCap() == Stroke::LINECAP_SQUARE ? slughorn::canvas::LineCap::Square :
                    slughorn::canvas::LineCap::Round;
                auto join = stroke.lineJoin() == Stroke::LINEJOIN_MITRE ? slughorn::canvas::LineJoin::Miter :
                    slughorn::canvas::LineJoin::Round;
                for (const auto& feature : local)
                {
                    Distance w = stroke.width()->eval(feature.get(), context);
                    Distance ow(0.0, Units::PIXELS);
                    double width = mercator ? w.getValue() * pixelScale : pixelWidth(w, extent, tileSize);
                    if (w.getUnits() != Units::PIXELS)
                        width = std::max(width, double(stroke.minPixels().get()));
                    double outline = 0.0;
                    if (stroke.outlineWidth().isSet())
                    {
                        ow = stroke.outlineWidth()->eval(feature.get(), context);
                        outline = mercator ? ow.getValue() * pixelScale : pixelWidth(ow, extent, tileSize);
                        if (ow.getUnits() != Units::PIXELS)
                            outline = std::max(outline, double(stroke.minPixels().get()));
                    }
                    ConstGeometryIterator iter(feature->getGeometry(), true);
                    while (iter.hasMore())
                    {
                        const Geometry* g = iter.next();
                        if (g->isPointSet() || g->size() < 2u || (g->isRing() && poly && !poly->outline().get()))
                            continue;
                        // Round joins and caps are the union of segment capsules. Other styles retain SDK curves.
                        if (stroke.lineJoin() == Stroke::LINEJOIN_ROUND &&
                            (g->isRing() || stroke.lineCap() == Stroke::LINECAP_ROUND))
                        {
                            if (width <= 0.0 && outline <= 0.0) continue;
                            Draw path = centerline(*g);
                            if (path.count == 0u) continue;
                            osg::Vec4f radii;
                            if (mercator)
                            {
                                auto a = widthComponents(w, stroke.minPixels().get(), 0.5);
                                auto b = widthComponents(ow, stroke.minPixels().get(), 1.0);
                                radii.set(a.x(), a.y(), b.x(), b.y());
                            }
                            if (outline > 0.0)
                                append(paint(mercator ? mercatorStroke(path, radii, stroke.outlineColor().get()) :
                                    roundStroke(path, width + 2.0 * outline, stroke.outlineColor().get()), outlineSlot));
                            if (width > 0.0)
                            {
                                Draw core = paint(mercator ?
                                    mercatorStroke(path, osg::Vec4f(radii.x(), radii.y(), 0, 0), stroke.color()) :
                                    roundStroke(path, width, stroke.color()), coreSlot);
                                if (stroke.outlineWidth().isSet()) strokes.push_back(core);
                                else append(core);
                            }
                            continue;
                        }
                        // Variable ground width cannot be baked into a constant-width outline shape.
                        if (mercator && (w.getUnits() != Units::PIXELS || ow.getUnits() != Units::PIXELS))
                            throw std::runtime_error("ground-width Mercator cap/join requires raster rendering");
                        Path path;
                        contour(path, *g, extent, g->isRing());
                        // Expand the underlying stroke by the border thickness on both sides.
                        if (outline > 0.0)
                            append(paint(canvas.stroke(path, float((width + 2.0 * outline) / tileSize),
                                slugColor(stroke.outlineColor().get()), 1.0f, {}, join, cap), outlineSlot));
                        if (width > 0.0)
                        {
                            Draw core = paint(canvas.stroke(path, float(width / tileSize), slugColor(stroke.color()),
                                1.0f, {}, join, cap), coreSlot);
                            if (stroke.outlineWidth().isSet()) strokes.push_back(core);
                            else append(core);
                        }
                    }
                }
                for (const auto& core : strokes) append(core);
            }
            if (point)
            {
                for (const auto& feature : local)
                {
                    // FeatureRasterizer paints a circular point at every geometry vertex, including rings.
                    ConstGeometryIterator iter(feature->getGeometry(), true);
                    while (iter.hasMore())
                    {
                        const Geometry* g = iter.next();
                        for (const auto& p : *g)
                        {
                            Path path;
                            float x = float((p.x() - extent.xMin()) / extent.width());
                            float y = float((p.y() - extent.yMin()) / extent.height());
                            float radius = float(point->size().get() * pixelScale * 0.5 / tileSize);
                            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius))
                                throw std::runtime_error("non-finite point geometry");
                            if (radius <= 0.0f) continue;
                            path.circle(x, y, radius);
                            append(canvas.fill(path, slugColor(point->fill()->color())));
                        }
                    }
                }
            }
        }

        // Indexes paint operations and long-path segments in tile UVs, preserving path and segment order.
        // A full-cell AA margin is conservative while the shader's pixel footprint fits this grid.
        // Short paths keep direct scans; segment geometry is shared, and references are packed four per texel.
        unsigned appendGrid(std::vector<osg::Vec4f>& data, const std::vector<osg::Vec4f>& bounds,
            unsigned size, unsigned segmentBase) const
        {
            struct Entry
            {
                unsigned address;
                std::vector<unsigned> references;
            };
            std::vector<std::vector<Entry>> grid(size * size);
            // Visits cells intersecting an expanded bound; clamps before integer conversion for remote geometry.
            auto visit = [&](osg::Vec4f b, unsigned address, unsigned segment, bool indexed)
            {
                b.y() /= float(gridHeight);
                b.w() /= float(gridHeight);
                if (b.z() < -1.0f || b.w() < -1.0f || b.x() > 2.0f || b.y() > 2.0f) return;
                int x0 = std::max(0, int(std::floor(std::max(-1.0f, b.x()) * size)) - 1);
                int y0 = std::max(0, int(std::floor(std::max(-1.0f, b.y()) * size)) - 1);
                int x1 = std::min(int(size) - 1, int(std::floor(std::min(2.0f, b.z()) * size)) + 1);
                int y1 = std::min(int(size) - 1, int(std::floor(std::min(2.0f, b.w()) * size)) + 1);
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                    {
                        auto& cell = grid[y * size + x];
                        if (cell.empty() || cell.back().address != address)
                            cell.push_back({ address, {} });
                        if (indexed) cell.back().references.push_back(segment);
                    }
            };
            for (unsigned i = 0; i < layers.size(); ++i)
            {
                const auto& layer = layers[i];
                unsigned address = 3u + i * 4u;
                if (layer.count <= 4u)
                {
                    visit(bounds[i], address, 0u, false);
                    continue;
                }
                for (unsigned j = 0; j < layer.count; ++j)
                {
                    const auto& segment = segments[layer.first + j];
                    osg::Vec4f b(std::min(segment.x(), segment.z()) - layer.radius,
                        std::min(segment.y(), segment.w()) - layer.radius,
                        std::max(segment.x(), segment.z()) + layer.radius,
                        std::max(segment.y(), segment.w()) + layer.radius);
                    visit(b, address, segmentBase + layer.first + j, true);
                }
            }
            unsigned base = unsigned(data.size());
            data.resize(data.size() + grid.size());
            for (unsigned i = 0; i < grid.size(); ++i)
            {
                const auto& cell = grid[i];
                unsigned entries = unsigned(data.size());
                data[base + i] = osg::Vec4f(float(entries), float(cell.size()), 0, 0);
                data.resize(data.size() + cell.size());
                for (unsigned j = 0; j < cell.size(); ++j)
                {
                    const auto& entry = cell[j];
                    unsigned first = unsigned(data.size());
                    data[entries + j] = osg::Vec4f(float(entry.address), float(first),
                        float(entry.references.size()), 0);
                    data.resize(data.size() + (entry.references.size() + 3u) / 4u);
                    for (unsigned k = 0; k < entry.references.size(); ++k)
                        data[first + k / 4u][k % 4u] = float(entry.references[k]);
                }
            }
            return base;
        }

        // Packs ordered curve/stroke records, spatial lists, shared centerlines, curves, and bands into one texture.
        // Integer addresses remain exact below 2^24; dimensions and address counts are checked before upload.
        osg::ref_ptr<osg::Texture2D> finish(const Color& background)
        {
            atlas.build();
            const auto& curves = atlas.getCurveTextureData();
            const auto& bands = atlas.getBandTextureData();
            if (curves.format != Atlas::TextureData::Format::RGBA32F ||
                bands.format != Atlas::TextureData::Format::RG16UI ||
                curves.width != atlasWidth || bands.width != atlasWidth)
                throw std::runtime_error("unexpected Slughorn texture format");
            std::vector<osg::Vec4f> data(3u + layers.size() * 4u);
            std::vector<osg::Vec4f> bounds(layers.size());
            for (unsigned i = 0; i < layers.size(); ++i)
            {
                const auto& layer = layers[i];
                unsigned address = 3u + i * 4u;
                data[address] = osg::Vec4f(layer.color.r, layer.color.g, layer.color.b, layer.color.a);
                if (layer.count > 0u)
                {
                    // Use the same carrier coordinates as curves so bounds rejection precedes metadata fetches.
                    bounds[i] = layer.bounds;
                    data[address + 1u] = osg::Vec4f(32.0f / std::max(layer.bounds.z() - layer.bounds.x(), 1e-9f),
                        32.0f / std::max(layer.bounds.w() - layer.bounds.y(), 1e-9f), 0, 0);
                    data[address + 3u] = osg::Vec4f(layer.bounds.x(), layer.bounds.y(), float(layer.material), layer.period);
                    continue;
                }
                const auto shape = atlas.getShape(layer.key);
                data[address + 1u] = osg::Vec4f(shape->bandScaleX, shape->bandScaleY,
                    shape->bandOffsetX, shape->bandOffsetY);
                data[address + 3u] = osg::Vec4f(layer.transform.x, layer.transform.y, float(layer.material), layer.period);
                float xmin = shape->bearingX + layer.transform.x;
                float ymin = shape->bearingY - shape->height + layer.transform.y;
                float xmax = xmin + shape->width, ymax = ymin + shape->height;
                // A full grid-cell margin covers analytic AA; very coarse footprints use the full shape list.
                if (!std::isfinite(xmin) || !std::isfinite(ymin) || !std::isfinite(xmax) || !std::isfinite(ymax))
                    throw std::runtime_error("non-finite shape bounds");
                bounds[i] = osg::Vec4f(xmin, ymin, xmax, ymax);
            }
            // Dense city tiles need short candidate lists at high magnification.
            // Retain a coarse grid so minification does not jump straight to every shape.
            unsigned segmentBase = unsigned(data.size());
            data.insert(data.end(), segments.begin(), segments.end());
            unsigned gridBase = appendGrid(data, bounds, gridSize, segmentBase);
            unsigned coarseBase = appendGrid(data, bounds, coarseGridSize, segmentBase);
            std::size_t curveBase = data.size();
            std::size_t curveCount = curves.bytes.size() / sizeof(osg::Vec4f);
            data.resize(data.size() + curveCount);
            if (curveCount) std::memcpy(data.data() + curveBase, curves.bytes.data(), curves.bytes.size());
            for (unsigned i = 0; i < layers.size(); ++i)
            {
                const auto& layer = layers[i];
                if (layer.count > 0u)
                {
                    data[3u + i * 4u + 2u] = osg::Vec4f(float(segmentBase + layer.first),
                        float(layer.count), layer.radius, 1.0f);
                    if (layer.widths.y() > 0.0f || layer.widths.w() > 0.0f)
                    {
                        data[3u + i * 4u + 2u].z() = float(data.size());
                        data[3u + i * 4u + 2u].w() = 2.0f;
                        data.push_back(layer.widths);
                    }
                    continue;
                }
                const auto shape = atlas.getShape(layer.key);
                unsigned base = appendBands(data, bands, shape->bandTexY * atlasWidth + shape->bandTexX,
                    shape->bandMaxY + 1u, shape->bandMaxX + 1u, unsigned(curveBase), unsigned(curveCount));
                data[3u + i * 4u + 2u] = osg::Vec4f(float(base), 0, 0, 0);
            }
            // Application metadata only: the SDK curves and band data above retain their vendor layout.
            // Header.z points to the optional geographic-to-Mercator frame; zero selects legacy tile UVs.
            unsigned projectionBase = 0u;
            if (mercator)
            {
                projectionBase = unsigned(data.size());
                data.push_back(projection);
                data.push_back(projectionExtra);
            }
            std::size_t count = data.size();
            unsigned height = unsigned((count + atlasWidth - 1u) / atlasWidth);
            unsigned maxSize = unsigned(Registry::capabilities().getMaxTextureSize());
            if (count >= (1u << 24u) || height > maxSize || atlasWidth > maxSize)
                throw std::runtime_error("Slug tile exceeds GPU atlas capacity");
            data.resize(std::size_t(height) * atlasWidth);
            data[0] = osg::Vec4f(atlasMagic, float(curveBase), float(projectionBase), float(layers.size()));
            data[1] = osg::Vec4f(background.r(), background.g(), background.b(), background.a());
            data[2] = osg::Vec4f(float(gridBase), float(gridSize), 1.0f, float(coarseBase));
            osg::ref_ptr<osg::Image> image = new osg::Image();
            image->allocateImage(atlasWidth, height, 1, GL_RGBA, GL_FLOAT);
            image->setInternalTextureFormat(GL_RGBA32F_ARB);
            std::memcpy(image->data(), data.data(), data.size() * sizeof(osg::Vec4f));
            return dataTexture(image);
        }
    };
}

// Creates data only at legal source LODs. REX inherits the texture and scale/bias matrix above max data LOD.
// No SDK state is shared between paging threads; failures fall back to the existing raster pipeline.
TextureWindow
FeatureImageLayer::createTexture(const TileKey& key, ProgressCallback* progress) const
{
    if (options().mode() != "vector" || !isOpen() || !isKeyInLegalRange(key) ||
        !mayHaveData(key) || (progress && progress->isCanceled()))
        return {};
    Isolate local(_global);
    if (!local._session || !local._session->getFeatureSource())
        return {};
    try
    {
        VectorTile tile(key, local._session->getFeatureSource()->getFeatureProfile(), getTileSize());
        tile.materials = FeatureImageLayerMaterials::get(this);
        FilterContext context(local._session.get(), key.getExtent());
        FeatureStyleSorter sorter;
        sorter.sort(key, options().bufferWidth().get(), local._session.get(), local._filterChain, nullptr,
            [&](const Style& style, FeatureList& features, ProgressCallback* callback)
            {
                // Cancellation aborts publication, including partially assembled style buckets.
                if (callback && callback->isCanceled()) throw std::runtime_error("canceled");
                tile.render(style, features, context);
            }, progress);
        if (progress && progress->isCanceled()) return {};
        auto texture = tile.finish(options().backgroundColor().get());
        return TextureWindow(texture.get(), osg::Matrix::identity());
    }
    catch (const std::exception& e)
    {
        if (progress && progress->isCanceled()) return {};
        OE_DEBUG << "[FeatureImageLayer] " << getName() << " " << key.str()
            << ": raster fallback: " << e.what() << std::endl;
        // Rasterize in the layer's native profile, then reproject the image. Calling the implementation
        // directly with a map-profile key would repeat the same geometry-wrap problem in the fallback.
        GeoImage image = const_cast<FeatureImageLayer*>(this)->createImage(key, progress);
        if (!image.valid()) return {};
        auto texture = dataTexture(image.takeImage().get());
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        osg::Matrix matrix;
        key.getExtent().createScaleBias(image.getExtent(), matrix);
        return TextureWindow(texture.get(), matrix);
    }
}
