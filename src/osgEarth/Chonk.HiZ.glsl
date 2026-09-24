#version 460
#extension GL_ARB_bindless_texture : require

// Builds one level of Chonk's occlusion pyramid. Every texel keeps the farthest depth of the
// texels it covers: level 0 halves the copied depth buffer, each later level halves the one
// before. Sizes round down like GL mipmaps, so the last row/column absorbs an odd remainder.
layout(local_size_x = 8, local_size_y = 8) in;

layout(bindless_sampler) uniform sampler2D oe_hiz_depth;             // single-sample depth copy
layout(r32f, bindless_image) readonly uniform image2D oe_hiz_source; // previous level
layout(r32f, bindless_image) writeonly uniform image2D oe_hiz_target;
uniform ivec2 oe_hiz_source_size;
uniform ivec2 oe_hiz_target_size;
uniform int oe_hiz_from_depth; // 1 when building level 0

void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(p, oe_hiz_target_size)))
        return;
    ivec2 lo = p*2;
    ivec2 hi = min(lo + 1, oe_hiz_source_size - 1);
    if (p.x == oe_hiz_target_size.x - 1) hi.x = oe_hiz_source_size.x - 1;
    if (p.y == oe_hiz_target_size.y - 1) hi.y = oe_hiz_source_size.y - 1;
    float farthest = 0.0;
    for (int y = lo.y; y <= hi.y; ++y)
    {
        for (int x = lo.x; x <= hi.x; ++x)
        {
            farthest = max(farthest, oe_hiz_from_depth != 0 ?
                texelFetch(oe_hiz_depth, ivec2(x, y), 0).r :
                imageLoad(oe_hiz_source, ivec2(x, y)).r);
        }
    }
    imageStore(oe_hiz_target, p, vec4(farthest));
}
