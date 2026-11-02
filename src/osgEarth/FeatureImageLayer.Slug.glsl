#pragma vp_name FeatureImageLayer Slug
#pragma vp_entryPoint oe_rex_imageLayer_FS
#pragma vp_location fragment_coloring
#pragma vp_order 0.5
#pragma import_defines(OE_USE_GL4, OE_TERRAIN_RENDER_IMAGERY)
#pragma import_defines(OE_TERRAIN_BLEND_IMAGERY, OE_TERRAIN_CAST_SHADOWS)
#pragma import_defines(OE_IS_PICK_CAMERA, OE_IS_SHADOW_CAMERA, OE_IS_DEPTH_CAMERA)

#define SLUG_INDIRECTION_SIZE 32

// Source-UV footprint at fade start/end. Layer configuration can request an earlier cutoff.
uniform vec2 oe_featureimage_vector_fade = vec2(1.0 / 32.0, 1.0 / 16.0);

// Reads linear addresses from the tile's combined metadata/curve/band texture.
vec4 oe_slug_read(sampler2D atlas, int address)
{
    // VectorTile packs 1024-wide rows. Avoid integer division at every dependent fetch.
    // Raster fallback detection uses address zero, which works for any image width.
    return texelFetch(atlas, ivec2(address & 1023, address >> 10), 0);
}

/*
 * Slug analytic coverage evaluator.
 *
 * Adapted from example/slughorn-example-glfw.cpp in Slughorn commit
 * 312ef217aaf6b1c47b05ba7575342b513daa830d.
 * Atlas addressing updated for the packed, row-spanning layout in commit
 * 1b203c191bd152c20685f8a896d22dcff974cec4.
 *
 * Copyright (c) 2026 AlphaPixel LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * The caller supplies derivatives of the inherited source UVs, so coverage
 * remains analytic when terrain children reuse an ancestor atlas.
 */
// Classifies signed quadratic crossings using the Slug root-code table.
uint slug_CalcRootCode(float y1, float y2, float y3)
{
    uint i1 = floatBitsToUint(y1) >> 31u;
    uint i2 = floatBitsToUint(y2) >> 30u;
    uint i3 = floatBitsToUint(y3) >> 29u;

    uint shift = (i2 & 2u) | (i1 & ~2u);
    shift = (i3 & 4u) | (shift & ~4u);

    return ((0x2E74u >> shift) & 0x0101u);
}

// Solves horizontal ray intersections, using a linear root for degenerate quadratics.
vec2 slug_SolveHorizPoly(vec4 p12, vec2 p3)
{
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float t1, t2;
    if (abs(a.y) < 1.0 / 65536.0)
    {
        // Straight and axis-degenerate curves dominate stroked line work. Take
        // the linear root before issuing the quadratic reciprocal and sqrt.
        t1 = p12.y * (0.5 / b.y);
        t2 = t1;
    }
    else
    {
        float ra = 1.0 / a.y;
        float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
        t1 = (b.y - d) * ra;
        t2 = (b.y + d) * ra;
    }
    return vec2(
        (a.x * t1 - b.x * 2.0) * t1 + p12.x,
        (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

// Solves vertical ray intersections with the same degeneracy policy.
vec2 slug_SolveVertPoly(vec4 p12, vec2 p3)
{
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float t1, t2;
    if (abs(a.x) < 1.0 / 65536.0)
    {
        t1 = p12.x * (0.5 / b.x);
        t2 = t1;
    }
    else
    {
        float ra = 1.0 / a.x;
        float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
        t1 = (b.x - d) * ra;
        t2 = (b.x + d) * ra;
    }
    return vec2(
        (a.y * t1 - b.y * 2.0) * t1 + p12.y,
        (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

// Combines signed horizontal/vertical coverage with analytic edge weights.
float slug_CalcCoverage(float xcov, float ycov, float xwgt, float ywgt)
{
    float coverage = max(
        abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, 1.0 / 65536.0),
        min(abs(xcov), abs(ycov)));
    return clamp(coverage, 0.0, 1.0);
}

// Evaluates one shape using direct band headers and four packed curve addresses per list texel.
float slug_Render(
    vec2 renderCoord,
    vec2 pixelsPerEm,
    vec2 bandCoord,
    int headerBase,
    sampler2D atlas
    )
{
    // CPU packing resolves the SDK's band indirection once instead of per fragment.
    int qY = clamp(int(bandCoord.y), 0, SLUG_INDIRECTION_SIZE - 1);
    int qX = clamp(int(bandCoord.x), 0, SLUG_INDIRECTION_SIZE - 1);
    ivec2 hband = ivec2(oe_slug_read(atlas, headerBase + qY).xy);
    float xcov = 0.0, xwgt = 0.0;
    vec4 references = vec4(0.0);
    for (int ci = 0; ci < hband.x; ++ci)
    {
        if ((ci & 3) == 0) references = oe_slug_read(atlas, hband.y + (ci >> 2));
        int curve = int(references[ci & 3]);
        vec4 p12 = oe_slug_read(atlas, curve) - vec4(renderCoord, renderCoord);
        vec2 p3 = oe_slug_read(atlas, curve + 1).xy - renderCoord;

        if (max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5) break;

        uint code = slug_CalcRootCode(p12.y, p12.w, p3.y);
        if (code != 0u)
        {
            vec2 r = slug_SolveHorizPoly(p12, p3) * pixelsPerEm.x;
            if ((code & 1u) != 0u)
            {
                xcov += clamp(r.x + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1u)
            {
                xcov -= clamp(r.y + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    ivec2 vband = ivec2(oe_slug_read(atlas, headerBase + qX).zw);
    float ycov = 0.0, ywgt = 0.0;
    for (int ci = 0; ci < vband.x; ++ci)
    {
        if ((ci & 3) == 0) references = oe_slug_read(atlas, vband.y + (ci >> 2));
        int curve = int(references[ci & 3]);
        vec4 p12 = oe_slug_read(atlas, curve) - vec4(renderCoord, renderCoord);
        vec2 p3 = oe_slug_read(atlas, curve + 1).xy - renderCoord;

        if (max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5) break;

        uint code = slug_CalcRootCode(p12.x, p12.z, p3.x);
        if (code != 0u)
        {
            vec2 r = slug_SolveVertPoly(p12, p3) * pixelsPerEm.y;
            if ((code & 1u) != 0u)
            {
                ycov -= clamp(r.x + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1u)
            {
                ycov += clamp(r.y + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    return slug_CalcCoverage(xcov, ycov, xwgt, ywgt);
}


// Updates the nearest capsule in original segment order, retaining deterministic join gradients.
void oe_slug_nearest(vec4 segment, vec2 uv, inout float closest2, inout vec2 nearest, inout vec2 tangent)
{
    vec2 edge = segment.zw - segment.xy, relative = uv - segment.xy;
    float t = clamp(dot(relative, edge) / max(dot(edge, edge), 1e-30), 0.0, 1.0);
    vec2 delta = relative - t * edge;
    float d2 = dot(delta, delta);
    if (d2 < closest2)
    {
        closest2 = d2;
        nearest = delta;
        tangent = edge;
    }
}

// Evaluates a round-capped, round-joined path as a union of capsules, blending the path only once.
// Cell-local segment references preserve nearest-segment order and one blend per path.
// Derivatives are supplied before divergent branches and remain in the inherited source-tile frame.
float oe_slug_stroke(sampler2D atlas, vec2 uv, vec2 dx, vec2 dy, vec4 stroke, vec2 segmentList)
{
    float closest2 = 1e30;
    vec2 nearest = vec2(0.0), tangent = vec2(1.0, 0.0);
    // Keep the full-path fallback loop free of per-segment indirection branches.
    if (segmentList.y > 0.0)
    {
        vec4 references = vec4(0.0);
        for (int i = 0; i < int(segmentList.y); ++i)
        {
            if ((i & 3) == 0) references = oe_slug_read(atlas, int(segmentList.x) + (i >> 2));
            vec4 segment = oe_slug_read(atlas, int(references[i & 3]));
            oe_slug_nearest(segment, uv, closest2, nearest, tangent);
        }
    }
    else
    {
        for (int i = 0; i < int(stroke.y); ++i)
        {
            vec4 segment = oe_slug_read(atlas, int(stroke.x) + i);
            oe_slug_nearest(segment, uv, closest2, nearest, tangent);
        }
    }
    float distance = sqrt(closest2);
    // At the centerline the distance gradient is undefined; the segment normal still gives the pixel width.
    vec2 normal = distance > 1e-12 ? nearest / distance :
        vec2(-tangent.y, tangent.x) * inversesqrt(max(dot(tangent, tangent), 1e-30));
    float aa = max(abs(dot(normal, dx)) + abs(dot(normal, dy)), 1e-9);
    // Account for both edges when a stroke is narrower than a pixel; it must fade toward zero with its width.
    return clamp(0.5 + (stroke.z - distance) / aa, 0.0, 1.0) -
        clamp(0.5 - (stroke.z + distance) / aa, 0.0, 1.0);
}

// Samples the source atlas in the inherited raster texture coordinate frame, not child-tile UVs.
// Ordinary raster fallback images are distinguished by a negative data-format sentinel.
vec4 oe_slug_sample(sampler2D atlas, vec2 uv, vec2 dx, vec2 dy)
{
    vec2 footprint = max(abs(dx) + abs(dy), vec2(1e-9));
    float projectedFootprint = max(footprint.x, footprint.y);
    // Skip atlas reads and all shape/segment searches once the source tile is too small on screen.
    if (projectedFootprint >= oe_featureimage_vector_fade.y) return vec4(0.0);
    // Equal endpoints are a hard cutoff; smoothstep is undefined when its edges coincide.
    float visibility = oe_featureimage_vector_fade.x < oe_featureimage_vector_fade.y ?
        1.0 - smoothstep(oe_featureimage_vector_fade.x, oe_featureimage_vector_fade.y, projectedFootprint) : 1.0;
    vec4 header = oe_slug_read(atlas, 0);
    if (header.x != -73821.0)
    {
        vec4 raster = textureGrad(atlas, uv, dx, dy);
        raster.a *= visibility;
        return raster;
    }
    vec4 meta = oe_slug_read(atlas, 2);
    vec2 pixelsPerEm = 1.0 / footprint;
    int gridSize = int(meta.y);
    int gridBase = int(meta.x);
    // Use the 16x16 index until the visibility cutoff; minification never scans all shapes.
    if (max(footprint.x, footprint.y) > 1.0 / float(gridSize) && meta.w > 0.0)
    {
        gridSize = 16;
        gridBase = int(meta.w);
    }
    ivec2 cell = clamp(ivec2(uv * float(gridSize)), ivec2(0), ivec2(gridSize - 1));
    vec2 list = oe_slug_read(atlas, gridBase + cell.y * gridSize + cell.x).xy;
    // Preserve off-tile sampling without treating minification as a full-path search request.
    bool outside = any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)));
    // Both source grids include a full cell of padding. Retain the original UVs for coverage,
    // and use a clamped edge cell when its padding also contains this fragment's AA footprint.
    if (outside)
    {
        float footprintSize = max(footprint.x, footprint.y);
        float overshoot = max(max(-uv.x, -uv.y), max(uv.x - 1.0, uv.y - 1.0));
        if (gridSize != 16 && meta.w > 0.0 &&
            overshoot > 1.0 / float(gridSize) - footprintSize - 2e-7)
        {
            gridSize = 16;
            cell = clamp(ivec2(uv * 16.0), ivec2(0), ivec2(15));
            list = oe_slug_read(atlas, int(meta.w) + cell.y * 16 + cell.x).xy;
        }
        // Leave room for antialiasing and float rounding before trusting an edge-cell list.
        float padding = 1.0 / float(gridSize) - footprintSize - 2e-7;
        if (padding >= 0.0 && all(greaterThanEqual(uv, vec2(-padding))) &&
            all(lessThanEqual(uv, vec2(1.0 + padding)))) outside = false;
    }
    int count = outside ? int(header.w) : int(list.y);
    vec4 result = vec4(0.0);
    float remaining = 1.0;
    // Front-to-back premultiplied compositing preserves paint order and can stop
    // exactly at opaque coverage. There is no approximate alpha cutoff.
    for (int i = count - 1; i >= 0; --i)
    {
        vec4 entry = outside ? vec4(float(3 + 4 * i), 0.0, 0.0, 0.0) :
            oe_slug_read(atlas, int(list.x) + i);
        int address = int(entry.x);
        vec4 band = oe_slug_read(atlas, address + 1);
        vec2 origin = oe_slug_read(atlas, address + 3).xy;
        vec2 coord = uv - origin;
        vec2 bandCoord = coord * band.xy + band.zw;
        vec2 margin = 0.5 * abs(footprint * band.xy);
        // Curves and strokes share carrier bounds. Reject before fetching type or curve/segment metadata.
        if (any(lessThan(bandCoord, -margin)) ||
            any(greaterThan(bandCoord, vec2(float(SLUG_INDIRECTION_SIZE)) + margin))) continue;
        vec4 shape = oe_slug_read(atlas, address + 2);
        float coverage = shape.w == 1.0 ? oe_slug_stroke(atlas, uv, dx, dy, shape, entry.yz) :
            slug_Render(coord, pixelsPerEm, bandCoord, int(shape.x), atlas);
        if (coverage == 0.0) continue;
        vec4 fill = oe_slug_read(atlas, address);
        float alpha = fill.a * coverage;
        result += vec4(fill.rgb * alpha, alpha) * remaining;
        remaining *= 1.0 - alpha;
        if (remaining == 0.0) break;
    }
    vec4 background = oe_slug_read(atlas, 1);
    result += vec4(background.rgb * background.a, background.a) * remaining;
    if (result.a > 0.0) result.rgb /= result.a;
    result.a *= visibility;
    return result;
}

// Computes UV derivatives before the evaluator branches on individual shape coverage.
vec4 oe_slug_sample(sampler2D atlas, vec2 uv)
{
    return oe_slug_sample(atlas, uv, dFdx(uv), dFdy(uv));
}

// The layer VP overrides REX's image coloring entry point only for this layer.
// REX still supplies coordinates/handles and performs tile inheritance and LOD selection.
#ifdef OE_USE_GL4
in vec2 oe_color_uv;
flat in uint64_t oe_color_handle;
flat in int oe_draw_order;
#else
uniform sampler2D oe_layer_tex;
uniform int oe_layer_uid;
uniform int oe_layer_order;
in vec2 oe_layer_texc;
#endif
in float oe_layer_opacity;
flat in int oe_terrain_vertexMarker;

// Retains REX's visibility, picking, depth/skirt, opacity, and first-layer blending policy.
void oe_rex_imageLayer_FS(inout vec4 color)
{
    if ((oe_terrain_vertexMarker & 1) == 0) discard;
#if defined(OE_IS_SHADOW_CAMERA) && !defined(OE_TERRAIN_CAST_SHADOWS)
    discard;
#endif
#ifdef OE_IS_DEPTH_CAMERA
    if ((oe_terrain_vertexMarker & 8) != 0) discard;
#endif
#ifdef OE_IS_PICK_CAMERA
    color = vec4(0);
    return;
#endif
#ifndef OE_TERRAIN_RENDER_IMAGERY
    return;
#endif
    // Vector layers inherit the source atlas/UV matrix, without imagery crossfades.
#ifdef OE_USE_GL4
    if (oe_color_handle == 0UL) return;
    bool first = oe_draw_order == 0;
    vec4 texel = oe_slug_sample(sampler2D(oe_color_handle), oe_color_uv);
#else
    if (oe_layer_uid < 0) return;
    bool first = oe_layer_order == 0;
    vec4 texel = oe_slug_sample(oe_layer_tex, oe_layer_texc);
#endif
    texel.a *= oe_layer_opacity;
#ifdef OE_TERRAIN_BLEND_IMAGERY
    if (first)
    {
        color.rgb = texel.rgb * texel.a + color.rgb * (1.0 - texel.a);
        color.a = 1.0;
    }
    else color = texel;
#else
    color = texel;
#endif
}
