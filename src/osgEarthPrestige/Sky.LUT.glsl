// Rendered by the private LUT cameras. Source is prefixed with the common model and pass defines.
in vec2 oe_psky_uv;
out vec4 oe_psky_result;

// Evaluates one texel of sky radiance, aerial perspective, or a filtered environment.
void main()
{
#if defined(OE_PRESTIGE_SKY_SKY_PASS)
    vec2 uv = oe_psky_uv;
    uv.y = clamp((gl_FragCoord.y-0.5)/(oe_prestige_sky_viewSize.y-1.0),0.0,1.0);
    vec3 direction = oe_psky_skyDirection(uv);
    vec3 transmission;
    vec3 radiance = oe_psky_integrate(oe_prestige_sky_eye,direction,1e8,OE_PRESTIGE_SKY_SAMPLES,transmission);
    vec2 ground = oe_psky_sphere(oe_prestige_sky_eye,direction,oe_psky_radius);
    if (ground.x > 0.0)
    {
        vec3 p = oe_prestige_sky_eye+direction*ground.x;
        radiance += transmission*0.1*oe_prestige_sky_solarIrradiance/oe_psky_pi*
            oe_psky_transmittance(p*1.000001,oe_prestige_sky_sun)*max(0.0,dot(normalize(p),oe_prestige_sky_sun));
    }
    oe_psky_result = vec4(radiance,1.0);
#elif defined(OE_PRESTIGE_SKY_AERIAL_PASS)
    float slice = floor(gl_FragCoord.x/oe_psky_aerialSize.x);
    vec2 uv = (mod(gl_FragCoord.xy,oe_psky_aerialSize)-0.5)/(oe_psky_aerialSize-1.0);
    vec3 direction = oe_psky_skyDirection(uv);
    vec2 interval = oe_psky_airInterval(oe_prestige_sky_eye,direction);
    vec3 warp = oe_psky_rayWarp(oe_prestige_sky_eye,direction,interval);
    float distance = oe_psky_rayDistance(warp,slice/(oe_prestige_sky_aerialSlices-1.0));
    vec3 transmission;
    vec3 radiance = oe_psky_integrate(oe_prestige_sky_eye,direction,distance,OE_PRESTIGE_SKY_SAMPLES,transmission);
    oe_psky_result = vec4(gl_FragCoord.y >= oe_psky_aerialSize.y ? transmission : radiance,1.0);
#else
    float level = floor(gl_FragCoord.y/32.0);
    vec2 uv = vec2(oe_psky_uv.x,(mod(gl_FragCoord.y,32.0)-0.5)/31.0);
    float theta = uv.y*oe_psky_pi, phi = uv.x*2.0*oe_psky_pi;
    vec3 N = oe_prestige_sky_basis*vec3(sin(theta)*cos(phi),sin(theta)*sin(phi),cos(theta));
    vec3 tangent = normalize(cross(abs(N.z)<0.99 ? vec3(0,0,1) : vec3(1,0,0),N));
    vec3 bitangent = cross(N,tangent);
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    float alpha = max(0.001,(level/5.0)*(level/5.0));
    for (int i=0; i<OE_PRESTIGE_SKY_FILTER_SAMPLES; ++i)
    {
        // Deterministic equal-area sequence; no per-frame random noise or temporal history.
        float u = (float(i)+0.5)/float(OE_PRESTIGE_SKY_FILTER_SAMPLES);
        float azimuth = float(i)*2.399963229728653;
        float cosine = level > 5.5 ? sqrt(1.0-u) : sqrt((1.0-u)/(1.0+(alpha*alpha-1.0)*u));
        float sine = sqrt(max(0.0,1.0-cosine*cosine));
        vec3 H = tangent*(sine*cos(azimuth))+bitangent*(sine*sin(azimuth))+N*cosine;
        vec3 L = level > 5.5 ? H : reflect(-N,H);
        float w = level > 5.5 ? 1.0 : max(0.0,dot(N,L));
        vec3 sky = oe_psky_sky(L);
#ifdef OE_CLOUD_LAYER
        sky = oe_cloud_apply(sky,L,1e8);
#endif
        sum += sky*w;
        weight += w;
    }
    oe_psky_result = vec4(sum/max(weight,1e-6),1.0);
#endif
}
