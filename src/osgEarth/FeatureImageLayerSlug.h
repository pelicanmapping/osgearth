/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once

#include <osgEarth/ImageLayer>
#include <osgEarth/Feature>
#include <osgEarth/Style>
#include <osgEarth/FilterContext>

#include <memory>

namespace osgEarth
{
    namespace Util
    {
        // C++17-facing boundary around the C++20 Slughorn SDK. This is private to
        // FeatureImageLayer so no Slughorn types become part of osgEarth's ABI.
        class FeatureImageLayerSlug
        {
        public:
            FeatureImageLayerSlug(
                unsigned tileSize,
                const GeoExtent& extent,
                const Color& backgroundColor,
                unsigned atlasTextureWidth);

            ~FeatureImageLayerSlug();

            void render(
                const FeatureList& features,
                const Style& style,
                FilterContext& context);

            TextureWindow finalize();

        private:
            struct Impl;
            std::unique_ptr<Impl> _impl;
        };
    }
}
