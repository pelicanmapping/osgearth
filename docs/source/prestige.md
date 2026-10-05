# Prestige nodekit

Build with `OSGEARTH_BUILD_PRESTIGE_NODEKIT=ON` to enable `osgEarthPrestige`.
The Windows `configure.bat` workflow enables it alongside `osgEarthProcedural`.
C++ callers include `<osgEarthPrestige/PrestigeLayer>`, use
`osgEarthPrestige::PrestigeLayer`, and link the `osgEarth::osgEarthPrestige`
CMake package target. `OSGEARTH_HAVE_PRESTIGE_NODEKIT` in `osgEarth/BuildConfig`
indicates that the nodekit was enabled.

When loading a `.earth` file, add the nodekit to the map's library list before
using the `<prestige:prestige>` layer configuration:

```xml
<map>
    <libraries>osgEarthPrestige</libraries>
    <!-- Existing Prestige layers go here. -->
</map>
```

The namespaced layer uses direct factory registration:

```cpp
REGISTER_OSGEARTH_LAYER_FACTORY("prestige:prestige", osgEarthPrestige::PrestigeLayer);
```

This macro accepts a string name and registers a factory used by `Layer::create`,
without generating plugin symbols or asking OSG to load a plugin. Load the nodekit
first, either by linking it or through the map's `<libraries>` list. For static
libraries, the object file containing the registration must be linked explicitly;
this macro does not provide a `USE_OSGEARTH_LAYER` anchor. Factory names are
case-insensitive; duplicate registrations leave the first factory in place.
The original `REGISTER_OSGEARTH_LAYER` macro remains available for plugin loading.

## Grime

`GrimeLayer` is part of this nodekit. Include `<osgEarthPrestige/GrimeLayer>` and
use `osgEarthPrestige::GrimeLayer`. Its shader is packaged with `osgEarthPrestige`.
The **Prestige > Grime** panel and the ImGui layer properties expose its controls when the nodekit is enabled.

Load `osgEarthPrestige` in the map's `<libraries>` list, then configure the effect
with `<prestige:grime>`. For example,
to apply weathering to a tiled model layer named `Buildings`:

```xml
<prestige:grime name="Building weathering">
    <model>Buildings</model>
    <amount>0.5</amount>
</prestige:grime>
```

## Texture preparation

Prestige enables the glTF reader option `gltfPrepareTextures`. The reader prepares
material textures on the loading thread, before publishing the material to its
shared cache. Other glTF callers can request the same option explicitly. Other
model readers are unaffected.

Preparation uses a private image and texture, preserving any source shared with
another loader or renderer. It selects compression from the material role:

| Role | Storage and mip filtering |
| --- | --- |
| Opaque color | BC1; preserve the texture's color space and filter sRGB colors in linear space. |
| Color with alpha | BC3; retain alpha and filter it linearly. |
| XYZ normal | BC3, linear; normalize filtered vectors and preserve XYZ encoding. |
| Packed DRAM | BC3, linear; preserve all four channels, including metallic in alpha. |
| Packed ORM, RM, or metallic/glossiness/AO | BC1, linear; preserve the existing RGB channel layout. |
| Separate occlusion | BC4, linear, using red. |

XYZ normals are deliberately not converted to BC5: Chonk's two-channel normal
path expects octahedral encoding. Preparation does not change shader encodings.
Compression is lossy. Alpha mip filtering matches ordinary averaging; it does
not implement alpha-test coverage preservation.

Mipmapped samplers receive a complete chain down to 1x1. Existing uncompressed
mip levels are retained as inputs, and missing levels are generated. Dimensions
are preserved, including non-power-of-two images. Compression block padding does
not change the texture's logical dimensions. Sampler settings remain unchanged.

Already compressed images, dynamic images, custom upload callbacks, unsupported
pixel formats, and unknown material layouts pass through. The optional `stbdxt`
image processor must be available; otherwise preparation leaves the input alone.
TextureArena diagnostics can still identify GPU work for these fallback cases.

The prepared-image cache is shared by glTF reads in the process. Its key includes
source contents and metadata, material role, channel layout, color space, and
mipmap policy. Different material factors or sampler wrapping do not require
recompressing an otherwise identical image. Concurrent requests for the same key
coordinate so only one compresses it.

Prepared images still referenced by tiles remain reusable. The cache also retains
up to 256 MiB or 4,096 recently used images after tiles release them. Once a result
is both unreferenced and evicted, a later load can recompress it. This is an
in-memory cache, not a persistent disk cache. Cold loads pay the CPU preparation
cost; warm material-cache hits bypass preparation entirely.

## Validation

On the development workstation, the `BM_MaterialUpload_*` benchmark measured
completed TextureArena compilation, including `glFinish`, for a constant opaque
RGBA source. Both paths read back every mip and verified identical red pixels.
Preparation selects BC1 for this opaque material; the baseline driver uses BC3.
These are microbenchmarks, not end-to-end tile-load or frame-rate measurements.

| Texture | Before: GPU compression/mips | After: prepared upload | Cold CPU preparation | Prepared-image cache hit |
| --- | ---: | ---: | ---: | ---: |
| 256 x 256 | 0.733 ms | 0.091 ms | 2.08 ms | 0.263 ms |
| 1024 x 1024 | 8.77 ms | 0.225 ms | 34.5 ms | 4.15 ms |

A shared-material cache hit avoids the prepared-image content hash as well.
The upload measurements use 12 iterations; cold preparation uses 5. Run from
`tests` after setting up `osgearth_shell.bat`:

```text
osgearth_benchmarks --benchmark_filter=BM_Material
osgearth_tests [materialprepare],[texturearena]
```

The targeted tests passed 200 assertions across 11 cases, covering concurrent
reuse, retention after unload, sharing beyond the retention budget, policy/source
changes, glTF cache publication, non-power-of-two dimensions, complete mip tails,
color filtering, normal encoding, and real compressed GPU uploads.

The broader existing PBR test `glTF converts channels and factors to shared PBR
materials` crashes when dereferencing a missing per-geometry VirtualProgram. It
also crashes when run alone with texture preparation disabled; that test still
assumes shader state on the geometry rather than the model root.
