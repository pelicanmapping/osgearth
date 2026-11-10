#version 330 core
in vec2 v_local;
flat in vec4 v_color;
flat in float v_length, v_halfWidth, v_opacity;
uniform float oe_VisibleLayer_opacityUniform = 1.0;
out vec4 fragColor;

// One-pixel analytic antialiasing and premultiplied blending keep thin leaders smooth throughout fades.
void main()
{
    float distanceToSegment = length(vec2(max(max(-v_local.x, v_local.x - v_length), 0.0), v_local.y));
    float coverage = clamp(v_halfWidth + 0.5 - distanceToSegment, 0.0, 1.0);
    float alpha = v_color.a * coverage * v_opacity * oe_VisibleLayer_opacityUniform;
    if (alpha <= 0.0) discard;
    fragColor = vec4(v_color.rgb * alpha, alpha);
}
