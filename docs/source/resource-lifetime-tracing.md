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
- Offload tile CPU destruction from the frame thread, including TileNode, geometry
  and KdTree teardown. Resource tracing does not implement that offloading.

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
