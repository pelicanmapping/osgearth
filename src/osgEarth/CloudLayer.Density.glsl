uniform sampler3D oe_cloud_noise;
uniform int oe_cloud_samples;
uniform int oe_cloud_lightSamples;

// Seam-free Earth-fixed procedural weather and eroded 3D density; returns extinction per scaled kilometer.
float oe_cloud_density(vec3 p, bool detail)
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
    float weather = textureLod(oe_cloud_noise,q*0.125,0.0).r;
    float coverage = clamp(oe_cloud_shape.x*(0.55+weather),0.0,1.0);
    vec3 noise = textureLod(oe_cloud_noise,q,0.0).rgb;
    float shape = noise.r*0.65+noise.g*0.35;
    float profile = smoothstep(0.0,0.12,h)*(1.0-smoothstep(0.55,1.0,h));
    float density = smoothstep(1.0-coverage,1.2-coverage,shape)*profile;
    if (detail && density > 0.0)
    {
        float erosion = textureLod(oe_cloud_noise,q*4.0,0.0).g;
        density = max(0.0,density-(1.0-erosion)*oe_cloud_shape.w*(1.0-density));
    }
    return density*oe_cloud_shape.y;
}

// Bounded secondary march; terrain shadows use visible density, while repeated self-shadow rays can omit erosion.
float oe_cloud_light(vec3 p, int samples, bool detail)
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
        opticalDepth += oe_cloud_density(p+oe_cloud_sun*t,detail)*total*(b*b-a*a);
    }
    return exp(-min(opticalDepth,40.0));
}

// Normalized Henyey-Greenstein phase function with a bounded denominator at the solar direction.
float oe_cloud_phase(float cosine, float g)
{
    return (1.0-g*g)/(12.56637061436*pow(max(0.001,1.0+g*g-2.0*g*cosine),1.5));
}
