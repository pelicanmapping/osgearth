#version 460
#extension GL_NV_gpu_shader5 : enable

#pragma import_defines(OE_GPUCULL_DEBUG)
#pragma import_defines(OE_IS_SHADOW_CAMERA)
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
    // globals:
    uint total_num_commands;
};

struct ChonkInstance
{
    mat4 xform;
    vec2 local_uv;
    uint lod;
    float visibility[2]; // per LOD
    float radius;
    float alpha_cutoff;
    int first_lod_cmd_index; // -1 means unused
};

layout(binding = 0) buffer OutputBuffer
{
    ChonkInstance output_instances[];
};

layout(binding = 29) buffer Commands
{
    DrawElementsIndirectBindlessCommandNV commands[];
};

layout(binding = 30) buffer ChonkLODs
{
    ChonkLOD chonks[];
};

layout(binding = 31) buffer InputBuffer
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



void cull()
{
    const uint i = gl_GlobalInvocationID.x; // instance
    const uint lod = gl_GlobalInvocationID.y; // lod

    // skip instances that exist only to pad the instance array to the workgroup size:
    if (input_instances[i].first_lod_cmd_index < 0)
        return;

    // initialize by clearing the visibility for this LOD:
    input_instances[i].visibility[lod] = 0.0;

    // bail if our chonk does not have this LOD
    uint v = input_instances[i].first_lod_cmd_index + lod;
    if (lod >= chonks[v].num_lods)
        return;

    // intialize:
    float fade = 1.0;

    // transform the bounding sphere to a view-space bbox.
    mat4 xform = input_instances[i].xform;
    vec4 center = xform * vec4(chonks[v].bs.xyz, 1);
    vec4 center_view = gl_ModelViewMatrix * center;

    // use the column lengths so this works for rotated instances too:
    float max_scale2 = max(dot(xform[0].xyz, xform[0].xyz),
        max(dot(xform[1].xyz, xform[1].xyz), dot(xform[2].xyz, xform[2].xyz)));
    float r = chonks[v].bs.w * sqrt(max_scale2);



    mat4 proj;
    vec2 viewport;


#ifdef OE_IS_SHADOW_CAMERA
    // For a shadow camera we want to cull instances based on their location
    // in the primary camera, not the shadow camera:
    center_view = oe_shadowToPrimaryMatrix * center_view;
    proj = oe_primaryProjectionMatrix;
    viewport = oe_primaryViewport;
#else
    proj = gl_ProjectionMatrix;
    viewport = oe_Camera.xy;

    // Trivially reject low-LOD instances that intersect the near clip plane:
    if ((lod > 0) && (proj[3][3] < 0.01)) // is perspective camera
    {
        // note: GLSL is column-major; this extracts the near plane distance.
        float near = proj[3][2] / (proj[2][2] - 1.0);
        if (-(center_view.z + r) <= near)
        {
            REJECT(REASON_NEARCLIP);
        }
    }
#endif


    // Clip-space frustum boundary (in each direction)
    float frustumBoundary = 1.0 * oe_chonk_shadow_buffer_multiplier;


    // Compute the minimum bounding box in clip space for this instance:
    vec4 LL, UR;
    compute_clip_mbb(center_view, r, proj, LL, UR);

    // Test against the view frustum:
    bool outsideFrustum =
        LL.x > frustumBoundary || UR.x < -frustumBoundary ||
        LL.y > frustumBoundary || UR.y < -frustumBoundary;


#ifdef OE_IS_SHADOW_CAMERA

    // For a shadow camera, keep coarse-LOD instances even if they do not pass the frustum cull.
    // This allows them to cast shadows into the visible frustum but only from a low LOD.
    uint coarsestLod = chonks[v].num_lods - 1;

    if (outsideFrustum && (lod != coarsestLod))
        REJECT(REASON_FRUSTUM);

#else // normal camera

    if (outsideFrustum)
        REJECT(REASON_FRUSTUM);

#endif

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

    // Pass! Set the visibility for this LOD:
    input_instances[i].visibility[lod] = fade;

    // Send along the other values:
    input_instances[i].alpha_cutoff = chonks[v].alpha_cutoff;

    // Send along the scaled radius of this instance
    input_instances[i].radius = r;

    // Bump all baseInstances following this one:
    const uint cmd_count = chonks[v].total_num_commands;
    for (uint i = v + 1; i < cmd_count; ++i)
    {
        atomicAdd(commands[i].cmd.baseInstance, 1);
    }
}

// Copies the visible instances to a compacted output buffer.
void compact()
{
    const uint i = gl_GlobalInvocationID.x; // instance
    const uint lod = gl_GlobalInvocationID.y; // lod

    float fade = input_instances[i].visibility[lod];
    if (fade < 0.1)
        return;

    uint v = input_instances[i].first_lod_cmd_index + lod;
    uint offset = commands[v].cmd.baseInstance;
    uint index = atomicAdd(commands[v].cmd.instanceCount, 1);

    // Lazy! Re-using the instance struct for render leaves..
    output_instances[offset + index] = input_instances[i];
    output_instances[offset + index].lod = lod;
}

// Entry point.
uniform int oe_pass;

void main()
{
    if (oe_pass == 0)
        cull();
    else // if (oe_pass == 1)
        compact();
}
