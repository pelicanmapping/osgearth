#extension GL_EXT_gpu_shader4 : enable
#extension GL_ARB_draw_instanced: enable

#pragma vp_entryPoint oe_draw_instanced_attribute_VS_MODEL
#pragma vp_location   vertex_model
#pragma vp_order      0.0

in vec3 oe_DrawInstancedAttribute_position;
in vec4 oe_DrawInstancedAttribute_rotation;
in vec3 oe_DrawInstancedAttribute_scale;

uniform vec3 oe_DrawInstancedAttribute_positionOffset;
uniform vec3 oe_DrawInstancedAttribute_positionScale;
uniform vec3 oe_DrawInstancedAttribute_scaleOffset;
uniform vec3 oe_DrawInstancedAttribute_scaleScale;
uniform vec2 oe_DrawInstancedAttribute_range;

vec3 vp_Normal;

vec3 rotateQuatPt(vec4 q, vec3 v)
{
    vec3 u = q.xyz;
    float s = q.w;
    return 2 * dot(u, v) * u + (s *s - dot(u, u)) * v + 2 * s * cross(u, v);
}

void oe_draw_instanced_attribute_VS_MODEL(inout vec4 currVertex)
{
    vec3 instancePosition =
        oe_DrawInstancedAttribute_positionOffset +
        oe_DrawInstancedAttribute_position * oe_DrawInstancedAttribute_positionScale;
    vec3 instanceScale =
        oe_DrawInstancedAttribute_scaleOffset +
        oe_DrawInstancedAttribute_scale * oe_DrawInstancedAttribute_scaleScale;
    bool visible = true;
    if (oe_DrawInstancedAttribute_range.x > 0.0 ||
        oe_DrawInstancedAttribute_range.y < 3.0e38)
    {
        vec3 eye = (gl_ModelViewMatrix *
            vec4(instancePosition, 1.0)).xyz;
        float range2 = dot(eye, eye);
        visible =
            range2 >= oe_DrawInstancedAttribute_range.x * oe_DrawInstancedAttribute_range.x &&
            range2 < oe_DrawInstancedAttribute_range.y * oe_DrawInstancedAttribute_range.y;
    }

    vec3 result = instanceScale * currVertex.xyz;
    result = rotateQuatPt(oe_DrawInstancedAttribute_rotation, result);
    result = result + instancePosition;
    // Collapse an invisible instance at its origin so triangles are degenerate
    // before rasterization.
    currVertex.xyz = visible ? result : instancePosition;
    // Transform normal by transpose of inverse transformation
    vp_Normal = vp_Normal / instanceScale;
    vp_Normal = rotateQuatPt(oe_DrawInstancedAttribute_rotation, vp_Normal);
    vp_Normal = normalize(vp_Normal);
}
