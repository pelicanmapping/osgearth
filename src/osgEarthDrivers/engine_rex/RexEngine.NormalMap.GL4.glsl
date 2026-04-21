#pragma include RexEngine.GL4.glsl
#pragma vp_function oe_rex_normalMapVS, vertex_view, 0.5

#pragma import_defines(OE_TERRAIN_RENDER_NORMAL_MAP)

#ifdef OE_TERRAIN_RENDER_NORMAL_MAP

// SDK functions:
vec2 oe_terrain_getNormalCoords();

// outputs:
flat out uint64_t oe_normal_handle;
out vec2 oe_normal_uv;

#else

// SDK functions:
vec3 oe_terrain_getPoint(in vec2 uv);

// stage globals / outputs:
out vec4 oe_layer_tilec;
out vec3 vp_Normal;

// use the elevatoin data to compute the normal vector in tangent space
vec3 oe_normalmap_compute_normal_ts(in vec2 uv)
{
    int elev_index = oe_tile[oe_tileID].elevIndex;
    vec2 texelSize = 1.0 / (textureSize(sampler2D(oe_terrain_tex[elev_index]), 0) - vec2(1.0));

    vec3 p_east = oe_terrain_getPoint(uv + vec2(texelSize.x, 0.0));
    vec3 p_west = oe_terrain_getPoint(uv + vec2(-texelSize.x, 0.0));
    vec3 p_north = oe_terrain_getPoint(uv + vec2(0.0, texelSize.y));
    vec3 p_south = oe_terrain_getPoint(uv + vec2(0.0, -texelSize.y));

    vec3 x = p_east - p_west;
    vec3 y = p_north - p_south;
    return normalize(cross(x, y));
}

// makes a basis matrix in tangent space form a normal (up) vector
mat3 oe_normalmap_compute_tbn_ts(in vec3 n)
{
    vec3 t = normalize(cross(vec3(0, 1, 0), n));
    vec3 b = normalize(cross(n, t));
    return mat3(t, b, n);
}

#endif // !OE_TERRAIN_RENDER_NORMAL_MAP


void oe_rex_normalMapVS(inout vec4 unused)
{
#ifdef OE_TERRAIN_RENDER_NORMAL_MAP

    oe_normal_handle = 0;
    int index = oe_tile[oe_tileID].normalIndex;
    if (index >= 0)
    {
        oe_normal_uv = oe_terrain_getNormalCoords();
        oe_normal_handle = oe_terrain_tex[index];
    }

#else // compute normal map from elevation data (per vertex)

    mat3 normalMatrix = mat3(oe_tile[oe_tileID].modelViewMatrix);

    vec3 normal_ts = oe_normalmap_compute_normal_ts(oe_layer_tilec.st);

    vec3 t_vs = normalize(cross(normalMatrix * vec3(0, 1, 0), vp_Normal));
    vec3 b_vs = normalize(cross(vp_Normal, t_vs));
    mat3 tbn_vs= mat3(t_vs, b_vs, vp_Normal);

    vp_Normal = normalize(tbn_vs * normal_ts);

#endif
}


[break]
#pragma include RexEngine.GL4.glsl
#pragma vp_function oe_rex_normalMapFS, fragment_coloring, 0.1

#pragma import_defines(OE_TERRAIN_RENDER_NORMAL_MAP)

// inputs:
in vec3 vp_Normal;
in vec3 oe_UpVectorView;

#ifdef OE_TERRAIN_RENDER_NORMAL_MAP

// SDK functions:
vec4 oe_terrain_getNormalAndCurvature(in uint64_t, in vec2); // SDK

// inputs:
flat in uint64_t oe_normal_handle;
in vec2 oe_normal_uv;

#endif

// stage global
mat3 oe_normalMapTBN;

void oe_rex_normalMapFS(inout vec4 color)
{
    mat3 normalMatrix = mat3(oe_tile[oe_tileID].modelViewMatrix);

#ifdef OE_TERRAIN_RENDER_NORMAL_MAP

    vp_Normal = oe_UpVectorView;

    if (oe_normal_handle > 0)
    {
        // build a view-space TBN based on the up vector
        vec3 t_vs = normalize(cross(normalMatrix * vec3(0, 1, 0), vp_Normal));
        vec3 b_vs = normalize(cross(vp_Normal, t_vs));
        mat3 tbn_vs = mat3(t_vs, b_vs, vp_Normal);

        vec4 normal_ts = oe_terrain_getNormalAndCurvature(oe_normal_handle, oe_normal_uv);
        vp_Normal = normalize(tbn_vs * normal_ts.xyz);
    }

    // finally, recompute it with the new normal vector:
    vec3 t_vs = normalize(cross(normalMatrix * vec3(0, 1, 0), vp_Normal));
    vec3 b_vs = normalize(cross(vp_Normal, t_vs));
    oe_normalMapTBN = mat3(t_vs, b_vs, vp_Normal);

#else // computed normal on GPU (per-vertex)

    // build a view-space TBN based on the normal vector.
    vec3 t_vs = normalize(cross(normalMatrix * vec3(0, 1, 0), vp_Normal));
    vec3 b_vs = normalize(cross(vp_Normal, t_vs));
    oe_normalMapTBN = mat3(t_vs, b_vs, vp_Normal);

#endif
}
