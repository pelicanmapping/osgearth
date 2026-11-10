#pragma vp_name FeatureImage stable material coordinates
#pragma vp_function oe_fil_material_model, vertex_model, last
#pragma import_defines(OE_TERRAIN_RENDER_ELEVATION)
struct FILCoordinates { vec4 u; vec4 v; vec4 low; };
layout(binding = 13, std430) readonly buffer FILCoordinateBuffer { FILCoordinates oe_fil_coordinates[]; };
flat out int oe_tileID;
flat out int oe_terrain_vertexMarker;
out vec3 vp_Normal;
out vec2 oe_fil_materialUV;
flat out vec4 oe_fil_materialOrigin;
uniform float oe_terrain_altitude;
float oe_terrain_getElevation();

// Use the morphed tile-local vertex, before camera-space rounding. Match REX's elevation and altitude offset.
void oe_fil_material_model(inout vec4 vertex)
{
    float height = oe_terrain_altitude;
#ifdef OE_TERRAIN_RENDER_ELEVATION
    if ((oe_terrain_vertexMarker & 1) != 0 && (oe_terrain_vertexMarker & 4) == 0)
        height += oe_terrain_getElevation();
#endif
    vec3 local = vertex.xyz + vp_Normal * height;
    FILCoordinates frame = oe_fil_coordinates[oe_tileID];
    oe_fil_materialUV = vec2(dot(frame.u.xyz, local), dot(frame.v.xyz, local));
    oe_fil_materialOrigin = vec4(frame.u.w, frame.v.w, frame.low.xy);
}

[break]
#pragma vp_name FeatureImage material view position
#pragma vp_function oe_fil_material_vertex, vertex_view, 0.9
out vec3 oe_fil_viewPosition;

// Keep view position only for the normal-map tangent frame, never for texture phase.
void oe_fil_material_vertex(inout vec4 vertex)
{
    oe_fil_viewPosition = vertex.xyz;
}

[break]
#pragma vp_name FeatureImage material evaluation
#pragma vp_location fragment_coloring
#pragma import_defines(OE_USE_PBR)
#extension GL_NV_gpu_shader5 : require
#pragma include MaterialArena.glsl
#pragma include PBRMaterial.glsl
layout(binding = 12, std430) readonly buffer FeatureImageMaterialSlots { uvec4 oe_fil_slots[]; };
in vec2 oe_fil_materialUV;
flat in vec4 oe_fil_materialOrigin;
in vec3 oe_fil_viewPosition;
in vec3 vp_Normal;
#ifdef OE_USE_PBR
struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;
#endif
vec2 oe_fil_dx, oe_fil_dy;
vec3 oe_fil_baseNormal, oe_fil_normalDelta;
vec4 oe_fil_basePBR, oe_fil_pbrDelta;
mat3 oe_fil_tbn;
float oe_fil_alpha;

// Fused remainder preserves the origin's low bits even for non-power-of-two material periods.
vec2 oe_fil_material_texcoord(float period)
{
    vec2 phase = mod(fma(-floor(oe_fil_materialOrigin.xy / period), vec2(period),
        oe_fil_materialOrigin.xy) + oe_fil_materialOrigin.zw, period);
    return (oe_fil_materialUV + phase) / period;
}

// Evaluate derivatives before the coverage loop diverges between pixels.
void oe_fil_material_begin()
{
    oe_fil_dx = dFdx(oe_fil_materialUV);
    oe_fil_dy = dFdy(oe_fil_materialUV);
    vec3 p1 = dFdx(oe_fil_viewPosition), p2 = dFdy(oe_fil_viewPosition);
    oe_fil_baseNormal = normalize(vp_Normal);
    vec3 q2 = cross(p2, oe_fil_baseNormal), q1 = cross(oe_fil_baseNormal, p1);
    vec3 T = q2 * oe_fil_dx.x + q1 * oe_fil_dy.x;
    vec3 B = q2 * oe_fil_dx.y + q1 * oe_fil_dy.y;
    float scale = inversesqrt(max(max(dot(T,T), dot(B,B)), 1e-20));
    oe_fil_tbn = mat3(T * scale, B * scale, oe_fil_baseNormal);
    oe_fil_normalDelta = vec3(0);
    oe_fil_basePBR = vec4(0, 1, 1, 0);
#ifdef OE_USE_PBR
    oe_fil_basePBR = vec4(oe_pbr.displacement, oe_pbr.roughness, oe_pbr.ao, oe_pbr.metal);
#endif
    oe_fil_pbrDelta = vec4(0);
    oe_fil_alpha = 0.0;
}

// Only contributing paint reaches here. Zero handles are safe defaults while uploads are pending.
void oe_fil_material_paint(inout vec4 fill, vec2 paint, float coverage, float remaining)
{
    uint slot = uint(paint.x);
    if (slot == 0u || slot >= uint(oe_fil_slots.length())) return;
    uvec4 mapping = oe_fil_slots[slot];
    ChonkMaterial material = chonkMaterials[mapping.x];
    vec2 uv = oe_fil_material_texcoord(paint.y);
    vec2 dx = oe_fil_dx / paint.y, dy = oe_fil_dy / paint.y;
    if (material.albedo != uint64_t(0))
    {
        vec4 albedo = textureGrad(sampler2D(material.albedo), uv, dx, dy);
        if ((mapping.y & 1u) != 0u)
            albedo.rgb = mix(1.055 * pow(max(albedo.rgb, 0.0), vec3(1.0/2.4)) - 0.055,
                12.92 * albedo.rgb, lessThanEqual(albedo.rgb, vec3(0.0031308)));
        fill *= albedo;
    }
    float weight = fill.a * coverage * remaining;
    if (weight <= 0.0) return;
    if (material.normal != uint64_t(0))
    {
        vec3 bump = textureGrad(sampler2D(material.normal), uv, dx, dy).xyz * 2.0 - 1.0;
        vec3 normal = oe_fil_tbn * bump;
        if (dot(normal, normal) > 1e-20)
            oe_fil_normalDelta += (normalize(normal) - oe_fil_baseNormal) * weight;
    }
#ifdef OE_USE_PBR
    vec4 packedValue = oe_pbr_default(material.layoutAndFactors.x);
    if (material.pbr != uint64_t(0)) packedValue = textureGrad(sampler2D(material.pbr), uv, dx, dy);
    float ao = material.occlusion == uint64_t(0) ? -1.0 :
        textureGrad(sampler2D(material.occlusion), uv, dx, dy).r;
    vec4 value = oe_pbr_decode(packedValue, material.layoutAndFactors, ao);
    oe_fil_pbrDelta += (value - oe_fil_basePBR) * weight;
#endif
}

// Remember raw coverage before vector fade and layer opacity are applied.
void oe_fil_material_coverage(float alpha) { oe_fil_alpha = alpha; }

// First-layer shader blending weights once; later passes let framebuffer alpha blend the lit output.
void oe_fil_material_finish(float alpha, bool composited)
{
    if (oe_fil_alpha <= 0.0) return;
    float weight = (composited ? alpha : 1.0) / oe_fil_alpha;
    vec3 normal = oe_fil_baseNormal + oe_fil_normalDelta * weight;
    if (dot(normal, normal) > 1e-20) vp_Normal = normalize(normal);
#ifdef OE_USE_PBR
    vec4 value = oe_fil_basePBR + oe_fil_pbrDelta * weight;
    oe_pbr.displacement = value.r;
    oe_pbr.roughness = value.g;
    oe_pbr.ao = value.b;
    oe_pbr.metal = value.a;
#endif
}
