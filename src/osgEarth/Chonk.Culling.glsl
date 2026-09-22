#version 460
#extension GL_NV_gpu_shader5 : enable

#pragma import_defines(OE_GPUCULL_DEBUG)
#pragma import_defines(OE_IS_SHADOW_CAMERA)
#pragma import_defines(OE_CHONK_MULTIVIEW)
#pragma import_defines(OE_LOD_SCALE_UNIFORM)

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

struct DrawElementsIndirectCommand
{
    uint count;
    uint instanceCount;
    uint firstIndex;
    uint baseVertex;
    uint baseInstance;
};

struct BindlessPtrNV
{
    uint index;
    uint reserved;
    uint64_t address;
    uint64_t length;
};

struct DrawElementsIndirectBindlessCommandNV
{
    DrawElementsIndirectCommand cmd;
    uint reserved;
    BindlessPtrNV indexBuffer;
    BindlessPtrNV vertexBuffer;
};

struct ChonkLOD
{
    vec4 bs;
    float far_pixel_scale;
    float near_pixel_scale;
    // chonk-globals:
    float alpha_cutoff;
    float birthday;
    float fade_near;
    float fade_far;
    uint num_lods;
    uint draw_group;
};

// 80-byte std430 source record; matches ChonkDrawable::Instance.
struct ChonkInstance
{
    mat4 xform;
    vec2 local_uv;
    float radius;
    int first_lod_cmd_index; // -1 means unused
};

// A visible LOD references the original placement instead of copying its matrix.
struct ChonkVisibleInstance
{
    uint source_index;
    uint lod;
    float fade;
    float alpha_cutoff;
};

layout(binding = 0, std430) writeonly buffer OutputBuffer
{
    ChonkVisibleInstance output_instances[];
};

layout(binding = 29) buffer Commands
{
    DrawElementsIndirectBindlessCommandNV commands[];
};

layout(binding = 30) readonly buffer ChonkLODs
{
    ChonkLOD chonks[];
};

layout(binding = 31, std430) readonly buffer InputBuffer
{
    ChonkInstance input_instances[];
};

uniform vec3 oe_Camera;
uniform float oe_sse;
uniform vec4 oe_lod_scale;
uniform float osg_FrameTime;
uniform float oe_chonk_lod_transition_factor = 0.0;

#ifdef OE_IS_SHADOW_CAMERA
// xform from shadow camera view space to primary camera view space
uniform mat4 oe_shadowToPrimaryMatrix;
uniform mat4 oe_primaryProjectionMatrix;
uniform vec2 oe_primaryViewport;
#endif
uniform float oe_chonk_shadow_buffer_multiplier = 1.0;

// Runtime zero selects the ordinary culler, including external shadow cameras.
uniform uvec4 oe_chonk_views; // view count, active mask, mesh/LOD commands, visible capacity per view
#ifdef OE_CHONK_MULTIVIEW
uniform int oe_chonk_view_phase;
uniform int oe_chonk_view_output; // 0: PER_VIEW, 1: UNION, 2: MERGED (ChonkRenderPass::Output)
uniform uint oe_chonk_view_groups;
uniform vec4 oe_chonk_view_planes[48]; // six normalized local-space planes per view, MAX_VIEWS = 8
uniform uint oe_chonk_orthographic_views;
uniform float oe_chonk_parent_scale;
uniform mat4 oe_chonk_lod_view;
uniform mat4 oe_chonk_lod_projection;
uniform vec2 oe_chonk_lod_viewport;
uniform bool oe_chonk_retain_outside_lod_view;
layout(binding = 24, std430) readonly buffer ViewGroups { uvec2 view_groups[]; };
layout(binding = 25, std430) writeonly buffer CoreCommands { DrawElementsIndirectCommand core_commands[]; };
layout(binding = 26) writeonly buffer CompactCommands { DrawElementsIndirectBindlessCommandNV compact_commands[]; };
layout(binding = 27, std430) buffer DrawCounts { uint draw_counts[]; };
layout(binding = 28) readonly buffer Templates { DrawElementsIndirectBindlessCommandNV templates[]; };

// Returns independent lists; merged instances share one mesh/LOD command across all active views.
uint viewListCount()
{
    return oe_chonk_view_output == 0 ? oe_chonk_views.x : 1u;
}

// Resets bounded GPU output ranges without streaming command lists from the CPU.
void resetViewCommands()
{
    uint i = gl_GlobalInvocationID.x;
    if (i < 8u + 8u*oe_chonk_view_groups) draw_counts[i] = 0u;
    if (i >= viewListCount()*oe_chonk_views.z) return;
    commands[i] = templates[i%oe_chonk_views.z];
    commands[i].cmd.instanceCount = 0u;
    if (oe_chonk_view_output == 0)
        commands[i].cmd.baseInstance += (i/oe_chonk_views.z)*oe_chonk_views.w;
    else if (oe_chonk_view_output == 2)
        commands[i].cmd.baseInstance *= oe_chonk_views.x;
}

// Compacts only nonempty commands; both API paths consume counts written entirely on the GPU.
void compactViewCommands()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= viewListCount()*oe_chonk_views.z || commands[i].cmd.instanceCount == 0u) return;
    uint view = i/oe_chonk_views.z;
    uint group = chonks[i%oe_chonk_views.z].draw_group;
    uint slot = atomicAdd(draw_counts[view],1u);
    compact_commands[view*oe_chonk_views.z+slot] = commands[i];
    slot = atomicAdd(draw_counts[8u+view*oe_chonk_view_groups+group],1u);
    core_commands[view*oe_chonk_views.z+view_groups[group].x+slot] = commands[i].cmd;
}

// Conservatively tests local spheres against orthographic, perspective or oblique view volumes.
bool inViewVolume(uint view, vec4 center, float radius)
{
    uint first = view*6u;
    if ((oe_chonk_orthographic_views & (1u<<view)) != 0u)
    {
        vec3 distance = vec3(dot(oe_chonk_view_planes[first],center),
            dot(oe_chonk_view_planes[first+2u],center),dot(oe_chonk_view_planes[first+4u],center));
        vec3 extent = vec3(oe_chonk_view_planes[first+1u].w,
            oe_chonk_view_planes[first+3u].w,oe_chonk_view_planes[first+5u].w);
        return all(lessThanEqual(abs(distance),extent+vec3(radius)));
    }
    for (uint plane=0u; plane<6u; ++plane)
        if (dot(oe_chonk_view_planes[first+plane],center) < -radius) return false;
    return true;
}
#endif

// Support a user-defined LOD scale uniform. When not present,
// default to osgEarth's LOD scale value in oe_Camera.z.
#ifdef OE_LOD_SCALE_UNIFORM
uniform float OE_LOD_SCALE_UNIFORM;
#else
#define OE_LOD_SCALE_UNIFORM oe_Camera.z
#endif

#if OE_GPUCULL_DEBUG
//#ifdef OE_GPUCULL_DEBUG
#define REJECT(X) if (fade==1.0) { fade=(X);}
#else
#define REJECT(X) return
#endif
#define REASON_FRUSTUM 1.5
#define REASON_SSE 2.5
#define REASON_NEARCLIP 3.5


// calcluates the clip-space minimum bounding box of a view-space bounding sphere
void compute_clip_mbb(in vec4 p_view, in float r, in mat4 proj, out vec4 LL, out vec4 UR)
{
    vec4 temp;
    temp = proj * (p_view + vec4(-r, -r, -r, 0)); temp /= temp.w;
    LL = temp; UR = temp;
    temp = proj * (p_view + vec4(-r, -r, +r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
    temp = proj * (p_view + vec4(-r, +r, -r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
    temp = proj * (p_view + vec4(-r, +r, +r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
    temp = proj * (p_view + vec4(+r, -r, -r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
    temp = proj * (p_view + vec4(+r, -r, +r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
    temp = proj * (p_view + vec4(+r, +r, -r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
    temp = proj * (p_view + vec4(+r, +r, +r, 0)); temp /= temp.w;
    LL = min(LL, temp); UR = max(UR, temp);
}



// Culls one instance/LOD pair and appends a survivor to its command's output
// range. The CPU reserves room for every input instance in each range and
// resets the command counts before dispatch; no inter-workgroup barrier is needed.
void cullAndCompact()
{
    const uint i = gl_GlobalInvocationID.x; // instance
    const uint lod = gl_GlobalInvocationID.y; // lod

    // skip instances that exist only to pad the instance array to the workgroup size:
    if (input_instances[i].first_lod_cmd_index < 0)
        return;

    // bail if our chonk does not have this LOD
    uint first = input_instances[i].first_lod_cmd_index;
    if (lod >= chonks[first].num_lods)
        return;
    uint v = first + lod;

    // intialize:
    float fade = 1.0;

    // transform the bounding sphere to a view-space bbox.
    mat4 xform = input_instances[i].xform;
    vec4 center = xform * vec4(chonks[v].bs.xyz, 1);
    vec4 center_view = gl_ModelViewMatrix * center;

    // The CPU computes a conservative transformed radius once per instance.
    float r = input_instances[i].radius;
    mat4 proj;
    vec2 viewport;
    bool retainOutside;
#ifdef OE_IS_SHADOW_CAMERA
    // For a shadow camera we want to cull instances based on their location
    // in the primary camera, not the shadow camera:
    center_view = oe_shadowToPrimaryMatrix * center_view;
    proj = oe_primaryProjectionMatrix;
    viewport = oe_primaryViewport;
    retainOutside = true;
#else
    proj = gl_ProjectionMatrix;
    viewport = oe_Camera.xy;
    retainOutside = false;
#endif
#ifdef OE_CHONK_MULTIVIEW
    if (oe_chonk_views.x != 0u)
    {
        center_view = oe_chonk_lod_view * center;
        r *= oe_chonk_parent_scale;
        proj = oe_chonk_lod_projection;
        viewport = oe_chonk_lod_viewport;
        retainOutside = oe_chonk_retain_outside_lod_view;
    }
#endif
    // Trivially reject low-LOD instances that intersect the near clip plane:
    if (!retainOutside && (lod > 0) && (proj[3][3] < 0.01)) // is perspective camera
    {
        float near = proj[3][2] / (proj[2][2] - 1.0);
        if (-(center_view.z + r) <= near)
        {
            REJECT(REASON_NEARCLIP);
        }
    }
    // Clip-space frustum boundary (in each direction)
#ifdef OE_GPUCULL_DEBUG
    float frustumBoundary = 0.95 * oe_chonk_shadow_buffer_multiplier;
#else
    float frustumBoundary = 1.0 * oe_chonk_shadow_buffer_multiplier;
#endif


    // Compute the minimum bounding box in clip space for this instance:
    vec4 LL, UR;
    if (proj[3][3] < 0.01 && center_view.z + r >= 0.0)
    {
        // A bound crossing the eye plane cannot be bounded by projecting
        // its corners (some homogeneous W values change sign). Keep it.
        LL = vec4(-1e6);
        UR = vec4(1e6);
    }
    else
        compute_clip_mbb(center_view, r, proj, LL, UR);

    // Test against the view frustum:
    bool outsideFrustum =
        LL.x > frustumBoundary || UR.x < -frustumBoundary ||
        LL.y > frustumBoundary || UR.y < -frustumBoundary;


    // A caller can retain off-reference-view instances at their coarsest LOD, for example shadow casters.
    if (outsideFrustum && (!retainOutside || lod != chonks[v].num_lods - 1u))
        REJECT(REASON_FRUSTUM);

    // Check this, since we could have a instance outside the frustum (from the shadow pass or
    // from a oe_chonk_shadow_buffer_multiplier > 1.0)
    if (!outsideFrustum)
    {
        // Pixel-size-on-screen culling:
        vec2 dims = 0.5 * (UR.xy - LL.xy) * viewport;

        float pixelSize = min(dims.x, dims.y);
        float pixelSizePad = pixelSize * oe_chonk_lod_transition_factor;

        float minPixelSize = oe_sse * chonks[v].far_pixel_scale * oe_lod_scale[lod];
        if (pixelSize < (minPixelSize - pixelSizePad))
            REJECT(REASON_SSE);

        float maxPixelSize = 3e38;
        if (lod > 0)
        {
            float near_scale = chonks[v].near_pixel_scale * oe_lod_scale[lod - 1];
            maxPixelSize = oe_sse * near_scale;

            if (pixelSize > (maxPixelSize + pixelSizePad))
                REJECT(REASON_SSE);
        }

        // LOD cross-fade:
        if (fade == 1.0)
        {
            pixelSizePad = max(pixelSizePad, 1.0);
            if (pixelSize > maxPixelSize)
                fade = 1.0 - (pixelSize - maxPixelSize) / pixelSizePad;
            else if (pixelSize < minPixelSize)
                fade = 1.0 - (minPixelSize - pixelSize) / pixelSizePad;
        }

        // Birthday fade-in:
        const float fadein_time = 2.0; // seconds
        float birth = clamp((osg_FrameTime - chonks[v].birthday) / fadein_time, 0.0, 1.0);
        fade *= birth;
    }

    // Distance-based fade:
    float fade_range = chonks[v].fade_far - chonks[v].fade_near;
    if (fade_range > 0.0)
    {
        float dist = length(center_view.xyz) * OE_LOD_SCALE_UNIFORM;
        fade *= clamp((chonks[v].fade_far - dist) / fade_range, 0.0, 1.0);
    }

    if (fade < 0.1)
        return;

    // Keep per-LOD results local; multiple visible LODs share the same immutable
    // source placement. Preserve full precision, including debug fade values.
    ChonkVisibleInstance result;
    result.source_index = i;
    result.lod = lod;
    result.fade = fade;
    result.alpha_cutoff = chonks[v].alpha_cutoff;

    // baseInstance is fixed before dispatch. The atomic reserves a unique
    // slot within this batch/LOD's range, including during LOD cross-fades.
#ifdef OE_CHONK_MULTIVIEW
    if (oe_chonk_views.x != 0u)
    {
        uint overlaps = 0u;
        for (uint view=0u; view<oe_chonk_views.x; ++view)
        {
            if ((oe_chonk_views.y & (1u<<view)) == 0u ||
                !inViewVolume(view,center,input_instances[i].radius)) continue;
            overlaps |= 1u << view;
            if (oe_chonk_view_output == 1) break;
        }
        if (overlaps == 0u) return;
        if (oe_chonk_view_output == 2)
        {
            // Reserve every surviving view at once, avoiding per-view contention on the merged command counter.
            uint index = commands[v].cmd.baseInstance + atomicAdd(commands[v].cmd.instanceCount,uint(bitCount(overlaps)));
            while (overlaps != 0u)
            {
                uint view = uint(findLSB(overlaps));
                overlaps &= overlaps-1u;
                result.lod = lod | (view << 16u);
                output_instances[index++] = result;
            }
            return;
        }
        while (overlaps != 0u)
        {
            uint view = uint(findLSB(overlaps));
            overlaps &= overlaps-1u;
            uint command = (oe_chonk_view_output == 0 ? view : 0u)*oe_chonk_views.z+v;
            uint index = atomicAdd(commands[command].cmd.instanceCount,1u);
            result.lod = lod | (oe_chonk_view_output == 1 ? 0u : view << 16u);
            output_instances[commands[command].cmd.baseInstance+index] = result;
        }
        return;
    }
#endif
    uint index = atomicAdd(commands[v].cmd.instanceCount, 1);
    output_instances[commands[v].cmd.baseInstance + index] = result;
}

// Each invocation independently culls and emits one instance/LOD pair.
void main()
{
#ifdef OE_CHONK_MULTIVIEW
    if (oe_chonk_views.x != 0u)
    {
        if (oe_chonk_view_phase == 0) { resetViewCommands(); return; }
        if (oe_chonk_view_phase == 2) { compactViewCommands(); return; }
    }
#endif
    cullAndCompact();
}
