layout(local_size_x=8,local_size_y=8) in;
#ifdef OE_CLOUD_SHADOW_PASS
layout(r16f,binding=0) uniform writeonly image2D oe_cloud_shadowOutput;

// Builds a bounded terrain shadow map from the exact procedural field used by the visible clouds.
void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy), size = imageSize(oe_cloud_shadowOutput);
    if (any(greaterThanEqual(pixel,size))) return;
    vec2 xy = ((vec2(pixel)+0.5)/vec2(size)*2.0-1.0)*oe_cloud_shadowOrigin.w;
    vec3 p = oe_cloud_shadowOrigin.xyz+oe_cloud_basis*vec3(xy,0.0);
    p = normalize(p)*(oe_cloud_shell.x+0.001);
    float transmission = dot(oe_cloud_sun,normalize(p)) > 0.02 ?
        oe_cloud_light(p,max(16,oe_cloud_lightSamples*4),true) : 1.0;
    imageStore(oe_cloud_shadowOutput,pixel,vec4(transmission));
}
#else
layout(rgba16f,binding=0) uniform writeonly image3D oe_cloud_output;

// Writes cumulative cloud transport per ray; all distance slices are produced by one invocation, without atomics.
void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec3 size = imageSize(oe_cloud_output);
    if (any(greaterThanEqual(pixel,size.xy))) return;
    vec2 uv = vec2((float(pixel.x)+0.5)/float(size.x),float(pixel.y)/float(size.y-1));
    vec3 direction = oe_cloud_direction(uv);
    if (oe_cloud_screenSpace)
    {
        uv = (vec2(pixel)+0.5)/vec2(size.xy);
        vec4 view = oe_cloud_inverseProjection*vec4(uv*2.0-1.0,0.0,1.0);
        direction = normalize(oe_cloud_viewToEarth*view.xyz);
    }
    vec4 span = oe_cloud_intervals(oe_cloud_eye,direction);
    float total = span.y-span.x+span.w-span.z;
    vec4 transport = vec4(0,0,0,1);
    imageStore(oe_cloud_output,ivec3(pixel,0),transport);
    // Grazing rays cross much more cloud than vertical rays. Bound their spatial sampling error without
    // spending the same extra work on short rays; the multiplier remains capped for predictable cost.
    float grazing = clamp(sqrt(total/max(oe_cloud_shell.z-oe_cloud_shell.y,0.1)),1.0,4.0);
    int viewSamples = int(ceil(float(oe_cloud_samples)*grazing));
    float cosine = dot(direction,oe_cloud_sun);
    float phase = 0.85*oe_cloud_phase(cosine,0.72)+0.15*oe_cloud_phase(cosine,-0.2);
    for (int slice=1; slice<size.z; ++slice)
    {
        float a = float(slice-1)/float(size.z-1), b = float(slice)/float(size.z-1);
        float first = total*a*a, last = total*b*b;
        int count = max(1,int(b*float(viewSamples))-int(a*float(viewSamples)));
        for (int step=0; step<count && total > 1e-5 && transport.a > 0.002; ++step)
        {
            float ds = (last-first)/float(count);
            float along = first+(float(step)+0.5)*ds;
            float distance = oe_cloud_distance(span,along);
            vec3 p = oe_cloud_eye+direction*distance;
            float extinction = oe_cloud_density(p,true);
            if (extinction <= 1e-5) continue;
            float absorbed = transport.a*(1.0-exp(-extinction*ds));
            float sunT = oe_cloud_light(p,oe_cloud_lightSamples,false);
            // Two broad scattering lobes retain illumination inside dense clouds without secondary path tracing.
            vec3 lighting = oe_cloud_sunlight(p)*(phase*sunT+0.04*sqrt(sunT)+0.02*pow(sunT,0.15));
            lighting += oe_cloud_ambient(p)*(0.3+0.7*sunT);
            vec3 airS, airT;
            oe_cloud_air(direction,distance,airS,airT);
            // Integration by parts: C = integral(T_cloud * sigma_cloud * (S_air + T_air * L_cloud)).
            // Then final radiance is (surface*T_air + S_air)*T_cloud + C, so foreground air stays visible.
            transport.rgb += absorbed*(airS+airT*lighting);
            transport.a -= absorbed;
        }
        imageStore(oe_cloud_output,ivec3(pixel,slice),transport);
    }
}
#endif
