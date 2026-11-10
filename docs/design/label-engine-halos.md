# Prestige halo and outline exploration

## What Slughorn provides

Checked the installed Slughorn headers and the matching cached source revision
`1b203c191bd152c20685f8a896d22dcff974cec4`, plus the upstream osgSlug rendering companion.

- `canvas::Canvas::strokeText()` exists in `slughorn/canvas.hpp`. It retrieves stored glyph contours,
  scales/translates them on the CPU, expands them with `Path::strokePath()`, and registers new atlas shapes.
  Those shapes can then use the ordinary analytic GPU coverage renderer. Stroke expansion flattens curves
  with a tolerance; it is not a constant-cost fragment-shader stroke-width parameter.
- `Path::strokePath()` supports miter, bevel, and round joins, with a miter limit. Its width is the full
  centered stroke width: a two-pixel outward border behind opaque fill needs a four-pixel centered stroke.
- The convenience `strokeText()` loops over bytes of `std::string_view` and applies advances. We must retain
  Prestige's HarfBuzz glyph IDs and offsets for Unicode, ligatures, and marks. A future outline cache should
  expand each shaped glyph once per font/width/size/join combination, then instance it across labels and views.
  Calling `strokeText()` repeatedly for each label would create new contour shapes unnecessarily.
- Slughorn has optional SDF/MSDF baking under `SLUGHORN_HAS_MSDF`, backed by msdfgen. Our vcpkg core-only port
  does not compile that feature or install osgSlug. Enabling it requires dependency/build work and a separate
  comparison of bake time, memory, corner quality, and frame time.
- The upstream osgSlug shader has `osgSlug_Effect_GlowSDF()` and an `osgSlug_FragmentExt` hook for exterior
  effects. It samples a baked distance field alongside analytic fill. This is real GPU halo support, but it
  belongs to the companion renderer and is not automatically part of our Slughorn atlas integration.

Sources:
[pinned stroke implementation](https://github.com/AlphaPixel/slughorn/blob/1b203c191bd152c20685f8a896d22dcff974cec4/slughorn/canvas.hpp),
[pinned atlas API](https://github.com/AlphaPixel/slughorn/blob/1b203c191bd152c20685f8a896d22dcff974cec4/slughorn/slughorn.hpp),
[upstream GPU effects](https://github.com/AlphaPixel/osgSlug/blob/main/src/Atlas.shaders.cpp).
The osgSlug link is upstream reference material, not a pinned dependency of this worktree.

## What Prestige already does

The current renderer has separate, ordered halo and analytic-fill passes. Small text uses a shared FreeType
distance cache: R8, 64 texels/em, 16-texel spread, an eight-MiB cache budget within the total atlas budget.
Cached halos are eligible up to 32 displayed pixels/em and only when their footprint fits the stored range.
Missing cache entries, larger text, or out-of-range widths use eight displaced analytic coverage samples
plus the central sample. The latter is an approximation to dilation, not a true vector stroke.

Styles expose halo color, width (zero to two logical pixels), and optional softness. Join style, hollow
text, and multiple rings remain outside this slice. Glyph geometry and collision footprints already include halo padding.
All glyph halos draw before all glyph fills, preventing the next glyph's halo from covering the previous fill.

## Crisp versus soft experiment

The transient harness in `build/label-halos` compares the production crisp distance threshold against a soft
`smoothstep` across the same two-pixel radius. Both use identical glyphs, colors, geometry, caches, blending,
two passes, and Slughorn analytic fills. No extra texture reads, blur passes, or CPU glyph work are added.
This initial soft variant was a renderer experiment. The integration below promotes its approach into
Prestige without adopting osgSlug's shader.

The samples cover 13, 16, and 24 pixel text, rotated labels, light/dark fills and backgrounds, accented names,
ligatures, and 16 logical pixels at 2x density. This narrow comparison stays inside the current cache's valid
range. It does not establish quality for large text, wider halos, unavailable caches, or terrain backgrounds.
The original analytic fallback remains crisp in this experiment; shipping a softness control needs a defined
fallback policy and wider quality coverage first.

## Original follow-up plan

1. Keep analytic Slughorn fill and the shared distance cache. Add an independent soft-edge width to halo
   styling, with zero preserving the current crisp result. Treat radius as the full outer extent so collision
   padding and label density remain predictable.
2. Specify behavior for cache misses, high DPI, large text, and widths outside the cache's distance range.
   Broaden the cache or use cached stroked glyphs where justified; avoid silently changing softness at a threshold.
3. Compare true cached contour strokes against distance outlines for large text and sharp joins. Preserve
   HarfBuzz placement and put all contour preparation on the font worker. Account for additional atlas pages
   and style-dependent cache keys.
4. Validate glyph interiors, counters, accents, neighboring halos, fades, both camera states, and road following.
   Measure one-camera frame time and GPU time at fixed label density, plus cold bake cost and memory separately.

## Implemented symbology integration

The private `LabelStyleInternal.h` adapter consumes real `Style`/`TextSymbol` objects, including CSS and structured
symbols. Existing size/fill/halo/offset/outline/none controls map to immutable renderer styles on layer open.
Standard symbol values take precedence over prototype attributes. Normalized halo offset scales with logical
font size; this does not promise pixel-identical legacy osgText outlines, whose rasterization differs.
CSS text styles without a halo leave it disabled. Unsupported shadow/depth-backdrop modes and nonconstant
size expressions fail explicitly.

The user selected `halo_softness`, a Prestige-only 0-1 feather fraction, instead of a pixel-valued blur property.
Zero preserves crisp coverage. Positive values move the inner end of a smoothstep across that fraction of
the existing halo radius. Outer bounds, road glyph footprints, and declutter bounds remain unchanged.
Softness is one additional immutable float per glyph, with a context-local instanced attribute. Camera-local
placement, draw ownership, fades, and path fitting remain independent. Core TextSymbol ABI is unchanged.

For cache misses or outside the distance-cache range, soft fallback integrates four radial dilations with
smoothstep-derived weights (32 shifted analytic samples). Crisp fallback retains eight shifted samples.
This approximation costs more than cached halos; it does not silently turn softness off. Atlas memory stays
bounded and shared, and no per-frame CPU contour or blur work is introduced.

`openfreemap_labels.earth` now uses text CSS with explicit 2px widths, feathered road halos (0.5), and crisp place
halos. It retains automatic smooth GPU road following. Usage and limits are documented beside the demo.
Transient tests, captures, and paired measurements are under `build/label-halo-style`.

Wider halos, true contour strokes, shadows, and improving the large-text/cache-miss fallback remain follow-up
work. The original experiment's captures and measurements remain under `build/label-halos`.


## Explicit pixel width and distance calibration

Prestige now accepts `text-halo-width: 2px` (or `2`) in each rule. It is a constant logical-pixel
outward radius and overrides normalized `text-halo-offset`, independent of CSS order and font size.
The current supported range remains 0-2. Invalid units, expressions, negative/nonfinite values, and
larger widths fail on layer open. Halo color must be present; backdrop `none` still disables rendering.
Structured symbols use `symbols/text/halo_width`. This is a private Prestige extension parsed with
CssUtils alongside TextSymbol; legacy TextSymbol layout/ABI and rendering are unchanged. Preserve the
extension in the layer config rather than serializing it through a standalone legacy Style.

The pixelation investigation found incorrect distance units, not a need for a larger atlas. FreeType's
bitmap SDF encodes `128 * (distance / spread + 1)`. With spread 16 and a 64-pixel/em bake, conversion to
em is `(byte - 128) / 512`. The previous divisor 256 doubled distances, so the threshold produced only
half the requested halo radius. Correcting this makes a requested 2px halo agree with the analytic
reference and restores a substantial antialiased border around place names. The soft path also uses
the corrected distances. Analytic fills, halo footprints, camera fades, shader specialization, texture
sample counts, and the 8 MiB distance-cache budget are unchanged. No higher-resolution atlas is needed
for this correction. High-DPI/large text outside cache eligibility retains its existing analytic path.

Earlier halo comparisons in this document used the miscalibrated cached radius. Their timing results
remain historical measurements, but they do not establish matching cached/analytic widths. See the
pixel-width section in the performance report for calibrated measurements and framebuffer checks.


## Fade blending under a VisibleLayer

VisibleLayer installs a straight-alpha BlendFunc with OVERRIDE. Prestige emits premultiplied RGB from
both halo and fill shaders, but its unprotected BlendFunc previously lost to that inherited state.
Partial opacity was therefore applied twice to RGB. A white halo at 50% fade contributed 25% white
while retaining 50% background, creating a dark ring. Antialiased edges were affected even at full fade.

The label draw StateSet now marks its premultiplied BlendFunc and blend-enable mode PROTECTED, so they
survive the layer's override. This is confined to Prestige glyph draws; it does not change VisibleLayer
or other map content. Both passes still use the existing per-camera fade and layer-opacity uniforms.
No shaders, fade curves, placement histories, geometry, cache budgets, or draw counts change.

The regression harness uses a real VisibleLayer StateSet above the production ViewNode, paired with
an otherwise identical standalone reference. It renders to a WGL pbuffer; capability probing is
headless and no viewer window is opened. Samples cover 0/25/50/75/100% fade, 50% layer opacity, crisp and
soft cached halos, and analytic fallback. Artifacts and comparisons are transient in build/label-fade.


Validation: all 17 fade/opacity combinations match the standalone premultiplied reference exactly after
the fix; all standalone before/after captures also match. The zero-opacity endpoints are background-only.
On the London stem's white halo at 50% fade, the old inherited blend produced RGB (147,155,144) over
background (166,184,166), visibly darker than the background. The corrected result is (210,218,204),
exactly the expected half-opacity blend. The same checks pass for soft halos and analytic fallback.
Twenty-three motion/dynamic/style tests pass (378 assertions), preserving independent camera histories,
fade reversals, and update behavior. No viewer was launched for these checks.


## Stylesheet softness

`text-halo-softness: 0.5` is the preferred Prestige syntax. It is constant and unitless, with the same
0-1 meaning and default zero. CSS overrides the legacy `halo_softness` rule attribute; structured text
symbols accept `halo_softness` as well. Invalid or nonfinite values and units fail on layer open.
The demo's existing road softness values now live in the stylesheet. Parsing happens only on layer
open; renderer styles, shader programs, footprints, fades, and frame costs are unchanged.
