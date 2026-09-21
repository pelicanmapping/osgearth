# SkyNode2 validation and performance

Measured September 21, 2026, with the installed RelWithDebInfo build on Windows,
NVIDIA GeForce RTX 5080, OpenGL 4.6, driver 610.60. Google Benchmark reports 24 CPU
threads at 2496 MHz. Results below are medians of three repetitions.
The frame timings below precede the subsequent horizon correction; its current
before/after measurements are in [the horizon report](sky2-horizon-validation.md).

## Atmospheric query before and after tabulation

Both paths evaluate the same Rayleigh/aerosol/ozone Beer-Lambert model over 128
representative rays. The reference integrates 128 intervals per ray; the optimized
path bilinearly samples the production transmittance table. Table construction is
outside the timed region. The GPU uses the same table parameterization.

| Implementation | Time per 128 queries | Time per query |
| --- | ---: | ---: |
| Reference integration | 185.424 us | 1448.6 ns |
| Production lookup | 5.805 us | 45.4 ns |

The lookup is **31.9x faster** in this CPU microbenchmark. Maximum absolute RGB
transmittance error on these rays is **0.0002836**. Separate tests compare 30 rays
spanning ground, altitude, horizon and planetary shadow against a 2048-interval
reference, enforcing a 0.015 absolute tolerance and energy bounds [0,1]. This is
an approximation with measured error, not bit-identical arithmetic. It is not a
31.9x whole-renderer speedup or a comparison against SimpleSky/Bruneton.

Raw results: [JSON](sky2-transmittance.json), [log](sky2-transmittance.log).

## Complete frame timing

The offscreen scene contains an empty globe terrain, a ground patch, five spheres
with varied metallic/roughness factors, the celestial background, and all required
atmosphere/environment/aerial LUT passes. There are no downloaded layers. Readback
is disabled during measurement, and eight warmup frames precede each run. A moving
camera changes position every frame, forcing all three atmosphere passes to update.

| Preset | Resolution | Stationary | Moving |
| --- | --- | ---: | ---: |
| Flat | 1920 x 1080 | 1.344 ms | 0.972 ms |
| Balanced | 1920 x 1080 | 0.890 ms | 1.021 ms |
| High | 1920 x 1080 | 0.927 ms | 1.011 ms |
| Balanced | 3840 x 2160 | — | 1.249 ms |
| High | 3840 x 2160 | — | 1.195 ms |

`GL_TIME_ELAPSED` surrounds the complete rendering traversal, so these intervals
include GPU submission gaps and ordinary scene rendering, not just sky shader
execution. They are not application FPS or a prediction for complex terrain and
model datasets. These short frames are sensitive to clock/submission variability;
the slower Flat stationary result does not imply greater shader cost. Shader compilation, startup table construction, and
first-use allocations are excluded. These completed runs report no shader/GL errors.

Still views and camera rotations reuse their LUTs; position or sunlight changes
update the sky, environment, and aerial LUTs. Scene
fragments perform bounded lookups, with no atmosphere ray marching. Two frame
slots consume approximately 1.54 MiB (Balanced) or 2.78 MiB (High) of RGBA16F LUT
storage per camera/cull visitor, plus shared atmosphere and star textures. Flat
allocates none of those per-view atmospheric textures. Pick, depth, and shadow
cameras skip sky rendering and LUT updates.

Raw results: [JSON](sky2-frames.json), [log](sky2-frames.log).

The [descent correction report](sky2-descent-validation.md) documents the orbital
sampling fix, reference-render accuracy, and before/after descent/rotation timings.

## Reproduction

From the repository root, build with the configured parent build directory:

```bat
set CMAKE_BUILD_PARALLEL_LEVEL=1
build.bat
call osgearth_shell.bat
cd tests
osgearth_tests "[sky2]"
osgearth_tests "[virtualprogram]"
osgearth_benchmarks --benchmark_filter=SkyNode2/Transmittance --benchmark_min_time=0.3s --benchmark_repetitions=3 --benchmark_out=sky2-transmittance.json
osgearth_benchmarks --benchmark_filter=SkyNode2/.*Frame --benchmark_min_time=0.3s --benchmark_repetitions=3 --benchmark_out=sky2-frames.json
```

The focused SkyNode2 run passes **1,705 assertions in 14 cases**, covering all presets,
ground/horizon/orbit/night rendering, stationary reuse, sparse-index point/spot
lights, solar shadows, simultaneous independent contexts, GPU release/recreation,
projected-map coordinates, disk visibility and lunar phase, runtime lighting
controls, earth-file loading, logarithmic depth, extension ownership, and reference
comparisons across 258 descent/airborne views and 144 horizon-crossing facade views.
Facade tests also verify opaque depth occlusion with logarithmic depth on and off.
Framebuffer PNGs `sky2-{ground,horizon,orbit,night}-{0,1,2}.png` and `sky2-map.png`
are written by the tests and were inspected visually.

The VirtualProgram run passes **881 assertions in 26 cases**. It includes regressions
for the ID-zero owner and context-specific program release bugs uncovered by the
benchmark. Shader snapshots and source-cached shaders now participate in context
release as well. The benchmark rejects GL/shader errors instead of accepting
timings from broken renders.

The broader default suite excluding the existing PBR group passes **4824 assertions
in 122 cases** (`osgearth_tests "~[pbr]~[.]"`). Graphical tests follow the repository's
hidden-test convention and are selected explicitly by the tags above. The full
unfiltered run is not green: it exits with status 1 in the existing `[pbr]` group;
that group also exits 1 by itself without a diagnostic. Its cause remains unresolved.
See [focused results](sky2-tests.log), [shader-cache results](sky2-virtualprogram-tests.log),
[broader results](sky2-suite-excluding-pbr.log), and [unfiltered output](sky2-suite.log).
