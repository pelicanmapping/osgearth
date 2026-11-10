#version 430 core
// One bounded workgroup per road. Project once, fit each glyph once, share with both drawing passes.
layout(local_size_x=64) in;
layout(rgba32f, binding=0) writeonly uniform image2D fittedGlyphs;
uniform sampler2D pathHeaders, pathVertices, pathOffsets;
uniform mat4 cameraLinearProjection;
uniform vec3 eyeHigh, eyeLow, viewportDensity;
uniform bool geocentric;
shared vec2 points[256];
shared float lengths[256];
shared vec4 fitted[256];
shared uint invalid;
shared bool reversed;
shared uint vertexCount;

// All private path tables use 512-wide rows and exact integer texel addresses.
ivec2 address(uint index) { return ivec2(index & 511u, index >> 9u); }

// Binary search bounds the lookup to eight steps for the maximum 256-vertex path.
uint segment(float distance, uint count)
{
    uint lo = 1u, hi = count - 1u;
    for (uint step = 0u; step < 8u && lo < hi; ++step)
    {
        uint mid = (lo + hi) >> 1u;
        if (lengths[mid] < distance) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

// Degenerate projected edges retain the neighboring direction instead of producing NaNs.
vec2 direction(vec2 delta, vec2 fallback)
{
    float magnitude = length(delta);
    return magnitude > 0.0001 ? delta / magnitude : fallback;
}

// Performs only projection and glyph fitting; eligibility and collision policy remain on the background worker.
void main()
{
    uint lane = gl_LocalInvocationID.x;
    uvec4 header = uvec4(texelFetch(pathHeaders, address(gl_WorkGroupID.x), 0));
    uint vertexStart = header.x, count = header.y, glyphStart = header.z, glyphCount = header.w;
    if (lane == 0u) invalid = 0u;
    barrier();
    for (uint i = lane; i < count; i += 64u)
    {
        vec3 high = texelFetch(pathVertices, address((vertexStart + i) * 2u), 0).xyz;
        vec3 low = texelFetch(pathVertices, address((vertexStart + i) * 2u + 1u), 0).xyz;
        vec3 relative = (high - eyeHigh) + (low - eyeLow);
        vec4 clip = cameraLinearProjection * vec4(relative, 1.0);
        vec3 normal = high * vec3(1.0, 1.0, 1.006739496742);
        bool visible = clip.w > 0.0 && !(any(isnan(clip)) || any(isinf(clip)));
        if (geocentric && dot(normal, -relative) <= 0.0) visible = false;
        points[i] = visible ? (clip.xy / clip.w + 1.0) * viewportDensity.xy * 0.5 : vec2(0.0);
        if (!visible) atomicOr(invalid, 1u);
    }
    barrier();
    float width = texelFetch(pathVertices, address(vertexStart * 2u), 0).w * viewportDensity.z;
    if (lane == 0u)
    {
        // Serial prefix avoids eight shared-memory scan barriers; road lengths are bounded and typically small.
        uint retained = 1u;
        for (uint i = 1u; i < count; ++i)
        {
            vec2 delta = points[i] - points[retained - 1u];
            if (dot(delta, delta) > 0.0001) points[retained++] = points[i];
        }
        if (retained < 2u) { invalid = 1u; points[1] = points[0]; }
        count = max(2u, retained);
        vertexCount = count;
        lengths[0] = 0.0;
        for (uint i = 1u; i < count; ++i) lengths[i] = lengths[i - 1u] + length(points[i] - points[i - 1u]);
        if (isnan(lengths[count - 1u]) || isinf(lengths[count - 1u]) ||
            lengths[count - 1u] < width + 8.0 * viewportDensity.z) invalid = 1u;
        uint mid = segment(lengths[count - 1u] * 0.5, count);
        vec2 tangent = direction(points[mid] - points[mid - 1u], vec2(1.0, 0.0));
        reversed = tangent.x < -0.01 || (abs(tangent.x) <= 0.01 && tangent.y < 0.0);
    }
    barrier();
    count = vertexCount;
    for (uint g = lane; g < glyphCount; g += 64u)
    {
        float total = lengths[count - 1u];
        float distance = (total - width) * 0.5 +
            texelFetch(pathOffsets, address(glyphStart + g), 0).x * viewportDensity.z;
        if (reversed) distance = total - distance;
        if (distance < 4.0 * viewportDensity.z || distance > total - 4.0 * viewportDensity.z)
            atomicOr(invalid, 1u);
        uint b = segment(distance, count), a = b - 1u;
        float fraction = clamp((distance - lengths[a]) / max(0.0001, lengths[b] - lengths[a]), 0.0, 1.0);
        vec2 tangent = direction(points[b] - points[a], vec2(1.0, 0.0));
        vec2 left = tangent, right = tangent;
        if (a > 0u)
        {
            vec2 before = direction(points[a] - points[a - 1u], tangent);
            if (dot(before, tangent) > -0.99) left = normalize(before + tangent);
        }
        if (b + 1u < count)
        {
            vec2 after = direction(points[b + 1u] - points[b], tangent);
            if (dot(after, tangent) > -0.99) right = normalize(after + tangent);
        }
        // Interpolated vertex tangents avoid angle steps when a glyph crosses a polyline vertex during zoom.
        vec2 smoothDirection = normalize(mix(left, right, fraction)) * (reversed ? -1.0 : 1.0);
        fitted[g] = vec4(mix(points[a], points[b], fraction), smoothDirection);
    }
    barrier();
    for (uint g = lane; g < glyphCount; g += 64u)
        imageStore(fittedGlyphs, address(glyphStart + g), invalid == 0u ? fitted[g] : vec4(0.0));
}
