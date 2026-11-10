#version 330 core
layout(location=1) in float haloSoftness;
layout(location=5) in float pathIndex;
layout(location=4) in vec3 pathTangent;
layout(location=6) in vec3 anchorHigh;
layout(location=7) in vec3 anchorLow;
layout(location=8) in vec4 pixelBounds;
layout(location=9) in vec4 emBounds;
layout(location=10) in vec4 bandTransform;
layout(location=11) in vec4 bandAddress;
layout(location=12) in vec4 fillColor;
layout(location=13) in vec4 haloColor;
layout(location=14) in vec4 parameters;
layout(location=15) in vec4 fade;
uniform float frameTime, adoptionTime;
uniform mat4 cameraLinearProjection;
uniform vec3 eyeHigh, eyeLow;
uniform vec3 viewportDensity;
uniform bool geocentric;
uniform bool gpuPaths = false;
uniform sampler2D pathGlyphs;
out vec2 v_emCoord;
flat out vec4 v_bandXform, v_shapeData, v_color, v_halo;
flat out float v_haloWidth, v_haloSoftness;
flat out float v_opacity;
flat out float v_pixelsPerEm;

// Reprojects accepted anchors with THIS frame's camera; placement age never freezes screen positions.
void main()
{
    vec2 corners[4] = vec2[4](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
    vec2 corner = corners[gl_VertexID];
    vec3 relative = (anchorHigh - eyeHigh) + (anchorLow - eyeLow);
    vec4 clip = cameraLinearProjection * vec4(relative, 1.0);
    bool visible = clip.w > 0.0 && abs(clip.x) <= clip.w && abs(clip.y) <= clip.w;
    // Surface-anchor WGS84 horizon approximation; elevated-anchor occlusion is deferred.
    vec3 normal = anchorHigh * vec3(1.0, 1.0, 1.006739496742);
    if (geocentric && dot(normal, -relative) <= 0.0) visible = false;
    vec2 offset = mix(pixelBounds.xy, pixelBounds.zw, corner) * viewportDensity.z;
    float angle = parameters.y;
    vec2 direction = vec2(cos(angle), sin(angle));
    if (gpuPaths && pathIndex >= 0.0)
    {
        int index = int(pathIndex);
        vec4 fitted = texelFetch(pathGlyphs, ivec2(index & 511, index >> 9), 0);
        visible = dot(fitted.zw, fitted.zw) > 0.5;
        clip = vec4(2.0 * fitted.xy / viewportDensity.xy - 1.0, 0.0, 1.0);
        direction = fitted.zw;
    }
    else if (dot(pathTangent, pathTangent) > 0.5)
    {
        // Project a world-space tangent as a derivative, avoiding a second high/low endpoint or CPU camera work.
        vec4 delta = cameraLinearProjection * vec4(pathTangent, 0.0);
        vec2 screen = (delta.xy * clip.w - clip.xy * delta.w) * viewportDensity.xy;
        float magnitude = length(screen);
        if (magnitude > 1e-12) direction = screen / magnitude;
        else visible = false;
    }
    offset = mat2(direction.x, direction.y, -direction.y, direction.x) * offset;
    gl_Position = visible ? vec4(clip.xy / clip.w + 2.0 * offset / viewportDensity.xy, 0.0, 1.0) :
        vec4(3.0, 3.0, 0.0, 1.0);
    v_emCoord = mix(emBounds.xy, emBounds.zw, corner);
    v_bandXform = bandTransform;
    v_shapeData = bandAddress;
    v_color = fillColor;
    v_halo = haloColor;
    v_haloWidth = parameters.x * viewportDensity.z;
    v_haloSoftness = haloSoftness;
    // The quad's pixel/em extents include identical padding; their ratio is its exact displayed font size.
    v_pixelsPerEm = (pixelBounds.z - pixelBounds.x) / (emBounds.z - emBounds.x) * viewportDensity.z;
    float elapsed = frameTime - fade.x - (fade.w < 0.0 ? adoptionTime : 0.0);
    float initial = fade.w < 0.0 ? clamp(fade.y + parameters.z * (adoptionTime - parameters.w), 0.0, 1.0) : fade.y;
    v_opacity = mix(initial, fade.z, clamp(elapsed / abs(fade.w), 0.0, 1.0));
}
