#pragma vp_entryPoint oe_sky_environment_init
#pragma vp_location fragment_lighting
#pragma vp_order 0.75

uniform sampler3D oe_sky_environmentTex;
uniform sampler2D oe_sky_brdfTex;
uniform vec3 oe_sky_irradiance[9];
uniform mat4 osg_ViewMatrixInverse;

// Reference radiance for the probe. Legacy ambient controls must not mute
// the environment; oe_sky_iblStrength blends its contribution separately.
const float oe_sky_environmentIntensity = 0.15;

void oe_sky_environment_init(inout vec4 color) { }

vec3 oe_sky_environmentRadiance(vec3 d, float roughness)
{
    const float pi=3.14159265359;
    vec3 size=vec3(textureSize(oe_sky_environmentTex,0));
    // atan(0,0) is undefined; the pole rows are constant in longitude.
    float longitude=dot(d.xy,d.xy)>0.0 ? atan(d.y,d.x)/(2.0*pi) : 0.0;
    float latitude=acos(clamp(d.z,-1.0,1.0))/pi;
    // Latitude includes both poles, and roughness includes both endpoints.
    // Map those endpoints to texel centers instead of texture boundaries.
    vec3 uvw=vec3(longitude,
        (latitude*(size.y-1.0)+0.5)/size.y,
        (clamp(roughness,0.0,1.0)*(size.z-1.0)+0.5)/size.z);
    return texture(oe_sky_environmentTex,uvw).rgb;
}

vec3 oe_sky_diffuseIrradiance(vec3 n)
{
    return max(vec3(0.0),
        oe_sky_irradiance[0]*0.282095 +
        oe_sky_irradiance[1]*(0.488603*n.y) +
        oe_sky_irradiance[2]*(0.488603*n.z) +
        oe_sky_irradiance[3]*(0.488603*n.x) +
        oe_sky_irradiance[4]*(1.092548*n.x*n.y) +
        oe_sky_irradiance[5]*(1.092548*n.y*n.z) +
        oe_sky_irradiance[6]*(0.315392*(3.0*n.z*n.z-1.0)) +
        oe_sky_irradiance[7]*(1.092548*n.x*n.z) +
        oe_sky_irradiance[8]*(0.546274*(n.x*n.x-n.y*n.y)));
}

vec3 oe_sky_environment(vec3 N, vec3 V, vec3 albedo, float roughness, float metal, float ao)
{
    roughness=clamp(roughness,0.04,1.0);
    metal=clamp(metal,0.0,1.0);
    float nv=max(dot(N,V),0.0);
    vec3 worldN=normalize(mat3(osg_ViewMatrixInverse)*N);
    vec3 worldR=normalize(mat3(osg_ViewMatrixInverse)*reflect(-V,N));
    vec3 f0=mix(vec3(0.04),albedo,metal);
    vec3 fresnel=f0+(max(vec3(1.0-roughness),f0)-f0)*pow(1.0-nv,5.0);
    vec3 kd=(1.0-fresnel)*(1.0-metal);
    vec3 diffuse=kd*albedo*oe_sky_diffuseIrradiance(worldN);
    vec2 brdf=texture(oe_sky_brdfTex,vec2(nv,roughness)).rg;
    vec3 radiance=oe_sky_environmentRadiance(worldR,roughness);
    vec3 specular=radiance*(f0*brdf.x+brdf.y);
    return (diffuse+specular)*clamp(ao,0.0,1.0)*oe_sky_environmentIntensity;
}
