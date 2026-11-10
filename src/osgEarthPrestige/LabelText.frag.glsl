/* Adapted from the pinned Slughorn GLFW example (1b203c1).
MIT License

Copyright (c) 2026 AlphaPixel LLC

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#version 330 core

in vec2 v_emCoord;

flat in vec4 v_color;

flat in vec4 v_bandXform; // bandScaleX/Y, bandOffsetX/Y
flat in vec4 v_shapeData; // bandTexX/Y, bandMaxX/Y

uniform sampler2D u_curveTexture;
uniform usampler2D u_bandTexture;

out vec4 fragColor;

// Hardcoded to log2(512) = 9, matching the default Atlas() constructor.
// If using Atlas(uint32_t texWidth), replace with: int(log2(atlas.getTextureWidth()))
// and pass as a uniform rather than a define.
#define TEX_WIDTH 9

// Must match slughorn::Atlas::INDIRECTION_SIZE
#define SLUG_INDIRECTION_SIZE 32

// ---------------------------------------------------------------------------
// Slug core (Lengyel 2017); identical to osgSlug's implementation.
// ---------------------------------------------------------------------------

// Classifies signed quadratic crossings using the Slug root-code table.
uint slug_CalcRootCode(float y1, float y2, float y3) {
    uint i1 = floatBitsToUint(y1) >> 31u;
    uint i2 = floatBitsToUint(y2) >> 30u;
    uint i3 = floatBitsToUint(y3) >> 29u;

    uint shift = (i2 & 2u) | (i1 & ~2u);
    shift = (i3 & 4u) | (shift & ~4u);

    return ((0x2E74u >> shift) & 0x0101u);
}

// Solves horizontal-ray intersections, falling back to a linear root for degenerate curves.
vec2 slug_SolveHorizPoly(vec4 p12, vec2 p3) {
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.y;
    float rb = 0.5 / b.y;
    float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
    float t1 = (b.y - d) * ra;
    float t2 = (b.y + d) * ra;
    if(abs(a.y) < 1.0 / 65536.0) { t1 = p12.y * rb; t2 = t1; }
    return vec2(
        (a.x * t1 - b.x * 2.0) * t1 + p12.x,
        (a.x * t2 - b.x * 2.0) * t2 + p12.x
    );
}

// Solves vertical-ray intersections with the same degeneracy policy.
vec2 slug_SolveVertPoly(vec4 p12, vec2 p3) {
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.x;
    float rb = 0.5 / b.x;
    float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
    float t1 = (b.x - d) * ra;
    float t2 = (b.x + d) * ra;
    if(abs(a.x) < 1.0 / 65536.0) { t1 = p12.x * rb; t2 = t1; }
    return vec2(
        (a.y * t1 - b.y * 2.0) * t1 + p12.y,
        (a.y * t2 - b.y * 2.0) * t2 + p12.y
    );
}

// Converts a shape-relative band address into a texture coordinate, including row crossings.
ivec2 slug_CalcBandLoc(ivec2 glyphLoc, uint offset) {
    ivec2 bandLoc = ivec2(glyphLoc.x + int(offset), glyphLoc.y);
    bandLoc.y += bandLoc.x >> TEX_WIDTH;
    bandLoc.x &= (1 << TEX_WIDTH) - 1;
    return bandLoc;
}

// 2026-08-30: re-derive a curve's second texel location the same way slug_CalcBandLoc()
// re-derives a band fetch's row, instead of flat-adding to curveLoc.x. Packing today always
// aligns a curve's 2 texels to start at an even X so they never straddle a row, making this a
// no-op -- but endpoint-shared curve packing (planned) removes that alignment, so a curve's
// second texel CAN legitimately land at the start of the next row.
ivec2 slug_CalcCurveLoc(ivec2 curveLoc, int offset) {
    ivec2 loc = ivec2(curveLoc.x + offset, curveLoc.y);
    loc.y += loc.x >> TEX_WIDTH;
    loc.x &= (1 << TEX_WIDTH) - 1;
    return loc;
}

// Combines orthogonal analytic coverage estimates into one clamped alpha.
float slug_CalcCoverage(float xcov, float ycov, float xwgt, float ywgt) {
    float coverage = max(
        abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, 1.0 / 65536.0),
        min(abs(xcov), abs(ycov))
    );
    return clamp(coverage, 0.0, 1.0);
}

// Evaluates the pinned atlas format at a glyph-local coordinate, with derivative-based antialiasing.
float slug_Render(vec2 renderCoord, vec4 bandTransform, ivec2 glyphLoc, ivec2 bandMax, vec2 pixelsPerEm) {


    // O(1) band index via indirection tables (2 fetches per axis).
    int qY = clamp(int(renderCoord.y * bandTransform.y + bandTransform.w), 0, SLUG_INDIRECTION_SIZE - 1);
    int qX = clamp(int(renderCoord.x * bandTransform.x + bandTransform.z), 0, SLUG_INDIRECTION_SIZE - 1);
    int bandY = int(texelFetch(u_bandTexture, slug_CalcBandLoc(glyphLoc, uint(qY)), 0).r);
    int bandX = int(texelFetch(u_bandTexture, slug_CalcBandLoc(glyphLoc, uint(SLUG_INDIRECTION_SIZE + qX)), 0).r);

    // Horizontal bands -- headers at glyphLoc + 2*IS + bandY
    float xcov = 0.0, xwgt = 0.0;
    uvec2 hbandData = texelFetch(u_bandTexture,
        slug_CalcBandLoc(glyphLoc, uint(2 * SLUG_INDIRECTION_SIZE + bandY)), 0).xy;

    for(int ci = 0; ci < int(hbandData.x); ci++) {
        // Re-derive this fetch's row every iteration (instead of computing hbandLoc once
        // and flat-adding ci to its X) so a band's curve-index list can span more than one
        // texture row -- removes the old "count > texWidth" hard limit on shape complexity.
        // Costs up to ~10us on complex scenes vs. the single-fetch version; worth it.
        ivec2 hbandLoc = slug_CalcBandLoc(glyphLoc, hbandData.y + uint(ci));
        ivec2 curveLoc = ivec2(texelFetch(u_bandTexture, hbandLoc, 0).xy);
        vec4 p12 = texelFetch(u_curveTexture, curveLoc, 0) - vec4(renderCoord, renderCoord);

        // 2026-08-31: same re-derive-per-fetch pattern as slug_CalcBandLoc above, now applied to
        // a curve's own second texel -- needed once packTextures() (slughorn.cpp) started
        // sharing texels between connected curves (Lengyel's Slug convention: a chain of N
        // connected curves costs N+1 texels instead of 2N), which packs densely with no
        // per-curve row alignment. Adds a handful of ALU ops per curve iteration; negligible
        // next to the texture fetch and sqrt-based polynomial solve already in this loop.
        // Shipped as the default (no opt-in flag) -- lossless, and verified bit-identical
        // against the pre-sharing renderer at real CJK-atlas scale.
        vec2 p3 = texelFetch(u_curveTexture, slug_CalcCurveLoc(curveLoc, 1), 0).xy - renderCoord;

        if(max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5) break;

        uint code = slug_CalcRootCode(p12.y, p12.w, p3.y);
        if(code != 0u) {
            vec2 r = slug_SolveHorizPoly(p12, p3) * pixelsPerEm.x;
            if((code & 1u) != 0u) {
                xcov += clamp(r.x + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if(code > 1u) { xcov -= clamp(r.y + 0.5, 0.0, 1.0); xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0)); }
        }
    }

    // Vertical bands -- headers at glyphLoc + 2*IS + numHBands + bandX
    float ycov = 0.0, ywgt = 0.0;
    uvec2 vbandData = texelFetch(u_bandTexture,
        slug_CalcBandLoc(glyphLoc, uint(2 * SLUG_INDIRECTION_SIZE + bandMax.y + 1 + bandX)), 0).xy;

    for(int ci = 0; ci < int(vbandData.x); ci++) {
        // See the matching comment in the horizontal-band loop above.
        ivec2 vbandLoc = slug_CalcBandLoc(glyphLoc, vbandData.y + uint(ci));
        ivec2 curveLoc = ivec2(texelFetch(u_bandTexture, vbandLoc, 0).xy);
        vec4 p12 = texelFetch(u_curveTexture, curveLoc, 0) - vec4(renderCoord, renderCoord);

        // See the matching endpoint-sharing tradeoff comment in the horizontal-band loop above.
        vec2 p3 = texelFetch(u_curveTexture, slug_CalcCurveLoc(curveLoc, 1), 0).xy - renderCoord;

        if(max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5) break;

        uint code = slug_CalcRootCode(p12.x, p12.z, p3.x);
        if(code != 0u) {
            vec2 r = slug_SolveVertPoly(p12, p3) * pixelsPerEm.y;
            if((code & 1u) != 0u) {
                ycov -= clamp(r.x + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if(code > 1u) { ycov += clamp(r.y + 0.5, 0.0, 1.0); ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0)); }
        }
    }

    return slug_CalcCoverage(xcov, ycov, xwgt, ywgt);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
flat in vec4 v_halo;
flat in float v_haloWidth, v_haloSoftness;
flat in float v_opacity;
flat in float v_pixelsPerEm;
uniform sampler2D u_distance, u_distanceMap;
uniform bool u_hasDistance;
// The standalone prototype has no VisibleLayer; retain full opacity when the uniform is absent.
uniform float oe_VisibleLayer_opacityUniform = 1.0;
uniform int u_textPass; // 0: halo coverage, 1: analytic fill; all halo bins render before all fill bins.

// Separates halo and fill coverage so neighboring glyph halos never darken letter interiors.
void main()
{
    ivec2 location = ivec2(v_shapeData.xy);
    ivec2 bands = ivec2(v_shapeData.zw);
    // Derivatives precede coverage-dependent branches; pass/style decisions are uniform over each glyph.
    vec2 emsPerPixel = fwidth(v_emCoord);
    vec2 pixelsPerEm = 1.0 / emsPerPixel;
    float coverage;
    vec4 color;
    if (u_textPass == 1)
    {
        coverage = slug_Render(v_emCoord, v_bandXform, location, bands, pixelsPerEm);
        color = v_color;
    }
    else
    {
        if (v_haloWidth <= 0.0 || v_halo.a <= 0.0) discard;
        color = v_halo;
        // 64 texels/em, 16-texel spread: +/-0.25 em. Keep the entire halo/AA ramp inside the stored range.
        // Above 32 displayed pixels/em, field quantization becomes visible; retain analytic halos.
        vec4 mapping = u_hasDistance ? texelFetch(u_distanceMap, location, 0) : vec4(0.0);
        bool cached = mapping.x > 0.0 && v_pixelsPerEm <= 32.0 &&
            v_haloWidth + 1.0 <= 0.25 * v_pixelsPerEm;
        if (cached)
        {
            int mapWidth = textureSize(u_distanceMap, 0).x;
            int address = location.y * mapWidth + location.x + 1;
            vec4 bounds = texelFetch(u_distanceMap, ivec2(address % mapWidth, address / mapWidth), 0);
            vec2 uv = clamp(v_emCoord * mapping.xy + mapping.zw, bounds.xy, bounds.zw);
            // FreeType encodes edge=128 and signed distance/spread=(byte-128)/128.
            // Convert bake pixels to em: (byte - 128) / 128 * spread(16) / pixelsPerEm(64).
            float distanceEm = (texture(u_distance, uv).r * 255.0 - 128.0) / 512.0;
            float distancePx = distanceEm * v_pixelsPerEm;
            float ramp = max(fwidth(distancePx), 0.0001);
#ifdef PRESTIGE_SOFT_HALOS
            coverage = v_haloSoftness > 0.0 ?
                smoothstep(-v_haloWidth - 0.5 * ramp,
                    -v_haloWidth * (1.0 - v_haloSoftness) + 0.5 * ramp, distancePx) :
                clamp((distancePx + v_haloWidth) / ramp + 0.5, 0.0, 1.0);
#else
            coverage = clamp((distancePx + v_haloWidth) / ramp + 0.5, 0.0, 1.0);
#endif
        }
        else
        {
            coverage = slug_Render(v_emCoord, v_bandXform, location, bands, pixelsPerEm);
            if (coverage < 1.0)
            {
                vec2 directions[8] = vec2[8](vec2(1,0), vec2(-1,0), vec2(0,1), vec2(0,-1),
                    vec2(0.707,0.707), vec2(-0.707,0.707), vec2(0.707,-0.707), vec2(-0.707,-0.707));
#ifdef PRESTIGE_SOFT_HALOS
                if (v_haloSoftness <= 0.0)
#endif
                {
                    // Preserve the existing crisp fallback's eight samples.
                    vec2 stepEm = emsPerPixel * v_haloWidth;
                    for (int i = 0; i < 8; ++i)
                        coverage = max(coverage, slug_Render(v_emCoord + directions[i] * stepEm,
                            v_bandXform, location, bands, pixelsPerEm));
                }
#ifdef PRESTIGE_SOFT_HALOS
                else
                {
                    // Integrate four weighted dilations to approximate the same smoothstep falloff without a cache.
                    // Costs 32 shifted analytic samples; never silently switch a soft style to a crisp outline.
                    float weights[4] = float[4](0.15625, 0.34375, 0.34375, 0.15625);
                    float fill = coverage;
                    coverage = 0.0;
                    for (int ring = 0; ring < 4; ++ring)
                    {
                        float radius = v_haloWidth * (1.0 - v_haloSoftness + v_haloSoftness * (float(ring) + 0.5) / 4.0);
                        vec2 stepEm = emsPerPixel * radius;
                        float expanded = fill;
                        for (int i = 0; i < 8; ++i)
                            expanded = max(expanded, slug_Render(v_emCoord + directions[i] * stepEm,
                                v_bandXform, location, bands, pixelsPerEm));
                        coverage += weights[ring] * expanded;
                    }
                }
#endif
            }
        }
    }
    float alpha = coverage * color.a * v_opacity * clamp(oe_VisibleLayer_opacityUniform, 0.0, 1.0);
    fragColor = vec4(color.rgb * alpha, alpha);
    if (fragColor.a < 0.001) discard;
}
