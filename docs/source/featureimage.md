# FeatureImage

FeatureImage renders styled feature data as terrain imagery. Its default `raster`
mode uses the existing feature rasterizer. Optional `vector` mode evaluates
Slughorn curves on the GPU, preserving smooth edges when zooming past the source LOD.

Enable `OSGEARTH_ENABLE_SLUGHORN` in CMake (default: OFF) to build Slughorn
statically and enable `FeatureImage` layers with `mode="vector"`. Use the normal
`configure.bat` and `build.bat` workflow on Windows. The implementation requires
a C++20-capable compiler; osgEarth's public headers and consumer compilation
remain C++17 (or the existing C++14 polyfill mode). Only the private Slughorn
translation unit and SDK core compile as C++20.

CMake downloads a pinned, SHA512-verified Slughorn source archive. It builds
only the core, with no Python, examples, or optional SDK backends. For offline
builds, set `FETCHCONTENT_SOURCE_DIR_OSGEARTH_SLUGHORN` to a checkout of the
pinned revision before configuring. The revision and checksum are in
`cmake/Slughorn.cmake`; the shader depends on that revision's atlas format.

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
limits are also honored. Atlases are authored in the requested source tile's
normalized coordinates. REX's existing parent texture and scale/bias matrix
therefore select the correct subregion at higher LODs. Pixel widths refer to
pixels at the source tile's nominal `tile_size`, as with raster imagery; they
are not constant screen-pixel widths while zooming. Meter widths are converted
at the source tile latitude using the rasterizer's distance policy.

## Rendering

Each paging request owns a private Slughorn atlas. Curves retain 32-bit float
precision; coordinates are rebased against the tile origin in double precision
first. Shape metadata, ordered 64-by-64 and 16-by-16 spatial indexes, curves, and
compact band data are packed into one immutable RGBA32F texture. Band lookup
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
and can coexist with vector parent or child tiles. Opening an explicitly
requested vector layer fails with an explanatory status if Slughorn or the
required GLSL support is unavailable. The default raster mode is unchanged.

The pinned Slughorn Blend2D adapter consumes BLPath objects, rather than
intercepting BLContext operations, and targets a newer Blend2D API than this
project's current vcpkg build. This implementation uses Slughorn's native Canvas
path API. A future shared Blend2D path producer could reduce duplicated style
conversion without exposing C++20 SDK types in osgEarth headers.
