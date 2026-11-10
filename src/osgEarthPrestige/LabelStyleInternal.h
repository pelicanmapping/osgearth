/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#pragma once
#include "LabelRendererInternal.h"
#include <osgEarth/Style>
#include <osgEarth/CssUtils>
#include <osgEarth/TextSymbol>
#include <osgEarth/Color>
#include <osgEarth/StringUtils>
#include <cmath>
#include <optional>

namespace osgEarthPrestige { namespace Labeling
{
    /** Reads private halo controls on layer open, matching Style's first CSS block or structured text symbol.
     * Normalizes CSS names to structured keys without changing the legacy TextSymbol ABI.
     */
    inline osgEarth::Config readHaloProperties(const osgEarth::Config& conf)
    {
        using namespace osgEarth;
        if (trim(conf.value()).empty() && conf.value("type") != "text/css")
            return conf.child("symbols").child("text");
        ConfigSet blocks;
        CssUtils::readConfig(conf.value(), conf.referrer(), blocks);
        Config result;
        if (!blocks.empty())
            for (const std::string name : { "width", "softness" })
                if (blocks.front().hasValue("text-halo-" + name))
                    result.set("halo_" + name, blocks.front().value("text-halo-" + name));
        return result;
    }

    /** Reads normalized halo properties' pixel width; accepts a bare number or px suffix.
     * Missing values return nullopt; invalid values throw before any renderer resources are prepared.
     */
    inline std::optional<float> readHaloWidth(const osgEarth::Config& values)
    {
        using namespace osgEarth;
        const std::string key = "halo_width";
        if (!values.hasValue(key)) return std::nullopt;
        auto value = trim(values.value(key));
        if (value.size() >= 2 && value.compare(value.size() - 2, 2, "px") == 0)
            value = trim(value.substr(0, value.size() - 2));
        const auto number = isValidNumber(value);
        if (!number.first || !std::isfinite(number.second) || number.second < 0 || number.second > 2)
            throw std::invalid_argument("Prestige text-halo-width must be a constant from 0 to 2 logical pixels");
        return static_cast<float>(number.second);
    }

    /** Resolves constant CSS/XML TextSymbol styling on the layer-open thread; no feature, font, or GL work.
     * Standard symbol values override prototype attributes. CSS without text-halo has no halo, like TextSymbol.
     * Explicit pixel width overrides normalized offset (offset * logical font pixels); backdrop none still disables it.
     * Throws for unsupported expressions/backdrops or invalid renderer limits; the caller reports a layer error.
     */
    inline TextStyle readTextStyle(const osgEarth::Config& conf)
    {
        using namespace osgEarth;
        TextStyle result;
        result.pixels = conf.value<float>("size", 18);
        result.color = Color(conf.value<std::string>("fill", "#263548"));
        result.halo = Color(conf.value<std::string>("halo", "#fffdf5"));
        result.haloWidth = conf.value<float>("halo_width", 1);
        result.haloSoftness = conf.value<float>("halo_softness", 0);
        const auto haloProperties = readHaloProperties(conf);
        const auto width = readHaloWidth(haloProperties);
        if (haloProperties.hasValue("halo_softness"))
        {
            const auto softness = isValidNumber(trim(haloProperties.value("halo_softness")));
            if (!softness.first || !std::isfinite(softness.second) || softness.second < 0 || softness.second > 1)
                throw std::invalid_argument("Prestige text-halo-softness must be a constant from 0 to 1");
            result.haloSoftness = static_cast<float>(softness.second);
        }
        if (width && conf.hasValue("halo")) result.haloWidth = *width;
        const Style style(conf);
        if (const auto* text = style.get<TextSymbol>())
        {
            if (text->size().isSet())
            {
                const auto& size = text->size().get();
                if (!size.expression().empty() && !isValidNumber(size.expression()).first)
                    throw std::invalid_argument("Prestige text-size must be a constant number");
                result.pixels = size.literal();
            }
            if (text->fill().isSet()) result.color = text->fill()->color();
            if (text->halo().isSet())
            {
                result.halo = text->halo()->color();
                result.haloWidth = text->haloOffset().get() * result.pixels;
            }
            else if (!conf.hasValue("halo"))
                result.haloWidth = 0;
            if (text->haloOffset().isSet() && (text->halo().isSet() || conf.hasValue("halo")))
                result.haloWidth = text->haloOffset().get() * result.pixels;
            if (width && (text->halo().isSet() || conf.hasValue("halo"))) result.haloWidth = *width;
            if (text->haloBackdropType().get() == osgText::Text::NONE)
                result.haloWidth = 0;
            else if (text->haloBackdropType().get() != osgText::Text::OUTLINE)
                throw std::invalid_argument("Prestige text-halo-backdrop-type supports outline and none");
            if (text->haloImplementation().isSet())
                throw std::invalid_argument("Prestige uses batched alpha halos; omit text-halo-implementation");
        }
        else if (!trim(conf.value()).empty() && !conf.hasValue("halo"))
            result.haloWidth = 0;
        Record validation;
        setTextFootprint(validation, TextRun(), result);
        for (unsigned i = 0; i < 4; ++i)
            if (!std::isfinite(result.color[i]) || !std::isfinite(result.halo[i]))
                throw std::invalid_argument("Invalid Prestige text color");
        return result;
    }
} }
