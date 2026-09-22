// Optional cloud-shadowed air and local haze, composed with the host's existing atmospheric solution.
layout(local_size_x=8,local_size_y=8) in;
#ifdef OE_CLOUD_SUN_PASS
layout(r16f,binding=0) uniform writeonly image3D oe_cloud_sunOutput;

// Stores sunlight transmission; filtering optical depth later would close bright gaps between opaque columns.
void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec3 size = imageSize(oe_cloud_sunOutput);
    if (any(greaterThanEqual(pixel,size.xy))) return;
    vec2 xy = oe_cloud_lightPosition((vec2(pixel)+0.5)/vec2(size.xy));
    vec3 origin = oe_cloud_sunOrigin(xy);
    vec4 span = oe_cloud_intervals(origin,-oe_cloud_sun);
    float total = span.y-span.x+span.w-span.z, opticalDepth = 0.0;
    float grazing = clamp(sqrt(total/max(oe_cloud_shell.z-oe_cloud_shell.y,0.1)),1.0,4.0);
    int samples = int(ceil(float(oe_cloud_sunSamples)*grazing));
    imageStore(oe_cloud_sunOutput,ivec3(pixel,0),vec4(1));
    for (int slice=1; slice<size.z; ++slice)
    {
        float a = float(slice-1)/float(size.z-1), b = float(slice)/float(size.z-1);
        int count = max(1,int(b*float(samples))-int(a*float(samples)));
        float ds = total*(b-a)/float(count);
        for (int step=0; step<count && total>0.0 && opticalDepth<16.0; ++step)
        {
            float distance = oe_cloud_distance(span,total*a+(float(step)+0.5)*ds);
            opticalDepth += oe_cloud_density(origin-oe_cloud_sun*distance,true)*ds;
        }
        imageStore(oe_cloud_sunOutput,ivec3(pixel,slice),vec4(exp(-min(opticalDepth,16.0))));
    }
}
#else
layout(rgba16f,binding=0) uniform writeonly image3D oe_cloud_raysOutput;

// Integrates signed air correction and haze transport; cloud radiance and foreground surfaces retain their own depths.
void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec3 size = imageSize(oe_cloud_raysOutput);
    if (any(greaterThanEqual(pixel,size.xy))) return;
    vec3 direction;
    if (oe_cloud_screenSpace)
    {
        vec2 uv = (vec2(pixel)+0.5)/vec2(size.xy);
        vec4 view = oe_cloud_inverseProjection*vec4(uv*2.0-1.0,0.0,1.0);
        direction = normalize(oe_cloud_viewToEarth*view.xyz);
    }
    else direction = oe_cloud_direction(vec2((float(pixel.x)+0.5)/float(size.x),float(pixel.y)/float(size.y-1)));
    float limit = oe_cloud_raySettings.x;
    vec2 ground = oe_cloud_sphere(oe_cloud_eye,direction,oe_cloud_shell.x);
    if (ground.x>0.0) limit = min(limit,ground.x);
    vec3 correction = vec3(0);
    float hazeT = 1.0;
    bool blocked = false;
    imageStore(oe_cloud_raysOutput,ivec3(pixel,0),vec4(0,0,0,1));
    for (int slice=1; slice<size.z; ++slice)
    {
        float a = float(slice-1)/float(size.z-1), b = float(slice)/float(size.z-1);
        float first = oe_cloud_raySettings.x*a*a, last = min(limit,oe_cloud_raySettings.x*b*b);
        int count = max(1,int(b*oe_cloud_raySettings.z)-int(a*oe_cloud_raySettings.z));
        float ds = max(0.0,last-first)/float(count);
        for (int step=0; step<count && ds>0.0 && !blocked; ++step)
        {
            float distance = first+(float(step)+0.5)*ds;
            vec3 p = oe_cloud_eye+direction*distance;
            vec3 source = oe_cloud_airSource(p,direction);
            if (max(source.r,max(source.g,source.b))<=0.0) continue;
            vec4 cloud = oe_cloud_sample(direction,distance);
            // Beyond an opaque cloud, further haze cancels against cumulative cloud radiance in the composite.
            // Stop both correction and extinction here, retaining the same cutoff as the primary cloud march.
            if (cloud.a<=0.002) { blocked=true; break; }
            float sunT = oe_cloud_sunVisibility(p);
            vec3 airS, airT;
            oe_cloud_air(direction,distance,airS,airT);
            float fade = 1.0-smoothstep(0.8,1.0,distance/oe_cloud_raySettings.x);
            // A shallow aerosol layer makes illuminated paths visible; it is active only in sunlit air.
            float haze = oe_cloud_raySettings.w*exp(-max(0.0,length(p)-oe_cloud_shell.x)/2.0)*fade;
            float sunUp = dot(normalize(p),oe_cloud_sun);
            float hazeSunT = exp(-min(20.0,haze*2.0/max(0.05,sunUp)));
            sunT *= hazeSunT;
            float stepT = exp(-haze*ds);
            float integral = haze > 1e-6 ? hazeT*(1.0-stepT)/haze : hazeT*ds;
            vec3 hazeSource = vec3(0);
            if (haze > 0.0)
            {
                float phase = oe_cloud_phase(dot(direction,oe_cloud_sun),0.7);
                hazeSource = 0.9*haze*(oe_cloud_sunlight(p)*(phase*sunT*oe_cloud_rayIntensity)+0.05*oe_cloud_ambient(p));
            }
            // B(s) is cumulative existing air+cloud light. Integration by parts adds integral(H*k*B),
            // preventing haze behind opaque clouds from attenuating the nearer cloud's radiance.
            vec3 base = airS*cloud.a+cloud.rgb;
            correction += integral*(haze*base+airT*cloud.a*(hazeSource-source*((1.0-sunT)*fade)));
            hazeT *= stepT;
        }
        imageStore(oe_cloud_raysOutput,ivec3(pixel,slice),vec4(correction,hazeT));
    }
}
#endif
