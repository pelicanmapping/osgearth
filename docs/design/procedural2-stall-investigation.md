# Loading-stall investigation — September 28, 2026

## Capture evidence

User-provided session: `D:/Captures/2026-09-28_13-43-03_osgearth_imgui.slp`.
Inspected the open matching session in Superluminal Performance 1.0.7359.1293.
This was analysis of an existing capture, not a benchmark or a new recording.

Searching Function Statistics for `osgEarth::TextureArena::apply`, with All Threads selected,
found 209 sampled instances totaling 3.621171 seconds; the median was 243 microseconds.
The four longest listed instances were all on Main:

| Start offset in capture (ms) | Duration (ms) |
| ---: | ---: |
| 4532.58 | 788.64 |
| 5963.81 | 613.21 |
| 3170.82 | 507.66 |
| 4099.53 | 396.72 |

These are profiler sampling estimates, not instrumented frame boundaries or an exact count of API calls.
All four precede the window shutdown interval.

Expanded the two longest instances in the call graph:

| Inclusive path | First instance (ms) | Second instance (ms) |
| --- | ---: | ---: |
| `TextureArena::apply` | 788.64 | 613.21 |
| `Texture::compileGLObjects` | 788.55 | 613.21 |
| `glTexSubImage2D` and descendants | 588.34 | 455.44 |
| Another direct NVIDIA driver child of texture compilation | 187.79 | 150.90 |

The second driver child lacks a resolved function name; do not label it as mipmap generation without
further evidence. The stacks originate in the viewer's render traversal and state application.
The two inspected stalls are in texture compilation/driver work, not OSM fetching or an arena mutex wait.
This does not exclude other stalls or background loading as a contributor to resource pressure.

The originally selected 74.53 ms `Viewer::eventTraversal` interval at 11069.96 ms contains
`GraphicsContext::close` (74.41 ms), window destruction, then viewer destruction and process exit.
That interval is application shutdown, not evidence of diagnostic windows being recreated during paging.

## Code findings and remaining hypotheses

- `src/osgEarth/TextureArena.cpp`: `TextureArena::apply` processes the current pending compilation batch
  without a time or byte budget. `Texture::compileGLObjects` uploads image data on that GL thread.
- `src/osgEarth/TextureArena`: `Texture::compress` defaults to true. For uncompressed RGB/RGBA inputs,
  the compiler selects DXT storage and uploads uncompressed pixels, requiring driver conversion.
  Missing mip levels are also generated during compilation. Driver compression and mip generation
  are plausible sources of these costs, but the opaque driver samples do not prove that attribution.
- Terrain and vegetation both use texture arenas; other model content can too. This capture inspection
  does not identify the slow textures' names, dimensions, formats, count, or owning layer. Do not assign
  these stalls specifically to vegetation, terrain imagery, or OSM buildings yet.
- The existing demo launcher does not request `--ico`. Incremental compilation is worth checking, but
  it is not a proven fix for this capture and cannot make a single expensive GL call interruptible.

Earlier code-audit candidates remain secondary until measured: unlimited default page merges/unloads,
two-second vegetation page expiration, linear GL-pool recycling searches, a small decoded-feature cache,
and texture-arena locking across optional image reads. None was established as the dominant cost in
the two inspected stalls.

## Next diagnostic and likely remedies

### Layer isolation follow-up

The user reports that removing the imagery layer did not eliminate the stalls. Imagery is therefore
not required to reproduce the symptom; this does not by itself identify the owner in the saved capture.
Subsequent user tests in separate fresh runs reproduce stalls with Vegetation2 alone and Prestige alone.

Vegetation2 does use textures: the starter tree impostors and canopy sources reference PNG atlases
(2048 x 512 for the broadleaf and conifer atlases). Shrub, grass, fern, and boulder coarse assets also
reference smaller atlases. `AssetCatalog::acquireModel` shares live source bundles; `acquireCanopy`
assembles geometry using those source materials and retains the sources. It does not bake a new
texture for every canopy page. The cache is weak, so source assets can be loaded again after their
last resident owner disappears.

Prestige also uses a shared Chonk texture arena and a shared detail factory. One inspected local LOD0
document references reusable BuildingKit, BridgeKit, and PowerlineKit KTX2 textures. The Basis reader
normally produces BC1/BC3 compressed images, with explicit/fallback RGBA decoding paths. This one
document does not establish the formats of every Prestige page or the textures present in the capture.

The observed uncompressed upload entry point is consistent with Vegetation2's then-PNG runtime atlases being
driver-compressed. Already-compressed images normally take `compressedSubImage2D` instead. This is a
useful discriminator, not proof of ownership. Isolate Prestige and Vegetation2 in separate fresh runs
over the same camera path, then identify the actual slow textures before changing upload policy.

Identify each slow compilation's texture name/category, dimensions, source and GPU formats,
mip count, and uploaded bytes. Distinguish many uploads in a batch from one expensive upload.
Then compare the same workload with driver compression disabled or with precompressed, premipped inputs.
Track residency/reuploads if the same textures repeatedly compile.

Depending on those results, remove runtime compression from the frame-critical path, prepare mipmaps
before rendering, and schedule bounded texture upload batches. Existing incremental compilation may
help schedule admission, but texture identity and timing should guide the change.

### Vegetation asset preparation -- September 28

The starter baker now writes BC3/DXT5 DDS files with every mip through 1x1. All nine starter atlases
(seven individual/static-cluster atlases and two canopy-source atlases) have been converted; their OSG model
references now use DDS. The PNGs remain editable inputs. `--compress-only` updates this encoding without
rebaking source geometry. The full bake and canopy-only bake also produce the compressed runtime files.
This removes driver compression and mip generation from these assets' normal TextureArena upload path.
It does not establish the measured reduction in frame stalls or eliminate GPU transfer/residency costs.

The user's expectation that a handful of assets should stay resident is correct while live page owners remain.
The current AssetCatalog is a weak cache: losing the final page/template owner immediately releases a bundle;
the arena subsequently purges orphaned textures. There is no bounded warm cache or grace period for source art.
A new aggregate template shares live source materials; it does not require a new atlas. Therefore ordinary
page-to-page movement with continuously resident source owners should not keep recompiling those images.
No repeated-upload defect with continuously live owners has yet been established. A small, separately bounded
warm cache for source art is a possible follow-up, along with recording actual evictions/reuploads if stalls remain.

No renderer code, demo earth configuration, or profiler installation was changed. The offline baker, generated
asset references/DDS payloads, asset validation, and documentation are the changes in this checkpoint.

Validation: build/install passed. All 45 Procedural2 test cases passed (1,204,272 assertions, plus isolated GPU
workers). The new OSG image-load check covers all nine atlases: BC3 format retained, all 11/12 mip levels present,
payload sizes correct, source orientation preserved, and color/alpha error bounded. All 22 atlas references in
the generated OSG files resolve to DDS. GPU canopy and impostor contracts pass with the compressed source art.
An OSM scene capture loaded the compressed assets with no asset failures or budget denials, but its overview was
not visually useful for comparison; the controlled GPU canopy image is the visual check for this checkpoint.
No fresh profiling capture or before/after stall-time measurement has been made.
