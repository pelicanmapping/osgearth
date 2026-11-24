// Occlusion test against a pyramid built by osgEarth::DepthPyramid, for culling compute shaders.
// The including shader must import the OE_LOG_DEPTH_BUFFER define, and must enable
// GL_ARB_bindless_texture when its pyramid sampler is bindless.
#ifndef OE_DEPTH_PYRAMID_GLSL
#define OE_DEPTH_PYRAMID_GLSL

// Window-space depth of a point at view distance w (> 0), matching the depth buffer's mapping.
// depthMapping is DepthPyramid::Result::depthMapping. Vertex-only log depth rasterizes at or
// beyond this value at every pixel (log(1+1/u) is convex in u = 1/w, which is what the
// rasterizer interpolates), so comparing it stays conservative.
float oe_depthPyramid_windowDepth(float w, mat4 proj, vec2 depthMapping)
{
#if defined(OE_LOG_DEPTH_BUFFER)
    float far = proj[3][2] / (proj[2][2] + 1.0);
  #if OE_LOG_DEPTH_BUFFER == 2
    return log(w*0.001 + 1.0) / log(far*0.001 + 1.0); // per-fragment log depth
  #else
    float ndc = 2.0 * log2(w + 1.0) / log2(far + 1.0) - 1.0; // per-vertex log depth
    return ndc * depthMapping.x + depthMapping.y;
  #endif
#else
    float ndc = (proj[2][2]*(-w) + proj[3][2])/w;
    return ndc * depthMapping.x + depthMapping.y;
#endif
}

// True when a volume lies behind the pyramid's depth at every pixel it can cover. The volume
// is given by its screen box [lo, hi] in normalized device coordinates and by w, the view
// distance to its nearest point. params is DepthPyramid::Result::params; a zero params.w
// means there is no pyramid. Answers false whenever it cannot tell, which includes
// orthographic projections and volumes that reach the eye plane. Samples the 2x2 texels
// spanning the box at the level where the box fits in two.
bool oe_depthPyramid_occluded(sampler2D pyramid, vec4 params, vec2 depthMapping, mat4 proj, vec2 lo, vec2 hi, float w)
{
    if (abs(params.w) < 0.5 || proj[3][3] > 0.01 || w <= 0.0)
        return false;
    vec2 a = clamp(lo*0.5 + 0.5, 0.0, 1.0) * params.xy * 0.5; // level-0 texels
    vec2 b = clamp(hi*0.5 + 0.5, 0.0, 1.0) * params.xy * 0.5;
    int level = min(int(ceil(log2(max(max(b.x - a.x, b.y - a.y), 1.0)))), int(params.z) - 1);
    ivec2 size = textureSize(pyramid, level);
    ivec2 ta = clamp(ivec2(a / exp2(float(level))), ivec2(0), size - 1);
    ivec2 tb = clamp(ivec2(b / exp2(float(level))), ivec2(0), size - 1);
    vec4 depths = vec4(texelFetch(pyramid, ta, level).r,
        texelFetch(pyramid, ivec2(tb.x, ta.y), level).r,
        texelFetch(pyramid, ivec2(ta.x, tb.y), level).r,
        texelFetch(pyramid, tb, level).r);
    bool reverse = params.w < 0.0;
    float farthest = reverse ? min(min(depths.x, depths.y), min(depths.z, depths.w)) :
        max(max(depths.x, depths.y), max(depths.z, depths.w));
    // The margin covers 24-bit depth quantization in either ordering.
    float nearest = oe_depthPyramid_windowDepth(w, proj, depthMapping);
    return reverse ? nearest < farthest - 1e-6 : nearest > farthest + 1e-6;
}

#endif
