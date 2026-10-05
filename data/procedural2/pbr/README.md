# Vegetation2 textured PBR art experiment

Copyright 2026 Pelican Mapping. MIT License, as in the repository LICENSE.txt file.

September 29, 2026. This is the active art set for all Vegetation2 earth files, including `tests/a.earth`,
`tests/prestige.earth`, and the Procedural2 art, OSM, coverage, row and canopy demos.
The optional procedural-grass demo keeps its blade generator; its asset fallback uses this catalog.
The original geometric starter set remains in `../starter`
for comparison. Placement, density, LOD thresholds and the 64 MiB catalog budget are unchanged.

The focus is texture detail on inexpensive geometry, with alpha-cutout foliage and runtime
lighting/shadows. These are original generated texture sources and procedural meshes, not
third-party game assets or measured photogrammetry. The sources were made with OpenAI's built-in
image generation tool; exact prompts are preserved in `source/prompts.json`.

| Asset | Previous near triangles | New near triangles | Distant triangles |
| --- | ---: | ---: | ---: |
| Broadleaf | 4,636 | 944 | 6 |
| Conifer | 3,616 | 940 | 6 |
| Shrub | 2,158 | 480 | 6 |
| Grass tuft | 150 | 10 | 6 |
| Fern | 1,008 | 48 | 6 |
| Boulder | 156 | 156 | 6 |

Leaves and needles are shared spray cards, grass is five cards, and fern fronds bend across
three segments. Tree trunks/branches and rocks have textured solid geometry. Card normals
describe foliage volume; bark/stone use smooth geometric normals. Texture maps supply the
small surface variations. There is no new runtime shader or geometry amplification.

## Materials

Each material uses Chonk's existing texture bindings:

- Unit 0: base color and coverage alpha. Sources remain in the renderer's existing color convention.
- Unit 1: OpenGL tangent-space RGB normal, opaque alpha.
- Unit 2: DRAM: displacement/height, roughness, ambient occlusion, metallic.

These natural materials have zero metallic. Leaf roughness is lower than bark/stone, with
restrained AO. The height, micro-normal, roughness and AO maps are approximate reconstructions
from the color source, not independent measured channels. Height is stored but does not add
triangles or enable parallax. Transmission/subsurface scattering and wind are not implemented.

Source images are retained verbatim. The compiler extracts the four botanical regions, prepares
512 px foliage materials and 1024 px bark/stone materials, adds transparent-edge RGB padding,
and exports editable PNGs plus runtime DDS. Foliage uses BC3; opaque bark/stone albedo uses BC1
so it does not require alpha testing. Normal and DRAM maps use linear BC3. Every DDS has all
mips through 1x1, avoiding driver compression and mip generation during paging.

## Source-derived distance representations

The offline baker rasterizes each textured near mesh from two sides and above, with depth and
alpha coverage, into a 3-view atlas. It carries source color and PBR values into six-triangle
proxies. Proxy normal maps contain only restrained detail on top of the existing runtime
crown-volume normals, avoiding normals that reveal the carrier planes.

Broadleaf/conifer canopy pieces bake five source trees into six triangles each. The runtime's
existing nine-piece templates therefore remain **54 triangles per aggregate**, for both tiers.
They continue to respect geographic masks and terrain fitting. The separate static cluster
demo retains 78 triangles near and six far. These art pieces do not pre-bake geographic pages.

## Rebuild and view

From the repository root, using Python with numpy and Pillow BCn encoding support:

```text
python tools/procedural2/generate_pbr_assets.py
```

The compiler enforces per-asset triangle caps and writes `manifest.json` with counts and DDS
payload totals. No network, model download, graphics context, or image generation is needed
to rebuild from the saved sources.

After `osgearth_shell.bat`, run from `tests`:

```text
osgearth_imgui procedural2-art.earth --sky2 --shadows --nvgl --samples 4 --vegetation2
osgearth_imgui a.earth --sky2 --shadows --nvgl --samples 4 --vegetation2
```

The first scene displays all five populations. `a.earth` keeps the user's population settings;
`prestige.earth` includes the same OSM vegetation setup alongside its existing building and sky layers.
Changing catalog paths back to `../data/procedural2/starter/` restores the original assets.

Limitations: three-view proxies can reveal their planes at grazing angles, plants share one
texture/shape per type, and texture-based roughness/normal estimation needs artist refinement.
Alpha-card overdraw still matters even with low triangle counts. Coverage/color matching through
the aggregate transitions remains Step 4B work. No performance benchmark is claimed.
