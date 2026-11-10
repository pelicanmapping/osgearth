# OpenFreeMap + Prestige labels

From this directory, in a Windows command prompt:

```bat
call ..\osgearth_shell.bat
osgearth_imgui openfreemap_labels.earth
```

The ImGui viewer (or `osgearth_viewer`) loads `prestige:labels` from the `osgEarthPrestige` nodekit. No custom label application is
required. Build with Prestige, MVT/Protobuf, and `OSGEARTH_ENABLE_SLUGHORN=ON`. Live tiles require internet access.

Drag and zoom normally. Number keys select the bookmarks:

1. London: cities, towns, and neighborhoods.
2. Europe: regional place labels.
3. World: countries and major places.
4. Manhattan: city and neighborhood labels.
5. Neighborhood detail: London streets and curved road names at a 2.5 km range.

The basemap builds on `openfreemap_vector.earth`, adding a zoom-dependent administrative boundary hierarchy:
regional/state/province levels 3-4 from source tile level 4, district/county levels 5-6 from level 7,
and local jurisdiction levels 7-10 from level 10. Finer boundaries use thinner, lighter purple lines;
national borders paint last. Maritime boundaries are excluded. Administrative meanings vary by country,
and lines appear only where supplied by the [OpenMapTiles boundary layer](https://openmaptiles.org/docs/schema/#boundary).
A separate `XYZFeatures` source requests
OpenFreeMap's `place` and `transportation_name` layers for labels. It uses native Web Mercator tiles through source level 14, retaining
source feature IDs as coverage changes and continuing to display level-14 labels when zoomed closer. Decluttering runs
in the background; each primary camera owns separate placement, history, fades, and device-pixel scaling.
Shadow, picking, and RTT cameras do not automatically register label views. At most eight primary cameras are
supported by this prototype.

English names are preferred, with the native `name` as fallback. The current single-font renderer skips a label
if that font cannot render its glyphs; it does not suppress the rest of the tile. Automatic font fallback and POI icons are not included. Local fonts resolve to Segoe UI on Windows, or DejaVu Sans, Liberation
Sans, or Arial on other systems. To select a font, add a local file path inside `<prestige:labels>`:

```xml
<font>/path/to/font.ttf</font>
```

The first matching geometry/class/level style supplies the size, colors, and priority. Lower source `rank` means more
important: effective priority is the style's `priority` minus that rank. Rule levels are source tile levels,
not camera distance. `placement="line"` fits the entire shaped name along a road; omitted placement means a point.
Roads and places share the same background collision pass, with places given higher priority in this demo.
`pixel_scale` (default 1) multiplies the current camera context's device-pixel ratio.
The layer is intended for geocentric maps. Public C++ APIs are installed as `osgEarthPrestige::LabelLayer` and `LabelNode`.

The label path keeps Slughorn analytic letter fills and uses cached halo distances where appropriate. Shared
immutable glyph pages and tile batches do not share mutable camera history. Visibility and opacity use the
normal layer controls. Place styles never enter the basemap's FeatureImage raster fallback path.

Road placement uses screen-space arc length with upright per-glyph tangents, retaining HarfBuzz shaping and
Slughorn analytic rendering. Names must fit completely and reject sharp bends. Multipart geometry retains its
longest tile-clipped continuous piece. One placement is allowed per source feature ID in each camera; different
source IDs can name different sections of the same road. This slice does not stitch tiles, repeat long-road
names at fixed intervals, draw route shields, or perform terrain occlusion. Conservative rectangles can suppress
some otherwise usable diagonal-road placements.

Smooth GPU path following is the default when osgEarth reports OpenGL/GLSL 4.3 support. On older supported
systems (GLSL 3.30+), the layer automatically uses the CPU path: anchors reproject every frame, with spacing
and shape refitted at the 15 Hz background placement cadence. Explicit `true` also falls back when unsupported.
To force that CPU path, set this inside `<prestige:labels>` and reload the earth file:

```xml
<gpu_paths>false</gpu_paths>
```

Capability selection happens once when the layer opens, using osgEarth's common capability probe. Applications
that explicitly create older contexts on a newer GPU must force `false`; automatic selection does not negotiate
different GL versions per camera. GPU mode requires OpenGL 4.3 in every rendering context. The placement worker still chooses
roads and handles collisions and camera-local fades. A compute pass refits accepted glyphs every frame and
interpolates tangents as letters cross polyline vertices. This removes the 15 Hz spacing/shape steps during
zoom. The existing background collision cadence, road-selection policy, and tile-fragment identities still apply;
cross-tile stitching remains deferred. Near-horizon or too-short paths can be suppressed immediately.
The worker uses matching smooth tangents for collision footprints, so eligible density can differ slightly
from the original mode on curved roads. See `docs/design/label-engine-performance.md` for paired frame timings;
GPU cost grows with retained path complexity. The switch preserves the measured CPU control.

Each class rule accepts the existing `Style`/`TextSymbol` parser: CSS in the rule body, or a
`<symbols><text>...</text></symbols>` block. Supported controls are constant `text-size`, `text-fill`
(and `text-fill-opacity`), `text-halo`/`text-halo-color`, `text-halo-offset`, and `text-halo-backdrop-type`. Prestige additionally accepts `text-halo-width` and `text-halo-softness`.
Backdrops support `outline` and `none`. Drop shadows and legacy `text-halo-implementation` strategies
report a configuration error. This adapter does not create legacy text nodes.

```xml
<style classes="city" priority="700">
    text-size: 22;
    text-fill: #25364a;
    text-halo: #ffffff;
    text-halo-width: 2px;
    text-halo-softness: 0;
    text-halo-backdrop-type: outline;
</style>
```

`text-halo-width` is a Prestige-only constant logical-pixel radius: `2px` and `2` both mean two pixels.
It overrides `text-halo-offset` regardless of property order or font size. Structured symbols use
`<symbols><text><halo_width>2px</halo_width>...</text></symbols>`; keep this extension in the layer config
when serializing, since a standalone legacy `Style` does not retain it. The current radius limit is 0-2;
negative/nonfinite values, expressions, other units, and larger widths fail on layer open.
Camera device-pixel ratio applies afterward. The compatibility `text-halo-offset` remains normalized
glyph-cell size: 0.0625 at font size 16 means one logical pixel. CSS text styles
without `text-halo` have no halo; `text-halo-backdrop-type: none` explicitly disables one.

`text-halo-softness` is a **Prestige-only** stylesheet property with a constant, unitless value.
It overrides the older `halo_softness` rule attribute, which remains a compatibility fallback.
Structured symbols use `<halo_softness>` inside `<symbols><text>`. Like pixel width, preserve this
extension in the layer config rather than serializing through a standalone legacy `Style`.
Nonfinite values, values outside 0-1, expressions, percentages, and pixel units fail on layer open.
It ranges from 0 (crisp, the default) to 1 (feather across the entire radius); 0.5 feathers the outer half.
It does not enlarge the radius or collision footprint. The demo uses 0.5 for roads and crisp halos for places.
Its pure-white halos are 2 logical pixels wide at every font size, for separation from the pale basemap.
Small text uses cached distances. Cache misses, out-of-range distances, or text above 32 displayed pixels/em
use analytic fallback; soft fallback costs more than crisp fallback. It remains soft instead of silently
switching to crisp. This is feathering, not Gaussian blur or vector-stroke join control.

Prototype `size`, `fill`, `halo`, and `halo_width` attributes remain accepted. Explicit standard symbol values
override them; mixing forms is unnecessary in new files. Top-level font/source/rank controls and class/level/
placement selectors remain unchanged. Per-feature text-size expressions are not implemented.
See [the halo design note](../docs/design/label-engine-halos.md) for renderer tradeoffs.

Data and schema: [OpenFreeMap](https://openfreemap.org/),
[OpenMapTiles place schema](https://openmaptiles.org/docs/schema/#place),
[transportation names](https://openmaptiles.org/docs/schema/#transportation_name), and
[OpenStreetMap contributors](https://www.openstreetmap.org/copyright).
Attribution and the current tile endpoint are also supplied by the source TileJSON.

## Edit label appearance live

Open **Prestige > Label styles** in `osgearth_imgui`. Choose a class/level rule to edit text size,
fill, halo color, halo width, softness, and inclusive source-tile Min/Max level. Declutter padding applies
to the whole layer, in logical pixels per side. Live preview coalesces slider edits; disable it to use Apply.
Revert restores the appearance captured when selecting the layer. Copy styles XML copies the applied
`<styles>` block for pasting into the earth file. The demo file is not modified automatically.
Appearance and padding edits preserve loaded feature tiles and camera fades. Level-range edits refresh
affected tile levels in the background and preserve existing camera histories. Copy styles XML includes
rule levels; the layer-wide margin is the separate `<declutter_padding>` setting.
