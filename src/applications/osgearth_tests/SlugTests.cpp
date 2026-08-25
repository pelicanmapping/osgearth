/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */

#include <osgEarth/catch.hpp>

#include "../../osgEarth/FeatureImageLayerSlugPacking.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace osgEarth::Util::detail;

TEST_CASE("Slug band packing preserves the consumed RG channels", "[slug]")
{
    constexpr std::size_t logicalTexels = 5u;
    const std::array<std::uint16_t, logicalTexels * 4u> source = {
        1u, 2u, 60001u, 60002u,
        3u, 4u, 60003u, 60004u,
        5u, 6u, 60005u, 60006u,
        7u, 8u, 60007u, 60008u,
        65535u, 32768u, 60009u, 60010u
    };

    REQUIRE(slugPackedBandTexelCount(logicalTexels) == 3u);

    std::vector<float> packed(
        slugPackedBandTexelCount(logicalTexels) * 4u, -1.0f);
    slugPackBandRG(source.data(), logicalTexels, packed.data());

    for (std::size_t i = 0u; i < logicalTexels; ++i)
    {
        const auto decoded = slugUnpackBandRG(packed.data(), i);
        CHECK(decoded.first == source[i * 4u]);
        CHECK(decoded.second == source[i * 4u + 1u]);
    }

    // The unused half of an odd final texel has deterministic zero padding.
    CHECK(packed[10] == 0.0f);
    CHECK(packed[11] == 0.0f);
}
