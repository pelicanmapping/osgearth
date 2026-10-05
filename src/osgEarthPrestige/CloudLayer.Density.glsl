uniform sampler3D oe_cloud_noise;
uniform int oe_cloud_samples;
uniform int oe_cloud_lightSamples;
uniform bool oe_cloud_detailFiltering;

// Integrates the clipped erosion response with three variance-matched samples, retaining wisps below the mean cutoff.
float oe_cloud_response(float shape, float coverage, float profile, vec3 erosion)
{
    float density = smoothstep(1.0-coverage,1.2-coverage,shape)*profile;
    vec3 eroded = max(vec3(0),density-erosion*(1.0-density));
    return dot(eroded,vec3(4.0/9.0,5.0/18.0,5.0/18.0));
}

// Filters the nonlinear density response, not just its input mean, to approximately retain unresolved cloud coverage.
float oe_cloud_filteredDensity(float shape, float sigma, vec4 detail, float lod, float coverage, float profile)
{
    float erosionSigma = sqrt(max(0.0,detail.a-detail.g*detail.g)*smoothstep(0.0,1.0,lod+2.0));
    vec3 erosion = oe_cloud_shape.w*(1.0-clamp(detail.g+erosionSigma*vec3(0,1.3416408,-1.3416408),0.0,1.0));
    if (lod <= 0.0) return oe_cloud_response(shape,coverage,profile,erosion);
    // Three-point quadrature of a bounded uniform distribution with the measured mean and variance.
    return (4.0/9.0)*oe_cloud_response(shape,coverage,profile,erosion)+
        (5.0/18.0)*(oe_cloud_response(shape-1.3416408*sigma,coverage,profile,erosion)+
                    oe_cloud_response(shape+1.3416408*sigma,coverage,profile,erosion));
}

// Seam-free advected density; footprint is the sample width in scaled kilometers, with zero preserving point sampling.
float oe_cloud_density(vec3 p, bool detail, float footprint)
{
    float h = (length(p)-oe_cloud_shell.x-oe_cloud_shell.y)/(oe_cloud_shell.z-oe_cloud_shell.y);
    if (h <= 0.0 || h >= 1.0 || oe_cloud_shape.x <= 0.0) return 0.0;
    vec3 weatherPosition = p;
    if (oe_cloud_advection.w > 0.5)
    {
        float radius = length(p);
        vec3 up = p/radius;
        vec3 east = dot(p.xy,p.xy) > 1e-8 ? normalize(vec3(-p.y,p.x,0)) : vec3(0,1,0);
        vec3 north = cross(up,east);
        weatherPosition = p*oe_cloud_advection.z-radius*(east*oe_cloud_advection.x+north*oe_cloud_advection.y);
    }
    vec3 q = (weatherPosition-oe_cloud_wind)/(oe_cloud_shape.z*4.0)+oe_cloud_seed;
    // Trilinear mip taps span neighboring texels; use the sample radius to avoid overfiltering cloud boundaries.
    float lod = log2(max(0.5*footprint*float(textureSize(oe_cloud_noise,0).x)/(oe_cloud_shape.z*4.0),1e-8));
    float weather = textureLod(oe_cloud_noise,q*0.125,max(0.0,lod-3.0)).r;
    float coverage = clamp(oe_cloud_shape.x*(0.55+weather),0.0,1.0);
    vec4 noise = textureLod(oe_cloud_noise,q,max(0.0,lod));
    float shape = noise.r*0.65+noise.g*0.35;
    float profile = smoothstep(0.0,0.12,h)*(1.0-smoothstep(0.55,1.0,h));
    if (lod > -2.0)
    {
        float sigma = sqrt(max(0.0,noise.b-shape*shape)*smoothstep(0.0,1.0,lod));
        // Erosion only removes density; skip its texture and response when even the upper shape tail is empty.
        if (shape+1.3416408*sigma <= 1.0-coverage) return 0.0;
        vec4 erosion = detail ? textureLod(oe_cloud_noise,q*4.0,max(0.0,lod+2.0)) : vec4(1);
        return oe_cloud_filteredDensity(shape,sigma,erosion,lod,coverage,profile)*oe_cloud_shape.y;
    }
    float density = smoothstep(1.0-coverage,1.2-coverage,shape)*profile;
    if (detail && density > 0.0)
    {
        float erosion = textureLod(oe_cloud_noise,q*4.0,0.0).g;
        density = max(0.0,density-(1.0-erosion)*oe_cloud_shape.w*(1.0-density));
    }
    return density*oe_cloud_shape.y;
}

// Point-sampled density remains available to the existing shadow and sunlight marches.
float oe_cloud_density(vec3 p, bool detail)
{
    return oe_cloud_density(p,detail,0.0);
}

// Bounded secondary march; a nonzero footprint filters unresolved far-cascade density in kilometers.
float oe_cloud_light(vec3 p, int samples, bool detail, float footprint)
{
    vec4 span = oe_cloud_intervals(p,oe_cloud_sun);
    float total = span.y-span.x+span.w-span.z;
    if (total <= 0.0) return 1.0;
    float opticalDepth = 0.0;
    for (int i=0; i<samples; ++i)
    {
        float a = float(i)/float(samples), b = float(i+1)/float(samples);
        // Concentrate samples near the shading point, where self-shadowing detail matters most.
        float t = oe_cloud_distance(span,total*0.5*(a*a+b*b));
        opticalDepth += oe_cloud_density(p+oe_cloud_sun*t,detail,footprint)*total*(b*b-a*a);
    }
    return exp(-min(opticalDepth,40.0));
}

// Keeps near shadows and repeated self-shadow rays on the original point-sampled path.
float oe_cloud_light(vec3 p, int samples, bool detail)
{
    return oe_cloud_light(p,samples,detail,0.0);
}

// Normalized Henyey-Greenstein phase function with a bounded denominator at the solar direction.
float oe_cloud_phase(float cosine, float g)
{
    return (1.0-g*g)/(12.56637061436*pow(max(0.001,1.0+g*g-2.0*g*cosine),1.5));
}
