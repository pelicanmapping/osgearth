#pragma import_defines(OE_TILES_PER_THREAD_GROUP)
#ifndef OE_TILES_PER_THREAD_GROUP
#define OE_TILES_PER_THREAD_GROUP 16
#endif

struct Frustum
{
    vec4 planes[4];
};

layout(std140, binding = OE_BINDING_FRUSTUM_PARAMS) uniform FrustumParams
{
    mat4 u_invProjMatrix;
    ivec4 u_viewport;
    ivec2 u_numTiles;
    uint u_pixelsPerTile;
    float u_debugTiles;
};

layout(binding = OE_BINDING_FRUSTUMS) buffer Frustums
{
    Frustum frustums[];
};

// Maps window coordinates to the top-origin frustum grid, including nonzero viewport origins.
ivec2 oe_screenBin(vec2 windowPosition)
{
    ivec2 pixel = ivec2(floor(windowPosition)) - u_viewport.xy;
    pixel.y = u_viewport.w - 1 - pixel.y;
    return pixel / int(u_pixelsPerTile);
}
