#pragma include RexEngine.GL4.glsl
#pragma vp_name Rex Terrain SDK
/**
 * SDK functions for the Rex engine.
 * Declare and call these from any shader that runs on the terrain.
 */

// Stage global
vec4 oe_layer_tilec;
vec4 oe_tile_key;

#if !defined(VP_STAGE_FRAGMENT)

uniform vec2 oe_tile_elevTexelCoeff;

// Gets the coordinate to use for elevation sampling.
vec2 oe_terrain_getElevationCoords(in vec2 uv)
{
    return uv
        * oe_tile_elevTexelCoeff.x * oe_tile[oe_tileID].elevMat[0][0]     // scale
        + oe_tile_elevTexelCoeff.x * oe_tile[oe_tileID].elevMat[3].st     // bias
        + oe_tile_elevTexelCoeff.y;
}

vec2 oe_terrain_getElevationCoords()
{
    return oe_layer_tilec.st
        * oe_tile_elevTexelCoeff.x * oe_tile[oe_tileID].elevMat[0][0]     // scale
        + oe_tile_elevTexelCoeff.x * oe_tile[oe_tileID].elevMat[3].st     // bias
        + oe_tile_elevTexelCoeff.y;
}

// reverse the operation performed by oe_terrain_getElevationCoords(vec2)
vec2 oe_terrain_unscaleElevationCoords(in vec2 uv)
{
    return (uv - oe_tile_elevTexelCoeff.y - oe_tile_elevTexelCoeff.x * oe_tile[oe_tileID].elevMat[3].st)
        / (oe_tile_elevTexelCoeff.x * oe_tile[oe_tileID].elevMat[0][0]);
}

// Gets the handle to use for elevation sampling
uint64_t oe_terrain_getElevationHandle()
{
    int index = oe_tile[oe_tileID].elevIndex;
    return (index >= 0) ? oe_terrain_tex[index] : 0;
}

// Sample the elevation data at a UV tile coordinate.
float oe_terrain_getElevation(in vec2 uv)
{
    // Texel-level scale and bias allow us to sample the elevation texture
    // on texel center instead of edge.
    // If a min and max elev are set, we use them to decode the 16-bit elevation
    // value. If not, assume a 32-bit single channel float value.
    int index = oe_tile[oe_tileID].elevIndex;
    if (index >= 0)
    {
        vec2 uv_scaledBiased = oe_terrain_getElevationCoords(uv);
        float encoded = texture(sampler2D(oe_terrain_tex[index]), uv_scaledBiased).r;
        float minh = oe_tile[oe_tileID].elevMin;
        float maxh = oe_tile[oe_tileID].elevMax;
        return minh > maxh ? encoded : mix(minh, maxh, encoded);
    }
    return 0.0;
}

vec3 oe_terrain_getPoint(in vec2 uv)
{
    float h = 0.0;
    int index = oe_tile[oe_tileID].elevIndex;
    vec2 clamped_uv = uv;
    if (index >= 0)
    {
        vec2 uv_scaledBiased = oe_terrain_getElevationCoords(uv);
        uv_scaledBiased = clamp(uv_scaledBiased, vec2(0.0), vec2(1.0)); // avoid sampling outside the texture
        float encoded = texture(sampler2D(oe_terrain_tex[index]), uv_scaledBiased).r;
        float minh = oe_tile[oe_tileID].elevMin;
        float maxh = oe_tile[oe_tileID].elevMax;
        h = minh > maxh ? encoded : mix(minh, maxh, encoded);
        clamped_uv = oe_terrain_unscaleElevationCoords(uv_scaledBiased);
    }
    float tileSpan = oe_tile[oe_tileID].tileSpan;
    return vec3(clamped_uv.s * tileSpan, clamped_uv.t * tileSpan, h);
}

// Read the elevation at the build-in tile coordinates (convenience)
float oe_terrain_getElevation()
{
    return oe_terrain_getElevation(oe_layer_tilec.st);
}

vec2 oe_terrain_getElevationMinMax()
{
    return vec2(oe_tile[oe_tileID].elevMin, oe_tile[oe_tileID].elevMax);
}

// Read the normal vector and curvature at resolved UV tile coordinates.
vec4 oe_terrain_getNormalAndCurvature(in vec2 uv_scaledBiased)
{
    int index = oe_tile[oe_tileID].normalIndex;
    if (index >= 0)
    {
        vec4 n = texture(sampler2D(oe_terrain_tex[index]), uv_scaledBiased);
        n.xyz = n.xyz * 2.0 - 1.0;
        float curv = n.z;
        n.z = 1.0 - abs(n.x) - abs(n.y);
        // unnecessary since Z is never < 0:
        //float t = clamp(-n.z, 0, 1);
        //n.x += (n.x > 0)? -t : t;
        //n.y += (n.y > 0)? -t : t;
        return vec4(normalize(n.xyz), curv);
    }
    else return vec4(0.0, 0.0, 1.0, 0.0);
}

vec4 oe_terrain_getNormalAndCurvature()
{
    vec2 uv_scaledBiased = oe_layer_tilec.st
        * oe_tile_elevTexelCoeff.x * oe_tile[oe_tileID].normalMat[0][0]
        + oe_tile_elevTexelCoeff.x * oe_tile[oe_tileID].normalMat[3].st
        + oe_tile_elevTexelCoeff.y;

    return oe_terrain_getNormalAndCurvature(uv_scaledBiased);
}

uint64_t oe_terrain_getNormalHandle()
{
    int index = oe_tile[oe_tileID].normalIndex;
    return (index >= 0) ? oe_terrain_tex[index] : 0;
}

vec2 oe_terrain_getNormalCoords()
{
    return oe_layer_tilec.st
        * oe_tile_elevTexelCoeff.x * oe_tile[oe_tileID].normalMat[0][0]
        + oe_tile_elevTexelCoeff.x * oe_tile[oe_tileID].normalMat[3].st
        + oe_tile_elevTexelCoeff.y;
}

#endif // !VP_STAGE_FRAGMENT

// Sample the elevation data at a UV tile coordinate.
float oe_terrain_getElevation(in uint64_t handle, in vec2 uv)
{
    // Texel-level scale and bias allow us to sample the elevation texture
    // on texel center instead of edge.
    // If a min and max elev are set, we use them to decode the 16-bit elevation
    // value. If not, assume a 32-bit single channel float value.
    if (handle >= 0)
    {
        float encoded = texture(sampler2D(handle), uv).r;
        float minh = oe_tile[oe_tileID].elevMin;
        float maxh = oe_tile[oe_tileID].elevMax;
        return minh > maxh ? encoded : mix(minh, maxh, encoded);
    }
    return 0.0;
}

vec4 oe_terrain_getNormalAndCurvature(in uint64_t handle, in vec2 uv)
{
    vec4 n = texture(sampler2D(handle), uv);
    n.xyz = n.xyz*2.0 - 1.0;
    float curv = n.z;
    n.z = 1.0 - abs(n.x) - abs(n.y);
    // unnecessary since Z is never < 0:
    //float t = clamp(-n.z, 0, 1);
    //n.x += (n.x > 0)? -t : t;
    //n.y += (n.y > 0)? -t : t;
    return vec4(normalize(n.xyz), curv);
}

/**
 * Scales repeating texture coordinate such that they are [0..1]
 * at a specific reference tile LOD. 
 */
vec2 oe_terrain_scaleCoordsToRefLOD(in vec2 tc, in float refLOD)
{
    float dL = oe_tile_key.z - refLOD;
    float factor = exp2(dL);
    float invFactor = 1.0/factor;
    vec2 result = tc * vec2(invFactor);

    vec2 a = floor(oe_tile_key.xy * invFactor);
    vec2 b = a * factor;
    vec2 c = b + factor;

    float m = floor(clamp(factor,0.0,1.0)); // if factor>=1.0
    result += m*(oe_tile_key.xy-b)/(c-b);

    return result;
}

/**
 * Scales repeating texture coordinate such that they are [0..1]
 * at a specific reference tile LOD.
 */
vec4 oe_terrain_scaleCoordsAndTileKeyToRefLOD(in vec2 tc, in float refLOD)
{
    float dL = oe_tile_key.z - refLOD;
    float factor = exp2(dL);
    float invFactor = 1.0 / factor;
    vec2 result = tc * vec2(invFactor);

    vec2 a = floor(oe_tile_key.xy * invFactor);
    vec2 b = a * factor;
    vec2 c = b + factor;

    float m = floor(clamp(factor, 0.0, 1.0)); // if factor>=1.0
    result += m * (oe_tile_key.xy - b) / (c - b);

    return vec4(result, a);
}
