# SkyNode2 horizon on foreground geometry

The horizon line across nearby buildings came from aerial interpolation. The
background depth test was working: an opaque, constant-color wall completely
occludes it with both ordinary and logarithmic depth. The artifact reappeared
when the wall used the production aerial lookup.

The volume's elevation rows have different atmospheric ray lengths, especially
across the horizon where one ray ends at the ground and its neighbor continues
through the atmosphere. Bilinear filtering at a shared normalized depth mixed
distant scattering into nearby surfaces. Interpolating the nonlinear depth
coordinate directly also gave incorrect weights within each depth slice.

## Correction

The shader now reconstructs each neighboring elevation row independently and
then blends their results. It uses physical distance within each depth interval,
Beer-Lambert transmittance interpolation, and the corresponding source integral
for scattering. This resolves short paths inside coarse horizon slices without
ray marching on geometry.

Row reconstruction preserves both nearby metric distance and the far atmospheric
endpoint. With atmospheric distance `d`, query span `L`, and neighboring row span
`R`, its remapped distance is:

```text
d_row = d / (1 - (d/L)^4 + (d/R)^4)^(1/4)
```

For positive spans and `0 <= d <= L`, this is monotonic, maps zero to zero and
`L` to `R`, and differs from metric distance near zero only at fifth order.
Preserving the far endpoint retains the earlier orbital correction. There is no
altitude-based switch or new LUT. Empty paths return zero haze. Complete
atmospheric paths use two texture reads; partial paths use up to eight.

## Validation

The new facade regression uses actual opaque geometry, with a constant dark
surface shaded by either the production aerial lookup or 256-step reference
integration. The reference shares the atmosphere coefficients, so this measures
sampling error rather than atmospheric ground truth. Metrics use display-encoded
RGB in an 8-bit framebuffer, read back as floats. Only the fictitious wall portion
below the reference ellipsoid is excluded; horizon pixels remain in the comparison.

Each preset covers camera heights of 2, 100 and 1,000 m, facade distances of 50,
300, 1,000 and 10,000 m, and solar elevations of 5, 35 and 85 degrees. Both depth
modes run every combination: **144 views total**, plus four opaque-occlusion
checks. The original shader fails 194 assertions. The correction passes the
same RMS < 0.015 and maximum-channel < 0.05 thresholds.

| Preset | Worst RMS across wall views | Worst channel error |
| --- | ---: | ---: |
| Balanced | 0.003720 | 0.039216 |
| High | 0.002571 | 0.023529 |

For the representative 300 m facade at 100 m observer altitude and 5-degree sun:

| Preset | RMS before | RMS after | Maximum before | Maximum after |
| --- | ---: | ---: | ---: | ---: |
| Balanced | 0.023751 | 0.001471 | 0.356863 | 0.015686 |
| High | 0.011435 | 0.000752 | 0.360784 | 0.007843 |

The full SkyNode2 suite passes **1,705 assertions in 14 cases**, including all
previous ground, orbital, airborne, lighting, multi-view and lifecycle regressions.
The tests reject shader compile/link errors and OpenGL errors.

Images: [before](sky2-wall-before-1.png), [corrected](sky2-wall-1.png),
[reference](sky2-wall-1-reference.png). The thin horizontal line through the wall
is present before correction and absent afterward.
Logs: [before](sky2-wall-before-tests.log), [wall and orbital regressions](sky2-wall-tests.log),
[full focused suite](sky2-tests.log).

## Performance

RTX 5080, OpenGL 4.6, driver 610.60, Windows RelWithDebInfo. Medians of five Google
Benchmark repetitions, at least 0.5 seconds each. The fixed camera views the same
300 m opaque facade at 100 m altitude, with an empty map and the complete SkyNode2
pipeline. The facade covers most of the screen and exercises finite-distance
aerial reconstruction. Readback, startup and shader compilation are excluded.

| Preset | Resolution | Before | After |
| --- | --- | ---: | ---: |
| Balanced | 1920 x 1080 | 0.805 ms | 0.813 ms |
| High | 1920 x 1080 | 0.816 ms | 0.813 ms |
| Balanced | 3840 x 2160 | 0.980 ms | 1.187 ms |
| High | 3840 x 2160 | 0.976 ms | 1.199 ms |

The 1080p differences are within timing variability. The additional reconstruction
cost is visible at 4K: approximately 0.21–0.22 ms in this scene. Texture memory,
texture unit count, LUT dimensions and integration sample counts are unchanged.
These GPU timer queries bracket the complete rendering traversal, including
submission gaps; they are not isolated shader timings or application FPS.

Raw results: [before JSON](sky2-wall-before.json), [after JSON](sky2-wall-after.json),
[before log](sky2-wall-before-benchmarks.log), [after log](sky2-wall-after-benchmarks.log).

After `build.bat`, run from the repository root:

```bat
call osgearth_shell.bat
cd tests
osgearth_tests "[sky2]"
osgearth_benchmarks --benchmark_filter=SkyNode2/.*Horizon --benchmark_min_time=0.5s --benchmark_repetitions=5 --benchmark_out=sky2-wall-after.json
```
