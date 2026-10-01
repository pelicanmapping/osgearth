#ifndef OE_PBR_MATERIAL_GLSL
#define OE_PBR_MATERIAL_GLSL

// Keep layout values in sync with PBRMaterial::Layout.
#define OE_PBR_LAYOUT_DRAM 0.0
#define OE_PBR_LAYOUT_ORM 1.0
#define OE_PBR_LAYOUT_RM 2.0
#define OE_PBR_LAYOUT_MTL_GLS_AO 3.0

// Encoded sample for a missing map: packed RGB layouts decode to unit factors,
// with zero displacement. Zero glossiness is required for unit roughness in VRV.
vec4 oe_pbr_default(float packing)
{
    return packing == OE_PBR_LAYOUT_MTL_GLS_AO ? vec4(1.0, 0.0, 1.0, 1.0) : vec4(1.0);
}

// Decode linear texture data to DRAM and then apply factors. Negative separateAO
// means no independent AO map. Use oe_pbr_default(layout) when the map is absent.
vec4 oe_pbr_decode(vec4 texel, vec4 layoutAndFactors, float separateAO)
{
    vec4 dram = texel;
    if (layoutAndFactors.x == OE_PBR_LAYOUT_MTL_GLS_AO)
        dram = vec4(0.0, 1.0 - texel.g, texel.b, texel.r);
    else if (layoutAndFactors.x != OE_PBR_LAYOUT_DRAM)
        dram = vec4(0.0, texel.g, layoutAndFactors.x == OE_PBR_LAYOUT_ORM ? texel.r : 1.0, texel.b);
    if (separateAO >= 0.0) dram.b = separateAO;
    dram.g *= layoutAndFactors.y;
    dram.b = mix(1.0, dram.b, layoutAndFactors.z);
    dram.a *= layoutAndFactors.w;
    return dram;
}

#endif
