# SkyNode2 space-to-ground correction

This records the initial descent correction. The subsequent
[horizon correction](sky2-horizon-validation.md) refines finite-distance
interpolation, changes the lookup read count, and reruns these regressions.

The expanding rings came from logarithmic aerial slices spanning the entire
camera-to-scene distance. From orbit, most slices lay in vacuum; the few that
crossed the atmosphere could not resolve its dense lower layers. Interpolation
between those slices produced large, distance-dependent changes in surface haze.
The original ray integrator also concentrated samples at the camera end, even
when an orbital observer's dense atmosphere was at the opposite end.

The corrected volume clips depth to each ray's atmospheric interval and uses a
signed square-root coordinate centered on its lowest altitude. Integration uses
the same density-focused spacing. A 64 x 32 horizon-centered angular grid retains
limb detail as the globe shrinks; the previous grid covered the screen with 32 x
32 samples. Both presets retain their original 16/32 depth slices and integration
sample counts. Surface rendering still uses four bilinear aerial reads without
ray marching. Camera rotation and projection changes now reuse the volume.

## Accuracy

The offscreen regression renders a constant dark analytic WGS84 ellipsoid through
the production aerial lookup and compares it to 256-step integration at every
pixel. This isolates atmospheric sampling from terrain LOD, texture streaming,
material lighting, and mesh tessellation. The reference uses the same physical
coefficients; it tests numerical approximation, not atmospheric ground truth.
The framebuffer is 8-bit RGBA, read back as floats. Metrics use display-encoded
RGB in [0,1], excluding the immediate silhouette (absolute ray/surface cosine
below 0.3). These bounds do not characterize silhouette error.

Each preset covers 65 camera altitudes from 2 m to 15,000 km, including both sides
of atmospheric entry and 250 km increments through the orbital range. Another
64 views per preset test surfaces at 1, 12, 80 and 120 km, cameras above and below
those surfaces, and solar elevations of -6, 5, 35 and 85 degrees. The 120 km
surfaces also check geometry in vacuum before the ray reaches the atmosphere.

| Preset | Worst descent RMS | Worst airborne RMS | Worst channel error, all cases |
| --- | ---: | ---: | ---: |
| Balanced | 0.004606 | 0.003819 | 0.047059 |
| High | 0.004206 | 0.002569 | 0.047059 |

At 5,000 km, RMS error fell from **0.244555 to 0.002434** in Balanced and from
**0.203126 to 0.001396** in High. The regression thresholds are RMS < 0.025 and
maximum channel error < 0.1. The original implementation fails both regression
cases; the corrected implementation passes. The full focused SkyNode2 suite
passes **1,265 assertions in 13 cases**, including actual map loading and orbital
renders with logarithmic depth. Shader compilation, linking and GL errors fail
the graphics tests.

Images: [before, Balanced](sky2-zoom-before-1.png),
[corrected, Balanced](sky2-zoom-1.png), [reference](sky2-zoom-1-reference.png).
Logs: [original sampling](sky2-zoom-before-tests.log), [corrected suite](sky2-tests.log).
The original log predates adding the 120 km surface cases; all shared cases use
the same error thresholds and coverage mask.

## Performance

Measured on the same RTX 5080 / OpenGL 4.6 / driver 610.60 system as
[the initial performance report](sky2-performance.md). These are medians of five
Google Benchmark repetitions, with at least 0.5 seconds per repetition, at
1920 x 1080. The analytic probe samples the production aerial volume over the
visible globe. Descent changes altitude over a 120-frame logarithmic sweep from
2 m to 15,000 km; rotation changes orientation at a fixed 5,000 km altitude.
The comparison uses the old and corrected renderers with the same scene and
camera paths. The probe calls each renderer's corresponding aerial lookup API.

| Scenario | Original | Corrected | Difference |
| --- | ---: | ---: | ---: |
| Balanced descent | 0.584 ms | 0.636 ms | +0.052 ms |
| High descent | 0.618 ms | 0.693 ms | +0.075 ms |
| Balanced rotation | 0.510 ms | 0.501 ms | -0.009 ms |
| High rotation | 0.546 ms | 0.519 ms | -0.027 ms |

The larger angular grid adds a small cost during descent. Rotation avoids LUT
updates, although the measured savings are small relative to timing variability.
These GL timer queries bracket the entire rendering traversal, including GPU
submission gaps; they are not isolated shader timings or application FPS.
Readback, shader compilation, and startup allocations are excluded. Timed runs
report no GL or shader errors. Two frame slots now use 1.54 MiB (Balanced) or
2.78 MiB (High), up from 1.04 / 1.78 MiB. Texture unit count is unchanged.

Raw benchmarks: [before](sky2-zoom-before.json), [after](sky2-zoom-after.json),
[before log](sky2-zoom-before-benchmarks.log), [after log](sky2-zoom-after-benchmarks.log).

After `build.bat`, run from the repository root:

```bat
call osgearth_shell.bat
cd tests
osgearth_tests "[sky2]"
osgearth_benchmarks --benchmark_filter="SkyNode2/.*(Descent|Rotation)" --benchmark_min_time=0.5s --benchmark_repetitions=5 --benchmark_out=sky2-zoom-after.json
```
