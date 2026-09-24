#ifdef OE_CLOUD_LAYER
uniform bool oe_cloud_enabled;
uniform float oe_cloud_fade;
uniform sampler3D oe_cloud_volume;
uniform sampler2D oe_cloud_shadowMap;
uniform vec3 oe_cloud_eye;
uniform mat3 oe_cloud_basis;
uniform vec4 oe_cloud_shell; // planet radius, base/top height, horizon angle; kilometers/radians
uniform vec4 oe_cloud_shape; // coverage, extinction per km, scale in km, erosion
uniform vec4 oe_cloud_grid; // width, height, depth, shadow strength
uniform vec3 oe_cloud_sun;
uniform vec3 oe_cloud_wind;
uniform vec4 oe_cloud_advection; // east/north sine components, cosine, shared WindLayer active
uniform vec3 oe_cloud_seed;
uniform vec4 oe_cloud_shadowOrigin; // center xyz and half width in km
uniform vec4 oe_cloud_shadowFarOrigin; // anchored tangent-plane center and half width in km; zero width disables
uniform mat3 oe_cloud_shadowFarBasis;
uniform bool oe_cloud_screenSpace;
uniform mat3 oe_cloud_viewToEarth, oe_cloud_earthToView;
uniform mat4 oe_cloud_projection, oe_cloud_inverseProjection;
uniform bool oe_cloud_raysEnabled;
uniform sampler3D oe_cloud_raysVolume;
uniform vec4 oe_cloud_raySettings; // range in km, strength, integration samples, surface haze extinction per km
uniform vec3 oe_cloud_rayGrid;
uniform float oe_cloud_rayIntensity;

// Stable ordered sphere roots in the host's scaled planetary coordinates.
vec2 oe_cloud_sphere(vec3 p, vec3 d, float radius)
{
    float b = dot(p,d), r = length(p);
    float h = b*b-(r-radius)*(r+radius);
    return h < 0.0 ? vec2(0.0,-1.0) : vec2(-b-sqrt(h),-b+sqrt(h));
}

// Returns both visible shell intervals, excluding the hollow interior and solid planet.
vec4 oe_cloud_intervals(vec3 p, vec3 d)
{
    vec2 top = oe_cloud_sphere(p,d,oe_cloud_shell.x+oe_cloud_shell.z);
    float a = max(0.0,top.x), b = max(a,top.y);
    vec2 ground = oe_cloud_sphere(p,d,oe_cloud_shell.x);
    if (ground.x > 0.0) b = max(a,min(b,ground.x));
    vec2 base = oe_cloud_sphere(p,d,oe_cloud_shell.x+oe_cloud_shell.y);
    if (base.y <= a || base.x >= b) return vec4(a,b,b,b);
    return vec4(a,clamp(base.x,a,b),clamp(base.y,a,b),b);
}

// Maps cumulative occupied-shell length to physical ray distance, skipping the hollow interior.
float oe_cloud_distance(vec4 span, float lengthInCloud)
{
    float first = span.y-span.x;
    return lengthInCloud <= first ? span.x+lengthInCloud : span.z+lengthInCloud-first;
}

// Reconstructs the horizon-focused angular direction; rows include both poles and longitude wraps.
vec3 oe_cloud_direction(vec2 uv)
{
    float y = uv.y*2.0-1.0, h = oe_cloud_shell.w;
    float theta = h+sign(y)*y*y*(y < 0.0 ? h : 3.14159265359-h);
    float phi = uv.x*6.28318530718;
    return oe_cloud_basis*vec3(sin(theta)*cos(phi),sin(theta)*sin(phi),cos(theta));
}

// Inverse angular encoding used by sky and arbitrary finite-distance geometry.
vec2 oe_cloud_uv(vec3 direction)
{
    vec3 d = transpose(oe_cloud_basis)*direction;
    float h = oe_cloud_shell.w, delta = acos(clamp(d.z,-1.0,1.0))-h;
    float y = sign(delta)*sqrt(abs(delta)/max(1e-5,delta < 0.0 ? h : 3.14159265359-h));
    return vec2(fract(atan(d.y,d.x)/6.28318530718),0.5+0.5*y);
}

// Samples one row at metric distance; exponential interpolation preserves Beer transmission between slices.
vec4 oe_cloud_row(float azimuth, float row, float distance)
{
    float v = row/(oe_cloud_grid.y-1.0);
    vec4 span = oe_cloud_intervals(oe_cloud_eye,oe_cloud_direction(vec2(0.0,v)));
    float total = span.y-span.x+span.w-span.z;
    float along = clamp(distance-span.x,0.0,span.y-span.x)+clamp(distance-span.z,0.0,span.w-span.z);
    if (total < 1e-5 || along <= 0.0) return vec4(0,0,0,1);
    float f = clamp(along/total,0.0,1.0);
    float z = sqrt(f)*(oe_cloud_grid.z-1.0), lo = floor(z), hi = min(lo+1.0,oe_cloud_grid.z-1.0);
    float a = lo/(oe_cloud_grid.z-1.0), b = hi/(oe_cloud_grid.z-1.0);
    float weight = clamp((f-a*a)/max(b*b-a*a,1e-8),0.0,1.0);
    vec3 uv = vec3(azimuth,(row+0.5)/oe_cloud_grid.y,(lo+0.5)/oe_cloud_grid.z);
    vec4 nearValue = texture(oe_cloud_volume,uv);
    uv.z = (hi+0.5)/oe_cloud_grid.z;
    vec4 farValue = texture(oe_cloud_volume,uv);
    float segment = clamp(farValue.a/max(nearValue.a,1e-6),1e-6,1.0);
    float partial = pow(segment,weight);
    float scatterWeight = segment < 0.9999 ? (1.0-partial)/(1.0-segment) : weight;
    return vec4(mix(nearValue.rgb,farValue.rgb,scatterWeight),nearValue.a*partial);
}

// Returns cumulative cloud correction and transmission without introducing cloud in front of its exact entry point.
vec4 oe_cloud_sample(vec3 direction, float distance)
{
    if (!oe_cloud_enabled) return vec4(0,0,0,1);
    vec4 span = oe_cloud_intervals(oe_cloud_eye,direction);
    float entry = span.y > span.x ? span.x : span.z;
    if (distance <= entry || span.w <= entry) return vec4(0,0,0,1);
    if (oe_cloud_screenSpace)
    {
        float total = span.y-span.x+span.w-span.z;
        float along = clamp(distance-span.x,0.0,span.y-span.x)+clamp(distance-span.z,0.0,span.w-span.z);
        float fraction = clamp(along/max(total,1e-5),0.0,1.0);
        float z = sqrt(fraction)*(oe_cloud_grid.z-1.0), lo = floor(z), hi = min(lo+1.0,oe_cloud_grid.z-1.0);
        float a = lo/(oe_cloud_grid.z-1.0), b = hi/(oe_cloud_grid.z-1.0);
        float weight = clamp((fraction-a*a)/max(b*b-a*a,1e-8),0.0,1.0);
        vec4 clip = oe_cloud_projection*vec4(oe_cloud_earthToView*direction,0.0);
        vec2 uv = clamp(clip.xy/max(clip.w,1e-8)*0.5+0.5,0.0,1.0);
        vec4 nearValue = texture(oe_cloud_volume,vec3(uv,(lo+0.5)/oe_cloud_grid.z));
        vec4 farValue = texture(oe_cloud_volume,vec3(uv,(hi+0.5)/oe_cloud_grid.z));
        float segment = clamp(farValue.a/max(nearValue.a,1e-6),1e-6,1.0);
        float partial = pow(segment,weight);
        float scatterWeight = segment < 0.9999 ? (1.0-partial)/(1.0-segment) : weight;
        return vec4(mix(nearValue.rgb,farValue.rgb,scatterWeight),nearValue.a*partial);
    }
    vec2 uv = oe_cloud_uv(direction);
    float row = clamp(uv.y,0.0,1.0)*(oe_cloud_grid.y-1.0);
    return mix(oe_cloud_row(uv.x,floor(row),distance),
        oe_cloud_row(uv.x,min(floor(row)+1.0,oe_cloud_grid.y-1.0),distance),fract(row));
}

// Reconstructs signed scattering correction and haze transmission at a receiver's metric distance.
vec4 oe_cloud_rays(vec3 direction, float distance)
{
    if (!oe_cloud_enabled || !oe_cloud_raysEnabled || distance <= 0.0) return vec4(0,0,0,1);
    vec2 uv;
    if (oe_cloud_screenSpace)
    {
        vec4 clip = oe_cloud_projection*vec4(oe_cloud_earthToView*direction,0.0);
        uv = clamp(clip.xy/max(clip.w,1e-8)*0.5+0.5,0.0,1.0);
    }
    else
    {
        uv = oe_cloud_uv(direction);
        uv.y = (uv.y*(oe_cloud_rayGrid.y-1.0)+0.5)/oe_cloud_rayGrid.y;
    }
    float f = clamp(distance/oe_cloud_raySettings.x,0.0,1.0);
    float z = sqrt(f)*(oe_cloud_rayGrid.z-1.0), lo = floor(z), hi = min(lo+1.0,oe_cloud_rayGrid.z-1.0);
    float a = lo/(oe_cloud_rayGrid.z-1.0), b = hi/(oe_cloud_rayGrid.z-1.0);
    float weight = clamp((f-a*a)/max(b*b-a*a,1e-8),0.0,1.0);
    vec4 nearValue = texture(oe_cloud_raysVolume,vec3(uv,(lo+0.5)/oe_cloud_rayGrid.z));
    vec4 farValue = texture(oe_cloud_raysVolume,vec3(uv,(hi+0.5)/oe_cloud_rayGrid.z));
    float segment = clamp(farValue.a/max(nearValue.a,1e-6),1e-6,1.0), partial = pow(segment,weight);
    float scatterWeight = segment < 0.9999 ? (1.0-partial)/(1.0-segment) : weight;
    return vec4(mix(nearValue.rgb,farValue.rgb,scatterWeight),nearValue.a*partial);
}

// Composes cloud transport, shadowed air, and optional haze once, before tone mapping and altitude fading.
vec3 oe_cloud_apply(vec3 radiance, vec3 direction, float distance)
{
    vec4 cloud = oe_cloud_sample(direction,distance);
    vec3 result = radiance*cloud.a+cloud.rgb;
    if (oe_cloud_raysEnabled)
    {
        vec4 rays = oe_cloud_rays(direction,distance);
        result = mix(result,max(vec3(0),result*rays.a+rays.rgb),oe_cloud_raySettings.y);
    }
    return mix(radiance,result,oe_cloud_fade);
}

// Samples one atlas tile without bilinear leakage into its neighbor; applies receiver height before cascade blending.
float oe_cloud_shadowTile(vec2 uv, bool farTile, float remaining)
{
    float tiles = oe_cloud_shadowFarOrigin.w > 0.0 ? 2.0 : 1.0;
    vec2 size = vec2(textureSize(oe_cloud_shadowMap,0))/vec2(tiles,1.0);
    uv = clamp(uv,0.5/size,1.0-0.5/size);
    uv.x = (uv.x+(farTile ? 1.0 : 0.0))/tiles;
    return pow(max(texture(oe_cloud_shadowMap,uv).r,1e-5),remaining);
}

// Blends near and optional far transmission over the near edge, then fades the outermost cascade to sunlight.
float oe_cloud_shadow(vec3 position)
{
    if (!oe_cloud_enabled || oe_cloud_grid.w <= 0.0) return 1.0;
    float height = length(position)-oe_cloud_shell.x;
    if (height >= oe_cloud_shell.z) return 1.0;
    vec3 up = oe_cloud_basis[2];
    float sunUp = dot(oe_cloud_sun,up);
    if (sunUp <= 0.02) return 1.0;
    vec3 onGround = position-oe_cloud_sun*height/sunUp;
    vec3 local = transpose(oe_cloud_basis)*(onGround-oe_cloud_shadowOrigin.xyz);
    vec2 uv = local.xy/(2.0*oe_cloud_shadowOrigin.w)+0.5;
    float edge = max(abs(uv.x-0.5),abs(uv.y-0.5))*2.0;
    float nearWeight = 1.0-smoothstep(0.8,1.0,edge);
    float remaining = clamp((oe_cloud_shell.z-height)/(oe_cloud_shell.z-oe_cloud_shell.y),0.0,1.0);
    float shadow = 1.0;
    if (nearWeight < 1.0 && oe_cloud_shadowFarOrigin.w > 0.0)
    {
        // Invert the radial projection used during generation, including the fixed tangent plane's curvature offset.
        vec3 normal = oe_cloud_shadowFarBasis[2];
        vec3 plane = onGround*(dot(oe_cloud_shadowFarOrigin.xyz,normal)/max(dot(onGround,normal),1e-4));
        vec3 farLocal = transpose(oe_cloud_shadowFarBasis)*(plane-oe_cloud_shadowFarOrigin.xyz);
        vec2 farUV = farLocal.xy/(2.0*oe_cloud_shadowFarOrigin.w)+0.5;
        float farEdge = max(abs(farUV.x-0.5),abs(farUV.y-0.5))*2.0;
        if (farEdge < 1.0)
            shadow = mix(1.0,oe_cloud_shadowTile(farUV,true,remaining),1.0-smoothstep(0.8,1.0,farEdge));
    }
    if (nearWeight > 0.0) shadow = mix(shadow,oe_cloud_shadowTile(uv,false,remaining),nearWeight);
    return mix(1.0,shadow,oe_cloud_fade*oe_cloud_grid.w);
}
#endif
