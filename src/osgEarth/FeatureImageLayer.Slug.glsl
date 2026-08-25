// Analytic Slug renderer for FeatureImageLayer payload textures.
// The Slughorn SDK builds the curve/band atlas; FeatureImageLayerSlug packs those
// buffers and a compact layer table into oe_layer_tex.

#pragma vp_name       FeatureImageLayer Slug incoming color
#pragma vp_entryPoint oe_featureImageLayer_slug_saveColor
#pragma vp_location   fragment_coloring
#pragma vp_order      0.49

vec4 oe_featureImageLayer_slug_incomingColor;

void oe_featureImageLayer_slug_saveColor(inout vec4 color)
{
    oe_featureImageLayer_slug_incomingColor = color;
}

[break]

#pragma vp_name       FeatureImageLayer Slug
#pragma vp_entryPoint oe_featureImageLayer_slug
#pragma vp_location   fragment_coloring
#pragma vp_order      0.55

#pragma import_defines(OE_TERRAIN_RENDER_IMAGERY)
#pragma import_defines(OE_TERRAIN_BLEND_IMAGERY)
#pragma import_defines(OE_IS_PICK_CAMERA)
#pragma import_defines(OE_IS_DEPTH_CAMERA)
#pragma import_defines(OE_USE_GL4)
#pragma import_defines(OE_SELF_MANAGE_LAYER_OPACITY)

#ifdef OE_USE_GL4
in vec2 oe_color_uv;
flat in uint64_t oe_color_handle;
flat in int oe_draw_order;
#else
uniform sampler2D oe_layer_tex;
uniform int oe_layer_order;
uniform mat4 oe_layer_texMatrix;
#endif

in vec4 oe_layer_tilec;
#ifdef OE_SELF_MANAGE_LAYER_OPACITY
in float oe_layer_opacity;
#else
uniform float oe_layer_opacity;
in float oe_layer_rangeOpacity;
#endif

vec4 oe_featureImageLayer_slug_incomingColor;

#define OE_SLUG_INDIRECTION_SIZE 32
#define OE_SLUG_MAX_LAYERS 64

vec4 oe_slug_texelFetch(ivec2 location)
{
#ifdef OE_USE_GL4
    return texelFetch(sampler2D(oe_color_handle), location, 0);
#else
    return texelFetch(oe_layer_tex, location, 0);
#endif
}

vec4 oe_slug_metadata(uint index, int textureWidth)
{
    int i = int(index);
    return oe_slug_texelFetch(ivec2(i % textureWidth, i / textureWidth));
}

vec4 oe_slug_curveFetch(ivec2 location, int curveRowOffset)
{
    return oe_slug_texelFetch(location + ivec2(0, curveRowOffset));
}

uvec4 oe_slug_bandFetch(ivec2 location, int bandRowOffset)
{
    vec4 value = oe_slug_texelFetch(location + ivec2(0, bandRowOffset));
    return uvec4(value + vec4(0.5));
}

uint oe_slug_calcRootCode(float y1, float y2, float y3)
{
    uint i1 = floatBitsToUint(y1) >> 31u;
    uint i2 = floatBitsToUint(y2) >> 30u;
    uint i3 = floatBitsToUint(y3) >> 29u;
    uint shift = (i2 & 2u) | (i1 & ~2u);
    shift = (i3 & 4u) | (shift & ~4u);
    return (0x2E74u >> shift) & 0x0101u;
}

vec2 oe_slug_solveHorizontal(vec4 p12, vec2 p3)
{
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float t1;
    float t2;
    if (abs(a.y) < 1.0 / 65536.0)
    {
        // Straight and axis-degenerate curves dominate stroked line work.
        // Branch before issuing the quadratic reciprocal and square root.
        t1 = p12.y * (0.5 / b.y);
        t2 = t1;
    }
    else
    {
        float reciprocal = 1.0 / a.y;
        float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
        t1 = (b.y - d) * reciprocal;
        t2 = (b.y + d) * reciprocal;
    }
    return vec2(
        (a.x * t1 - b.x * 2.0) * t1 + p12.x,
        (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

vec2 oe_slug_solveVertical(vec4 p12, vec2 p3)
{
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float t1;
    float t2;
    if (abs(a.x) < 1.0 / 65536.0)
    {
        t1 = p12.x * (0.5 / b.x);
        t2 = t1;
    }
    else
    {
        float reciprocal = 1.0 / a.x;
        float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
        t1 = (b.x - d) * reciprocal;
        t2 = (b.x + d) * reciprocal;
    }
    return vec2(
        (a.y * t1 - b.y * 2.0) * t1 + p12.y,
        (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

ivec2 oe_slug_bandLocation(
    ivec2 shapeLocation,
    uint offset,
    int textureWidthLog2)
{
    ivec2 location = ivec2(shapeLocation.x + int(offset), shapeLocation.y);
    location.y += location.x >> textureWidthLog2;
    location.x &= (1 << textureWidthLog2) - 1;
    return location;
}

float oe_slug_coverage(float xCoverage, float yCoverage, float xWeight, float yWeight)
{
    float coverage = max(
        abs(xCoverage * xWeight + yCoverage * yWeight) /
            max(xWeight + yWeight, 1.0 / 65536.0),
        min(abs(xCoverage), abs(yCoverage)));
    return clamp(coverage, 0.0, 1.0);
}

float oe_slug_render(
    vec2 renderCoord,
    vec2 pixelsPerEm,
    vec4 bandTransform,
    ivec2 shapeLocation,
    ivec2 bandMax,
    int curveRowOffset,
    int bandRowOffset,
    int textureWidthLog2)
{
    vec2 bandCoord = renderCoord * bandTransform.xy + bandTransform.zw;
    int qy = clamp(int(bandCoord.y), 0, OE_SLUG_INDIRECTION_SIZE - 1);
    int qx = clamp(int(bandCoord.x), 0, OE_SLUG_INDIRECTION_SIZE - 1);
    int bandY = int(oe_slug_bandFetch(
        shapeLocation + ivec2(qy, 0),
        bandRowOffset).r);
    int bandX = int(oe_slug_bandFetch(
        shapeLocation + ivec2(OE_SLUG_INDIRECTION_SIZE + qx, 0),
        bandRowOffset).r);

    float xCoverage = 0.0;
    float xWeight = 0.0;
    uvec2 horizontal = oe_slug_bandFetch(
        shapeLocation + ivec2(2 * OE_SLUG_INDIRECTION_SIZE + bandY, 0),
        bandRowOffset).xy;
    ivec2 horizontalLocation = oe_slug_bandLocation(
        shapeLocation, horizontal.y, textureWidthLog2);

    for (int curveIndex = 0; curveIndex < int(horizontal.x); ++curveIndex)
    {
        ivec2 curveLocation = ivec2(oe_slug_bandFetch(
            horizontalLocation + ivec2(curveIndex, 0),
            bandRowOffset).xy);
        vec4 p12 = oe_slug_curveFetch(curveLocation, curveRowOffset) -
            vec4(renderCoord, renderCoord);
        vec2 p3 = oe_slug_curveFetch(
            curveLocation + ivec2(1, 0), curveRowOffset).xy - renderCoord;

        if (max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5)
            break;

        uint code = oe_slug_calcRootCode(p12.y, p12.w, p3.y);
        if (code != 0u)
        {
            vec2 roots = oe_slug_solveHorizontal(p12, p3) * pixelsPerEm.x;
            if ((code & 1u) != 0u)
            {
                xCoverage += clamp(roots.x + 0.5, 0.0, 1.0);
                xWeight = max(xWeight, clamp(1.0 - abs(roots.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1u)
            {
                xCoverage -= clamp(roots.y + 0.5, 0.0, 1.0);
                xWeight = max(xWeight, clamp(1.0 - abs(roots.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    float yCoverage = 0.0;
    float yWeight = 0.0;
    uvec2 vertical = oe_slug_bandFetch(
        shapeLocation + ivec2(
            2 * OE_SLUG_INDIRECTION_SIZE + bandMax.y + 1 + bandX, 0),
        bandRowOffset).xy;
    ivec2 verticalLocation = oe_slug_bandLocation(
        shapeLocation, vertical.y, textureWidthLog2);

    for (int curveIndex = 0; curveIndex < int(vertical.x); ++curveIndex)
    {
        ivec2 curveLocation = ivec2(oe_slug_bandFetch(
            verticalLocation + ivec2(curveIndex, 0),
            bandRowOffset).xy);
        vec4 p12 = oe_slug_curveFetch(curveLocation, curveRowOffset) -
            vec4(renderCoord, renderCoord);
        vec2 p3 = oe_slug_curveFetch(
            curveLocation + ivec2(1, 0), curveRowOffset).xy - renderCoord;

        if (max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5)
            break;

        uint code = oe_slug_calcRootCode(p12.x, p12.z, p3.x);
        if (code != 0u)
        {
            vec2 roots = oe_slug_solveVertical(p12, p3) * pixelsPerEm.y;
            if ((code & 1u) != 0u)
            {
                yCoverage -= clamp(roots.x + 0.5, 0.0, 1.0);
                yWeight = max(yWeight, clamp(1.0 - abs(roots.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1u)
            {
                yCoverage += clamp(roots.y + 0.5, 0.0, 1.0);
                yWeight = max(yWeight, clamp(1.0 - abs(roots.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    return oe_slug_coverage(xCoverage, yCoverage, xWeight, yWeight);
}

void oe_featureImageLayer_slug(inout vec4 color)
{
#if defined(OE_IS_PICK_CAMERA) || defined(OE_IS_DEPTH_CAMERA)
    return;
#endif

#ifndef OE_TERRAIN_RENDER_IMAGERY
    return;
#endif

#ifdef OE_USE_GL4
    if (oe_color_handle == 0UL)
        return;
#endif

    vec4 header = oe_slug_texelFetch(ivec2(0, 0));
    int layerCount = min(int(header.y + 0.5), OE_SLUG_MAX_LAYERS);
    int textureWidthLog2 = int(header.z + 0.5);
    int textureWidth = int(header.w + 0.5);
    vec4 offsets = oe_slug_texelFetch(ivec2(1, 0));
    int curveRowOffset = int(offsets.x + 0.5);
    int bandRowOffset = int(offsets.y + 0.5);

    vec4 background = oe_slug_texelFetch(ivec2(2, 0));
    vec4 accumulated = vec4(background.rgb * background.a, background.a);

#ifdef OE_USE_GL4
    vec2 tileCoord = oe_color_uv;
    int layerOrder = oe_draw_order;
#else
    vec2 tileCoord = (oe_layer_texMatrix * oe_layer_tilec).st;
    int layerOrder = oe_layer_order;
#endif
    vec2 tileUnitsPerPixel = max(fwidth(tileCoord), vec2(1e-8));

    for (int layer = 0; layer < layerCount; ++layer)
    {
        uint record = uint(3 + layer * 4);
        vec4 bandTransform = oe_slug_metadata(record, textureWidth);
        vec4 shapeData = oe_slug_metadata(record + 1u, textureWidth);
        vec4 layerTransform = oe_slug_metadata(record + 2u, textureWidth);
        vec4 layerColor = oe_slug_metadata(record + 3u, textureWidth);

        if (any(lessThanEqual(abs(bandTransform.xy), vec2(1e-8))))
            continue;

        // Canvas normalizes every committed path to a local shape origin and
        // records its placement separately on the CompositeShape layer.
        float layerScale = abs(layerTransform.z) > 1e-8 ?
            layerTransform.z : 1.0;
        vec2 renderCoord = tileCoord / layerScale - layerTransform.xy;
        vec2 emsPerPixel = tileUnitsPerPixel / abs(layerScale);
        vec2 pixelsPerEm = 1.0 / emsPerPixel;

        // Reject fragments outside the authored shape bounds before entering the
        // curve loops. The indirection grid spans exactly this em-space box.
        vec2 shapeMin = -bandTransform.zw / bandTransform.xy;
        vec2 shapeMax = shapeMin +
            vec2(float(OE_SLUG_INDIRECTION_SIZE)) / bandTransform.xy;
        vec2 margin = emsPerPixel;
        if (any(lessThan(renderCoord, shapeMin - margin)) ||
            any(greaterThan(renderCoord, shapeMax + margin)))
        {
            continue;
        }

        float coverage = oe_slug_render(
            renderCoord,
            pixelsPerEm,
            bandTransform,
            ivec2(shapeData.xy + vec2(0.5)),
            ivec2(shapeData.zw + vec2(0.5)),
            curveRowOffset,
            bandRowOffset,
            textureWidthLog2);

        float sourceAlpha = coverage * layerColor.a;
        vec3 sourcePremultiplied = layerColor.rgb * sourceAlpha;
        accumulated.rgb = sourcePremultiplied +
            accumulated.rgb * (1.0 - sourceAlpha);
        accumulated.a = sourceAlpha + accumulated.a * (1.0 - sourceAlpha);
    }

    vec4 slugColor = accumulated.a > 0.0 ?
        vec4(accumulated.rgb / accumulated.a, accumulated.a) : vec4(0.0);
#ifdef OE_SELF_MANAGE_LAYER_OPACITY
    slugColor.a *= oe_layer_opacity;
#else
    slugColor.a *= oe_layer_opacity * oe_layer_rangeOpacity;
#endif

    // The engine's ordinary imagery hook sampled the packed atlas, so replace
    // that temporary result. For the first layer, reproduce its composition
    // against the color saved immediately before the engine hook.
#if defined(OE_TERRAIN_BLEND_IMAGERY) || !defined(OE_SELF_MANAGE_LAYER_OPACITY)
    if (layerOrder == 0)
    {
        color.rgb = slugColor.rgb * slugColor.a +
            oe_featureImageLayer_slug_incomingColor.rgb * (1.0 - slugColor.a);
        color.a = 1.0;
    }
    else
    {
        color = slugColor;
    }
#else
    color = slugColor;
#endif
}
