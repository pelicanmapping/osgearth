#pragma vp_entryPoint oe_sky2_vertex
#pragma vp_location vertex_view

// Activates VirtualProgram's view-space position/normal stage, including its instancing overrides.
void oe_sky2_vertex(inout vec4 vertex) { }

[break]

#pragma vp_entryPoint oe_sky2_lighting
#pragma vp_location fragment_lighting
#pragma vp_order 0.8
#pragma import_defines(OE_LIGHTING, OE_NUM_LIGHTS, OE_SHADOWING)
#pragma import_defines(OE_SKY2_ATMOSPHERE, OE_SKY2_SRGB, OE_SKY2_TONEMAP)
#pragma include SkyNode2.Common.glsl

in vec3 vp_Normal;
in vec3 vp_VertexView;
uniform int oe_sky2_sunIndex;
struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;
struct osg_LightSourceParameters
{
    vec4 ambient, diffuse, specular, position;
    vec3 spotDirection;
    float spotExponent, spotCutoff, spotCosCutoff;
    float constantAttenuation, linearAttenuation, quadraticAttenuation;
    bool enabled;
};
uniform osg_LightSourceParameters osg_LightSource[OE_NUM_LIGHTS];
struct osg_MaterialParameters
{
    vec4 emission, ambient, diffuse, specular;
    float shininess;
};
uniform osg_MaterialParameters osg_FrontMaterial;
#ifdef OE_SHADOWING
float oe_shadow_visibility;
#endif

// Schlick Fresnel evaluated without a general exponent instruction.
vec3 oe_s2_fresnel(float cosine, vec3 f0)
{
    float x = 1.0-clamp(cosine,0.0,1.0), x2 = x*x;
    return f0+(1.0-f0)*(x2*x2*x);
}

// Energy-conserving Lambert + GGX with height-correlated Smith visibility.
vec3 oe_s2_brdf(vec3 N, vec3 V, vec3 L, vec3 albedo, vec3 f0, float metal, float a2, float nv, float nl)
{
    vec3 H = oe_s2_normalize(V+L,N);
    float nh = max(0.0,dot(N,H));
    float denominator = nh*nh*(a2-1.0)+1.0;
    float D = a2/(oe_s2_pi*denominator*denominator);
    float visibility = 0.5/max(1e-6,nl*sqrt(nv*nv*(1.0-a2)+a2)+nv*sqrt(nl*nl*(1.0-a2)+a2));
    vec3 F = oe_s2_fresnel(dot(V,H),f0);
    return ((1.0-F)*(1.0-metal)*albedo/oe_s2_pi + D*visibility*F)*nl;
}

// Applies all direct lights, indirect sky, then aerial perspective in linear HDR space.
void oe_sky2_lighting(inout vec4 color)
{
#ifdef OE_LIGHTING
    vec3 albedo = mix(max(color.rgb,0.0)/12.92,
        pow((max(color.rgb,0.0)+0.055)/1.055,vec3(2.4)),step(vec3(0.04045),color.rgb));
    vec3 N = oe_s2_normalize(vp_Normal,vec3(0,0,1));
    vec3 V = oe_s2_normalize(-vp_VertexView,N);
    float roughness = clamp(oe_pbr.roughness,0.045,1.0);
    float metal = clamp(oe_pbr.metal,0.0,1.0), ao = clamp(oe_pbr.ao,0.0,1.0);
    vec3 f0 = mix(vec3(0.04),albedo,metal);
    float nv = max(1e-4,dot(N,V)), alpha = roughness*roughness, a2 = alpha*alpha;
    vec3 earthPosition = oe_sky2_eye+oe_sky2_viewToEarth*vp_VertexView;
    float r = max(length(earthPosition),oe_s2_radius+0.001);
    // Terrain below the reference ellipsoid must still see the sky.
    earthPosition *= max(1.0,(oe_s2_radius+0.001)/max(length(earthPosition),1.0));
    vec3 radiance = max(osg_FrontMaterial.emission.rgb,vec3(0.0));
    for (int i=0; i<OE_NUM_LIGHTS; ++i)
    {
        if (!osg_LightSource[i].enabled) continue;
        bool sun = i == oe_sky2_sunIndex;
        vec4 position = osg_LightSource[i].position;
        vec3 delta = position.xyz-vp_VertexView*position.w;
        vec3 L = oe_s2_normalize(delta,N);
        float nl = max(0.0,dot(N,L));
        if (nl <= 0.0) continue;
        float attenuation = 1.0;
        if (position.w != 0.0)
        {
            float distance = length(delta)/abs(position.w);
            attenuation = 1.0/max(1e-6,osg_LightSource[i].constantAttenuation+
                distance*(osg_LightSource[i].linearAttenuation+distance*osg_LightSource[i].quadraticAttenuation));
            if (osg_LightSource[i].spotCutoff <= 90.0)
            {
                float cosine = dot(-L,oe_s2_normalize(osg_LightSource[i].spotDirection,vec3(0,0,-1)));
                // Derive the cutoff from degrees; legacy generators sometimes upload cos(degrees).
                attenuation *= cosine < cos(radians(osg_LightSource[i].spotCutoff)) ? 0.0 :
                    pow(max(cosine,0.0),max(osg_LightSource[i].spotExponent,0.0));
            }
        }
        vec3 light = osg_LightSource[i].diffuse.rgb*attenuation;
        if (sun)
        {
            vec3 direction = normalize(oe_sky2_viewToEarth*L);
            light *= oe_sky2_settings.x;
#ifdef OE_SKY2_ATMOSPHERE
            light *= oe_s2_transmittance(earthPosition,direction);
#else
            if (oe_s2_occluded(r,dot(earthPosition,direction)/r)) light = vec3(0.0);
#endif
#ifdef OE_SHADOWING
            light *= oe_shadow_visibility;
#endif
        }
        if (max(light.r,max(light.g,light.b)) > 0.0)
            radiance += light*oe_s2_brdf(N,V,L,albedo,f0,metal,a2,nv,nl);
    }
    vec3 F = oe_s2_fresnel(nv,f0);
    vec3 diffuse = albedo*(1.0-metal)*(1.0-F);
    radiance += oe_sky2_settings.w*diffuse*ao;
    if (oe_sky2_settings.z > 0.0)
    {
#ifdef OE_SKY2_ATMOSPHERE
        // The two uniform rotations are composed once per view instead of twice per fragment.
        vec3 en = normalize(oe_sky2_viewToSky*N);
        vec3 reflection = normalize(oe_sky2_viewToSky*reflect(-V,N));
        vec3 irradiance = oe_s2_environment(en,6.0);
        vec3 specular = oe_s2_environment(reflection,roughness*5.0);
        // Analytic split-sum DFG approximation (Karis), no extra BRDF texture.
        vec4 q = roughness*vec4(-1.0,-0.0275,-0.572,0.022)+vec4(1.0,0.0425,1.04,-0.04);
        float a = min(q.x*q.x,exp2(-9.28*nv))*q.x+q.y;
        vec2 dfg = vec2(-1.04,1.04)*a+q.zw;
        radiance += oe_sky2_settings.z*ao*(diffuse*irradiance+specular*max(vec3(0.0),f0*dfg.x+dfg.y));
#else
        // Atmosphere-free environment retains a finite reflection for metallic materials.
        radiance += oe_sky2_settings.z*0.03*ao*(diffuse+F);
#endif
    }
#ifdef OE_SKY2_ATMOSPHERE
    if (oe_sky2_flags.x > 0.5)
    {
        vec3 ray = oe_sky2_viewToEarth*vp_VertexView;
        vec3 scattering, transmission;
        oe_s2_aerial(oe_s2_normalize(ray,vec3(0,0,1)),length(ray),scattering,transmission);
        radiance = radiance*transmission+scattering;
    }
#endif
    color.rgb = oe_s2_output(radiance);
#endif
}
