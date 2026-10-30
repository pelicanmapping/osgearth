#version 450
#extension GL_ARB_bindless_texture : require
#extension GL_ARB_gpu_shader_int64 : require
#pragma include FeatureImageLayer.ScreenBins.h.glsl
layout(local_size_x = 64) in;
layout(std430, binding = 29) readonly buffer VectorAtlases { uint64_t oe_vector_atlases[]; };
layout(std430, binding = 16) buffer VectorAllocation { uint oe_vector_allocated; };
uniform uint oe_vector_bin_count;
uniform uint oe_vector_receiver_count;
uniform uint oe_vector_capacity;
uniform int oe_vector_phase;

// Reads the existing immutable FIL atlas without repacking or retaining SDK state.
vec4 atlasRead(sampler2D atlas, uint address)
{
    return texelFetch(atlas, ivec2(address & 1023u, address >> 10u), 0);
}

// Clips the source-UV domain against a screen frustum widened by the receiver's elevation/morph envelope.
// The fragment consumer verifies both the resulting UV bounds and the allowed AA footprint before using a bin.
bool sourceBounds(VectorReceiver receiver, Frustum frustum, out vec4 bounds)
{
    vec2 polygon[12], clipped[12];
    polygon[0] = receiver.domain.xy;
    polygon[1] = receiver.domain.zy;
    polygon[2] = receiver.domain.zw;
    polygon[3] = receiver.domain.xw;
    int count = 4;
    for (int plane = 0; plane < 4; ++plane)
    {
        vec4 f = frustum.planes[plane];
        vec2 normal = vec2(dot(f.xyz, receiver.axisU.xyz), dot(f.xyz, receiver.axisV.xyz));
        float offset = dot(f.xyz, receiver.origin.xyz) - f.w + dot(abs(f.xyz), receiver.error.xyz) + 0.001;
        int outputCount = 0;
        for (int i = 0; i < count; ++i)
        {
            vec2 a = polygon[i], b = polygon[(i + 1) % count];
            float da = dot(normal, a) + offset, db = dot(normal, b) + offset;
            if (da >= 0.0) clipped[outputCount++] = a;
            if ((da >= 0.0) != (db >= 0.0)) clipped[outputCount++] = mix(a, b, da / (da - db));
        }
        if (outputCount == 0) return false;
        count = outputCount;
        for (int i = 0; i < count; ++i) polygon[i] = clipped[i];
    }
    bounds = vec4(polygon[0], polygon[0]);
    for (int i = 1; i < count; ++i)
        bounds = vec4(min(bounds.xy, polygon[i]), max(bounds.zw, polygon[i]));
    bounds = vec4(max(bounds.xy - vec2(1e-7), vec2(0)), min(bounds.zw + vec2(1e-7), vec2(1)));
    return all(greaterThanEqual(bounds.zw, bounds.xy));
}

// Tests a stroke/curve carrier against an expanded source-UV query rectangle.
bool overlaps(vec4 a, vec4 b)
{
    return all(lessThanEqual(a.xy, b.zw)) && all(greaterThanEqual(a.zw, b.xy));
}

// Refines one ordered source cell per screen bin, reusing its full-cell padding as a safe temporal guard band.
// This avoids sorting/merging, preserves segment tie order, and blends each path once. Two passes reserve exact space.
bool buildList(sampler2D atlas, uint sourceCell, vec4 bounds, float aa, bool emit,
    uint base, out uint words, out uint head)
{
    words = 0u;
    head = 0u;
    vec4 meta = atlasRead(atlas, 2u);
    if (atlasRead(atlas, 0u).x != -73821.0 || meta.y != 64.0) return false;
    vec2 list = atlasRead(atlas, uint(meta.x) + sourceCell).xy;
    vec4 query = bounds + vec4(-aa, -aa, aa, aa);
    uint work = 0u;
    for (uint i = 0u; i < uint(list.y); ++i)
    {
        if (++work > 4096u) return false;
        vec4 entry = atlasRead(atlas, uint(list.x) + i);
        uint paint = uint(entry.x);
        vec4 band = atlasRead(atlas, paint + 1u);
        vec2 origin = atlasRead(atlas, paint + 3u).xy;
        vec2 carrierMin = origin - band.zw / band.xy;
        if (!overlaps(vec4(carrierMin, carrierMin + 32.0 / band.xy), query)) continue;
        vec4 shape = atlasRead(atlas, paint + 2u);
        uint kept = 0u;
        uint refsStart = base + words;
        uvec4 packedRefs = uvec4(0u);
        if (shape.w == 1.0)
        {
            uint segments = entry.z > 0.0 ? uint(entry.z) : uint(shape.y);
            vec4 references = vec4(0);
            for (uint j = 0u; j < segments; ++j)
            {
                if (++work > 4096u) return false;
                uint reference = uint(shape.x) + j;
                if (entry.z > 0.0)
                {
                    if ((j & 3u) == 0u) references = atlasRead(atlas, uint(entry.y) + (j >> 2u));
                    reference = uint(references[j & 3u]);
                }
                vec4 segment = atlasRead(atlas, reference);
                vec4 segmentBounds = vec4(min(segment.xy, segment.zw) - shape.z,
                    max(segment.xy, segment.zw) + shape.z);
                if (!overlaps(segmentBounds, query)) continue;
                packedRefs[kept & 3u] = reference;
                ++kept;
                if ((kept & 3u) == 0u)
                {
                    if (emit) oe_vector_words[base + words] = packedRefs;
                    ++words;
                }
            }
            if ((kept & 3u) != 0u)
            {
                if (emit) oe_vector_words[base + words] = packedRefs;
                ++words;
            }
        }
        if (shape.w != 1.0 || kept > 0u)
        {
            if (emit) oe_vector_words[base + words] = uvec4(paint, refsStart, kept, head);
            head = base + words;
            ++words;
        }
    }
    return true;
}

// One invocation per receiver/screen-bin pair; unsupported or overflowing bins are explicitly marked for fallback.
void main()
{
    uint bin = gl_GlobalInvocationID.x;
    if (bin >= oe_vector_bin_count) return;
    uint lo = 0u, hi = oe_vector_receiver_count;
    while (lo + 1u < hi)
    {
        uint mid = (lo + hi) / 2u;
        if (oe_vector_receivers[mid].data.x <= bin) lo = mid; else hi = mid;
    }
    VectorReceiver receiver = oe_vector_receivers[lo];
    sampler2D atlas = sampler2D(oe_vector_atlases[receiver.data.y]);
    if (oe_vector_phase == 0)
    {
        oe_vector_bins[bin].list = uvec4(OE_VECTOR_FALLBACK, 0u, 0u, 0u);
        uint local = bin - receiver.data.x;
        ivec2 xy = receiver.bins.xy + ivec2(local % uint(receiver.bins.z), local / uint(receiver.bins.z));
        vec4 bounds;
        if (!sourceBounds(receiver, frustums[xy.y * u_numTiles.x + xy.x], bounds)) return;
        float aa = min(1.0 / 64.0, max(bounds.z - bounds.x, bounds.w - bounds.y) * (1.1 / float(u_pixelsPerTile)));
        aa = max(aa, 1e-7);
        // Avoid constructing an index when a screen bin spans many fine source cells.
        if (any(greaterThan(bounds.zw - bounds.xy, vec2(2.0 / 64.0)))) return;
        ivec2 sourceCell = clamp(ivec2((bounds.xy + bounds.zw) * 32.0), ivec2(0), ivec2(63));
        // appendGrid includes a full source-cell margin. Reserve AA space inside it, then expand the
        // query by two projected pixels so small camera movements do not split fragment warps into both paths.
        vec2 safeMin = max(vec2(sourceCell - 1) / 64.0 + aa + 2e-7, vec2(0));
        vec2 safeMax = min(vec2(sourceCell + 2) / 64.0 - aa - 2e-7, vec2(1));
        bounds = vec4(max(bounds.xy - 2.0 * aa, safeMin), min(bounds.zw + 2.0 * aa, safeMax));
        if (any(lessThan(bounds.zw, bounds.xy))) return;
        uint words, head;
        if (!buildList(atlas, uint(sourceCell.y * 64 + sourceCell.x), bounds, aa, false, 0u, words, head)) return;
        uint base = words > 0u ? atomicAdd(oe_vector_allocated, words) : 0u;
        if (base + words > oe_vector_capacity) return;
        oe_vector_bins[bin].bounds = bounds;
        oe_vector_bins[bin].list = uvec4(base, words, floatBitsToUint(aa), uint(sourceCell.y * 64 + sourceCell.x + 1));
    }
    else
    {
        uvec4 list = oe_vector_bins[bin].list;
        if (list.x == OE_VECTOR_FALLBACK || list.y == 0u) return;
        uint words, head;
        bool ok = buildList(atlas, list.w - 1u, oe_vector_bins[bin].bounds,
            uintBitsToFloat(list.z), true, list.x, words, head);
        oe_vector_bins[bin].list.x = ok && words == list.y ? head : OE_VECTOR_FALLBACK;
    }
}
