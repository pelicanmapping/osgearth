#pragma vp_define OE_USE_NORMAL_MAP

#ifdef OE_USE_NORMAL_MAP

// normal map version:
uniform sampler2D oe_tile_normalTex;
in vec2 oe_normalMapCoords;

float oe_bumpmap_getSlope()
{
    vec4 encodedNormal = texture(oe_tile_normalTex, oe_normalMapCoords);
    vec3 normalTangent = normalize(encodedNormal.xyz*2.0-1.0);
    return clamp((1.0-normalTangent.z)/0.8, 0.0, 1.0);
}

#else // OE_USE_NORMAL_MAP

// non- normal map version:
in float oe_bumpmap_slope;

float oe_bumpmap_getSlope()
{
    return oe_bumpmap_slope;
}

#endif // OE_USE_NORMAL_MAP
