# Label engine implementation plan

November 8, 2026. Status: the private Stage 1 core, initial Stage 2 Slughorn rendering slice, and Stage 3 background
greedy decluttering and motion stability are implemented. The initial Stage 4 tiled point-feature slice renders
the repository's global city dataset in two cameras, with independent tile selections, placements, fade timelines,
and adopted-result history. Dynamic annotation edits now feed the same independent camera pipelines. Per the
user's sequencing, performance work precedes the remaining terrain/style features. The first single-camera pass
now reduces glyph preparation and cached shaping costs. Sparse updates now reuse stable 64-label chunks; see the [performance report](label-engine-performance.md)
for before/after measurements, equivalence checks, and remaining work.
The frame-rendering pass now uses bounded cached halo distances with analytic Slughorn fills, measured with
single-camera completed-frame and GPU timings. Halo/fill geometry shares buffers but uses two ordered draws
per page/packet when halos are enabled. The performance report separates dense text throughput from the normal
global viewer's CPU/GPU costs; camera history remains independent.
Legacy memory diagnosis is deferred at the user's
direction; its unresolved measurements do not block implementation. The original legacy-versus-successor whole-frame comparison remains open.

Implements the [label engine design](label-engine-design.md). Build a useful end-to-end slice before expanding
feature coverage. Background decluttering and independent per-view placement are foundational, not later patches.
Legacy collision interoperability is excluded.

## Stage 0: reproducible build and baseline

**Current result:** isolated configuration/build succeeds using `D:\vcpkg`; all 76 dependencies were restored
from cache and match master. Stationary one/two-camera captures and checked production-solver microbenchmarks
are available. The global two-camera replay exceeds the memory cap, and the full unit suite crashes after
reporting baseline assertions. See [the baseline report](label-engine-baseline.md) for measurements, commands,
limits, and initial targets. The complete Stage 0 gate remains open; the private Stage 1 prototype can proceed
using the validated stationary fixtures while these failures are investigated separately.

Prepare only this worktree's build configuration. Continue to configure with `configure.bat`, build with
`build.bat`, and initialize the runtime environment with `osgearth_shell.bat` before osgEarth commands.

The inspected master build uses `D:\vcpkg`, triplet `x64-windows-release`, and dependencies installed at
`D:\devel\osgearth\master\build\vcpkg_installed`. Its repo-local build directory is a junction to the master build.
The worktree's `configure.bat` now honors VCPKG_ROOT and validates the selected existing installation. Set it to
`D:\vcpkg` to avoid the repo-local clone fallback. Keep worktree build and install outputs isolated.
Use the same dependency versions/options and reuse vcpkg binary caches. Prefer a separate installed tree restored
from cache; do not let manifest installation alter master's installed dependencies. Record actual cache hits and
any additional dependencies needed for shaping. Verify the shell script resolves the chosen runtime paths.

Capture a baseline from this branch's starting revision, including the input data, fonts, styles, camera paths,
display density, hardware, resolution/DPI, build configuration, and OSG threading model. Use frozen local data/cache
for repeatable comparisons. Include a dense city, global zoom/pan, tile-boundary transitions, and two independent
cameras viewing the same data. Separate cold loading from warmed rendering and isolate labels from terrain cost.

Instrument existing traversal, placement, draw submission, and paging costs. Create transient Google Benchmarks
in the existing benchmark project for representative collision and data-processing workloads. From `tests`, run
`osgearth_benchmarks` after shell setup. Keep benchmark/test source additions transient and outside the final diff;
record commands, input-generation details, and results so experiments can be reproduced.

**Deliverable:** working isolated build, baseline captures, and a reproducible workload specification.

**Gate:** baseline scene and multi-camera route replay reliably; dependency resolution is understood and master
outputs are untouched. Set initial numerical frame, worker, memory, and latency budgets from this evidence.

## Stage 1: records, views, and asynchronous handoff

**Implemented:** [LabelEngineInternal.h](../../src/osgEarthPrestige/LabelEngineInternal.h) and
[LabelEngineInternal.cpp](../../src/osgEarthPrestige/LabelEngineInternal.cpp) provide private C++17 records, immutable
batches, camera snapshots, a generation-based view registry, bounded mailboxes, and a background projection pass.
These files compile directly into `osgEarthPrestige`, alongside the font service, shaders, and renderer, under
`osgEarthPrestige::Labeling`. The demo and transient tests link the nodekit. There is no separate label-engine
library or new text implementation in core osgEarth. Headers remain uninstalled; private entry points use the
nodekit's DLL export macro so the demo exercises the actual library. Existing applications do not automatically use
the prototype. The Stage 2 demo supplies rendering; Stage 3 below adds default background collision placement.

All remaining stages belong to this nodekit as well. Both `OSGEARTH_BUILD_PRESTIGE_NODEKIT` and
`OSGEARTH_ENABLE_SLUGHORN` must be enabled to build the rendering demo. The scheduler alone remains C++17 and
does not require Slughorn. The existing core FeatureImageLayer Slughorn integration is separate and unchanged.

The initial scheduler uses one owned worker for all registered views. It visits each view once per pass, with at
most one active solve globally and one replaceable pending request per view. An owned worker keeps this persistent
service out of the general job queues and makes cancellation/join ownership explicit. Idle passes sleep for up
to 8 ms; cadence tuning and an on-demand frame wakeup belong to the rendering integration. Engine destruction
joins on the control thread after traversal stops, never in cull or draw.

Each view also has two retired-input slots and four result slots. Rendering-side submit/acquire use try-locks;
contention or full retirement capacity returns immediately. A rejected request stays with its caller. Only the
worker releases accepted requests and unpinned results during normal operation. Slow frames retaining all result
slots apply backpressure to solving rather than growing a queue. Retired view slots cannot be reused until their
results are unpinned; reuse increments the generation and old handles remain closed.

Default admission limits are eight views, 256 batches and 16,384 records per request, and 4 MiB of referenced
batch/request storage per request. Accounting includes vector capacity and conservatively counts shared batches
again for each snapshot. These are prototype limits, not measured production budgets. Retention is bounded by
the fixed mailbox/result slots plus worker-local reclamation, not solely by the one pending request. Application
batch ownership and external frame pins remain the application's responsibility.

The worker projects candidates in double precision, using each input's own viewport origin, matrices, and pixel
density. Results retain the complete immutable input and identify records by batch/record indices, preserving
stable feature keys and revisions. Compatible older poses can publish while newer snapshots are pending. Hard
configuration invalidation rejects stale completions immediately; the prototype requires explicit invalidation
for incompatible content changes as well as resize/projection changes. Per-record compatibility and pose/age
policies remain for Stage 3.

**OSG integration point:** `captureCamera` copies an active world-root cull visitor before local transforms.
After coarse batch collection/traversal, submit the completed snapshot for a later frame. Acquire a compatible
result once at this view's cull boundary, then pin it together with that frame's camera in render-leaf-owned state.
The transient probe implements this with a separate immutable drawable per frame. The Stage 2 renderer now
uses four reusable frame slots per view. Workers never access live cameras, visitors, render leaves, or execute
GL commands. Renderer preparation creates detached CPU geometry and retains immutable resource descriptors;
context-specific upload and drawing occur on the graphics threads.

**Validation so far:** the prescribed configure/build scripts succeed with the existing `D:\vcpkg` installation.
All 11 transient contract tests pass (111 assertions), alongside 28 existing focused tests (473 assertions),
for 39 tests and 584 assertions total. The new tests exercise camera transforms/DPI, admission bounds, slow workers, pending
replacement, invalidation, pinned frames, generation reuse, worker-side destruction, solver exceptions,
concurrent submissions, and cancellation. Actual OSG probes ran two explicitly registered cameras for 400 frames
each in both `SingleThreaded` and `CullThreadPerCameraDrawThreadPerContext` modes, using separate GL contexts.
Both modes reported zero identity/frame errors and 398 pinned draw frames per camera. The probe validates
ownership and handoff, not typography, visual quality, collision correctness, or a performance gain.

Reproduction artifacts are transient under `build/label-stage1/`: `LabelEngineTests.cpp`, `CullSmoke.cpp`, build
logs, `tests-final.log`, `cull-single.log`, and `cull-threaded.log`. Tests compile the private implementation into the
test executable without exporting prototype symbols. Run `osgearth_tests "[label-engine]" -r compact` and
`osgearth_label_cull_smoke [threaded]` from `tests`, after `osgearth_shell.bat`, while the transient CMake targets
are enabled. Their temporary CMake additions are removed from the final working diff.

**Integration update:** the Stage 2 demo connects frame pinning to instanced glyph rendering with geocentric
anchors, normal EarthManipulator cameras, and automatic near/far adjustment. On-demand frame requests and the
broader projection/context recreation matrix remain open. There is no public layer or tile adapter yet;
the Stage 3 slice below now provides background collision placement.

Introduce private immutable batch/record structures, stable label identity with revisions, and an explicit
ViewKey/generation registry. Implement bounded request/result exchange and fair scheduling using existing job
facilities where suitable. One active solve and one replaceable pending request per view is the initial contract.

Use a trivial solver or synthetic placements to establish the full lifecycle before implementing sophisticated
collisions. Determine the exact OSG integration point for per-view cull snapshots and safe frame adoption. Capture
batch references and coherent camera/viewport state in reusable storage. Pin displayed state for in-flight frames.
Keep worker completion, destruction, resource reclamation, and late-result rejection off the critical traversal path.

Exercise two cameras concurrently with different poses, viewports, and device-pixel ratios. Reuse a camera address
after destruction to test generation handling. Simulate slow workers, rapid updates, and continuous movement.

**Deliverable:** observable independent view states and nonblocking snapshot-to-result flow.

**Gate:** no worker reads live traversal state; no cross-view visibility/offset leakage; queue lengths and retained
memory remain bounded. A lagging worker cannot stall cull or starve all publications. Validate supported OSG
single-threaded and concurrent cull/draw configurations before proceeding.

## Stage 2: typography and a Slughorn rendering slice

**Implemented rendering slice:** `LabelTextInternal.h` / `LabelText_Slughorn.cpp` provide a worker-confined font
service using FreeType outlines and HarfBuzz glyph-ID shaping. Only the SDK bridge requires C++20. The Slughorn
overlay port now installs its pinned FreeType adapter; its port version is incremented. FreeType is an explicit
dependency and HarfBuzz is added without optional features. This worktree uses `D:\vcpkg` and has
`OSGEARTH_ENABLE_SLUGHORN=ON` in its preserved CMake cache. The core dependencies were reused; the new HarfBuzz
package and revised Slughorn package required local builds (about 36 seconds and 3 seconds respectively).

Text preparation caches immutable runs for a fixed font instance, retains glyph IDs and UTF-8 clusters, and
appends sealed atlas pages for missing glyphs. Existing pages/runs remain unchanged. The explicit limits are
1,024 cached runs, 4,096 glyphs, 32 pages, 32 MiB of atlas data, and 16 MiB of shaped-run data. OSG 3.6 cannot size
RG_INTEGER images, so the upload representation expands the SDK's RG16UI bands to RGBA16UI with zero padding;
all addresses and curve data are preserved. CPU images remain available for additional GL contexts.

`LabelRendererInternal.h/.cpp` batches accepted glyphs into instanced quads, with one draw per occupied page.
The worker packs detached CPU geometry; unchanged accepted record sets reuse their instance buffers. Rendering
uses current-frame camera matrices and high/low world coordinates, independent viewport/DPI state, rotated
labels, small halos, and a 200 ms per-view initial fade. Four reusable frame slots pin results and uniforms until
render leaves retire. Vertex divisors are applied after VAO binding to accommodate OSG's core-profile behavior.
The demo uses no PlaceNode, LabelNode, ScreenSpaceLayout, or osgText drawing.

**Runnable demo:** [osgearth_labels](../../src/applications/osgearth_labels/osgearth_labels.cpp) displays ten
shared geocentric Manhattan labels in two independently manipulable cameras. Camera B has a different heading,
range, and 1.25x pixel density. It includes 12-pixel text, Latin accents/combining marks, Arabic, and rotated text.
The default Windows font is Segoe UI; `--font` selects another scalable font. Ligature support depends on the
font: tests verify the default `ffi` substitution with Calibri, while Segoe UI renders that string separately.

From a cmd session in `tests`, after building with the prescribed scripts and Slughorn enabled:

```bat
call ..\osgearth_shell.bat
osgearth_labels
osgearth_labels --threaded --separate-contexts
osgearth_labels --threaded --animate
```

Drag/zoom either viewport independently. `--animate` moves only camera A. `--frames N --capture image.png`
provides a finite visual check; capture occurs after frame 90. With separate windows, the capture shows camera B.
The demo uses offline terrain and does not fetch map imagery. The library, shaders, and font service remain
private; the executable is installed by the tools/examples build when Prestige and Slughorn are enabled.

**Validation:** all 16 engine/typography contract tests pass (179 assertions); combined with the existing focused
Config/String/GeoExtent tests, 44 tests and 652 assertions pass. The new contracts cover shaping, combining
marks, RTL clusters, malformed UTF-8, missing fonts/glyphs, incremental page reuse, cache admission, preparation
thread confinement, failure containment, stable instance-buffer reuse, and the earlier handoff invariants.
Visual runs pass with single-threaded rendering, concurrent cull/draw in a shared context, and two separate
contexts. The final ten-label demo uses one 81,920-byte atlas page and one instanced text draw per view once ready.
Animating camera A changed 29,742 pixels on the left side of a matched capture and zero pixels on camera B's
side. Artifacts and transient test sources are under `build/label-stage2/`; tests are not added to the repository.
These are correctness checks and resource counts, not a before/after frame-rate claim.

**Prestige migration validation:** the prescribed configure/build scripts succeed with `D:\vcpkg`; the same
44 tests / 652 assertions pass when linked to `osgEarthPrestige`. Shared-context and separate-context captures
are byte-identical to their pre-move counterparts. Binary inspection confirms that the demo imports labeling
entry points from `osgEarthPrestige.dll`, with no labeling exports in core osgEarth and no private label headers
installed. Migration logs, captures, and transient tests are under `build/label-prestige/`.

**Remaining Stage 2 gate:** font fallback, mixed-direction paragraph segmentation, variable-font instances,
live resource replacement, explicit upload budgets/readiness, context-loss testing, and icon/background/leader
compositing are not implemented. The current font service shapes single-direction runs and reports missing
glyphs instead of silently substituting another font. Halos use a small eight-neighbor analytic coverage stencil,
limited to two logical pixels. Fade/history is per view at initial publication/resize; per-label transitions
belong to Stage 3. The horizon check approximates surface anchors on WGS84; elevated-anchor and terrain/scene
occlusion remain open. Stage 3 now supplies collision placement; asynchronous motion can still cause temporary overlap.

Resolve the private C++17/SDK boundary and glyph-ID import. Package required font adapter support, add shaping
dependencies deliberately, and implement a bounded shaped-run cache plus glyph atlas pages. Establish font
fallback, malformed-input handling, and consistent ink/advance/collision metrics.

Render synthetic batches with instanced Slughorn glyph quads, current-camera anchors, and per-label offsets.
Include halos and alpha fades immediately. Make resource readiness, upload budgets, multiple graphics contexts,
and context recreation visible in diagnostics. Use a simple icon/background/leader sample to establish compositing
order without implementing full callout placement yet.

**Deliverable:** inspectable multilingual text in two independently rendered views, using the Stage 1 handoff.

**Gate:** small text, rotated text, halos, combining marks, ligatures, right-to-left runs, mixed direction, fallback
fonts, and high DPI render correctly. Glyph keys distinguish font instances and shaping output. New glyphs do not
rebuild the whole atlas. Record CPU/GPU costs and image-quality evidence before expanding scale.

## Stage 3: background collision placement and motion policy

**Implemented first placement slice:** [LabelDeclutterInternal.cpp](../../src/osgEarthPrestige/LabelDeclutterInternal.cpp)
adds synchronous, testable greedy decluttering, selected by default when `Engine` has no custom solver. The existing
worker invokes it once per pending view request; cull still only posts snapshots and adopts completed results.
The demo requests at most 15 ordinary updates/second per camera, with urgent refresh after hard invalidation and
idle worker polling unchanged at 8 ms. Motion stability adds bounded pose checks and a settling follow-up.

Projection and the renderer's WGS84 surface-anchor horizon test run before collision ordering. Higher numeric
priority wins. Equal priorities prefer the same key/revision in that view's preceding result, then stable
feature identity, then revision and input index. History from another view, reused generation, or incompatible
configuration is ignored. Higher-priority newcomers can displace retained labels; accepted duplicate identities
are suppressed. With animation enabled, the acknowledged displayed result supplies history; scheduling-only
clients can still use the latest published result. No separate unbounded history cache retains old batches.

The index has at most 64 x 64 cells. A normal footprint occupies at most 64 cells; larger footprints use a separate
list and exact scans rather than unlimited cell insertion. Repeated cell hits count as one exact comparison per
candidate/accepted pair. Default per-view solve limits are 4,096 accepted labels and 262,144 exact comparisons,
in addition to the existing 16,384-record admission limit. Exhaustion publishes only the tested, collision-free
prefix and sets a diagnostic flag; canceled work publishes nothing. These are operation limits, not a wall-time
deadline. Sorting remains O(K log K), and large/dense footprints can reach the comparison cap.

`setTextFootprint` prepares conservative axis-aligned bounds of rotated text, including halos and antialiasing
padding. The solver adds two logical pixels per side, scaled by the camera's pixel density. Touching padded edges
are allowed. Rotated labels can be rejected conservatively; exact oriented-footprint packing remains deferred.
Rendering reprojects accepted world anchors with the current camera and reuses unchanged glyph instance buffers.
The demo gives gold landmarks higher priority; `--no-declutter` restores the projection-only comparison path.

**Validation:** 51 focused tests / 783 assertions pass, including exhaustive and R-tree selection equivalence on
five spatial distributions at three pixel densities; priorities, history, revision/configuration/generation reset,
duplicate IDs, budgets, cancellation, horizon filtering, and rotated halo bounds. A real-worker test seeds two
cameras with different winners over the same batch, then confirms their histories remain different even after
their camera transforms become identical. Existing mailbox, fairness, lifetime, and typography tests still pass.

In threaded shared-context and separate-context visual runs, camera A accepts 9/10 labels and B accepts 7/10.
Moving only A changes 28,388 pixels in A's capture and zero in B's. Final sampled solve times in these tiny demo
runs are 0.017ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â¦ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦ÃƒÂ¢Ã¢â€šÂ¬Ã…â€œ0.042 ms; they are not representative throughput or percentile measurements.
Single-threaded and threaded captures are byte-identical. The `--no-declutter` capture is also byte-identical
to the preceding rendering slice, providing a reproducible visual before/after comparison.

**Controlled index comparison:** Google Benchmark, Windows x64 RelWithDebInfo, 4,096 candidates, identical
projection/priority/padding semantics, three repetitions, 0.15-second minimum per repetition. Entries below are
median wall time in milliseconds. Every implementation verifies its full ordered selection and padded rectangles
against the exhaustive oracle before timing. Benchmarks raise the placement/comparison caps to avoid truncation.

| Fixture | Exhaustive scan baseline | Grid | R-tree | Accepted |
| --- | ---: | ---: | ---: | ---: |
| Sparse small labels | 13.428 | 1.419 | 4.868 | 3,708 |
| Dense long labels | 3.010 | 1.086 | 1.810 | 890 |
| Tight cluster | 0.390 | 0.552 | 0.632 | 27 |
| Conservative rotated bounds | 1.797 | 0.882 | 1.328 | 650 |
| Mixed oversized labels | 10.499 | 1.450 | 3.133 | 2,660 |

The grid wins four cases against exhaustive scanning and all five against the R-tree. The tight cluster favors
the simple scan because so few labels survive. Sparse exact comparisons fall from 7,329,147 to 3,842. These are
algorithm comparisons for the new worker, not measurements against legacy ScreenSpaceLayout or viewer frame-rate
improvements. Sources, logs, JSON, and captures are transient under `build/label-declutter/`. Reproduce after
hooking its transient sources into the existing test/benchmark targets and building with the prescribed scripts:

```bat
call ..\osgearth_shell.bat
osgearth_tests "[label-engine],[label-text],[label-declutter],Config*,String*,GeoExtent" -r compact
osgearth_benchmarks --benchmark_filter=LabelLayout --benchmark_min_time=0.15s --benchmark_repetitions=3
osgearth_labels --threaded
osgearth_labels --threaded --animate
osgearth_labels --threaded --separate-contexts
```

**Implemented motion stability:** [LabelMotionInternal.cpp](../../src/osgEarthPrestige/LabelMotionInternal.cpp)
prepares per-label fade curves on the existing worker. Each camera acknowledges its displayed sequence and actual
adoption time in the next request. A bounded mailbox pin keeps that result alive until the worker consumes it;
the new result never retains a chain of previous results. Skipped publications cannot become visible fade history.
Cull rejects a publication based on a superseded display acknowledgement and retries against the current one.

Entries and exits use 200 ms linear fades. Relative entry/exit curves begin at actual adoption, so worker latency
cannot consume an entry before it is displayed. Continuing curves are converted to absolute camera time; reversals
carry the preceding slope so their initial opacity is evaluated at adoption rather than at the older request time.
Glyph instances carry immutable fade descriptors; the vertex shader evaluates opacity from per-camera uniforms.
There is no per-frame CPU loop over all glyphs for animation. Shader times are rebased near the result's sample
time to avoid losing fade precision in long-running viewers.

Outgoing labels reserve that view against new entries until their fades end (at most 200 ms). This conservative
view-wide exit-then-entry barrier avoids a second collision pass and extra spatial index. Already visible winners
continue uninterrupted. It can delay unrelated new labels briefly; spatially local reservations are a future
refinement. Outgoing labels are capped at the configured placement count, and additional retained batches obey
the batch-count/byte limits. Under overload, excess outgoing labels disappear immediately. Total glyph instance
capacity, including outgoing labels, is 131,072; preparation failure retains the prior publication.

`compatible` checks viewport/DPI, camera direction and up to 32 representative accepted anchors. Moving layouts
expire after 500 ms; direction changes over 35 degrees or displacement beyond 35% of the smaller viewport dimension
reject them immediately. Stable layouts do not expire while idle; the age window restarts when ordinary movement
resumes. Resize, clock reversal, large jumps, and expired moving layouts invalidate only their own view and request
fresh work. These are initial policy thresholds, not calibrated production guarantees. Bounded sampling can miss
local movement outliers. Current-frame GPU horizon/offscreen rejection remains active.

Near/far-only automatic projection changes no longer trigger new layouts. CPU eligibility now matches the existing
screen-overlay shader: clip X/Y and positive W, plus the horizon test, determine visibility; the depth interval
does not. Terrain occlusion and explicit range policy remain separate, deferred work. A final follow-up request compacts
completed fade descriptors and releases outgoing labels, then stops. `ViewNode::needsFrame()` is a thread-safe
frame-demand signal for the viewer's control loop while submissions, publications, fades or settling work remain.
It does not call mutable viewer APIs from the worker/cull threads. The demo integrates it with the viewer's normal
frame-demand check using `--on-demand`; `--seconds N` bounds such a run. `--motion-test` jumps only camera A at
frames 60 and 120, and `--capture-frame N` supports inspecting intermediate fades.

**Motion validation:** 58 focused tests / 834 assertions cover scheduling/typography/decluttering contracts plus actual
adoption clocks, independent camera fades, exit reservations, settling, delayed reversal continuity, bounded old
batch ownership, configuration resets, pose/age/jump policy, depth-only changes, acknowledged-history pinning,
skipped publications, and frame demand after success, failure and invalidation. Tests and benchmark additions
remain transient under `build/label-motion/`.

The settled image is byte-identical to the preceding decluttering slice. Moving only A changes 28,388 pixels in
A and zero in B. The two-jump run reports two invalidations for A and zero for B; B remains pixel-identical in
the jump and intermediate fade captures. An early post-jump image has 435,392 units of summed RGB distance from
the background on A, increasing to 1,730,523 when settled, while B changes zero pixels. Shared and separate GL
contexts render successfully. Both stationary views settle after two publications and report no further label
frame demand. The normal map still requests frames, so `--on-demand` does not demonstrate a fully idle map viewer.

Google Benchmark (same Windows RelWithDebInfo build, three repetitions, 0.15-second minimum) measures median
compatibility checks at 0.568 / 0.576 / 0.652 microseconds for 32 / 4,096 / 16,384 placements. Only 32 anchors are
sampled. Worker fade preparation for 4,096 labels measures 1.134 ms for unchanged selection and 1.059 ms with half
departing. These measure the added policy/preparation cost, not full cull percentiles or a frame-rate improvement.

```bat
call ..\osgearth_shell.bat
osgearth_tests "[label-engine],[label-text],[label-declutter],[label-motion],Config*,String*,GeoExtent" -r compact
osgearth_benchmarks --benchmark_filter=Motion --benchmark_min_time=0.15s --benchmark_repetitions=3
osgearth_labels --threaded --motion-test --frames 180
osgearth_labels --threaded --on-demand --seconds 3
```

**Remaining Stage 3 gate:** iterative regional refinement, spatial fairness/aging of equal-priority losers,
range/scale policy, urgent selection work, and cull/result-age/settling percentiles on a representative moving
tiled dataset. Stationary layouts settle but are not repeatedly refined. Collision freedom applies to the solved
snapshot; it is not promised between asynchronous updates during motion. Existing visible labels may overlap
temporarily while reprojecting or exiting. The rest of this stage's original acceptance plan follows.

Implement worker-side projection, horizon/range filtering, bounded candidate selection, deterministic priorities,
and a spatial collision index. Compare a grid with an R-tree on sparse, dense, clustered, rotated, and oversized
label workloads. First publish a coherent greedy result; then add previous-placement preference and bounded
incremental reconsideration. Provide a synchronous harness around the same solver for repeatable validation.

Implement publication ordering, record compatibility, pose/age validation, sleep/wake behavior, and fair slices
across views. Adopt older compatible results during continuous motion; reject results after hard invalidation.
Add fades and bounded reservations, current-frame accepted-anchor rejection, and frame requests for on-demand
viewers. Selecting a label requests priority work without introducing synchronous layout.

**Deliverable:** decluttering that remains responsive while the solver runs at lower frequency than rendering.

**Gate:** paired before/after collision benchmarks demonstrate the actual change in work. Correctness tests cover
priority, collisions, budgets, deterministic ties, and starvation. Measure cull p95/p99, result age, worker CPU,
temporary overlap, and settling time during continuous motion and after large camera jumps. Report aggregate and
per-view results; do not hide slow-view latency behind an average.

## Stage 4: tiled feature integration and dynamic annotations

**Implemented tiled slice:** `LabelTilesInternal.h/.cpp` connects an opened FeatureSource to immutable batches
inside `osgEarthPrestige`. The demo's `--tiled` mode spatially queries `data/cities.gpkg` (7,322 Natural Earth
populated places) and uses `data/world.tif` as offline globe imagery. These are geographic tile queries against
a GeoPackage, not a newly generated on-disk tile pyramid. No per-feature PlaceNode or LabelNode is constructed.

One dedicated `oe.labeltiles` worker owns feature queries, geographic selection, shaping, and cache reclamation;
the existing `oe.labels` placement worker remains independent. Each camera sends replaceable value snapshots
through a nonblocking mailbox. The worker computes a horizon/frustum-filtered global-geodetic quadtree cut for
that camera, visits registrations round-robin, and loads at most one missing tile per registration per pass.
There is no tile enumeration, source I/O, or font preparation in cull. Geographic bounds queries reuse core
FeatureSource/Profile/TileKey facilities. Native tiled sources must cover the requested global-geodetic levels
starting at zero; other tiled profiles/level mappings are explicitly rejected in this slice.

Each view has four bounded coverage publications. A subtree retains its ready parent until all needed child
coverage and shaped resources are ready; a publication never contains overlapping parent and child coverage.
Boundary ownership is canonical, including exact poles and either representation of the antimeridian.
Identity is source ID plus feature ID, independent of display text and tile level. The same record can therefore
keep its per-camera placement/fade history across tile replacement. New content requests ordinary rate-limited
layout, while camera jumps still invalidate urgently. Background tile arrivals participate in on-demand frame demand.
Sources must supply stable IDs across repeated queries/levels; per-query `autoFID` is rejected.

Default limits are 24 selected tiles per camera, 256 records per tile, level 8, and a shared 128-entry/32 MiB
tile cache. Each query scans at most 50,000 features and retains only the highest-priority bounded subset before
shaping. Failed loads back off for ten seconds and retain available parent fallback. Cache eviction releases
cache ownership; bounded coverage, placement, outgoing-fade, and draw slots may still pin older immutable batches.
The cache byte counter conservatively includes tile-local runs/styles, but excludes shared glyph pages and all
terrain/driver allocations. It is not a process-memory cap. The font service bounds its run cache to 1,024 entries,
evicts cache ownership without invalidating published runs, and retains the existing 32-page/32 MiB atlas limit.
The tiled demo raises per-request batch accounting to 32 MiB. Glyphs from different tiles are grouped by shared
atlas page, rather than generating a draw call per tile.
Underlying FeatureSource cursor materialization and source caches are also outside the provider's budget;
the checked GeoPackage path streams bounded spatial queries without the core tile-query feature cache.

The initial style policy matches the earlier city fixture: rank at least 12 below level 6, rank at least 8 at
levels 6/7, and all ranks from level 8, subject to per-tile admission. Major/minor labels use two fixed styles.
The sample enables an explicit repair of three legacy CP1252 control characters in its names. This is not a
general encoding conversion or CSS style translator. Point/multipoint-singleton geometry is supported; anchors
are ten metres above the ellipsoid. Editing the underlying tiled FeatureSource, terrain-relative/clamped anchors,
terrain/scene occlusion,
general style translation, and font fallback remain unimplemented. The complete Stage 4 gate stays open.

Run from `tests`, after `call ..\osgearth_shell.bat`:

```bat
osgearth_labels --tiled --threaded
osgearth_labels --tiled --threaded --tour --frames 4320
osgearth_labels --tiled --threaded --separate-contexts
```

Drag/zoom either camera independently. `--features` and `--imagery` override the local files; `--animate` moves
only camera A, while `--tour` takes both cameras through eight offset global/regional stops, including the
antimeridian and a polar view. The synthetic typography fixture remains the default without `--tiled`.

**Tiled validation:** the prescribed scripts build/install against `D:\vcpkg`. Transient integration tests cover
independent deterministic geographic cuts, shared batch identity, parent/child exclusivity at every observed
publication, stable IDs at exact tile boundaries/poles/antimeridian, repeated names, cache eviction with older
frame pins, dynamic batch rendering, failure handling, persistent-ID validation, and 3,072 names through the
bounded run cache. They run alongside the existing focused engine, typography, declutter, motion, and core tests;
all 64 focused cases passed (10,099 assertions in the final run). Assertion totals vary with the number of
intermediate asynchronous publications checked. The full-suite baseline failures remain outside this validation.
Test sources/hooks stay out of the final diff.

The normal CompositeViewer with shared offline imagery, two independent EarthManipulators, Segoe UI, 720x800
viewports (DPI 1.0/1.25), and threaded cull/draw completed 4,320 frames: three passes through the eight-stop tour,
with camera B three stops ahead. It performed 566 tile loads and 438 evictions, ending at 128 resident tiles and
1,121,640 conservatively accounted tile bytes, with no query/font failures. Half-second process sampling peaked
at **236.4 MiB working set / 284.9 MiB private bytes**. Approximate 24-second pass windows peaked at
230.7/236.4/235.9 MiB working set and 276.3/284.9/283.9 MiB private bytes; the final sample was
210.6/258.5 MiB. This is a roughly 73-second bounded-cache exercise, not a long-duration leak proof or a paired
speedup claim against the legacy route, which included a different label workload.

Separate GL contexts completed 360 frames with 32/36 accepted labels in the stationary cameras and no tile
failures. During 600 frames moving only A, A adopted 138 publications while B settled after five (36 accepted
labels, no invalidations, no pending label work). A remained responsive with current-frame anchor reprojection.
The eight-second on-demand run ended with no pending work in either label view; the map's other frame requests
kept the viewer rendering, so this does not establish whole-viewer idle sleep. Captures were visually checked for
both global cameras, regional labels after paging, and the second graphics context. Local logs, captures, source
fixtures, memory CSV, and the sampler script are under `build/label-tiles/`.

**Implemented dynamic annotation slice:** `LabelDynamicInternal.h/.cpp` adds a private `DynamicSource` to Prestige.
`set(id, value)` adds or replaces text, absolute world anchor, text style, and priority; `erase(id)` removes it.
Each successful submission returns a monotonically increasing revision; zero indicates rejected capacity or a
closed/failed source. Invalid values throw before altering desired content. Source IDs must be unique among
sources attached to an engine. Annotations supplement the tiled/static batches; they do not edit the GeoPackage.

The dedicated `oe.labelupdates` worker owns its font service. Edits coalesce into the latest bounded desired set,
and preparation publishes one complete immutable set of stable 64-slot chunks only if its generation is current.
An edit replaces only its chunk; insertion/erasure reuse vacant slots without shifting other chunks. The worker
captures only dirty chunks as immutable entry references, and reuses unchanged prepared batches. Successful
superseded preparations may seed the bounded chunk cache, but never publish as current content. A failed preparation retains the last good publication and records its error/completion revision;
the application can correct or erase the failing entry. Four publication slots provide backpressure while older
camera snapshots remain pinned. Idle work polls at eight milliseconds, with edit notifications waking preparation.
Default limits are 1,024 annotations, 1 MiB of desired UTF-8, and 8 MiB per prepared set, in addition to the font
service's existing run/glyph limits. These are component budgets, not total process limits. The caller must size
Engine input limits for its combined sources; the dynamic demo uses 32 MiB. Destruction joins preparation only
on the control thread after traversal stops.

Every entry has a content revision and a lifetime token. Changes keep the lifetime but advance only that record's
revision, causing its footprint/priority to be reconsidered while unrelated records retain placement preference.
If the edited label is accepted, its new anchor/text/style replaces the old presentation at adoption and carries
forward that camera's opacity; movement does not spawn an outgoing ghost or restart entry fades. Removal uses
the existing outgoing fade, and remove/re-add creates a fresh lifetime. Rejected edits can leave the old outgoing
presentation visible during its bounded fade. There is no position interpolation or text morphing in this slice.

Each ViewNode independently combines static, tiled, and dynamic content and tags placement requests with its
prepared annotation revision. Adoption rejects an older completion after a newer content snapshot has arrived,
without invalidating unrelated displayed history. Existing compatible output remains until a replacement is
ready. Fade clocks and acknowledged-result history remain strictly per camera. Thread-safe frame-demand polling
checks each camera's own consumed annotation revision, so an idle camera wakes even when another has adopted it.

Use `osgearth_labels --tiled --dynamic --threaded` from the initialized `tests` shell. Both cameras start over
Europe at different scales/headings, and a ten-second cycle moves/renames/restyles an annotation, adds a report,
removes the moving label, and re-adds it with a new lifetime. The same option works over the synthetic fixture,
with separate GL contexts, and in on-demand mode. Use `--single` for performance work. Dynamic and tiled sources
still own separate font services. The incremental renderer now caches one packet per dynamic chunk, while merging
ordinary static/tiled selections by atlas page. Unchanged record selections and fade descriptors reuse immutable
instance arrays; a free frame also reuses unchanged drawable/VAO/uniform clones. Packets retain separate time
origins, so reuse cannot shift a camera's fades. Selection and fades can invalidate additional chunks when required;
the background collision pass still considers the complete candidate set. See the performance report for measured
sparse-edit gains and the extra draw-submission cost of chunking.

**Dynamic validation:** 73 focused cases passed, including nine new transient update cases (183 assertions).
The combined run reported 10,446 assertions; tiled publication checks vary with scheduling. Tests exercise
immutable snapshots, unchanged record revisions and collision preference, edit/re-add lifetime semantics,
four-slot backpressure with 100 coalesced edits followed by deletion, admission limits, font/byte-budget errors,
recovery, and different per-camera opacity clocks. Real headless SceneView culls prove independent idle wakeup
and reject an old blocked placement completion after a newer annotation generation arrives. The existing
engine/typography/tile/motion tests also pass. Full-suite baseline failures remain outside this focused gate.

The threaded tiled viewer completed two edit cycles over 22 seconds (1,296 frames), with 169 submitted/completed/
published revisions, no dynamic or tile failures, and both cameras displaying revision 169 independently with
no hard invalidations. Separate-context tiled and on-demand synthetic runs each completed a full cycle plus
re-entry over 12 seconds; both cameras reached revision 94 with no failures or pending label work. Captures were
visually checked in all three configurations. These are behavior checks, not frame-time benchmarks; the map still
requests frames in on-demand mode. Artifacts and transient source are under `build/label-dynamic/`; test CMake
hooks are removed from the final tree. The prescribed configure/build scripts continue to use `D:\vcpkg`.

Connect feature sources and style translation to batch preparation without constructing one PlaceNode per feature.
Reuse suitable paging machinery, with explicit per-tile ownership and cancellation. Add stable IDs, cross-tile
duplicate handling, scale-dependent density, parent/child coverage handoff, and resource-ready fallback behavior.

Integrate dynamic label handles and asynchronous terrain-relative/clamped anchors. Revision changes invalidate
affected placement records without throwing away unrelated view results. Keep tile preparation, shaping, and
elevation work out of cull. Track memory, cancelled jobs, retained snapshots, and stale references during travel.

**Deliverable:** the global tiled workload and dynamic annotations running through the new engine.

**Gate:** no duplicate/gap caused by normal parent/child handoff; repeated names remain distinct; unload/reload,
partial children, tile boundaries, antimeridian, poles, and multiple views behave according to the design. Compare
full-route frame timings, draw counts, displayed density, loading latency, and memory against Stage 0. Separate
intentional layout-policy differences from regressions and document unsupported style options explicitly.

### Earth-file / OpenFreeMap integration

`tests/openfreemap_labels.earth` now combines the GPU vector basemap with native OpenFreeMap `place` tiles through
an experimental `prestige:labels` factory in the Prestige nodekit. It loads in the ordinary viewer. Native
Web Mercator ownership is separate from Earth-fixed anchors and globe culling bounds; source levels cap the
paging cut. Class rules apply inverse source ranking, localized-name fallback, and per-class typography.
Primary-camera routing preallocates bounded view registrations and keeps independent placement/fades, including
context-specific pixel scaling; auxiliary cameras do not acquire placements. Missing glyph runs are isolated
instead of failing the entire tile. The C++ adapter remains private; see `tests/openfreemap_labels.md` for usage.

Validation: 80 focused cases pass, including a native Web Mercator fixture, geocentric near-field culling,
inverse ranking, fallback names, stable IDs/precise anchors, missing-glyph isolation, strict font resolution,
factory/configuration round trips, layer removal/re-addition, independent camera buffers, and auxiliary-camera
exclusion. Live normal-viewer captures of London, Manhattan, and the world had zero tile failures (161, 32, and
80 accepted labels in these captures). Sources/logs/images remain transient under `build/label-earth`; no new
permanent test targets are required by the demo.

## Stage 5: iterative callouts and picking

**Initial slice implemented:** opt-in point placement alternatives and instanced leaders. The background
solver tries the previous camera-local alternative first, then the centered position and eight compass
alternatives, with the existing strict priority and shared comparison budget. `placement="callout"` on a
Prestige style selects a fixed 12 logical-pixel gap; the demo enables it for cities and smaller settlements.
Leaders use the label fill color at 70% alpha and a fixed one-logical-pixel width, with analytic antialiasing.
Text and leader anchors reproject on the GPU using the current camera; they share the same fade descriptor.
The worker packs offsets and leaders into immutable cached packets. One additional instanced draw covers
all displaced labels in a merged point packet; there are no per-label scene nodes or cull loops.

This is a bounded greedy first solution. It does not yet refine crossings, collide leader segments, animate
relocations between alternatives, draw icons/backgrounds, or provide picking. Retained slots reduce motion,
but a blocked slot can change immediately at publication. Full viewport fit is required for callout text.
Default point placement and road rendering are unchanged. The remaining Stage 5 work follows below.

Implement compound text/icon/background footprints, placement alternatives, leader constraints, and bounded
crossing penalties in the shared solver. Publish a useful initial solution, then improve it in background slices.
Add improvement thresholds and convergence limits to prevent perpetual rearrangement. Validate transitions, not
only endpoint layouts, and batch leaders without per-label draw calls.

Connect picking to the source view's actual displayed placement and animation state, including viewport/DPI
conversion and feature identity. Test both source and pick viewports at different sizes and origins. Picking must
not independently solve layout or include records that the displayed frame could not render.

**Deliverable:** stable callouts and picking for dynamic and tiled labels in multiple cameras.

**Gate:** leaders remain attached during motion; constraints and explicit overlap overrides are respected; rejected
refinements do not produce jitter. Picks match displayed labels during fades, view changes, tile eviction, and
resource arrival. No per-callout growth in scene-graph or draw-submission overhead.

## Stage 6: robustness, API, and release decision

Finalize the public LabelLayer/LabelHandle configuration, availability reporting, style support matrix, limits,
and diagnostics. Keep public interfaces C++17 and Slughorn private. Document all new functions, ownership,
threading, preconditions, and failure behavior. Increment OSGEARTH_SOVERSION when public ABI changes require it.

Exercise camera removal, context loss/recreation, font failure, atlas exhaustion, cancellation, overload, and long
travel sessions. Implement or explicitly defer terrain/scene occlusion; absence cannot be presented as parity.
Add stereo grouping only with eye-specific projection and group-aware selection tests. Keep legacy interoperation
outside the scope; any future adapters submit new-engine records rather than adding a collision bridge.

Run unit tests from `tests` with `osgearth_tests`, after shell setup. Keep new scenario tests and microbenchmarks
transient unless requested otherwise; existing core tests may be updated where appropriate. Build with the
prescribed scripts and verify portability of C++17 interfaces and the confined SDK bridge.

**Deliverable:** reviewable implementation, usage documentation, and a before/after validation report.

**Gate:** agreed performance/latency budgets pass at comparable workloads and quality. Memory reaches a bounded
steady state, multi-camera isolation holds, and supported behavior is verified. Do not enable the new engine by
default merely because it draws text successfully.

## Road-name rendering slice (implemented)

The OpenFreeMap companion now requests `transportation_name` alongside `place`. A `placement="line"` style
selects road geometry and shares the existing per-camera priority/collision pass with point labels. The demo's
Neighborhood detail bookmark shows London street names at a 2.5 km range.

The installed Slughorn SDK includes `Canvas::textOnPath`, but that convenience method iterates individual
bytes and resolves codepoint shapes. Prestige therefore retains HarfBuzz-shaped glyph IDs, advances, offsets,
and clusters. It implements the same arc-length/tangent placement approach over native polylines, additionally
retaining source segments for perspective-correct ECEF interpolation. Slughorn still renders every glyph;
no legacy osgText layout or rendering is involved, and no dependency update is required.

- Tile preparation clips multipart lines to the native tile, keeps the longest continuous piece, and converts
  it to double-precision ECEF. Limits are 4,096 scanned vertices per feature, 256 retained path vertices, and
  256 ink glyphs per road name; path and footprint storage counts against existing batch/cache budgets.
- Each camera's placement worker fits the complete shaped run in projected arc length, selects upright
  direction, rejects insufficient length or sharp bends, and produces a conservative ink/halo rectangle for
  the existing bounded declutter grid. Paths do not introduce work proportional to road count in cull.
- Immutable camera-local glyph anchors/tangents travel with placements and fade presentations. The renderer
  reprojects them every frame; screen-pixel glyph size stays fixed and spacing refits at the existing 15 Hz
  worker cadence. Point buffers retain their existing reuse behavior; roads use a separate merged packet so
  changing path layouts do not repack unchanged point labels. Halos and fills remain batched by atlas page.
- Source feature ID plus label role identifies a road; identity is not derived from the displayed name.
  Camera registration generations, independent history, fade reversal, and retirement continue to use the
  existing engine contracts. No installed public API was added.

The first slice intentionally does not stitch adjoining tile/feature fragments, select repeated placements
along long roads, draw route shields, or test terrain occlusion. One candidate uses the center of the retained
piece and must fit entirely inside the viewport; alternate visible spans are a follow-up. Conservative AABBs
can over-reject diagonal roads. Different source IDs can produce multiple labels for one named road.

Validation artifacts are transient under `build/label-roads`: native tiled multipart/shaping checks, path
fit/reversal/curvature/perspective/DPI checks, shared point-road collisions, independent camera placement/fades
and actual cull buffers, a real London framebuffer, and single-camera GL/worker benchmarks. See the performance
report for controlled timing and its limits.

## Smooth GPU road following (automatic when supported)

The earth layer enables per-frame GPU fitting by default when osgEarth reports GLSL 4.30 support. Unsupported
systems retain CPU fitting; `<gpu_paths>false</gpu_paths>` forces that control even on capable hardware.
Explicit true also falls back when unsupported. Selection uses the layer-open common capability probe;
applications deliberately creating older GL contexts on newer hardware must force false.
The GPU renderer requires OpenGL 4.3 in each rendering context. Source paths and glyph offsets remain immutable and bounded;
`LabelPath.comp.glsl` projects each road once and shares its fitted glyph anchors/directions between halo/fill
passes. `LabelText.vert.glsl` fetches those results. Point labels keep their existing rendering path.

Placement and fading remain camera-local background work. The worker uses matching smooth tangents for
collision footprints. Renderer packet caching ignores pose-specific fitted anchors only for GPU paths, retaining
immutable source inputs until content, selection, or fades change. Per-packet output images and explicit GPU
barriers preserve camera/context isolation and frame ordering. No per-label path work is introduced into cull.

The single-camera comparison and motion/readback validation are recorded in the performance report. For 1,024
17-vertex roads, completed-frame medians were 0.194 ms -> 0.201 ms stationary and 0.188 ms -> 0.203 ms during zoom.
The rapid zoom tracking test reduced maximum displayed-anchor lag from 13.35 px to below 0.00014 px. Keep the
CPU override available while evaluating real-data motion and longer-path costs; the prototype does not solve tile-fragment
transitions, road repetition/stitching, or strict per-frame collision guarantees.

## TextSymbol halos and feathering (implemented)

Prestige consumes the existing Style/TextSymbol parser for constant text size, fill, halo color/offset and
outline/none backdrops. CSS and structured symbols work. Class/tile selectors remain Prestige metadata.
The demo now uses text CSS with explicit pixel halo widths, crisp places, and moderately feathered roads.

The preferred Prestige control is stylesheet `text-halo-softness` (0-1); the XML `halo_softness` attribute
remains a fallback, with CSS taking precedence. It preserves outer bounds and per-camera fades,
uses cached distances for small text, and retains a soft analytic fallback. Crisp packets and all fills
use the lean shader. The cached comparison is 0.239 versus 0.240 ms for 1,024 labels; forced cache-miss soft
fallback is much slower (3.944 ms) and needs further performance work. See the halo design and performance
reports for semantics, limitations, full validation, and reproducible transient artifacts.

## Measurement and behavior matrix

| Scenario | Main evidence |
| --- | --- |
| Stationary dense view | Layout converges and sleeps; no recurring shaping or full layout work |
| Continuous orbit/pan/zoom | Current anchors remain attached; useful results publish despite changing poses |
| Camera jump or resize | Incompatible placements disappear; time to fresh labels is measured |
| Two or more independent cameras | Different placements, fair worker service, no shared fade/history |
| Shared versus separate GL contexts | Correct resources, readiness, and retirement independent of view state |
| Picking during transitions | Hits agree with the displayed frame and its source viewport |
| Tiled global travel | Stable ownership, duplicate suppression, bounded loading and memory |
| High-priority dynamic updates | Bounded admission latency without cull stalls or unfair scheduling |
| Dense callouts | Bounded refinement cost, measured settling, no endless movement |
| Multilingual text and high DPI | Shaping, fallback, metrics, halos, and visual quality |
| Repeated teardown and recreation | No stale publications, reused identities, or accumulated resources |

Record total frame costs as well as isolated label costs. Report worker CPU separately so moving work off cull
cannot be mistaken for reducing total computation. Compare display density and visual correctness alongside
speed; silently dropping more labels is not a performance improvement. For new algorithms, validate invariants
against a simple deterministic reference; for optimized existing helpers, validate unchanged behavior directly.

## Deferred work

Road fragment stitching and repeated placement, sophisticated polygon placement, rich text/color glyph coverage,
optional compatibility adapters, and GPU placement are follow-on candidates. A production synchronous solver or strict every-frame
collision guarantee would be a distinct policy with separate costs, not an implicit fallback. Resolve the
foundational batching, asynchronous scheduling, and multi-camera contracts first.


### Pixel-width and halo calibration follow-up

Implemented `text-halo-width` in the Prestige rule adapter with explicit-width precedence and strict
constant-pixel validation. Demo rules use 2px halos: crisp places and 0.5-soft roads. Corrected a factor-of-two
SDF decoding error that halved cached halo radii. The single-camera benchmark and framebuffer comparisons
are recorded in the performance report; no additional draw work or atlas memory was introduced.


### Fade blend-state correction

Protected Prestige's premultiplied-alpha BlendFunc from VisibleLayer's straight-alpha override. This
fixes dark halo/fill transitions and antialiased fringes without changing shaders or per-camera fade
history. Offscreen regression captures cover inherited state, fade endpoints/intermediate values,
layer opacity, crisp/soft halos and analytic fallback. See label-engine-halos.md and the performance
report for the verified results. No interactive viewer launch is required for these checks.


## Public layer and IDE organization

The feature layer is now the exported `osgEarthPrestige::LabelLayer`, with an installed `LabelLayer`
header, typed `Options`, ordered `StyleRule` values, and ordinary source/lifecycle methods. The existing
`prestige:labels` factory/earth-file syntax is retained. The rendering and worker implementation stays
private. See [the public API guide](label-layer.md) for configuration and availability details.
The public ABI version advances to 209. Prestige CMake now passes all label files through the normal
library helper, keeping Visual Studio sources, headers, shaders and templates in their matching groups.

## Public scene-graph LabelNode (implemented)

`osgEarthPrestige::LabelNode` binds explicitly to a `LabelLayer`, which now accepts an optional feature
source. Literal text, Style/Config styling, local anchor and priority feed the existing dynamic chunks.
Tiled labels and visible nodes enter the same camera-specific solver and renderer. Parent transforms,
traversal masks/LOD, removal/re-addition, independent camera clocks, and layer reopen are covered by
transient regression tests. Nodes share the layer font; use GeoTransform for geographic positioning.

One bounded collector per layer captures per-camera scene membership. It consumes the prior completed
camera frame, avoiding dependence on traversal child order. Unchanged nodes do not resubmit content;
shaping and changed-chunk preparation remain on workers. Continuous motion accepts completed content
generations in sequence rather than starving on a perpetually newer update, while camera-membership
checks reject stale visibility. The existing camera-local fade/history mechanism remains authoritative.

See [the public API guide](label-layer.md#scene-graph-labelnode) for lifetime, culling, capacity and style
contracts. Public ABI is 210. The single-camera frame/admission measurements are recorded in the
performance report; transient harnesses live under `build/label-node`.

## LabelNode and styling TODO

- [ ] Icon support in LabelNode.
- [x] Small ImGui appearance editor for existing LabelLayer rules, with live preview and Copy styles XML.
  Selector/font/shared-style editing remains future work.
- [ ] Explore shared label styles.
- [ ] Explore importing or adopting a new style specification.

Sluggit/stroke-rendering investigation remains deferred.


## Runtime layer styles and small editor (2026-11-09)

Implemented `LabelLayer::setStyle`, synchronized rule snapshots, constant appearance resolution and
CSS/structured appearance overlays. An existing rule's size, fill and halo values update in place;
selectors, font, rule count and node-local styles retain their existing configuration paths.

The tile worker coalesces edits, replaces only affected immutable batches and reuses shaped runs/atlases.
Size/width changes also refresh point/road bounds. Source queries, camera registration, placement
preferences and fade timelines are retained. ImGui adds Prestige > Label styles with throttled live
preview, Apply, Revert, refresh and Copy styles XML. Public ABI is 212.

Build/install, focused camera/style/road regressions, backend-disabled public linkage and interactive
OpenFreeMap editing checks pass. One-camera completed-frame medians for 1,024 labels: 89.87 us before,
88.93 us after at rest, 109.69 us during color updates every ten frames. See the runtime-style section
of `label-engine-performance.md` for scope, worker rebuild comparisons and transient artifacts.


### Runtime levels and spacing follow-up

The ImGuiLTable editor includes rule Min/Max level (inclusive source-tile levels 0-16) and layer-wide
Declutter padding in logical pixels per side. `setStyle` has an appearance/level overload;
`setDeclutterPadding` posts new solves without retiring view registrations. Invalid values leave the
accepted configuration intact. Level edits reload affected tile levels lazily, retaining old coverage
through I/O and retry failures. Revert covers appearance, levels and padding; styles XML includes rule
levels, while padding remains a layer option. Public ABI is 213. Validation and frame measurements are
recorded in the performance report.
