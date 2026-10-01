// Shared screen-size policy for ordinary instances and members of grouped geometry.
#ifndef OE_CHONK_SCREEN_SIZE
#define OE_CHONK_SCREEN_SIZE

// Calculates the clip-space minimum bounding box of a view-space bounding sphere
void compute_clip_mbb(in vec4 p_view, in float r, in mat4 proj, out vec4 LL, out vec4 UR)
{
    vec4 temp;
    temp = proj * (p_view + vec4(-r, -r, -r, 0)); temp /= temp.w;
    LL = temp; UR = temp;
    temp = proj * (p_view + vec4(-r, -r, +r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
    temp = proj * (p_view + vec4(-r, +r, -r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
    temp = proj * (p_view + vec4(-r, +r, +r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
    temp = proj * (p_view + vec4(+r, -r, -r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
    temp = proj * (p_view + vec4(+r, -r, +r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
    temp = proj * (p_view + vec4(+r, +r, -r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
    temp = proj * (p_view + vec4(+r, +r, +r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
}

// Literal-pixel visibility is opt-in; ordinary Chonk clients retain their authored SSE coefficients.
// Detail handovers can still use normalizedBudget without shrinking the final visibility error.
float oe_chonk_visibility_cutoff(float normalizedBudget, float pixelError, float authoredPixels, float lodScale)
{
#if defined(OE_CHONK_SSE_LOD_ONLY)
    // A range-limited population uses quality for representation, never for disappearance.
    return 0.0;
#elif defined(OE_CHONK_SSE_PIXEL_CUTOFF)
    return max(pixelError,authoredPixels)*lodScale;
#else
    return normalizedBudget*authoredPixels*lodScale;
#endif
}

// Matches the ordinary culler's lower LOD cutoff, including its minimum one-pixel fade width.
float oe_chonk_size_fade(float pixels, float cutoff, float transition)
{
    float pad = pixels*transition;
    if (pixels < cutoff-pad) return 0.0;
    return clamp(1.0-(cutoff-pixels)/max(pad,1.0),0.0,1.0);
}

// Uses the member's sphere, never the cluster radius. Eye-plane intersections conservatively retain detail.
float oe_chonk_member_fade(vec4 center, float radius, mat4 projection, vec2 viewport,
    float cutoff, float transition, vec2 range, float lodScale)
{
    float fade = 1.0;
    if (cutoff > 0.0 && (projection[3][3] >= 0.01 || center.z+radius < 0.0))
    {
        vec4 low, high;
        compute_clip_mbb(center,radius,projection,low,high);
        vec2 pixels = 0.5*(high.xy-low.xy)*viewport;
        fade = oe_chonk_size_fade(min(pixels.x,pixels.y),cutoff,transition);
    }
    if (range.y > range.x)
        fade *= clamp((range.y-length(center.xyz)*lodScale)/(range.y-range.x),0.0,1.0);
    return fade < 0.1 ? 0.0 : fade;
}
#endif
