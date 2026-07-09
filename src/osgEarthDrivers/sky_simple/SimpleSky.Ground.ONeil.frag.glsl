#pragma vp_entryPoint atmos_fragment_main
#pragma vp_location   fragment_lighting
#pragma vp_order      0.8

#pragma import_defines(OE_LIGHTING)
#pragma import_defines(OE_NUM_LIGHTS)
#pragma import_defines(OE_USE_PBR)

uniform float oe_sky_exposure = 3.3; // HDR scene exposure (ground level)
uniform float oe_sky_ambientBoostFactor; // ambient sunlight booster for daytime (material mode only)
uniform float oe_sky_maxAmbientIntensity = 0.75; // maximum daytime ambient intensity (PBR mode only)

in vec3 atmos_lightDir;    // light direction (view coords)
in vec3 atmos_color;       // atmospheric lighting color
//in vec3 atmos_atten;     // atmospheric lighting attenuation factor (not currently written by the VS)
in vec3 atmos_up;          // earth up vector at fragment (in view coords)
in float atmos_space;      // camera altitude (0=ground, 1=atmos outer radius)
in vec3 vp_Normal; // surface normal (from osgEarth)
in vec3 vp_VertexView; // from osgEarth

// frag stage global PBR parameters
#ifdef OE_USE_PBR
// fragment stage global PBR parameters.
struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;
#endif

// Parameters of each light:
struct osg_LightSourceParameters
{
   vec4 ambient;
   vec4 diffuse;
   vec4 specular;
   vec4 position;
   vec3 spotDirection;
   float spotExponent;
   float spotCutoff;
   float spotCosCutoff;
   float constantAttenuation;
   float linearAttenuation;
   float quadraticAttenuation;

   bool enabled;
};
uniform osg_LightSourceParameters osg_LightSource[OE_NUM_LIGHTS];

// Surface material:
struct osg_MaterialParameters
{
   vec4 emission;    // Ecm
   vec4 ambient;     // Acm
   vec4 diffuse;     // Dcm
   vec4 specular;    // Scm
   float shininess;  // Srm
};
uniform osg_MaterialParameters osg_FrontMaterial;

// https://learnopengl.com/PBR/Lighting

const float PI = 3.14159265359;

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

float GeometrySchlickGGX(float NdotX, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float nom = NdotX;
    float denom = NdotX * (1.0 - k) + k;

    return nom / denom;
}

//float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    //float NdotV = max(dot(N, V), 0.0);
    //float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

const float oe_wrap = 0.22;
uniform float oe_normal_boost = 1.0;

#ifdef OE_USE_PBR
void atmos_fragment_main_pbr(inout vec4 color)
{
#ifndef OE_LIGHTING
    return;
#endif

    // SRGB to linear for PBR compute:
    vec3 albedo = pow(color.rgb, vec3(2.2));

    vec3 N = normalize(vp_Normal);
    vec3 V = normalize(-vp_VertexView);

    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, vec3(oe_pbr.metal));

    vec3 Lo = vec3(0.0);
    float ai = 0.0; // ambient intensity (based on time of day)

    for (int i = 0; i < OE_NUM_LIGHTS; ++i)
    {
        // per-light radiance:
        vec3 L = normalize(osg_LightSource[i].position.xyz - vp_VertexView);
        vec3 H = normalize(V + L);
        //float distance = length(osg_LightSource[i].position.xyz - atmos_vert);
        //float attenuation = 1.0 / (distance * distance);
        vec3 radiance = osg_LightSource[i].diffuse.rgb; // * attenuation

        float NdotL = max(dot(N, L), 0.0);

        // wrap diffuse:
        // https://developer.nvidia.com/gpugems/gpugems/part-iii-materials/chapter-16-real-time-approximations-subsurface-scattering
        float NdotL_wrap = max(0.0, (NdotL + oe_wrap) / (1.0 + oe_wrap));

        float NdotV = max(0.0, dot(N, V));

        // cook-torrance BRDF:
        float NDF = DistributionGGX(N, H, oe_pbr.roughness);
        float G = GeometrySmith(NdotV, NdotL_wrap, oe_pbr.roughness);
        vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - oe_pbr.metal;

        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL_wrap;
        vec3 specular = NdotL > 0.0 ? numerator / max(denominator, 0.001) : vec3(0.0);

        // daytime metric
        float day = i == 0 ? max(0.0, dot(atmos_up, L)) : 1.0;

        // color contribution
        Lo += (kD * albedo / PI + specular) * radiance * NdotL_wrap * day;

        // ambient intesntity contribution
        ai = max(ai, NdotL_wrap * oe_sky_maxAmbientIntensity) * day;
    }

    vec3 ambient = clamp(osg_LightSource[0].ambient.rgb + vec3(ai), 0.0, 1.0) * albedo * oe_pbr.ao;

    color.rgb = ambient + Lo;

    // tone map:
    color.rgb = color.rgb / (color.rgb + vec3(1.0));

    // add in the haze
    color.rgb += pow(atmos_color, vec3(2.2)); // add in the (SRGB) color

    // exposure:
    color.rgb = 1.0 - exp(-oe_sky_exposure * color.rgb);

    // brightness and contrast
    //color.rgb = ((color.rgb - 0.5)*oe_pbr.contrast + 0.5) * oe_pbr.brightness;

    // linear back to SRGB
    color.rgb = pow(color.rgb, vec3(1.0/2.2));
}

#else

void atmos_fragment_material(inout vec4 color)
{
    // See:
    // https://en.wikipedia.org/wiki/Phong_reflection_model
    // https://www.opengl.org/sdk/docs/tutorials/ClockworkCoders/lighting.php
    // https://en.wikibooks.org/wiki/GLSL_Programming/GLUT/Multiple_Lights
    // https://en.wikibooks.org/wiki/GLSL_Programming/GLUT/Specular_Highlights

    // normal vector at vertex
    vec3 N = normalize(vp_Normal);
    //vec3 N = normalize(gl_FrontFacing ? vp_Normal : -vp_Normal);

    float shine = clamp(osg_FrontMaterial.shininess, 1.0, 128.0);
    vec4 surfaceSpecularity = osg_FrontMaterial.specular;

    // up vector at vertex
    vec3 U = normalize(atmos_up);

    // Accumulate the lighting for each component separately.
    vec3 totalDiffuse = vec3(0.0);
    vec3 totalAmbient = vec3(0.0);
    vec3 totalSpecular = vec3(0.0);

    int numLights = OE_NUM_LIGHTS;

    for (int i=0; i<numLights; ++i)
    {
        if (osg_LightSource[i].enabled)
        {
            float attenuation = 1.0;

            // L is the normalized camera-to-light vector.
            vec3 L = normalize(osg_LightSource[i].position.xyz);

            // V is the normalized vertex-to-camera vector.
            vec3 V = -normalize(vp_VertexView);

            // point or spot light:
            if (osg_LightSource[i].position.w != 0.0)
            {
                // VLu is the unnormalized vertex-to-light vector
                vec3 Lu = osg_LightSource[i].position.xyz - vp_VertexView;

                // calculate attenuation:
                float distance = length(Lu);
                attenuation = 1.0 / (
                    osg_LightSource[i].constantAttenuation +
                    osg_LightSource[i].linearAttenuation * distance +
                    osg_LightSource[i].quadraticAttenuation * distance * distance);

                // for a spot light, the attenuation help form the cone:
                if (osg_LightSource[i].spotCutoff <= 90.0)
                {
                    vec3 D = normalize(osg_LightSource[i].spotDirection);
                    float clampedCos = max(0.0, dot(-L,D));
                    attenuation = clampedCos < osg_LightSource[i].spotCosCutoff ?
                        0.0 :
                        attenuation * pow(clampedCos, osg_LightSource[i].spotExponent);
                }
            }

            // a term indicating whether it's daytime for light 0 (the sun).
            float dayTerm = i==0? dot(U,L) : 1.0;

            // This term boosts the ambient lighting for the sun (light 0) when it's daytime.
            float ambientBoost = i==0? 1.0 + oe_sky_ambientBoostFactor*clamp(2.0*(dayTerm-0.5), 0.0, 1.0) : 1.0;

            vec3 ambientReflection =
                attenuation
                * osg_LightSource[i].ambient.rgb
                * ambientBoost;

            float NdotL = max(dot(N,L), 0.0);

            // this term, applied to light 0 (the sun), attenuates the diffuse light
            // during the nighttime, so that geometry doesn't get lit based on its
            // normals during the night.
            float diffuseAttenuation = clamp(dayTerm+0.35, 0.0, 1.0);

            vec3 diffuseReflection =
                attenuation
                * diffuseAttenuation
                * osg_LightSource[i].diffuse.rgb
                * NdotL;

            vec3 specularReflection = vec3(0.0);
            if (NdotL > 0.0)
            {
                // prevent a sharp edge where NdotL becomes positive
                // by fading in the spec between (0.0 and 0.1)
                float specAttenuation = clamp(NdotL*10.0, 0.0, 1.0);

                vec3 H = reflect(-L,N);
                float HdotV = max(dot(H,V), 0.0);

                specularReflection =
                      specAttenuation
                    * attenuation
                    * osg_LightSource[i].specular.rgb
                    * surfaceSpecularity.rgb
                    * pow(HdotV, shine);
            }

            totalDiffuse += diffuseReflection;
            totalAmbient += ambientReflection;
            totalSpecular += specularReflection;
        }
    }

    // add the atmosphere color, and incorpoate the lights.
    color.rgb += atmos_color;

    vec3 lightColor =
        osg_FrontMaterial.emission.rgb +
        totalDiffuse * osg_FrontMaterial.diffuse.rgb +
        totalAmbient * osg_FrontMaterial.ambient.rgb;

    color.rgb =
        color.rgb * lightColor +
        totalSpecular; // * osg_FrontMaterial.specular.rgb;

    // Simulate HDR by applying an exposure factor (1.0 is none, 2-3 are reasonable)
    color.rgb = 1.0 - exp(-oe_sky_exposure * 0.33 * color.rgb);
}

#endif


void atmos_fragment_main(inout vec4 color)
{
#ifndef OE_LIGHTING
    return;
#endif

#ifdef OE_USE_PBR
    atmos_fragment_main_pbr(color);
#else
    atmos_fragment_material(color);
#endif
}
