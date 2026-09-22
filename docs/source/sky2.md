# SkyNode2

`SkyNode2` is a core-library Earth sky and metallic/roughness lighting implementation.
It does not load the SimpleSky or Bruneton drivers. It uses one material lighting
path at every quality level, including the atmosphere-free preset.

```cpp
#include <osgEarth/SkyNode2>

osgEarth::SkyNode2::Options options;
options.preset = osgEarth::SkyNode2::BALANCED;
osg::ref_ptr<osgEarth::SkyNode2> sky = new osgEarth::SkyNode2(options);
sky->addChild(mapNode);
sky->attach(viewer);
viewer->setSceneData(sky);
```

In an earth file, add `<sky2><preset>balanced</preset></sky2>`. Example applications
also accept `--sky2`, optionally with `--sky-low` (flat) or `--sky-high` (high).
This command-line selection replaces sky extensions already present in the earth file.
`SkyNode::create("sky2")` constructs the core implementation directly. Existing
explicit legacy sky selections keep working. Remove the old sky when adopting
SkyNode2; nesting two atmospheric lighting implementations is unsupported.

## Quality and costs

| Preset | Sky LUT | Integration samples | Aerial distance slices | Environment samples |
| --- | --- | --- | --- | --- |
| Flat | none | none | none | analytic ambient |
| Balanced | 192 x 108 | 16 | 16 | 16 |
| High | 256 x 144 | 32 | 32 | 32 |

“Flat” means atmosphere-free, not unlit: all presets support GGX specular,
metallic/roughness materials, normal maps, ambient occlusion, and OSG lights.
Balanced and High use the same scattering equations; only sampling density changes.

Atmosphere-enabled rendering reserves four terrain texture units. If unavailable,
the node reports a warning and uses atmosphere-free PBR. Standalone model scenes
reserve from unit 8 upward; material textures should use units 0–7. The catalog star
texture uses unit 0 only on the private background drawable.

The immutable 256 x 96 RGB32F atmosphere atlas is shared by every instance. Its
CPU initialization runs once. Camera motion and time changes perform **no CPU
atmosphere integration or probe filtering**. Three bounded GPU passes produce
sky radiance, a GGX/cosine-convolved environment, and aerial perspective. The passes
rerun for position/sun changes; camera rotation and projection changes reuse all
lookup textures. Each camera/cull
visitor owns two frame slots to avoid mutating a previous frame's uniforms during
OSG's pipelined draw. The background renders at far depth in bin 5, between ordinary
opaque geometry and transparent bin 10, avoiding shading sky hidden by terrain.

The 32 aerial elevation-row intersections and distance warps are cached in small
per-view uniform arrays when the observer moves. Environment-coordinate transforms
are also composed once per view, reducing repeated fragment work without changing
the atmosphere's sampling quality.

The aerial volume uses 64 azimuths by 32 horizon-centered elevations. Distance
slices cover only the atmospheric part of each ray, with extra resolution near
its lowest altitude. The same continuous mapping works from the ground through
orbit without altitude-based model switches or empty-space depth slices. Scene
fragments reconstruct finite-distance haze independently in the two neighboring
elevation rows before blending them. This prevents the distant horizon from
bleeding onto nearby geometry. Complete atmospheric paths use two bilinear reads;
partial paths use up to eight, with optical-depth interpolation between slices.
Scene fragments do not march atmosphere rays.
Two frame slots occupy approximately 1.54 MiB (Balanced) or 2.78 MiB (High) per
camera/cull visitor, plus the shared atmosphere and star textures.

## Lighting contract

Incoming material color uses osgEarth's existing sRGB color contract. Lighting,
environment sampling and atmospheric compositing operate in linear HDR space.
Exposure, a filmic output curve, and sRGB encoding occur once at the end. With an
external HDR renderer set `toneMapping = false` and `outputSRGB = false`, and use a
floating-point framebuffer. Do not also enable automatic framebuffer sRGB encoding
when `outputSRGB` is true. Alpha is preserved.

`oe_pbr` material factors remain compatible with PBRMaterial, imported glTF
materials, terrain, and Chonk. The sun occupies the OSG light index supplied to
`attach` (0 by default). OSG light indices 0–7 are supported, including directional,
point and spot lights with distance/cone attenuation. Install
`GenerateGL3LightingUniforms` on application light sources as usual. The sun uses
the existing `oe_shadow_visibility` interface, so a ShadowCaster can use
`sky->getSunLight()`. SkyNode2 does not automatically allocate shadow maps.
See [Sun shadows](shadows.md) for installation, quality controls, and texture-unit reservation.

`sunIntensity` sets incident solar irradiance in the model's relative radiometric
units (default 10). The sun light's diffuse RGB and intensity also tint and scale
the visible solar disk and reflected lunar light. `environmentIntensity` adjusts
indirect sky lighting (default 1); zero skips its reads. `ambient` is a minimum
night fill, default 0.033. Set it
to zero for a physically dark night. `setExposure`, `setSunIntensity`,
`setEnvironmentIntensity`, and `setAmbientIntensity` are runtime controls.
EnvironmentGUI recognizes SkyNode2 and exposes these settings and solar color,
alongside the existing time, shadow, and celestial visibility controls. Opening
the panel preserves the scene's lighting settings; edits run during update.
The panel's Install button also creates SkyNode2. Quality is selected at construction.
All setters and scene-graph changes belong on
the update thread, with normal OSG update/cull synchronization.

## Planet and celestial coordinates

The atmosphere uses a 100 km shell with Rayleigh, aerosol and ozone extinction,
single scattering and an isotropic multiple-scattering closure. WGS84 ECEF is
scaled to an oblate atmosphere, avoiding the polar altitude error of a single
equatorial-radius sphere. Geometry remains in OSG's original double-precision
coordinate system; only atmospheric calculations use scaled kilometers. Rays are
clipped against both solid Earth and the atmospheric shell, including from orbit.

The inherited ephemeris and date/time APIs position the sun and moon. The moon has
observer parallax, angular size and a sun-lit phase; its built-in albedo is procedural.
Stars use the existing Yale catalog with sidereal Earth rotation. Visibility
switches affect the visible bodies; hiding the sun disk does not turn off sunlight.
`setAtmosphereVisible(false)` hides sky scattering and aerial perspective while
retaining atmospheric illumination. ECI is selectable through inherited options.

For projected maps set a reference point before traversal (the earth-file extension
does this automatically). The projected map is treated as a local east/north/up
tangent plane around that point, not a globally curved projection. This is an Earth
model, not an arbitrary-planet atmosphere. Perspective camera rendering is the
primary supported projection.

## Approximation boundaries

Optional [procedural volumetric clouds](clouds.md) attach through `setCloudLayer` or a nested `<clouds>` block.
Cloud-free rendering remains the default. The base renderer has no local reflection captures,
screen-space ambient occlusion, terrain-aware indirect occlusion, or automatic
exposure. Ambient occlusion comes from materials, and terrain shadows come from
the existing shadow system. The low-resolution aerial volume can smooth distant
atmospheric transitions; High increases angular/distance integration quality.
Environment reflections describe the sky and a mean ground albedo, not nearby
buildings. The moon does not yet use a photographic albedo map or cast a second
directional light. Custom opaque render bins above 5 should place the background
appropriately before adopting this renderer.

The atmosphere architecture is informed by Sébastien Hillaire's
[A Scalable and Production Ready Sky and Atmosphere Rendering Technique](https://sebh.github.io/publications/egsr2020.pdf).
The implementation is new; it does not wrap or copy the old osgEarth sky shaders.
Material shading uses the standard GGX/Smith/Schlick model and a split-sum environment
approximation. Validation and measured timings are recorded in
`tests/sky2-performance.md`, `tests/sky2-descent-validation.md`,
`tests/sky2-horizon-validation.md`, and `tests/sky2-lighting-performance.md`.
