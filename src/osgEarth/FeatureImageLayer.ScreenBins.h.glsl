// osgEarth - Copyright 2026 Pelican Mapping - MIT License
#define OE_BINDING_FRUSTUM_PARAMS 12
#define OE_BINDING_FRUSTUMS 12
// Separate interface names let FIL and ordinary decals coexist in the same VirtualProgram.
#define Frustum VectorFrustum
#define FrustumParams VectorFrustumParams
#define Frustums VectorFrustums
#define frustums oe_vector_frustums
#define u_invProjMatrix oe_vector_invProjMatrix
#define u_viewport oe_vector_viewport
#define u_numTiles oe_vector_numTiles
#define u_pixelsPerTile oe_vector_pixelsPerTile
#define u_debugTiles oe_vector_debugTiles
#define oe_screenBin oe_vector_screenBin
#pragma include FrustumGrid.h.glsl

struct VectorReceiver
{
    vec4 origin;
    vec4 axisU;
    vec4 axisV;
    vec4 error;
    vec4 domain;
    ivec4 bins;
    uvec4 data; // first bin, atlas index, reserved, reserved
};
struct VectorBin
{
    vec4 bounds;
    uvec4 list; // linked paint head; allocation size; max footprint as float bits; source cell + 1
};
layout(std430, binding = 13) readonly buffer VectorReceivers { VectorReceiver oe_vector_receivers[]; };
layout(std430, binding = 14) buffer VectorBins { VectorBin oe_vector_bins[]; };
layout(std430, binding = 15) buffer VectorWords { uvec4 oe_vector_words[]; };
const uint OE_VECTOR_FALLBACK = 0xffffffffu;
