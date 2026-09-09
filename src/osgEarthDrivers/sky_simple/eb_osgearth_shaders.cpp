

header = R"(
#version 430
#pragma import_defines(RADIANCE_API_ENABLED)
#pragma import_defines(COMBINED_SCATTERING_TEXTURES)
#pragma import_defines(UNIT_LENGTH_METERS_INVERSE)
)";


pbr = R"(

uniform float oe_sky_maxAmbientIntensity = 0.15;

// https://learnopengl.com/PBR/Lighting

//const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

#ifdef OE_USE_PBR
// fragment stage global PBR params
struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;

void atmos_pbr_spec(in vec3 vertex_dir, in vec3 vert_to_light, in vec3 N, inout vec3 ambience, inout vec3 COLOR)
{
    // PBR (https://learnopengl.com/PBR/Lighting)
    vec3 V = -vertex_dir;
    vec3 L = normalize(vert_to_light);
    vec3 H = normalize(V + L);

    vec3 albedo = COLOR;
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, vec3(oe_pbr.metal));

    // cook-torrance BRDF:
    float NDF = DistributionGGX(N, H, oe_pbr.roughness);
    float G = GeometrySmith(N, V, L, oe_pbr.roughness);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - oe_pbr.metal;

    float NdotL = max(dot(N, L), 0.0);
    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL;
    vec3 specular = numerator / max(denominator, 0.001);

    //ambience *= oe_pbr.ao;

    // ONLY calcuate the specularity here since we are incoporating
    // the radiance, etc. elsewhere.
    vec3 Lo = (kD * albedo + specular*NdotL);

    // Original equation for reference
    //vec3 Lo = (kD * albedo / PI + specular) * radiance * NdotL;
    
    // ambient intensity based on time of day
    ambience = clamp(ambience + vec3(oe_sky_maxAmbientIntensity*NdotL), 0.0, 1.0);

    COLOR = Lo * oe_pbr.ao;
}
#endif
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
out vec3 atmos_vert_to_light;
out vec3 atmos_ambient;

void atmos_eb_ground_render_vert(inout vec4 vertex_view)
{
#ifdef OE_LIGHTING
    vec4 temp = osg_ViewMatrix * vec4(0,0,0,1);
    vec3 earth_center = temp.xyz/temp.w;
    atmos_light_dir = normalize(osg_LightSource[0].position.xyz);  // view space
    atmos_ambient = osg_LightSource[0].ambient.rgb;
    const vec3 camera_pos = vec3(0,0,0);
    atmos_center_to_camera = (camera_pos - earth_center) * UNIT_LENGTH_METERS_INVERSE;
    atmos_center_to_vert = (vertex_view.xyz - earth_center) * UNIT_LENGTH_METERS_INVERSE;
    atmos_view_dir = normalize(vertex_view.xyz);
    atmos_vert_to_light = (osg_LightSource[0].position.xyz - vertex_view.xyz);
#endif
}

)";


ground_best_frag = R"(
#pragma import_defines(OE_LIGHTING)
#pragma import_defines(OE_USE_PBR)

uniform float oe_sky_exposure;
uniform vec2 sun_size;
const vec3 white_point = vec3(1,1,1);
const vec3 camera_pos = vec3(0,0,0);

in vec3 vp_Normal;
in vec3 atmos_view_dir;
in vec3 atmos_light_dir;
in vec3 atmos_center_to_camera;
in vec3 atmos_center_to_vert;
in vec3 atmos_vert_to_light;
in vec3 atmos_ambient;

void atmos_eb_ground_render_frag(inout vec4 COLOR)
{    
#ifdef OE_LIGHTING
    vec3 N = normalize(vp_Normal);
	vec3 sky_irradiance;
	vec3 sun_irradiance = GetSunAndSkyIrradiance(atmos_center_to_vert, N, atmos_light_dir, sky_irradiance);
	vec3 radiance = (1.0 / PI) * (sun_irradiance + sky_irradiance);
	vec3 atmos_transmittance;
	vec3 atmos_scatter = GetSkyRadianceToPoint(atmos_center_to_camera, atmos_center_to_vert, 4.0, atmos_light_dir, atmos_transmittance);
    vec3 ambience = atmos_ambient;

#ifdef OE_USE_PBR
    atmos_pbr_spec(atmos_view_dir, atmos_vert_to_light, N, ambience, COLOR.rgb);
#endif

    vec3 ambient_floor = COLOR.rgb*ambience;

    // apply radiance and atmospheric effects:
    COLOR.rgb = COLOR.rgb * radiance * atmos_transmittance + atmos_scatter;

    // apply white point, exposure, and gamma correction:
	COLOR.rgb = pow(vec3(1,1,1) - exp(-COLOR.rgb / white_point * oe_sky_exposure*1e-5), vec3(1.0 / 2.2));

#ifdef OE_USE_PBR
    // diffuse contrast + brightness
    // COLOR.rgb = ((COLOR.rgb - 0.5)*oe_pbr.contrast + 0.5) * oe_pbr.brightness;
#endif

    // limit to ambient floor:
    COLOR.rgb = max(COLOR.rgb, ambient_floor);
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
out vec3 atmos_vert_to_light;
out vec3 atmos_transmittance;
out vec3 atmos_scatter;
out vec3 atmos_ambient;

uniform float atmos_haze_cutoff;
uniform float atmos_haze_strength;

void atmos_eb_ground_render_vert(inout vec4 vertex_view)
{
#ifdef OE_LIGHTING
    vec4 temp = osg_ViewMatrix * vec4(0,0,0,1);
    vec3 earth_center = temp.xyz/temp.w;
    atmos_light_dir = normalize(osg_LightSource[0].position.xyz);  // view space
    atmos_ambient = osg_LightSource[0].ambient.rgb;
    const vec3 camera_pos = vec3(0,0,0);
    vec3 center_to_camera = (camera_pos - earth_center) * UNIT_LENGTH_METERS_INVERSE;
    atmos_center_to_vert = (vertex_view.xyz - earth_center) * UNIT_LENGTH_METERS_INVERSE;
    atmos_view_dir = normalize(vertex_view.xyz);
    atmos_vert_to_light = (osg_LightSource[0].position.xyz - vertex_view.xyz);

	vec3 transmittance;
	vec3 in_scatter = GetSkyRadianceToPoint(center_to_camera, atmos_center_to_vert, 4.0, atmos_light_dir, transmittance);

    float vert_unitz = clamp((length(atmos_center_to_vert)-bottom_radius)/(top_radius-bottom_radius), 0, 1);
    float atmos_haze = vert_unitz < atmos_haze_cutoff ? mix(atmos_haze_strength, 1, vert_unitz/atmos_haze_cutoff) : 1.0;

    atmos_transmittance = transmittance;
    atmos_scatter = in_scatter * atmos_haze;
#endif
}

)";


ground_fast_frag= R"(
#pragma import_defines(OE_LIGHTING)
#pragma import_defines(OE_USE_PBR)
in vec3 atmos_transmittance;
in vec3 atmos_scatter;
in vec3 atmos_view_dir;
in vec3 atmos_center_to_vert;
in vec3 atmos_light_dir;
in vec3 atmos_vert_to_light;
in vec3 atmos_ambient;
in vec3 vp_Normal;

uniform float oe_sky_exposure;
const vec3 white_point = vec3(1,1,1);

void atmos_eb_ground_render_frag(inout vec4 COLOR)
{
#ifdef OE_LIGHTING
    vec3 N = normalize(vp_Normal);
	vec3 sky_irradiance;
	vec3 sun_irradiance = GetSunAndSkyIrradiance(atmos_center_to_vert, N, atmos_light_dir, sky_irradiance);
    vec3 radiance = (1.0 / PI) * (sun_irradiance + sky_irradiance);
    vec3 ambience = atmos_ambient;

#ifdef OE_USE_PBR
    atmos_pbr_spec(atmos_view_dir, atmos_vert_to_light, N, ambience, COLOR.rgb);
#endif

    vec3 ambient_floor = COLOR.rgb*ambience;

    // apply radiance and atmospheric effects:
    COLOR.rgb = COLOR.rgb * radiance * atmos_transmittance + atmos_scatter;

    // apply white point, exposure, and gamma correction:
	COLOR.rgb = pow(vec3(1,1,1) - exp(-COLOR.rgb / white_point * oe_sky_exposure*1e-5), vec3(1.0 / 2.2));

#ifdef OE_USE_PBR
    // diffuse contrast + brightness
    // COLOR.rgb = ((COLOR.rgb - 0.5)*oe_pbr.contrast + 0.5) * oe_pbr.brightness;
#endif

    // limit to ambient floor:
    COLOR.rgb = max(COLOR.rgb, ambient_floor);

#endif // OE_LIGHTING
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
    atmos_view_dir = normalize(vertex_view.xyz);
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
	vec3 radiance = GetSkyRadiance(atmos_center_to_camera, atmos_view_dir, 0.0, atmos_light_dir, transmittance);

	// If the view ray intersects the Sun, add the Sun radiance.
    // GW: don't need this.
	//if (dot(atmos_view_dir, atmos_light_dir) > sun_size.y) 
	//    radiance = radiance + transmittance * GetSolarRadiance();

	radiance = pow(vec3(1,1,1) - exp(-radiance / white_point * oe_sky_exposure*1e-4), vec3(1.0 / 2.2));

	OUT_COLOR = vec4(radiance, 1.0);
}
)";
