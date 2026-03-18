// Slug text rendering shaders (Lengyel 2017)
// Renders text directly from quadratic Bezier curve outlines on the GPU.

out vec2 slug_texcoord;
flat out vec4 slug_banding;
flat out ivec2 slug_glyphLoc;
flat out ivec2 slug_bandMax;

#pragma vp_name       SlugText Vertex Shader
#pragma vp_entryPoint oe_SlugText_VS
#pragma vp_location   vertex_model

void oe_SlugText_VS(inout vec4 vertex)
{
    slug_texcoord = gl_MultiTexCoord0.xy;
    slug_banding = gl_MultiTexCoord1;
    slug_glyphLoc = ivec2(gl_MultiTexCoord2.xy);
    slug_bandMax = ivec2(gl_MultiTexCoord2.zw);
}


[break]
// Fragment shader: Slug algorithm ported from HLSL reference (Lengyel 2017)

#pragma vp_name       SlugText Fragment Shader
#pragma vp_entryPoint oe_SlugText_FS
#pragma vp_location   fragment_coloring

uniform sampler2D slug_curveTexture;
uniform usampler2D slug_bandTexture;
uniform vec4 slug_color;

in vec2 slug_texcoord;
flat in vec4 slug_banding;
flat in ivec2 slug_glyphLoc;
flat in ivec2 slug_bandMax;

#define kLogBandTextureWidth 12

uint slug_CalcRootCode(float y1, float y2, float y3)
{
    uint i1 = floatBitsToUint(y1) >> 31u;
    uint i2 = floatBitsToUint(y2) >> 30u;
    uint i3 = floatBitsToUint(y3) >> 29u;

    uint shift = (i2 & 2u) | (i1 & ~2u);
    shift = (i3 & 4u) | (shift & ~4u);

    return ((0x2E74u >> shift) & 0x0101u);
}

vec2 slug_SolveHorizPoly(vec4 p12, vec2 p3)
{
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.y;
    float rb = 0.5 / b.y;

    float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
    float t1 = (b.y - d) * ra;
    float t2 = (b.y + d) * ra;

    if (abs(a.y) < 1.0 / 65536.0) { t1 = p12.y * rb; t2 = t1; }

    return vec2(
        (a.x * t1 - b.x * 2.0) * t1 + p12.x,
        (a.x * t2 - b.x * 2.0) * t2 + p12.x
    );
}

vec2 slug_SolveVertPoly(vec4 p12, vec2 p3)
{
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.x;
    float rb = 0.5 / b.x;

    float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
    float t1 = (b.x - d) * ra;
    float t2 = (b.x + d) * ra;

    if (abs(a.x) < 1.0 / 65536.0) { t1 = p12.x * rb; t2 = t1; }

    return vec2(
        (a.y * t1 - b.y * 2.0) * t1 + p12.y,
        (a.y * t2 - b.y * 2.0) * t2 + p12.y
    );
}

ivec2 slug_CalcBandLoc(ivec2 glyphLoc, uint offset)
{
    ivec2 bandLoc = ivec2(glyphLoc.x + int(offset), glyphLoc.y);
    bandLoc.y += bandLoc.x >> kLogBandTextureWidth;
    bandLoc.x &= (1 << kLogBandTextureWidth) - 1;
    return bandLoc;
}

float slug_CalcCoverage(float xcov, float ycov, float xwgt, float ywgt)
{
    float coverage = max(
        abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, 1.0 / 65536.0),
        min(abs(xcov), abs(ycov))
    );
    return clamp(coverage, 0.0, 1.0);
}

float slug_Render(vec2 renderCoord, vec4 bandTransform, ivec2 glyphLoc, ivec2 bandMax)
{
    int curveIndex;

    vec2 emsPerPixel = fwidth(renderCoord);
    vec2 pixelsPerEm = 1.0 / emsPerPixel;

    int bandMaxY = bandMax.y & 0x00FF;
    int bandMaxX = bandMax.x;

    ivec2 bandIndex = clamp(
        ivec2(renderCoord * bandTransform.xy + bandTransform.zw),
        ivec2(0, 0),
        ivec2(bandMaxX, bandMaxY)
    );

    float xcov = 0.0;
    float xwgt = 0.0;

    // Horizontal band
    uvec2 hbandData = texelFetch(slug_bandTexture, ivec2(glyphLoc.x + bandIndex.y, glyphLoc.y), 0).xy;
    ivec2 hbandLoc = slug_CalcBandLoc(glyphLoc, hbandData.y);

    for (curveIndex = 0; curveIndex < int(hbandData.x); curveIndex++)
    {
        ivec2 curveLoc = ivec2(texelFetch(slug_bandTexture, ivec2(hbandLoc.x + curveIndex, hbandLoc.y), 0).xy);

        vec4 p12 = texelFetch(slug_curveTexture, curveLoc, 0) - vec4(renderCoord, renderCoord);
        vec2 p3 = texelFetch(slug_curveTexture, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;

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

    float ycov = 0.0;
    float ywgt = 0.0;

    // Vertical band
    uvec2 vbandData = texelFetch(slug_bandTexture, ivec2(glyphLoc.x + bandMaxY + 1 + bandIndex.x, glyphLoc.y), 0).xy;
    ivec2 vbandLoc = slug_CalcBandLoc(glyphLoc, vbandData.y);

    for (curveIndex = 0; curveIndex < int(vbandData.x); curveIndex++)
    {
        ivec2 curveLoc = ivec2(texelFetch(slug_bandTexture, ivec2(vbandLoc.x + curveIndex, vbandLoc.y), 0).xy);
        vec4 p12 = texelFetch(slug_curveTexture, curveLoc, 0) - vec4(renderCoord, renderCoord);
        vec2 p3 = texelFetch(slug_curveTexture, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;

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

void oe_SlugText_FS(inout vec4 color)
{
    float coverage = slug_Render(slug_texcoord, slug_banding, slug_glyphLoc, slug_bandMax);
    if (coverage < 0.001) discard;
    color = slug_color * coverage;
}
