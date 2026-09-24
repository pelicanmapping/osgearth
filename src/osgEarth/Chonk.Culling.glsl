#version 460
#extension GL_NV_gpu_shader5 : enable
#extension GL_ARB_bindless_texture : enable

#pragma import_defines(OE_GPUCULL_DEBUG)
#pragma import_defines(OE_IS_SHADOW_CAMERA)
#pragma import_defines(OE_CHONK_MULTIVIEW)
#pragma import_defines(OE_LOD_SCALE_UNIFORM)
#pragma import_defines(OE_LOG_DEPTH_BUFFER)

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
    uint flags; // CHONK_FLAG_*
    uint pad0, pad1, pad2;
};
#define CHONK_FLAG_ALPHA_TESTED 1u

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

// Single-view culler. Commands form four lists of oe_chonk_list_stride entries each:
// early opaque, early cutout, late opaque, late cutout. Opaque lists draw without discard.
#define CHONK_PASS_RESET 0 // zero every list's instance counts
#define CHONK_PASS_CULL  1 // frustum/LOD culling into the early lists
#define CHONK_PASS_EARLY 2 // like CULL, but only instances visible last frame
#define CHONK_PASS_LATE  3 // test against this frame's depth pyramid, emit the rest late
uniform int oe_chonk_pass;
uniform uint oe_chonk_list_stride;
uniform uint oe_chonk_max_lods;   // visibility records per source instance
uniform vec4 oe_chonk_hiz_params; // viewport width and height in pixels, pyramid levels, 1 if valid
layout(bindless_sampler) uniform sampler2D oe_chonk_hiz; // farthest depth per texel; level 0 = half res
layout(binding = 23, std430) buffer ChonkVisibility { uint visibility[]; }; // 1 = visible last frame
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

// True when a view-space sphere lies wholly outside the left, right, bottom or top plane of a
// perspective frustum. The planes pass through the eye, so this holds for spheres that reach the
// eye plane, where projecting corners does not work (Gribb/Hartmann planes from the projection rows).
bool outsideSidePlanes(in vec3 c, in float r, in mat4 proj)
{
    vec4 row0 = vec4(proj[0][0], proj[1][0], proj[2][0], proj[3][0]);
    vec4 row1 = vec4(proj[0][1], proj[1][1], proj[2][1], proj[3][1]);
    vec4 row3 = vec4(proj[0][3], proj[1][3], proj[2][3], proj[3][3]);
    vec4 planes[4] = vec4[4](row3 + row0, row3 - row0, row3 + row1, row3 - row1);
    for (int i = 0; i < 4; ++i)
        if (dot(planes[i].xyz, c) + planes[i].w < -r * length(planes[i].xyz))
            return true;
    return false;
}

// Window-space depth of a point at view distance w (> 0), matching the depth buffer's mapping.
// Vertex-only log depth rasterizes at or beyond this value at every pixel (log(1+1/u) is convex
// in u = 1/w, which is what the rasterizer interpolates), so comparing it stays conservative.
float windowDepth(float w, mat4 proj)
{
#if defined(OE_LOG_DEPTH_BUFFER)
    float far = proj[3][2] / (proj[2][2] + 1.0);
  #if OE_LOG_DEPTH_BUFFER == 2
    return log(w*0.001 + 1.0) / log(far*0.001 + 1.0); // per-fragment log depth
  #else
    return log2(w + 1.0) / log2(far + 1.0); // per-vertex log depth
  #endif
#else
    return 0.5*(proj[2][2]*(-w) + proj[3][2])/w + 0.5;
#endif
}

// True when a view-space sphere lies behind this frame's depth pyramid at every pixel it can
// cover. Samples the 2x2 texels spanning its screen box at the level where the box fits in two.
bool occludedByHiZ(vec4 center_view, float r, mat4 proj, vec4 LL, vec4 UR)
{
    float w = -(center_view.z + r); // view distance to the sphere's nearest point
    if (oe_chonk_hiz_params.w < 0.5 || proj[3][3] > 0.01 || w <= 0.0 || LL.x < -1e5)
        return false;
    vec2 lo = clamp(LL.xy*0.5 + 0.5, 0.0, 1.0) * oe_chonk_hiz_params.xy * 0.5; // level-0 texels
    vec2 hi = clamp(UR.xy*0.5 + 0.5, 0.0, 1.0) * oe_chonk_hiz_params.xy * 0.5;
    int level = min(int(ceil(log2(max(max(hi.x - lo.x, hi.y - lo.y), 1.0)))),
        int(oe_chonk_hiz_params.z) - 1);
    ivec2 size = textureSize(oe_chonk_hiz, level);
    ivec2 a = clamp(ivec2(lo / exp2(float(level))), ivec2(0), size - 1);
    ivec2 b = clamp(ivec2(hi / exp2(float(level))), ivec2(0), size - 1);
    float farthest = max(
        max(texelFetch(oe_chonk_hiz, a, level).r, texelFetch(oe_chonk_hiz, ivec2(b.x, a.y), level).r),
        max(texelFetch(oe_chonk_hiz, ivec2(a.x, b.y), level).r, texelFetch(oe_chonk_hiz, b, level).r));
    // The margin covers 24-bit depth quantization.
    return windowDepth(w, proj) > farthest + 1e-6;
}

// Zeroes every list's instance count; command templates stay resident on the GPU.
void resetCommands()
{
    uint i = gl_GlobalInvocationID.x;
    if (i < 4u*oe_chonk_list_stride)
        commands[i].cmd.instanceCount = 0u;
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
    bool outsideFrustum;
    if (proj[3][3] < 0.01 && center_view.z + r >= 0.0)
    {
        // A bound reaching the eye plane cannot be bounded by projecting its corners (some
        // homogeneous W values change sign). Reject it if it lies wholly behind the eye or outside
        // a side plane; any other such bound surrounds the near frustum, so keep it at full size.
        outsideFrustum = center_view.z - r >= 0.0 || outsideSidePlanes(center_view.xyz, r, proj);
        LL = vec4(-1e6);
        UR = vec4(1e6);
    }
    else
    {
        compute_clip_mbb(center_view, r, proj, LL, UR);
        outsideFrustum =
            LL.x > frustumBoundary || UR.x < -frustumBoundary ||
            LL.y > frustumBoundary || UR.y < -frustumBoundary;
    }


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
    // Opaque, unfaded instances cannot fail the alpha test, so their list draws without discard.
    uint list = ((chonks[v].flags & CHONK_FLAG_ALPHA_TESTED) == 0u && fade == 1.0) ? 0u : 1u;
    if (oe_chonk_pass >= CHONK_PASS_EARLY)
    {
        // Two-phase occlusion culling: the early pass redraws last frame's visible set; the late
        // pass tests everything against the depth that produced, then draws only what it missed.
        uint slot = i*oe_chonk_max_lods + lod;
        bool wasVisible = visibility[slot] != 0u;
        if (oe_chonk_pass == CHONK_PASS_EARLY)
        {
            if (!wasVisible) return;
        }
        else
        {
            bool visible = !occludedByHiZ(center_view, r, proj, LL, UR);
            visibility[slot] = visible ? 1u : 0u;
            if (!visible || wasVisible) return;
            list += 2u;
        }
    }
    uint command = list*oe_chonk_list_stride + v;
    uint index = atomicAdd(commands[command].cmd.instanceCount, 1);
    output_instances[commands[command].cmd.baseInstance + index] = result;
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
    else
#endif
    if (oe_chonk_pass == CHONK_PASS_RESET) { resetCommands(); return; }
    cullAndCompact();
}
