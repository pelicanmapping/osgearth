/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarth/Chonk>
#include <osgEarth/Status>
#include <array>

namespace osgEarth { namespace Procedural2
{
    //! Combines nine static source proxies into certified template footprints, sharing materials and textures.
    //! CPU only; sources must have one LOD of at most 128 triangles. Failure clears output. Caller pins sources.
    Status assembleCanopyAsset(unsigned coverage, unsigned layout, const std::array<Chonk::Ptr,9>& sources,
        Chonk::Ptr& output);
} }
