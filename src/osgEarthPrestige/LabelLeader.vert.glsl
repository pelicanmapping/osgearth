#version 330 core
layout(location=6) in vec3 anchorHigh;
layout(location=7) in vec3 anchorLow;
layout(location=8) in vec4 endpoints;
layout(location=12) in vec4 color;
layout(location=14) in vec4 parameters;
layout(location=15) in vec4 fade;
uniform float frameTime, adoptionTime;
uniform mat4 cameraLinearProjection;
uniform vec3 eyeHigh, eyeLow, viewportDensity;
uniform bool geocentric;
out vec2 v_local;
flat out vec4 v_color;
flat out float v_length, v_halfWidth, v_opacity;

// Expands a screen-space capsule from the same current-frame anchor and fade used by its label.
void main()
{
    vec3 relative = (anchorHigh - eyeHigh) + (anchorLow - eyeLow);
    vec4 clip = cameraLinearProjection * vec4(relative, 1.0);
    bool visible = clip.w > 0.0 && abs(clip.x) <= clip.w && abs(clip.y) <= clip.w;
    vec3 normal = anchorHigh * vec3(1.0, 1.0, 1.006739496742);
    if (geocentric && dot(normal, -relative) <= 0.0) visible = false;
    vec2 start = endpoints.xy * viewportDensity.z;
    vec2 delta = (endpoints.zw - endpoints.xy) * viewportDensity.z;
    v_length = length(delta);
    vec2 direction = delta / max(v_length, 1e-6);
    v_halfWidth = 0.5 * parameters.x * viewportDensity.z;
    float radius = v_halfWidth + 0.5;
    vec2 corners[4] = vec2[4](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
    v_local = mix(vec2(-radius, -radius), vec2(v_length + radius, radius), corners[gl_VertexID]);
    vec2 offset = start + direction * v_local.x + vec2(-direction.y, direction.x) * v_local.y;
    gl_Position = visible ? vec4(clip.xy / clip.w + 2.0 * offset / viewportDensity.xy, 0.0, 1.0) :
        vec4(3.0, 3.0, 0.0, 1.0);
    v_color = color;
    float elapsed = frameTime - fade.x - (fade.w < 0.0 ? adoptionTime : 0.0);
    float initial = fade.w < 0.0 ? clamp(fade.y + parameters.z * (adoptionTime - parameters.w), 0.0, 1.0) : fade.y;
    v_opacity = mix(initial, fade.z, clamp(elapsed / abs(fade.w), 0.0, 1.0));
}
