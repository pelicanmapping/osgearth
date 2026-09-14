#pragma vp_function oe_vegetation_vs_view, vertex_view
#pragma import_defines(OE_WIND_TEX)
#pragma import_defines(OE_WIND_TEX_MATRIX)
#pragma import_defines(OE_NOISE_TEX_INDEX)

// 80-byte std430 source record; matches ChonkDrawable::Instance.
struct Instance
{
    mat4 xform;
    vec2 local_uv;
    float radius;
    uint first_lod_cmd_index;
};

layout(binding = 31, std430) readonly buffer Instances {
    Instance instances[];
};
struct VisibleInstance
{
    uint source_index;
    uint lod;
    float fade;
    float alpha_cutoff;
};
layout(binding = 0, std430) readonly buffer VisibleInstances {
    VisibleInstance visibleInstances[];
};
layout(binding = 1, std430) buffer TextureArena {
    uint64_t textures[];
};
#define NOISE_TEX sampler2D(textures[OE_NOISE_TEX_INDEX])

layout(location = 5) in vec3 flex;

// outputs
flat out uint oe_lod;

#ifdef OE_WIND_TEX
uniform sampler3D OE_WIND_TEX;
uniform mat4 OE_WIND_TEX_MATRIX;
uniform float osg_FrameTime;
uniform mat4 osg_ViewMatrixInverse;
uniform float oe_wind_power = 1.0;

// Apply wind using the original placement's transform and tile UV, addressed
// by the visible record's source index rather than the compacted draw index.
void oe_apply_wind(inout vec4 vertex, in uint index)
{    // scale the vert's flexibility by the model Z scale factor

    mat3 vec3xform = mat3(instances[index].xform);

    float flexibility = length(flex) * vec3xform[2][2];

    if (flexibility > 0.0 && oe_wind_power > 0.0)
    {
        vec3 center = instances[index].xform[3].xyz;
        vec2 tile_uv = instances[index].local_uv;

        // sample the wind direction and speed:
        vec4 wind = textureProj(OE_WIND_TEX, (OE_WIND_TEX_MATRIX * vertex));
        vec3 wind_vec = normalize(wind.rgb * 2.0 - 1.0);
        float speed = wind.a * oe_wind_power;

        const float rate = 0.05 * speed;
        vec4 noise_moving = textureLod(NOISE_TEX, tile_uv + osg_FrameTime * rate, 0);
        speed *= noise_moving[3];

        // final wind force vector:
        vec3 wind_force = wind_vec * speed;

        // project the wind vector onto the flex plane
        vec3 flex_plane_normal = normalize(gl_NormalMatrix * vec3xform * flex);
        float dist = dot(wind_force, flex_plane_normal);
        vec3 wind_vec_projected = wind_force - flex_plane_normal * dist;

        // move the vertex within the flex plane
        vertex.xyz += wind_vec_projected * flexibility;
    }
}
#endif

// Resolve this draw's LOD and use its source placement for wind deformation.
void oe_vegetation_vs_view(inout vec4 vertex)
{
    int i = gl_BaseInstance + gl_InstanceID;
    VisibleInstance visible = visibleInstances[i];
    oe_lod = visible.lod;

#ifdef OE_WIND_TEX
    if (oe_lod == 0)
        oe_apply_wind(vertex, visible.source_index);
#endif
}


[break]
#pragma vp_function oe_vegetation_fs, fragment, 0.9
#pragma import_defines(OE_IS_SHADOW_CAMERA)

flat in uint oe_lod; // from VS
in vec3 vp_VertexView;

uniform float oe_veg_bbd0 = 0.5;
uniform float oe_veg_bbd1 = 0.75;

void oe_vegetation_fs(inout vec4 color)
{
#ifndef OE_IS_SHADOW_CAMERA

    // reduce the alpha on faces that are orthogonal to the view vector.
    // this makes cross-hatch impostors look better.
    // (only do this for impostor lods)
    if (oe_lod > 0)
    {
        vec3 dx = dFdx(vp_VertexView);
        vec3 dy = dFdy(vp_VertexView);
        vec3 fn = normalize(cross(dx, dy));
        float f = abs(dot(fn, normalize(vp_VertexView)));
        color.a *= clamp(mix(0, 1, (f - oe_veg_bbd0) / (oe_veg_bbd1 - oe_veg_bbd0)), 0, 1);
    }

#endif // !OE_IS_SHADOW_CAMERA
}
