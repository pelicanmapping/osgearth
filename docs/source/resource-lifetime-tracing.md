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

- Affinity experiment validated in the November 5, 17:10:26 capture; retain the
  change and use a fixed camera route for controlled frame-time comparisons.
- **Next: identify the geometry/buffer behind the 7.9 ms upload** in the November 5,
  17:10:26 capture (7.755-7.771 s frame). Follow `CompileDrawableOp::compile` through
  `osg::Geometry::compileGLObjects` and `osg::GLBufferObject::compileBuffer` into the
  NVIDIA allocation/upload path. Tile destruction offloading does not address this cost.
- Capture a reproducible hitch with this trace and correlate release lifetimes
  with the long swap/driver cleanup interval.
- Use the identified resources to evaluate reuse and GPU-completion-aware
  retirement; do not move GL deletion to a worker without a valid shared context
  and correct synchronization.
- Tile CPU destruction offloading is implemented; verify in the next capture that
  `Destroy expired tiles` and the associated geometry/KdTree frees run on a worker,
  with short `Retire expired tiles` scopes on the frame thread.

## Tile CPU destruction offloading

REX retains strong references while collecting expired tiles, including registry-only
orphans. During update traversal it detaches the expired subtrees, clears their weak
parent links, and makes the existing null-state GL-release requests. Clearing descendant
links on update prevents the cleanup worker from modifying graph parent lists or
invalidating parent pointers still used by registered descendants.

The existing jobs system then releases the retained references on `oe.rex.destroy`.
There are at most two outstanding batches per unloader; expiry pauses when both slots
are occupied, without waiting on the frame thread. This bounds queued batches, not
bytes or total resident tiles: existing per-frame expiry limits still apply, and a
subtree can contain additional descendants. Engine teardown drains its outstanding
jobs before destroying engine services. Normal frame traversal never joins the worker.

Superluminal scopes use the existing API integration:

- `Retire expired tiles`: collection, graph detachment and handoff on update.
- `Destroy expired tiles`: reference release and CPU teardown on a worker.

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

The final `build.bat` install completed successfully. A viewer smoke run opened and
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
