#extension GL_ARB_gpu_shader_int64 : enable

#pragma vp_function oe_applyDecals, fragment, last

#pragma include Decals.h.glsl

in vec3 vp_VertexView;

// Decal bindless texture arena
layout(binding = OE_BINDING_DECAL_TEXTURES, std430) readonly buffer DecalTextures
{
    uint64_t oe_decalTextures[];
};

// returns the index of the tile containing gl_FragCoord.xy
int tileIndex()
{
    // compute the tile index for this fragment:
    int vpHeight = (u_viewport[3] - u_viewport[1]);
    ivec2 tileCoord = ivec2(gl_FragCoord.x - u_viewport[0], vpHeight - gl_FragCoord.y - u_viewport[1]) / int(u_pixelsPerTile);
    return tileCoord.y * u_numTiles.x + tileCoord.x;
}


// Find the tile containing the current fragment, and apply all decals in that tile
// that intersect.
void oe_applyDecals(inout vec4 color)
{
    DecalTile tile = oe_decalTiles[tileIndex()];

    for (int i = 0; i < tile.count; ++i)
    {
        Decal decal = oe_decals[tile.indices[i]];

        vec3 local = (decal.mvmInverse * vec4(vp_VertexView, 1.0)).xyz;

        vec2 uv;
        bool inside = false;

        if (decal.distance > 0.0) // perspective
        {
            // decal.a = zmin (far), decal.b = zmax (near)
            if (local.z >= decal.a && local.z <= decal.b)
            {
                float halfW = (decal.distance - local.z) * decal.tanHalfFovY * decal.aspect;
                float halfH = (decal.distance - local.z) * decal.tanHalfFovY;

                if (abs(local.x) <= halfW && abs(local.y) <= halfH)
                {
                    uv = vec2(local.x / halfW, local.y / halfH) * 0.5 + 0.5;
                    inside = true;
                }
            }
        }
        else // orthographic
        {
            vec3 bbox = vec3(decal.a, decal.b, decal.c); // decal.a,b,c = tangent bbox half-extents
            if (all(lessThanEqual(abs(local), bbox)))
            {
                uv = (local.xy / bbox.xy + vec2(1.0)) * vec2(0.5);
                inside = true;
            }
        }

        if (inside)
        {
            int ti = decal.textureIndex;
            vec4 tex = ti >= 0 ? texture(sampler2D(oe_decalTextures[ti]), uv) : vec4(1, 0, 0, 1);
            color.rgb = mix(color.rgb, tex.rgb, tex.a * decal.opacity * (1.0-u_debugTiles));
        }
    }

    // debugging overlay to show tile density
    float ramp = clamp(float(tile.count) / 5.0, 0.0, 1.0);
    vec3 debugColor = vec3(0, ramp, ramp);
    color.rgb = mix(color.rgb, debugColor, clamp(float(tile.count), 0, 1) * u_debugTiles * 0.5);
}
