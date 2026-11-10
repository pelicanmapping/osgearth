# Prestige labels: single-camera performance

November 8, 2026. These passes target one camera, as requested. The completed-frame rendering results below
follow the original preparation and incremental-update measurements. Background glyph preparation and cached
text preparation improve materially; the dense text GPU workload is substantially faster. The small-label
global viewer remains CPU-bound. No label-density
reduction, solver-policy change, reduced update cadence, or multi-camera performance claim is involved.

## Changes

- Count glyphs per occupied atlas page before allocating instance arrays. Each of the ten attribute buffers
  allocates its exact capacity once instead of repeatedly growing and copying as glyphs arrive.
- Resolve each text resource's local page indices to draw-page indices once. The fill loop uses indexed access
  rather than repeated shared-pointer map lookups per glyph. Page order and attribute arithmetic are preserved.
- Return cached shaped runs directly when every requested name is present and the existing reclamation threshold
  is not exceeded. Cache misses and budget reclamation still use the existing preparation path.
- Reserve known annotation record/style/string vector sizes before rebuilding the immutable batch.
- Add `--single` to the normal demo, including its capture, tour, dynamic, and on-demand paths.

All implementation remains private in `osgEarthPrestige`; there is no installed/public ABI change.

## Google Benchmark results

Intel Core Ultra 9 285 (24 cores/24 logical processors), Windows, RelWithDebInfo, dependencies from `D:\vcpkg`.
Segoe UI is the local font. Each measurement uses five repetitions with a 0.2-second minimum per repetition.
Numbers below are median elapsed microseconds; dynamic update uses manual edit-to-publication wall timing,
including scheduling. Other fixtures run synchronously, with setup outside the measured region.

| Operation | Before | After | Ratio |
| --- | ---: | ---: | ---: |
| Prepare 256 accepted labels / 5,888 glyphs, one page | 430.36 us | 105.88 us | 4.06x |
| Prepare 4,096 accepted labels / 94,208 glyphs, one page | 12,249.09 us | 3,722.80 us | 3.29x |
| Prepare 4,096 one-glyph labels, sixteen pages | 688.20 us | 387.13 us | 1.78x |
| Return 64 cached names | 17.32 us | 3.16 us | 5.48x |
| Return 1,024 cached names | 441.69 us | 87.30 us | 5.06x |
| Change one anchor among 1,024 annotations, through worker | 542.44 us | 191.90 us | 2.83x |

Renderer tests force a fade change, retain the prior result as an in-flight frame would, and include retirement
of the replaced buffers. They fix the accepted label/glyph set, bypassing collision selection to isolate rendering
preparation. The 94,208-glyph case is a stress workload, not the displayed density of the global city demo.
Cached-run tests also verify pointer identity of every returned run/page. The dynamic fixture updates one anchor,
retains all 1,024 records, and uses a yielding caller to observe completion; it is not end-to-end display latency.

A frozen pre-change DLL/executable rerun after the optimized measurements returned 12,881.50 us for the dense
renderer case, 351.36 us for 1,024 cached names, and 522.79 us for the dynamic update. This confirms the gains
survive reversing measurement order, while showing why small timing differences should not be overinterpreted.

The unchanged single-camera cull/submission control measured 3.74 -> 3.53 us with one atlas page and
10.13 -> 9.03 us with sixteen pages. No cull optimization is claimed from those differences. The unchanged
collision solver was checked against the independent R-tree reference: accepted identities/order remained exact
for sparse, dense, and clustered 4,096-candidate inputs (3,708 / 890 / 27 accepted respectively).

## Buffer allocation and correctness

Real headless SceneView culls collected every emitted glyph attribute array. All byte fingerprints matched the
baseline for the three renderer workloads above, including page grouping, anchors, bounds, band addresses,
colors, halos, and fade parameters. Fewer reserved bytes did not mean fewer emitted glyphs.

| Workload | Used bytes, unchanged | Before reserved bytes | After reserved bytes |
| --- | ---: | ---: | ---: |
| 256 labels, one page | 894,976 | 1,229,984 | 894,976 |
| 4,096 labels, one page | 14,319,616 | 21,014,760 | 14,319,616 |
| 4,096 labels, sixteen pages | 622,592 | 768,512 | 622,592 |

The dense batch drops from 20.04 to 13.66 MiB of instance-array capacity (31.9% less). This measures actual array
capacity, not total heap allocations or whole-process memory. Arrays now allocate once per attribute/page;
broader heap-allocation tracing and CPU sampling remain future measurement work.

Seventy-three focused tests passed in three consecutive final runs, including existing typography, tile eviction,
dynamic updates, late-result rejection, and independent camera histories/fades. Assertion totals vary with the
number of asynchronous tile publications checked. An initial transient test falsely reported non-idle after
observing idle because it immediately repeated a nonblocking, contention-sensitive probe. Its settle helper now
asserts the observation that ended its wait. Production frame-demand behavior was unchanged. The previously
documented full-suite baseline failures remain outside this focused validation.

## Whole viewer: one camera

The transient viewer uses the normal MapNode, offline `world.tif`, spatially queried `cities.gpkg`, EarthManipulator,
and 720x800 viewport at 1x DPI. It renders the same rank policy before/after; static views accept 32 city labels.
The moving case follows a time-based 15-degree/second longitude sweep, and the dynamic case uses the existing
ten-second annotation cycle. Each run lasts eight seconds; summaries exclude the first two seconds.

Merely setting v-sync false still yielded approximately 60 FPS in the double-buffered Windows window. These CPU/
GPU workload captures therefore use a single front buffer to remove SwapBuffers pacing, with SingleThreaded OSG
traversal and asynchronous label workers. They are not presentation-latency or production-display FPS measurements.
OSG collects update/cull/draw/GPU query times; CSV writing happens after traversal stops. Hardware: RTX 5080,
OpenGL 4.6, NVIDIA driver 610.60.

| Case | Before frame p50 / p95 / p99, ms | After frame p50 / p95 / p99, ms |
| --- | --- | --- |
| Stationary global | 0.3319 / 0.4710 / 1.3444 | 0.3345 / 0.4196 / 0.6603 |
| Moving global, initial pair | 0.3476 / 0.4270 / 0.6300 | 0.3555 / 0.5454 / 1.8327 |
| Moving global, repeated pair | 0.3492 / 0.4327 / 0.6552 | 0.3486 / 0.4379 / 0.6748 |
| Global with dynamic edits | 0.3377 / 0.4194 / 0.6721 | 0.3429 / 0.4330 / 0.6784 |

Whole-viewer medians are approximately unchanged; tail variation does not establish a frame-time improvement.
After-change whole-camera cull p95 is approximately 0.090-0.105 ms, CPU draw p95 0.139-0.173 ms, and GPU draw p95
0.063-0.076 ms. These include terrain/imagery, unlike the label-only cull microbenchmark. Async arrival timing can
change equal-priority history winners during travel; no policy or admission limit was changed. The repeated moving
pair ended at 54 versus 53 accepted labels, so it is not an exact displayed-set comparison.

A separate normally double-buffered, threaded, one-camera global tour completed 30 seconds / 1,787 frames with
205 tile loads, 77 evictions, and no failures. The cache ended at 128 tiles / 1,235,168 accounted bytes. Half-second
process samples peaked at 212.1 MiB working set / 242.2 MiB private bytes. This is a bounded-travel sanity check,
not a paired process-memory improvement or a long-duration leak proof. Do not compare its memory directly with
the instrumented viewer, whose retained frame statistics add overhead.

## Incremental dynamic updates: single camera

Dynamic annotations now occupy stable 64-slot chunks. Editing one label captures/prepares only its chunk; insertion
or erasure does not shift unrelated chunks. Each publication atomically carries the full set of chunk references.
The worker cache commits only complete, budget-valid preparations. Four publication slots and existing generation
checks still provide backpressure and reject superseded work.

Rendering caches the accepted membership and fade descriptors for each dynamic chunk. Unchanged packets retain
their immutable glyph arrays, and free frame slots reuse the corresponding drawable/VAO/uniform clones. Each packet
keeps its own time origin; frame and adoption uniforms are rebased separately, preserving camera-local fades.
Ordinary static/tiled labels continue to merge by atlas page. Collision selection still runs over the complete
candidate set in the background. This is bounded chunk replacement, not mutable GPU subrange writes.

The new transient Google Benchmark moves one label while retaining 64 or 1,024 accepted labels, each containing
23 visible glyphs from `Metropolitan label 012345`, Segoe UI, one atlas page. One view slot is used. Five repetitions
use at least 0.3 seconds each; values below are medians of manual elapsed time. Initial font loading and population
are excluded. The combined measurement includes source `set`, worker scheduling/preparation/publication, construction
of the accepted set, renderer preparation, and retirement of the preceding result. It excludes collision solving,
cull, GPU execution, and presentation latency. A yielding caller observes source completion.

| Sparse move operation | Before, us | After, us |
| --- | ---: | ---: |
| Source publication, 64 labels | 8.73 | 8.62 |
| Source publication, 1,024 labels | 186.68 | 7.90 |
| Source plus glyph preparation, 64 labels | 71.49 | 61.17 |
| Source plus glyph preparation, 1,024 labels | 956.56 | 145.87 |

At 1,024 labels, source publication is 23.6x faster and the combined operation is 6.6x faster (84.8% less time).
The renderer reuses 15 of 16 packets: 960 labels / 22,080 glyphs retain their arrays. Only 64 labels / 1,472 glyphs
are repacked, instead of 23,552 glyphs. Their instance-array payload is 223,744 bytes versus 3,579,904 bytes for a
full rebuild. These are CPU array contents, not measured driver transfer or total allocation counts.

A frozen before DLL/executable repeated after the optimized run measured 191.82 us for source publication and
1,256.36 us for source plus glyph preparation at 1,024 labels, confirming the gain despite scheduling variation.
The repeated 64-label combined baseline was 60.48 us, so no meaningful small-population speedup is claimed.
The initial after run overlapped correctness tests; the reported final after run was performed separately.

Chunking adds draw submissions when many dynamic labels are visible. A separate real headless single-camera
SceneView benchmark uses the same 1,024 labels/resources in either one merged packet or sixteen chunk packets:
stationary cull/submission measures 3.56 versus 9.51 us, respectively, with exactly 1 versus 16 draw submissions.
The increase is about 6 us per frame, independent of edit frequency. This measures CPU cull only, not GPU cost.
The unchanged static/tiled controls remain 3.23 us for one page and 9.30 us for sixteen pages. Whole-frame FPS gains
are not claimed from these preparation benchmarks.

Seventy-five focused tests pass. New transient cases verify stable chunk identity across moves, erase, and vacant-slot
reuse with arbitrary IDs; old snapshots retain their original coordinates/lifetimes. Real SceneView culls prove
15 unchanged array packets survive each sparse move/style edit, and canonical per-glyph hashes of every emitted
attribute match fresh packing after move, text/style change, and deletion. Existing cases continue to cover
camera-independent history/fades, stale completions, coalescing/backpressure, failure recovery, and tile paging.
The cold-packing fixture deliberately renders only current opaque placements; outgoing animation is covered by
the existing motion/dynamic cases. Full-suite baseline failures remain outside this focused gate.

The normal double-buffered, threaded, single-camera global viewer completed two dynamic edit cycles over 22 seconds
(1,308 frames). All 171 submitted generations completed/published; there were no dynamic/tile failures or hard
invalidations. The updated text capture was visually checked. Half-second process samples peaked at 185.4 MiB
working set and 193.6 MiB private bytes. This is a runtime sanity check, not a paired memory/FPS comparison.

Incremental sources, frozen binaries, raw JSON, and validation logs are in `build/label-incremental/`. The local
`build/label-perf` harness also includes these transient targets. From the initialized `tests` shell:

```bat
osgearth_benchmarks --benchmark_filter="SparseMove|ChunkCull|^Cull" ^
  --benchmark_min_time=0.3s --benchmark_repetitions=5
osgearth_tests "[label-incremental],[label-engine],[label-text],[label-declutter],[label-motion],[label-tiles],[label-dynamic]"
osgearth_labels --single --tiled --dynamic --threaded
```

## Frame rendering: analytic fills and cached halos

The original fragment shader evaluates analytic curve coverage nine times for each halo fragment. The selected
implementation retains Slughorn analytic fills and samples a bounded distance field for ordinary-size halos.
All halo draws precede all fill draws, preventing neighboring glyph halos from darkening letter interiors.
The two passes share instance arrays. Static/tiled text still merges by atlas page; incremental chunks retain
independent buffers. No placement, source density, update cadence, or per-camera fade/history policy changed.

The optional FreeType bitmap distance fields are prepared on the font worker at 64 texels/em and a 16-texel
spread, with at most 8 MiB of images/maps per font service inside its 32 MiB atlas cap. Cached halos apply up to
32 physical pixels/em when their complete halo/AA ramp fits the field. Large/wide-halo text, unsupported glyphs,
missing SDF support, and exhausted cache space retain analytic halos. Sealed pages never mutate. The existing
cached FreeType dependency supplies this capability; no Slughorn/dependency upgrade is involved.

A transient Google Benchmark renders actual shaped Segoe UI glyphs through the OSG viewer and GL driver on one
1920x1080 camera, NVIDIA RTX 5080 (610.60), Core Ultra 9 285, RelWithDebInfo. Every timed frame ends with
`glFinish`, so these are completed-frame latency measurements, including CPU/GPU synchronization; they are not
normal pipelined presentation FPS. Setup, font preparation, shader warmup, capture, and teardown are excluded.
The fixture renders the same eight city names, including rotated and translucent text, with a one-pixel halo.
It holds 256 labels at 24px in one merged packet, or 1,024 labels at 16px in sixteen dynamic chunks. Some labels
overlap in this stress fixture; it deliberately bypasses decluttering to compare identical visible glyph work.

| Visible labels | Completed frame before, us | After, us | GPU before, us | After, us |
| --- | ---: | ---: | ---: | ---: |
| 256 | 338.35 | 123.10 | 254.59 | 40.20 |
| 1,024 | 740.88 | 221.75 | 621.72 | 98.08 |

At 1,024 labels, completed-frame latency improves 3.34x and GPU time improves 6.34x.
One after repetition at 256 labels showed a CPU scheduling outlier; GPU timing remained stable (0.18% CV).

Before values are medians of three 0.2-second pilot repetitions with the original single-pass renderer;
after values are five 0.3-second repetitions with the final two-pass renderer. A later control using the old
shader with the new submission path measured 359/776 us and 258/631 us GPU for the two populations, respectively.
That control includes an extra discarded halo draw, so the table conservatively uses the original, lower baseline.
These measurements retain the same shaped glyphs and instance data; they do not hide cost by drawing fewer labels.

The alternatives were measured before choosing the implementation. Skipping redundant coverage inside opaque
fills saved only 1-2% of GPU time. Pure distance-field fills/halos were faster (about 30 us GPU for 1,024 labels)
but changed letter-edge rendering. Analytic fills with cached halos retained the desired letter quality. The
final ordered two-pass version pays additional draw overhead to fix halo/fill overlap. The earlier 128 texels/em
cache delayed initial global tile labels about 2.8 seconds; reducing the cache resolution substantially reduced
that preparation cost. Cold first-label latency remains a tradeoff: the final static global case shows its first labels at 0.691
seconds versus 0.134 seconds before. Cache construction is off cull/draw, but still delays initial publication.
A future lazy cache upgrade should publish analytic text immediately and add cached halos asynchronously.

| Global viewer case | Viewer frame p50 before / after, ms | Scene GPU p50 before / after, ms |
| --- | ---: | ---: |
| static | 0.3379 / 0.3398 | 0.0595 / 0.0205 |
| moving | 0.3511 / 0.3575 | 0.0572 / 0.0203 |
| dynamic | 0.3455 / 0.3613 | 0.0616 / 0.0191 |

The normal global scene uses the repository's 7,322-city tiled source plus world imagery in a single 720x800
camera. The measurement viewer disables vsync and uses the front buffer; it does not insert `glFinish` per frame.
Each eight-second case excludes the first two seconds. The before DLL/viewer is frozen under
`build/label-frame/before-bin`. Static final accepted count is 32; moving/dynamic counts vary with asynchronous
paging and placement. These are whole-scene GPU times, not label-only query times. This small-label workload
remains CPU-bound: the GPU savings do not establish a whole-viewer FPS gain, and extra submissions add CPU work.

Seventy-six focused tests pass (3,306,426 assertions in the final run), covering sealed cache bounds/accounting,
unchanged glyph buffers after sparse updates, independent camera history/fades, tile paging, and the existing
core cases. The fill-only multilingual capture is byte-identical to the original shader when halo alpha is zero.
Visual captures cover 4/12/24/64/128/256px, ligatures, combining marks, Greek/Cyrillic/Arabic glyphs, rotation,
translucency, and 2x density. The final normal double-buffered threaded single-camera viewer ran for 22 seconds:
1,306 frames, 170 dynamic generations completed/published, no tile/dynamic failures or hard invalidations.
Its updated-text capture was inspected. Half-second samples peaked at 248.0 MiB working set and 254.3 MiB private
bytes. This is a runtime sanity check, not a paired memory improvement claim; driver resources and duplicated
pass/frame draw state add memory beyond the explicitly bounded distance images/maps. Temporary test/CMake hooks
were removed after validation, and the prescribed configure/build scripts were run again with D:\vcpkg.

Halos intentionally differ from the old eight-direction dilation. All fills render above all halos; overlapping
translucent glyphs and partial fades therefore composite differently from a single precomposited glyph. Both
passes use the same camera-local fade timeline. A disabled halo no longer tints translucent fill edges.
The no-halo reference explicitly disables both halo width and alpha, avoiding that old edge case.

Raw measurements, shader alternatives, quality images, frozen binaries, and transient sources are in
`build/label-frame/`; the global viewer CSVs/logs are in `build/label-perf/`. Re-enable the local harness as below,
then from the initialized `tests` shell run:

```bat
osgearth_benchmarks --benchmark_filter="RenderFrame/(256|1024)/4/" ^
  --benchmark_min_time=0.3s --benchmark_repetitions=5
osgearth_benchmarks --benchmark_filter=FrameQuality
```

Prototype shader variants 1-3 are archived experiments for the original single-pass geometry; they must be
adapted before rerunning against the current two-pass renderer. Variant 0 is the old shader control, including
the discarded halo pass; variant 4 is the actual current production shader.

## Reproduction

Transient sources, frozen before DLL/executables, raw benchmark JSON, glyph hashes, viewer CSVs, images, build logs,
and memory samples are under `build/label-perf/`. They are intentionally outside the final repository diff.
Restore local measurement targets using `build/label-perf/enable-harness.ps1`, then use the prescribed scripts:

```powershell
./build/label-perf/enable-harness.ps1
$env:VCPKG_ROOT = 'D:\vcpkg'
$env:CMAKE_BUILD_PARALLEL_LEVEL = '1'
./configure.bat
./build.bat
```

From `tests`, initialize the runtime before commands:

```bat
call ..\osgearth_shell.bat
osgearth_benchmarks --benchmark_filter="^(Renderer|CachedRuns|Layout|Cull|Dynamic|Verification)" ^
  --benchmark_min_time=0.2s --benchmark_repetitions=5
osgearth_label_perf --tiled --seconds 8 --output ..\build\label-perf\capture.csv
osgearth_labels --single --tiled --threaded
```

The next performance questions are end-to-end edit-to-display latency under sustained load, transient allocations
during result adoption, shared atlas ownership across tiled/dynamic sources, and reducing chunk draw overhead
without rebuilding unchanged glyphs. The measured improvements here do not require changing placement/history semantics.

## Curved road-label rendering slice

OpenFreeMap now supplies `place` and `transportation_name` to one Prestige layer. Roads retain HarfBuzz shaping
and use Slughorn glyph rendering; screen-space arc-length fitting runs on the placement worker, independently
for each camera. The renderer adds a world tangent to each glyph instance and projects it in the vertex shader.
Roads merge by atlas page into separate packets from points, preserving unchanged point-buffer reuse.

Controlled single-camera frame measurement on the same Core Ultra 9 285 / RTX 5080 / driver 610.60 system:
1920x1080, RelWithDebInfo, front buffer, vsync off, `glFinish` after each frame. Five repetitions, minimum
0.3 seconds each; the table reports medians. Point/path cases use the same names, glyph counts, font sizes,
colors and halos, with slightly different glyph orientations/positions required by the curved paths. Every
case asserts its full requested label population before rendering. The 1,024-label control here uses one
merged packet, unlike the older 16-chunk dynamic benchmark; compare only the paired controls below.

| Labels | Point frame | Road frame | Point GPU | Road GPU | Point cull | Road cull |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 at 24 px | 116.96 us | 120.22 us | 39.82 us | 41.02 us | 5.37 us | 5.45 us |
| 1,024 at 16 px | 179.20 us | 183.89 us | 96.58 us | 101.55 us | 5.52 us | 5.52 us |

These are isolated completed text-frame latencies, not whole-map frame rates or a before/after optimization
claim. Terrain, source I/O, font preparation, layout and instance preparation are outside the frame timing.
There was one 294 us road-frame outlier in the 1,024-label case (other samples 181ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã…â€œ187 us); its frame CV was
24%, while GPU CV was 0.72%. Medians therefore describe typical latency, not a worst-case guarantee.

A separate worker-only benchmark projects/fits 1,024 candidates with ten glyphs and three path vertices each:
point projection median 9.56 us, road fitting 481.24 us wall time (CPU medians 9.77 / 488.28 us). Road samples
ranged approximately 433ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã…â€œ658 us. This includes candidate/path allocation and projection, but excludes collision
solving, shaping, tile loading, and draw-buffer packing. It makes the additional worker cost explicit; that
work is not performed in cull and is not repeated every rendered frame.

The live London neighborhood capture at 2.5 km range loaded 25 label tiles with zero failures and displayed
39 accepted place/road labels. The label tile cache reported 3,619,084 resident bytes. This is cache accounting,
not process memory or a representative global-travel memory measurement. The frame capture confirms street
and bridge names follow their source lines; the basemap geometry renderer is unchanged.

Validation: 85 focused regression cases / 3,312,200 assertions passed, including complete-run fitting, upright
reversal, sharp-bend/short-path rejection, perspective interpolation, DPI, point/road collisions, camera-local
fades, native multipart source preparation, unsupported-glyph isolation, and distinct actual road instance
buffers for two cameras. Temporary harness sources and artifacts remain under `build/label-roads`, including
`frame.json`, `worker.json`, `tests-final.log`, and `london.png`; repository test CMake hooks are removed after
validation. Cross-tile stitching, repeated road placement, shields, and terrain occlusion remain deferred.

## Opt-in smooth GPU path prototype

`<gpu_paths>true</gpu_paths>` inside `prestige:labels` enables the prototype. It defaults to false and requires
OpenGL 4.3 in every drawing context. The baseline retains its existing GL 3.3 path. No Slughorn/dependency
upgrade is involved. Builds use the prescribed scripts and the cached `D:\vcpkg` dependencies.

The background worker still selects paths, rejects unsuitable fits, handles collisions, and maintains each
camera's history and fades. For GPU-enabled paths, its collision footprints use the same interpolated tangent
field as rendering. A compute dispatch per merged road packet projects each source polyline once per frame,
builds a bounded screen arc-length table, and fits each glyph once. Both halo and fill passes fetch that output.
Glyph corners do not search the road separately. Vertex-tangent interpolation removes rotation steps as a glyph
crosses a polyline vertex. Immutable input tables and draw packets survive ordinary pose changes with unchanged
selection/fades; output textures belong to a camera's packet and are allocated independently in each GL context.
Image/texture barriers order reuse without CPU readback, fences, or blocking work in cull.

All timings below are same-run controls, not comparisons against the earlier 0.184 ms historical sample.
Hardware: Core Ultra 9 285, RTX 5080, driver 610.60; Windows RelWithDebInfo. One camera, 1920x1080, vsync off,
front buffer, `glFinish` per measured frame. The figures are medians of five Google Benchmark repetitions:
minimum 0.3 s stationary and 1 s moving. Identical names, glyph counts, font sizes, colors, halos and source
paths are used in each pair. The GPU mode intentionally interpolates tangent direction. Decluttering is bypassed
for this fixed-density stress workload; complete populations are checked before timing, including actual GPU
output readback so shader failure cannot produce an apparently fast empty frame.

| Labels / vertices per road | Current frame | GPU frame | Current GPU | Prototype GPU |
| --- | ---: | ---: | ---: | ---: |
| 256 / 3 | 127.49 us | 140.41 us | 41.58 us | 47.50 us |
| 1,024 / 3 | 194.60 us | 209.58 us | 102.17 us | 107.48 us |
| 1,024 / 17 | 194.34 us | 201.16 us | 100.60 us | 109.17 us |
| 1,024 / 65 | 191.68 us | 212.64 us | 100.57 us | 114.67 us |
| 1,024 / 256 (maximum) | 193.36 us | 229.98 us | 100.64 us | 132.95 us |
| 1,024 / 17, continuously zooming | 187.54 us | 203.43 us | 99.19 us | 107.31 us |

The moving case uses a continuous +/-2% zoom at 1 Hz with normal wall-clock worker publication cadence. It
includes frame-side adoption/submission and concurrent worker activity. Cull medians are 5.50 us for the
current path and 6.36 us for GPU mode; the additional compute leaf adds constant per-packet submission work.
These are text-only completed-frame latencies, not whole-map FPS. The prototype costs about 6.82 us (+3.5%)
stationary and 15.89 us (+8.5%) while zooming in the 17-vertex case. At 256 vertices it costs 36.62 us (+18.9%).
Some stationary CPU/frame samples had outliers (the 1,024/3 GPU-frame CV was 16%, for example); GPU-time CVs
were under 1% in the reported cases. Treat medians as typical latency, not a worst-case guarantee.

A separate 90-frame, 60 Hz rapid +/-35% zoom cycle measures actual displayed glyph anchors against a fresh
worker fit. The current path reached 13.354 px maximum tracking error (4.408 px average of frame maxima).
GPU mode reached 0.0001322 px maximum (0.0000879 px average). Captures show fixed-size text following the curve
continuously; this is a controlled rapid zoom, not a claim that ordinary map navigation always has 13 px error.
The side-by-side recording is `build/label-gpu-path/zoom-comparison.gif`.

Functional validation:

- 85 focused regression cases passed, including CPU and GPU-enabled tiled roads and camera-local cull buffers.
- 43,520 GPU glyph checks across zoom, orthographic/perspective projection, varying path depth, slight roll,
  1x/2x DPI, and a large world-coordinate origin agreed with the double-precision worker fit within 0.00025 px.
  GPU-rendered footprint bounds also agreed within 0.00025 px.
- Two cameras sharing one engine and GL context retained independent output: changing/drawing the second
  camera left the first camera's output byte-for-byte unchanged.
- The live London GPU capture loaded 25 label tiles with zero failures and accepted 39 labels, matching the
  prior control capture. The label tile cache reported 3,631,780 bytes. That excludes renderer/GPU tables and
  is not process memory. Point labels and road labels were visually checked together.

Input/output texel storage is bounded per packet: 262,144 source vertices, 131,072 glyphs, and 65,535 roads.
The tables use roughly 12.4 MiB at these caps, retained on CPU for context recreation plus a context-local GPU
copy, before driver overhead and older pinned publications. The 1,024-label/17-vertex benchmark uses 832 KiB
of table texels per CPU/GPU copy; the 256-vertex stress case uses 8,480 KiB. Worker collision fitting remains
necessary; no claim is made that total worker CPU has been eliminated. Ordinary camera changes reuse tables.

The initial prototype was opt-in. It fixes zoom stepping at a small absolute but measurable rendering cost.
The accepted policy now enables it automatically when supported; see the follow-up below.
Long paths cost more; road selection, source tile transitions, upright-direction flips, horizon/length rejection,
and collision-driven fades can still cause legitimate discrete changes. Cross-tile stitching remains deferred.
A further optimization would need paired frame numbers and the same readback/motion checks.

Reproduction artifacts remain transient under `build/label-gpu-path`: `GpuPathBench.cpp`, `frame.json`,
`motion.json`, `quality.log`, `zoom.log`, `zoom-5.csv`, `zoom-6.csv`, `tests.log`, `london-gpu.png`, and the GIF.
The initial failed shader pilot is excluded from all reported measurements. Test/benchmark CMake hooks are
removed from repository sources after validation; the current prototype and documentation remain in Prestige.


## Automatic GPU path selection and halo exploration

GPU path following now defaults on when osgEarth's common layer-open capability probe reports GLSL 4.30.
Unsupported systems automatically retain CPU fitting, including when configuration explicitly requests true.
An explicit `<gpu_paths>false</gpu_paths>` preserves the CPU control. The demo now uses automatic selection.
Applications creating older contexts on newer hardware must force false: this policy does not negotiate
different GL versions per camera. This change selects the previously measured renderer; its algorithms and
cost are unchanged.

Transient native-road integration checks inspect actual glyph draw state for omitted, false, and true settings.
All three select correctly on the RTX 5080. Repeating them with osgEarth's headless capability probe exercises
its GLSL 3.30 fallback and all three produce CPU glyph state without a layer-open error. This is a simulated
capability test, not a physical old-GPU run.

The halo exploration keeps the current production shader and compares it with a transient soft distance ramp.
Full implementation findings and next steps are in [label-engine-halos.md](label-engine-halos.md).
Actual GL captures are in `build/label-halos/crisp-vs-soft.png`. The soft variant is not installed as a style option.

Single-camera, 1,024 labels at 16 px, 2 px halo, 1920 x 1080, identical shaped population and two passes:

| Halo | Completed frame median | GPU median | Cull median |
| --- | ---: | ---: | ---: |
| Current crisp distance threshold | 0.269 ms | 0.1473 ms | 0.0061 ms |
| Prototype soft distance ramp | 0.241 ms | 0.1478 ms | 0.0057 ms |

Google Benchmark, five repetitions of at least 0.3 seconds, RelWithDebInfo, Core Ultra 9 285 / RTX 5080,
NVIDIA 610.60; vsync off and `glFinish` per measured frame. Shader/font/cache setup and captures are outside
timing. No terrain or tile loading is included. The fixture verifies equal accepted label counts and bypasses
decluttering to hold density constant. This text set and halo width differ from the earlier road benchmark;
compare only the paired rows here.

Completed-frame variation was 7.3% for crisp and 19.7% for soft, including one soft run at 0.353 ms.
The lower soft frame median is not evidence of a speedup. GPU medians differ by only 0.0005 ms; this narrow
softening experiment adds no meaningful observed GPU cost. Wider blur radii, contour strokes, and MSDF have
not been benchmarked here. Neither halo shader changes the analytic fill or its geometry.

Sources, screenshots, raw `frame.json`, logs, and the comparison compositor remain transient under
`build/label-halos`; the fixture extension is in `build/label-frame/FrameBench.cpp`. Test/benchmark CMake hooks
are removed after validation.

Validation: all 58 label test cases passed (3,313,817 assertions), plus the separate simulated-GLSL-3.30
selection check (12 assertions). Captured opaque fill pixels remained unchanged in all five visual cases.
The unrestricted suite was not clean: two Chonk assertions failed, and both reproduce with the saved
pre-change executable (`prior-chonk.log`). They are outside the changed label code; no full-suite pass is claimed.

## TextSymbol halo integration

Prestige now reads existing CSS/structured TextSymbol controls for constant size, fill, halo color,
normalized halo offset, and outline/none backdrops. The demo uses this symbology throughout. A separate
Prestige-only `halo_softness` rule attribute feathers inward across 0-1 of the halo radius. Default zero
keeps crisp rendering. Places in the demo use 0.5; roads remain crisp. No legacy text nodes are involved.

The soft halo shader is specialized separately; all fills and crisp-only packets retain the lean shader.
This matters: an initial universal shader added about 8 microseconds of GPU time to the crisp control.
The specialized version restores its cost and produces a pixel-identical framebuffer.

The same pre-change and current 1,024-label/16 px fixture, 1 px halo, dynamic chunks, single camera:

| Renderer | Completed frame median | GPU median |
| --- | ---: | ---: |
| Pre-change crisp | 0.233 ms | 0.09970 ms |
| Current crisp, specialized | 0.226 ms | 0.09995 ms |

Frame variation was 5.1% and 2.4%; interpret this as unchanged cost, not a speedup. All framebuffer pixels match.
The following separate fixture holds 1,024 longer labels at 16 px with a 2 px halo and merged static batches:

| Halo mode | Completed frame median | GPU median |
| --- | ---: | ---: |
| Cached crisp | 0.239 ms | 0.1495 ms |
| Cached fully feathered | 0.240 ms | 0.1517 ms |
| Forced cache miss, crisp | 1.162 ms | 1.0591 ms |
| Forced cache miss, fully feathered | 3.944 ms | 3.7967 ms |

Same Core Ultra 9 285 / RTX 5080, driver 610.60, RelWithDebInfo, 1920 x 1080, vsync off, Google Benchmark,
five repetitions of at least 0.3 seconds and `glFinish` each frame. Warm setup and captures are excluded.
Equal accepted populations are verified. No terrain or source/network work is included.
Forced cache misses clear the fixture's distance images while retaining the same analytic glyphs and styles.
They are an intentional stress case, not the normal cached 16 px path.

The cache-miss result is a material limit: soft analytic fallback uses 32 shifted samples versus eight for
crisp fallback. Large text above 32 displayed pixels/em, cache exhaustion, or unavailable SDF baking can
select that path. Softness remains opt-in; this work does not claim inexpensive soft halos in those cases.
Higher-DPI/large-text cache quality and a cheaper fallback are follow-up performance work.

Validation: 61 label tests passed (3,310,399 assertions), including CSS/structured symbol equivalence,
alias/opacity handling, precedence, disabled halos, invalid values, unchanged outer footprints, camera-local
history/fades, and actual road draw-buffer styling. GPU framebuffer checks cover cached rendering, forced
cache misses, 2x density, and 64 px analytic fallback. All opaque fill pixels stay unchanged; both cached modes
match the prior prototype exactly. The standard viewer loaded the updated live earth file with no layer error
and automatic GPU paths enabled. Place and neighborhood-road captures are saved with the artifacts.

Transient reproduction: `build/label-halo-style` contains before/specialized/halos JSON and logs, StyleTests.cpp,
pixel checks, the production contact sheet, and viewer captures. `build/label-frame/FrameBench.cpp` extends the
existing actual-GL fixture; `build/label-halos/DefaultTests.cpp` checks resolved styling in actual cull leaves.
Early runs with the soft shader define accidentally inside its license comment were rejected by pixel checks
and are excluded from the halo timings above. Test/benchmark CMake hooks are removed after validation.


## Explicit pixel halo width and corrected SDF scale

Added Prestige `text-halo-width` in constant logical pixels, with explicit-width precedence over the
normalized legacy offset. The demo uses 2px white halos, crisp places and 0.5-soft roads. Parsing happens
on layer open, with no additional frame work or public TextSymbol ABI change.

A calibration error doubled the decoded cached distance: FreeType spread 16 at 64 pixels/em requires
`(byte - 128) / 512`, not `/256`. The old cached 2px style therefore drew approximately a 1px radius.
Corrected the conversion without enlarging the cache, adding samples, changing geometry, or changing
analytic fills. The wider, correctly antialiased border improves separation around place names.

Paired single-camera frame measurements, same 1,024 accepted labels at 16px and requested 2px halos,
1920x1080, Core Ultra 9 285 / RTX 5080, NVIDIA 610.60, RelWithDebInfo, vsync off, five repetitions with
0.3s minimum measured time and `glFinish` per frame. Setup, font baking, and captures are outside timing.
The fixture uses the same London/Westminster and multilingual/rotated samples in both runs; it includes
no terrain or network work. Soft mode uses softness 1 to exercise the full feather range.

| Mode | Before frame median | Corrected frame median | Before GPU | Corrected GPU |
| --- | ---: | ---: | ---: | ---: |
| Cached crisp | 0.2235 ms | 0.2312 ms | 0.13236 ms | 0.13290 ms |
| Cached soft | 0.2493 ms | 0.2302 ms | 0.13442 ms | 0.13544 ms |

GPU changes are +0.00054 ms (0.4%) and +0.00102 ms (0.8%): effectively unchanged frame-rendering cost
for this workload. Completed-frame variation is 2.6-18.3% across these groups; do not interpret the soft
frame-time decrease as a speedup. This fixes requested visual behavior, not an optimization that reduces
work. The 8 MiB distance cache, atlas data, draw count, and texture-read count are unchanged.

Framebuffer validation at 22px: cached/analytic-reference mean RGB disagreement over 14,613 active pixels
falls from 29.08 to 5.73 byte levels (80% lower). The analytic eight-direction dilation is an approximation,
not an exact vector-stroke oracle. The London L's outer half-coverage edge moves outward exactly 1 pixel.
All 1,528 sampled fully opaque fill pixels are unchanged. Forced analytic fallback is pixel-identical
before/after; at 2x DPI (44 displayed pixels/em) the cached fixture correctly uses that same fallback and
is also pixel-identical. These checks cover actual rendered buffers, not just style values. Increasing
atlas resolution was unnecessary for this correction; larger text and analytic fallback costs remain as
documented above.

Reproduction artifacts remain transient under `build/label-halo-width`: before/after DLLs and captures,
benchmark JSON/logs, `pixels.json`, and an enlarged London comparison. The transient FrameBench.cpp adds
22px cached/reference and 2x-DPI cases; StyleTests.cpp covers explicit pixel precedence, font independence,
structured config, disable behavior, invalid inputs, and actual demo XML. DefaultTests.cpp checks that
explicit width reaches real road glyph draw buffers. Build hooks are removed after verification.


Final validation: the normal build passed after removing transient test/benchmark CMake hooks. Seven targeted
style/actual-draw-buffer tests passed (144 assertions). The live London earth file opened with no layer errors,
56 label tiles and zero tile failures, and 187 accepted labels; the framebuffer capture was inspected.
The interactive osgearth_imgui demo was reopened with the same updated earth file.


## Dark fade correction: inherited blend state

VisibleLayer's straight-alpha BlendFunc uses OVERRIDE, which overrode the renderer's premultiplied
BlendFunc. The shader already multiplies RGB by alpha, so this darkened fades and AA edges by applying
alpha a second time. Prestige now protects its GL_ONE / GL_ONE_MINUS_SRC_ALPHA blend state and blend
enable mode. The fix changes no shader instructions, samples, atlas memory, geometry, or draw passes.
The generic VisibleLayer implementation and each camera's independent fades/history are unchanged.

Offscreen-only measurements: 1,024 accepted labels at 16px, requested 2px halos, 50% label fade under
an actual VisibleLayer StateSet, layer opacity 1; one camera, 1920x1080 WGL pbuffer, RTX 5080 / NVIDIA
610.60 / Core Ultra 9 285, RelWithDebInfo. Five repetitions of at least 0.3s, glFinish per frame, warm
setup excluded. Soft mode uses softness 1. The same harness loads saved pre-fix and corrected DLLs.
Capability probing uses OSGEARTH_HEADLESS=1; actual rendering still uses the hardware pbuffer. No viewer
or visible test window is launched. These offscreen timings are comparable within this table, not to
previous window/presentation timings.

| Halo | Before frame median | Corrected frame median | Before GPU median | Corrected GPU median |
| --- | ---: | ---: | ---: | ---: |
| Crisp | 0.16705 ms | 0.16670 ms | 0.12859 ms | 0.12864 ms |
| Soft | 0.16949 ms | 0.16923 ms | 0.13112 ms | 0.13113 ms |

Frame/GPU cost is unchanged at this measurement's precision; no speedup is claimed. Offscreen pixel
checks pass for 17 fade/layer-opacity cases (34 frames per build), including 0/25/50/75/100% fade, 50%
layer opacity, crisp/soft cached halos, and forced analytic fallback. Corrected inherited-state images
match the standalone premultiplied reference exactly, and standalone before/after images match exactly.
Zero-opacity images contain only background. A white London halo at half fade changes from incorrect
RGB (147,155,144) to the expected (210,218,204), over background (166,184,166).

Twenty-three motion, dynamic-update, style and draw-buffer tests pass (378 assertions), including
per-camera fade/history and reversal behavior. Artifacts in build/label-fade include FadeBench.cpp,
verify_pixels.py, captures, pixels.json, before/after benchmark JSON/logs, and the comparison image.
The initial zero-opacity fixture failed to settle because its synthetic constant-zero fade was still
marked as an active incoming animation; only the fixture was corrected to mark sampled frames settled.
Failed exploratory runs are excluded. Transient CMake hooks are removed after validation.


## Initial point callouts: bounded worker alternatives and batched leaders

The pre-callout work is checkpointed as `f619c60bb` on `codex/text-engine-proposal`. Callouts are opt-in
with `placement="callout"` on a Prestige point style. The OpenFreeMap demo enables five settlement
rules; country/state labels and roads retain their previous modes. The fixed first-slice styling is a
12 logical-pixel gap and a one-logical-pixel leader using 70% of text-fill alpha.

The worker tries the last camera-local alternative first, then center and eight compass alternatives.
It preserves strict priority, duplicate suppression, bounded grid storage and the shared exact-comparison
budget. Explicit dynamic lifetimes retain alternative preference across revisions; other records use
identity/revision. Text rectangles, including halo padding, must fit the viewport for callouts. Leader
segments do not participate in collisions yet. Slot changes are immediate; no iterative refinement,
crossing optimization, animated relocation, icon/background footprints, or picking is included.

Offsets are baked into immutable glyph instance bounds on the worker, compensating for text rotation;
the existing text shader is unchanged. Leaders share the label's world anchor and fade descriptor,
project on the GPU with this frame's camera, and use a small analytic antialiased capsule shader.
All leaders in a merged point packet use one extra instanced draw. Dynamic chunks retain their existing
packet granularity. No per-label scene nodes, frame-time CPU projection, or cull collision work is added.
Arrays remain bounded by existing selection/presentation limits. Displacement participates in packet-cache
identity, and outgoing fades retain their displacement independently for each camera.

### Single-camera rendering measurements

Same hardware and pbuffer methodology as the fade correction above: 1920x1080, RTX 5080, 1,024 labels,
16px text, 2px crisp/soft halos, 50% label fade, inherited VisibleLayer blend state, warmed shaders,
`glFinish` per completed frame, five repetitions of at least 0.3 seconds. These are text-only offscreen
throughput measurements, not whole-map interactive frame rates. Font construction and placement are
outside timing. The saved pre-callout benchmark executable and its matching DLL provide the before control.

| Existing path | Before frame median | After frame median | Before GPU median | After GPU median |
| --- | ---: | ---: | ---: | ---: |
| Crisp point labels | 0.17288 ms | 0.16933 ms | 0.12935 ms | 0.12894 ms |
| Soft point labels | 0.17238 ms | 0.17473 ms | 0.13151 ms | 0.13114 ms |

Ordinary rendering remains unchanged within measurement variation. A separate matched pair places the
same 1,024 labels inside a common inset viewport region so displaced text stays on screen. Both use
crisp halos and identical shaped runs; the enabled case displaces every label to its east alternative.
The harness asserts two draws for the control and exactly three with all 1,024 leaders.

| Matched inset population | Completed-frame median | GPU median | Draws |
| --- | ---: | ---: | ---: |
| Fixed placement | 0.18176 ms | 0.12871 ms | 2 |
| Displaced text plus 1,024 leaders | 0.17504 ms | 0.12990 ms | 3 |

The GPU difference is about 1.2 microseconds. Completed-frame variation exceeds this difference; the
lower enabled frame median is not evidence of a speedup. The important structural result is one added
draw for the entire population, with no recurring placement work on the rendering thread.

### Worker cost and correctness

A separate 1,024-candidate dense uniform grid measures cold background solves, independent of drawing:
fixed placement takes 0.11821 ms (512 accepted, 2,320 exact comparisons); callouts take 0.20491 ms
(480 accepted, 14,519 comparisons). Accepted counts differ because callouts require full viewport fit
and use greedy alternatives, whereas fixed labels allow clipped edge text. This slice does not promise
maximum label density; the comparison reports the additional worker cost, not a rendering speedup.
The tiled-layer collision fixture does demonstrate two accepted labels with callouts versus one with
fixed placement in each independent camera. More sophisticated density/refinement policies remain open.

The prescribed build succeeds using D:\vcpkg. Seventy-two distinct focused tests pass across the main
regression run and final callout/road/style run. Coverage includes existing exhaustive solver equivalence,
priority, bounded/canceled work, viewport fit, DPR scaling, per-camera slot retention and fades, outgoing
displacements, invalid gaps, cache invalidation/reuse, and native tiled-layer leader batching in two cameras.
Actual GL captures cover rotated text and 0/50/100% fades under inherited layer blending. All eight sampled
leaders blend linearly within 0.5 of an 8-bit color value; zero fade is exactly the background. Captures
were inspected, and shader compilation reported no errors. No Slughorn shaping changes are involved.

Transient artifacts are under `build/label-callouts`: `CalloutTests.cpp`, `CalloutBench.cpp`, before/after
JSON and logs, `verify-fades.py`, framebuffer captures, and matched executables/DLL in `after/`.
The native-layer test extends the existing transient `build/label-earth/EarthTests.cpp`. Temporary CMake
hooks were removed after verification; no new permanent test/benchmark sources or targets are added.

## Public LabelNode / shared tiled placement (2026-11-09)

Measured on the RTX 5080, Windows RelWithDebInfo, cached D:\vcpkg dependencies. Google Benchmark uses
one normal single-threaded OSG viewer with an offscreen GL 4.6 context, 1920x1080, vsync off and glFinish
inside each timed frame. Initial loading/shaping/placement and shader compilation occur before timing.
These are isolated label frames with synthetic native XYZ data, not end-to-end OpenFreeMap terrain timings.

The public API fixture always submits exactly 1,024 labels / 5,120 fill glyphs. It replaces part of the
XYZ population with LabelNodes at the same positions and with identical size/halo/text. It checks actual
render-leaf instance counts before timing and reports them afterward. Medians of three repetitions:

| Input composition | Completed frame | GPU draw |
| --- | ---: | ---: |
| 1,024 tiled labels | 0.0922 ms | 0.0468 ms |
| 896 tiled + 128 LabelNodes | 0.1295 ms | 0.0489 ms |
| 1,024 LabelNodes | 0.2726 ms | 0.0476 ms |

The 128-node mixture adds about 0.0373 ms of completed-frame work; GPU cost remains close. Individual
scene traversal and per-chunk submissions account for most of the additional cost. This does not change
the recommendation to use multilevel tiled sources for large datasets.

The existing 1,024-label halo frame benchmark also supplies a before/after regression baseline (three
repetitions each, same geometry, half-fade state and inherited layer blending):

| Existing renderer case | Before | After | Before / after GPU |
| --- | ---: | ---: | ---: |
| Crisp halo | 0.1718 ms | 0.1674 ms | 0.1294 / 0.1290 ms |
| Soft halo | 0.1749 ms | 0.1747 ms | 0.1316 / 0.1317 ms |

These differences are treated as run variation, not an algorithmic frame-speed improvement. No shader
or halo change is part of LabelNode.

A separate admission microbenchmark compares an unchanged node visit with unconditional resubmission
of the identical dynamic label: 15.84 ns versus 88.82 ns median. The fast path preserves the source's
submission revision; integration tests verify unchanged chunks/resources and correct visual population.
This measures admission only, not shaping, scene traversal or rendering.

Artifacts: `build/label-node/NodeTests.cpp`, `NodeBench.cpp`, `before.json`, `after.json`, and
`nodes-initial.json`. Temporary build hooks are removed from tracked CMake files after validation.
The `SceneAdmission` comparison is a deliberately unconditional-update reference, not a previous public
LabelNode implementation (there was none).

After the continuous-motion adoption fix and control-thread snapshot review, the mixed 896+128 case was
rerun against the final implementation: 0.1245 ms completed-frame median, 0.0489 ms GPU, all 5,120 glyphs
present (`nodes-final.json`). The small change from 0.1295 ms is measurement variation.

## Runtime layer-style updates (2026-11-09)

RelWithDebInfo, cached D:\vcpkg dependencies, RTX 5080, one camera, 1920x1080 offscreen GL 4.6 context,
VSync off, normal public LabelLayer/XYZ source. Google Benchmark, three repetitions, median wall time.
Completed-frame measurements include `glFinish`; GPU timer values are reported separately. Terrain and
ImGui are excluded so the numbers isolate labels. Each frame test verifies 1,024 labels / 5,120 glyphs.

| Single-camera case | Completed frame | GPU |
| --- | ---: | ---: |
| Before runtime-style support | 89.87 us | 47.21 us |
| After, unchanged styles | 88.93 us | 47.12 us |
| After, color update every ten frames | 109.69 us | 47.25 us |

The stationary difference is measurement variation, not a claimed frame-rendering speedup. The stress
case includes style Config creation/validation/submission in frame timing and deliberately submits much
faster than the editor's 150 ms throttle. Rendering retains the same glyph population and GPU cost.

A separate worker microbenchmark changes one of two styles in a 256-label batch. The rebuild reference
repeats the old preparation path with already cached font runs and no source I/O, providing a conservative
comparison. Both implementations are checked for identical point/path bounds, glyph distances and IDs
before timing. Paint changes reuse paths and footprints; size/width changes recompute only affected ones.

| Batch/edit | Cached-font full rebuild | Runtime restyle | Speedup |
| --- | ---: | ---: | ---: |
| Points, paint | 21.39 us | 5.59 us | 3.8x |
| Points, size/width | 20.56 us | 7.71 us | 2.7x |
| Roads, paint | 61.86 us | 20.16 us | 3.1x |
| Roads, size/width | 61.34 us | 37.24 us | 1.6x |

These are preparation costs, not whole-map frame timings. Shaped runs and atlases remain shared;
unchanged-rule batches retain pointer identity. Tile-worker tests confirm no new feature queries during
restyling and latest-value publication to two cameras. Tests also cover preserved per-camera collision
winners and mid-fade opacity, invalid edits, source-less/closed layer handling, XML round trips, mixed
LabelNode/tile visibility, reopening and backend-disabled public linkage.

Transient harnesses and raw measurements: `build/label-style-editor/StyleTests.cpp`, `StyleBench.cpp`,
`before.json`, `after.json`, and verification logs. Build hooks are removed after validation.


### Runtime level bounds and declutter padding

The editor now also changes rule Min/Max level and layer-wide collision padding. Level edits lazily
requery only cached tile levels whose rule eligibility changes; unchanged levels keep their batches.
Failed refreshes retain old coverage and back off. Padding changes request a new worker solve and
preserve the camera's acknowledged result; cull adds one atomic refresh-flag read, with no extra glyph work.

The same three-repetition, single-camera 1,024-label benchmark measured 93.68 us completed frames and
47.18 us GPU at rest, versus 88.93 us / 47.12 us in the preceding build. Color-edit stress measured
113.45 us / 47.34 us versus 109.69 us / 47.25 us. The few-microsecond CPU-side difference is within the
spread observed in the original baseline; these measurements do not establish a speedup. All 5,120
glyphs remained present. Level/padding correctness was exercised separately, not timed as color edits.

46 focused cases (873 assertions) passed, including two-camera padding, restored filtered features,
first-matching fallback rules, unaffected-LOD query counts, retained coverage on failed reload, range
validation and Config serialization. Backend-disabled linkage also passed. Artifacts are under
`build/label-controls`; test/benchmark build hooks are removed from tracked CMake files.
