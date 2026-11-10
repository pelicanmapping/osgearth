# Prestige LabelLayer

`osgEarthPrestige::LabelLayer` is the public shared label layer. Include `<osgEarthPrestige/LabelLayer>`
and link `osgEarthPrestige`. It uses the existing `<prestige:labels>` earth-file element, so the demo and
saved earth files remain compatible. The public header is installed with the other nodekit headers;
font shaping, placement workers, tile management, rendering packets, and shaders remain private.

## C++ configuration

Configure the layer before opening or adding it to a map. Given an application-owned feature source
`places` and a geocentric `map`:

```cpp
#include <osgEarthPrestige/LabelLayer>

using osgEarthPrestige::LabelLayer;
LabelLayer::Options options;
options.featureSource().setLayer(places);
options.textAttribute() = "name";
options.fallbackTextAttribute() = "name:en";
options.maxLevel() = 14u;
options.gpuPaths() = true;

LabelLayer::StyleRule city;
city.classes = { "city", "town" };
city.priority = 700.0;
city.placement = "callout";
city.style = osgEarth::Config("style",
    "text-size: 22; text-fill: #25364a; text-halo: #ffffff; "
    "text-halo-width: 2px; text-halo-softness: 0;");
options.styles().push_back(city);

osg::ref_ptr<LabelLayer> labels = new LabelLayer(options);
map->addLayer(labels);
```

Alternatively, use `options.featureSource().setExternalLayerName("places")` to resolve a source already
in the map, or configure `LayerReference` with embedded source options. `setFeatureSource()` replaces
any named/embedded reference with a retained source pointer. `getFeatureSource()` returns the resolved
source, or null before resolution. Explicit sources remain configured through close/reopen.

`Options::getConfig()` and the Config constructor round-trip the controls and ordered rules. A rule's
`style` accepts the same CSS or structured symbol Config as the earth file. Ordinary `osgEarth::Style`
values can be assigned with `style.getConfig()`; retain raw CSS/Config for Prestige-specific halo width
and softness properties that the core TextSymbol does not represent. Rule selector members override
selector fields in the retained Config when serialized. The first matching class/level rule wins.

## Options

| C++ option | Earth-file key | Default |
| --- | --- | --- |
| `featureSource()` | `features` | Required |
| `font()` | `font` | Available platform font |
| `pixelScale()` | `pixel_scale` | 1.0, multiplied by camera device-pixel ratio |
| `declutterPadding()` | `declutter_padding` | 2 logical pixels per side; finite and nonnegative |
| `textAttribute()` | `text_attribute` | name:en |
| `fallbackTextAttribute()` | `fallback_text_attribute` | name |
| `rankAttribute()` | `rank_attribute` | rank; smaller ranks win |
| `classAttribute()` | `class_attribute` | class |
| `maxLevel()` | `max_level` | 14, allowed range 0-16 |
| `tilePixels()` | `tile_pixels` | 512 logical pixels |
| `gpuPaths()` | `gpu_paths` | true, with worker fallback below GL 4.3 |
| `styles()` | `styles/style` | Ordered rules; required with a feature source |

`declutterPadding()` adds a collision margin around each label footprint in the shared background
solver, for both tiled labels and LabelNodes. For example, `<declutter_padding>4</declutter_padding>`
reserves an extra 4 logical pixels per side, giving neighboring footprints 8 pixels of separation.
The margin uses each camera's effective device-pixel ratio (including `pixelScale()`); placement and
fades remain independent per camera. Zero removes this extra margin, retaining the text/halo footprint.
Use `setDeclutterPadding(pixels)` for live changes; `getDeclutterPadding()` returns the accepted value.
Edits request a new background solve, preserving camera history and fades. No text or tiles are rebuilt.

Each `StyleRule` contains class tokens, inclusive `minLevel`/`maxLevel` (0/16 by default), priority (0),
placement (`point`, `line`, or `callout`), and text styling. Invalid settings return a configuration error
on open. Existing constant-text styling and callout limits still apply; this change adds no new draw work.

## Runtime styles and the ImGui editor

`getStyles()` returns a synchronized copy of the current ordered rules. On the update/control thread,
call `setStyle(ruleIndex, config)` to replace one rule's text appearance. It validates before accepting:
invalid values or indices leave the old configuration and rendering intact. Success means the style
was accepted for asynchronous preparation; labels retain their old appearance until prepared results
arrive. Calls must be serialized with layer lifecycle operations. Do not mutate `options().styles()`
while the layer is open. Export options/configuration on the same control thread, or build a styles
block from the thread-safe `getStyles()` snapshot.

```cpp
auto rules = labels->getStyles();
osgEarthPrestige::LabelLayer::StyleValues appearance;
auto status = labels->resolveStyle(rules.at(0).style, appearance);
if (status.isOK())
{
    appearance.size = 24.0f;
    appearance.haloWidth = 2.0f;
    appearance.haloSoftness = 0.5f;
    status = labels->setStyle(0, appearance.getConfig(rules[0].style));
}
```

`StyleValues` covers size, RGBA fill/halo colors, halo width and softness. Its `getConfig(base)` overlays
those values on CSS or structured text symbols, retaining other properties and the base referrer.
CSS formatting/comments are normalized when edited. Direct `setStyle` calls accept the same constant
style Config as initial configuration. Rule order, classes, priority, placement mode and the layer's font remain unchanged; node-local `LabelNode::setStyle()` remains independent.

The overload `setStyle(ruleIndex, config, minLevel, maxLevel)` also accepts inclusive source-tile level
bounds, requiring `0 <= minLevel <= maxLevel <= 16`. The source/layer maximum still caps tile selection.
It accepts appearance and bounds together. Changes to level eligibility lazily requery affected tile
levels on the source worker, including formerly empty tiles, so features previously filtered out can
appear and later matching rules can take over. Old coverage remains while replacements load; query
failures retain it and retry with backoff. Offscreen tiles refresh when requested. Camera registrations,
identity-based history and fades remain intact; appearance-only changes still make no source queries.

The tile worker keeps one pending table and coalesces superseded edits. Only batches containing changed
rules are replaced. Paint changes reuse footprints and path data; size/width changes refresh bounds and
road glyph distances from already shaped runs. Both paths share existing glyph atlases and make no
feature queries. Cached offscreen tiles and subsequently loaded tiles use the latest table. Record
identities/revisions, camera registrations, placement history and fade curves survive the change; a
new footprint can naturally change collision winners. The cull path checks one atomic refresh flag for padding edits; draw calls, shader uniforms and per-glyph
work remain unchanged. Existing bounded publications retain old content while frames use it.

In `osgearth_imgui`, open **Prestige > Label styles**. Select a layer/rule, edit appearance and Min/Max level, use Live preview
or Apply, and Revert to the settings captured when the panel selected that layer. Declutter padding is
layer-wide, measured in logical pixels per side, and applies to both feature labels and LabelNodes. Live edits are throttled
to at most one submission every 150 ms and are queued onto viewer update traversal. Reload from layer
refreshes external API edits and establishes a new revert point. Copy styles XML exports the accepted
rules as a `<styles>` block, excluding unapplied drafts. It does not write the earth file. This first
panel edits existing feature rules; class selectors, fonts, shared styles and LabelNode editing are separate work.
Copy styles XML includes rule levels; layer-wide padding serializes separately as `<declutter_padding>`
in the layer configuration.

Validation: the normal build/install and backend-disabled API check pass. Focused regression checks
cover appearance serialization, invalid edits, shared runs/atlases, point/road bounds, camera-local
winners/fades, rapid latest-value updates, no source requery and mixed LabelNode/tile rendering.
The live OpenFreeMap ImGui check verified city-size preview, Revert and Copy styles XML. Single-camera
before/after measurements and transient harness locations are in the performance report.

## Lifecycle and availability

Use `setStyle()` for live appearance/level changes and `setDeclutterPadding()` for collision spacing. For other configuration changes, remove the layer from its map,
close it, then reopen/re-add it.
Stop traversal before lifecycle changes. Named sources must precede the label layer in the map. Sources
must have stable feature IDs. Placement, fade history and rendering state remain independent for up to
eight primary cameras; picking/shadow/RTT cameras do not acquire placement slots.

The public class and factory are available in every Prestige build. Rendering requires
`OSGEARTH_ENABLE_SLUGHORN=ON` and GL 3.3. Without the backend, `open()` returns `ServiceUnavailable`.
Applications can include `<osgEarth/BuildConfig>` and test `#ifdef OSGEARTH_HAVE_SLUGHORN` for backend
availability. This reports Slughorn support in osgEarth; check `OSGEARTH_HAVE_PRESTIGE_NODEKIT` as well
when conditionally using LabelLayer. The macro is undefined in builds with Slughorn disabled.
Smooth road following uses GL 4.3 when available; set `gpuPaths()` to false when using older application
contexts. Current limitations include geocentric maps only, no per-glyph font fallback, no picking, and
no leader-crossing optimization. Unsupported glyph runs are skipped.

The public ABI version is 213. Rebuild applications against the matching installed headers/libraries.
Visual Studio groups now come from the normal library helper: Sources, Headers, Shaders, and Templates.
Generated project/filter files are not edited manually.


## Validation

The Windows build/install passes with the cached D:\vcpkg dependencies. Seventy-seven focused tests pass,
including public construction and exported factory typing, typed option/CSS/symbol round trips, invalid
configuration, explicit-source close/reopen, and existing tiled/road/callout/multi-camera behavior.
A standalone compile of the backend-disabled implementation verifies the unavailable status. The installed
header matches the source, and generated VS filters were checked for every label source/header/shader.
Transient checks and logs are in `build/label-public`; temporary build hooks are removed afterward.

## Scene-graph LabelNode

Include `<osgEarthPrestige/LabelNode>` and bind each node explicitly to the `LabelLayer` whose tiled
labels it should compete with. The layer may omit its feature source and rules for a node-only scene.
The node does not own the layer, and no global/default layer is inferred.

```cpp
#include <osgEarthPrestige/LabelNode>
#include <osgEarth/GeoTransform>

osg::ref_ptr<osgEarthPrestige::LabelNode> label =
    new osgEarthPrestige::LabelNode(labels, "My annotation");
label->setPriority(1000.0f); // Higher values win, including against tiled labels.
label->setStyle(osgEarth::Config("style",
    "text-size: 22; text-fill: #25364a; text-halo: #ffffff; "
    "text-halo-width: 2px; text-halo-softness: 0;"));

osg::ref_ptr<osgEarth::GeoTransform> anchor = new osgEarth::GeoTransform;
anchor->setPosition(osgEarth::GeoPoint(
    osgEarth::SpatialReference::get("wgs84"), -0.12, 51.5, 0.0, osgEarth::ALTMODE_ABSOLUTE));
anchor->addChild(label);
sceneRoot->addChild(anchor); // Same world root as the map/layer.
```

`setPosition(Vec3d)` supplies a local offset under ordinary parent transforms. Parent rotation and scale
move the anchor; glyphs remain screen-facing and pixel-sized. Use one parental path per LabelNode and
clone it for another location. Camera-dependent parent transforms are not supported by the shared world
anchor. Terrain clamping belongs to GeoTransform. Literal UTF-8 text, priority, position and constant
Style/Config text styling are mutable during exclusive scene access; empty text hides the label. Font
selection is layer-wide. Node StateSets do not style the batched text; use `setStyle`.

All bound nodes share one bounded asynchronous dynamic source per layer. Static traversal does not
resubmit text or rebuild geometry; edits replace only their 64-slot chunk. A layer currently admits
1024 live registered nodes, at most 4096 UTF-8 bytes per label and 1 MiB total text, with the existing
8 MiB dynamic prepared-data budget. Large populations should continue to use multilevel feature tiles.

Each primary camera records only the nodes reached by its traversal, respecting masks, Switch/LOD
selection and callbacks. Fixed-radius OSG culling is disabled for screen-sized nodes (and, through OSG's
normal propagation, their ancestor branches); explicit anchor projection rejects offscreen/behind-camera
labels, with geocentric horizon filtering in the worker. Organize/cull large collections at the source.
Membership comes from the previous completed camera frame, independent of child order. Changes then
use the existing asynchronous placement cadence and fades; masks/removal are not immediate hard cuts.

Tiled and node records enter the same priority/collision solve, with no separate annotation overlay.
Placement history, outgoing fades and draw packets stay camera-local. The dynamic source shares its
glyph cache across all nodes/cameras; tile preparation retains its own existing font cache. Render
packets preserve separate dynamic chunks so editing one node cannot repack an entire tiled population.
Layer removal/reopen retires camera state, restarts preparation, and retains live node registrations.

Icons, shared styles and adopting/importing a style specification remain on the implementation plan's
TODO list. The first ImGui layer-style editor is described above.

Validation for this slice: build/install with cached D:\vcpkg dependencies, 85 focused tests (including
continuous parent motion, concurrent camera collection, shared tile/node priorities, masks, cloning and
reopen), and a standalone backend-disabled compile/link/run. Installed headers and Visual Studio
Sources/Headers groups were checked. Tests and benchmarks remain transient under `build/label-node`.
