#ifndef OE_PBR_MATERIAL_GLSL
#define OE_PBR_MATERIAL_GLSL

// Keep layout values in sync with PBRMaterial::Layout.
#define OE_PBR_LAYOUT_DRAM 0.0
#define OE_PBR_LAYOUT_ORM 1.0
#define OE_PBR_LAYOUT_RM 2.0

// Decode a packed sample to DRAM order and apply material factors. A negative
// separateAO means absent; an absent ORM/RM map must be sampled as vec4(1).
vec4 oe_pbr_decode(vec4 texel, vec4 layoutAndFactors, float separateAO)
{
    vec4 dram = texel;
    if (layoutAndFactors.x != OE_PBR_LAYOUT_DRAM)
        dram = vec4(0.0, texel.g, layoutAndFactors.x == OE_PBR_LAYOUT_ORM ? texel.r : 1.0, texel.b);
    if (separateAO >= 0.0) dram.b = separateAO;
    dram.g *= layoutAndFactors.y;
    dram.b = mix(1.0, dram.b, layoutAndFactors.z);
    dram.a *= layoutAndFactors.w;
    return dram;
}

#endif
