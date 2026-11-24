#version 460
#extension GL_ARB_bindless_texture : require
#pragma import_defines(OE_LOG_DEPTH_BUFFER)

// GPU culling and LOD selection for ClusterMeshNode.
//
// PASS_INSTANCES runs once per instance. It rejects instances outside the view, bounds the
// range of that instance's clusters that could be part of the LOD cut, and queues the range
// as chunks. PASS_CLUSTERS runs once per queued (instance, cluster) pair. It applies the cut
// and visibility tests and appends an indirect draw command for every cluster that survives.
//
// Occlusion culling splits a frame in two phases. The early phase tests bounds against the
// depth pyramid of the previous frame, as seen by the previous frame's camera. That is only
// a prediction, so everything it rejects is recorded instead of dropped. Once the early
// commands are drawn, the node builds this frame's pyramid and the late phase tests the
// recorded instances and clusters against it. Whatever turns out visible is drawn late, in
// the same frame, so a wrong prediction never leaves a hole.

#define CHUNK_SIZE 64u
// Every pass is dispatched DISPATCH_WIDTH work groups wide and as many rows as needed, which
// keeps each dimension under the 65535 groups that OpenGL guarantees. All but the first
// are dispatched indirectly from row counts that earlier passes leave in the counters.
#define DISPATCH_WIDTH 256u
// Instances with at most this many candidate clusters are finished without queuing chunks.
#define INLINE_LIMIT 8u

#define PASS_INSTANCES 0
#define PASS_CLUSTERS 1
#define PASS_LATE_INSTANCES 2 // instances the early phase judged hidden
#define PASS_LATE_CLUSTERS 3  // clusters the early phase judged hidden

#define OCCLUSION_OFF 0
#define OCCLUSION_EARLY 1 // test against the previous frame and record what is rejected
#define OCCLUSION_LATE 2  // test against this frame and emit to the late draw lists

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// 80 bytes; matches ClusterMesh::Cluster.
struct Cluster
{
    vec4 bounds;
    vec4 lodSelf;
    vec4 lodParent;
    float selfError;
    float parentError;
    float prefixMinSelfError;
    uint flags;
    uint firstIndex;
    uint indexCount;
    uint material;
    uint baseVertex;
};
#define FLAG_ALPHA_TESTED 1u

// 32 bytes; matches ClusterMeshNode's Mesh record.
struct Mesh
{
    vec4 bounds;
    uint firstCluster;
    uint clusterCount;
    uint pad0, pad1;
};

// 80 bytes; matches ClusterMeshNode's Instance record.
struct Instance
{
    mat4 xform; // to the frame of its batch
    uint mesh;  // NO_MESH marks a free slot
    float scale;
    uint batch;
    uint pad0;
};
#define NO_MESH 0xffffffffu

// 128 bytes; matches ClusterMeshNode's Batch record. A batch is a set of instances that share
// a frame. The node composes each frame with the camera in double precision, so instances
// keep their accuracy however far the batch is from the node's origin.
struct Batch
{
    mat4 modelView;     // batch frame to this camera's view; all zeros hides the batch
    mat4 testModelView; // batch frame to the view of the camera the early phase tests against
};

struct DrawCommand
{
    uint count;
    uint instanceCount;
    uint firstIndex;
    uint baseVertex;
    uint baseInstance;
};

// One record per draw command: instance, material, cluster, cluster flags.
layout(binding = 0, std430) writeonly buffer DrawData { uvec4 drawData[]; };

layout(binding = 23, std430) readonly buffer Batches { Batch batches[]; };

// (instance, cluster) pairs and instances that the early phase judged hidden.
layout(binding = 24, std430) buffer HiddenClusters { uvec2 hiddenClusters[]; };
layout(binding = 25, std430) buffer HiddenInstances { uint hiddenInstances[]; };

// Matches ClusterMeshNode's Counters record. Each X, Y, Z triple is an indirect dispatch.
layout(binding = 26, std430) buffer Counters
{
    uint chunkCount;
    uint dispatchX, dispatchY, dispatchZ;
    uint drawCount[4]; // early opaque, early cutout, late opaque, late cutout
    uint triangleCount;
    uint candidateCount;
    uint hiddenInstanceCount;
    uint hiddenInstanceX, hiddenInstanceY, hiddenInstanceZ;
    uint hiddenClusterCount;
    uint hiddenClusterX, hiddenClusterY, hiddenClusterZ;
    uint occludedInstanceCount; // confirmed hidden by the late phase
    uint occludedClusterCount;
    // The most entries any pass wanted in a draw list and in the chunk queue, recorded only
    // when that was more than the list holds. Culls do not reset these. The node reads them
    // and enlarges the lists, so that an overflow lasts a few frames at most.
    uint drawDemand;
    uint chunkDemand;
};

layout(binding = 27, std430) writeonly buffer Commands { DrawCommand commands[]; };
layout(binding = 28, std430) buffer Chunks { uvec4 chunks[]; };
layout(binding = 29, std430) readonly buffer Clusters { Cluster clusters[]; };
layout(binding = 30, std430) readonly buffer Meshes { Mesh meshes[]; };
layout(binding = 31, std430) readonly buffer Instances { Instance instances[]; };

layout(location = 0) uniform int oe_cm_pass;
// Instances whose bounding sphere covers fewer pixels than this, as a radius, are skipped.
layout(location = 1) uniform float oe_cm_minInstancePixels;
// View-space frustum planes with unit normals pointing inward: left, right, bottom, top, near.
layout(location = 2) uniform vec4 oe_cm_planes[5];
// x: pixels per unit of size at unit distance (perspective) or per unit of size (orthographic)
// y: error threshold in pixels
// z: closest distance at which an error is projected
// w: 1 for an orthographic projection
layout(location = 7) uniform vec4 oe_cm_lod;
// x: instance count, y: chunk capacity, z: draw capacity per list
layout(location = 8) uniform uvec4 oe_cm_sizes;
// Largest axis scale of the node's model-view matrix. Batch frames do not scale.
layout(location = 9) uniform float oe_cm_viewScale;

layout(location = 10) uniform int oe_cm_occlusion;
// Projection of the camera that rendered the depth pyramid: last frame's in the early phase,
// this frame's in the late phase.
layout(location = 12) uniform mat4 oe_cm_testProjection;
// Viewport width and height, levels, +1 forward / -1 reverse / 0 no pyramid.
layout(location = 13) uniform vec4 oe_cm_hizParams;
// Scale and bias from clip-space Z/W to window depth.
layout(location = 14) uniform vec2 oe_cm_hizDepthMapping;
// Farthest depth per texel; level 0 is half the viewport size.
layout(location = 15, bindless_sampler) uniform sampler2D oe_cm_hiz;

#pragma include DepthPyramid.glsl

// Index of this invocation in a list that is dispatched DISPATCH_WIDTH groups wide.
uint listIndex()
{
    return (gl_WorkGroupID.y * DISPATCH_WIDTH + gl_WorkGroupID.x) * gl_WorkGroupSize.x + gl_LocalInvocationID.x;
}

// Rows of work groups needed to visit a list of the given length.
uint listRows(uint count)
{
    uint perRow = DISPATCH_WIDTH * gl_WorkGroupSize.x;
    return (count + perRow - 1u) / perRow;
}

bool sphereVisible(vec3 center, float radius)
{
    for (int i = 0; i < 5; ++i)
        if (dot(oe_cm_planes[i].xyz, center) + oe_cm_planes[i].w < -radius)
            return false;
    return true;
}

// True when a sphere, given in the view space of the pyramid's camera, lies behind the
// pyramid's depth at every pixel it can cover. Answers false whenever it cannot tell: no
// pyramid, an orthographic camera, a sphere that reaches the eye plane or misses the screen.
bool occluded(vec3 center, float radius)
{
    mat4 proj = oe_cm_testProjection;
    float nearest = -center.z - radius;
    if (nearest <= 1.0e-3)
        return false;

    // Screen bounds of the sphere's view-space box. Each edge is divided by whichever of the
    // nearest and farthest depths pushes it outward.
    float farthest = -center.z + radius;
    vec2 low = center.xy - radius, high = center.xy + radius;
    vec2 scale = vec2(proj[0][0], proj[1][1]), offset = vec2(proj[2][0], proj[2][1]);
    vec2 lo = scale * low / mix(vec2(farthest), vec2(nearest), lessThan(low, vec2(0.0))) - offset;
    vec2 hi = scale * high / mix(vec2(nearest), vec2(farthest), lessThan(high, vec2(0.0))) - offset;
    if (any(greaterThan(lo, vec2(1.0))) || any(lessThan(hi, vec2(-1.0))))
        return false;

    return oe_depthPyramid_occluded(oe_cm_hiz, oe_cm_hizParams, oe_cm_hizDepthMapping, proj, lo, hi, nearest);
}

// Size in pixels of an error, given the LOD sphere it applies to. The nearest point of the
// sphere is used so that every cluster sharing a group reaches the same decision.
float projectedError(mat4 toView, vec4 sphere, float error, float scale)
{
    float size = error * scale * oe_cm_lod.x;
    if (oe_cm_lod.w > 0.5)
        return size;
    vec3 center = (toView * vec4(sphere.xyz, 1.0)).xyz;
    return size / max(length(center) - sphere.w * scale, oe_cm_lod.z);
}

// Append the draw command of one cluster of one instance. Opaque commands and alpha-tested
// commands go to separate lists, and the late phase has its own pair of lists.
void emitCluster(uint instanceIndex, uint clusterIndex)
{
    Cluster cluster = clusters[clusterIndex];
    uint list = ((cluster.flags & FLAG_ALPHA_TESTED) != 0u ? 1u : 0u) + (oe_cm_occlusion == OCCLUSION_LATE ? 2u : 0u);
    uint capacity = oe_cm_sizes.z;
    uint slot = atomicAdd(drawCount[list], 1u);
    if (slot >= capacity)
    {
        atomicMax(drawDemand, slot + 1u);
        return;
    }
    slot += list * capacity;

    commands[slot] = DrawCommand(cluster.indexCount, 1u, cluster.firstIndex, cluster.baseVertex, slot);
    drawData[slot] = uvec4(instanceIndex, cluster.material, clusterIndex, cluster.flags);
    atomicAdd(triangleCount, cluster.indexCount / 3u);
}

// Transform from an instance's mesh to the view of the camera that rendered the pyramid.
mat4 toTestView(Instance instance)
{
    Batch batch = batches[instance.batch];
    return (oe_cm_occlusion == OCCLUSION_LATE ? batch.modelView : batch.testModelView) * instance.xform;
}

// True when the pyramid hides a cluster of an instance.
bool clusterOccluded(Instance instance, float scale, uint clusterIndex)
{
    vec4 bounds = clusters[clusterIndex].bounds;
    vec3 center = (toTestView(instance) * vec4(bounds.xyz, 1.0)).xyz;
    return occluded(center, bounds.w * scale);
}

// Apply the cut and visibility tests to one cluster of one instance, appending a draw
// command when it passes.
void testCluster(uint instanceIndex, Instance instance, float scale, uint clusterIndex)
{
    Cluster cluster = clusters[clusterIndex];
    mat4 toView = batches[instance.batch].modelView * instance.xform;

    // Draw exactly the clusters that are detailed enough while their parents are not.
    if (projectedError(toView, cluster.lodSelf, cluster.selfError, scale) > oe_cm_lod.y)
        return;
    if (cluster.parentError < 1.0e30 &&
        projectedError(toView, cluster.lodParent, cluster.parentError, scale) <= oe_cm_lod.y)
        return;

    vec3 center = (toView * vec4(cluster.bounds.xyz, 1.0)).xyz;
    if (!sphereVisible(center, cluster.bounds.w * scale))
        return;

    if (oe_cm_occlusion != OCCLUSION_OFF && clusterOccluded(instance, scale, clusterIndex))
    {
        if (oe_cm_occlusion == OCCLUSION_LATE)
        {
            atomicAdd(occludedClusterCount, 1u);
            return;
        }
        // Record the cluster for the late phase. With the list full it is drawn now instead:
        // a prediction may waste a draw but must never lose one.
        uint slot = atomicAdd(hiddenClusterCount, 1u);
        if (slot < oe_cm_sizes.z)
        {
            hiddenClusters[slot] = uvec2(instanceIndex, clusterIndex);
            atomicMax(hiddenClusterY, listRows(slot + 1u));
            return;
        }
    }

    emitCluster(instanceIndex, clusterIndex);
}

// Bound the clusters of a visible instance that can be part of its cut, then test them here
// when they are few or queue them as chunks for PASS_CLUSTERS.
void expandInstance(uint i)
{
    Instance instance = instances[i];
    Mesh mesh = meshes[instance.mesh];
    float scale = instance.scale * oe_cm_viewScale;
    vec3 center = (batches[instance.batch].modelView * (instance.xform * vec4(mesh.bounds.xyz, 1.0))).xyz;
    float radius = mesh.bounds.w * scale;

    // Every LOD sphere of the mesh lies inside its bounding sphere, so the distance any
    // cluster can be judged at lies in [nearest, farthest].
    float nearest = 1.0, farthest = 1.0;
    if (oe_cm_lod.w < 0.5)
    {
        // Clamp both ends as projectedError does, or an instance wholly inside the
        // minimum distance would be given a range that misses its own cut.
        float distance = length(center);
        nearest = max(distance - radius, oe_cm_lod.z);
        farthest = max(distance + radius, oe_cm_lod.z);
    }

    // Convert the pixel threshold to mesh-space error at those two distances.
    float errorPerDistance = oe_cm_lod.y / (oe_cm_lod.x * scale);
    float tooFine = errorPerDistance * nearest;    // a parent at or under this always suffices
    float tooCoarse = errorPerDistance * farthest; // a cluster over this never suffices

    // Clusters are sorted by descending parentError, and prefixMinSelfError never rises.
    uint first = mesh.firstCluster, last = mesh.firstCluster + mesh.clusterCount;

    uint lo = first, hi = last;
    while (lo < hi)
    {
        uint mid = (lo + hi) / 2u;
        if (clusters[mid].parentError > tooFine) lo = mid + 1u; else hi = mid;
    }
    uint end = lo;

    lo = first; hi = end;
    while (lo < hi)
    {
        uint mid = (lo + hi) / 2u;
        if (clusters[mid].prefixMinSelfError > tooCoarse) lo = mid + 1u; else hi = mid;
    }
    uint start = lo;

    if (start >= end)
        return;

    uint count = end - start;
    atomicAdd(candidateCount, count);

    // A distant instance has only a few candidates; testing them here costs less than
    // giving each a chunk whose other invocations would have nothing to do.
    if (count <= INLINE_LIMIT)
    {
        for (uint c = start; c < end; ++c)
            testCluster(i, instance, scale, c);
        return;
    }

    uint numChunks = (count + CHUNK_SIZE - 1u) / CHUNK_SIZE;
    uint capacity = oe_cm_sizes.y;
    uint base = atomicAdd(chunkCount, numChunks);
    if (base + numChunks > capacity)
        atomicMax(chunkDemand, base + numChunks);
    // Never dispatch rows beyond the chunk buffer, however far the count overflows.
    atomicMax(dispatchY, (min(base + numChunks, capacity) + DISPATCH_WIDTH - 1u) / DISPATCH_WIDTH);

    for (uint c = 0u; c < numChunks && base + c < capacity; ++c)
    {
        uint offset = c * CHUNK_SIZE;
        chunks[base + c] = uvec4(i, start + offset, min(CHUNK_SIZE, count - offset), 0u);
    }
}

// True when the pyramid hides a whole instance.
bool instanceOccluded(uint i)
{
    Instance instance = instances[i];
    vec4 bounds = meshes[instance.mesh].bounds;
    vec3 center = (toTestView(instance) * vec4(bounds.xyz, 1.0)).xyz;
    return occluded(center, bounds.w * instance.scale * oe_cm_viewScale);
}

void cullInstance()
{
    uint i = listIndex();
    if (i >= oe_cm_sizes.x)
        return;

    Instance instance = instances[i];
    if (instance.mesh == NO_MESH)
        return;
    mat4 modelView = batches[instance.batch].modelView;
    if (modelView[3][3] == 0.0)
        return;

    vec4 bounds = meshes[instance.mesh].bounds;
    vec3 center = (modelView * (instance.xform * vec4(bounds.xyz, 1.0))).xyz;
    float radius = bounds.w * instance.scale * oe_cm_viewScale;
    if (!sphereVisible(center, radius))
        return;

    // Skip an instance too small on screen to matter. Without this, a distant instance
    // would still cost the triangles of its coarsest level.
    float pixels = radius * oe_cm_lod.x;
    if (oe_cm_lod.w < 0.5)
        pixels /= max(length(center), oe_cm_lod.z);
    if (pixels < oe_cm_minInstancePixels)
        return;

    // Every instance fits in the hidden list, so nothing is lost by recording it.
    if (oe_cm_occlusion == OCCLUSION_EARLY && instanceOccluded(i))
    {
        uint slot = atomicAdd(hiddenInstanceCount, 1u);
        hiddenInstances[slot] = i;
        atomicMax(hiddenInstanceY, listRows(slot + 1u));
        return;
    }

    expandInstance(i);
}

void cullCluster()
{
    uint chunkIndex = gl_WorkGroupID.y * DISPATCH_WIDTH + gl_WorkGroupID.x;
    if (chunkIndex >= min(chunkCount, oe_cm_sizes.y))
        return;

    uvec4 chunk = chunks[chunkIndex];
    if (gl_LocalInvocationID.x >= chunk.z)
        return;

    Instance instance = instances[chunk.x];
    testCluster(chunk.x, instance, instance.scale * oe_cm_viewScale, chunk.y + gl_LocalInvocationID.x);
}

// Retest an instance that the early phase judged hidden against this frame's depth.
void cullLateInstance()
{
    uint index = listIndex();
    if (index >= hiddenInstanceCount)
        return;

    uint i = hiddenInstances[index];
    if (instanceOccluded(i))
        atomicAdd(occludedInstanceCount, 1u);
    else
        expandInstance(i);
}

// Retest a cluster that the early phase judged hidden. It already passed the LOD and
// frustum tests, which do not change within a frame.
void cullLateCluster()
{
    uint index = listIndex();
    if (index >= min(hiddenClusterCount, oe_cm_sizes.z))
        return;

    uvec2 hidden = hiddenClusters[index];
    Instance instance = instances[hidden.x];
    if (clusterOccluded(instance, instance.scale * oe_cm_viewScale, hidden.y))
        atomicAdd(occludedClusterCount, 1u);
    else
        emitCluster(hidden.x, hidden.y);
}

void main()
{
    if (oe_cm_pass == PASS_INSTANCES) cullInstance();
    else if (oe_cm_pass == PASS_CLUSTERS) cullCluster();
    else if (oe_cm_pass == PASS_LATE_INSTANCES) cullLateInstance();
    else cullLateCluster();
}
