#version 430

// Compacts visible Kit records on the GPU before the model vertex shader runs.
// Packed instance records are five uints (20 bytes); the output uses the same
// representation so the existing normalized integer vertex attributes can
// consume it directly.

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

layout(std430, binding = 20) readonly buffer oe_KitCullInput
{
    uint oe_kit_input[];
};

layout(std430, binding = 21) writeonly buffer oe_KitCullOutput
{
    uint oe_kit_output[];
};

// first input record, record count, descriptor/batch index, unused
layout(std430, binding = 22) readonly buffer oe_KitCullWork
{
    uvec4 oe_kit_work[];
};

// first output record and maximum record count for each batch
layout(std430, binding = 23) readonly buffer oe_KitCullSpans
{
    uvec2 oe_kit_spans[];
};

layout(std430, binding = 24) buffer oe_KitCullCounts
{
    uint oe_kit_counts[];
};

// Interpreted as either four-uint DrawArraysIndirectCommand records or
// five-uint DrawElementsIndirectCommand records according to oe_kit_pass.
layout(std430, binding = 25) writeonly buffer oe_KitCullCommands
{
    uint oe_kit_commands[];
};

layout(std430, binding = 28) readonly buffer oe_KitBatchBuffer
{
    vec4 oe_Kit_batches[];
};

uniform int oe_kit_pass;
uniform uint oe_kit_batchCount;
uniform uint oe_kit_inputBase;
uniform uint oe_kit_primitiveCount;
uniform uint oe_kit_primitiveFirst;
uniform uint oe_kit_baseVertex;
uniform vec4 oe_kit_modelSphere;
uniform mat4 oe_kit_projection;

shared uint oe_kit_prefix[128];
shared uint oe_kit_groupBase;

vec3 oe_kit_rotate(vec4 q, vec3 v)
{
    vec3 u = q.xyz;
    float s = q.w;
    return 2.0 * dot(u, v) * u +
        (s * s - dot(u, u)) * v +
        2.0 * s * cross(u, v);
}

float oe_kit_snorm16(int value)
{
    return max(-1.0, float(value) * (1.0 / 32767.0));
}

bool oe_kit_insidePlane(vec4 plane, vec3 center, float radius)
{
    float normalLength = length(plane.xyz);
    return normalLength == 0.0 ||
        dot(plane.xyz, center) + plane.w >= -radius * normalLength;
}

bool oe_kit_visible(uint recordIndex, uint batchIndex)
{
    uint word = recordIndex * 5u;
    uint p0 = oe_kit_input[word + 0u];
    uint p1 = oe_kit_input[word + 1u];
    uint p2 = oe_kit_input[word + 2u];
    uint p3 = oe_kit_input[word + 3u];
    uint p4 = oe_kit_input[word + 4u];

    uvec3 encodedPosition = uvec3(
        p0 & 0xffffu, p0 >> 16u, p1 & 0xffffu);
    uvec3 encodedScale = uvec3(
        p3 >> 16u, p4 & 0xffffu, p4 >> 16u);
    encodedScale = uvec3(
        encodedScale.x >> 5u,
        encodedScale.y >> 6u,
        encodedScale.z >> 5u);
    ivec4 encodedRotation = ivec4(
        int(p1) >> 16,
        int(p2 << 16u) >> 16,
        int(p2) >> 16,
        int(p3 << 16u) >> 16);
    vec4 rotation = vec4(
        oe_kit_snorm16(encodedRotation.x),
        oe_kit_snorm16(encodedRotation.y),
        oe_kit_snorm16(encodedRotation.z),
        oe_kit_snorm16(encodedRotation.w));

    uint descriptor = batchIndex * 12u;
    vec4 positionDecode = oe_Kit_batches[descriptor + 0u];
    vec4 positionStepRange = oe_Kit_batches[descriptor + 1u];
    vec4 scaleDecode = oe_Kit_batches[descriptor + 2u];
    vec4 scaleStep = oe_Kit_batches[descriptor + 3u];
    vec3 basisX = oe_Kit_batches[descriptor + 4u].xyz;
    vec3 basisY = oe_Kit_batches[descriptor + 5u].xyz;
    vec3 basisZ = oe_Kit_batches[descriptor + 6u].xyz;
    vec3 translation = oe_Kit_batches[descriptor + 7u].xyz +
        oe_Kit_batches[descriptor + 8u].xyz;

    vec3 instancePosition = positionDecode.xyz +
        vec3(encodedPosition) * positionStepRange.xyz;
    vec3 instanceScale = scaleDecode.xyz +
        vec3(encodedScale) * scaleStep.xyz;

    vec3 eyeOrigin = translation +
        basisX * instancePosition.x +
        basisY * instancePosition.y +
        basisZ * instancePosition.z;
    float range2 = dot(eyeOrigin, eyeOrigin);
    float minRange = positionDecode.w;
    float maxRange = positionStepRange.w;
    if (range2 < minRange * minRange ||
        (maxRange < 3.0e38 && range2 >= maxRange * maxRange))
    {
        return false;
    }

    vec3 localCenter = instancePosition + oe_kit_rotate(
        rotation, instanceScale * oe_kit_modelSphere.xyz);
    vec3 viewCenter = translation +
        basisX * localCenter.x +
        basisY * localCenter.y +
        basisZ * localCenter.z;
    float instanceMaxScale = max(
        abs(instanceScale.x), max(abs(instanceScale.y), abs(instanceScale.z)));
    float basisMaxScale = max(
        length(basisX), max(length(basisY), length(basisZ)));
    float radius = oe_kit_modelSphere.w * instanceMaxScale * basisMaxScale;

    // OSG stores row-vector matrices and uploads their contiguous storage to
    // OpenGL. The GLSL columns therefore contain the clip-plane coefficient
    // vectors needed for a view-space sphere test.
    return
        oe_kit_insidePlane(oe_kit_projection[3] + oe_kit_projection[0], viewCenter, radius) &&
        oe_kit_insidePlane(oe_kit_projection[3] - oe_kit_projection[0], viewCenter, radius) &&
        oe_kit_insidePlane(oe_kit_projection[3] + oe_kit_projection[1], viewCenter, radius) &&
        oe_kit_insidePlane(oe_kit_projection[3] - oe_kit_projection[1], viewCenter, radius) &&
        oe_kit_insidePlane(oe_kit_projection[3] + oe_kit_projection[2], viewCenter, radius) &&
        oe_kit_insidePlane(oe_kit_projection[3] - oe_kit_projection[2], viewCenter, radius);
}

void oe_kit_resetCounts()
{
    uint index = gl_GlobalInvocationID.x;
    if (index < oe_kit_batchCount)
        oe_kit_counts[index] = 0u;
}

void oe_kit_cullAndCompact()
{
    uvec4 work = oe_kit_work[gl_WorkGroupID.x];
    uint lane = gl_LocalInvocationID.x;
    bool visible = lane < work.y && oe_kit_visible(
        oe_kit_inputBase + work.x + lane, work.z);

    oe_kit_prefix[lane] = visible ? 1u : 0u;
    barrier();
    for (uint offset = 1u; offset < 128u; offset <<= 1u)
    {
        uint addend = lane >= offset ? oe_kit_prefix[lane - offset] : 0u;
        barrier();
        if (lane >= offset)
            oe_kit_prefix[lane] += addend;
        barrier();
    }

    if (lane == 0u)
        oe_kit_groupBase = atomicAdd(oe_kit_counts[work.z], oe_kit_prefix[127]);
    barrier();

    if (visible)
    {
        uint sourceRecord = oe_kit_inputBase + work.x + lane;
        uint targetRecord = oe_kit_spans[work.z].x +
            oe_kit_groupBase + oe_kit_prefix[lane] - 1u;
        for (uint component = 0u; component < 5u; ++component)
        {
            oe_kit_output[targetRecord * 5u + component] =
                oe_kit_input[sourceRecord * 5u + component];
        }
    }
}

void oe_kit_buildArrayCommands()
{
    uint batch = gl_GlobalInvocationID.x;
    if (batch >= oe_kit_batchCount)
        return;
    uint command = batch * 4u;
    oe_kit_commands[command + 0u] = oe_kit_primitiveCount;
    oe_kit_commands[command + 1u] = oe_kit_counts[batch];
    oe_kit_commands[command + 2u] = oe_kit_primitiveFirst;
    oe_kit_commands[command + 3u] = oe_kit_spans[batch].x;
}

void oe_kit_buildElementCommands()
{
    uint batch = gl_GlobalInvocationID.x;
    if (batch >= oe_kit_batchCount)
        return;
    uint command = batch * 5u;
    oe_kit_commands[command + 0u] = oe_kit_primitiveCount;
    oe_kit_commands[command + 1u] = oe_kit_counts[batch];
    oe_kit_commands[command + 2u] = oe_kit_primitiveFirst;
    oe_kit_commands[command + 3u] = oe_kit_baseVertex;
    oe_kit_commands[command + 4u] = oe_kit_spans[batch].x;
}

void main()
{
    if (oe_kit_pass == 0)
        oe_kit_resetCounts();
    else if (oe_kit_pass == 1)
        oe_kit_cullAndCompact();
    else if (oe_kit_pass == 2)
        oe_kit_buildArrayCommands();
    else
        oe_kit_buildElementCommands();
}
