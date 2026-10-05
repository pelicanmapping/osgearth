# Vegetation2 PBR art detour

September 29, 2026: a texture-first art experiment within Step 4B. The user's requirement is
better textures and transparent foliage while keeping polygon counts low.

The September 29 demos selected `data/procedural2/pbr`; the September 30 `tests/a.earth` override is documented below. The original
starter catalog remains available for comparison. No placement, paging, density, quality,
runtime lighting or source composition policy was changed for this detour.

Original botanical, bark and stone texture sources were generated using the built-in image
generation tool. The reproducible asset compiler builds low-poly meshes, normal/DRAM maps,
compressed mip chains, individual impostors and source-derived reusable canopy pieces.
See the [art README](../../data/procedural2/pbr/README.md),
[manifest](../../data/procedural2/pbr/manifest.json), and
[generation prompts](../../data/procedural2/pbr/source/prompts.json).

Near trees stay below 1,000 triangles each (944 broadleaf, 940 conifer), shrubs use 480,
grass 10, ferns 48, and rocks 156. All distant proxies remain six triangles, with 54 triangles
per runtime aggregate template. These are asset counts, not measured performance results.

Use existing Chonk texture units and Sky2 PBR lighting. Preserve foliage alpha through shadows
and source baking. Keep albedo/normal/DRAM separate; natural surfaces have zero metallic.
Normal, roughness and AO reconstructions are plausible approximations, not measured scans.
Do not imply the presence of transmission, subsurface scattering, parallax or production wind.

Validation includes production AssetCatalog loading, triangle caps, complete compressed mip
chains, all three PBR texture bindings, finite normalized geometry, correct cutout/opaque
classification, the unchanged 64 MiB admission budget with all assets pinned, and canopy assembly.
Rendered checks use the actual Sky2/shadow/Chonk path with four-sample antialiasing.
Build/install and all 52 Procedural2 tests passed (1,233,384 assertions, including GPU workers).
The new art-specific case verifies 16,511 assertions. No benchmarks were generated.

- [Woodland capture](../../tests/procedural2-pbr-stand.png)
- [Ground-cover capture](../../tests/procedural2-pbr-ground.png)
- [OSM canopy/detail capture](../../tests/procedural2-pbr-canopy-detail.png)

The presentation is an experiment for tuning and review. Follow-up art work can refine the
species silhouettes, foliage color, overdraw, material response and distant coverage without
changing the modular placement or exclusion architecture. Step 4B remains in progress.

## Packed material formats (September 30, 2026)

`PBRMaterial` also accepts VRV RGB textures: red is metallic, green is glossiness
(`1 - roughness`), and blue is ambient occlusion. Select the layout explicitly:

```xml
<material layout="mtl_gls_ao" packed="surface_MTL_GLS_AO.dds"
          color="surface.dds" normal="surface_NML.dds"/>
```

`vrv` is an alias; layout names are case-insensitive and serialize as `mtl_gls_ao`.
In C++, select `PBRMaterial::MTL_GLS_AO` and supply `packed()` or `packedImage`.
These maps contain linear material data, not sRGB color. Alpha is ignored; foliage
opacity continues to come from the color/opacity input. VRV has no displacement.

Standard PBR, ShaderGenerator, Chonk, feature splatting and road substrate shaders
share the decoder. Roughness and metallic factors apply after channel decoding;
`occlusion_strength` blends AO toward one, and an independent AO input overrides
packed AO. Compressed input and mipmaps remain intact; no CPU channel repacking
is necessary. Existing DRAM, ORM and RM assets keep their channel conventions.
LayerShader custom PBR samplers expose `<name>_layoutAndFactors`; custom shaders
must use `oe_pbr_decode` from `PBRMaterial.glsl` rather than assume DRAM channels.

The current generated Vegetation2 art still uses DRAM. Legacy models with plain
textures on Chonk's units 0/1/2 likewise remain DRAM; a VRV material needs a
`PBRTexture` descriptor carrying its layout. File suffixes alone do not select it.


## VRV tree import (September 30, 2026)

`tests/a.earth` uses three selected trees from `D:/data/assets/vrv-vegetation/Trees`:

| Asset | Source folder | Near triangles | Individual proxy | Canopy piece |
|---|---|---:|---:|---:|
| red-maple | DeciduousBroadLeaf/Maple/RedMaple | 6,178 | 6 | 6 |
| white-oak | DeciduousBroadLeaf/Oak/WhiteOak | 5,478 | 6 | 6 |
| white-pine | EvergreenNeedleLeaf/Pine/EasternWhitePine | 4,273 | 6 | 6 |

Each source folder now has an `osgearth` subfolder containing stock static OSG models,
editable PNGs, compressed DDS textures with complete mip chains, and `manifest.json`.
Only these three trees are prepared for the demo; the remaining library is not loaded.
The original IVE/DDS files remain unchanged. Near triangle counts are higher than our
placeholder art; detailed meshes retain the existing individual LOD switch. Each reusable
canopy piece originally came from five instances of its actual source mesh; the far-coverage correction below replaces
that sparse arrangement with thirteen tightly packed trees. Runtime canopy assembly
still combines nine pieces (54 triangles) and applies geographic coverage and terrain fitting.

The earlier wrapper decoder preserved MAK IVE version 48, which stock OSG cannot read.
`tools/procedural2/export_source_scene.cpp` is an **offline** bridge built against the
matching source OSG SDK. It exports ordinary OSG text and resolves external image paths.
The vendor SDK is not linked into osgEarth. `osgearth_procedural2 --asset-source source.osg
--asset-json mesh.json --asset-mesh` then flattens the compatible input through Chonk.

`tools/procedural2/import_vrv_assets.py mesh.json --name red-maple --output <source-folder>/osgearth`
compiles that mesh and its PBR sidecars. Repeat for WhiteOakSpring and EasternWhitePine.
Source files selected here are RedMapleSpring.ive, WhiteOakSpring.ive and EasternWhitePine.ive.
The CLI accepts `--size` for atlas view resolution (512 by default; the corrected six-view atlas is 1536x1024).
For the offline bridge on this machine, use the MAK OSG SDK in
`D:/mak/engineering-mirror/3rdParty/libraries/makOsg/3.6.4_rev292407/win32-msvc++17.0-64bit`
and its osg/osgDB/OpenThreads libraries; execute with VRV's matching bin64 and plugin directory.

The importer preserves albedo DDS bytes, alpha and authored mipmaps. It resolves normal and
MTL_GLS_AO sidecars by the source albedo name. VRV BC5 normals contain tangent-space X/Y,
not Chonk's octahedral encoding: reconstruct positive Z offline and emit RGB normals.
For these plain unit-0/1/2 OSG files, material maps are repacked offline to DRAM with
D=0, R=1-GLS, A=AO, M=MTL. The native `PBRMaterial::MTL_GLS_AO` path remains supported;
this preparation avoids requiring custom material serialization in the portable OSG files.
No lighting is baked into the generated proxy colors. The corrected proxies bake source
surface normals and tangent normal maps into planar card frames, as described below.

`a.earth` retains its OSM rules, density, SSE adjustment, range, overlays, existing viewpoints
and other layers. Its mean canopy height changes from 14m to 21m for these taller sources;
three additional named VRV viewpoints provide close, middle and distant forest inspection.
The 64 MiB asset budget is unchanged. Local absolute paths deliberately reference the supplied
library; these external assets are not included in the Git repository.

Initial import validation: all three near/coarse/canopy bundles load through production AssetCatalog,
with 54-triangle assembled templates. All 60 runtime DDS files have complete compressed
mip chains and all source texture hashes still match. Rendered `a.earth` checks report
45,277,040 bytes of asset content, zero vegetation load failures and zero budget denials.
No benchmarks were generated. Step 4B remains in progress.

Build/install and the existing PBR-art regression passed (16,511 assertions). The capture helper
now accepts `--utc-hours` so review images can use daytime regardless of local clock/longitude.
The Finland captures use 10:00 UTC; the interactive Sky panel can set the same hour.

- [Close asset stand](../../tests/procedural2-vrv-art.png) (isolated flat placement for inspection)
- [Real OSM near forest in a.earth](../../tests/procedural2-vrv-forest-near.png)
- [Real OSM 12km canopy view in a.earth](../../tests/procedural2-vrv-forest-far.png)


## Detailed foliage coverage correction (September 30, 2026)

The initial VRV import exposed a rendering difference from the older VegetationLayer:
Vegetation2 used a hard 0.5 alpha test, and its individual Chonk drawables left mip
compensation at zero. Thin source foliage became nearly bare as it minified, while
its denser impostors retained their silhouettes. Source mip chains were intact;
for example, pine foliage retains about 8% base cutout coverage but no texels above
0.5 by mip level 5, making ordinary hard testing unsuitable without compensation.

Match the established layer's settings: A2C for multisampled targets, tree mip
compensation 0.75, shrubs 0.35, and other cover 0.25. The existing aggregate setting
remains 0.15. Chonk checks `gl_NumSamples` before relying on coverage, retaining a
hard cutout for single-sample targets (including application FBOs). The Chonk-specific
A2C define does not bypass VisibleLayer's ordinary opacity modulation. Shadow/depth
passes retain their existing alpha-tested path. Source images and generated meshes,
placement density, LOD ranges and triangle counts are unchanged.

Controlled captures force detailed geometry at the identical 300m camera with Sky2,
shadows and four samples, so the difference cannot come from switching to impostors:
[before](../../tests/procedural2-vrv-alpha-before.png),
[after](../../tests/procedural2-vrv-alpha-after.png).
The earlier VRV captures above predate this coverage correction.

The rebuilt code passes the new GPU checks for actual single-sample and 4x targets,
plus existing individual-LOD transition, canopy coverage/shadow, and PBR-art checks.
The targeted run passed 16,571 parent assertions and 71,573 isolated GPU-worker assertions.

## Source-normal impostor correction (September 30, 2026)

The first PBR baker copied a weakened leaf tangent normal map onto generic upward-facing
crown normals. It never transferred the source mesh normals. That lost the orientation of
leaves and branches, giving the impostors a smooth, overly bright appearance compared with
the detailed trees.

The baker now interpolates the source geometry normals, applies the source tangent normal
map using Chonk's cotangent-frame convention, and reproduces the two-sided leaf normal rule.
It encodes the resulting direction in the receiving card's orthonormal frame. Six captures
cover both sides of the two upright cards and the overhead card. Albedo, alpha and DRAM
use those same captures; colors remain unlit and the sun can move normally at runtime.

Chonk normal technique 4 (`NORMAL_TECHNIQUE_BAKED`, vertex attribute 6) identifies this
contract: geometric planar vertex normals, full-sphere RGB tangent normals, and front-view
UVs in the upper atlas half. Back faces select the lower half for every material channel,
including shadow alpha. Tangent axes are normalized independently so atlas aspect ratio
and aggregate footprint stretching do not deform the baked lighting directions. Canopy
assembly retains this technique and rotates its card frames instead of substituting volume
normals. Existing technique-3 artwork remains supported unchanged.

All three VRV individual impostors and their source-derived canopy pieces were regenerated
beside the original models. Each still uses six triangles; runtime canopy templates still
use 54. Compact 3-by-2 atlases use all their space. Their BC3 DDS files include every mip,
including non-power-of-two levels. Runtime albedo cutouts retain A2C with four samples;
single-sample and shadow passes retain their existing hard-cutout behavior.

Validation: build/install passed; two Python checks cover source normal transfer, both
faces, compact DDS layout, mip payloads and image orientation. GPU/material integration
checks passed 17,243 parent assertions plus 71,575 isolated worker assertions, covering
front/back lighting frames, instance rotation/stretching, A2C, individual LOD transitions,
canopy fades and shadows. Each imported bundle also passes production AssetCatalog loading.
The mixed-LOD OSM capture reports 57,859,472 bytes (55.2 MiB), zero failed loads and zero
budget denials under the unchanged 64 MiB limit. No benchmarks were generated.

- [Original impostor shading](../../tests/procedural2-vrv-normals-before.png)
- [Source-normal impostor shading](../../tests/procedural2-vrv-normals-after.png)
- [Detailed reference, same camera and sun](../../tests/procedural2-vrv-alpha-after.png)
- [Mixed detailed/impostor OSM forest](../../tests/procedural2-vrv-normals-transition.png)

The comparison uses an isolated stand with identical placement, camera, Sky2 time (10 UTC),
shadows and four samples. The OSM check uses `a.earth` vegetation/elevation with imagery and
Prestige omitted from a temporary validation scene. The user earth file is unchanged.
These remain crossed-card approximations: silhouettes, parallax and self-shadowing cannot
exactly match the full mesh. Source-derived normals fix the lighting-frame error; a more
complete view-dependent impostor/depth representation remains a separate art improvement.

## View-conditioned crown lighting (September 30, 2026)

Source normals alone did not resolve the next reported artifact: bright horizontal caps
and dark vertical walls in oblique views. The same pattern appeared with shadows disabled.
The correctly decoded normal map still described a fixed capture direction; applying it
rigidly to a crossed card lit that card like a different face of the crown.

Vegetation2 now enables the opt-in `OE_CHONK_BAKED_CROWN` Chonk shader path for normal
technique 4. After decoding the source normals, it rotates their distribution from the
selected front/back capture axis toward the actual viewing direction. This approximates
the changing visible surface of a rounded foliage volume while retaining leaf-normal
variation. It is view-conditioned relighting, not a new hemisphere painted over the source
normal map. It does not reconstruct tree depth or parallax. Orthographic cameras use their
parallel view direction instead of a perspective eye vector.

The shader also fades grazing cards with A2C, following the useful edge-on suppression in
the older VegetationLayer. Coverage uses each card's facing relative to the most favorable
of its three frame axes: the best-facing card keeps full coverage even at diagonal views.
Apply this fade after mip alpha compensation so distant mip levels cannot resurrect an
edge-on sheet. Single-sample targets retain the cutout fallback. Camera-depth passes use
the same card-facing suppression; shadow cameras retain all silhouettes independently of
the main eye, so turning away does not remove a tree's cast shadow. The card frame comes
from view-position derivatives and therefore also works in depth-only shader programs.

This applies to source-baked individual impostors and source-derived canopy pieces, with
no new textures, rebaking, triangles, placement changes or LOD changes. Detailed mesh leaf
lighting and the older technique-3 art retain their existing behavior. Shader work increases
slightly; this checkpoint does not make a measured performance claim or add benchmarks.

Validation: build/install and the existing baked-frame, lighting, A2C, individual transition,
PBR-art and canopy GPU checks pass. The added 292-assertion GPU regression covers front/back
views, rotation, nonuniform scale, perspective/orthographic cameras, angle coverage and
color/depth/shadow variants with four actual framebuffer samples. Controlled captures use
the same stand, 300m camera range, 10 UTC sun and four samples, at pitches of -25, -45 and
-75 degrees. The -45 comparison is:

- [Fixed-capture lighting before](../../tests/procedural2-vrv-crown-before.png)
- [View-conditioned lighting after](../../tests/procedural2-vrv-crown-after.png)
- [Low-angle view](../../tests/procedural2-vrv-crown-low.png)
- [Overhead view](../../tests/procedural2-vrv-crown-high.png)

The bright-cap/dark-wall lighting split is substantially reduced. Crossed-card silhouettes
and cast/self shadows remain approximations, especially close up: these proxies can cast
denser and more angular shadows than the full source trees. Solving that remaining issue
requires a better depth/shadow representation; the normal correction does not claim to do so.
The user accepts these crude shadows, so a more elaborate shadow representation is not required now.

## Stationary shimmer and inherited blending (September 30, 2026)

The real demo was applying A2C and conventional alpha blending simultaneously. VisibleLayer initializes
`GL_BLEND` as `ON | OVERRIDE` on the layer root during rendering preparation (or an opacity edit).
Vegetation2's `OFF | OVERRIDE` was on its child content StateSet, so the parent's override defeated it.
The older VegetationLayer disables blending on the layer StateSet itself and does not have this conflict.
With conventional blending still active, the GPU's changing instance order changed the composited color,
even with four samples, A2C and depth writes enabled. Isolated shader tests had missed the parent state.

The fix adds `PROTECTED` to Vegetation2's child blend-disable. It preserves A2C, depth writes, the single-sample
cutout fallback and the existing layer-opacity shader. Density, art, normals and crude shadow geometry stay
unchanged. This also prevents the base layer's late initialization from re-enabling conventional blending.

The new GPU regression instantiates the real VegetationLayer2/VisibleLayer hierarchy, initializes parent
opacity state, and renders 128 overlapping instances at distinct depths. Both submission orders and eight
stationary frames agree, for GPU-culled and unculled draws, at full and half layer opacity. It also reads the
effective driver state during state application: four samples, A2C on, blending off, depth testing/writes on.
The same test failed before the fix because blending was enabled. Coplanar depth ties remain a separate
depth-buffer limitation; this fix does not perturb placement or introduce depth bias.

In a 1440-by-960 stationary OSM forest capture with Sky2 and four samples, before the fix an average of
65,948 pixels changed by more than 3/255 across 19 comparisons to the first frame. Afterward the average was
20 (maximum 28), confirming that the widespread draw-order shimmer is removed, not claiming perfect
bit-identical output from the asynchronous whole scene. With shadows enabled the corrected scene averaged
24 changed pixels (maximum 37). These are image-correctness checks, not benchmarks.
Temporary draw-state logging and frame-sequence hooks were removed after verification; only automated
test instrumentation remains. Existing lighting, baked-normal, A2C, PBR-art, canopy and LOD tests also pass.

## Alpha-ramped transitions (September 30, 2026)

The user requested removal of the visible dither pattern. Vegetation2 no longer enables Chonk's optional Bayer
fallback, and its canopy/page shader no longer hashes screen coordinates or discards fragments by a coverage rank.
Individual LOD and range fades retain their alpha ramps; nested canopy handovers, page arrival and whole-page
visibility now multiply material alpha by their composed weight before A2C. The cached interval endpoints still
encode nested weights without per-instance uploads; only their difference is used for shading.

Mip-alpha compensation is clamped before applying the weight so distant texture minification cannot cancel a
fade. Four-sample MSAA/A2C resolves fractional coverage with blending disabled and depth writes enabled. A2C has
finite sample coverage levels; there is no additional shader-authored screen-door pattern. Ordinary single-sample
color targets use the existing alpha-test fallback. Depth and shadow targets threshold the page/representation
weight at one half, keeping the existing leaf cutout rather than gradually eroding the leaf silhouette. Crude
impostor shadows remain intentionally supported.

GPU regression coverage includes both aggregate culling switches, opaque-path routing, color/depth/shadow
variants, minified textures, nested asynchronous paging, and page extinction/restoration. Each interior test pixel
must resolve the expected alpha coverage, rejecting the old binary pixel-noise pattern.

Build/install and the targeted A2C, lighting, source-normal, PBR-art, individual-LOD and canopy GPU checks passed.
A four-sample Sky2/shadow forest approach-and-return captured all 60 frames successfully with no asset load
failures or budget denials. Visual evidence is in `build/vegetation-alpha-ramp-tour/`; the final paging report
is `build/vegetation-alpha-ramp-forest.png.txt`. These are temporary visual checks, not new shipped art or benchmarks.

## Leaf shadow coverage (September 30, 2026)

The imported VRV trees were casting mostly trunk shadows. Their sparse leaf alpha averages below 0.5 in
smaller texture mips: the maple's leaf materials have no texels above the shadow cutoff by mip 6. Color rendering
already applies per-population mip-alpha compensation, but the shadow/depth branch skipped it entirely.

Chonk now applies that same existing compensation before the color/depth/shadow split. Shadows retain filtered
mips and their hard silhouette cutoff; no shadow-map resolution increase, extra geometry, textures, forced
mip-zero sampling or dithering is needed. The before/after stand capture restores broad canopy shade at unchanged
resolution. Transparent gaps still discard. This is an approximate shadow silhouette, as requested, and does not
claim leaf-level coverage conservation or eliminate all shadow aliasing.

A new single-sample GPU depth-readback regression uses an opaque trunk region, an empty gap, and sparse leaves
that disappear under ordinary mip averaging. It covers two minifications, GPU culling on/off, and color/depth/shadow
variants. Turning compensation off reproduces the missing leaf depth; the existing tree coefficient restores it
without closing the empty gap. Controlled visual captures with Sky2, shadows and four samples are
`build/vegetation-leaf-shadow-before.png` and `build/vegetation-leaf-shadow-after.png`.

Build/install and the targeted foliage, A2C, lighting, baked-normal, PBR-art, LOD and canopy tests passed.
The broad shadow run passed 20/21 cases; `Shadow46 isolates views and parent transforms` intermittently exceeds
its image-equivalence tolerance. It passes alone, and the same suite failure reproduces with the pre-fix Chonk
shader supplied through a temporary external shader path. That separate multi-camera issue is not changed here.

## Fuller far canopy coverage (September 30, 2026)

The old canopy combined a small fixed 50 m2 crown-area estimate, narrow separated template pieces, and sparse
five-tree textures. Enlarging the footprint also increased its LOD error, requesting finer detail farther away.
Those mechanisms made the footprint selector a poor way to tune distant forest coverage.

- Canopy handovers now use a per-tier reference based on the default 16x16 patch grid, plus measured terrain-fit
  residual. Changing footprint or fullness leaves that nominal reference unchanged. The global-plus-population
  quality budget still controls handovers and page disappearance.
- Nine shared pieces overlap within the same certified unit-square bounds. Crown coverage uses population density,
  scale, and an estimated diameter of 0.8 times configured canopy height, with a `canopy_cover_scale` multiplier.
  The ImGui **Canopy fullness** control applies to both tiers (0.25..4, default 1); use **Apply population**.
  This is an art-calibration heuristic, not an asset measurement. Fine placements remain unchanged.
- `import_vrv_assets.py --canopy-only` bakes thirteen source trees in a compact spiral with spacing 0.24 times
  the source crown diameter. All three local VRV species were rebuilt in their existing `osgearth` folders.
  The 108 non-canopy asset files remained byte-identical. Each piece still uses six triangles and the same six-view
  albedo, normal, and DRAM atlas dimensions with full precompressed mip chains. Top-view mean source alpha increased
  from 0.277 to 0.486 (maple), 0.251 to 0.449 (oak), and 0.166 to 0.340 (pine).
- At 2x/4x footprints, boundary/terrain refinement now keeps the default terminal geographic resolution instead
  of stopping at a correspondingly coarser grid. Up to four splits use a compact 16x16 clip code in the existing
  instance payload. This preserves the selected art scale while retaining forest next to exclusions. The maximum
  remains 4,096 terminal pieces per page at scales >=1 and 16,384 at 0.5; no per-page textures or geometry are added.
  Boundary-heavy pages can require more clipped pieces than before; this is a coverage correction, not a measured
  performance improvement. Narrow unresolved slivers can still be omitted conservatively.

Validation: `build.bat` build/install passed (RelWithDebInfo). All 54 Procedural2 cases excluding the separate lighting
suite passed, including fixed handover references at every tier/footprint, independent fullness, unchanged fine
placements, preserved road/hole exclusions, and GPU clipping reconstruction at all four depths and quarter turns
in color/depth/shadow passes. The Python bake checks also passed. The broad run found twelve existing baked-normal
fixture comparisons one channel value outside tolerance (5 versus allowed 4); this reproduces in the isolated crown
worker, which does not use aggregate assembly or placement. No lighting shader or tolerance was changed in this task.

Controlled OSM captures use Sky2, shadows, and four samples:
`build/canopy-cover-before.png`, `build/canopy-cover-final.png`, and `build/canopy-cover-4x.png`.
The larger-footprint scene retains aggregate rendering without bringing back the detailed population. Asset content
stays at 57,859,472 bytes with zero load failures or budget denials in these checks. White terrain deliberately exposes
remaining gaps; this is fuller coverage, not a claim of exact near/far silhouette or color agreement. Step 4B remains
in progress; no benchmarks were generated.

## Application multisampling inheritance (October 1, 2026)

Vegetation2's local `GL_MULTISAMPLE` ON state caused OSG to track that mode and restore its assumed OFF default
outside the layer. A four-sample framebuffer could therefore render ordinary geometry without multisampling
after vegetation, including on following frames. The original VegetationLayer also installs a local mode after
detecting requested/tracked multisampling; copying that code does not address this particular state leak.

Vegetation2 now leaves `GL_MULTISAMPLE` inherited from the application. It still scopes alpha-to-coverage and
protected unblended rendering to its content. No application-wide override is installed. Chonk's existing
`gl_NumSamples > 1` branch handles single-sample targets, and shadow/depth passes retain their hard cutouts.
That built-in reports framebuffer storage, not the multisample enable switch; no new shader check is introduced.

The dedicated regression uses the real Vegetation2/VisibleLayer state with ordinary geometry drawn before and
after it. Both single-sample and four-sample workers pass 336 assertions each, covering multiple frames, GPU
culling on/off, inherited defaults, explicit application ON/OFF/ON, and vegetation removal. The old local ON
reproduced the reset. Existing ImGui, A2C order-independence, and foliage-shadow tests pass (327 assertions).
`build.bat` build/install passed in RelWithDebInfo. No benchmarks or runtime diagnostic instrumentation were added.


## Tree-card cluster renderer (October 1, 2026)

The active Vegetation2 canopy tiers now reuse each asset's **individual coarse model**. They no longer load
`canopy` art or stretch a baked stand to a patch. Shared slot meshes plus resident per-tree transforms retain
tree size, placement, normal/PBR textures, alpha coverage, and terrain roots. No atlas rebake is needed.
The old aggregate art and its generation notes remain historical/reference material. See the current
[Step 4B plan](procedural2-plan.md) for controls, validation, and the option to approximate coverage later.
