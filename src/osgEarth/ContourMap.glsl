#pragma vp_function oe_contour_vertex, vertex_view
#pragma import_defines(OE_USE_GL4)
#pragma import_defines(OE_USE_GEOID)

out vec4 oe_layer_tilec;

#ifdef OE_USE_GEOID
  vec4 oe_tile_key;
  uniform sampler2D oe_contour_geoid;
  out float oe_contour_offset;
#endif

#ifdef OE_USE_GL4
#pragma include RexEngine.GL4.glsl

  uint64_t oe_terrain_getElevationHandle();
vec2 oe_terrain_getElevationCoords(in vec2);
vec2 oe_terrain_getElevationMinMax();

out vec2 oe_elev_coord;
flat out uint64_t oe_elev_tex;
flat out vec2 oe_elev_min_max;

void oe_contour_vertex(inout vec4 not_used)
{
    oe_elev_coord = oe_terrain_getElevationCoords(oe_layer_tilec.st);
    oe_elev_tex = oe_terrain_getElevationHandle();
    oe_elev_min_max = oe_terrain_getElevationMinMax();

#ifdef OE_USE_GEOID
    // calculate long and lat from [0..1] across the profile:
    vec2 r = (oe_tile_key.xy + oe_layer_tilec.xy) / exp2(oe_tile_key.z);
    vec2 uv = vec2(0.5 * r.x, r.y);
    oe_contour_offset = texture(oe_contour_geoid, uv).r;
#endif
}
#else
void oe_contour_vertex(inout vec4 not_used)
{
#ifdef OE_USE_GEOID
    // calculate long and lat from [0..1] across the profile:
    vec2 r = (oe_tile_key.xy + oe_layer_tilec.xy) / exp2(oe_tile_key.z);
    vec2 uv = vec2(0.5 * r.x, r.y);
    oe_contour_offset = texture(oe_contour_geoid, uv).r;
#endif
}
#endif

[break]
#pragma vp_function oe_contour_fragment, fragment_coloring
#pragma import_defines(OE_USE_GL4)
#pragma import_defines(OE_USE_GEOID)

uniform sampler1D oe_contour_xfer;
uniform float oe_contour_min;
uniform float oe_contour_range;

#ifdef OE_USE_GEOID
in float oe_contour_offset;
#endif

#ifdef OE_USE_GL4
#pragma include RexEngine.GL4.glsl
flat in uint64_t oe_elev_tex;
in vec2 oe_elev_coord;
flat in vec2 oe_elev_min_max;

void oe_contour_fragment( inout vec4 color )
{
    if (oe_elev_tex > 0)
    {
        float offset = 0;
#ifdef OE_USE_GEOID
        offset = oe_contour_offset;
#endif

        float encoded = texture(sampler2D(oe_elev_tex), oe_elev_coord).r;
        float minh = oe_elev_min_max[0];
        float maxh = oe_elev_min_max[1];
        float height = (minh == maxh) ? encoded : mix(minh, maxh, encoded); 
            // *mix(minh, maxh, dot(encoded, vec2(65280.0, 255.0)) / 65535.0); // RG8

        height -= offset;

        float height_normalized = (height - oe_contour_min) / oe_contour_range;
        float lookup = clamp(height_normalized, 0.0, 1.0);
        vec4 texel = texture(oe_contour_xfer, lookup);
        color.rgb = mix(color.rgb, texel.rgb, texel.a);
    }
}

#else // not OE_USE_GL4

// GL3 implementation:
float oe_terrain_getElevation(in vec2 uv);
in vec4 oe_layer_tilec;

void oe_contour_fragment(inout vec4 color)
{
    float offset = 0;
#ifdef OE_USE_GEOID
    offset = oe_contour_offset;
#endif
    float height = oe_terrain_getElevation(oe_layer_tilec.st) - offset;
    float height_normalized = (height - oe_contour_min) / oe_contour_range;
    float lookup = clamp(height_normalized, 0.0, 1.0);
    vec4 texel = texture(oe_contour_xfer, lookup);
    color.rgb = mix(color.rgb, texel.rgb, texel.a);
}

#endif // OE_USE_GL4
