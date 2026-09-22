#pragma vp_name Stable Shadow Receiver Vertex
#pragma vp_entryPoint oe_shadow_vertex
#pragma vp_location vertex_view
#pragma vp_order last

out vec3 oe_shadow_view;

// Interpolate one view-space position regardless of cascade count.
void oe_shadow_vertex(inout vec4 vertex)
{
    oe_shadow_view = vertex.xyz;
}

[break]
#pragma vp_name Stable Shadow Receiver Fragment
#pragma vp_entryPoint oe_shadow_fragment
#pragma vp_location fragment_lighting
#pragma vp_order 0.1
#pragma import_defines(OE_LIGHTING, OE_IS_SHADOW_CAMERA, OE_IS_DEPTH_CAMERA, OE_IS_PICK_CAMERA)

uniform sampler2DArrayShadow oe_shadow_map;
uniform mat4 oe_shadow_matrix[$OE_SHADOW_NUM_SLICES];
uniform vec2 oe_shadow_interval[$OE_SHADOW_NUM_SLICES];
uniform float oe_shadow_bias[$OE_SHADOW_NUM_SLICES];
uniform float oe_shadow_color;
uniform float oe_shadow_blur;
uniform int oe_shadow_filter;
uniform float oe_shadow_blend;
uniform vec3 oe_shadow_light;
in vec3 oe_shadow_view;
in vec3 vp_Normal;

// Shared lighting contract: only the direct solar contribution consumes this visibility.
float oe_shadow_visibility;

// Comparison happens before bilinear interpolation; ordinary filtered depth cannot implement PCF.
float oe_shadow_compare(vec3 coordinate, int layer, vec2 offset)
{
    return texture(oe_shadow_map,vec4(coordinate.xy+offset,float(layer),coordinate.z));
}

// Integrates a box or tent over a texel cell, keeping continuous coverage as the filter crosses texel boundaries.
vec2 oe_shadow_cellWeight(vec2 offset, float radius)
{
    vec2 lower = clamp((offset-0.5)/radius,vec2(-1.0),vec2(1.0));
    vec2 upper = clamp((offset+0.5)/radius,vec2(-1.0),vec2(1.0));
    if (oe_shadow_filter == 2)
        return upper*(1.0-0.5*abs(upper))-lower*(1.0-0.5*abs(lower));
    return 0.5*(upper-lower);
}

// Combines adjacent texel weights into one bilinear comparison per axis; XY is location and ZW is weight.
vec4 oe_shadow_filterPair(vec2 base, vec2 position, float radius, float index, vec2 texelSize)
{
    vec2 first = base+index*2.0;
    vec2 a = oe_shadow_cellWeight(first-position,radius);
    vec2 b = oe_shadow_cellWeight(first+1.0-position,radius);
    vec2 weight = a+b;
    return vec4((first+0.5+b/max(weight,vec2(1e-6)))*texelSize,weight);
}

// Samples a filled footprint instead of spreading sparse taps into visibly separate shadow silhouettes.
float oe_shadow_sample(int layer, float slope)
{
    vec3 c = (oe_shadow_matrix[layer]*vec4(oe_shadow_view,1.0)).xyz;
    if (any(lessThan(c,vec3(0.0))) || any(greaterThan(c,vec3(1.0)))) return 1.0;
    c.z -= oe_shadow_bias[layer]*slope;
    if (oe_shadow_filter == 0 || oe_shadow_blur <= 0.0)
        return oe_shadow_compare(c,layer,vec2(0.0));
    vec2 mapSize = vec2(textureSize(oe_shadow_map,0).xy);
    vec2 texelSize = 1.0/mapSize;
    float radius = clamp(oe_shadow_blur*mapSize.x,0.5,8.0);
    if (radius <= 0.5) return oe_shadow_compare(c,layer,vec2(0.0));
    vec2 position = c.xy*mapSize-0.5;
    vec2 base = floor(position-radius+0.5);
    vec4 a = oe_shadow_filterPair(base,position,radius,0.0,texelSize);
    vec4 b = oe_shadow_filterPair(base,position,radius,1.0,texelSize);
    // The default radius covers at most four texels per axis: retain a four-comparison fast path.
    if (radius <= 1.5)
    {
        return a.z*a.w*oe_shadow_compare(c,layer,a.xy-c.xy)+
            b.z*a.w*oe_shadow_compare(c,layer,vec2(b.x,a.y)-c.xy)+
            a.z*b.w*oe_shadow_compare(c,layer,vec2(a.x,b.y)-c.xy)+
            b.z*b.w*oe_shadow_compare(c,layer,b.xy-c.xy);
    }
    // Wide user-selected filters need additional samples; up to nine pairs cover the maximum eight-texel radius.
    int count = int(ceil(radius+0.5));
    vec4 taps[9];
    taps[0] = a;
    taps[1] = b;
    for (int i=2; i<count; ++i)
        taps[i] = oe_shadow_filterPair(base,position,radius,float(i),texelSize);
    float visibility = 0.0;
    for (int y=0; y<count; ++y)
    for (int x=0; x<count; ++x)
        visibility += taps[x].z*taps[y].w*oe_shadow_compare(c,layer,vec2(taps[x].x,taps[y].y)-c.xy);
    return visibility;
}

// Select one depth interval, sample its neighbor only in the transition, and fade at the coverage limit.
void oe_shadow_fragment(inout vec4 color)
{
    oe_shadow_visibility = 1.0;
#if defined(OE_LIGHTING) && !defined(OE_IS_SHADOW_CAMERA) && !defined(OE_IS_DEPTH_CAMERA) && !defined(OE_IS_PICK_CAMERA)
    float depth = -oe_shadow_view.z;
    if (depth < oe_shadow_interval[0].x || depth >= oe_shadow_interval[$OE_SHADOW_NUM_SLICES-1].y) return;
    int layer = 0;
    for (int i=0; i<$OE_SHADOW_NUM_SLICES-1; ++i)
        if (depth >= oe_shadow_interval[i].y) layer = i+1;
    vec3 N = vp_Normal*inversesqrt(max(dot(vp_Normal,vp_Normal),1e-8));
    float cosine = clamp(dot(N,oe_shadow_light),0.0,1.0);
    float slope = 1.0+min(3.0,sqrt(max(0.0,1.0-cosine*cosine))/max(cosine,0.1));
    float visibility = oe_shadow_sample(layer,slope);
    vec2 interval = oe_shadow_interval[layer];
    float width = (interval.y-interval.x)*oe_shadow_blend;
    if (width > 0.0 && depth > interval.y-width)
    {
        float next = 1.0;
        if (layer+1 < $OE_SHADOW_NUM_SLICES) next = oe_shadow_sample(layer+1,slope);
        visibility = mix(visibility,next,smoothstep(interval.y-width,interval.y,depth));
    }
    oe_shadow_visibility = mix(clamp(oe_shadow_color,0.0,1.0),1.0,visibility);
#endif
}
