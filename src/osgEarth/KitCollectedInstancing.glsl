#extension GL_ARB_shader_storage_buffer_object : require
#extension GL_ARB_shader_draw_parameters : require

#pragma vp_entryPoint oe_kit_collected_model
#pragma vp_location   vertex_model
#pragma vp_order      0.0

in vec3 oe_Kit_position;
in vec4 oe_Kit_rotation;
in vec3 oe_Kit_scale;

// Twelve vec4s per visible submission. Keeping this as a plain vec4 array
// makes the CPU/GPU layout unambiguous under std430.
layout(std430, binding = 28) readonly buffer oe_KitBatchBuffer
{
    vec4 oe_Kit_batches[];
};

vec3 vp_Normal;

vec3 oe_Kit_rotate(vec4 q, vec3 v)
{
    vec3 u = q.xyz;
    float s = q.w;
    return 2.0 * dot(u, v) * u +
        (s * s - dot(u, u)) * v +
        2.0 * s * cross(u, v);
}

void oe_kit_collected_model(inout vec4 vertexModel)
{
    // One multi-draw command represents one visible city batch. gl_DrawID
    // therefore selects its decode/transform descriptor without expanding a
    // batch index into every instance record on the CPU.
    uint base = uint(gl_DrawIDARB) * 12u;
    vec4 positionDecode = oe_Kit_batches[base + 0u];
    vec4 positionStepRange = oe_Kit_batches[base + 1u];
    vec4 scaleDecode = oe_Kit_batches[base + 2u];
    vec4 scaleStep = oe_Kit_batches[base + 3u];
    vec3 basisX = oe_Kit_batches[base + 4u].xyz;
    vec3 basisY = oe_Kit_batches[base + 5u].xyz;
    vec3 basisZ = oe_Kit_batches[base + 6u].xyz;
    vec3 translationHigh = oe_Kit_batches[base + 7u].xyz;
    vec3 translationLow = oe_Kit_batches[base + 8u].xyz;
    vec3 normalX = oe_Kit_batches[base + 9u].xyz;
    vec3 normalY = oe_Kit_batches[base + 10u].xyz;
    vec3 normalZ = oe_Kit_batches[base + 11u].xyz;

    vec3 instancePosition =
        positionDecode.xyz + oe_Kit_position * positionStepRange.xyz;
    vec3 instanceScale =
        scaleDecode.xyz + oe_Kit_scale * scaleStep.xyz;

    vec3 localVertex = oe_Kit_rotate(
        oe_Kit_rotation, instanceScale * vertexModel.xyz) + instancePosition;
    vec3 viewVertex =
        basisX * localVertex.x +
        basisY * localVertex.y +
        basisZ * localVertex.z;
    vec3 viewOrigin =
        basisX * instancePosition.x +
        basisY * instancePosition.y +
        basisZ * instancePosition.z;

    vec3 eyeOrigin =
        translationHigh + translationLow + viewOrigin;
    float range2 = dot(eyeOrigin, eyeOrigin);
    float minRange = positionDecode.w;
    float maxRange = positionStepRange.w;
    bool visible = range2 >= minRange * minRange &&
        (maxRange >= 3.0e38 || range2 < maxRange * maxRange);

    // Cull collection stores each submission's double-precision local-to-view
    // transform, so this hook deliberately produces a view-space vertex. The
    // model-to-view override below prevents VirtualProgram from applying the
    // persistent renderer's world-origin model matrix a second time.
    vec3 desiredViewVertex =
        translationHigh + translationLow + (visible ? viewVertex : viewOrigin);
    vertexModel = vec4(desiredViewVertex, 1.0);

    vec3 normal = oe_Kit_rotate(
        oe_Kit_rotation, vp_Normal / instanceScale);
    vec3 desiredViewNormal = normalize(
        normalX * normal.x + normalY * normal.y + normalZ * normal.z);
    vp_Normal = desiredViewNormal;
}

[break]

#pragma vp_entryPoint oe_kit_collected_model_to_view
#pragma vp_location   vertex_transform_model_to_view

// The model hook already emits view-space data. Installing this override makes
// VirtualProgram skip its normal model-view transform.
void oe_kit_collected_model_to_view()
{
}

[break]

#pragma vp_entryPoint oe_kit_collected_view
#pragma vp_location   vertex_view
#pragma vp_order      -100.0

// A view-stage hook makes VirtualProgram run the model-to-view override and
// project its view-space result in the usual way.
void oe_kit_collected_view(inout vec4 vertexView)
{
}
