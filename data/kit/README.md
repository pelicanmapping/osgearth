# osgEarth Kit prototype data

`buildings.kit` maps stable model names to ordinary osgDB-loadable model files. The
three brick models are half-metre vertical quads and `roof_gray` is a half-metre
horizontal quad. City fixtures scale them uniformly to two-metre modules and
assemble every building from exposed wall, terrace, and roof faces. Wall
instances are offset and rotated so their normals face away from the building;
no interior, back, bottom, or otherwise hidden cube faces are submitted.
The generator authors Wavefront OBJ geometry in Y-up coordinates because OSG's
OBJ reader performs its default Y-up-to-Z-up conversion while loading.
The tree, fire-hydrant, recessed-window, framed-door, modular fire-escape, and
street-trash-can models are intentionally lightweight procedural kit assets.
Windows and doors project slightly beyond the wall plane, and fire escapes use
a platform, guard rails, and a ladder that joins vertically stacked modules.
The recessed window uses only 18 triangles despite having a pane, projecting
frame, and four reveal faces.
`building` is a complete textured residential model loaded directly from
`models/building.osgb`; the residential fixture instances it as a whole instead
of assembling it from voxel pieces.
`power_tower` and `power_tower2` reference the textured OSGB transmission-tower
assets in the parent data directory at their native 31.7 m and 23.4 m heights.

The generated fixtures intentionally stress the instancing path:

| fixture | buildings | skyscrapers | trees | power towers | windows | doors | fire escapes | trash cans | instances | binary size |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `grid_dense` | 4,900 | 12 | 1,372 | 40 | 592,276 | 4,900 | 1,542 | 490 | 1,524,299 | 17.8 MiB |
| `downtown_dense` | 4,900 | 13 | 1,372 | 40 | 1,015,868 | 4,900 | 6,996 | 490 | 2,377,287 | 27.3 MiB |
| `old_town_dense` | 6,400 | 6 | 1,792 | 40 | 661,578 | 6,400 | 1,932 | 640 | 1,685,288 | 19.8 MiB |
| `residential_dense` | 6,400 | 0 | 1,200 | 40 | 0 | 0 | 0 | 0 | 7,640 | 0.1 MiB |

Every prototype also has an ordinary textured road grid in its generic scene
companion. A road occupies every corridor between adjacent city blocks: 48
quads in `grid_dense` and `downtown_dense`, 63 in `old_town_dense`, and 99 in
`residential_dense`. Horizontal strips own each intersection and the vertical
strips stop at their edges, so the mesh has no coplanar overlapping polygons.
World-space UVs continuously repeat the 512-pixel `textures/asphalt_road.jpg`
surface at a 12 metre scale. The complete grid is only 96-198 triangles per
prototype and is converted to ordinary indexed OpenSceneGraph geometry. For
this proof of concept, road vertices sit 5 metres above the local ground
plane, and the loader applies a
negative filled-polygon depth bias to the named road drawable. This prevents
globe/terrain z-fighting without affecting powerlines or other generic scene
content.

Each prototype also authors four rooftop smoke-stack markers in the companion
scene. The high-detail loader replaces those invisible marker points with real
`osgParticle::SmokeEffect` children using endless emission, a 6 km visibility
limit, and a 160-particle capacity per stack. OSG expands the puffs into
GL3-compatible triangle billboards. Every effect starts from one shared
StateSet and the 256-pixel transparent `textures/smoke_particle.png` sprite;
osgEarth's shader generator then composes the particle rendering with the same
GL3 and logarithmic-depth pipeline as the scene. Voxel-city markers sit on the
highest roof columns of four skyscrapers; the residential prototype puts four
stacks on central-neighborhood roofs so they are easy to inspect from the
startup view.

Every prototype has four straight utility corridors around its perimeter. Nine
full-height towers at 280 m spacing run along both east and west edges, and
eleven smaller towers at 220 m spacing run along both north and south edges.
Their local conductor axes are aligned with each corridor, so the crossarms face
perpendicular to each line. Six conductors connect each neighboring pair at the
tower models' three crossarm levels. Every span uses 24 ordinary GL line
segments and a parabolic midpoint sag, for 216 spans and 5,184 segments per
city prototype.

The roads and conductors live in a generic `*_scene.osgb` companion rather than
in the Kit instance file. The companion is an ordinary OpenSceneGraph node
loaded only with the high-detail city page. It is intentionally not tied to a
particular content type and can later contain arbitrary geometry, transforms,
or other scene-graph nodes without changing the city loader.

Corners and roof edges now emit multiple face instances, but each brick module
contains only two triangles instead of a cube's twelve. Excluding the tree,
hydrant, and facade/street decoration geometry, the brick-shell counts are:

| fixture | cube triangles | quad triangles | reduction |
| --- | ---: | ---: | ---: |
| `grid_dense` | 8,445,792 | 1,847,308 | 78.1% |
| `downtown_dense` | 11,550,744 | 2,695,192 | 76.7% |
| `old_town_dense` | 9,010,920 | 2,025,748 | 77.5% |

Each binary prototype also has a resident low-detail impostor. The generator
writes OBJ source and the example loads its preconverted OSGB form. It uses one
flat-colored box per distinct building height tier, retaining the city
footprints, neighborhood rotations, setbacks, and overall skyline while
omitting voxel-scale detail, facade/street decorations, trees, hydrants, power
towers, and courtyards:

| fixture | detailed shell | impostor | reduction | impostor size |
| --- | ---: | ---: | ---: | ---: |
| `grid_dense` | 1,847,308 triangles | 49,320 triangles | 97.3% | 2.5 MiB OSGB |
| `downtown_dense` | 2,695,192 triangles | 98,030 triangles | 96.4% | 5.1 MiB OSGB |
| `old_town_dense` | 2,025,748 triangles | 64,060 triangles | 96.8% | 3.3 MiB OSGB |
| `residential_dense` | 281,600 house triangles | 64,000 triangles | 77.3% | 3.6 MiB OSGB |

Each voxel neighborhood contains 100 buildings plus street trees and periodic
fire hydrants. Every building gets a door, and every exposed wall module above
the door/ground level gets a recessed window, including setback and courtyard
walls. Rust-colored fire
escapes appear on 30% of low-rise buildings, 55% of tall buildings, and every
skyscraper, with additional vertically connected modules where height permits.
Trash cans are distributed around all four street edges. Taller buildings use
one or two setbacks, and some buildings use open courtyards, giving the
prototypes a deliberately blocky Minecraft skyline.
This is over eleven times as many buildings as the previous nine-per-neighborhood
fixtures, with smaller 10-16 metre typical footprints and mostly 6-16 metre
low-rise heights.

Seven trees per side form a jittered street-tree ring around every neighborhood,
for seven times the previous tree density. Rare skyscraper profiles add 60-116
metre base towers plus one or two setbacks, producing maximum skyline heights of
128-160 metres while leaving the overwhelming majority of buildings low-rise.

The residential fixture covers the same 2446 metre L14 footprint with one
hundred 230 metre blocks. Each block has 64 complete houses plus twelve street
trees, for 6,400 residential buildings at roughly 1,070 buildings/km². Three
discrete scales, two street-facing rotations, and a small reusable RGB palette
provide variation while keeping the binary input compact.

Optional `cities/*.kitcity` files are human-readable OpenSceneGraph node fixtures.
The osgEarth `kitcity` reader turns each file into a hierarchy of
`osg::MatrixTransform` and lightweight `osgEarth::KitNode` objects. Commands are:

```
kitcity 3
transform "name" px py pz qx qy qz qw sx sy sz
instance "model-name" px py pz qx qy qz qw sx sy sz [minRange maxRange] [red green blue]
end
```

The optional `minRange maxRange` pair is an eye-distance visibility interval in
metres, using `[minRange, maxRange)`. Missing ranges and legacy `kitcity 1`
instances are treated as always visible. The generated city files set windows to
`0..1500` metres so they remain visible farther into the detailed-city range
in the distance; walls, roofs, doors, fire escapes, trees, hydrants, and trash
cans remain always visible until the city-level impostor LOD takes over.

The optional RGB values are normalized `0..1` multipliers for the model's
existing color and texture. White (`1 1 1`) leaves a model unchanged, so a
white-textured kit model can be instanced in any RGB color without creating a
new model or draw call. The renderer stores the tint as RGB565 in the low bits
of the quantized scale words, retaining the 20-byte instance record and current
batching behavior.

The generator also writes a `.kitcityb` sibling for each fixture. This binary
format flattens neighborhood transforms, groups instances that share a model,
rotation, scale, visibility range, and tint, and stores only three position
floats per record. Version `OEKITB03` stores RGB tint per batch; `OEKITB02`
stores only the range pair, and `OEKITB01` files still load with always-visible
ranges and white tint. The example automatically prefers
`.kitcityb`; the reader retains text as a debug/fallback format. At startup, the
example loads the named Kit models and discovers the available prototypes. Each
paged tile then independently reads its city file, compiles its instance arrays
and render state, and loads its impostor. Its generic `*_scene.osgb` companion
loads and unloads with the high-detail instance page. Tile construction is
serialized so no OSG compile or database visitor operates concurrently on
shared construction inputs.

Each tile keeps both its own detailed instance batches and corresponding
`*_impostor.osgb` in memory. An `osg::LOD` draws instances within 3,000 metres
and the impostor beyond that range. Override the switch distance with
`--lod-range <metres>`.

The scene is wrapped in an osgEarth `SkyNode` and the example uses the same
ImGui tool panels as `osgearth_imgui`. The Sky panel opens at startup and exposes
date/time, lighting, exposure, ambient and diffuse light, haze, atmosphere, sun,
moon, and star controls. Use `--nogui` to start with the interface hidden and
the `--sky-low`, `--sky-medium`, `--sky-high`, or `--sky-best` switches to select
the initial sky quality preset.

The detailed instance path also uses lightweight culling inside the city. Kit
compilation can split model batches into spatial cells; `osgearth_kit` defaults
to 512 metre cells so off-screen chunks get ordinary OSG bounding-volume culling
instead of drawing the whole city-sized batch. Set `--chunk-size 0` to disable
chunking or `--chunk-size <metres>` to tune the draw-call/culling tradeoff.
Finite-range instance batches, like windows, are also wrapped in a conservative
chunk-level `osg::LOD` so distant chunks stop submitting entirely. The shader
still applies the exact per-instance range inside active chunks.

Coordinates are local east/north/up metres. Each fixture occupies roughly one
equatorial spherical-Mercator level-14 tile (2445.9849 m square). `osgearth_kit`
rescales that footprint for latitude before placing it in a requested tile.

Run `python data/kit/generate_kit_data.py` from any directory to regenerate the
OBJ/MTL and compact binary city files. Add `--text` to also generate the much
larger human-readable `.kitcity` siblings. The existing brick textures and
generated asphalt texture are not recreated. After regenerating, rebuild the
runtime impostors and generic
companion scenes with OSG's binary writer:

```bat
osgearth_shell.bat
build\vcpkg_installed\x64-windows-release\tools\osg\osgconv data\kit\cities\downtown_dense_impostor.obj data\kit\cities\downtown_dense_impostor.osgb
build\vcpkg_installed\x64-windows-release\tools\osg\osgconv data\kit\cities\grid_dense_impostor.obj data\kit\cities\grid_dense_impostor.osgb
build\vcpkg_installed\x64-windows-release\tools\osg\osgconv data\kit\cities\old_town_dense_impostor.obj data\kit\cities\old_town_dense_impostor.osgb
build\vcpkg_installed\x64-windows-release\tools\osg\osgconv data\kit\cities\residential_dense_impostor.obj data\kit\cities\residential_dense_impostor.osgb
build\vcpkg_installed\x64-windows-release\tools\osg\osgconv -O WriteImageHint=UseExternal data\kit\cities\downtown_dense_scene.obj data\kit\cities\downtown_dense_scene.osgb
build\vcpkg_installed\x64-windows-release\tools\osg\osgconv -O WriteImageHint=UseExternal data\kit\cities\grid_dense_scene.obj data\kit\cities\grid_dense_scene.osgb
build\vcpkg_installed\x64-windows-release\tools\osg\osgconv -O WriteImageHint=UseExternal data\kit\cities\old_town_dense_scene.obj data\kit\cities\old_town_dense_scene.osgb
build\vcpkg_installed\x64-windows-release\tools\osg\osgconv -O WriteImageHint=UseExternal data\kit\cities\residential_dense_scene.obj data\kit\cities\residential_dense_scene.osgb
```

The application intentionally requires the OSGB files instead of falling back
to OBJ, since OBJ parsing was the dominant per-tile load cost. Keep
`WriteImageHint=UseExternal` on companion-scene conversions so independently
loaded city scenes all reference the small shared asphalt file instead of
embedding a full image allocation in every OSGB.

After building, run the example from the repository root so the default data
paths resolve:

```bat
osgearth_shell.bat
osgearth_kit
```

The default globe uses OpenStreetMap imagery. Use `osgearth_kit --no-imagery`
for an offline globe, `--earth path\to\map.earth` for another base map, or
`--validate-only` to load and batch a prototype without opening a viewer.
Use `--chunk-size <metres>` to tune spatial batch culling and `--lod-range
<metres>` to tune the detailed-city/impostor switch distance.
Use `--view-range <metres>` to move the initial camera closer for inspecting
roads, rooftop placement, and particle effects. `--view-longitude <degrees>`
and `--view-latitude <degrees>` can aim the initial/direct-tile diagnostic at a
specific position.

Use `--stress-paging` to move the camera across adjacent level-14 tiles and
render many independently loaded placements of these 7.6K-2.38M-instance city
batches.
