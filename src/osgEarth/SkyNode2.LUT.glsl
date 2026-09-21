// Rendered by the private LUT cameras. Source is prefixed with the common model and pass defines.
in vec2 oe_s2_uv;
out vec4 oe_s2_result;

// Evaluates one texel of sky radiance, aerial perspective, or a filtered environment.
void main()
{
#if defined(OE_SKY2_SKY_PASS)
    vec2 uv = oe_s2_uv;
    uv.y = clamp((gl_FragCoord.y-0.5)/(oe_sky2_viewSize.y-1.0),0.0,1.0);
    vec3 direction = oe_s2_skyDirection(uv);
    vec3 transmission;
    vec3 radiance = oe_s2_integrate(oe_sky2_eye,direction,1e8,OE_SKY2_SAMPLES,transmission);
    vec2 ground = oe_s2_sphere(oe_sky2_eye,direction,oe_s2_radius);
    if (ground.x > 0.0)
    {
        vec3 p = oe_sky2_eye+direction*ground.x;
        radiance += transmission*0.1*oe_sky2_solarIrradiance/oe_s2_pi*
            oe_s2_transmittance(p*1.000001,oe_sky2_sun)*max(0.0,dot(normalize(p),oe_sky2_sun));
    }
    oe_s2_result = vec4(radiance,1.0);
#elif defined(OE_SKY2_AERIAL_PASS)
    float slice = floor(gl_FragCoord.x/oe_s2_aerialSize.x);
    vec2 uv = (mod(gl_FragCoord.xy,oe_s2_aerialSize)-0.5)/(oe_s2_aerialSize-1.0);
    vec3 direction = oe_s2_skyDirection(uv);
    vec2 interval = oe_s2_airInterval(oe_sky2_eye,direction);
    vec3 warp = oe_s2_rayWarp(oe_sky2_eye,direction,interval);
    float distance = oe_s2_rayDistance(warp,slice/(oe_sky2_aerialSlices-1.0));
    vec3 transmission;
    vec3 radiance = oe_s2_integrate(oe_sky2_eye,direction,distance,OE_SKY2_SAMPLES,transmission);
    oe_s2_result = vec4(gl_FragCoord.y >= oe_s2_aerialSize.y ? transmission : radiance,1.0);
#else
    float level = floor(gl_FragCoord.y/32.0);
    vec2 uv = vec2(oe_s2_uv.x,(mod(gl_FragCoord.y,32.0)-0.5)/31.0);
    float theta = uv.y*oe_s2_pi, phi = uv.x*2.0*oe_s2_pi;
    vec3 N = oe_sky2_basis*vec3(sin(theta)*cos(phi),sin(theta)*sin(phi),cos(theta));
    vec3 tangent = normalize(cross(abs(N.z)<0.99 ? vec3(0,0,1) : vec3(1,0,0),N));
    vec3 bitangent = cross(N,tangent);
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    float alpha = max(0.001,(level/5.0)*(level/5.0));
    for (int i=0; i<OE_SKY2_FILTER_SAMPLES; ++i)
    {
        // Deterministic equal-area sequence; no per-frame random noise or temporal history.
        float u = (float(i)+0.5)/float(OE_SKY2_FILTER_SAMPLES);
        float azimuth = float(i)*2.399963229728653;
        float cosine = level > 5.5 ? sqrt(1.0-u) : sqrt((1.0-u)/(1.0+(alpha*alpha-1.0)*u));
        float sine = sqrt(max(0.0,1.0-cosine*cosine));
        vec3 H = tangent*(sine*cos(azimuth))+bitangent*(sine*sin(azimuth))+N*cosine;
        vec3 L = level > 5.5 ? H : reflect(-N,H);
        float w = level > 5.5 ? 1.0 : max(0.0,dot(N,L));
        sum += oe_s2_sky(L)*w;
        weight += w;
    }
    oe_s2_result = vec4(sum/max(weight,1e-6),1.0);
#endif
}
