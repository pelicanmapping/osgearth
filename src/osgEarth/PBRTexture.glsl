#pragma vp_function oe_pbr_texture_vertex_model, vertex_model, 0.0

// Standard program for geometry that carries an osgEarth PBRTexture.
// One shared program serves every material: the component textures sit on
// fixed units (see PBRTexture::install) and per-material state travels in
// uniforms. Texture coordinates come from unit 0.

out vec2 oe_pbr_texture_uv;

void oe_pbr_texture_vertex_model(inout vec4 vertex)
{
    oe_pbr_texture_uv = gl_MultiTexCoord0.xy;
}

[break]
#pragma vp_function oe_pbr_texture_vertex_view, vertex_view, 0.0

out vec3 oe_pbr_texture_view_position;

void oe_pbr_texture_vertex_view(inout vec4 vertex_view)
{
    oe_pbr_texture_view_position = vertex_view.xyz / vertex_view.w;
}

[break]
#pragma vp_function oe_pbr_texture_fragment, fragment_coloring, 0.0
#pragma import_defines(OE_IS_SHADOW_CAMERA, OE_IS_DEPTH_CAMERA)
#pragma include PBRMaterial.glsl

in vec2 oe_pbr_texture_uv;
in vec3 oe_pbr_texture_view_position;
in vec3 vp_Normal;

uniform sampler2D oe_pbr_texture_albedo;
uniform sampler2D oe_pbr_texture_normal;
uniform sampler2D oe_pbr_texture_pbr;
uniform sampler2D oe_pbr_texture_occlusion;

// (layout, roughness factor, occlusion strength, metallic factor); see PBRMaterial::Layout.
uniform vec4 oe_pbr_texture_layoutAndFactors;
// 1 = normal map bound, 2 = packed PBR map bound, 4 = separate occlusion map bound.
uniform int oe_pbr_texture_flags;

struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;

// Tangent frame from screen-space derivatives; the bitangent follows increasing v.
vec3 oe_pbr_texture_perturb(vec3 sampled, vec2 uv)
{
    vec3 N = normalize(vp_Normal);
    vec3 dp1 = dFdx(oe_pbr_texture_view_position), dp2 = dFdy(oe_pbr_texture_view_position);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 p2 = cross(dp2, N), p1 = cross(N, dp1);
    vec3 T = p2 * duv1.x + p1 * duv2.x;
    vec3 B = p2 * duv1.y + p1 * duv2.y;
    float det = max(dot(T, T), dot(B, B));
    float inv = det > 0.0 ? inversesqrt(det) : 0.0;
    vec3 n = mat3(T * inv, B * inv, N) * (sampled * 2.0 - 1.0);
    float len = length(n);
    return len > 0.0 ? n / len : N;
}

void oe_pbr_texture_fragment(inout vec4 color)
{
    color *= texture(oe_pbr_texture_albedo, oe_pbr_texture_uv);

#if !defined(OE_IS_SHADOW_CAMERA) && !defined(OE_IS_DEPTH_CAMERA)
    if ((oe_pbr_texture_flags & 1) != 0)
    {
        vp_Normal = oe_pbr_texture_perturb(
            texture(oe_pbr_texture_normal, oe_pbr_texture_uv).xyz, oe_pbr_texture_uv);
    }

    if ((oe_pbr_texture_flags & 6) != 0 || oe_pbr_texture_layoutAndFactors.x != OE_PBR_LAYOUT_DRAM)
    {
        vec4 texel = vec4(1.0);
        if ((oe_pbr_texture_flags & 2) != 0)
            texel = texture(oe_pbr_texture_pbr, oe_pbr_texture_uv);
        float separateAO = (oe_pbr_texture_flags & 4) != 0 ?
            texture(oe_pbr_texture_occlusion, oe_pbr_texture_uv).r : -1.0;
        texel = oe_pbr_decode(texel, oe_pbr_texture_layoutAndFactors, separateAO);
        oe_pbr.displacement = texel.r;
        oe_pbr.roughness *= texel.g;
        oe_pbr.ao *= texel.b;
        oe_pbr.metal = clamp(oe_pbr.metal + texel.a, 0.0, 1.0);
    }
#endif

    // Vertex and factor colors are linear; the rest of the pipeline expects sRGB.
    vec3 c = clamp(color.rgb, 0.0, 1.0);
    color.rgb = mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, 12.92 * c, lessThanEqual(c, vec3(0.0031308)));
}
