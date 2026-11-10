# FeatureImage

FeatureImage renders styled feature data as terrain imagery. Its default `raster`
mode uses the existing feature rasterizer. Optional `vector` mode evaluates
Slughorn curves and centerline strokes on the GPU, preserving smooth edges when
zooming past the source LOD.

vcpkg installs Slughorn as a regular dependency from the repository's overlay port.
Enable `OSGEARTH_ENABLE_SLUGHORN` in CMake (default OFF) to use
`FeatureImage` layers with `mode="vector"`. The core library and osgEarth's
Slughorn implementation file require C++20. osgEarth's public headers require C++17.

For other builds, CMake looks for an installed Slughorn package first. Set
`Slughorn_DIR` to the directory containing `SlughornConfig.cmake` if needed.
If the package is absent, CMake fetches and verifies the same pinned archive
and builds only the core. For an offline fallback, point
`FETCHCONTENT_SOURCE_DIR_OSGEARTH_SLUGHORN` at a checkout of the revision in
`cmake/ports/slughorn/source.cmake`. The shader depends on that revision's atlas format.

## Earth file

Run `osgearth_viewer slughorn.earth` from `tests` after running
`osgearth_shell.bat`. The example uses the existing ReadyMap-hosted OpenStreetMap
MVT service, requires network access and Protobuf/MVT support, and contains
separate land-use, water, building, and road layers. Its detail viewpoint shows
inherited atlases beyond the source's LOD 14.

For the same categories in one layer, run
`osgearth_imgui slughorn_single_layer.earth`. Its six CSS `select:` predicates
share one FeatureImageLayer and one atlas per source tile. Styles paint in
stylesheet order, with roads over buildings, water, and land use. Both demos
start at Honolulu and include a close-up viewpoint for inherited atlas detail.

```xml
<FeatureImage name="Vector roads" mode="vector" min_level="16" max_data_level="16">
    <features>roads</features>
    <styles>
        <style type="text/css">
            default { stroke: #ffffff; stroke-width: 8m; stroke-linecap: round; }
        </style>
    </styles>
</FeatureImage>
```

Use `max_data_level`, rather than `max_level`, to cap loading while allowing
higher-resolution terrain to display inherited data. Source feature-profile
limits are also honored. REX's existing parent texture and scale/bias matrix
select the correct subregion at higher LODs. Pixel widths refer to pixels at
the source tile's nominal `tile_size`, as with raster imagery; they are not
constant screen-pixel widths while zooming.

For spherical Mercator feature sources on geographic terrain tiles, vector
geometry is evaluated in a shared Mercator plane. Both geometry axes use the
same scale, preserving stroke widths, round caps, and joins at high latitudes.
The CPU rebases geometry in double precision; the shader maps inherited terrain
UVs and their derivatives into that plane without subtracting large world
coordinates. Neighboring tiles share the projection and width policy, while
the spatial grid accounts for each tile's projected aspect ratio. Slughorn's
vendor atlas format is unchanged.

Tiled Mercator sources use their equivalent source LOD for pixel widths,
including the source's maximum LOD. Round strokes with linear-unit widths use
latitude-dependent Mercator scale per fragment, so ground widths do not change
at tile boundaries. Mercator tiles crossing the projection limit (about
85.0511 degrees), angular-unit strokes, and non-round strokes requiring ground
widths use the existing raster fallback. Other source/map combinations retain
the rasterizer's existing distance policy. Source data must still include
geometry crossing tile edges; the projection cannot repair missing source
buffers or inconsistent source geometry.

## TileJSON sources

`XYZFeatures` accepts a TileJSON metadata URL in place of a tile URL template:

```xml
<XYZFeatures name="openfreemap">
    <url>https://tiles.openfreemap.org/planet/latest</url>
</XYZFeatures>
<FeatureImage name="Water" mode="vector">
    <features>openfreemap</features>
    <styles>
        <style type="text/css"><![CDATA[
            water {
                select: feature.properties.mvt_layer === 'water';
                fill: #a9cee2;
            }
        ]]></style>
    </styles>
</FeatureImage>
```

A URL containing tile placeholders keeps the existing XYZ behavior. Otherwise,
the source reads TileJSON when it opens, including extensionless endpoints such
as `latest`. It selects the first endpoint in `tiles` (equivalent mirrors),
resolves relative endpoints against the metadata URL, and honors `scheme: "tms"`
by inverting Y. The default tiling profile is spherical Mercator; geographic
`bounds` describe feature coverage without changing the tile grid.

The metadata supplies `minzoom`, `maxzoom`, and `attribution`. Missing or invalid
optional zoom values default to 0 and 30. Explicit `min_level`, `max_level`,
`profile`, `format`, and `attribution` settings take precedence. Feature data
defaults to PBF; the optional `format` field can select `pbf` (or `mvt`), `json`,
`gml`, or `fgb`. Raster tile formats are not supported by `XYZFeatures`.

Derived settings and the resolved tile endpoint are not written back into the
earth file. The original metadata URL is retained and resolved again on reopen,
subject to the configured URI cache policy. Local TileJSON sources are read-only.
Malformed metadata or unusable tile endpoints report an error when opening.

TileJSON does not supply rendering styles. Use `mvt_layer` and the source's
attributes in your stylesheet as above. The source zoom limit automatically caps
feature requests, so `max_data_level` is unnecessary unless you want an override.
The metadata's `center`, `fillzoom`, and descriptive `vector_layers` fields do not
change the viewpoint, overzoom policy, or style selection.

See `tests/openfreemap_vector.earth` for a complete vector basemap and
`tests/maplibre_vector.earth` for a simpler country map.

## Rendering

Each paging request owns a private Slughorn atlas. Curves retain 32-bit float
precision; coordinates are rebased against the tile origin in double precision
first. Shape metadata, ordered 64-by-64 and 16-by-16 spatial indexes, curves, and
compact band data and shared centerline segments are packed into one immutable
RGBA32F texture. Band lookup
indirection is resolved during packing, and each list texel holds four linear
curve addresses. Curve coordinates and their evaluation order are unchanged.
Integer addresses are exactly representable in this format, and address/dimension limits are
checked before upload. A layer-local VirtualProgram replaces the image-coloring
function and evaluates Slug coverage directly on terrain fragments. Terrain
shaders are unchanged. Both standard texture bindings and GL4 bindless handles
use the same evaluator, including opacity and layer blending. Vector mode disables
imagery morphing: each fragment evaluates only the selected source atlas, while
parent texture/matrix inheritance remains unchanged. No offscreen rasterization
is involved for supported vector tiles. Shape evaluation runs front to back and stops at exactly opaque coverage; transparent shapes retain
their paint order. The finer spatial index shortens candidate lists when zoomed in,
while the coarse index preserves antialiasing support when zoomed out.
Queries just outside a source tile also use the padded edge-cell lists when their
antialiasing footprint fits. The wider coarse-grid padding handles queries beyond
the fine grid's safe margin. Coverage uses the original UVs; queries beyond both
indexed margins retain the full-shape fallback. This applies to both rendering
backends.

Round-joined strokes with round caps use compact centerlines instead of CPU-expanded
outline curves. Closed round-joined rings also use this path. The FIL fragment shader
evaluates the union of segment capsules and applies color/opacity once per path,
so overlapping segments within a path do not darken translucent joins. Outlines
and cores share their centerline data. Coverage uses a derivative-based distance
filter, including both edges for subpixel strokes; edge pixels can differ from
Slug's curve-coverage filter, especially under strong minification. Flat/square
caps and miter joins retain CPU-expanded Slughorn curves. Polygon fills, holes,
and point symbols retain the Slug curve evaluator. Both kinds of paint operation
share the existing spatial indexes, style order, and inherited texture matrix.
Long round strokes use cell-local segment lists in 64x64 and 16x16 tile grids,
so fragments search nearby segments rather than the entire path. Segment bounds
include stroke width and an antialiasing margin; segment order and one blend per
path are preserved. Minified vector layers fade out instead of scanning complete paths.

`vector_fade_start` and `vector_fade_cutoff` set the projected source-tile sizes in
pixels at which fading starts and ends. Start must be at least cutoff; cutoff must
be at least `16`. Equal values give a hard cutoff, for example
`vector_fade_start="200" vector_fade_cutoff="200"`. These are pixel thresholds,
not camera ranges. Omitted start defaults to twice the configured cutoff.

For compatibility, `vector_min_tile_pixels` supplies the cutoff when
`vector_fade_cutoff` is omitted (default `16`, minimum `16`).
By default, fading starts at twice this projected source-tile size and the layer
becomes transparent at the cutoff. For example,
`<vector_min_tile_pixels>32</vector_min_tile_pixels>` fades between approximately
64 and 32 pixels across the source tile. Evaluation uses the largest source-UV
pixel footprint per fragment, so oblique views can fade earlier in the compressed
direction. Inherited atlases use their original source UVs, not child-tile UVs.
The fade includes the layer background and raster fallback tiles; normal raster-mode
layers are unaffected. No atlas data is read beyond the cutoff. This is a render-only
configuration value, retained when serializing the layer, and does not change data LODs.

In `osgearth_imgui`, expand the vector layer in the Map panel to adjust **Vector
fade start (px)** and **Vector fade cutoff (px)** live. These controls update the
uniform immediately, without rebuilding tiles. Cutoff stays at least 16 pixels
and at or below fade start. **Reset fade** restores the configured values. Live changes
last until the layer is reopened.

This is a layer-local fragment implementation, with no separate stroke geometry
or compute pass, and it does not use osgSlug's PathDrawable.

Supported styling includes solid polygon fills with holes, solid line strokes
and outlines with cap/join settings and width expressions, and point fills.
`stroke-outline-width` is the border thickness on each side of the stroke.
For a 5 m road with a 0.5 m border, use `stroke-width: 5m; stroke-outline-width: 0.5m;`
and set `stroke-outline` to its color. Both vector and raster modes expand the
underlying stroke to 6 m in this example. Within each style, all outlines are
painted before any stroke interiors, so road junctions remain connected.
Selector styles retain their existing drawing order.
Style selection and filters use FeatureImageLayer's existing FeatureStyleSorter.
Vector fills use exact analytic boundaries, without the rasterizer's implicit
one-pixel seam-covering stroke. Non-vector symbols (including text/icons),
material fills, stippling, and tiles exceeding SDK/GPU capacity use the existing
rasterizer for that tile. Raster fallbacks are reported at DEBUG notify level
and can coexist with vector parent or child tiles. If Slughorn support is not
compiled in, a vector layer logs a warning and uses the normal raster pipeline.
With Slughorn enabled, missing required GLSL support still prevents the layer
from opening. The default raster mode is unchanged.

The pinned Slughorn Blend2D adapter consumes BLPath objects, rather than
intercepting BLContext operations, and targets a newer Blend2D API than this
project's current vcpkg build. This implementation uses Slughorn's native Canvas
path API. A future shared Blend2D path producer could reduce duplicated style
conversion without exposing C++20 SDK types in osgEarth headers.

## PBR stroke materials

Vector FILs support multiple PBR stroke materials in a single terrain pass. This
path requires the NVGL renderer (`--nvgl`) and `GL_NV_gpu_shader5` for material
selection that varies between fragments. A configured material palette fails to
open on the non-NVGL renderer. Ordinary colored vector layers keep their GL3 path.

Define named materials directly on the FIL and reference them from selector styles:

```xml
<FeatureImage name="Road details" mode="vector">
    <features>roads</features>
    <materials>
        <material name="paint" color="paint-color.png" normal="paint-normal.png"
                  packed="paint-orm.png" layout="orm" />
        <material name="concrete" color="concrete-color.png"
                  roughness_factor="0.9" metallic_factor="0" />
    </materials>
    <styles>
        <style type="text/css"><![CDATA[
            markings {
                select: feature.properties.material === 'white';
                stroke: #ffffff;
                stroke-width: [width_m];
                stroke-width-units: meters;
                stroke-material: paint;
                stroke-material-period: 0.5m;
            }
            curbs {
                select: feature.properties.material === 'curb';
                stroke: #ffffff;
                stroke-width: [width_m];
                stroke-width-units: meters;
                stroke-material: concrete;
                stroke-material-period: 1m;
            }
        ]]></style>
    </styles>
</FeatureImage>
```

`stroke-outline-material` independently assigns an outline's material. An omitted
name keeps solid color. `stroke-material-period` applies to both core and outline,
defaults to one meter, and must be finite and positive. Stroke/outline colors tint
the albedo; their opacity multiplies material alpha and analytic coverage. Existing
style ordering and the outline-first pass within each style are preserved.

Material definitions use `PBRMaterial`'s albedo, normal, packed PBR, optional AO,
channel layouts and factors. They share the terrain `TextureArena`/`MaterialArena`;
textures are sampled only after a paint operation contributes coverage. A missing
or pending GPU map uses safe defaults. Texture reads add GPU cost, but additional
materials do not create additional FIL terrain passes. With no palette configured,
the shader excludes material evaluation entirely.

Coordinates are world-aligned meters on a regional tangent plane, derived from the
morphed tile-local terrain vertices and elevation. Tile-to-material transforms are
computed in double precision on the CPU. The shader keeps the local coordinates
small and carries the regional origin separately, preserving its low bits when
computing texture phase. Camera-space positions are used only for the normal-map
frame; zooming does not perturb material coordinates. As with Prestige surface splatting,
the default frame follows
the camera in one-degree cells with hysteresis; changing regions can shift texture
phase. Set `<material_anchor>longitude,latitude,height</material_anchor>` (WGS84,
height in meters) to fix the frame for a regional scene. Materials currently require
a geocentric map. They do not follow stroke direction, displace geometry or perform
parallax occlusion mapping.

osgEarth stores a stable material slot and period in unused components of its own
paint records. The vendor curve/band atlas data is unchanged. Parent atlas and UV
matrix inheritance carries the same paint assignments above `max_data_level`.
The runtime palette resolves those slots to reference-counted arena IDs and handles;
runtime handles are never serialized into an atlas. Material parameters and normals
are coverage-blended at overlaps; this approximates separately lighting translucent
surfaces. Raster fallback tiles retain flat stroke colors rather than PBR materials.

`slughorn_materials.earth` demonstrates multiple selector materials and an outline
material using the bundled textures. Run it with `--nvgl --prestige-sky` for lighting.
