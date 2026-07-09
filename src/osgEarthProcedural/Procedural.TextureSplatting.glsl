#pragma vp_name Texture Splatter VV
#pragma vp_function oe_splat_View, vertex_view

#pragma import_defines(OE_TWEAKABLE)
#pragma import_defines(OE_SPLAT_NUM_LEVELS)
#pragma import_defines(OE_SNOW)

const int levels[2] = int[](14, 19);
flat out vec2 splat_tilexy[2];
out vec2 splat_uv[2];

// from REX SDK:
vec4 oe_terrain_scaleCoordsAndTileKeyToRefLOD(in vec2 tc, in float refLOD);

out vec4 oe_layer_tilec;
out float oe_splat_levelblend;

#ifdef OE_SNOW
out float oe_elev;
float oe_terrain_getElevation();
#endif

#ifdef OE_TWEAKABLE
#define tweakable uniform
#else
#define tweakable const
#endif

tweakable float oe_splat_blend_start = 2500.0;
tweakable float oe_splat_blend_end = 500.0;

#define MAP_TO_01(VAL,LO,HI) clamp((VAL-LO) / (HI-LO), 0.0, 1.0)

void oe_splat_View(inout vec4 vertex_view)
{
    // texture coordinates
    for (int i = 0; i < OE_SPLAT_NUM_LEVELS; ++i)
    {
        vec4 uvxy = oe_terrain_scaleCoordsAndTileKeyToRefLOD(oe_layer_tilec.st, levels[i]);
        splat_uv[i] = uvxy.xy;
        splat_tilexy[i] = uvxy.zw;
    }
    oe_splat_levelblend = MAP_TO_01(-vertex_view.z, oe_splat_blend_start, oe_splat_blend_end);

#ifdef OE_SNOW
    oe_elev = oe_terrain_getElevation();
#endif
}


[break]
#pragma vp_name Texture Splatter FS
#pragma vp_function oe_splat_Frag, fragment, 0.8

#pragma oe_use_shared_layer(OE_LIFEMAP_TEX, OE_LIFEMAP_MAT)

#pragma import_defines(OE_TEX_DIM_X)
//#pragma import_defines(OE_TEX_DIM_Y)
#define OE_TEX_DIM_Y 2

#pragma import_defines(OE_TWEAKABLE)
#pragma import_defines(OE_LIFEMAP_DIRECT)
//#pragma import_defines(OE_SNOW)

layout(binding = 5, std430) buffer SplatTextureArena {
    uint64_t texHandle[];
};

uniform float oe_texScale[128];

#define RUGGED 0
#define DENSE 1
#define LUSH 2
#define SPECIAL 3

in vec3 vp_Normal;
in vec3 vp_VertexView;
in vec3 oe_UpVectorView;
in float oe_splat_levelblend;
in vec4 oe_layer_tilec;

#pragma import_defines(OE_SPLAT_NUM_LEVELS)
flat in vec2 splat_tilexy[2];
in vec2 splat_uv[2];

#ifdef OE_SNOW
in float oe_elev;
#endif

#ifdef OE_LIFEMAP_DIRECT
#define tweakable uniform
#else
#define tweakable const
#endif

tweakable float dense_power = 1.0;
tweakable float lush_power = 1.0;
tweakable float rugged_power = 1.0;
tweakable float ao_power = 1.0;
tweakable float oe_snow = 0.0;
tweakable float oe_snow_min_elev = 1000.0;
tweakable float oe_snow_max_elev = 3500.0;
tweakable float oe_splat_blend_rgbh_mix = 0.8;
tweakable float oe_splat_blend_normal_mix = 0.85;
tweakable float oe_splat_brightness = 1.0;
tweakable float oe_splat_contrast = 1.0;
tweakable float oe_dense_contrast = 1.0;

uniform float oe_normal_power = 1.0;
uniform float oe_mask_alpha = 0.0;
uniform float oe_displacement_depth = 0.1;
uniform float oe_normal_boost = 1.0;

mat3 oe_normalMapTBN;

#define MAP_TO_01(VAL,LO,HI) clamp((VAL-LO) / (HI-LO), 0.0, 1.0)
#define DECEL(V,P) (1.0-pow(1.0-(V),(P)))

#if defined(OE_LIFEMAP_DIRECT) && OE_LIFEMAP_DIRECT
#define MODIFY(V,M) clamp((M), 0.0, 1.0)
#else
#define MODIFY(V,M) clamp((V)*(M), 0.0, 1.0)
#endif

// optimized uncompressor (assumes Z is never negative)
#define UNPACK_NORMAL(P,N) N.xy = P*2.0-1.0; N.z = 1.0-abs(N.x)*oe_normal_boost-abs(N.y)*oe_normal_boost; N /= length(N)

struct SplatRowData
{
    vec3 p1_weights;
    vec3 p2_weights;
    float column_mix;
};

struct SplatLevelData
{
    vec3 override_material_weights;
    float sub_surf_mix;
    float splat_level_mix;

    // 0 = substrate (dirt and rocks)
    // 1 = surface (greenery and debris)
    SplatRowData rowData[2];
};

struct SplatSharedData
{
    float rugged;
    float lush;
    float dense;
    int material_index;

    SplatLevelData[OE_SPLAT_NUM_LEVELS] levelData;
};

// We cache the data computed in the shader here so it can be
// accessed as a stage-global from other shaders that need to benefit
// from the splatting calculations (sensors, e.g.)
SplatSharedData splatData;

struct Pixel {
    vec4 rgbh;
    vec3 normal;
    vec3 material;
};

#define ROUGHNESS 0
#define AO 1
#define METAL 2

// fragment stage global PBR parameters.
struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;

// compute the splatting texture coordinate by combining the macro (tile_xy)
// and micro (local xy) components. Cannot do this in the VS because it will
// overflow the interpolator and pause pixel jitter
void get_coord(out vec2 coord, in int index, in int level)
{
    vec2 scale = vec2(oe_texScale[index]);
    vec2 a = fract(splat_tilexy[level] * scale);
    vec2 b = splat_uv[level] * scale;
    coord = a + b;
}

#pragma import_defines(OE_SPLAT_HEX_TILER)
#ifndef OE_SPLAT_HEX_TILER
#define OE_SPLAT_HEX_TILER 0
#endif

#if OE_SPLAT_HEX_TILER == 1
// declare function from HexTiling.glsl library
void ht_hex2colTex_optimized(
    in sampler2D color_tex,
    in sampler2D material_tex,
    in vec2 st,
    out vec4 color,
    out vec4 material,
    inout vec3 weighting);
#endif

#if 1 // old PBR
void get_pixel(out Pixel res, inout vec3 weights, in int index, in vec2 coord)
{
    vec4 nnra;

#if OE_SPLAT_HEX_TILER == 1
    ht_hex2colTex_optimized(
        sampler2D(texHandle[index * 2]),
        sampler2D(texHandle[index * 2 + 1]),
        coord,
        res.rgbh,
        nnra,
        weights);

#else
    res.rgbh = texture(sampler2D(texHandle[index * 2]), coord);
    nnra = texture(sampler2D(texHandle[index * 2 + 1]), coord);
#endif

    UNPACK_NORMAL(nnra.xy, res.normal);
    res.material = vec3(nnra[2], nnra[3], 0.0); // roughness, ao, metal
}
#else
void get_pixel(out Pixel res, inout vec3 weights, in int index, in vec2 coord)
{
    vec4 rgbh, norm, pbr;

#if OE_SPLAT_HEX_TILER == 1
    ht_hex2colTex_optimized(
        sampler2D(texHandle[index * 2]),     // color
        sampler2D(texHandle[index * 2 + 1]), // normal
        sampler2D(texHandle[index * 2 + 2]), // PBR dram
        coord,
        rgbh,
        norm,
        pbr,
        weights);

#else
    rgbh = texture(sampler2D(texHandle[index * 2]), coord);
    norm = texture(sampler2D(texHandle[index * 2 + 1]), coord);
    pbr = texture(sampler2D(texHandle[index * 2 + 2]), coord);
#endif

    res.rgbh[3] = dram[0]; // displacement height
    UNPACK_NORMAL(norm.xy, res.normal);
    res.material = vec3(dram[1], dram[2], dram[3]); // roughness, ao, metal
    //res.material = vec3(nnra[2], nnra[3], 0.0); // roughness, ao, metal
}
#endif

float heightAndEffectMix(in float h1, in float a1, in float h2, in float a2)
{
    // Andrey Mishkinis height+alpha blender.
    // https://www.gamedeveloper.com/programming/advanced-terrain-texture-splatting
    float ma = max(h1 + a1, h2 + a2) - oe_displacement_depth;
    float b1 = max(h1 + a1 - ma, 0.0);
    float b2 = max(h2 + a2 - ma, 0.0);
    return b2 / (b1 + b2);
}

void pixmix(out Pixel res, in Pixel p1, in Pixel p2, float m)
{
    res.rgbh = mix(p1.rgbh, p2.rgbh, m);
    res.normal = mix(p1.normal, p2.normal, m);
    res.material = mix(p1.material, p2.material, m);
}

void resolveRow(out Pixel result, int level, int row, float xvar)
{
    Pixel p1, p2;
    vec3 w1 = vec3(0.0);
    vec3 w2 = vec3(0.0);
    vec2 coord;

    // calulate first column index and mix factor
    float xf = xvar * (float(OE_TEX_DIM_X) - 1.0);
    float xf_floor = floor(xf);
    int x = int(xf_floor);
    float x_mix = xf - xf_floor;

    // texture index:
    int i = row * OE_TEX_DIM_X + x;

    // read both columns (clamp to the last column so we don't
    // wrap around into the next row's textures):
    get_coord(coord, i, level);
    get_pixel(p1, w1, i, coord);
    i = (i % OE_TEX_DIM_X < OE_TEX_DIM_X - 1) ? i + 1 : i;
    get_coord(coord, i, level);
    get_pixel(p2, w2, i, coord);

    // blend them using both heightmap:
    float m = heightAndEffectMix(p1.rgbh[3], 1.0 - x_mix, p2.rgbh[3], x_mix);
    pixmix(result, p1, p2, m);

    // publish splatting variables for use by other shaders
    SplatRowData rowData;
    rowData.p1_weights = w1;
    rowData.p2_weights = w2;
    rowData.column_mix = m;
    splatData.levelData[level].rowData[row] = rowData;
}

void resolveLevel(out Pixel result, int level, float rugged, float lush, float dense, int override_material_index)
{
    SplatLevelData levelData;
    float surface_mix = dense;

    // resolve the substrate (dirt and rocks)
    Pixel substrate;
    resolveRow(substrate, level, 0, rugged);

    // resolve the surface texture (greenery and debris)
    Pixel surface;
    if (override_material_index > 0)
    {
        vec3 weights = vec3(0.0);
        vec2 coord;
        get_coord(coord, override_material_index - 1, 0);
        get_pixel(surface, weights, override_material_index - 1, coord);
        surface_mix = clamp(1.0 - DECEL(dense, 2.0), 0, 1); //  (rugged + dense + lush), 0, 1);
        levelData.override_material_weights = weights;
    }
    else
    {
        resolveRow(surface, level, 1, lush);
    }

    // use density to modulate the depth blend between the two.
    float m = heightAndEffectMix(
        substrate.rgbh[3], 1.0 - surface_mix,
        surface.rgbh[3], surface_mix);

    levelData.sub_surf_mix = m;

    if (level == 0)
    {
        pixmix(result, substrate, surface, m);
    }
    else
    {
        Pixel temp;
        pixmix(temp, substrate, surface, m);

        float mat_mix = min(oe_splat_levelblend, oe_splat_blend_rgbh_mix);
        levelData.splat_level_mix = mat_mix;

        result.rgbh = mix(result.rgbh, temp.rgbh, mat_mix);
        result.normal = mix(result.normal, temp.normal, min(oe_splat_levelblend, oe_splat_blend_normal_mix));
        result.material = mix(result.material, temp.material, mat_mix);
    }

    splatData.levelData[level] = levelData;
}

void oe_splat_Frag(inout vec4 quad)
{
    // sample the life map and extract the compenents:
    vec2 c = (OE_LIFEMAP_MAT * oe_layer_tilec).st;
    vec4 life = texture(OE_LIFEMAP_TEX, c);
    splatData.rugged = MODIFY(life[RUGGED], rugged_power);
    splatData.lush = MODIFY(life[LUSH], lush_power);
    splatData.dense = MODIFY(life[DENSE], dense_power);

    ivec2 lmsize = textureSize(OE_LIFEMAP_TEX, 0);
    ivec2 tfc = ivec2(min(int(c.x * float(lmsize.x)), lmsize.x - 1), min(int(c.y * float(lmsize.y)), lmsize.y - 1));
    vec4 life_i = texelFetch(OE_LIFEMAP_TEX, tfc, 0);
    splatData.material_index = int(life_i[3] * 255.0f);

    // compute the pixel color:
    Pixel pixel;
    for (int level = 0; level < OE_SPLAT_NUM_LEVELS; ++level)
    {
        resolveLevel(pixel, level, splatData.rugged, splatData.lush, splatData.dense, splatData.material_index);
    }

    // apply PBR
    oe_pbr.displacement = pixel.rgbh.a; // pixel.material[DISPLACEMENT];
    oe_pbr.roughness = clamp(oe_pbr.roughness * pixel.material[ROUGHNESS], 0.0, 1.0);
    oe_pbr.ao = clamp(oe_pbr.ao * pow(pixel.material[AO], ao_power), 0.0, 1.0);
    oe_pbr.metal = clamp(pixel.material[METAL], 0.0, 1.0);

    vec3 color = pixel.rgbh.rgb;

    color = ((color - 0.5) * oe_splat_contrast + 0.5) * oe_splat_brightness;

    pixel.normal = vec3(
        DECEL(pixel.normal.x, oe_normal_power),
        DECEL(pixel.normal.y, oe_normal_power),
        pixel.normal.z);

    vp_Normal = normalize(vp_Normal + oe_normalMapTBN * pixel.normal);

    float composite = DECEL(clamp(splatData.dense + splatData.lush + splatData.rugged, 0.0, 1.0), oe_mask_alpha);
    float alpha = oe_mask_alpha > 0.0 ? composite : 1.0;

    // final color output:
    quad = vec4(color, alpha);
}
