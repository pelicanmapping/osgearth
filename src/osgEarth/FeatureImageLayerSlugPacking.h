/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace osgEarth
{
    namespace Util
    {
        namespace detail
        {
            // Slughorn emits RGBA16UI band texels, but its rendering contract
            // only consumes R and G. The FeatureImage tile has one RGBA32F
            // texture for metadata, curves, and bands, so store two logical
            // RG band records in each physical RGBA texel.
            inline std::size_t slugPackedBandTexelCount(
                std::size_t logicalTexelCount)
            {
                return (logicalTexelCount + 1u) / 2u;
            }

            inline void slugPackBandRG(
                const void* sourceRGBA16,
                std::size_t logicalTexelCount,
                float* destinationRGBA32)
            {
                const auto* source = static_cast<const std::uint8_t*>(sourceRGBA16);
                const std::size_t packedTexelCount = logicalTexelCount / 2u;

                for (std::size_t packed = 0u; packed < packedTexelCount; ++packed)
                {
                    std::uint16_t first[2];
                    std::uint16_t second[2];
                    std::memcpy(first, source + packed * 16u, sizeof(first));
                    std::memcpy(second, source + packed * 16u + 8u, sizeof(second));

                    float* destination = destinationRGBA32 + packed * 4u;
                    destination[0] = static_cast<float>(first[0]);
                    destination[1] = static_cast<float>(first[1]);
                    destination[2] = static_cast<float>(second[0]);
                    destination[3] = static_cast<float>(second[1]);
                }

                if ((logicalTexelCount & 1u) != 0u)
                {
                    std::uint16_t final[2];
                    std::memcpy(
                        final, source + packedTexelCount * 16u, sizeof(final));
                    float* destination = destinationRGBA32 + packedTexelCount * 4u;
                    destination[0] = static_cast<float>(final[0]);
                    destination[1] = static_cast<float>(final[1]);
                    destination[2] = 0.0f;
                    destination[3] = 0.0f;
                }
            }

            inline std::pair<std::uint16_t, std::uint16_t> slugUnpackBandRG(
                const float* sourceRGBA32,
                std::size_t logicalIndex)
            {
                const float* source = sourceRGBA32 + (logicalIndex / 2u) * 4u;
                const std::size_t channel = (logicalIndex & 1u) * 2u;
                return {
                    static_cast<std::uint16_t>(source[channel] + 0.5f),
                    static_cast<std::uint16_t>(source[channel + 1u] + 0.5f)
                };
            }
        }
    }
}
