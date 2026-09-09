

header = R"(
#version 430
#pragma import_defines(RADIANCE_API_ENABLED)
#pragma import_defines(COMBINED_SCATTERING_TEXTURES)
#pragma import_defines(UNIT_LENGTH_METERS_INVERSE)
)";


pbr = R"(
#pragma import_defines(OE_USE_PBR)
#pragma import_defines(OE_SHADOWING)
#pragma import_defines(OE_SKY_ENVIRONMENT)
#pragma import_defines(OE_SKY_REFLECTION_SAMPLES)

#ifdef OE_SHADOWING
float oe_shadow_visibility;
#endif
#ifdef OE_USE_PBR
struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;
#endif

uniform float oe_sky_iblStrength = 1.0;
uniform float oe_sky_groundReflectance = 0.15;
uniform float oe_sky_exposure;
uniform mat4 osg_ViewMatrix;

vec3 atmos_normalize(vec3 v, vec3 fallback)
{
    float len2 = dot(v, v);
    return len2 > 1e-12 ? v * inversesqrt(len2) : fallback;
}

vec3 atmos_decode(vec3 c)
{
    c = max(c, vec3(0.0));
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)),
        greaterThan(c, vec3(0.04045)));
}

vec3 atmos_encode(vec3 c)
{
    c = max(c, vec3(0.0));
    return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
        greaterThan(c, vec3(0.0031308)));
}

vec3 atmos_fresnel(float vh, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - clamp(vh, 0.0, 1.0), 5.0);
}

float atmos_G1(float nx, float alpha2)
{
    return 2.0 * nx / max(nx + sqrt(alpha2 + (1.0 - alpha2) * nx * nx), 1e-6);
}

vec3 atmos_directBRDF(vec3 N, vec3 V, vec3 L, vec3 albedo, vec3 f0,
    float metal, float alpha2)
{
    float nv = max(dot(N, V), 1e-4), nl = max(dot(N, L), 0.0);
    if (nl <= 0.0) return vec3(0.0);
    vec3 H = atmos_normalize(V + L, N);
    float nh = max(dot(N, H), 0.0);
    float d = nh * nh * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 / max(PI * d * d, 1e-12);
    vec3 F = atmos_fresnel(dot(V, H), f0);
    vec3 specular = distribution * atmos_G1(nv, alpha2) * atmos_G1(nl, alpha2) * F /
        max(4.0 * nv * nl, 1e-6);
    // GetSunAndSkyIrradiance already includes NdotL; do not multiply it again.
    return (1.0 - F) * (1.0 - metal) * albedo / PI + specular;
}

#ifdef OE_SKY_ENVIRONMENT
// Heitz 2018 GGX visible-normal sampling. With separable Smith masking the
// importance weight reduces to Fresnel * G1(NdotL), bounded even at grazing angles.
// https://jcgt.org/published/0007/04/01/
vec3 atmos_sampleVisibleGGX(vec3 view, float alpha, vec2 xi)
{
    vec3 stretched = normalize(vec3(alpha * view.xy, max(view.z, 1e-4)));
    float lensq = dot(stretched.xy, stretched.xy);
    vec3 t1 = lensq > 1e-8 ? vec3(-stretched.y, stretched.x, 0.0) / sqrt(lensq) : vec3(1, 0, 0);
    vec3 t2 = cross(stretched, t1);
    float r = sqrt(xi.x), phi = 2.0 * PI * xi.y;
    float x = r * cos(phi), y = r * sin(phi);
    float blend = 0.5 * (1.0 + stretched.z);
    y = mix(sqrt(max(1.0 - x * x, 0.0)), y, blend);
    vec3 h = x * t1 + y * t2 + sqrt(max(1.0 - x * x - y * y, 0.0)) * stretched;
    return normalize(vec3(alpha * h.xy, max(h.z, 0.0)));
}

vec3 atmos_environmentRadiance(vec3 point, vec3 direction, vec3 sun,
    vec3 groundRadiance)
{
    float radius = length(point);
    float mu = dot(point, direction) / radius;
    // The atmosphere LUT excludes surface reflection. Supply a neutral mean
    // ground radiance for rays hitting Earth; no fabricated building reflections.
    if (RayIntersectsGround(radius, mu))
        return groundRadiance;
    vec3 transmittance;
    return max(GetSkyRadiance(point, direction, 0.0, sun, transmittance), vec3(0.0));
}

vec3 atmos_environmentSpecular(vec3 point, vec3 N, vec3 V, vec3 sun, vec3 f0,
    float alpha, vec3 groundRadiance, out vec3 reflectedEnergy)
{
    // Anchor the sampling frame in world space so orbiting the camera does not
    // rotate an arbitrary sample pattern around the normal.
    vec3 axis = osg_ViewMatrix[2].xyz;
    if (abs(dot(axis, N)) > 0.99) axis = osg_ViewMatrix[0].xyz;
    vec3 T = normalize(cross(axis, N)), B = cross(N, T);
    vec3 localV = vec3(dot(V, T), dot(V, B), max(dot(V, N), 1e-4));
    vec3 result = vec3(0.0);
    reflectedEnergy = vec3(0.0);
    for (int i = 0; i < OE_SKY_REFLECTION_SAMPLES; ++i)
    {
        vec2 xi = vec2((float(i) + 0.5) / float(OE_SKY_REFLECTION_SAMPLES),
            float(bitfieldReverse(uint(i))) * 2.3283064365386963e-10);
        vec3 h = atmos_sampleVisibleGGX(localV, alpha, xi);
        vec3 H = T * h.x + B * h.y + N * h.z;
        float vh = max(dot(V, H), 0.0);
        vec3 L = 2.0 * vh * H - V;
        float nl = max(dot(N, L), 0.0);
        if (nl > 0.0)
        {
            vec3 weight = atmos_fresnel(vh, f0) * atmos_G1(nl, alpha * alpha);
            result += atmos_environmentRadiance(point, L, sun, groundRadiance) * weight;
            reflectedEnergy += weight;
        }
    }
    reflectedEnergy /= float(OE_SKY_REFLECTION_SAMPLES);
    return result / float(OE_SKY_REFLECTION_SAMPLES);
}
#endif

vec3 atmos_surfaceRadiance(vec3 displayColor, vec3 point, vec3 N, vec3 V,
    vec3 L, vec3 ambient, vec3 sunColor, float sunEnabled)
{
    vec3 albedo = atmos_decode(displayColor);
    float roughness = 1.0, metal = 0.0, ao = 1.0;
#ifdef OE_USE_PBR
    roughness = clamp(oe_pbr.roughness, 0.045, 1.0);
    metal = clamp(oe_pbr.metal, 0.0, 1.0);
    ao = clamp(oe_pbr.ao, 0.0, 1.0);
#endif
    // Filter sub-pixel normal variance into the specular lobe to limit sparkle.
    vec3 dx = dFdx(N), dy = dFdy(N);
    float variance = 0.5 * (dot(dx, dx) + dot(dy, dy));
    float alpha2 = clamp(pow(roughness, 4.0) + min(variance, 0.25), 4e-6, 1.0);
    float alpha = sqrt(alpha2);
    vec3 f0 = mix(vec3(0.04), albedo, metal);
    vec3 U = atmos_normalize(point, N);
    // Avoid invalid LUT coordinates for geometry slightly below the ellipsoid.
    point = U * max(length(point), bottom_radius + UNIT_LENGTH_METERS_INVERSE);
    vec3 skyIrradiance;
    vec3 sunIrradiance = GetSunAndSkyIrradiance(point, N, L, skyIrradiance);
    float sunlightVisibility = sunEnabled;
#ifdef OE_SHADOWING
    sunlightVisibility *= oe_shadow_visibility;
#endif
    // Approximate the finite solar disk in the direct specular lobe, without
    // broadening reflections of the rest of the environment.
    float sunAlpha2 = min(alpha2 + 0.25 * sun_angular_radius * sun_angular_radius, 1.0);
    vec3 direct = atmos_directBRDF(N, V, L, albedo, f0, metal, sunAlpha2) *
        max(sunIrradiance, vec3(0.0)) * sunColor * sunlightVisibility;

    vec3 indirect = vec3(0.0);
#ifdef OE_SKY_ENVIRONMENT
    float environmentStrength = clamp(oe_sky_iblStrength, 0.0, 1.0) * sunEnabled;
    if (environmentStrength > 0.0)
    {
        vec3 skyUp;
        vec3 sunUp = GetSunAndSkyIrradiance(point, U, L, skyUp);
        vec3 groundRadiance = clamp(oe_sky_groundReflectance, 0.0, 1.0) *
            max(sunUp + skyUp, vec3(0.0)) / PI;
        vec3 groundIrradiance = groundRadiance * PI * clamp(0.5 - 0.5 * dot(N, U), 0.0, 1.0);
        vec3 reflectedEnergy;
        vec3 specular = atmos_environmentSpecular(point, N, V, L, f0, alpha,
            groundRadiance, reflectedEnergy);
        // Share the indirect energy budget between diffuse and specular. A
        // rough dielectric must not lose all diffuse light at grazing angles.
        vec3 kd = (1.0 - clamp(reflectedEnergy, 0.0, 1.0)) * (1.0 - metal);
        vec3 diffuse = kd * albedo * max(skyIrradiance + groundIrradiance, vec3(0.0)) / PI;
        // AO describes local indirect-light visibility, not sunlight intensity.
        indirect = (diffuse + specular) * ao * environmentStrength;
    }
#endif
    // Retain an optional night-time fill in linear space. Never impose a
    // sun-facing brightness floor after tone mapping, or let it bypass shadows.
    indirect += (1.0 - metal) * albedo * atmos_decode(ambient) * (1e5 / PI) * ao;
    return direct + indirect;
}

vec3 atmos_finish(vec3 surface, vec3 transmittance, vec3 scatter)
{
    vec3 radiance = max(surface * transmittance + scatter, vec3(0.0));
    return atmos_encode(1.0 - exp(-radiance * max(oe_sky_exposure, 0.0) * 1e-5));
}
)";

ground_best_vert = R"(
#pragma import_defines(OE_LIGHTING)
#pragma import_defines(OE_NUM_LIGHTS)

// Parameters of each light:
struct osg_LightSourceParameters 
{   
   vec4 ambient;              // Aclarri   
   vec4 diffuse;              // Dcli   
   vec4 specular;             // Scli   
   vec4 position;             // Ppli   
   //vec4 halfVector;           // Derived: Hi   
   vec3 spotDirection;        // Sdli   
   float spotExponent;        // Srli   
   float spotCutoff;          // Crli                              
                              // (range: [0.0,90.0], 180.0)   
   float spotCosCutoff;       // Derived: cos(Crli)                 
                              // (range: [1.0,0.0],-1.0)   
   float constantAttenuation; // K0   
   float linearAttenuation;   // K1   
   float quadraticAttenuation;// K2  

   bool enabled;
};  
uniform osg_LightSourceParameters osg_LightSource[OE_NUM_LIGHTS];

uniform mat4 osg_ViewMatrix;
out vec3 atmos_view_dir;
out vec3 atmos_light_dir;
out vec3 atmos_center_to_camera;
out vec3 atmos_center_to_vert;
out vec3 atmos_ambient;
out vec3 atmos_sun_color;
flat out float atmos_sun_enabled;

void atmos_eb_ground_render_vert(inout vec4 vertex_view)
{
#ifdef OE_LIGHTING
    vec4 temp = osg_ViewMatrix * vec4(0,0,0,1);
    vec3 earth_center = temp.xyz/temp.w;
    atmos_light_dir = normalize(osg_LightSource[0].position.xyz);  // view space
    atmos_ambient = osg_LightSource[0].ambient.rgb;
    atmos_sun_color = osg_LightSource[0].diffuse.rgb;
    atmos_sun_enabled = osg_LightSource[0].enabled ? 1.0 : 0.0;
    const vec3 camera_pos = vec3(0,0,0);
    atmos_center_to_camera = (camera_pos - earth_center) * UNIT_LENGTH_METERS_INVERSE;
    atmos_center_to_vert = (vertex_view.xyz - earth_center) * UNIT_LENGTH_METERS_INVERSE;
    atmos_view_dir = vertex_view.xyz;
#endif
}

)";


ground_best_frag = R"(
#pragma import_defines(OE_LIGHTING)
in vec3 vp_Normal;
in vec3 atmos_view_dir;
in vec3 atmos_light_dir;
in vec3 atmos_center_to_vert;
in vec3 atmos_ambient;
in vec3 atmos_sun_color;
flat in float atmos_sun_enabled;

in vec3 atmos_center_to_camera;
void atmos_eb_ground_render_frag(inout vec4 COLOR)
{
#ifdef OE_LIGHTING

    vec3 U = atmos_normalize(atmos_center_to_vert, vec3(0, 0, 1));
    vec3 N = atmos_normalize(vp_Normal, U);
    if (!gl_FrontFacing) N = -N;
    vec3 V = atmos_normalize(-atmos_view_dir, N);
    vec3 L = atmos_normalize(atmos_light_dir, U);
    vec3 surface = atmos_surfaceRadiance(COLOR.rgb, atmos_center_to_vert, N, V, L,
        atmos_ambient, atmos_sun_color, atmos_sun_enabled);

    vec3 transmittance;
    vec3 scatter = GetSkyRadianceToPoint(atmos_center_to_camera, atmos_center_to_vert,
        0.0, L, transmittance);
    COLOR.rgb = atmos_finish(surface, transmittance, scatter);
#endif
}
)";





ground_fast_vert = R"(
#pragma import_defines(OE_LIGHTING)
#pragma import_defines(OE_NUM_LIGHTS)

struct osg_LightSourceParameters 
{   
   vec4 ambient;              // Aclarri   
   vec4 diffuse;              // Dcli   
   vec4 specular;             // Scli   
   vec4 position;             // Ppli   
   //vec4 halfVector;           // Derived: Hi   
   vec3 spotDirection;        // Sdli   
   float spotExponent;        // Srli   
   float spotCutoff;          // Crli                              
                              // (range: [0.0,90.0], 180.0)   
   float spotCosCutoff;       // Derived: cos(Crli)                 
                              // (range: [1.0,0.0],-1.0)   
   float constantAttenuation; // K0   
   float linearAttenuation;   // K1   
   float quadraticAttenuation;// K2  

   bool enabled;
};  
uniform osg_LightSourceParameters osg_LightSource[OE_NUM_LIGHTS];

out vec3 vp_Normal;

uniform mat4 osg_ViewMatrix;
out vec3 atmos_view_dir;
out vec3 atmos_light_dir;
out vec3 atmos_center_to_vert;
out vec3 atmos_transmittance;
out vec3 atmos_scatter;
out vec3 atmos_ambient;
out vec3 atmos_sun_color;
flat out float atmos_sun_enabled;

uniform float atmos_haze_cutoff;
uniform float atmos_haze_strength;

void atmos_eb_ground_render_vert(inout vec4 vertex_view)
{
#ifdef OE_LIGHTING
    vec4 temp = osg_ViewMatrix * vec4(0,0,0,1);
    vec3 earth_center = temp.xyz/temp.w;
    atmos_light_dir = normalize(osg_LightSource[0].position.xyz);  // view space
    atmos_ambient = osg_LightSource[0].ambient.rgb;
    atmos_sun_color = osg_LightSource[0].diffuse.rgb;
    atmos_sun_enabled = osg_LightSource[0].enabled ? 1.0 : 0.0;
    const vec3 camera_pos = vec3(0,0,0);
    vec3 center_to_camera = (camera_pos - earth_center) * UNIT_LENGTH_METERS_INVERSE;
    atmos_center_to_vert = (vertex_view.xyz - earth_center) * UNIT_LENGTH_METERS_INVERSE;
    atmos_view_dir = vertex_view.xyz;

	vec3 transmittance;
	vec3 in_scatter = GetSkyRadianceToPoint(center_to_camera, atmos_center_to_vert, 0.0, atmos_light_dir, transmittance);

    float vert_unitz = clamp((length(atmos_center_to_vert)-bottom_radius)/(top_radius-bottom_radius), 0, 1);
    float atmos_haze = vert_unitz < atmos_haze_cutoff ? mix(atmos_haze_strength, 1, vert_unitz/atmos_haze_cutoff) : 1.0;

    atmos_transmittance = transmittance;
    atmos_scatter = in_scatter * atmos_haze;
#endif
}

)";


ground_fast_frag= R"(
#pragma import_defines(OE_LIGHTING)
in vec3 vp_Normal;
in vec3 atmos_view_dir;
in vec3 atmos_light_dir;
in vec3 atmos_center_to_vert;
in vec3 atmos_ambient;
in vec3 atmos_sun_color;
flat in float atmos_sun_enabled;

in vec3 atmos_transmittance;
in vec3 atmos_scatter;
void atmos_eb_ground_render_frag(inout vec4 COLOR)
{
#ifdef OE_LIGHTING

    vec3 U = atmos_normalize(atmos_center_to_vert, vec3(0, 0, 1));
    vec3 N = atmos_normalize(vp_Normal, U);
    if (!gl_FrontFacing) N = -N;
    vec3 V = atmos_normalize(-atmos_view_dir, N);
    vec3 L = atmos_normalize(atmos_light_dir, U);
    vec3 surface = atmos_surfaceRadiance(COLOR.rgb, atmos_center_to_vert, N, V, L,
        atmos_ambient, atmos_sun_color, atmos_sun_enabled);

    COLOR.rgb = atmos_finish(surface, atmos_transmittance, atmos_scatter);
#endif
}
)";

sky_vert = R"(
#pragma import_defines(OE_NUM_LIGHTS)

// Parameters of each light:
struct osg_LightSourceParameters 
{   
   vec4 ambient;              // Aclarri   
   vec4 diffuse;              // Dcli   
   vec4 specular;             // Scli   
   vec4 position;             // Ppli   
   //vec4 halfVector;           // Derived: Hi   
   vec3 spotDirection;        // Sdli   
   float spotExponent;        // Srli   
   float spotCutoff;          // Crli                              
                              // (range: [0.0,90.0], 180.0)   
   float spotCosCutoff;       // Derived: cos(Crli)                 
                              // (range: [1.0,0.0],-1.0)   
   float constantAttenuation; // K0   
   float linearAttenuation;   // K1   
   float quadraticAttenuation;// K2  

   bool enabled;
};  
uniform osg_LightSourceParameters osg_LightSource[OE_NUM_LIGHTS];

uniform mat4 osg_ViewMatrix;
out vec3 atmos_view_dir;
out vec3 atmos_light_dir;
out vec3 atmos_transmittance;
out vec3 atmos_center_to_camera;

void atmos_eb_sky_render_vert(inout vec4 vertex_view)
{
    atmos_view_dir = vertex_view.xyz;
    vec4 temp = osg_ViewMatrix * vec4(0,0,0,1);
    vec3 earth_center = temp.xyz/temp.w;
    atmos_light_dir = normalize(osg_LightSource[0].position.xyz);  // view space
    atmos_center_to_camera = -earth_center * UNIT_LENGTH_METERS_INVERSE;
}
)";

sky_frag = R"(
in vec3 atmos_view_dir;
in vec3 atmos_light_dir;
in vec3 atmos_center_to_camera;
const vec3 white_point=vec3(1,1,1);
uniform float oe_sky_exposure;
//uniform vec2 sun_size;

void atmos_eb_sky_render_frag(inout vec4 OUT_COLOR)
{
	vec3 transmittance;
	vec3 radiance = GetSkyRadiance(atmos_center_to_camera, normalize(atmos_view_dir), 0.0, atmos_light_dir, transmittance);

	// If the view ray intersects the Sun, add the Sun radiance.
    // GW: don't need this.
	//if (dot(atmos_view_dir, atmos_light_dir) > sun_size.y) 
	//    radiance = radiance + transmittance * GetSolarRadiance();

	vec3 mapped = 1.0 - exp(-max(radiance, vec3(0.0)) * max(oe_sky_exposure, 0.0) * 1e-5);
    radiance = mix(12.92 * mapped, 1.055 * pow(mapped, vec3(1.0 / 2.4)) - 0.055,
        greaterThan(mapped, vec3(0.0031308)));

	OUT_COLOR = vec4(radiance, 1.0);
}
)";
