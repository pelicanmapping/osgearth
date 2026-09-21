// Shared linear-radiance model. Distances are ellipsoid-scaled kilometers.
const float oe_s2_pi = 3.141592653589793;
const float oe_s2_radius = 6360.0;
const float oe_s2_top = 6460.0;
const vec2 oe_s2_aerialSize = vec2(64.0,32.0);
uniform sampler2D oe_sky2_atmosphere;
uniform sampler2D oe_sky2_view;
uniform sampler2D oe_sky2_environment;
uniform sampler2D oe_sky2_aerial;
uniform vec3 oe_sky2_eye;
uniform vec3 oe_sky2_sun;
uniform vec3 oe_sky2_solarIrradiance;
uniform mat3 oe_sky2_basis;
uniform mat3 oe_sky2_viewToEarth;
uniform mat4 oe_sky2_inverseProjection;
uniform vec4 oe_sky2_settings; // solar irradiance, exposure, environment strength, night ambient
uniform vec4 oe_sky2_flags; // atmosphere, sun, moon, stars
uniform float oe_sky2_horizon;
uniform vec2 oe_sky2_viewSize;
uniform float oe_sky2_aerialSlices;

// Normalizes even degenerate surface normals and half vectors without propagating NaNs.
vec3 oe_s2_normalize(vec3 v, vec3 fallback)
{
    float d = dot(v,v);
    return d > 1e-12 ? v*inversesqrt(d) : fallback;
}

// Returns ordered sphere intersections, or a negative far distance on a miss.
vec2 oe_s2_sphere(vec3 p, vec3 d, float radius)
{
    float b = dot(p,d);
    float c = (length(p)-radius)*(length(p)+radius);
    float h = b*b-c;
    if (h < 0.0) return vec2(0.0,-1.0);
    h = sqrt(h);
    return vec2(-b-h,-b+h);
}

// Clips a unit ray to air, excluding both vacuum and the planet; an empty interval has zero length.
vec2 oe_s2_airInterval(vec3 origin, vec3 direction)
{
    vec2 shell = oe_s2_sphere(origin,direction,oe_s2_top);
    float start = max(0.0,shell.x), finish = shell.y;
    vec2 ground = oe_s2_sphere(origin,direction,oe_s2_radius);
    if (ground.x > 0.0) finish = min(finish,ground.x);
    return vec2(start,max(start,finish));
}

// Signed square-root coordinates concentrate samples near the ray's lowest altitude, from either side.
vec3 oe_s2_rayWarp(vec3 origin, vec3 direction, vec2 interval)
{
    float closest = clamp(-dot(origin,direction),interval.x,interval.y);
    return vec3(-sqrt(max(0.0,closest-interval.x)),sqrt(max(0.0,interval.y-closest)),closest);
}

// Inverts the ray warp continuously, including limb rays whose lowest point lies inside the interval.
float oe_s2_rayDistance(vec3 warp, float fraction)
{
    float q = mix(warp.x,warp.y,fraction);
    return warp.z+q*abs(q);
}

// Blocks light through solid Earth, including cameras in orbit looking past the limb.
bool oe_s2_occluded(float r, float mu)
{
    return mu < 0.0 && r*r*(1.0-mu*mu) < oe_s2_radius*oe_s2_radius;
}

// Samples the static Beer-Lambert LUT with explicit texel-center addressing.
vec3 oe_s2_transmittance(vec3 p, vec3 d)
{
    float r = max(length(p),oe_s2_radius+0.001);
    float mu = dot(p,d)/r;
    if (oe_s2_occluded(r,mu)) return vec3(0.0);
    if (r >= oe_s2_top) return vec3(1.0);
    float H = sqrt(oe_s2_top*oe_s2_top-oe_s2_radius*oe_s2_radius);
    float rho = sqrt(max(0.0,(r-oe_s2_radius)*(r+oe_s2_radius)));
    float distance = max(0.0,-r*mu+sqrt(max(0.0,r*r*(mu*mu-1.0)+oe_s2_top*oe_s2_top)));
    vec2 uv = vec2((distance-(oe_s2_top-r))/(rho+H-(oe_s2_top-r)),rho/H);
    return texture(oe_sky2_atmosphere,(clamp(uv,0.0,1.0)*vec2(255,63)+0.5)/vec2(256,96)).rgb;
}

// Evaluates Rayleigh, aerosol and ozone densities, in inverse kilometers.
void oe_s2_medium(float h, out vec3 extinction, out vec3 rayleigh, out float mie)
{
    h = max(h,0.0);
    rayleigh = vec3(0.005802,0.013558,0.033100)*exp(-h/8.0);
    mie = 0.003996*exp(-h/1.2);
    extinction = rayleigh + vec3(mie/0.9) +
        vec3(0.000650,0.001881,0.000085)*max(0.0,1.0-abs(h-25.0)/15.0);
}

// Samples the isotropic multiple-scattering closure independently of camera position.
vec3 oe_s2_multiple(float r, float sunMu)
{
    vec2 uv = clamp(vec2(sunMu*0.5+0.5,(r-oe_s2_radius)/100.0),0.0,1.0);
    return texture(oe_sky2_atmosphere,(uv*31.0+vec2(0.5,64.5))/vec2(256,96)).rgb;
}

// Integrates a finite ray segment. Used only by bounded LUT passes, never on scene geometry.
vec3 oe_s2_integrate(vec3 origin, vec3 direction, float limit, int count, out vec3 transmittance)
{
    transmittance = vec3(1.0);
    vec2 interval = oe_s2_airInterval(origin,direction);
    float start = interval.x, finish = min(limit,interval.y);
    if (finish <= start) return vec3(0.0);
    vec3 warp = oe_s2_rayWarp(origin,direction,vec2(start,finish));
    float cosine = dot(direction,oe_sky2_sun);
    float phaseR = 3.0*(1.0+cosine*cosine)/(16.0*oe_s2_pi);
    const float g = 0.8;
    float phaseM = (1.0-g*g)/(4.0*oe_s2_pi*pow(max(0.01,1.0+g*g-2.0*g*cosine),1.5));
    vec3 radiance = vec3(0.0);
    for (int i=0; i<count; ++i)
    {
        float a = float(i)/float(count), b = float(i+1)/float(count);
        float ta = oe_s2_rayDistance(warp,a), tb = oe_s2_rayDistance(warp,b);
        vec3 p = origin+direction*(0.5*(ta+tb));
        float r = length(p);
        vec3 extinction, rayleigh;
        float mie;
        oe_s2_medium(r-oe_s2_radius,extinction,rayleigh,mie);
        vec3 source = oe_s2_transmittance(p,oe_sky2_sun)*(rayleigh*phaseR+mie*phaseM);
        source += (rayleigh+mie)*oe_s2_multiple(r,dot(p,oe_sky2_sun)/r);
        vec3 stepT = exp(-extinction*(tb-ta));
        radiance += transmittance*source*(1.0-stepT)/max(extinction,vec3(1e-8));
        transmittance *= stepT;
    }
    return radiance*oe_sky2_solarIrradiance;
}

// Decodes a direction with extra angular precision around the visible planetary horizon.
vec3 oe_s2_skyDirection(vec2 uv)
{
    float y = 2.0*uv.y-1.0;
    float theta = oe_sky2_horizon + sign(y)*y*y*(y < 0.0 ? oe_sky2_horizon : oe_s2_pi-oe_sky2_horizon);
    float phi = uv.x*2.0*oe_s2_pi;
    return oe_sky2_basis*vec3(sin(theta)*cos(phi),sin(theta)*sin(phi),cos(theta));
}

// Encodes direction in a horizon-centered angular grid that retains precision as the planet shrinks on screen.
vec2 oe_s2_skyUV(vec3 direction)
{
    vec3 d = transpose(oe_sky2_basis)*direction;
    float theta = acos(clamp(d.z,-1.0,1.0));
    float delta = theta-oe_sky2_horizon;
    float y = sign(delta)*sqrt(abs(delta)/max(1e-5,delta < 0.0 ? oe_sky2_horizon : oe_s2_pi-oe_sky2_horizon));
    return vec2(fract(atan(d.y,d.x)/(2.0*oe_s2_pi)),0.5+0.5*y);
}

// Samples sky radiance; periodic longitude and texel centers avoid LUT seams.
vec3 oe_s2_sky(vec3 direction)
{
    vec2 uv = oe_s2_skyUV(direction);
    uv.y = (uv.y*(oe_sky2_viewSize.y-1.0)+0.5)/oe_sky2_viewSize.y;
    return texture(oe_sky2_view,uv).rgb;
}

// Reconstructs the view ray without unprojecting the potentially infinite far plane.
vec3 oe_s2_viewRay(vec2 uv)
{
    vec4 v = oe_sky2_inverseProjection*vec4(uv*2.0-1.0,0.0,1.0);
    return oe_s2_normalize(v.xyz,vec3(0,0,-1));
}

// Reads one atlas slice without bleeding into adjacent distances or the transmittance half.
vec3 oe_s2_aerialSlice(vec2 uv, float slice, bool transmittance)
{
    vec2 pixel = clamp(uv,0.0,1.0)*(oe_s2_aerialSize-1.0)+0.5;
    pixel.x += oe_s2_aerialSize.x*slice;
    pixel.y += transmittance ? oe_s2_aerialSize.y : 0.0;
    return texture(oe_sky2_aerial,pixel/(oe_s2_aerialSize*vec2(oe_sky2_aerialSlices,2.0))).rgb;
}

// Reconstructs one elevation row, preserving metric distance near the eye and the atmospheric endpoint in orbit.
void oe_s2_aerialRow(vec2 uv, float depth, float remaining, out vec3 scattering, out vec3 transmittance)
{
    vec3 direction = oe_s2_skyDirection(vec2(0.0,uv.y));
    vec2 interval = oe_s2_airInterval(oe_sky2_eye,direction);
    if (interval.y <= interval.x)
    {
        scattering = vec3(0.0);
        transmittance = vec3(1.0);
        return;
    }
    // Inverse-fourth-distance remapping is monotonic and fixes both endpoints. Its near-eye correction is O(depth^5),
    // avoiding the horizon leak of normalized depth while retaining the dense surface layer in orbital views.
    float ratio = depth/(interval.y-interval.x);
    float ratio2 = ratio*ratio;
    float localDepth = depth/sqrt(sqrt(max(1e-24,remaining+ratio2*ratio2)));
    float distance = interval.x+min(localDepth,interval.y-interval.x);
    vec3 warp = oe_s2_rayWarp(oe_sky2_eye,direction,interval);
    float offset = clamp(distance,interval.x,interval.y)-warp.z;
    float q = sign(offset)*sqrt(abs(offset));
    float fraction = (q-warp.x)/max(1e-6,warp.y-warp.x);
    float z = clamp(fraction,0.0,1.0)*(oe_sky2_aerialSlices-1.0);
    float lo = floor(z), hi = min(lo+1.0,oe_sky2_aerialSlices-1.0);
    float nearDistance = oe_s2_rayDistance(warp,lo/(oe_sky2_aerialSlices-1.0));
    float farDistance = oe_s2_rayDistance(warp,hi/(oe_sky2_aerialSlices-1.0));
    float weight = clamp((distance-nearDistance)/max(1e-6,farDistance-nearDistance),0.0,1.0);
    vec3 nearT = oe_s2_aerialSlice(uv,lo,true), farT = oe_s2_aerialSlice(uv,hi,true);
    vec3 segmentT = clamp(farT/max(nearT,vec3(1e-6)),vec3(1e-6),vec3(1.0));
    vec3 partialT = pow(segmentT,vec3(weight));
    // Beer-Lambert interpolation resolves short paths even when a horizon slice spans optically thick air.
    vec3 scatterWeight = mix(vec3(weight),(1.0-partialT)/max(1.0-segmentT,vec3(1e-4)),
        step(vec3(1e-4),1.0-segmentT));
    scattering = mix(oe_s2_aerialSlice(uv,lo,false),oe_s2_aerialSlice(uv,hi,false),scatterWeight);
    transmittance = nearT*partialT;
}

// Reconstructs finite-distance haze before angular interpolation, so a distant horizon cannot bleed onto geometry.
void oe_s2_aerial(vec3 direction, float distance, out vec3 scattering, out vec3 transmittance)
{
    vec2 interval = oe_s2_airInterval(oe_sky2_eye,direction);
    if (distance <= interval.x || interval.y <= interval.x)
    {
        scattering = vec3(0.0);
        transmittance = vec3(1.0);
        return;
    }
    float depth = min(distance,interval.y)-interval.x;
    float fraction = depth/(interval.y-interval.x);
    float fraction2 = fraction*fraction;
    float remaining = max(0.0,1.0-fraction2*fraction2);
    vec2 uv = oe_s2_skyUV(direction);
    if (remaining == 0.0)
    {
        // The complete atmospheric endpoint is shared by all rows; opaque ground needs only two reads.
        scattering = oe_s2_aerialSlice(uv,oe_sky2_aerialSlices-1.0,false);
        transmittance = oe_s2_aerialSlice(uv,oe_sky2_aerialSlices-1.0,true);
        return;
    }
    float row = clamp(uv.y,0.0,1.0)*(oe_s2_aerialSize.y-1.0);
    float lo = floor(row), hi = min(lo+1.0,oe_s2_aerialSize.y-1.0);
    vec3 scatterLo, scatterHi, transmitLo, transmitHi;
    oe_s2_aerialRow(vec2(uv.x,lo/(oe_s2_aerialSize.y-1.0)),depth,remaining,scatterLo,transmitLo);
    oe_s2_aerialRow(vec2(uv.x,hi/(oe_s2_aerialSize.y-1.0)),depth,remaining,scatterHi,transmitHi);
    scattering = mix(scatterLo,scatterHi,fract(row));
    transmittance = mix(transmitLo,transmitHi,fract(row));
}

// Samples a cosine-convolved or GGX-prefiltered local environment (6 specular + 1 diffuse rows).
vec3 oe_s2_environment(vec3 earthDirection, float level)
{
    vec3 d = transpose(oe_sky2_basis)*earthDirection;
    vec2 uv = vec2(fract(atan(d.y,d.x)/(2.0*oe_s2_pi)),acos(clamp(d.z,-1.0,1.0))/oe_s2_pi);
    float lo = floor(level), hi = min(6.0,lo+1.0);
    float y = clamp(uv.y,0.0,1.0)*31.0+0.5;
    vec3 a = texture(oe_sky2_environment,vec2(uv.x,(y+lo*32.0)/224.0)).rgb;
    vec3 b = texture(oe_sky2_environment,vec2(uv.x,(y+hi*32.0)/224.0)).rgb;
    return mix(a,b,fract(level));
}

// Converts linear HDR radiance once, after lighting and atmospheric compositing.
vec3 oe_s2_output(vec3 radiance)
{
    vec3 x = max(vec3(0.0),radiance*oe_sky2_settings.y);
#ifdef OE_SKY2_TONEMAP
    // Smooth filmic shoulder, shared by surfaces and celestial background.
    x = clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0.0,1.0);
#endif
#ifdef OE_SKY2_SRGB
    x = mix(12.92*x,1.055*pow(x,vec3(1.0/2.4))-0.055,step(vec3(0.0031308),x));
#endif
    return x;
}
