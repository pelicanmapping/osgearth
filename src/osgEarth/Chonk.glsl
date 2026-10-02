#pragma vp_function oe_chonk_default_vertex_model, vertex_model, 0.0
#pragma import_defines(OE_IS_SHADOW_CAMERA)
#pragma import_defines(OE_IS_DEPTH_CAMERA)
// Available after vertex_model order 0.0; callers own projection and render-target routing.
int oe_chonk_view_index;
#pragma import_defines(OE_CHONK_MAX_LOD_FOR_NORMAL_MAPS)
#pragma import_defines(OE_CHONK_MAX_LOD_FOR_PBR_MAPS)
#pragma include PBRMaterial.glsl

#ifndef OE_CHONK_MAX_LOD_FOR_NORMAL_MAPS
#define OE_CHONK_MAX_LOD_FOR_NORMAL_MAPS 99
#endif

#ifndef OE_CHONK_MAX_LOD_FOR_PBR_MAPS
#define OE_CHONK_MAX_LOD_FOR_PBR_MAPS 99
#endif

// 80-byte std430 source record; matches ChonkDrawable::Instance.
struct ChonkInstance
{
    mat4 xform;
    vec2 local_uv;
    float radius;
    uint first_lod_cmd_index;
};
layout(binding = 31, std430) readonly buffer ChonkInstances {
    ChonkInstance chonkInstances[];
};
struct ChonkVisibleInstance
{
    uint source_index;
    uint lod;
    float fade;
    float alpha_cutoff;
};
layout(binding = 0, std430) readonly buffer ChonkVisibleInstances {
    ChonkVisibleInstance chonkVisibleInstances[];
};
struct ChonkMaterial
{
    uint64_t albedo;
    uint64_t normal;
    uint64_t pbr;
    uint64_t material1;
    uint64_t material2;
    ivec2 extended;
    uint64_t occlusion;
    vec4 layoutAndFactors;
};
layout(binding = 2, std430) readonly buffer ChonkMaterialArena {
    ChonkMaterial chonkMaterials[];
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in uint normal_technique;
layout(location = 3) in vec4 color;
layout(location = 4) in vec2 uv;
layout(location = 5) in vec3 flex;
layout(location = 6) in uint material_index;
layout(location = 7) in uint color_is_linear;

#define NT_DEFAULT 0
#define NT_ZAXIS 1
#define NT_HEMISPHERE 2 
#define NT_BAKED 4

// stage global
mat3 xform3;
uint chonk_lod;

// outputs
out vec3 vp_Normal;
out vec4 vp_Color;
out float oe_fade;
out vec2 oe_tex_uv;
out vec3 oe_position_vec;
out vec3 oe_position_view;
flat out uint oe_normal_technique;
flat out uint oe_color_is_linear;
flat out float oe_alpha_cutoff;
flat out uint64_t oe_albedo_tex;
flat out uint64_t oe_normal_tex;
flat out uint64_t oe_pbr_tex;
flat out uint64_t oe_occlusion_tex;
flat out vec4 oe_pbr_layoutAndFactors;
flat out ivec2 oe_extended_materials;
flat out uint64_t oe_material1_tex;
flat out uint64_t oe_material2_tex;

// Resolve the visible LOD to its stable source placement, then transform the
// vertex and resolve its material ID to cached handles.
// Keep legacy extended IDs available to custom shaders and honor map LOD limits.
void oe_chonk_default_vertex_model(inout vec4 vertex)
{
    ChonkVisibleInstance visible = chonkVisibleInstances[gl_BaseInstance + gl_InstanceID];
    uint i = visible.source_index;

    chonk_lod = visible.lod & 0xffffu;
    oe_chonk_view_index = int(visible.lod >> 16u);

    vertex = chonkInstances[i].xform * vec4(position, 1.0);
    vp_Color = color;
    oe_color_is_linear = color_is_linear;
    xform3 = mat3(chonkInstances[i].xform);
    vp_Normal = transpose(inverse(xform3)) * normal;
    oe_normal_technique = normal_technique;
    oe_tex_uv = uv;
    oe_alpha_cutoff = visible.alpha_cutoff;
    oe_fade = visible.fade;
    oe_albedo_tex = chonkMaterials[material_index].albedo;
    oe_extended_materials = chonkMaterials[material_index].extended;
    oe_material1_tex = chonkMaterials[material_index].material1;
    oe_material2_tex = chonkMaterials[material_index].material2;

    // Color and camera-depth passes need the same frame for view-dependent impostor coverage.
    oe_position_view = (gl_ModelViewMatrix * vertex).xyz;

#if defined(OE_IS_SHADOW_CAMERA) || defined(OE_IS_DEPTH_CAMERA)
    oe_fade = 1.0;
    return;
#endif

    // stuff we need only for a non-depth or non-shadow camera

    if (oe_normal_technique == NT_HEMISPHERE)
    {
        // Position vector scaled by the (scaled) radius of the instance
        oe_position_vec = gl_NormalMatrix *
            ((xform3 * position.xyz) / chonkInstances[i].radius);
    }

    // disable/ignore normal maps as directed:
    oe_normal_tex = 0;
    if (chonk_lod <= OE_CHONK_MAX_LOD_FOR_NORMAL_MAPS)
    {
        oe_normal_tex = chonkMaterials[material_index].normal;
    }

    oe_pbr_tex = 0;
    oe_occlusion_tex = 0;
    oe_pbr_layoutAndFactors = vec4(OE_PBR_LAYOUT_DRAM, 1, 1, 1);
    if (chonk_lod <= OE_CHONK_MAX_LOD_FOR_PBR_MAPS)
    {
        oe_pbr_tex = chonkMaterials[material_index].pbr;
        oe_occlusion_tex = chonkMaterials[material_index].occlusion;
        oe_pbr_layoutAndFactors = chonkMaterials[material_index].layoutAndFactors;
    }
}

[break]
#pragma vp_function oe_chonk_default_fragment, fragment
#pragma import_defines(OE_IS_SHADOW_CAMERA)
#pragma import_defines(OE_IS_DEPTH_CAMERA)
#pragma import_defines(OE_USE_ALPHA_TO_COVERAGE)
#pragma import_defines(OE_CHONK_ALPHA_TO_COVERAGE)
#pragma import_defines(OE_GL_RG_COMPRESSED_NORMALS)
#pragma import_defines(OE_GPUCULL_DEBUG)
#pragma import_defines(OE_CHONK_SINGLE_SIDED)
#pragma import_defines(OE_CHONK_OPAQUE)
#pragma import_defines(OE_CHONK_DITHER_FADE)
#pragma import_defines(OE_CHONK_BAKED_CROWN)
#pragma import_defines(OE_CHONK_COVERAGE)

struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;
#pragma include PBRMaterial.glsl

// inputs
in vec3 vp_Normal;
in vec3 oe_position_vec;
in vec3 oe_position_view;
in vec2 oe_tex_uv;
in vec3 oe_UpVectorView;
in float oe_fade;
flat in uint64_t oe_albedo_tex;
flat in uint64_t oe_normal_tex;
flat in uint64_t oe_pbr_tex;
flat in uint64_t oe_occlusion_tex;
flat in vec4 oe_pbr_layoutAndFactors;
flat in float oe_alpha_cutoff;

flat in uint oe_normal_technique;
flat in uint oe_color_is_linear;
#define NT_DEFAULT 0
#define NT_ZAXIS 1
#define NT_HEMISPHERE 2 
#define NT_BAKED 4

const float oe_normal_attenuation = 0.65;

uniform float oe_alpha_discard_threshold = 0.5;
uniform float oe_shadow_alpha_discard_threshold = 0.5;

#ifdef OE_CHONK_COVERAGE
// Nested page/representation intervals encode an alpha weight, independent of texture mip compensation.
uniform vec2 oe_chonk_coverage = vec2(0.0,1.0);
#endif

// make a TBN from normal, vertex position, and texture uv
// https://gamedev.stackexchange.com/a/86543
mat3 make_tbn(vec3 N, vec3 p, vec2 uv)
{
    // get edge vectors of the pixel triangle
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    // solve the linear system
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    // construct a scale-invariant frame 
    float det = max(dot(T, T), dot(B, B));
    float invmax = det > 0.0 ? inversesqrt(det) : 0.0;
    return mat3(T * invmax, B * invmax, N);
}

#ifdef OE_CHONK_BAKED_CROWN
// A parallel camera has one eye direction; using its eye position would bend normals across an orthographic view.
vec3 oe_chonk_crown_view()
{
    return gl_ProjectionMatrix[3][3] != 0.0 ? vec3(0,0,1) : normalize(-oe_position_view);
}

// Transport a captured foliage-normal distribution without adding a new specular lobe or changing its detail.
vec3 oe_chonk_crown_normal(vec3 normal, vec3 capture, vec3 view)
{
    vec3 axis = cross(capture, view);
    return normal + cross(axis, normal) + cross(axis, cross(axis, normal)) /
        max(1.0 + dot(capture, view), 1e-4);
}
#endif

void oe_chonk_default_fragment(inout vec4 color)
{
    float coverage = 1.0;
    #ifdef OE_CHONK_COVERAGE
    coverage = clamp(oe_chonk_coverage.y - oe_chonk_coverage.x, 0.0, 1.0);
    #endif
    // Baked cards carry independent front/back captures, including normals and cutout silhouettes.
    if (oe_normal_technique == NT_BAKED && !gl_FrontFacing)
        oe_tex_uv.t -= 0.5;

    #if defined(OE_CHONK_BAKED_CROWN) && !defined(OE_IS_SHADOW_CAMERA)
    mat3 crownFrame;
    vec3 crownView;
    float crownCoverage = 1.0;
    if (oe_normal_technique == NT_BAKED)
    {
        // Derive the planar frame in view space even in depth-only programs without a normal-transform stage.
        vec3 cardNormal = normalize(cross(dFdx(oe_position_view), dFdy(oe_position_view))) *
            (gl_FrontFacing ? 1.0 : -1.0);
        crownFrame = make_tbn(cardNormal, oe_position_view, oe_tex_uv);
        crownFrame[0] = normalize(crownFrame[0]);
        crownFrame[1] = normalize(crownFrame[1]);
        crownView = oe_chonk_crown_view();
        // Keep the best-facing card at full coverage, suppressing edge-on slices through its crown.
        // Relative rather than absolute facing avoids holes between the horizontal and upright views.
        vec3 facing = abs(transpose(crownFrame) * crownView);
        crownCoverage = smoothstep(0.45, 0.95, facing.z / max(max(facing.x, facing.y), facing.z));
    }
    #endif

    // Legacy simulated normals mirror full-texture cards. Authored volume normals
    // retain their atlas coordinates: mirroring U would sample a different view.
    if (!gl_FrontFacing && (oe_normal_technique == NT_ZAXIS || oe_normal_technique == NT_HEMISPHERE))
    {
        oe_tex_uv.s = 1.0 - oe_tex_uv.s;
    }

    // Apply the base color:
    if (oe_albedo_tex > 0)
    {
        color *= texture(sampler2D(oe_albedo_tex), oe_tex_uv);
    }
    if (oe_color_is_linear != 0u)
    {
        // Albedo textures decode sRGB on sample; vertex colors and
        // material factors remain linear until after their multiplication.
        vec3 c = clamp(color.rgb, 0.0, 1.0);
        color.rgb = mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
                        12.92 * c, lessThanEqual(c, vec3(0.0031308)));
    }

    // Recover foliage coverage lost to mip filtering in every pass. Shadow maps often minify leaf textures
    // more than the color view; testing their uncorrected alpha can erase the entire crown, leaving only trunks.
    // Keep this before silhouette cutouts and opacity ramps; a zero per-draw coefficient leaves alpha unchanged.
    if (oe_albedo_tex > 0UL)
    {
        vec2 miplevel = textureQueryLod(sampler2D(oe_albedo_tex), oe_tex_uv);
        color.a *= (1.0 + miplevel.x * oe_alpha_cutoff);
    }

#if defined(OE_IS_SHADOW_CAMERA) || defined(OE_IS_DEPTH_CAMERA)

    #if defined(OE_CHONK_BAKED_CROWN) && !defined(OE_IS_SHADOW_CAMERA)
    // A camera depth pass must not leave invisible edge-on sheets in front of visible geometry.
    color.a *= crownCoverage;
    #endif

    // for shadowing cameras, just do a simple step discard.
    color.a = step(oe_shadow_alpha_discard_threshold, color.a * oe_fade);
    // Single-sample shadow/depth passes select the dominant representation without thinning its leaf silhouette.
    color.a *= step(0.5, coverage);
  #ifndef OE_CHONK_OPAQUE // opaque, unfaded instances never fail it; omitting keeps early-Z
    if (color.a < 1.0)
        discard;
  #endif

#else // !OE_IS_SHADOW_CAMERA && !OE_IS_DEPTH_CAMERA


#if OE_GPUCULL_DEBUG

    // apply the high fade from the instancer
    if (oe_fade <= 1.0) color.a *= oe_fade; // color.rgb = vec3(oe_fade, oe_fade, oe_fade); // color.a *= oe_fade;
    else if (oe_fade <= 2.0) color.rgb = vec3(1, 0, 0); // REASON_FRUSTUM
    else if (oe_fade <= 3.0) color.rgb = vec3(1, 1, 0); // REASON_SSE
    else if (oe_fade <= 4.0) color.rgb = vec3(0, 1, 0); // REASON_NEARCLIP
    else color.rgb = vec3(1, 0, 1); // should never happen :)

#else // normal rendering path:

    #ifdef OE_CHONK_BAKED_CROWN
    if (oe_normal_technique == NT_BAKED)
    {
        // Apply after mip compensation so distance cannot bring an edge-on card back into view.
        color.a = clamp(color.a, 0.0, 1.0) * crownCoverage;
    }
    #endif
    #ifdef OE_CHONK_COVERAGE
    // Saturate mip compensation before any opacity ramp, otherwise high mips can cancel a transition's fade.
    color.a = clamp(color.a, 0.0, 1.0) * coverage;
    #endif
  #if defined(OE_USE_ALPHA_TO_COVERAGE) || defined(OE_CHONK_ALPHA_TO_COVERAGE)
    // Requested MSAA does not guarantee that this camera/FBO has multisample storage.
    // Keep cutouts functional in single-sample render targets instead of drawing solid foliage cards.
    if (gl_NumSamples > 1)
        color.a *= oe_fade;
    else
  #endif
    {

    // When A2C is not available, we force the alpha to 0 or 1 and then
    // discard the invisible fragments.    
    // This is necessary because the GPU culler generates the draw commands in an
    // arbitrary order and not depth sorted. Even if we did depth-sort the results,
    // there are plenty of overlapping geometries in vegetation that would cause
    // flickering artifacts.
    // (TODO: consider a cheap alpha-only pass that we can sample to prevent overdraw
    // and discard in the expensive shader)
    #ifdef OE_CHONK_DITHER_FADE
    // Screen-door coverage separates LOD/distance fade from the asset's alpha test; no blended draw ordering needed.
    const int bayer[16] = int[16](0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5);
    ivec2 pixel = ivec2(gl_FragCoord.xy) & 3;
    float threshold = (float(bayer[pixel.y*4 + pixel.x]) + 0.5) / 16.0;
    color.a = step(oe_alpha_discard_threshold, color.a) * step(threshold, oe_fade);
    #else
    color.a = step(oe_alpha_discard_threshold, color.a * oe_fade);
    #endif
    #ifndef OE_CHONK_OPAQUE
    // The culler routes only opaque, unfaded instances to the OE_CHONK_OPAQUE lists. Just
    // compiling a discard makes the GPU test depth after shading once depth writes are on.
    if (color.a < 1.0)
        discard;
    #endif

    } // single-sample alpha test

#endif // !OE_GPUCULL_DEBUG

    vec3 normal_view = vp_Normal;

    if (oe_normal_technique == NT_HEMISPHERE)
    {
        // for billboarded normals, adjust the normal so its coverage
        // is a hemisphere facing the viewer. Should we recalculate the TBN here?
        // Probably, but let's not if it already looks good enough.
        vec3 v3d = oe_position_vec; // do not normalize!
        vec3 v2d = vec3(v3d.x, v3d.y, 0.0);
        float size2d = length(v2d) * 1.2021; // adjust for radius, bbox diff
        size2d = mix(0.0, oe_normal_attenuation, clamp(size2d, 0.0, 1.0));
        normal_view = mix(vec3(0, 0, 1), normalize(v2d), size2d);
    }

    vec3 pixel_normal = vec3(0, 0, 1);

    if (oe_normal_tex > 0)
    {
        vec4 n = texture(sampler2D(oe_normal_tex), oe_tex_uv);

        if (n.a == 0) // swizzled in GLUtils::storage2D to indicate a compressed normal
        {
            n.xy = n.xy*2.0 - 1.0;
            n.z = 1.0 - abs(n.x) - abs(n.y);
            float t = clamp(-n.z, 0, 1);
            n.x += (n.x > 0) ? -t : t;
            n.y += (n.y > 0) ? -t : t;
            pixel_normal = n.xyz;
        }
        else
        {
            pixel_normal = normalize(n.xyz*2.0 - 1.0);
        }
    }

    mat3 TBN;
    #ifdef OE_CHONK_BAKED_CROWN
    if (oe_normal_technique == NT_BAKED) TBN = crownFrame;
    else
    #endif
        TBN = make_tbn(normalize(normal_view), oe_position_view, oe_tex_uv);

    if (oe_normal_technique == NT_BAKED)
    {
        // Bake coordinates are an orthonormal card frame, independent of atlas aspect or instance scale.
        // Derivative cotangents reverse on a back face; the stored frame and its normal must not.
        float facing = gl_FrontFacing ? 1.0 : -1.0;
        TBN[0] = normalize(TBN[0]) * facing;
        TBN[1] = normalize(TBN[1]) * facing;
    }

    // Without a normal map, preserve the geometric normal even if UVs
    // are missing or degenerate.
    vp_Normal = oe_normal_tex > 0 ? TBN * pixel_normal : normalize(normal_view);

    #ifdef OE_CHONK_BAKED_CROWN
    if (oe_normal_technique == NT_BAKED)
    {
        // The capture is a view of a foliage volume, not a lit sheet. Rotate its normal distribution
        // from the capture axis toward the actual eye, as a rounded crown would present a new surface.
        vec3 capture = TBN[2] * (gl_FrontFacing ? 1.0 : -1.0);
        vp_Normal = oe_chonk_crown_normal(vp_Normal, capture, crownView);
    }
    #endif

    if (oe_normal_technique == NT_ZAXIS)
    {
        // attenuate the normal to a z-up orientation
        vec3 world_up = gl_NormalMatrix * vec3(0,0,1);
        vec3 face_up = TBN[1].xyz;
        vp_Normal = normalize(mix(world_up, face_up, 0.25));
    }

    if (oe_pbr_tex > 0 || oe_occlusion_tex > 0 || oe_pbr_layoutAndFactors.x != OE_PBR_LAYOUT_DRAM)
    {
        // apply PBR maps:
        vec4 texel = oe_pbr_tex > 0 ? texture(sampler2D(oe_pbr_tex), oe_tex_uv) :
            oe_pbr_default(oe_pbr_layoutAndFactors.x);
        float ao = oe_occlusion_tex > 0 ? texture(sampler2D(oe_occlusion_tex), oe_tex_uv).r : -1.0;
        texel = oe_pbr_decode(texel, oe_pbr_layoutAndFactors, ao);

        oe_pbr.displacement = texel[0];
        oe_pbr.roughness *= texel[1];
        oe_pbr.ao *= texel[2];
        oe_pbr.metal = clamp(oe_pbr.metal + texel[3], 0, 1);
    }

#endif // !OE_IS_SHADOW_CAMERA
}
