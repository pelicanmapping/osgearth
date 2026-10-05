#pragma vp_function oe_grime_vertex, vertex_view, 0.9
#pragma import_defines(OE_IS_SHADOW_CAMERA, OE_IS_DEPTH_CAMERA, OE_IS_PICK_CAMERA)

uniform mat4 oe_grime_viewToMacro;
uniform mat4 oe_grime_viewToStreak;
uniform mat3 oe_grime_viewToMeters;
uniform mat3 oe_grime_normalToRegion;
out vec3 vp_Normal;
out vec3 oe_grime_macro;
out vec3 oe_grime_streak;
out vec3 oe_grime_eyeOffset;
out vec3 oe_grime_normal;

// Evaluate coordinates after instance placement, without wrapping interpolants.
void oe_grime_vertex(inout vec4 vertex)
{
#if !defined(OE_IS_SHADOW_CAMERA) && !defined(OE_IS_DEPTH_CAMERA) && !defined(OE_IS_PICK_CAMERA)
    oe_grime_macro = (oe_grime_viewToMacro * vertex).xyz;
    oe_grime_streak = (oe_grime_viewToStreak * vertex).xyz;
    // Preserve a camera-relative metric displacement and the geometric normal,
    // after instance placement but before fragment normal mapping.
    oe_grime_eyeOffset = oe_grime_viewToMeters * (vertex.xyz / vertex.w);
    oe_grime_normal = oe_grime_normalToRegion * vp_Normal;
#endif
}

[break]
#pragma vp_function oe_grime_fragment, fragment_coloring, 2.0
#pragma import_defines(OE_IS_SHADOW_CAMERA, OE_IS_DEPTH_CAMERA, OE_IS_PICK_CAMERA, OE_USE_PBR)

uniform sampler3D oe_grime_volume;
uniform float oe_grime_amount;
uniform vec3 oe_grime_tint;
// Target roughness, blending strength, and roughen-only flag.
uniform vec3 oe_grime_roughness;
// Original-roughness threshold, feather width, and retained stain strength.
uniform vec3 oe_grime_smooth;
// Maximum eye distance, fade width in meters, and upward-surface suppression.
uniform vec3 oe_grime_attenuation;
// Conventional geometry may override this uniform to exclude a material.
uniform float oe_grime_acceptance = 1.0;
in vec3 oe_grime_macro;
in vec3 oe_grime_streak;
in vec3 oe_grime_eyeOffset;
in vec3 oe_grime_normal;
#ifdef OE_USE_PBR
struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;
#endif

// Modulate decoded base color before lighting, preserving alpha and pick/depth passes.
void oe_grime_fragment(inout vec4 color)
{
#if !defined(OE_IS_SHADOW_CAMERA) && !defined(OE_IS_DEPTH_CAMERA) && !defined(OE_IS_PICK_CAMERA)
    if (oe_grime_amount <= 0.0 || oe_grime_acceptance <= 0.0)
        return;
    // Compute gradients before the per-fragment cutoff so mip selection remains
    // defined for neighboring fragments on opposite sides of the fade boundary.
    vec3 macroDx = dFdx(oe_grime_macro), macroDy = dFdy(oe_grime_macro);
    vec3 streakDx = dFdx(oe_grime_streak), streakDy = dFdy(oe_grime_streak);
    float distanceToEye = length(oe_grime_eyeOffset);
    float maxDistance = oe_grime_attenuation.x;
    float fadeStart = max(maxDistance - oe_grime_attenuation.y, 0.0);
    float distanceWeight = distanceToEye >= maxDistance ? 0.0 : 1.0;
    if (fadeStart < maxDistance)
        distanceWeight *= 1.0 - smoothstep(fadeStart, maxDistance, distanceToEye);

    // Region +Z is the anchor's up direction. Preserve walls and undersides,
    // feather suppression over slopes, and fully suppress nearly flat roofs.
    float upness = max(oe_grime_normal.z * inversesqrt(max(dot(oe_grime_normal, oe_grime_normal), 1e-12)), 0.0);
    float surfaceWeight = 1.0 - oe_grime_attenuation.z * smoothstep(0.5, 0.9, upness);
    float attenuation = distanceWeight * surfaceWeight;
    if (attenuation <= 0.0)
        return;

    float broad = smoothstep(0.25, 0.78, textureGrad(oe_grime_volume, oe_grime_macro, macroDx, macroDy).r);
    float streak = smoothstep(0.30, 0.75, textureGrad(oe_grime_volume, oe_grime_streak, streakDx, streakDy).g);
    float mask = broad * (0.65 + 0.35 * streak);
    float coverage = clamp(oe_grime_amount * oe_grime_acceptance * mask, 0.0, 1.0) * attenuation;
    float materialResponse = 1.0;
#ifdef OE_USE_PBR
    // Classify the underlying material before grime changes its roughness.
    float baseRoughness = oe_pbr.roughness;
    float originalRoughness = clamp(baseRoughness, 0.0, 1.0);
    float threshold = oe_grime_smooth.x;
    float featherEnd = min(threshold + oe_grime_smooth.y, 1.0);
    if (originalRoughness <= threshold)
        materialResponse = 0.0;
    else if (featherEnd > threshold)
        materialResponse = smoothstep(threshold, featherEnd, originalRoughness);
    // A zero-width feather uses the hard cutoff above, avoiding equal smoothstep edges.
#endif
    float colorCoverage = coverage * mix(oe_grime_smooth.z, 1.0, materialResponse);
    // osgEarth's coloring stage supplies display-encoded RGB to its lighting stage.
    vec3 linearColor = pow(max(color.rgb, vec3(0.0)), vec3(2.2));
    linearColor *= mix(vec3(1.0), oe_grime_tint, colorCoverage);
    color.rgb = pow(linearColor, vec3(1.0 / 2.2));
#ifdef OE_USE_PBR
    // Smooth surfaces retain their original roughness even when carrying a faint stain.
    if (materialResponse > 0.0 && oe_grime_roughness.y > 0.0)
    {
        // Dry deposits must not polish an already rough surface. Unrestricted
        // blending also permits smoother stains, such as wet or oily patches.
        float targetRoughness = oe_grime_roughness.z > 0.5 ?
            max(baseRoughness, oe_grime_roughness.x) : oe_grime_roughness.x;
        oe_pbr.roughness = mix(baseRoughness, targetRoughness,
            coverage * oe_grime_roughness.y * materialResponse);
    }
#endif
#endif
}
