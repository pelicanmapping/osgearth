#version 460
layout(local_size_x = 128) in;

// Each count is also an NV indirect draw parameter at byte offset zero.
layout(std430, binding = 14) buffer Parameter { uint count; float cut; } bands[8];
layout(std430, binding = 24) readonly buffer Commands { uint command[]; };
layout(std430, binding = 25) readonly buffer Visible { uvec4 visible[]; };
layout(std430, binding = 26) readonly buffer Centers { vec4 center[]; };
layout(std430, binding = 27) writeonly buffer Sorted { uvec4 sorted[]; };
layout(std430, binding = 28) writeonly buffer Output { uint outputCommand[]; };
layout(location = 0) uniform vec4 viewDepth;
layout(location = 1) uniform uint meshCount;
layout(location = 2) uniform uint capacity;
layout(location = 3) uniform int phase;

const uint stride = 18u; // Chonk::DrawCommand, including both bindless addresses
const uint countWord = 1u, baseWord = 4u;
const float unused = 3.402823e38;
shared float depths[256], cuts[7];
shared uint totals[8], starts[8], cursor[8], commandIndex[8], masks[32];

// Find the mesh/LOD owning a stratified capacity slot. Its live prefix may be empty after culling.
float sampleDepth(uint index)
{
    uint numerator = 2u * index + 1u;
    uint slot = (capacity / 512u) * numerator + ((capacity % 512u) * numerator) / 512u;
    uint low = 0u, high = meshCount;
    while (low + 1u < high)
    {
        uint mid = (low + high) / 2u;
        if (command[mid * stride + baseWord] <= slot) low = mid;
        else high = mid;
    }
    uint base = command[low * stride + baseWord];
    if (slot - base >= command[low * stride + countWord]) return unused;
    return dot(viewDepth, center[visible[slot].x]);
}

// One workgroup resets submission counts and periodically estimates seven quantiles from the current visible list.
void prepare(uint lane)
{
    for (uint b = 0u; b < 8u; ++b)
        if (lane == b) bands[b].count = 0u;
    if (phase == 0) return;
    depths[lane] = sampleDepth(lane);
    depths[lane + 128u] = sampleDepth(lane + 128u);
    barrier();
    for (uint size = 2u; size <= 256u; size *= 2u)
    {
        for (uint step = size / 2u; step != 0u; step /= 2u)
        {
            uint lo = (lane / step) * (2u * step) + lane % step;
            uint hi = lo + step;
            float a = depths[lo], b = depths[hi];
            bool ascending = (lo & size) == 0u;
            depths[lo] = ascending ? min(a, b) : max(a, b);
            depths[hi] = ascending ? max(a, b) : min(a, b);
            barrier();
        }
    }
    if (lane == 0u)
    {
        uint lo = 0u, hi = 256u;
        while (lo < hi)
        {
            uint mid = (lo + hi) / 2u;
            if (depths[mid] < unused) lo = mid + 1u;
            else hi = mid;
        }
        for (uint b = 0u; b < 7u; ++b)
            bands[b].cut = depths[(b + 1u) * lo / 8u];
    }
}

// Classify by this draw's eye-space depth, even when the quantiles are reused.
uint bandOf(uint source)
{
    float depth = dot(viewDepth, center[source]);
    uint band = 0u;
    for (uint b = 0u; b < 7u; ++b) band += uint(depth >= cuts[b]);
    return band;
}

// One workgroup partitions one mesh/LOD in its original allocation, preserving all visible-record fields.
void partitionMesh(uint mesh, uint lane)
{
    uint base = command[mesh * stride + baseWord];
    uint size = command[mesh * stride + countWord];
    if (size == 0u) return;
    if (lane < 8u) totals[lane] = 0u;
    for (uint b = 0u; b < 7u; ++b)
        if (lane == b) cuts[b] = bands[b].cut;
    barrier();
    for (uint i = lane; i < size; i += 128u)
        atomicAdd(totals[bandOf(visible[base + i].x)], 1u);
    barrier();
    for (uint b = 0u; b < 8u; ++b)
    {
        if (lane == b && totals[b] > 0u)
        {
            uint start = base;
            for (uint j = 0u; j < b; ++j) start += totals[j];
            starts[b] = cursor[b] = start;
            commandIndex[b] = b * meshCount + atomicAdd(bands[b].count, 1u);
        }
    }
    barrier();
    for (uint i = lane; i < 8u * stride; i += 128u)
    {
        uint b = i / stride, word = i % stride;
        if (totals[b] == 0u) continue;
        uint value = command[mesh * stride + word];
        if (word == countWord) value = totals[b];
        if (word == baseWord) value = starts[b];
        outputCommand[commandIndex[b] * stride + word] = value;
    }

    // A prefix popcount ranks each instance among earlier lanes in the same band.
    for (uint first = 0u; first < size; first += 128u)
    {
        if (lane < 32u) masks[lane] = 0u;
        barrier();
        uint band = 0u;
        uvec4 record;
        uint bit = 1u << (lane % 32u);
        if (first + lane < size)
        {
            record = visible[base + first + lane];
            band = bandOf(record.x);
            atomicOr(masks[4u * band + lane / 32u], bit);
        }
        barrier();
        if (first + lane < size)
        {
            uint rank = bitCount(masks[4u * band + lane / 32u] & (bit - 1u));
            for (uint j = 0u; j < lane / 32u; ++j) rank += bitCount(masks[4u * band + j]);
            sorted[cursor[band] + rank] = record;
        }
        barrier();
        if (lane < 8u && totals[lane] != 0u)
            for (uint j = 0u; j < 4u; ++j) cursor[lane] += bitCount(masks[4u * lane + j]);
        barrier();
    }
}

// Dispatch preparation before partitioning so all meshes share the same boundaries.
void main()
{
    if (phase == 2) partitionMesh(gl_WorkGroupID.x, gl_LocalInvocationID.x);
    else prepare(gl_LocalInvocationID.x);
}
