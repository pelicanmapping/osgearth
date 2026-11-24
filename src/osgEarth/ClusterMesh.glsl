#pragma vp_name ClusterMesh model to view
#pragma vp_function oe_clustermesh_xform, vertex_transform_model_to_view

// 80 bytes; matches ClusterMeshNode's Instance record.
struct ClusterMeshInstance
{
    mat4 xform;
    uint mesh;
    float scale;
    uint batch;
    uint pad0;
};
layout(binding = 31, std430) readonly buffer ClusterMeshInstances {
    ClusterMeshInstance oe_cm_instances[];
};
// 128 bytes; matches ClusterMeshNode's Batch record.
struct ClusterMeshBatch
{
    mat4 modelView;
    mat4 testModelView;
};
layout(binding = 23, std430) readonly buffer ClusterMeshBatches {
    ClusterMeshBatch oe_cm_batches[];
};
// One record per indirect draw, addressed by its base instance: instance, material, cluster,
// cluster flags.
layout(binding = 0, std430) readonly buffer ClusterMeshDrawData {
    uvec4 oe_cm_drawData[];
};

layout(location = 0) in vec3 oe_cm_position;
layout(location = 1) in vec3 oe_cm_normal;

out vec4 vp_Vertex;
out vec3 vp_Normal;

// Takes the place of the pipeline's gl_ModelViewMatrix transform. The node composes each
// batch's frame with the camera in double precision, which the single-precision
// gl_ModelViewMatrix of a node far from its origin could not match. Everything the pipeline
// derives from the view-space vertex afterwards, such as vp_VertexView, follows from here.
void oe_clustermesh_xform()
{
    ClusterMeshInstance instance = oe_cm_instances[oe_cm_drawData[gl_BaseInstance].x];
    mat4 xform = oe_cm_batches[instance.batch].modelView * instance.xform;

    vp_Vertex = xform * vec4(oe_cm_position, 1.0);

    // The cofactor matrix is the inverse transpose up to a positive scale for any
    // non-mirroring transform.
    mat3 m = mat3(xform);
    vp_Normal = normalize(mat3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1])) * oe_cm_normal);
}

[break]
#pragma vp_name ClusterMesh vertex
#pragma vp_function oe_clustermesh_vertex, vertex_view, first
#pragma include MaterialArena.glsl

// One record per indirect draw, addressed by its base instance: instance, material, cluster,
// cluster flags.
layout(binding = 0, std430) readonly buffer ClusterMeshDrawData {
    uvec4 oe_cm_drawData[];
};

layout(location = 3) in vec4 oe_cm_color;
layout(location = 4) in vec2 oe_cm_texcoord;

out vec4 vp_Color;
out vec2 oe_cm_uv;
out vec3 oe_cm_view;
flat out uint64_t oe_cm_albedo_tex;
flat out uint64_t oe_cm_normal_tex;
flat out uint64_t oe_cm_pbr_tex;
flat out uint64_t oe_cm_occlusion_tex;
flat out vec4 oe_cm_layoutAndFactors;
flat out uvec3 oe_cm_ids;

// Resolve the cluster's material to texture handles once per vertex, so the fragment stage
// needs no buffer lookups. The vertex arrives in view space from oe_clustermesh_xform. Being
// a view-stage function, this one also makes the pipeline run that transform.
void oe_clustermesh_vertex(inout vec4 vertex)
{
    uvec4 draw = oe_cm_drawData[gl_BaseInstance];

    vp_Color = oe_cm_color;
    oe_cm_uv = oe_cm_texcoord;
    oe_cm_view = vertex.xyz;
    oe_cm_ids = uvec3(draw.x, draw.z, draw.w);

    // Geometry without a material samples nothing; see ClusterMesh::NO_MATERIAL.
    oe_cm_albedo_tex = 0UL;
    oe_cm_normal_tex = 0UL;
    oe_cm_pbr_tex = 0UL;
    oe_cm_occlusion_tex = 0UL;
    oe_cm_layoutAndFactors = vec4(0.0, 1.0, 1.0, 1.0);
    if (draw.y != 0xffffffffu)
    {
        oe_cm_albedo_tex = chonkMaterials[draw.y].albedo;
        oe_cm_normal_tex = chonkMaterials[draw.y].normal;
        oe_cm_pbr_tex = chonkMaterials[draw.y].pbr;
        oe_cm_occlusion_tex = chonkMaterials[draw.y].occlusion;
        oe_cm_layoutAndFactors = chonkMaterials[draw.y].layoutAndFactors;
    }
}

[break]
#pragma vp_function oe_clustermesh_fragment, fragment_coloring, 0.0
#pragma import_defines(OE_IS_SHADOW_CAMERA)
#pragma import_defines(OE_IS_DEPTH_CAMERA)
#pragma import_defines(OE_CLUSTERMESH_CUTOUT)
#pragma include PBRMaterial.glsl

struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;

in vec3 vp_Normal;
in vec2 oe_cm_uv;
in vec3 oe_cm_view;
flat in uint64_t oe_cm_albedo_tex;
flat in uint64_t oe_cm_normal_tex;
flat in uint64_t oe_cm_pbr_tex;
flat in uint64_t oe_cm_occlusion_tex;
flat in vec4 oe_cm_layoutAndFactors;
flat in uvec3 oe_cm_ids; // instance, cluster, cluster flags
#define OE_CM_FLAG_LINEAR_COLOR 2u

// Texels with less alpha than this are cut out of alpha-tested materials.
uniform float oe_clustermesh_alpha_cutoff = 0.4;
// 0: shaded, 1: color by cluster, 2: color by triangle, 3: color by instance.
uniform int oe_clustermesh_debug = 0;

// Stable pseudo-random color for an integer ID.
vec3 oe_cm_id_color(uint id)
{
    id = (id ^ 61u) ^ (id >> 16u);
    id *= 9u;
    id ^= id >> 4u;
    id *= 0x27d4eb2du;
    id ^= id >> 15u;
    return vec3(float(id & 255u), float((id >> 8u) & 255u), float((id >> 16u) & 255u)) / 255.0 * 0.8 + 0.2;
}

// Cotangent frame from screen-space derivatives; meshes carry no tangents.
mat3 oe_cm_tbn(vec3 N, vec3 p, vec2 uv)
{
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float det = max(dot(T, T), dot(B, B));
    float inv = det > 0.0 ? inversesqrt(det) : 0.0;
    return mat3(T * inv, B * inv, N);
}

void oe_clustermesh_fragment(inout vec4 color)
{
    if (oe_cm_albedo_tex > 0UL)
        color *= texture(sampler2D(oe_cm_albedo_tex), oe_cm_uv);

    if ((oe_cm_ids.z & OE_CM_FLAG_LINEAR_COLOR) != 0u)
    {
        // The albedo texture decoded sRGB on sampling and vertex colors are linear; the
        // rest of the pipeline expects sRGB.
        vec3 c = clamp(color.rgb, 0.0, 1.0);
        color.rgb = mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, 12.92 * c, lessThanEqual(c, vec3(0.0031308)));
    }

    bool cutout = color.a < oe_clustermesh_alpha_cutoff;
    color.a = 1.0;

#if !defined(OE_IS_SHADOW_CAMERA) && !defined(OE_IS_DEPTH_CAMERA)
    if (oe_clustermesh_debug == 1) color.rgb = oe_cm_id_color(oe_cm_ids.y);
    else if (oe_clustermesh_debug == 2) color.rgb = oe_cm_id_color(oe_cm_ids.y * 257u + uint(gl_PrimitiveID));
    else if (oe_clustermesh_debug == 3) color.rgb = oe_cm_id_color(oe_cm_ids.x);

    vec3 N = normalize(vp_Normal);
#ifdef OE_CLUSTERMESH_CUTOUT
    // Alpha-tested geometry is drawn two-sided.
    if (!gl_FrontFacing) N = -N;
#endif

    if (oe_cm_normal_tex > 0UL)
    {
        vec4 n = texture(sampler2D(oe_cm_normal_tex), oe_cm_uv);
        vec3 tangentNormal;
        if (n.a == 0.0)
        {
            // Two-channel compressed normal; GLUtils::storage2D zeroes alpha to mark it.
            n.xy = n.xy * 2.0 - 1.0;
            n.z = 1.0 - abs(n.x) - abs(n.y);
            float t = clamp(-n.z, 0.0, 1.0);
            n.x += (n.x > 0.0) ? -t : t;
            n.y += (n.y > 0.0) ? -t : t;
            tangentNormal = n.xyz;
        }
        else
        {
            tangentNormal = n.xyz * 2.0 - 1.0;
        }
        vec3 perturbed = oe_cm_tbn(N, oe_cm_view, oe_cm_uv) * tangentNormal;
        float len = length(perturbed);
        if (len > 0.0) N = perturbed / len;
    }
    vp_Normal = N;

    if (oe_cm_pbr_tex > 0UL || oe_cm_occlusion_tex > 0UL || oe_cm_layoutAndFactors.x != OE_PBR_LAYOUT_DRAM)
    {
        vec4 texel = oe_cm_pbr_tex > 0UL ? texture(sampler2D(oe_cm_pbr_tex), oe_cm_uv) :
            oe_pbr_default(oe_cm_layoutAndFactors.x);
        float ao = oe_cm_occlusion_tex > 0UL ? texture(sampler2D(oe_cm_occlusion_tex), oe_cm_uv).r : -1.0;
        texel = oe_pbr_decode(texel, oe_cm_layoutAndFactors, ao);
        oe_pbr.displacement = texel[0];
        oe_pbr.roughness *= texel[1];
        oe_pbr.ao *= texel[2];
        oe_pbr.metal = clamp(oe_pbr.metal + texel[3], 0.0, 1.0);
    }
#endif

#ifdef OE_CLUSTERMESH_CUTOUT
    // Only the alpha-tested list compiles a discard. Its mere presence makes the GPU run
    // the depth test after shading, which opaque geometry should never pay for. It comes
    // last because derivatives are undefined in a pixel quad once one of its fragments
    // has been discarded, and the texture lookups above need them.
    if (cutout)
        discard;
#endif
}
