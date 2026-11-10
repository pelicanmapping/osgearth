/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
// Only this bridge uses C++20; the private engine and renderer remain C++17.
#include "LabelTextInternal.h"
#include <osg/Texture>
#define SLUGHORN_FREETYPE_IMPLEMENTATION
#include <slughorn/freetype.hpp>
#include <ft2build.h>
#include FT_MODULE_H
#include <hb.h>
#include <hb-ot.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

using namespace osgEarthPrestige::Labeling;

namespace
{
    //! Copies SDK texture storage into immutable CPU images; GPU allocation remains context-local in OSG.
    osg::ref_ptr<osg::Image> image(const slughorn::Atlas::TextureData& source, bool bands)
    {
        if (source.width == 0 || source.height == 0 || source.width > 512 || source.height > 16384)
            throw std::runtime_error("Slughorn atlas exceeds texture bounds");
        osg::ref_ptr<osg::Image> result = new osg::Image;
        // OSG 3.6 Image cannot size RG_INTEGER images. Expand only the CPU upload format to RGBA16UI;
        // the atlas coordinates and integer address values stay unchanged, and the shader reads only RG.
        result->allocateImage(source.width, source.height, 1, bands ? GL_RGBA_INTEGER_EXT : GL_RGBA,
            bands ? GL_UNSIGNED_SHORT : GL_FLOAT, 1);
        result->setInternalTextureFormat(bands ? GL_RGBA16UI_EXT : GL_RGBA32F_ARB);
        const auto expected = std::size_t(source.width) * source.height * (bands ? 4 : 16);
        if (source.bytes.size() != expected)
            throw std::runtime_error("Unexpected Slughorn texture format");
        if (bands)
        {
            std::memset(result->data(), 0, result->getTotalSizeInBytes());
            for (std::size_t texel = 0; texel < source.bytes.size() / 4; ++texel)
                std::memcpy(result->data() + texel * 8, source.bytes.data() + texel * 4, 4);
        }
        else
            std::memcpy(result->data(), source.bytes.data(), source.bytes.size());
        return result;
    }
}

struct TextService::State
{
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    hb_blob_t* blob = nullptr;
    hb_face_t* hbFace = nullptr;
    hb_font_t* font = nullptr;
    const std::thread::id owner = std::this_thread::get_id();
    std::map<unsigned, Glyph> glyphs;
    std::map<std::string, std::shared_ptr<const TextRun>> runs;
    std::vector<std::shared_ptr<const GlyphPage>> pages;
    std::size_t bytes = 0, runBytes = 0, distanceBytes = 0;

    //! Releases partially initialized font state as well as normal successful construction.
    ~State()
    {
        if (font) hb_font_destroy(font);
        if (hbFace) hb_face_destroy(hbFace);
        if (blob) hb_blob_destroy(blob);
        if (face) FT_Done_Face(face);
        if (library) FT_Done_FreeType(library);
    }

    /** Bakes optional halo distances on the font worker, never on cull/draw. Keeps analytic glyph outlines intact.
     * Uses the bitmap SDF rasterizer to handle overlapping contours safely. Missing module, failed glyphs,
     * oversized tiles, or exhausted cache budget fall back to analytic halos without rejecting text.
     * Fixed 64 texels/em and 16-texel spread match the decoding constants in LabelText.frag.glsl.
     */
    void bakeDistance(GlyphPage& page, const slughorn::Atlas& atlas, const std::vector<unsigned>& ids,
        const std::set<unsigned>& empty)
    {
#if FREETYPE_MAJOR > 2 || (FREETYPE_MAJOR == 2 && FREETYPE_MINOR >= 11)
        constexpr std::size_t budget = 8 * 1024 * 1024;
        constexpr unsigned width = 1024, pixels = 64;
        const auto available = std::min(budget - distanceBytes, 32 * 1024 * 1024 - bytes - page.bytes);
        const auto mapBytes = std::size_t(page.bands->s()) * page.bands->t() * sizeof(osg::Vec4f);
        if (available <= mapBytes + width) return;
        FT_Int spread = 16;
        if (FT_Property_Set(library, "bsdf", "spread", &spread) || FT_Set_Pixel_Sizes(face, 0, pixels)) return;
        struct Tile
        {
            unsigned id, x, y, w, h;
            int left, top;
            std::vector<unsigned char> data;
        };
        std::vector<Tile> tiles;
        unsigned x = 1, y = 1, rowHeight = 0;
        for (auto id : ids)
        {
            if (empty.count(id)) continue;
            if (FT_Load_Glyph(face, id, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) ||
                FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF))
                continue;
            const auto& bitmap = face->glyph->bitmap;
            if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || !bitmap.width || !bitmap.rows ||
                bitmap.width > 512 || bitmap.rows > 512 || bitmap.pitch < int(bitmap.width)) continue;
            if (x + bitmap.width + 1 > width) { y += rowHeight + 2; x = 1; rowHeight = 0; }
            const auto height = y + std::max(rowHeight, bitmap.rows) + 1;
            if (mapBytes + std::size_t(width) * height > available) break;
            Tile tile { id, x, y, bitmap.width, bitmap.rows, face->glyph->bitmap_left, face->glyph->bitmap_top, {} };
            tile.data.resize(std::size_t(tile.w) * tile.h);
            for (unsigned r = 0; r < tile.h; ++r)
            {
                const auto sourceRow = tile.h - 1 - r;
                std::memcpy(tile.data.data() + std::size_t(r) * tile.w,
                    bitmap.buffer + std::size_t(sourceRow) * bitmap.pitch, tile.w);
            }
            tiles.push_back(std::move(tile));
            x += bitmap.width + 2;
            rowHeight = std::max(rowHeight, bitmap.rows);
        }
        if (tiles.empty()) return;
        unsigned height = 1;
        for (const auto& tile : tiles) height = std::max(height, tile.y + tile.h + 1);
        osg::ref_ptr<osg::Image> distance = new osg::Image, mapping = new osg::Image;
        distance->allocateImage(width, height, 1, GL_RED, GL_UNSIGNED_BYTE, 1);
        distance->setInternalTextureFormat(GL_R8);
        mapping->allocateImage(page.bands->s(), page.bands->t(), 1, GL_RGBA, GL_FLOAT, 1);
        mapping->setInternalTextureFormat(GL_RGBA32F_ARB);
        std::memset(distance->data(), 0, distance->getTotalSizeInBytes());
        std::memset(mapping->data(), 0, mapping->getTotalSizeInBytes());
        for (const auto& tile : tiles)
        {
            for (unsigned r = 0; r < tile.h; ++r)
                std::memcpy(distance->data(tile.x, tile.y + r), tile.data.data() + std::size_t(r) * tile.w, tile.w);
            const auto shape = atlas.getShape(slughorn::Key(tile.id));
            auto* uv = reinterpret_cast<float*>(mapping->data(shape->bandTexX, shape->bandTexY));
            uv[0] = float(pixels) / width;
            uv[1] = float(pixels) / height;
            uv[2] = (float(tile.x) - tile.left) / width;
            uv[3] = (float(tile.y) - (tile.top - int(tile.h))) / height;
            // The next map texel is within this glyph's band header. Clamp to its own border texel centers.
            // Quad AA padding can extend past the distance tile, especially for small text with a wide halo.
            auto* bounds = reinterpret_cast<float*>(mapping->data()) +
                (std::size_t(shape->bandTexY) * mapping->s() + shape->bandTexX + 1) * 4;
            bounds[0] = (tile.x + 0.5f) / width;
            bounds[1] = (tile.y + 0.5f) / height;
            bounds[2] = (tile.x + tile.w - 0.5f) / width;
            bounds[3] = (tile.y + tile.h - 0.5f) / height;
        }
        const auto size = distance->getTotalSizeInBytes() + mapping->getTotalSizeInBytes();
        page.distance = std::move(distance);
        page.distanceMap = std::move(mapping);
        page.bytes += size;
        distanceBytes += size;
#endif
    }

    //! Appends a sealed page for at most 128 new glyph IDs; never repacks a previously published page.
    void appendPage(const std::vector<unsigned>& ids)
    {
        if (pages.size() >= 32 || glyphs.size() + ids.size() > 4096)
            throw std::runtime_error("Label glyph cache is full");
        slughorn::Atlas atlas(512);
        const float em = 1.0f / face->units_per_EM;
        std::set<unsigned> empty;
        for (unsigned id : ids)
        {
            if (FT_Load_Glyph(face, id, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP))
                throw std::runtime_error("Cannot load shaped glyph outline");
            slughorn::Atlas::ShapeInfo info;
            if (!slughorn::freetype::decomposeGlyph(face, em, face->glyph->metrics.horiAdvance * em, nullptr, info))
            {
                if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
                    throw std::runtime_error("Only outline glyphs are supported");
                empty.insert(id);
                continue;
            }
            info.autoMetrics = false;
            info.bearingX = face->glyph->metrics.horiBearingX * em;
            info.bearingY = face->glyph->metrics.horiBearingY * em;
            info.width = face->glyph->metrics.width * em;
            info.height = face->glyph->metrics.height * em;
            atlas.addShape(slughorn::Key(id), info);
        }
        std::shared_ptr<GlyphPage> page;
        if (empty.size() != ids.size())
        {
            atlas.build();
            page = std::make_shared<GlyphPage>();
            page->curves = image(atlas.getCurveTextureData(), false);
            page->bands = image(atlas.getBandTextureData(), true);
            page->bytes = page->curves->getTotalSizeInBytes() + page->bands->getTotalSizeInBytes();
            if (page->bytes > 32 * 1024 * 1024 - bytes)
                throw std::runtime_error("Label atlas memory budget exceeded");
            bakeDistance(*page, atlas, ids, empty);
        }
        for (unsigned id : ids)
        {
            Glyph glyph;
            glyph.id = id;
            glyph.page = static_cast<unsigned>(pages.size());
            if (!empty.count(id))
            {
                const auto shape = atlas.getShape(slughorn::Key(id));
                if (!shape) throw std::runtime_error("Slughorn omitted a requested glyph");
                glyph.bounds.set(shape->bearingX, shape->bearingY - shape->height,
                    shape->bearingX + shape->width, shape->bearingY);
                glyph.bandTransform.set(shape->bandScaleX, shape->bandScaleY, shape->bandOffsetX, shape->bandOffsetY);
                glyph.bandAddress.set(shape->bandTexX, shape->bandTexY, shape->bandMaxX, shape->bandMaxY);
            }
            glyphs.emplace(id, glyph);
        }
        if (page)
        {
            bytes += page->bytes;
            pages.push_back(std::move(page));
        }
    }
};

TextService::TextService(const std::string& fontPath) : _state(std::make_unique<State>())
{
    auto& state = *_state;
    if (FT_Init_FreeType(&state.library) || FT_New_Face(state.library, fontPath.c_str(), 0, &state.face) ||
        !FT_IS_SCALABLE(state.face) || state.face->units_per_EM == 0)
        throw std::runtime_error("Cannot open scalable font: " + fontPath);
    state.blob = hb_blob_create_from_file(fontPath.c_str());
    if (hb_blob_get_length(state.blob) == 0)
        throw std::runtime_error("Cannot open HarfBuzz font: " + fontPath);
    state.hbFace = hb_face_create(state.blob, 0);
    state.font = hb_font_create(state.hbFace);
    hb_ot_font_set_funcs(state.font);
    hb_font_set_scale(state.font, state.face->units_per_EM, state.face->units_per_EM);
}

TextService::~TextService() = default;

std::shared_ptr<const TextResources> TextService::prepare(const std::vector<std::string>& text)
{
    auto& state = *_state;
    if (std::this_thread::get_id() != state.owner)
        throw std::logic_error("TextService used outside its preparation thread");
    if (text.size() > 1024)
        throw std::runtime_error("Too many label runs");
    // Moving/restyling labels normally changes no text. Avoid rebuilding eviction sets and scanning the entire
    // cache when every requested run is already present and the soft reclamation threshold is not exceeded.
    if (state.runBytes <= 8 * 1024 * 1024)
    {
        auto cached = std::make_shared<TextResources>();
        cached->runs.reserve(text.size());
        for (const auto& value : text)
        {
            auto found = state.runs.find(value);
            if (found == state.runs.end()) { cached.reset(); break; }
            cached->runs.push_back(found->second);
        }
        if (cached)
        {
            cached->pages = state.pages;
            cached->bytes = state.bytes;
            return cached;
        }
    }
    std::set<std::string> requested(text.begin(), text.end());
    std::size_t newRuns = 0;
    for (const auto& value : requested) if (!state.runs.count(value)) ++newRuns;
    // Evict cache ownership, never published tile ownership. Revisiting an evicted run shapes it again.
    for (auto i = state.runs.begin(); i != state.runs.end() &&
        (state.runs.size() + newRuns > 1024 || state.runBytes > 8 * 1024 * 1024);)
    {
        if (requested.count(i->first)) { ++i; continue; }
        state.runBytes -= std::min(state.runBytes,
            sizeof(TextRun) + sizeof(RunGlyph) * i->second->glyphs.capacity() + i->first.size());
        i = state.runs.erase(i);
    }
    struct Shaped
    {
        std::vector<hb_glyph_info_t> info;
        std::vector<hb_glyph_position_t> positions;
    };
    std::map<std::string, Shaped> pending;
    std::set<unsigned> missing;
    std::size_t reservedBytes = state.runBytes;
    for (const auto& value : text)
    {
        if (state.runs.count(value) || pending.count(value)) continue;
        if (value.size() > 4096 || state.runs.size() + pending.size() >= 1024)
            throw std::runtime_error("Label shaped-run budget exceeded");
        std::unique_ptr<hb_buffer_t, decltype(&hb_buffer_destroy)> buffer(hb_buffer_create(), hb_buffer_destroy);
        hb_buffer_add_utf8(buffer.get(), value.data(), static_cast<int>(value.size()), 0, static_cast<int>(value.size()));
        hb_buffer_guess_segment_properties(buffer.get());
        hb_shape(state.font, buffer.get(), nullptr, 0);
        unsigned count = 0;
        const auto* info = hb_buffer_get_glyph_infos(buffer.get(), &count);
        const auto* positions = hb_buffer_get_glyph_positions(buffer.get(), &count);
        if (count > 4096 || !hb_buffer_allocation_successful(buffer.get()))
            throw std::runtime_error("Cannot shape label within budget");
        const auto runBytes = sizeof(TextRun) + sizeof(RunGlyph) * count + value.size();
        if (runBytes > 16 * 1024 * 1024 - reservedBytes)
            throw std::runtime_error("Label shaped-run memory budget exceeded");
        reservedBytes += runBytes;
        for (unsigned i = 0; i < count; ++i)
        {
            if (info[i].codepoint == 0)
                throw UnsupportedGlyph();
            if (!state.glyphs.count(info[i].codepoint)) missing.insert(info[i].codepoint);
        }
        auto& shaped = pending[value];
        shaped.info.assign(info, info + count);
        shaped.positions.assign(positions, positions + count);
    }
    if (missing.size() > 4096 - state.glyphs.size())
        throw std::runtime_error("Label glyph cache is full");
    while (!missing.empty())
    {
        std::vector<unsigned> page;
        while (!missing.empty() && page.size() < 128)
        {
            page.push_back(*missing.begin());
            missing.erase(missing.begin());
        }
        state.appendPage(page);
    }
    for (const auto& entry : pending)
    {
        const auto& info = entry.second.info;
        const auto& positions = entry.second.positions;
        const auto count = info.size();
        auto run = std::make_shared<TextRun>();
        run->glyphs.reserve(count);
        const float em = 1.0f / state.face->units_per_EM;
        osg::Vec2f pen;
        bool first = true;
        for (unsigned i = 0; i < count; ++i)
        {
            RunGlyph item;
            item.glyph = state.glyphs.at(info[i].codepoint);
            item.cluster = info[i].cluster;
            item.offset = pen + osg::Vec2f(positions[i].x_offset * em, positions[i].y_offset * em);
            const auto& bounds = item.glyph.bounds;
            if (bounds.z() > bounds.x() && bounds.w() > bounds.y())
            {
                const osg::Vec2f lower = item.offset + osg::Vec2f(bounds.x(), bounds.y());
                const osg::Vec2f upper = item.offset + osg::Vec2f(bounds.z(), bounds.w());
                for (unsigned axis = 0; axis < 2; ++axis)
                {
                    run->lowerLeft[axis] = first ? lower[axis] : std::min(run->lowerLeft[axis], lower[axis]);
                    run->upperRight[axis] = first ? upper[axis] : std::max(run->upperRight[axis], upper[axis]);
                }
                first = false;
                run->glyphs.push_back(item);
            }
            pen += osg::Vec2f(positions[i].x_advance * em, positions[i].y_advance * em);
        }
        run->advance = pen.x();
        state.runs.emplace(entry.first, std::move(run));
    }
    state.runBytes = 0;
    for (const auto& run : state.runs)
        state.runBytes += sizeof(TextRun) + sizeof(RunGlyph) * run.second->glyphs.capacity() + run.first.size();
    auto output = std::make_shared<TextResources>();
    for (const auto& value : text) output->runs.push_back(state.runs.at(value));
    output->pages = state.pages;
    output->bytes = state.bytes;
    return output;
}
