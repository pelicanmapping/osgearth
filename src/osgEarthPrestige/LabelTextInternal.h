/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#pragma once
#include "LabelEngineInternal.h"
#include <osg/Image>
#include <osg/Vec4f>
#include <string>
#include <stdexcept>

namespace osgEarthPrestige { namespace Labeling
{
    //! A shaped run needs glyphs absent from the selected font; callers may skip it or try another font.
    struct UnsupportedGlyph : std::runtime_error
    {
        //! Preserves the diagnostic while allowing tile preparation to isolate unsupported labels.
        UnsupportedGlyph() : std::runtime_error("Font lacks a glyph required by this label") { }
    };

    struct Glyph
    {
        unsigned page = 0, id = 0;
        osg::Vec4f bounds; // Ink bounds in em units.
        osg::Vec4f bandTransform, bandAddress;
    };

    struct RunGlyph
    {
        Glyph glyph;
        osg::Vec2f offset;
        unsigned cluster = 0; // UTF-8 byte offset, retained for future picking/editing.
    };

    struct TextRun
    {
        std::vector<RunGlyph> glyphs;
        osg::Vec2f lowerLeft, upperRight;
        float advance = 0.0f;
    };

    struct GlyphPage
    {
        osg::ref_ptr<osg::Image> curves, bands;
        // Optional immutable halo distance cache and band-address-indexed em-to-UV transform table.
        // Missing images or zero map entries select analytic rendering; storage counts toward bytes.
        osg::ref_ptr<osg::Image> distance, distanceMap;
        std::size_t bytes = 0;
    };

    struct TextResources
    {
        std::vector<std::shared_ptr<const GlyphPage>> pages;
        std::vector<std::shared_ptr<const TextRun>> runs;
        std::size_t bytes = 0;
    };

    /** Worker-confined font service. Uses glyph IDs from HarfBuzz, never codepoints as atlas glyph identity.
     * Each instance owns one fixed font/face; font fallback and mixed-direction paragraph layout are not provided.
     * Pages and runs are immutable; later prepare calls reuse old pages and only build pages for missing glyphs.
     */
    class OSGEARTHPRESTIGE_EXPORT TextService
    {
    public:
        //! Opens a scalable font on the preparation thread; throws on invalid fonts or missing dependencies.
        explicit TextService(const std::string& fontPath);
        //! Closes preparation-only FreeType/HarfBuzz state; published resources remain independently owned.
        ~TextService();
        /** Shapes UTF-8 runs, using HarfBuzz replacement for malformed input and rejecting missing glyphs.
         * Returns runs in input order, evicting unused cache entries while published runs remain pinned.
         * Throws on the fixed limits: 1,024 runs per call, 4,096 glyphs,
         * 32 atlas pages, 32 MiB atlas storage, 16 MiB shaped-run storage, or 4,096 bytes per input run.
         * Call only on the owning thread. Failures may retain completed cache entries but never alter published data.
         */
        std::shared_ptr<const TextResources> prepare(const std::vector<std::string>& text);
        TextService(const TextService&) = delete;
        TextService& operator=(const TextService&) = delete;
    private:
        struct State;
        std::unique_ptr<State> _state;
    };
} }
