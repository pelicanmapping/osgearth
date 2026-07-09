#pragma vp_entryPoint oe_rex_normalMapVS
#pragma vp_location   vertex_view
#pragma vp_order      0.5

#pragma import_defines(OE_TERRAIN_RENDER_NORMAL_MAP)

uniform mat4 oe_tile_normalTexMatrix;
uniform vec2 oe_tile_elevTexelCoeff;

uniform mat4 oe_tile_elevationTexMatrix;

// stage globals
out vec4 oe_layer_tilec;
out vec2 oe_normal_uv;

void oe_rex_normalMapVS(inout vec4 unused)
{
#ifndef OE_TERRAIN_RENDER_NORMAL_MAP
    return;
#endif

    oe_normal_uv = oe_layer_tilec.st
        * oe_tile_elevTexelCoeff.x * oe_tile_elevationTexMatrix[0][0]
        + oe_tile_elevTexelCoeff.x * oe_tile_elevationTexMatrix[3].st
        + oe_tile_elevTexelCoeff.y;
}


[break]

#pragma vp_entryPoint oe_rex_normalMapFS
#pragma vp_location   fragment_coloring
#pragma vp_order      0.1

#pragma import_defines(OE_TERRAIN_RENDER_NORMAL_MAP)


// import terrain SDK
vec4 oe_terrain_getNormalAndCurvature(in vec2);

uniform sampler2D oe_tile_normalTex;

in vec3 vp_Normal;
in vec3 vp_VertexView;

in vec3 oe_UpVectorView;
in vec2 oe_normal_uv;
//in vec3 oe_normal_binormal;

// global
mat3 oe_normalMapTBN;


void oe_rex_normalMapFS(inout vec4 color)
{
#ifndef OE_TERRAIN_RENDER_NORMAL_MAP
    return;
#endif

    vec4 N_TS = oe_terrain_getNormalAndCurvature(oe_normal_uv);

    // +Y is "north" because we're already in the local frame of the current tile!
    // Orthonormalize the frame (matches the GL4 path):
    vec3 T = normalize(cross(gl_NormalMatrix * vec3(0, 1, 0), oe_UpVectorView));
    vec3 B = normalize(cross(oe_UpVectorView, T));
    oe_normalMapTBN = mat3(T, B, oe_UpVectorView);
    vp_Normal = normalize(oe_normalMapTBN * N_TS.xyz);
}