uniform mat3 oe_cloud_sunBasis;
uniform sampler3D oe_cloud_sunVolume;
uniform int oe_cloud_sunSamples;

// A logarithmic light grid preserves nearby openings while retaining the full bounded integration range.
vec2 oe_cloud_lightPosition(vec2 uv)
{
    float focus=min(2.0,oe_cloud_raySettings.x*0.25);
    vec2 q=uv*2.0-1.0;
    return sign(q)*focus*(exp(abs(q)*log(1.0+oe_cloud_raySettings.x/focus))-1.0);
}

// Inverts the near-focused light grid without adding another texture or per-view allocation.
vec2 oe_cloud_lightUV(vec2 xy)
{
    float focus=min(2.0,oe_cloud_raySettings.x*0.25);
    return 0.5+0.5*sign(xy)*log(1.0+abs(xy)/focus)/log(1.0+oe_cloud_raySettings.x/focus);
}

// Locates a sunward shell entry for one light-space column, including curved-Earth horizon columns.
vec3 oe_cloud_sunOrigin(vec2 xy)
{
    vec3 p = oe_cloud_eye+oe_cloud_sunBasis*vec3(xy,0.0);
    vec2 top = oe_cloud_sphere(p,oe_cloud_sun,oe_cloud_shell.x+oe_cloud_shell.z);
    return p+oe_cloud_sun*(top.y+0.01);
}

// Filters transmission across columns, with Beer interpolation along depth for partial in-cloud columns.
float oe_cloud_sunVisibility(vec3 p)
{
    float height=length(p)-oe_cloud_shell.x;
    // Above the deck, outward sunlight cannot cross cloud; below it, the entire cached column applies.
    if (height>=oe_cloud_shell.z && dot(p,oe_cloud_sun)>=0.0) return 1.0;
    vec2 xy = (transpose(oe_cloud_sunBasis)*(p-oe_cloud_eye)).xy;
    vec2 uv = oe_cloud_lightUV(xy);
    float edge = max(abs(xy.x),abs(xy.y))/oe_cloud_raySettings.x;
    if (edge>=1.0) return 1.0;
    if (height<=oe_cloud_shell.y)
        return mix(texture(oe_cloud_sunVolume,vec3(uv,1.0)).r,1.0,smoothstep(0.8,1.0,edge));
    vec3 origin = oe_cloud_sunOrigin(xy);
    vec4 span = oe_cloud_intervals(origin,-oe_cloud_sun);
    float total = span.y-span.x+span.w-span.z;
    if (total<=1e-5) return 1.0;
    float distance = dot(origin-p,oe_cloud_sun);
    float along = clamp(distance-span.x,0.0,span.y-span.x)+clamp(distance-span.z,0.0,span.w-span.z);
    float depth = float(textureSize(oe_cloud_sunVolume,0).z);
    float z = clamp(along/total,0.0,1.0)*(depth-1.0);
    float lo = floor(z), hi = min(lo+1.0,depth-1.0);
    float nearT = texture(oe_cloud_sunVolume,vec3(uv,(lo+0.5)/depth)).r;
    float farT = texture(oe_cloud_sunVolume,vec3(uv,(hi+0.5)/depth)).r;
    float transmission = exp(mix(log(max(nearT,1e-7)),log(max(farT,1e-7)),fract(z)));
    return mix(transmission,1.0,smoothstep(0.8,1.0,edge));
}

