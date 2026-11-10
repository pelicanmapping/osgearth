/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#include "LabelRendererInternal.h"
#include "LabelTilesInternal.h"
#include "LabelDynamicInternal.h"
#include "LabelSceneInternal.h"
#include "LabelTextShaders.h"
#include <osg/BindImageTexture>
#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/Program>
#include <osg/Texture2D>
#include <osg/GLExtensions>
#include <osgUtil/CullVisitor>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

using namespace osgEarthPrestige::Labeling;

namespace
{
    class GlyphGeometry : public osg::Geometry
    {
    public:
        //! Creates CPU geometry whose own VAO receives divisors at draw time, after OSG binds it.
        GlyphGeometry()
        {
            setUseDisplayList(false);
            setUseVertexBufferObjects(true);
            setUseVertexArrayObject(true);
        }

        //! Shares immutable arrays while giving each in-flight frame its own drawable/VAO and uniforms.
        explicit GlyphGeometry(const osg::Geometry& source) : osg::Geometry(source, osg::CopyOp::SHALLOW_COPY) { }

        //! Applies VAO-local divisors after binding; resets them so OSG's general state cache remains consistent.
        void drawImplementation(osg::RenderInfo& info) const override
        {
            auto* gl = info.getState()->get<osg::GLExtensions>();
            if (!gl->glVertexAttribDivisor) return; // OpenGL 3.3 instanced arrays are required by this renderer.
            gl->glVertexAttribDivisor(5, 1);
            gl->glVertexAttribDivisor(4, 1);
            gl->glVertexAttribDivisor(1, 1);
            for (unsigned i = 6; i <= 15; ++i) gl->glVertexAttribDivisor(i, 1);
            osg::Geometry::drawImplementation(info);
            gl->glVertexAttribDivisor(5, 0);
            gl->glVertexAttribDivisor(4, 0);
            gl->glVertexAttribDivisor(1, 0);
            for (unsigned i = 6; i <= 15; ++i) gl->glVertexAttribDivisor(i, 0);
        }
    };

    class PathComputeGeometry : public osg::Geometry
    {
        unsigned _roads;
    public:
        //! Creates a draw-ordered compute leaf; GL objects remain managed by the frame's OSG state.
        explicit PathComputeGeometry(unsigned roads) : _roads(roads)
        { setUseDisplayList(false); setUseVertexArrayObject(false); }
        //! Gives each in-flight frame its own camera uniforms while sharing immutable path inputs and context-local output.
        PathComputeGeometry(const PathComputeGeometry& source) :
            osg::Geometry(source, osg::CopyOp::SHALLOW_COPY), _roads(source._roads) { }
        //! Orders image writes after prior consumers and makes this frame's glyph positions visible to texture fetches.
        void drawImplementation(osg::RenderInfo& info) const override
        {
            auto* gl = info.getState()->get<osg::GLExtensions>();
            if (!gl->glDispatchCompute || !gl->glMemoryBarrier) return;
            gl->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            gl->glDispatchCompute(_roads, 1, 1);
            gl->glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
        }
    };

    struct DrawPacket
    {
        std::vector<osg::ref_ptr<osg::Geometry>> meshes;
        std::vector<osg::ref_ptr<osg::StateSet>> states;
        std::vector<std::pair<BatchPtr, std::size_t>> records;
        std::vector<Fade> fades;
        std::vector<std::shared_ptr<const PathPlacement>> paths;
        std::vector<osg::Vec2d> offsets;
        double timeOrigin = 0.0;
        std::size_t glyphs = 0;
    };

    struct DrawData : ResultPayload
    {
        std::vector<std::pair<BatchPtr, std::size_t>> records;
        std::vector<Fade> fades;
        std::vector<std::shared_ptr<const PathPlacement>> paths;
        std::vector<osg::Vec2d> offsets;
        std::vector<std::shared_ptr<const DrawPacket>> packets;
        std::vector<osg::ref_ptr<osg::Geometry>> meshes;
        std::vector<osg::ref_ptr<osg::StateSet>> states;
        // Reused packets retain their own epoch; each frame rebases time/adoption uniforms to that epoch.
        std::vector<double> timeOrigins;
    };

    //! Creates context-managed atlas storage; only cached distances use linear filtering. CPU images support recreation.
    osg::ref_ptr<osg::Texture2D> texture(osg::Image* image, bool linear = false)
    {
        osg::ref_ptr<osg::Texture2D> result = new osg::Texture2D(image);
        result->setInternalFormat(image->getInternalTextureFormat());
        result->setFilter(osg::Texture::MIN_FILTER, linear ? osg::Texture::LINEAR : osg::Texture::NEAREST);
        result->setFilter(osg::Texture::MAG_FILTER, linear ? osg::Texture::LINEAR : osg::Texture::NEAREST);
        result->setResizeNonPowerOfTwoHint(false);
        result->setUnRefImageDataAfterApply(false);
        return result;
    }

    //! Packs a bounded floating-point GPU table; zeros initialize unused texels and support GL context recreation.
    osg::ref_ptr<osg::Texture2D> pathTable(const std::vector<osg::Vec4f>& values)
    {
        osg::ref_ptr<osg::Image> pixels = new osg::Image;
        pixels->allocateImage(512, std::max<std::size_t>(1, (values.size() + 511) / 512), 1, GL_RGBA, GL_FLOAT);
        pixels->setInternalTextureFormat(GL_RGBA32F_ARB);
        auto* output = reinterpret_cast<osg::Vec4f*>(pixels->data());
        std::fill(output, output + pixels->s() * pixels->t(), osg::Vec4f());
        std::copy(values.begin(), values.end(), output);
        return texture(pixels);
    }

    //! GPU path geometry depends on immutable source paths and fades, not the worker's current fitted glyph anchors.
    bool samePaths(const std::vector<std::pair<BatchPtr, std::size_t>>& records,
        const std::vector<std::shared_ptr<const PathPlacement>>& a,
        const std::vector<std::shared_ptr<const PathPlacement>>& b)
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            const auto& path = records[i].first->records()[records[i].second].path;
            if ((!path || !path->gpu) && a[i] != b[i]) return false;
        }
        return true;
    }

    //! Splits a world coordinate into floats before GPU subtraction, preserving precision near the camera.
    void split(const osg::Vec3d& value, osg::Vec3f& high, osg::Vec3f& low)
    {
        high = osg::Vec3f(value);
        low = osg::Vec3f(value - osg::Vec3d(high));
    }

    struct InstanceArrays
    {
        osg::ref_ptr<osg::Vec3Array> high = new osg::Vec3Array, low = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec3Array> tangents = new osg::Vec3Array;
        osg::ref_ptr<osg::FloatArray> pathIndices = new osg::FloatArray;
        osg::ref_ptr<osg::FloatArray> softness = new osg::FloatArray;
        std::array<osg::ref_ptr<osg::Vec4Array>, 7> values;
        osg::ref_ptr<osg::Vec4Array> fades = new osg::Vec4Array;
        //! Allocates CPU-only arrays on the preparation worker; no graphics context or GL work occurs here.
        InstanceArrays() { for (auto& value : values) value = new osg::Vec4Array; }
        //! Allocates each instance buffer once, after counting the exact glyph population of this atlas page.
        void reserve(std::size_t count)
        {
            high->reserve(count); low->reserve(count); fades->reserve(count); tangents->reserve(count);
            softness->reserve(count);
            for (auto& value : values) value->reserve(count);
        }
    };
}

void osgEarthPrestige::Labeling::setTextFootprint(Record& record, const TextRun& run, const TextStyle& style)
{
    if (!std::isfinite(style.pixels) || style.pixels < 4 || style.pixels > 256 ||
        !std::isfinite(style.rotation) || !std::isfinite(style.haloWidth) ||
        !std::isfinite(style.calloutGap) || style.calloutGap < 0 || style.calloutGap > 64 ||
        !std::isfinite(style.haloSoftness) || style.haloSoftness < 0 || style.haloSoftness > 1 ||
        style.haloWidth < 0 || style.haloWidth > 2 || !std::isfinite(run.advance))
        throw std::invalid_argument("Invalid label footprint style");
    for (unsigned i = 0; i < 2; ++i)
        if (!std::isfinite(run.lowerLeft[i]) || !std::isfinite(run.upperRight[i]) ||
            run.lowerLeft[i] > run.upperRight[i])
            throw std::invalid_argument("Invalid label ink bounds");
    const osg::Vec2d center(run.advance * 0.5, 0);
    const osg::Vec2d padding(style.haloWidth + 1.5, style.haloWidth + 1.5);
    const auto lower = (osg::Vec2d(run.lowerLeft) - center) * style.pixels - padding;
    const auto upper = (osg::Vec2d(run.upperRight) - center) * style.pixels + padding;
    osg::Vec2d minimum(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
    osg::Vec2d maximum(-minimum.x(), -minimum.y());
    const double c = std::cos(style.rotation), s = std::sin(style.rotation);
    for (unsigned i = 0; i < 4; ++i)
    {
        const double x = i & 1 ? upper.x() : lower.x(), y = i & 2 ? upper.y() : lower.y();
        const osg::Vec2d corner(c * x - s * y, s * x + c * y);
        for (unsigned axis = 0; axis < 2; ++axis)
        {
            minimum[axis] = std::min(minimum[axis], corner[axis]);
            maximum[axis] = std::max(maximum[axis], corner[axis]);
        }
    }
    record.calloutGap = style.calloutGap;
    record.lowerLeft = minimum;
    record.upperRight = maximum;
}

std::size_t TextBatchResources::bytes() const
{
    std::size_t size = sizeof(*this) + styles.capacity() * sizeof(TextStyle);
    if (text)
    {
        size += sizeof(TextResources) + text->runs.capacity() * sizeof(std::shared_ptr<const TextRun>) +
            text->pages.capacity() * sizeof(std::shared_ptr<const GlyphPage>);
        for (const auto& run : text->runs) size += sizeof(TextRun) + run->glyphs.capacity() * sizeof(RunGlyph);
    }
    return size;
}

PrepareResult osgEarthPrestige::Labeling::prepareText(
    std::shared_ptr<const TextResources> resources, std::vector<TextStyle> styles)
{
    if (resources && (resources->pages.size() > 32 || styles.empty()))
        throw std::invalid_argument("Invalid label rendering resources");
    for (const auto& style : styles)
    {
        if (!std::isfinite(style.pixels) || style.pixels < 4 || style.pixels > 256 ||
            !std::isfinite(style.haloWidth) || style.haloWidth < 0 || style.haloWidth > 2 ||
            !std::isfinite(style.rotation) || !std::isfinite(style.haloSoftness) ||
            style.haloSoftness < 0 || style.haloSoftness > 1 ||
            !std::isfinite(style.calloutGap) || style.calloutGap < 0 || style.calloutGap > 64)
            throw std::invalid_argument("Invalid label text style");
        for (unsigned component = 0; component < 4; ++component)
            if (!std::isfinite(style.color[component]) || !std::isfinite(style.halo[component]))
                throw std::invalid_argument("Invalid label text color");
    }
    osg::ref_ptr<osg::Program> program = new osg::Program;
    program->addShader(new osg::Shader(osg::Shader::VERTEX, textVertexShader));
    program->addShader(new osg::Shader(osg::Shader::FRAGMENT, textFragmentShader));
    // Compile feathering only for packets that use it; keep crisp halos and every fill on the lean shader.
    osg::ref_ptr<osg::Program> softProgram = new osg::Program;
    softProgram->addShader(program->getShader(0));
    std::string softFragment(textFragmentShader);
    softFragment.insert(softFragment.find('\n', softFragment.find("#version")) + 1, "#define PRESTIGE_SOFT_HALOS 1\n");
    softProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT, softFragment));
    using PassStates = std::array<osg::ref_ptr<osg::StateSet>, 2>;
    using PageStates = std::map<std::shared_ptr<const GlyphPage>, PassStates>;
    auto pageStates = std::make_shared<PageStates>();
    // Valid zero textures keep optional samplers complete on pages that use the analytic fallback.
    osg::ref_ptr<osg::Image> empty = new osg::Image;
    empty->allocateImage(1, 1, 1, GL_RGBA, GL_FLOAT);
    empty->setInternalTextureFormat(GL_RGBA32F_ARB);
    *reinterpret_cast<osg::Vec4f*>(empty->data()) = osg::Vec4f();
    //! Creates immutable atlas state on the preparation worker, shared by all tiles using the same sealed page.
    auto stateFor = [program, pageStates, empty](const std::shared_ptr<const GlyphPage>& page)
    {
        auto found = pageStates->find(page);
        if (found != pageStates->end()) return found->second;
        if (pageStates->size() >= 64) throw std::runtime_error("Label renderer atlas page budget exceeded");
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
        state->setAttributeAndModes(program, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        state->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS, 0, 1, false));
        // VisibleLayer overrides blending with straight alpha. Our shaders premultiply both fill and halo;
        // protect the matching blend function and enable mode so fades/AA do not multiply alpha twice.
        state->setAttributeAndModes(new osg::BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA),
            osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);
        state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
        // Screen-space glyphs already apply their own horizon test; MapNode clip planes expect world vertices.
        state->setMode(GL_CLIP_PLANE0, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        state->setRenderBinDetails(100, "RenderBin");
        state->setTextureAttributeAndModes(0, texture(page->curves));
        state->setTextureAttributeAndModes(1, texture(page->bands));
        state->addUniform(new osg::Uniform("gpuPaths", false));
        state->addUniform(new osg::Uniform("pathGlyphs", 0));
        state->addUniform(new osg::Uniform("u_curveTexture", 0));
        state->addUniform(new osg::Uniform("u_bandTexture", 1));
        const bool cached = page->distance.valid() && page->distanceMap.valid();
        state->setTextureAttributeAndModes(2, texture(cached ? page->distance.get() : empty.get(), true));
        state->setTextureAttributeAndModes(3, texture(cached ? page->distanceMap.get() : empty.get()));
        state->addUniform(new osg::Uniform("u_distance", 2));
        state->addUniform(new osg::Uniform("u_distanceMap", 3));
        state->addUniform(new osg::Uniform("u_hasDistance", cached));
        PassStates passes;
        for (unsigned pass = 0; pass < passes.size(); ++pass)
        {
            passes[pass] = new osg::StateSet(*state, osg::CopyOp::SHALLOW_COPY);
            passes[pass]->addUniform(new osg::Uniform("u_textPass", int(pass)));
            // Every halo precedes every fill, across all atlas pages/chunks, to protect neighboring letter interiors.
            passes[pass]->setRenderBinDetails(100 + pass, "RenderBin");
        }
        (*pageStates)[page] = passes;
        return passes;
    };
    // Compile once per renderer/context, including across fade and tile publications.
    osg::ref_ptr<osg::Program> pathProgram = new osg::Program;
    pathProgram->addShader(new osg::Shader(osg::Shader::COMPUTE, pathComputeShader));
    osg::ref_ptr<osg::Program> leaderProgram = new osg::Program;
    leaderProgram->addShader(new osg::Shader(osg::Shader::VERTEX, leaderVertexShader));
    leaderProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT, leaderFragmentShader));
    //! Packs one bounded dynamic chunk, or the merged static/tiled selection, entirely on the worker.
    auto buildPacket = [resources, styles, stateFor, pathProgram, softProgram, leaderProgram](std::shared_ptr<DrawPacket> data)
    {
        struct ResolvedLabel
        {
            const Record* record;
            const TextRun* run;
            const TextStyle* style;
            const std::vector<std::size_t>* pages;
        };
        std::vector<ResolvedLabel> labels;
        labels.reserve(data->records.size());
        std::vector<std::shared_ptr<const GlyphPage>> pageSources;
        std::vector<std::size_t> counts;
        std::map<std::shared_ptr<const GlyphPage>, std::size_t> pages;
        // Runs from a batch share the same page-index mapping. Resolve each used page once, not once per glyph.
        std::map<const TextResources*, std::vector<std::size_t>> mappings;
        std::size_t total = 0;
        bool hasHalo = false, hasSoftHalo = false;
        for (std::size_t index = 0; index < data->records.size(); ++index)
        {
            const auto& selected = data->records[index];
            const auto& record = selected.first->records().at(selected.second);
            const auto* local = dynamic_cast<const TextBatchResources*>(selected.first->payload().get());
            const auto& text = local ? local->text : resources;
            const auto& textStyles = local ? local->styles : styles;
            if (!text || record.shapedRun >= text->runs.size() || record.style >= textStyles.size())
                throw std::runtime_error("Label refers to an unavailable shaped run or style");
            const auto& run = *text->runs.at(record.shapedRun);
            const auto& style = textStyles.at(record.style);
            hasHalo = hasHalo || (style.haloWidth > 0 && style.halo.a() > 0);
            hasSoftHalo = hasSoftHalo || (style.haloWidth > 0 && style.halo.a() > 0 && style.haloSoftness > 0);
            if (run.glyphs.size() > 131072 - total)
                throw std::runtime_error("Label result exceeds glyph instance budget");
            total += run.glyphs.size();
            auto mapping = mappings.find(text.get());
            if (mapping == mappings.end())
                mapping = mappings.emplace(text.get(), std::vector<std::size_t>(
                    text->pages.size(), std::numeric_limits<std::size_t>::max())).first;
            for (const auto& item : run.glyphs)
            {
                auto& destination = mapping->second.at(item.glyph.page);
                if (destination == std::numeric_limits<std::size_t>::max())
                {
                    const auto& glyphPage = text->pages.at(item.glyph.page);
                    auto found = pages.find(glyphPage);
                    if (found == pages.end())
                    {
                        found = pages.emplace(glyphPage, counts.size()).first;
                        pageSources.push_back(glyphPage);
                        counts.push_back(0);
                    }
                    destination = found->second;
                }
                ++counts[destination];
            }
            labels.push_back({ &record, &run, &style, &mapping->second });
        }
        std::vector<int> gpuStarts(labels.size(), -1);
        std::vector<osg::Vec4f> headers, vertices, offsets;
        for (std::size_t i = 0; i < labels.size(); ++i)
        {
            const auto& path = labels[i].record->path;
            if (!path || !path->gpu) continue;
            if (headers.size() == 65535 || vertices.size() / 2 + path->world.size() > 262144 ||
                offsets.size() + path->glyphs.size() > 131072)
                throw std::runtime_error("GPU label path table budget exceeded");
            gpuStarts[i] = static_cast<int>(offsets.size());
            headers.emplace_back(vertices.size() / 2, path->world.size(), offsets.size(), path->glyphs.size());
            for (const auto& vertex : path->world)
            {
                osg::Vec3f high, low; split(vertex, high, low);
                vertices.emplace_back(high, static_cast<float>(path->width));
                vertices.emplace_back(low, 0);
            }
            for (const auto& glyph : path->glyphs) offsets.emplace_back(glyph.distance, 0, 0, 0);
        }
        osg::ref_ptr<osg::Texture2D> gpuOutput;
        if (!headers.empty())
        {
            gpuOutput = pathTable(std::vector<osg::Vec4f>(offsets.size()));
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->setAttributeAndModes(pathProgram, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            state->setTextureAttributeAndModes(4, pathTable(headers));
            state->setTextureAttributeAndModes(5, pathTable(vertices));
            state->setTextureAttributeAndModes(6, pathTable(offsets));
            state->setTextureAttributeAndModes(7, gpuOutput);
            state->setAttributeAndModes(new osg::BindImageTexture(0, gpuOutput,
                osg::BindImageTexture::WRITE_ONLY, GL_RGBA32F_ARB));
            state->addUniform(new osg::Uniform("pathHeaders", 4));
            state->addUniform(new osg::Uniform("pathVertices", 5));
            state->addUniform(new osg::Uniform("pathOffsets", 6));
            state->setRenderBinDetails(99, "RenderBin");
            data->meshes.push_back(new PathComputeGeometry(static_cast<unsigned>(headers.size())));
            data->states.push_back(state);
        }
        // Leaders share the text packet's immutable ownership and epoch; no per-label cull work is added.
        InstanceArrays leaders;
        std::vector<InstanceArrays> arrays(counts.size());
        for (std::size_t i = 0; i < arrays.size(); ++i) arrays[i].reserve(counts[i]);
        for (std::size_t index = 0; index < labels.size(); ++index)
        {
            const auto& label = labels[index];
            const auto& run = *label.run;
            const auto& style = *label.style;
            const auto& fade = data->fades[index];
            osg::Vec3f high, low;
            split(label.record->worldAnchor, high, low);
            const auto& displacement = data->offsets[index];
            const double c = std::cos(style.rotation), s = std::sin(style.rotation);
            const osg::Vec2f localOffset(c * displacement.x() + s * displacement.y(),
                -s * displacement.x() + c * displacement.y());
            if (!label.record->path && displacement.length2() > 0.0)
            {
                const auto lower = label.record->lowerLeft + displacement;
                const auto upper = label.record->upperRight + displacement;
                const osg::Vec2f end(std::clamp(0.0, lower.x(), upper.x()), std::clamp(0.0, lower.y(), upper.y()));
                if (end.length2() > 0.0)
                {
                    leaders.high->push_back(high); leaders.low->push_back(low);
                    leaders.values[0]->push_back(osg::Vec4f(0, 0, end.x(), end.y()));
                    auto color = style.color; color.a() *= 0.7f;
                    leaders.values[4]->push_back(color);
                    leaders.values[6]->push_back(osg::Vec4f(1, 0, fade.velocity, fade.sampledTime - data->timeOrigin));
                    leaders.fades->push_back(osg::Vec4f(fade.relative ? fade.start : fade.start - data->timeOrigin,
                        fade.from, fade.to, fade.relative ? -0.2f : 0.2f));
                }
            }
            const float padding = (style.haloWidth + 1.5f) / style.pixels;
            std::size_t glyphIndex = 0;
            for (const auto& item : run.glyphs)
            {
                auto& page = arrays[(*label.pages)[item.glyph.page]];
                auto bounds = item.glyph.bounds + osg::Vec4f(-padding, -padding, padding, padding);
                const auto offset = item.offset - osg::Vec2f(run.advance * 0.5f, 0);
                osg::Vec4f pixels = (bounds + osg::Vec4f(offset.x(), offset.y(), offset.x(), offset.y())) *
                    style.pixels;
                pixels += osg::Vec4f(localOffset.x(), localOffset.y(), localOffset.x(), localOffset.y());
                osg::Vec3f tangent;
                if (data->paths[index])
                {
                    const auto& glyph = data->paths[index]->glyphs.at(glyphIndex);
                    split(glyph.world, high, low);
                    tangent = osg::Vec3f(glyph.tangent);
                    const auto& footprint = label.record->path->glyphs.at(glyphIndex);
                    pixels.set(footprint.lowerLeft.x(), footprint.lowerLeft.y(),
                        footprint.upperRight.x(), footprint.upperRight.y());
                }
                if (gpuOutput) page.pathIndices->push_back(gpuStarts[index] < 0 ? -1.0f : gpuStarts[index] + glyphIndex);
                ++glyphIndex;
                page.tangents->push_back(tangent);
                if (hasSoftHalo) page.softness->push_back(style.haloSoftness);
                page.high->push_back(high); page.low->push_back(low);
                page.values[0]->push_back(pixels);
                page.values[1]->push_back(bounds);
                page.values[2]->push_back(item.glyph.bandTransform);
                page.values[3]->push_back(item.glyph.bandAddress);
                page.values[4]->push_back(style.color);
                page.values[5]->push_back(style.halo);
                page.values[6]->push_back(osg::Vec4f(style.haloWidth, style.rotation, fade.velocity,
                    fade.sampledTime - data->timeOrigin));
                page.fades->push_back(osg::Vec4f(fade.relative ? fade.start : fade.start - data->timeOrigin,
                    fade.from, fade.to, fade.relative ? -0.2f : 0.2f));
            }
        }
        if (!leaders.high->empty())
        {
            osg::ref_ptr<osg::Geometry> geometry = new GlyphGeometry;
            geometry->setVertexArray(new osg::Vec3Array(4));
            geometry->setVertexAttribArray(6, leaders.high, osg::Array::BIND_PER_VERTEX);
            geometry->setVertexAttribArray(7, leaders.low, osg::Array::BIND_PER_VERTEX);
            for (unsigned i : { 0u, 4u, 6u })
                geometry->setVertexAttribArray(i + 8, leaders.values[i], osg::Array::BIND_PER_VERTEX);
            geometry->setVertexAttribArray(15, leaders.fades, osg::Array::BIND_PER_VERTEX);
            auto primitive = new osg::DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            primitive->setNumInstances(static_cast<int>(leaders.high->size()));
            geometry->addPrimitiveSet(primitive);
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->setAttributeAndModes(leaderProgram, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            state->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS, 0, 1, false));
            state->setAttributeAndModes(new osg::BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA),
                osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);
            state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
            state->setMode(GL_CLIP_PLANE0, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            state->setRenderBinDetails(98, "RenderBin");
            data->meshes.push_back(geometry);
            data->states.push_back(state);
        }
        data->glyphs = total;
        for (std::size_t p = 0; p < arrays.size(); ++p)
        {
            const auto& page = arrays[p];
            if (page.high->empty()) continue;
            osg::ref_ptr<osg::Geometry> geometry = new GlyphGeometry;
            geometry->setVertexArray(new osg::Vec3Array(4));
            if (hasSoftHalo) geometry->setVertexAttribArray(1, page.softness, osg::Array::BIND_PER_VERTEX);
            geometry->setVertexAttribArray(4, page.tangents, osg::Array::BIND_PER_VERTEX);
            if (gpuOutput) geometry->setVertexAttribArray(5, page.pathIndices, osg::Array::BIND_PER_VERTEX);
            geometry->setVertexAttribArray(6, page.high, osg::Array::BIND_PER_VERTEX);
            geometry->setVertexAttribArray(7, page.low, osg::Array::BIND_PER_VERTEX);
            for (unsigned i = 0; i < page.values.size(); ++i)
                geometry->setVertexAttribArray(i + 8, page.values[i], osg::Array::BIND_PER_VERTEX);
            geometry->setVertexAttribArray(15, page.fades, osg::Array::BIND_PER_VERTEX);
            auto primitive = new osg::DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            primitive->setNumInstances(static_cast<int>(page.high->size()));
            geometry->addPrimitiveSet(primitive);
            // Keep source geometry detached: the worker must not mutate shared StateSet parent lists.
            auto states = stateFor(pageSources[p]);
            if (gpuOutput) for (auto& state : states)
            {
                state = new osg::StateSet(*state, osg::CopyOp::SHALLOW_COPY);
                state->setTextureAttributeAndModes(7, gpuOutput);
                state->addUniform(new osg::Uniform("pathGlyphs", 7));
                state->addUniform(new osg::Uniform("gpuPaths", true));
            }
            if (hasSoftHalo)
            {
                states[0] = new osg::StateSet(*states[0], osg::CopyOp::SHALLOW_COPY);
                states[0]->setAttributeAndModes(softProgram, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            }
            if (hasHalo)
            {
                data->states.push_back(states[0]);
                data->meshes.push_back(new GlyphGeometry(*geometry));
            }
            data->states.push_back(states[1]);
            data->meshes.push_back(std::move(geometry));
        }
    };
    // Weak entries neither retain retired views nor defeat the engine's bounded result ownership.
    using Cache = std::array<std::weak_ptr<const DrawData>, 8>;
    return [buildPacket, cache = std::make_shared<Cache>()](Result& result)
    {
        if (result.input->view.slot >= cache->size())
            throw std::runtime_error("Text renderer supports at most eight view slots");
        auto data = std::make_shared<DrawData>();
        const auto count = result.presentation.empty() ? result.placements.size() : result.presentation.size();
        data->records.reserve(count); data->fades.reserve(count);
        if (result.presentation.empty())
        {
            for (const auto& placement : result.placements)
            {
                data->records.emplace_back(result.input->batches.at(placement.batch), placement.record);
                data->fades.emplace_back();
                data->paths.push_back(placement.path);
                data->offsets.push_back(placement.offset);
            }
        }
        else for (const auto& label : result.presentation)
        {
            data->records.emplace_back(label.batch, label.record);
            data->fades.push_back(label.fade);
            data->paths.push_back(label.path);
            data->offsets.push_back(label.offset);
        }
        auto previous = (*cache)[result.input->view.slot].lock();
        if (previous && previous->records == data->records && previous->fades == data->fades &&
            previous->offsets == data->offsets &&
            samePaths(data->records, previous->paths, data->paths))
        {
            result.payload = previous;
            return;
        }
        //! Keeps camera-dependent roads separate so road motion never repacks otherwise unchanged point-label arrays.
        auto packetKey = [](const BatchPtr& batch, std::size_t record)
        {
            const auto* local = dynamic_cast<const TextBatchResources*>(batch->payload().get());
            return std::make_pair(local && local->incremental ? batch.get() : nullptr, bool(batch->records()[record].path));
        };
        using PacketKey = std::pair<const Batch*, bool>;
        std::map<PacketKey, std::shared_ptr<const DrawPacket>> old;
        if (previous) for (const auto& packet : previous->packets)
            old.emplace(packetKey(packet->records.front().first, packet->records.front().second), packet);
        std::map<PacketKey, std::size_t> groups;
        std::vector<std::shared_ptr<DrawPacket>> packets;
        for (std::size_t i = 0; i < data->records.size(); ++i)
        {
            const auto key = packetKey(data->records[i].first, data->records[i].second);
            auto inserted = groups.emplace(key, packets.size());
            if (inserted.second)
            {
                auto packet = std::make_shared<DrawPacket>();
                const auto capacity = key.first ? key.first->records().size() : count;
                packet->records.reserve(capacity); packet->fades.reserve(capacity);
                packets.push_back(std::move(packet));
            }
            auto& packet = packets[inserted.first->second];
            packet->records.push_back(data->records[i]);
            packet->fades.push_back(data->fades[i]);
            packet->paths.push_back(data->paths[i]);
            packet->offsets.push_back(data->offsets[i]);
        }
        std::size_t glyphs = 0;
        for (auto& candidate : packets)
        {
            const auto found = old.find(packetKey(candidate->records.front().first, candidate->records.front().second));
            std::shared_ptr<const DrawPacket> packet;
            if (found != old.end() && found->second->records == candidate->records &&
                found->second->fades == candidate->fades && found->second->offsets == candidate->offsets &&
                samePaths(candidate->records, found->second->paths, candidate->paths))
                packet = found->second;
            else
            {
                candidate->timeOrigin = result.input->camera.time;
                buildPacket(candidate);
                packet = std::move(candidate);
            }
            if (packet->glyphs > 131072 - glyphs)
                throw std::runtime_error("Label result exceeds glyph instance budget");
            glyphs += packet->glyphs;
            data->packets.push_back(packet);
            data->meshes.insert(data->meshes.end(), packet->meshes.begin(), packet->meshes.end());
            data->states.insert(data->states.end(), packet->states.begin(), packet->states.end());
            data->timeOrigins.insert(data->timeOrigins.end(), packet->meshes.size(), packet->timeOrigin);
        }
        (*cache)[result.input->view.slot] = data;
        result.payload = std::move(data);
    };
}

ViewNode::ViewNode(std::shared_ptr<Engine> engine, std::vector<BatchPtr> batches, float density, bool geocentric,
    std::shared_ptr<TileSource> tiles, std::shared_ptr<DynamicSource> updates, std::shared_ptr<SceneSource> scene) :
    _engine(std::move(engine)), _sceneSource(std::move(scene)), _dynamicSource(std::move(updates)),
    _baseBatches(std::move(batches)),
    _batches(_baseBatches), _geocentric(geocentric), _density(density)
{
    if (!_engine || !std::isfinite(density) || density <= 0)
        throw std::invalid_argument("Invalid label view configuration");
    _view = _engine->registerView();
    if (!_view) throw std::runtime_error("Label view capacity exhausted");
    _tileSource = std::move(tiles);
    if (_tileSource)
    {
        _tileView = _tileSource->registerView();
        if (!_tileView) { _engine->removeView(_view); throw std::runtime_error("Tile view registration failed"); }
    }
    setCullingActive(false);
}

ViewNode::~ViewNode()
{
    if (_tileView) _tileView->close();
    _engine->removeView(_view);
}

void ViewNode::setDevicePixelRatio(float density)
{
    if (std::isfinite(density) && density > 0) _density = density;
}

void ViewNode::setDeclutterPadding(double padding)
{
    if (!std::isfinite(padding) || padding < 0) throw std::invalid_argument("Invalid declutter padding");
    if (_declutterPadding.exchange(padding) != padding) _layoutRefresh.store(true);
}

bool ViewNode::needsFrame() const
{
    // The control thread must not read cull-owned snapshots or the lazily adopted scene source pointer.
    const auto updates = _sceneSource ? _sceneSource->source() : _dynamicSource;
    return _layoutRefresh.load() || _needsFrame.load() || (_tileView && _tileView->busy()) ||
        (updates && updates->pendingOrNew(_dynamicSeen.load()));
}

void ViewNode::traverse(osg::NodeVisitor& visitor)
{
    auto* cv = dynamic_cast<osgUtil::CullVisitor*>(&visitor);
    CameraSnapshot camera;
    if (!cv || !captureCamera(*cv, _density, camera))
    {
        osg::Group::traverse(visitor);
        return;
    }
    camera.geocentric = _geocentric;
    if (_layoutRefresh.load() && _layoutRefresh.exchange(false)) _refresh = true;
    bool contentChanged = false;
    if (_sceneSource)
    {
        _dynamicSource = _sceneSource->source();
        auto visibility = _dynamicSource ? _sceneSource->visibility(cv->getCurrentCamera(), camera.frame) : nullptr;
        if (visibility != _sceneVisibility) { _sceneVisibility = std::move(visibility); _refresh = true; }
    }
    if (_tileView)
    {
        if (!_haveTileCamera || !samePose(_tileCamera, camera) || _tileCamera.width != camera.width ||
            _tileCamera.height != camera.height || _tileCamera.devicePixelRatio != camera.devicePixelRatio)
        {
            if (_tileView->submit(camera)) { _tileCamera = camera; _haveTileCamera = true; }
        }
        auto tiles = _tileView->acquire(_tiles ? _tiles->revision : 0);
        if (tiles)
        {
            contentChanged = !_tiles || tiles->batches != _tiles->batches;
            _tiles = std::move(tiles);
        }
    }
    if (_dynamicSource)
    {
        auto next = _dynamicSource->acquire(_dynamic ? _dynamic->revision : 0);
        if (next)
        {
            _dynamic = std::move(next);
            _dynamicSeen.store(_dynamic->revision);
            contentChanged = true;
        }
    }
    if (contentChanged)
    {
        _batches = _baseBatches;
        if (_tiles) _batches.insert(_batches.end(), _tiles->batches.begin(), _tiles->batches.end());
        if (_dynamic) _batches.insert(_batches.end(), _dynamic->batches.begin(), _dynamic->batches.end());
        _refresh = true;
    }
    const bool resize = _haveCamera && (camera.width != _last.width || camera.height != _last.height ||
        camera.x != _last.x || camera.y != _last.y || camera.devicePixelRatio != _last.devicePixelRatio);
    bool refresh = _refresh;
    bool urgent = false;
    if (_displayed)
    {
        const bool moving = !samePose(_displayed->input->camera, camera);
        if (!moving || !_inMotion) _stationaryTime = camera.time;
        _inMotion = moving;
    }
    if (resize || (_haveCamera && camera.time < _last.time) ||
        (_displayed && !compatible(*_displayed, camera, _stationaryTime)))
    {
        _view->invalidate();
        _displayed.reset();
        _haveCamera = false;
        _compacted = true;
        ++_invalidations;
        refresh = true;
        urgent = true;
    }
    auto result = _view->acquire(_seenSequence);
    if (result && dynamic_cast<const DrawData*>(result->payload.get()))
    {
        _seenSequence = result->input->sequence;
        // Moving scene nodes can publish every frame. Accept completed content generations in sequence;
        // requiring the newest one here would starve presentation indefinitely during continuous movement.
        // Camera membership still has to match, so removed/hidden registrations cannot reenter via stale work.
        const bool contentCompatible = _sceneSource ||
            result->input->dynamicRevision == (_dynamic ? _dynamic->revision : 0);
        if (compatible(*result, camera) && contentCompatible && result->input->sceneVisibility == _sceneVisibility &&
            result->input->displayedSequence == (_displayed ? _displayed->input->sequence : 0))
        {
            _displayed = std::move(result);
            _adoptedTime = camera.time;
            _inMotion = !samePose(_displayed->input->camera, camera);
            _stationaryTime = _displayed->input->camera.time;
            _compacted = _displayed->settledTime <= 0;
            ++_publications;
        }
        else { refresh = true; urgent = true; }
    }
    osg::Group::traverse(visitor);
    if (_displayed && _displayed->payload)
    {
        auto free = std::find_if(_frames.begin(), _frames.end(), [](const Frame& frame)
        {
            return std::all_of(frame.meshes.begin(), frame.meshes.end(),
                [](const auto& mesh) { return mesh->referenceCount() == 1; });
        });
        if (free != _frames.end())
        {
            const auto* data = static_cast<const DrawData*>(_displayed->payload.get());
            if (!free->pin || free->pin->payload != _displayed->payload)
            {
                // A free frame owns its clones exclusively. Reuse unchanged VAOs/state without touching in-flight frames.
                std::map<const osg::Geometry*, osg::ref_ptr<osg::Geometry>> reusable;
                if (free->pin && free->pin->payload)
                {
                    const auto* old = static_cast<const DrawData*>(free->pin->payload.get());
                    for (std::size_t i = 0; i < old->meshes.size(); ++i)
                        reusable.emplace(old->meshes[i].get(), free->meshes[i]);
                }
                free->meshes.clear();
                for (std::size_t i = 0; i < data->meshes.size(); ++i)
                {
                    auto found = reusable.find(data->meshes[i].get());
                    if (found != reusable.end())
                    {
                        free->meshes.push_back(std::move(found->second));
                        continue;
                    }
                    const auto* compute = dynamic_cast<const PathComputeGeometry*>(data->meshes[i].get());
                    osg::ref_ptr<osg::Geometry> mesh = compute ?
                        static_cast<osg::Geometry*>(new PathComputeGeometry(*compute)) :
                        static_cast<osg::Geometry*>(new GlyphGeometry(*data->meshes[i]));
                    auto* state = new osg::StateSet(*data->states[i], osg::CopyOp::SHALLOW_COPY);
                    state->addUniform(new osg::Uniform("cameraLinearProjection", osg::Matrixf()));
                    state->addUniform(new osg::Uniform("eyeHigh", osg::Vec3f()));
                    state->addUniform(new osg::Uniform("eyeLow", osg::Vec3f()));
                    state->addUniform(new osg::Uniform("viewportDensity", osg::Vec3f()));
                    state->addUniform(new osg::Uniform("frameTime", 0.0f));
                    state->addUniform(new osg::Uniform("adoptionTime", 0.0f));
                    state->addUniform(new osg::Uniform("geocentric", _geocentric));
                    mesh->setStateSet(state);
                    free->meshes.push_back(std::move(mesh));
                }
            }
            free->pin = _displayed;
            osg::Vec3f high, low;
            split(osg::Matrixd::inverse(camera.worldToView).getTrans(), high, low);
            auto linear = camera.worldToView;
            linear.setTrans(0, 0, 0);
            for (std::size_t i = 0; i < free->meshes.size(); ++i)
            {
                const auto& mesh = free->meshes[i];
                const double timeOrigin = data->timeOrigins[i];
                auto* state = mesh->getStateSet();
                state->getUniform("cameraLinearProjection")->set(osg::Matrixf(linear * camera.projection));
                state->getUniform("eyeHigh")->set(high);
                state->getUniform("eyeLow")->set(low);
                state->getUniform("viewportDensity")->set(osg::Vec3f(camera.width, camera.height, _density));
                state->getUniform("frameTime")->set(float(camera.time - timeOrigin));
                state->getUniform("adoptionTime")->set(float(_adoptedTime - timeOrigin));
                cv->pushStateSet(state);
                cv->addDrawable(mesh, cv->getModelViewMatrix());
                cv->popStateSet();
                ++_submissions;
            }
        }
    }
    const bool moved = !_haveCamera || !samePose(camera, _last);
    const bool settling = _displayed && !_compacted;
    const bool compact = settling && camera.time >= _adoptedTime + _displayed->settledTime;
    if ((moved || refresh || compact) && (!_haveCamera || urgent || camera.time - _lastSubmit >= 1.0 / 15.0))
    {
        auto request = std::make_unique<Request>();
        request->view = _view->key(); request->configuration = _view->configuration();
        request->camera = camera; request->sequence = ++_sequence; request->batches = _batches;
        request->dynamicRevision = _dynamic ? _dynamic->revision : 0;
        request->sceneVisibility = _sceneVisibility;
        request->declutterPadding = _declutterPadding.load();
        request->animate = true;
        request->displayedSequence = _displayed ? _displayed->input->sequence : 0;
        request->adoptedTime = _adoptedTime;
        std::unique_ptr<const Request> immutable = std::move(request);
        if (_view->submit(immutable))
        {
            _lastSubmit = camera.time;
            _last = camera;
            _haveCamera = true;
            refresh = false;
            if (compact) _compacted = true;
        }
    }
    _refresh = refresh;
    _needsFrame.store(moved || refresh || settling || _view->pendingOrNew(_seenSequence) ||
        (_tileView && _tileView->busy()) || (_dynamicSource && _dynamicSource->pendingOrNew(_dynamicSeen.load())));
}
