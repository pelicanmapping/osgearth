# Label engine build setup and baseline

November 8, 2026. Engine source revision: `fc8b074f0bddee9e2f21c0556bdaf8bd08caff3c`.
Worktree branch: `codex/text-engine-proposal`.

The isolated build works and matches master's cached dependencies. The legacy renderer has a usable stationary
one-camera and two-camera baseline, and the production declutter implementation has measured microbenchmarks.
Global route testing also exposes substantial resident-memory growth. The two-camera route does not complete
within the imposed memory budget, so Stage 0's reliable global replay and bounded-memory gate remains open.
No new label engine or performance optimization has been implemented.

See the [design](label-engine-design.md) and [implementation plan](label-engine-plan.md).

## Build setup

The worktree uses `D:\vcpkg`, with its own `build`, `install`, and `build/vcpkg_installed` directories.
It does not share master's generated build products or installed-dependency directory.

- `configure.bat` honors an explicitly supplied VCPKG_ROOT and validates its toolchain before configuring.
  The existing repo-local clone fallback remains available when no root is supplied.
- `bootstrap-vcpkg.bat` preserves the CMake cache on repeat configuration, corrects manifest-path quoting,
  quotes build-directory creation, and propagates CMake failures to its caller.
- `build.bat` passes CMAKE_BUILD_PARALLEL_LEVEL explicitly when supplied. Bare `--parallel` selected native
  default concurrency rather than the intended four-job limit. A four-job build succeeds; earlier unrestricted
  incremental invocations also encountered intermittent access-denied errors launching vcpkg app-local steps.
- `osgearth_shell.bat` already points at worktree-local install and dependency paths and did not need changes.

From the worktree root in PowerShell:

```powershell
$env:VCPKG_ROOT = 'D:\vcpkg'
$env:CMAKE_BUILD_PARALLEL_LEVEL = '4'
.\configure.bat
.\build.bat
```

The first configure restored **76 packages from the binary cache in 2.7 seconds**, with package installation
reported complete in 3.2 seconds. All 126 installed package/feature entries match master in versions, port
versions, and available ABIs. No dependency rebuild was required. Master's installed-package status SHA256 was
unchanged before and after the work. No worktree-local vcpkg clone was created.

Relevant configuration:

| Setting | Value |
| --- | --- |
| vcpkg checkout | `8e8dfb4ba483886936ded5ca201b500b8d8b0096` |
| Triplet | `x64-windows-release`, repository overlay, OSG GL3 |
| Generator/configuration | Visual Studio 17 2022, x64, RelWithDebInfo |
| Tests and benchmarks | Enabled |
| Slughorn | Enabled; cached port `0.0.3-1b203c1` |
| CPU | Intel Core Ultra 9 285, 24 logical processors |
| System memory | 68,403,589,120 bytes |
| GPU | NVIDIA GeForce RTX 5080 |
| GL | 4.6 core, NVIDIA 610.60 |

Negative setup checks return nonzero for an invalid VCPKG_ROOT and for an invalid CMake generator.
Repeat configuration succeeds without deleting the CMake cache.

## Workloads and measurement

All transient sources, fixtures, binaries, raw logs, CSVs, JSON, and screenshots are retained locally under
`build/label-baseline`. They are intentionally excluded from the repository. The temporary benchmark CMake
inclusion is removed from the final source diff. The reproduction instructions below re-enable it when needed.

The frame harness uses the existing TiledFeatureModel and PlaceNode path with an otherwise empty, untextured
globe. It reads `data/cities.gpkg` and a deterministic 64 by 64 grid of 4,096 NYC points. Grid coordinates are
longitude `-74.12 + column * 0.004` and latitude `40.60 + row * 0.004`. Names are `Site 0000` through `Site 4095`;
priority is `4096 - id`. Text is 18 pixels, centered, with a dark halo. City text is white and grid text yellow.
Fade-in, fade-out, and minimum rejected alpha are zero. City tiles extend to level 8; grid tiles use levels 8 to 12.

The font is a frozen local copy of Arial. The dataset SHA256 is
`99F79E8879A9DCE9B98840678C1F97C03C557717C31B5EC05E48CF6AEE748DEA`; the font SHA256 is
`B3658EADAE55E682B5F69EB64C439C1ECC8F196C0BB8D4756D145D13BC86476A`.
Additional hashes are in the local artifact manifest. There are no network imagery or elevation requests.

Each view renders at 1280 by 720, device-pixel ratio 1, in a pbuffer with vsync disabled. Two views share the same
map and graphics context but have independent cameras and viewports. The viewer uses SingleThreaded rendering;
the map's background paging remains active. This is not a benchmark of concurrent OSG cull/draw threading or
multiple graphics contexts.

Stationary cameras look toward NYC at latitude 40.72, range 25,000 m, pitch -65 degrees. View 0 uses longitude
-74.0 and heading 0; view 1 uses longitude -73.95 and heading 35. The capture has 480 frames. Reported stationary
percentiles use the last 120 frames, after submitted and accepted label counts have stopped changing. The
one-camera count-stable suffix was 236 frames; the two-camera suffix was 341 frames. This verifies stable label
counts, not that every background queue is empty.

The route has four stops, with 60 frames between stops: `(lon, lat, range)` equals `(0, 25, 16000000)`,
`(10, 48, 2200000)`, `(-74, 40.72, 25000)`, and `(-74, 40.72, 800000)`. Longitude/latitude interpolate linearly,
range logarithmically, with heading 0 and pitch -90. The second view is 120 frames ahead. A full run is three
240-frame loops, advancing simulation time by 1/60 second per frame without pacing wall time. Therefore these
captures deliberately stress paging during movement; they are not a real-time flight with guaranteed residency.

Total frame time is wall time around `viewer.frame`. OSG statistics supply update, cull, draw, and asynchronous
GPU times. A transient sort-callback wrapper times the unchanged production declutter callback and counts
submitted leaves. Layout time is included in cull time; do not add the two together. Accepted counts refer to
render leaves, not glyphs or an instrumented count of GL draw calls. Missing statistics are NaN, not zero.
Screenshot reads and CSV summarization happen outside measured frames. Instrumentation overhead is included
in the frame result; no overhead subtraction is attempted.

## Stationary baseline

All times below are milliseconds. These are baseline observations, not before/after improvement claims.

| Workload | Cameras | Frame p50 | Frame p95 | Frame p99 |
| --- | ---: | ---: | ---: | ---: |
| Empty terrain control | 1 | 0.163 | 0.209 | 0.240 |
| Empty terrain control | 2 | 0.311 | 0.367 | 0.526 |
| Dense tiled labels | 1 | 25.090 | 26.581 | 28.120 |
| Dense tiled labels | 2 | 52.468 | 54.310 | 55.592 |

| Dense workload/view | Submitted | Accepted | Cull p50 | Layout p50 | Draw p50 | GPU p50 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| One camera, view 0 | 2,169 | 326 | 22.526 | 2.392 | 1.784 | 0.617 |
| Two cameras, view 0 | 2,169 | 326 | 23.254 | 2.456 | 2.587 | 1.851 |
| Two cameras, view 1 | 2,003 | 317 | 23.028 | 2.348 | 2.541 | 1.266 |

Screenshots confirm visible labels and different camera placements. The dense fixture is synthetic and is not a
typography-quality reference. The timings indicate that replacing collision search alone would leave substantial
cull overhead. They support evaluating batched submission alongside background placement; they do not isolate
which remaining cull operations account for that time.

## Declutter microbenchmarks

The transient Google Benchmark fixture directly instantiates the current DeclutterImplementation. It supplies
unique drawable parents, production-style StateGraph leaves, a 1920 by 1080 projection, and a valid RTT-camera
attachment. Fixtures are prepared outside the timed region. The measured operation includes projection, bounds,
overlap checks, replacement matrices, and visibility transitions. Sorting is disabled to isolate this work.

Each rectangle is 8 by 4 pixels, placed on a 16 by 12 pixel grid with 120 columns. The separated case accepts
every rectangle. The clustered case puts four identical rectangles in each cell and accepts the first identity.
Expected accepted IDs are checked before and after repeated solves; all six cases pass. These small rectangles
stress solver scaling and are not a model of actual text density or glyph rendering cost.

Results are median manual wall times across five repetitions, with a 0.2-second minimum measured time per case.

| Candidates | Separated accepted | Separated time | Clustered accepted | Clustered time |
| ---: | ---: | ---: | ---: | ---: |
| 1,000 | 1,000 | 0.330 ms | 250 | 0.346 ms |
| 4,000 | 4,000 | 4.37 ms | 1,000 | 2.85 ms |
| 10,000 | 10,000 | 31.8 ms | 2,500 | 12.9 ms |

Use these same identities and overlap rules when comparing an optimized equivalent solver. A deliberately new
placement policy needs separate behavioral expectations. Preserve visible density when comparing full frames.

## Route captures and memory limits

The initial fixture exposed all city names at every eligible tile level. During its first single-camera global
stress replay, a host sample recorded 47,358,627,840 resident bytes and 48,564,854,784 peak resident bytes. The run
was stopped before completion to release memory. Its log and fixture are preserved as a failed stress case;
there is no complete route CSV from that run.

A second fixture applies the sample's scale-dependent city selection: rank at least 12 at coarse scales, rank
at least 8 from level 6, and other cities from level 8. The dense grid remains unchanged. This is a different
display workload, not an engine optimization. The harness now samples resident memory outside the timed frame
and aborts if it exceeds 12,288 MiB, preserving completed rows.

| Route workload | Result | Frame p50/p95/p99 | Resident-memory observation |
| --- | --- | --- | --- |
| Empty terrain, one camera | 720 frames completed | 0.240 / 0.334 / 0.492 ms | Not instrumented |
| Empty terrain, two cameras | 720 frames completed | 0.455 / 0.630 / 1.198 ms | Not instrumented |
| Scale-dependent labels, one camera | 720 frames completed | 3.334 / 21.592 / 35.834 ms | Maximum sample 5,637 MiB |
| Scale-dependent labels, two cameras | Aborted after frame index 383 | Incomplete; no full-route result | 12,382.5 MiB |

Completed-route percentiles use passes 2 and 3. The raw CSV names their later interval `warm`, but it does not
establish complete tile residency. Low accepted counts around NYC during rapid motion also show that this route
is not equivalent to the stationary dense-label workload. Do not interpret its lower frame median as improved
rendering throughput. The summary marks the two-camera capture incomplete.

Resident growth alone does not establish a leak or attribute retention to labels versus paging and terrain.
Queue depth, tile residency, and object lifetime need a separate investigation before claiming bounded global
operation. The new engine must retain explicit budgets for queued work, tiles, glyph pages, and retired snapshots.

## Tests and limits of verification

- Config/String/GeoExtent selection: **28 test cases and 473 assertions pass**.
- Full `osgearth_tests`: does not pass. It reports unchanged ChonkPage assertions at lines 124 and 549 and then
  terminates with access violation `0xC0000005`. The XML log ends during a PagedNode test; that is not a proven
  crash attribution. A run excluding the pagednode tag still terminates with the same exception and also reports
  a ChonkPage assertion at line 353. These are baseline failures, not corrected in the build-setup change.
- Benchmark fixture: all exact accepted-ID checks pass; the JSON contains no benchmark errors.
- Setup checks: valid and repeated configuration, isolated install, dependency matching, and invalid-input
  failure propagation verified. Final build uses four explicit jobs.

Actual GL draw-call counts, allocation rates, paging queue instrumentation, stable long-run memory, concurrent
cull/draw, and multiple GL contexts are not yet measured. The baseline is sufficient to begin the private batch
and per-view handoff prototype, but does not close all Stage 0 acceptance gates or establish release readiness.

## Reproduction and artifacts

The retained local artifact directory contains `LabelBenchmarks.cpp`, `LabelFrameBaseline.cpp`,
`transient-targets.cmake`, `enable-harness.ps1`, `prepare-fixtures.ps1`, `run-frames.ps1`, `summarize.py`,
the earth/GeoJSON/font inputs, `declutter.json`, `summary.json`, CSVs, screenshots, build logs, and test logs.
These files are transient and will not travel with a commit of this report. Preserve the directory separately
before cleaning the worktree if exact local reproduction is required. The report specifies the data generator,
camera path, style policy, and statistic definitions for recreating the workload elsewhere.

To rebuild the retained harnesses, from the worktree root:

```powershell
.\build\label-baseline\enable-harness.ps1
$env:VCPKG_ROOT = 'D:\vcpkg'
$env:CMAKE_BUILD_PARALLEL_LEVEL = '4'
.\configure.bat
.\build.bat
```

From a cmd session in `tests`, initialize the environment before each new shell session:

```bat
call ..\osgearth_shell.bat
osgearth_tests "Config*,String*,GeoExtent" -r compact
osgearth_benchmarks --benchmark_filter=Labels/LegacyDeclutter --benchmark_min_time=0.2s --benchmark_repetitions=5
osgearth_label_baseline ..\build\label-baseline\labels.earth stationary.csv 2 stationary ..\build\label-baseline\arial.ttf
osgearth_label_baseline ..\build\label-baseline\labels-scaled.earth route.csv 1 route ..\build\label-baseline\arial.ttf
```

The guarded harness refuses to continue a measured run above its resident-memory cap. The original stress
fixture remains for diagnosis, not as a recommended unattended replay. After experiments, remove only the
temporary include from the benchmark CMakeLists and rerun the prescribed configure/build scripts.

## Initial implementation targets

Use the dense stationary workload and separate per-view statistics as the first performance gate. Provisional
targets for the new path are at most 2 ms of label-attributable cull/submission work per view at p95, with no
synchronous layout wait, and bounded background slices of about 2 ms. Measure result age and settle time, aiming
initially for useful placement within 100 ms during ordinary motion and convergence within 250 ms after motion
stops. These are planning targets, not achieved results or requirements to hide labels to meet a budget.

Before expanding global scale, choose explicit candidate, resident-batch, glyph-cache, and retired-frame byte
budgets from measured per-record costs. Do not use the failed legacy route's working set as the new memory budget.
The user deferred the global paging-memory investigation to proceed with implementation. The initial stress
results and test-runner crash remain unresolved; neither is a demonstrated cause or performance target for the
new engine. Partial normal-viewer control runs are retained under `build/label-controlled/`, but their analysis
was stopped and they do not establish a representative replacement baseline.
