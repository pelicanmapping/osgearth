# Resource lifetime tracing

Set `OSGEARTH_RESOURCE_TRACE` to an output JSONL filename before starting the
application. The parent directory must exist. The file is overwritten on launch;
use a different name for each capture. Exit the application normally to flush it.
Leave this variable unset for normal performance measurements.

On Windows, from a command prompt in the repository:

```bat
call osgearth_shell.bat
set "OSGEARTH_RESOURCE_TRACE=%CD%\resource-lifetime.jsonl"
```

Launch the rebuilt application from your configured `CMAKE_INSTALL_PREFIX` under
Superluminal. Use the same earth file, camera path, and `--novsync` arguments for
comparative captures. Set the environment variable in Superluminal's launch
configuration, or start Superluminal from this configured shell. Ensure that the executable and
DLLs on `PATH` come from the same build; the configured installation prefix can
differ from the repository's `install` directory.

The existing Superluminal API integration emits `resource.*` events with resource
metadata as event data. No additional profiler integration or loader is needed.
The CMake finder forwards to the installed SDK's own finder; a relocated SDK can
be selected with `SuperluminalAPI_ROOT`. Without the SDK, the JSONL trace still
works; its header reports `superluminal: false`.

## Reading a capture

Select a long `resource.swap` scope, then inspect deletion scopes and the
preceding resource history on all graphics contexts, including the compiler
context. Swap scopes are installed by `CustomRealizeOperation` and preserve an
existing swap callback. Applications using a different realize operation must
arrange their own swap timing.

- `resource.create`: osgEarth starts tracking a GL object.
- `resource.borrow_osg_texture`: osgEarth wraps an OSG-owned texture name. This is
  a wrapper lifetime, not creation of new texture storage.
- `resource.label`: associates a category and logical owner with the resource.
  Terrain imagery uses tile/layer names; elevation now includes its tile key.
- `resource.buffer_storage` / `resource.texture_storage`: metadata snapshot after
  storage setup; these are markers, not durations of the allocation calls.
- `resource.recycle`: an existing object is reused. Its resource ID stays the
  same and its recycling count increases; the next label identifies its new use.
- `resource.texture_detach`: a Texture drops its reference without immediate GL
  deletion. Other references or the pool can keep the resource alive.
- `resource.orphan`: the pool first starts aging an unreferenced object.
- `resource.pool_delete`: pool eviction, containing the actual release scope.
- `resource.texture_release_immediate`: a Texture requests immediate GL release.
- `resource.delete_texture` / `resource.delete_buffer`: CPU duration of the
  release, including residency teardown and the GL deletion call.
- `resource.ico_compile_and_flush`: OSG incremental compilation and deletion
  flushing as a batch, including OSG-owned buffers/textures outside these wrappers.

JSONL contains paired `B`/`E` records with `span`, `seq`, `tick`, and `thread`.
`trace_span` also appears in Superluminal event data to join the two outputs.
Multiply a tick difference by the header's `tick_seconds` to obtain seconds.
On Windows this is the native QPC clock. `resource` distinguishes wrapper
lifetimes even when GL names or C++ addresses are reused. Records snapshot
metadata before deletion, so the end record keeps the original GL name.

`context` is the executing context ID when available, otherwise -1;
`creator_context` is the resource's OSG context/share-group ID. Frame numbers are
provided on swap events when a FrameStamp is available; -1 means unknown.
Texture profile fields include dimensions, format and mip count. Wrapped OSG
textures can have unknown size/profile data. Sizes are wrapper bookkeeping,
not driver allocation sizes. Labels can contain paths or dataset names.

The trace adds CPU work, allocations, a short global logging lock and buffered
file writes. It is diagnostic instrumentation, not an overhead-free benchmark.
It does not add GL queries, fences, `glFinish`, or change pool deletion/reuse policy.

## Attribution limits

A slow deletion scope identifies the GL resource involved directly. A slow swap
may process cleanup deferred by the driver from earlier releases; adjacent
release events are candidates, not proof of a particular Windows allocation's
identity. GL names do not expose `DxgKrnl` allocation handles. Resources deleted
solely inside OSG's own object managers have batch timing only; exact attribution
there requires corresponding instrumentation in OSG. Do not interpret wrapper
detachment as deletion of an OSG-owned texture.

## GL debug labels

`--gl-debug` (also `--gldebug`) enables GL debug output and causes calls to
`GLObject::debugLabel` to submit object labels to OpenGL. It does not automatically
name every GL object or add resource names to Superluminal sampled CPU stacks.
The labels are stored by osgEarth even without the option, so this trace can use
them independently. The option also enables synchronous debug output; leave it
off for comparable timing unless investigating GL diagnostics.

## Frame-thread affinity experiment

`osgearth_imgui` now disables OSG's automatic affinity configuration before
viewer realization and supplies an empty affinity mask. This allows the frame
thread to run on the process's allowed CPUs instead of being pinned to CPU 0.
It uses OSG's public API and requires no OSG dependency changes. Rendering stays
single-threaded. Other applications are unchanged; no osgEarth job-pool affinity
or worker-priority changes were made.

Repeat the same scene and camera motion with `--novsync`, resource tracing, and
Killer Prioritization Engine still disabled. Compare ready-to-run delays and
frame-time tails. The 17:10:26 capture on November 5 verified execution across
all 24 logical CPUs: maximum Ready-state delay fell from 42.507 to 2.375 ms.
The largest ordinary swap-return interval fell from 77.103 to 15.882 ms, with
zero intervals above 16.67 ms after excluding final window-close interaction.
Camera workloads were not controlled, so this validates the scheduling change
without establishing a general throughput improvement.
A live Windows affinity query against the installed application verified the
before/after masks: `0x1` (CPU 0) to `0xFFFFFF` (all 24 logical CPUs allowed by
this process). Both smoke runs opened the viewer and exited normally.

## Investigation action list

- **Next: evaluate recycling and reuse, starting with TileDrawable KdTree refitting.**
  Keep the completed retirement fixes as the baseline. Validate triangle coverage,
  intersection behavior and reader synchronization before reusing tree storage.
- **Deferred: buffer upload/allocation investigation.** Revisit the native OSG
  buffer identity and Chonk compile costs after the recycling work. Preserve the
  capture evidence below; this work remains open but is not the next priority.
- **REX retirement validated in the 11:25:00 capture; larger hitches remain.**
  No sampled KdTree destruction ran on Main; 971 instances ran on `oe.dispose`.
  GeometryPool traversal peaked at 0.122 ms. Of 243 sampled SharedGeometry
  destructions, 240 ran on the worker; three remaining Main instances were small
  (maximum 0.174 ms), with retained DrawTileCommand ownership identified in
  `LayerDrawableNVGL::refreshRenderState`. Track that remaining owner separately.
  The worst frame was 510.432 ms (503.562 ms waiting); UI inspection confirms long
  **Preemption** intervals with unknown preempting/unblocking threads. A neighboring
  242.964 ms frame contains a 182.773 ms driver allocation-destruction wait inside
  swap. The disposal worker executed only 3.805 ms during this 1.073 s episode.
  Resolve system-wide scheduling and driver retirement before attributing the
  observed worse hitches to CPU offloading. Keep new KdTree construction open:
  a later 23.704 ms frame has 15.025 ms of heap wait during tree allocation.
  Chonk compile reaches 20.711 ms (182 buffer-storage events in that frame), and
  native OSG `glBufferSubData` reaches 9.665 ms. Keep both buffer investigations
  and Prestige asphalt sharing (271 post-startup storage events) on the list.
  Report: `D:/Captures/2026-11-06_11-25-00_analysis.txt`.
- **Investigate Prestige asphalt sharing.** This texture is intended to load once and be
  reused across tiles. The November 6, 09:52:49 capture records 466 texture-storage
  events with `Prestige asphalt` labels during the measured interval. Trace their source
  images and ownership to determine whether these are expected per-tile generated
  resources or redundant allocations of the intended shared texture. Resource 4758
  allocated storage once, then recycled for two other tiles without reallocating storage.
- Affinity experiment validated in the November 5, 17:10:26 capture; retain the
  change and use a fixed camera route for controlled frame-time comparisons.
- **Still open: identify the geometry/buffer behind the 7.9 ms upload** in the November 5,
  17:10:26 capture (7.755-7.771 s frame). Follow `CompileDrawableOp::compile` through
  `osg::Geometry::compileGLObjects` and `osg::GLBufferObject::compileBuffer` into the
  NVIDIA allocation/upload path. Tile destruction offloading does not address this cost.
- Capture a reproducible hitch with this trace and correlate release lifetimes
  with the long swap/driver cleanup interval.
- Use the identified resources to evaluate reuse and GPU-completion-aware
  retirement; do not move GL deletion to a worker without a valid shared context
  and correct synchronization.
- November 6, 09:20:13 capture validates the shared retirement path: all 1,665
  `Destroy retired objects` scopes ran on `oe.dispose`, including a 57.823 ms batch.
  Sampled PagedNode2 and 3D Tiles unload calls on Main peaked at 0.146 and 0.122 ms.
- **ICO ownership retirement observed on the worker in the November 6, 09:52:49 capture.**
  All sampled `ICOCallback` destruction ran on `oe.dispose`; the worker stack shows
  ObjectReleaser releasing the plain CompileSet carrier and its callback. The retiring
  subclass on Main peaked at 0.150 ms. All 5,377 `Destroy retired objects` scopes ran
  on the worker; the previous long Main-thread callback cleanup did not recur.
  This confirms thread placement, but the same heavy late-completion payload was not
  reproduced. Stock OSG compatibility is also covered by the transient validation below.
- **TextureArena orphan retirement validated in the 10:29:14 capture.** The
  09:52:49 capture's 11.328 ms CPU texture/image cleanup on Main now uses the shared
  ObjectReleaser with label `TextureArena`. Update and flush retain the outgoing owner
  before detaching, then submit after unlocking. Automatic expiry uses the advisory
  queue gate; explicit flush detaches immediately. Existing GL release routing stays
  on the caller. Arena teardown drains its submitted batches.
  Main's sampled purge peaks at 0.290 ms; all 503 sampled TextureArena-holder
  destructions run on `oe.dispose` (maximum 3.777 ms). The new worker stack explicitly
  reaches Texture/Image destruction. A separate 18.440 ms ICO callback cleanup also
  runs on the worker. All 6,558 disposal scopes are on `oe.dispose`.
  Trace ordering implicated `Prestige road surfaces (raster)` tile `17/16092/49994`,
  resource 2725 / GL name 1009; CPU image identity was inferred, not directly recorded.
- **GLObjectPool in-place compaction validated in the 10:51:50 capture.**
  In the 10:29:14 capture, Main waited 19.970 ms for the Windows heap lock at
  `releaseOrphans()`'s `keepers.reserve(_objects.size())`. The per-pass temporary
  vector is now removed. Survivors swap into the existing vector's front, preserving
  their order; removed references stay alive until the scan ends, then the vector
  shrinks without releasing its capacity. This preserves orphan ownership decisions,
  deletion delays, soft byte budgets and recycling. Other allocation/free sites can
  still contend with worker image cleanup; this targets the observed reserve call.
- The 10:51:50 capture confirms pool servicing peaks at 1.511 ms with no recorded
  wait in that longest sampled call; the previous keeper-vector allocation is gone.
  After excluding the first 5 seconds and a final Windows nonclient mouse event,
  p99 is 11.465 ms and the worst frame is 35.618 ms. That frame spends 30.126 ms
  in cull traversal, including 21.540 ms waiting; terrain KdTree build/replacement
  still allocates/frees on Main and encounters heap-lock contention.
- **New retirement owner: REX GeometryPool.** Cache eviction drops the last
  SharedGeometry reference on Main under its map mutex. In a 25.094 ms frame,
  freeing its CPU GL4Vertex vector takes 15.087 ms (14.808 ms heap-lock wait).
  Implemented: retain outgoing geometry for ObjectReleaser and submit after
  unlocking, preserving existing GL ownership. Sweeps use the advisory admission
  gate; explicit clear bypasses it. Validated in the 11:25:00 capture above. This is
  separate from the fixed GLObjectPool vector.
- Current remaining upload maximum after startup is 5.796 ms in native OSG
  `glBufferSubData`; exact buffer/asset identity remains open. The matching trace
  records 318 `Prestige asphalt` storage events (all 512x512 R8) after startup,
  so the intended-sharing investigation remains open. An isolated 20.876 ms
  frame has an unresolved draw/state wait and remains a secondary lead.
  Report: `D:/Captures/2026-11-06_10-51-50_analysis.txt`.
- **Still open: new KdTree construction on Main.** Replaced-tree retirement and
  duplicate initial construction are now addressed (validation below). The 10:29:14
  capture's worst
  frame is 30.264 ms, with 22.916 ms in cull traversal including 11.869 ms waiting.
  Child creation calls `TileDrawable::setElevationRaster`, building/replacing KdTrees
  and allocating/freeing memory on Main. Preserve intersection behavior when moving
  or reusing this work. No frames exceeded 33.333 ms; p99 was 15.019 ms.
- The 10:29:14 capture adds a 16.207 ms native OSG `glBufferSubData` call and a
  25.088 ms Chonk update to the upload/compile investigation. Its current JSONL
  belongs to a different viewer thread/run, so no resource identities or asphalt
  allocation counts are attributed from that log. Preserve the matching JSONL before
  restarting the viewer. Report: `D:/Captures/2026-11-06_10-29-14_analysis.txt`.
- **Buffer upload/allocation investigation remains open.** The 09:52:49 capture has
  a 12.558 ms native OSG buffer compile in a 24.132 ms frame, dominated by
  `glBufferSubData`, and an 11.829 ms compile around 21.888 s dominated by
  `glBufferData` and driver memory decommit. Native OSG buffer/asset identity still
  needs instrumentation. Separately, `Prestige asphalt` tile `15/4010/12490`,
  resource 4758 / GL name 348, takes 14.126 ms in texture storage (512x512 R8),
  including 12.954 ms of driver `VirtualFree`. CPU retirement does not cover that.
  Capture report: `D:/Captures/2026-11-06_09-52-49_analysis.txt`.
- The same capture still has a 13.252 ms OSG `glBufferSubData` call around 3.170 s.
  Its exact OSG buffer/asset identity remains uninstrumented. A separate Chonk compile
  around 4.637 s takes 16.102 ms, including 4.307 ms of instance-vector growth.
  Its page VBO is resource 3225 / GL name 2353 / 14,021,280 bytes; its input instance
  buffer is resource 3229 / GL name 2631 / 6,676,480 bytes. Keep both upload paths open.
- The November 5, 21:35:36 capture adds two upload leads: OSG's unlabelled
  `glBufferSubData` path took 16.388 ms around 2.992 s; Chonk VBO resource 6025
  (GL name 236, `Chonk geometry:VBO Chonk geometry page`, 1,275,560 bytes) spent
  10.655 ms in buffer storage around 17.180 s, mostly driver memory decommit.
  Trace these to their source geometry/assets; the original 7.9 ms upload remains open.

## Shared CPU object retirement

`Registry::getObjectReleaser()` owns one core `ObjectReleaser` backed by the existing
jobs system (`oe.dispose`). A pager gets a copyable `Client` handle for submission,
admission checks, and owner-local teardown draining. Registry drains the service
before shutting down jobs. The service does not replace OSG's global delete handler
or intercept arbitrary `osg::Referenced` destruction.

- REX's unloader uses a client instead of its former private TileReleaser worker.
- PagingManager batches normal PagedNode2 expiry and tracker-only orphans. Existing
  visitor-based manager discovery propagates this behavior to layers without new
  per-layer plumbing. Virtual `unload()` overrides still run. Explicit PagedNode2
  unloads use their manager's handle, or a service handle for standalone nodes.
- ThreeDTilesetNode has its own client, since 3D Tiles does not use PagedNode2.
  It retires both detached content and the future that retains the loaded graph.
  Request cancellation also retains the old future across reset to close the
  completion/reset race.
- Custom pagers can obtain a client from Registry, retain outgoing references,
  detach their live graph links on update, then submit the retained batch.
  Their destructors must be safe on a worker, and their owner must call `drain()`
  with producers stopped before destroying services used by those objects.

Automatic expiry checks an advisory limit of two outstanding batches per client
and 64 across the service. It postpones expiry when cleanup is busy instead of
joining the worker. Concurrent producers can cross the advisory threshold;
explicit unload/cancel operations can also exceed it to detach immediately. These
are batch limits, not a byte or resident-memory cap. Every accepted batch retains
its references until disposal. Teardown drains may block; normal expiry does not.
A nested paging owner destroyed on the disposal worker drains queued work inline
instead of waiting on its own active batch.

Retaining a future handle avoids dropping a just-completed result on the frame
thread. It delays reference-count-based cooperative cancellation until worker
release; PagedNode2 revision invalidation is still immediate. This does not wait
for unfinished IO/GL compilation or control arbitrary external owners. Compiler-created
ICO sets now cover the late GL-completion ownership path described below. Actual GL deletion stays with the existing
context-aware resource managers. In particular, PagedNode2 and 3D Tiles do not
call `releaseGLObjects()` on shared live material state.

The public ABI version is 205 (TextureArena now holds a retirement client). Rebuild dependent applications/plugins with the
new headers and library.

## ICO ownership retirement (November 6)

All three CompileSet creation sites in GLObjectsCompiler now use a private
`RetiringCompileSet` subclass. It works with an unmodified OSG ICO: final reference
release is the trigger, independent of callbacks returning or ICO invocations
finishing. Other contexts, temporary compile lists, and external holders keep the
set alive until they release it normally.

The subclass preallocates an empty stock CompileSet as a retirement carrier. Its
final destructor swaps the subgraph, completion callback, per-context compile map,
and marker into that carrier, leaving the original base members empty before
submitting to ObjectReleaser. It never resurrects or submits the dying object.
Pending compile operations and the callback's node/future are therefore covered,
including sets removed without completing and sets discarded during ICO teardown.
The map's context keys are non-owning; retirement does not dereference them.

GL compilation and promise/continuation publication keep their existing thread and
ordering. CPU reference release runs on the existing disposal worker, with the
Superluminal label `ICO compile set`; there are no worker GL calls or new worker
pools. The callback-based `totalJobs()` counter reaches zero when the callback is
actually retired, so it can briefly include completed sets awaiting disposal.
Registry shutdown drains accepted retirement batches; a set released after service
shutdown follows ObjectReleaser's existing synchronous terminal-teardown behavior.
External owners and OSG's global DeleteHandler can still affect final destruction.
Arbitrary stock-OSG-created CompileSets are not intercepted. The private ICO change
needed no ABI bump; the subsequent TextureArena client changes the public layout (ABI 205).

Transient validation used real pbuffer contexts and actual stock ICO operator
calls, plus osgEarth's customized ICO. It covered all four public compiler entry
points, successful publication with a live consumer, abandoned futures, removal of
pending sets (including compile-map-only drawable ownership, subgraph and marker),
sequential and concurrent contexts, an extra temporary CompileSet owner, stock ICO
teardown, callback job accounting, and absence of worker GL compilation/release.
The targeted existing `[pagednode],[texturearena]` tests passed 268 assertions in
19 cases. Temporary benchmark/test harness sources were removed afterward.
The final `build.bat` build/install passed, and the installed osgEarth DLL's SHA-256
matched the tested DLL. A viewer launch/close smoke check passed with the frame
thread still allowed on all 24 CPUs (`0xFFFFFF`).

A Google Benchmark compared identical actual compiler callbacks/payloads in a stock
CompileSet versus the new retiring subclass. Each graph contained independent 65x65
geometry/KdTree meshes. Compilation used a probe drawable to isolate CPU lifetime
cost from real buffer uploads. The measured caller interval includes context
binding, stock ICO execution/completion, and context release; setup and waiting for
the disposal worker were excluded. Medians of three repetitions of ten iterations,
RelWithDebInfo, resource tracing disabled:

| Meshes | Original stock-set caller time | Retiring-set caller time |
| --- | ---: | ---: |
| 64 | 3.344 ms | 0.213 ms |
| 256 | 9.466 ms | 0.346 ms |

These measurements establish reduced caller blocking, not reduced total destruction
work or an application frame-rate guarantee. Raw results are preserved at
`D:/Captures/ico-retirement-benchmarks.json`. The buffer-upload investigation remains
on the action list; this change does not remove GL upload or Chonk instance-building
costs.

## Shared-service validation (November 6)

The final `build.bat` build/install succeeded. The existing PagedNode tests were
registered in the test target; `[pagednode],[texturearena]` passed 268 assertions
in 19 cases. A viewer launch/close smoke check passed and confirmed the frame-thread
mask remains `0xFFFFFF` (all 24 permitted CPUs).

A transient Google Benchmark used identical 65x65 geometry/KdTree meshes and the
actual PagedNode2/3D Tiles unload paths, comparing against their original synchronous
bodies. Setup was excluded from manual caller timing. Results below are medians of
three repetitions of ten iterations, RelWithDebInfo, resource tracing disabled.
The saved harness was rerun against the final installed osgEarth DLL after removing
all transient sources and access hooks from the repository.

| Unload path | Meshes | Original caller | Offloaded caller | Offloaded completion |
| --- | ---: | ---: | ---: | ---: |
| PagedNode2 | 64 | 3.200 ms | 0.023 ms | 2.786 ms |
| PagedNode2 | 256 | 9.967 ms | 0.024 ms | 8.288 ms |
| 3D Tiles | 64 | 0.439 ms | 0.019 ms | 0.837 ms |
| 3D Tiles | 256 | 7.612 ms | 0.022 ms | 6.589 ms |

Completion includes waiting for the worker; it is not worker CPU time. Allocator
state and cache effects affect these figures. This demonstrates reduced caller
blocking, not reduced total destruction work or guaranteed application frame times.

Behavioral checks passed for destruction thread/count, graph detachment, absence
of explicit shared-state GL release, future retention through reset/completion,
completed-but-unmerged results, the two-batch admission gate, owner-local draining,
nested PagingManager destruction on the disposal worker, tracker-only orphans,
virtual unload overrides, normal expiry in both pagers, shutdown drain, and terminal
synchronous release after shutdown. Existing paging tests also cover stale loads,
reload generations, concurrent first culls, and manager replacement.

Raw final results: `D:/Captures/object-retirement-final-benchmarks.json`.
The temporary harness and logs are retained outside the repository for investigation.
Use the next controlled `--novsync` capture to verify the real 20-27 ms disposal
hitches moved off the frame thread; keep the buffer-upload investigation open.

## Terrain retirement details and earlier validation

REX retains strong references while collecting expired tiles, including registry-only
orphans. During update traversal it detaches the expired subtrees, clears their weak
parent links, and makes the existing null-state GL-release requests. Clearing descendant
links on update prevents the cleanup worker from modifying graph parent lists or
invalidating parent pointers still used by registered descendants.

The retained references now go to the shared `ObjectReleaser` with the `REX terrain`
client label. The earlier terrain-only implementation used `oe.rex.destroy`.
Existing per-frame expiry limits still apply, and a subtree can contain additional
descendants. Engine teardown drains its submitted releases before destroying engine
services. Normal frame traversal never joins the worker.

Superluminal scopes use the existing API integration:

- `Retire expired tiles`: collection, graph detachment and handoff on update.
- `Destroy retired objects` (`REX terrain`): reference release and CPU teardown on a worker
  (formerly `Destroy expired tiles`).

Actual GL deletion remains with the existing context-aware resource managers. Other
owners can keep resources alive after the worker drops its references; this does not
promise that every shared resource's final release occurs on that worker. OSG's global
deferred DeleteHandler, if enabled by another application/threading mode, is respected
and can move actual destruction to its own flush. The current single-threaded
`osgearth_imgui` configuration does not install that handler.

A transient Google Benchmark compared the original synchronous `removeSubTiles` body
against the new implementation on identical complete four-child TileNode trees. Each
leaf owned a 65x65 OSG geometry and a built KdTree. Setup and cleanup waiting were outside
the manual caller-time measurement; the asynchronous measurement also included checks
that detached nodes had no children or parent links. Results are medians of three runs
of ten iterations, RelWithDebInfo, resource JSONL tracing disabled:

| Mesh-bearing leaf tiles | Synchronous caller time | Offloaded caller time | Offloaded completion latency |
| --- | ---: | ---: | ---: |
| 4 | 0.166 ms | 0.005 ms | 0.192 ms |
| 64 | 3.134 ms | 0.053 ms | 2.035 ms |
| 256 | 9.241 ms | 0.193 ms | 8.685 ms |

Completion latency includes waiting for background destruction; it is not worker CPU
time. These isolated results demonstrate reduced caller blocking, not an application
FPS guarantee or elimination of allocator contention. The next interactive capture
must verify the real scene. Keep raw benchmark results alongside the capture
artifacts when repeating this comparison.

Transient checks verified exact destruction counts, worker versus caller thread,
update-thread GL-release requests, registry-only orphan expiry, outstanding future
references, external owners, the two-batch admission limit, and shutdown draining.
The validation sources were removed from the repository after running.

The earlier terrain-only `build.bat` install completed successfully. A viewer smoke run opened and
closed normally and confirmed that the frame-thread affinity remains `0xFFFFFF`.
The installed REX plugin contains the two new profiling scopes and no transient
validation exports.

Existing-suite baseline comparison: temporarily restoring only the pre-change REX
sources reproduced the same glTF test access violation and the same four ordinary
failures. With hidden worker tests and the crashing `glTF converts channels and
factors to shared PBR materials` case excluded, both baseline and final builds had
172 cases (168 passed, 4 failed) and 5,678 assertions (5,674 passed, 4 failed).
Failures were in Chonk page state validation, Chonk PBR conversion, glTF default
materials, and the PBR rendering subprocess. These remain separate existing issues;
the suite is not wholly passing. The tile-offload benchmark's behavioral checks passed.

## Validation

The GPU suite plus a temporary lifetime/callback test passed 222 assertions in
12 cases with tracing enabled. The temporary test also passed its 9 assertions
with tracing disabled. The resulting JSONL had 200 balanced scopes; validation
checked sequential records, resource identity through recycling, retained GL
names at deletion, escaped labels and single swap delegation. Temporary test
changes were removed afterward.

The user's subsequent November 5 captures verified native Superluminal resource
events end to end, with matching JSONL swap timings. This environment's own
SuperluminalCmd recording attempt required Administrator rights.

## TextureArena CPU retirement (November 6)

Orphan purging retains the final Texture::Ptr before changing the arena. It zeros
handles, removes indices/dynamic membership, clears the texture host, and requests
GL release through the existing route. CPU reference release is submitted after
unlocking, so image destruction and its heap decommit run on `oe.dispose` under
`Destroy retired objects` with the label `TextureArena`. Even the synchronous
stopped-service fallback runs outside the arena mutex. External texture/image
owners can still delay final destruction beyond worker release.

Automatic update skips expiry when the shared service or client is busy; explicit
flush bypasses the advisory queue gate and immediately detaches unused slots.
Teardown requires producers stopped and drains this arena's accepted batches.
Remaining live textures at arena destruction keep their existing teardown behavior.
This change does not move GL calls to the worker or address driver decommit during
new texture/buffer storage allocation.

A transient Google Benchmark measured the actual public update/flush paths before
and after the change, using touched RGBA image allocations. Results below are
median caller times from three runs of ten iterations; setup, arena destruction,
and retirement draining are outside the timed interval. Resource tracing was off.

| Operation | Images | Before | After |
| --- | --- | ---: | ---: |
| update | 8 x 512x512 | 0.275 ms | 0.007 ms |
| update | 8 x 2048x2048 | 3.903 ms | 0.017 ms |
| flush | 64 x 1024x1024 | 8.730 ms | 0.052 ms |

Every iteration checked empty slots, exact image destruction counts, and caller-side
GL-release requests. Baseline image destruction occurred on the caller; all upgraded
benchmark image destruction occurred off the caller. These measure caller blocking,
not total cleanup work, application FPS, or allocator contention under a real scene.
Separate transient checks passed for auto-release disabled, external owners, slot
reuse, the two-batch admission gate, explicit flush while busy, destructor draining,
reentrant image cleanup outside the mutex, and stopped-service fallback. Existing
`[pagednode],[texturearena]` tests passed 268 assertions in 19 cases.
Raw results and validation logs are `D:/Captures/texture-retirement-*`.
Transient benchmark/validation code was removed after use. Confirm real-scene
thread placement and remaining frame hitches in the next capture.

The final `build.bat` install succeeded. The installed osgEarth DLL hash matches
the validated DLL after transient-source removal. The viewer smoke test opened
and exited normally with frame-thread affinity `0xFFFFFF` (all 24 logical CPUs).

## GLObjectPool in-place compaction (November 6)

`releaseOrphans()` no longer creates/reserves a temporary keeper vector each frame.
It swaps survivors into a stable prefix of `_objects`, then shrinks the vector while
retaining capacity. Removed owners stay in the scanned tail until all orphan checks
finish: overwriting them early could destroy an object that owns a later entry and
change that entry's reference count during the pass. GL release order, orphan aging,
soft byte-budget overshoot, survivor order, total bytes, and recycling stay intact.
No class layout or public ABI changed (ABI remains 205).

Transient Google Benchmark measurements used the actual public pool cleanup call,
with setup outside the interval and live CPU-only GLObject probes. Medians of three
runs of twenty iterations, RelWithDebInfo, resource tracing/debug output disabled:

| Pool workload | Before | After |
| --- | ---: | ---: |
| 4,096 live objects | 0.013525 ms | 0.009590 ms |
| 32,768 live objects | 0.171955 ms | 0.124185 ms |
| 4,096 live objects; controlled Windows heap contention | 15.744700 ms | 0.052500 ms |

The contention case deliberately locks the CRT heap on another thread, signals the
caller to begin servicing, and unlocks after a requested 10 ms sleep (about 16 ms at
this machine's observed timer granularity). Thread creation/join are outside the
measured interval. This isolates the removed allocation's exposure to a busy heap;
it is not an application frame-time forecast. The original code replaced backing
storage on all twenty passes in each repetition; the new code replaced it zero times.
Tracing, GL deletion, other allocations and object destructors can still use the heap.

Transient behavioral checks passed before and after the change for invalid entries,
externally owned survivors, delayed orphans, byte-budget overshoot, zero-budget
aging, survivor/release order, recycling age reset, total-byte accounting, and an
invalid object that retains a later entry until the end of the scan. The upgraded
check also passed with resource tracing enabled (21 balanced scopes). Existing
`[pagednode],[texturearena]` tests passed 268 assertions in 19 cases.
Results: `D:/Captures/pool-compaction-before.json` and `pool-compaction-after.json`.
Transient benchmark/validation code was removed after use.

Next capture: verify the `keepers.reserve` allocation/wait has disappeared from
pool servicing. Keep buffer uploads, terrain/KdTree construction and replacement,
and Prestige asphalt sharing on the investigation action list. Reusing this vector
removes one demonstrated wait point; it does not eliminate all shared-heap contention.

The final `build.bat` install succeeded after benchmark-source removal; the installed
osgEarth DLL hash matches the tested DLL. The viewer opened and exited normally,
with frame-thread affinity still `0xFFFFFF`.


## REX KdTree and geometry retirement (November 6)

The existing Registry ObjectReleaser now accepts two more REX owners. Each
GeometryPool creates shared clients labelled `REX geometry pool` and
`REX replaced KdTrees`; TileDrawable copies the latter handle. There is no
per-tile queue or new worker. The existing `Destroy retired objects` SL scope
carries these labels on `oe.dispose`.

On a successful KdTree replacement, TileDrawable retains the outgoing shape
before `setShape`, publishes the newly built tree immediately, and submits the old
owner. This bypasses the advisory expiry gate so picking never waits for disposal
to catch up. Tree construction still happens synchronously. An external owner can
keep the retired tree alive beyond the worker release; the service is not a global
delete hook.

GeometryPool retains orphan SharedGeometry objects before erasing their map entries,
then submits the batch after unlocking. The existing admission gate defers automatic
expiry when busy; explicit `clear()` still detaches immediately and submits outside
the mutex. Its existing GL-release request remains on the caller; actual GL-object
pooling/deletion rules are unchanged. Pool teardown drains its submitted clients
with producers stopped. Scanning, map-node erasure, and batch submission remain on
the caller; the heavy geometry/vertex-array destructors move to the worker. This
does not remove every possible shared-heap allocation or free from the frame path.

SurfaceNode construction now initializes culling data from TileDrawable's already
built mesh/tree. A shared `updateCullingData()` helper preserves child-corner,
bounds, debug-box and horizon-point updates. Subsequent elevation updates still
update the drawable and then those caches. Initial tree builds drop from two to one.
These are internal REX classes; no public osgEarth ABI change (ABI remains 205).

Transient Google Benchmark probes called the actual installed REX plugin methods.
Medians of three repetitions of twelve iterations, RelWithDebInfo, tracing off;
setup, final fixture destruction and worker draining are outside caller timing:

| Workload | Before | After |
| --- | ---: | ---: |
| Sweep 8 orphan geometries, 16,384 GL4 vertices each | 0.305892 ms | 0.007067 ms |
| Sweep 8 orphan geometries, 65,536 GL4 vertices each | 1.185183 ms | 0.012358 ms |
| Construct 8 tile/surface pairs, 17x17 meshes | 0.391308 ms | 0.210258 ms |
| Construct 8 tile/surface pairs, 65x65 meshes | 7.032925 ms | 3.940417 ms |
| Replace 8 KdTrees, 65x65 meshes, uncontended | 2.702867 ms | 2.715442 ms |
| Replace 1 KdTree, 65x65 mesh, controlled destructor delay | 14.496917 ms | 0.388167 ms |

The last row deliberately sleeps in the outgoing-tree destructor for a requested
6 ms (about 14 ms observed timer granularity), before normal KdTree destruction.
It verifies caller isolation from slow cleanup, not a real-scene heap-contention
speedup. Total completion remains about 14.7 ms after offloading. The uncontended
replacement result is effectively unchanged because building the new tree dominates.
All measured retired trees and orphan geometries moved from caller destruction to
worker destruction. Total work is not eliminated; the next capture must establish
the effect on actual hitches.

Transient checks passed for immediate accelerated and primitive intersections after
elevation changes, bounding boxes/spheres, all 32 child corners, horizon visibility,
initial tree identity/build count, external tree and geometry owners, sweep/clear,
missing KdTree builder, GL_PATCHES, saturated queues, deferred automatic expiry,
and immediate replacement/clear while saturated. Existing `[pagednode],[texturearena]`
tests passed 268 assertions in 19 cases. Results and logs are in
`D:/Captures/rex-retirement-{before,after}.{json,txt}`, `rex-retirement-validation.txt`
and `rex-retirement-tests.txt`. The temporary benchmark/probe sources are removed
from the repository after validation.

Next capture: confirm replaced-tree and orphan-geometry destruction under the new
worker labels and examine the largest remaining Main-thread waits. Keep **new
KdTree construction**, **buffer upload identity/cost**, and **Prestige asphalt
sharing** on the action list. Whole-tile/PagedNode retirement remains enabled.

The final build.bat install succeeded after removing the transient sources and
exports. The installed REX plugin hash matches the final built plugin. The viewer
smoke test opened and closed successfully; frame-thread affinity remains
`0xFFFFFF` (all 24 logical CPUs allowed by the process).
