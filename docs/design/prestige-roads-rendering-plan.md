# Prestige Roads rendering implementation plan

**Driving policy (October 23, 2026).** Visual believability outranks source fidelity in all cases, for OSM
geometry and for terrain grading: the output serves a 3D simulation, and immersion matters more than geospatial
accuracy. [POLICY.md](../../tools/experiments/prestige_roads/POLICY.md) is authoritative and overrides any gate or
working rule below that conflicts with it. Stages 0–5 below record what was built under the earlier
source-fidelity rules; the plan continues in
[REVIEW_2026-10-23.md](../../tools/experiments/prestige_roads/REVIEW_2026-10-23.md).

October 12, 2026. Status: three Stage 0 fixtures and the minimal ownership contract are implemented.
The Stage 1 asphalt prototype runs on regular terrain in all three cities; its full correctness gate remains open.
Broader performance tuning is deferred; the user-requested R8 coverage conversion is complete and measured. See the
[asphalt prototype report](../../tools/experiments/prestige_roads/SURFACE_SPLATTING.md) for evidence and remaining checks.
Stage 2 now has a [first marking comparison](../../tools/experiments/prestige_roads/MARKINGS.md): terrain paint
coverage is the provisional ground backend; the GPU depth-clamped decal baseline failed close-range attachment.
Stage 3 now has a [Congress Street bridge slice](../../tools/experiments/prestige_roads/BRIDGE_SLICE.md),
with generated shared materials, a solid roadway slab and paint partitioned into its deck triangles.
The user's subsequent direction resumed grading integration. `GradingLayer` now provides centerline-driven
grading and shared junction planes in all three cities. Boston's Congress Street bridge now consumes one
solved profile together with its graded approaches; see [bridge grading](../../tools/experiments/prestige_roads/BRIDGE_GRADING.md).
The user next selected Springfield as the multilayer stress case. Its separate
[network experiment](../../tools/experiments/prestige_roads/SPRINGFIELD_INTERCHANGE.md) implements relative-layer
crossing constraints, shared merge profiles and raised-approach receiver inference. Full lane layouts and
regional paging remain open; the three city fixtures are preserved.
Sion now exercises joined deck geometry and inferred bridge railings. Its 78 source surfaces assemble into
49 deck sheets, with source-connected merge mouths open and stacked crossings kept separate. See the
[Sion assembly update](../../tools/experiments/prestige_roads/SION_CITY.md#joined-decks-and-railings).

Implements the [rendering architecture](prestige-roads-rendering.md). Work from one useful visual slice
to the next. Resolve terrain attachment and marking quality before expanding the geographic footprint.
Each stage ends with inspectable output, measured evidence, and a recorded decision.

The first sequence is data contract, asphalt, marking comparison, one structure, then broader structure cases.
The shared-profile grading interface is implemented. Dynamic attachment remains a bounded validation gate;
elevated joins now have a bounded Springfield prototype, while full regional structure paging remains open.
Use the existing regular terrain/elevation stack as an input throughout the active sequence.

## Stage 0: freeze inputs and define the smallest render contract

Use three fixed 2 km by 2 km local-metric squares, each containing source-tagged road bridges:

| Fixture | Center (longitude, latitude) | Coverage and purpose |
| --- | --- | --- |
| Central Boston | -71.0565, 42.3515 | Downtown, Fort Point bridges, South Station elevated ramps |
| Central NYC | -73.9950, 40.7095 | Lower Manhattan, East River bridges and Brooklyn approaches |
| Central London | -0.1130, 51.5060 | Westminster, Waterloo and Blackfriars; left-hand traffic |

These replace the earlier single-city fixture selection. Preserve the older Boston samples as regression
references. Compile the three fixtures from source with `city_fixtures.py`; retain a context halo and complete
returned source ways before cropping material output. Use the same fixtures at each later renderer gate,
starting implementation on one representative intersection and checking the other two cities before advancing.
The [fixture report](../../tools/experiments/prestige_roads/RENDER_FIXTURES.md) records output and limitations.

Record source/compiler hashes, camera poses, terrain settings, and the current vector-geometry baseline.
Retain the corrected double-precision triangulation path.

Define stable surface/marking ownership, material identity, geographic coordinates, local metric frames,
structure connections, and vertical profiles. Ownership belongs to the normalized design network (links and
junction clusters); visual overlap and proximity are legitimate inputs to that normalization (POLICY, rule 3).

Keep the current structure-file packaging. Consumers select its material features and shared surface IDs.
Preserve raw layer/level/incline values as evidence; every height is resolved by design, none stays unknown.
Distinguish source graph incidence from resolved lane movements and permissions.

**Deliverable:** a versioned minimal data contract and reproducible Boston, NYC, and London fixtures.

**Gate:** IDs and references resolve; the design network is the geometry authority, and the normalizer heals
near-misses and forms junction clusters; units and source evidence are logged, never a constraint on geometry.
Avoid choosing a new binary storage format or building a universal material framework in this stage.

**Current result:** attributed GeoJSON copies identify compiler junction owners plus one explicit shared
network remainder per city. The original vectors and connectivity are unchanged. Ownership does not imply
per-road corridor resolution, and individual paint-to-source evidence links remain a later refinement.
The optional GeoPackage is a disposable spatial index of unchanged GeoJSON, not a compiler format change.

## Stage 1: one asphalt material on ordinary terrain

Implement the smallest Prestige splatting path: compiled pavement coverage plus one reusable asphalt
PBR material. Use the current terrain without new grading. Establish metric texture scale, normal-map
conventions, lighting order, hole preservation, page borders, and fallbacks for absent textures.

**Coverage activation:** start asphalt and ground paint at global-geodetic LOD 15 in the demo settings,
using the existing image-layer `min_level` option. The layer and reusable materials may initialize earlier;
coverage tile creation and terrain visibility begin at the configured level. Do not generate road coverage
for the lower levels. Keep both materials on the same initial cutoff, then inspect the zoom-in transition
for popping and isolated paint before choosing a final threshold or fade. LOD is a tile-profile level,
not a fixed camera altitude. Reduced coarse-page work is expected; no speedup has been measured.
The Boston runtime check now rejects LOD 14 requests and accepts LOD 15; paired views confirm absence/presence.
These checks establish the cutoff, not a finished visual fade through it.

Begin with conventional texture bindings and the existing PBR decoding/preparation facilities.
Add larger-scale variation and optional anti-tiling only after the simple baseline works and is measured.
Do not introduce parallax, displacement, or multiple new material families to make the first demo attractive.

**Coverage encoding update:** the user requested R8 to reduce tile storage and upload bandwidth. All ground
splatting masks now store one linear coverage channel, with exact alpha-to-red conversion before caching.
The existing Blend2D rasterizer still has an RGBA scratch buffer. Both renderers decode red before parent
fallback/morphing and opacity. Separate material passes remain; consolidation is explicitly deferred.
Three-city coverage, old/new caches, GPU readback, and parent-blend checks pass. The host upload payload is
75% smaller; [measured upload results](../../tools/experiments/prestige_roads/SURFACE_SPLATTING.md#r8-coverage-validation-and-upload-measurement)
distinguish NVGL's previous driver compression from the bandwidth-only control. This does not migrate shared materials to arenas.

**Deferred implementation note (user-requested):** migrate splatting material/texture storage and bindings
to osgEarth's `MaterialArena` and `TextureArena` after the current visual work. Share material records and
texture residency, retain the same coverage and metric-coordinate contract, and revisit the layer-local
Texture2D upload workaround when adopting the arena upload path. Do not implement this migration now;
the immediate next step is markings.

**Deliverable:** the Boston intersection with inspectable pavement boundaries and close asphalt detail,
then the same material/coverage path on the NYC and London fixtures.

**Gate:** no pavement outside its intended coverage; islands remain open; texture scale/phase is stable
under camera movement and tile changes; shadows and lighting respond correctly. GPU cost,
material/coverage residency, and page-generation comparisons are deferred until performance work resumes.

**Current result:** separate Prestige image layer, metric material frame, existing ReadyMap terrain,
preserved polygon holes, and ordinary/GL4 shader paths are implemented. Intersection views from all cities
and Boston detail/grazing views are saved. Remaining correctness work includes dedicated normal-map,
shadow/depth/picking tests and a repeatable forced-LOD camera run. Marking backend work follows this asphalt slice.

## Stage 2: choose the marking backend using a controlled comparison

**Current result:** all three ground marking packages render through paint coverage. Boston mesh-decal
comparison exposes an unresolved GPU-clamping failure, while a CPU-clamped diagnostic retains the same
triangles. Paint now disables the rasterizer's implicit seam outline to avoid inflating narrow strokes.
The current mask uses parent/child coverage at all distances; no dual-representation handoff is implemented.
Exact-grid decals, exhaustive forced-LOD tests and page/revision coordination remain open. See the marking report.

Render identical white/yellow polygons, arrows, stop bars, and crosswalks through two paths:

1. Batched nearby mesh decals with reusable paint materials.
2. Paint coverage evaluated in the terrain material shader, with a documented texel-density budget.

For decals, implement a bounded terrain-attachment spike that follows actual regular-grid triangle
interpolation. Exercise LOD morphing immediately. The existing GPU depth-clamping path is a baseline,
not proof that final decal conformity is solved. Share material coordinates and geometric normals where
possible; compare paint roughness/lighting, not just flat color.

Measure shader/draw cost, triangles, memory, build latency, and cold-loading. Inspect source-width
stripes, arrow corners, crosswalk spacing, grazing views, page edges, and movement. Use enlarged static
images and a repeatable moving-camera run. Check both coarse and fine terrain settings.

**Deliverable:** a saved side-by-side comparison and a decision recorded in the architecture.

**Gate:** choose decals only if conformity, depth stability, and cost justify them. Choose receiver masks
if decals need visible offsets or excessive tessellation; document the mask memory/quality tradeoff.
Keep the same source contract whichever wins. Screen-space projected decals become a third bounded
experiment only if the primary candidates leave a specific unresolved problem.

Use the three fixed fixture regions without expanding them until a marking backend passes.
Implement one-owner handoff to distant coverage
and verify that a marking is not doubled or dropped when its representation changes.

## Stage 3: one generated bridge with shared appearance

**Current result:** Congress Street's three source-connected segments render as one closed roadway slab.
Asphalt and yellow paint share the ground renderer's generated color/roughness maps and metric texture phase;
the underside and fascia use generated concrete. Paint is part of the solved deck triangulation, with no overlay bias.
The temporary profile is explicitly inferred and remains steep at the east approach. It is not a surveyed bridge design.
Source vector footprints and connectivity remain unchanged. Detailed landing refinement has stopped as requested.

Choose a simple connected bridge/approach case from the three retained fixtures. Prepare one explicit
vertical profile using known observations or clearly identified inference. Keep datum, grade, clearance,
and uncertainty in the output. Do not turn layer numbers into assumed meter heights.

Build the whole connected deck before paging it. Generate the minimum useful solid: top, underside,
and edges. Reuse the asphalt material and selected marking backend. Bind paint to the deck surface
so another road beneath it cannot receive the markings.

For the first visual slice, hold a documented terrain LOD fixed while establishing the shared landing
cross-sections and material continuity. This does not complete dynamic terrain attachment.

**Deliverable:** one bridge and approaches rendered with the same asphalt and paint as the ground roads.

**Gate:** valid geometry, correct source connectivity, matching landing widths, consistent material scale,
and no wrong-deck paint. Audit the chosen profile's grades and clearance against its declared assumptions.
If the source cannot support a profile, design a plausible one and log the inference; a structure is never left
unresolved or unbuilt (POLICY, rule 2).

## Stage 4: share the road profile with grading and validate the landing

**Current result:** Boston has a separate shared-profile prototype. An opt-in absolute-profile mode in
GradingLayer consumes compiler-owned centerline Z, while the bridge generator uses those same samples.
Regular native-LOD-16 grids modify the connected ground approaches and short landing support areas only.
The central span's underlying terrain remains independent. Ten-meter endpoint transitions rejoin existing
road grading. A separately attributed three-centimeter terrain recess beneath the deck overlap avoids
coplanar flicker; the deck itself is not raised to conceal an attachment error.

The Boston report records native-grid, forced render-LOD and moving-camera checks. These are a local
prototype, not a completed runtime revision/attachment service. Inferred heights and support pads remain
explicit. Live elevation revision handling, arbitrary terrain LODs and elevated junctions remain open.

Extend the terrain attachment interface established during the marking experiment. Connect the terrain-facing
landing edge to rendered grid geometry and morph state, while the span retains its resolved profile.
Define ownership of the short attachment region and how material/marking continuity crosses it.

Coordinate terrain/decal/landing revisions and replacement content. Avoid using a large vertical bias
to conceal geometric error. Inspect the top surface and the underside separately; an abutment skirt
must not be accepted as proof that the driving surface meets correctly.

**Deliverable:** a repeatable camera tour over one landing with forced terrain LOD changes and cold loads.

**Gate:** no visible opening, doubled paint, unstable depth, or abrupt grade at the landing over the tested
LOD range. Record maximum measured boundary mismatch and test settings. Invalidate the terrain input
and verify stale attachment results cannot publish. Do not proceed to a complex interchange if this fails.

## Stage 5: extend to ramps and regional paging

Retain the compiler's **fat-merge policy** as the default for compatible source-connected deck joins:
shared cross-section envelopes for equal or unequal widths, bounded rounding of inside corners, and
railing extraction after the footprint is complete. Preserve source evidence and export the added areas
as explicit inference. Keep unrelated receivers, islands, and ordered crossings protected. The Sion
equal-width end-cap regression and its current terrain-validation limitation are recorded in
[the Sion report](../../tools/experiments/prestige_roads/SION_CITY.md#equal-width-joins-and-the-retained-fat-merge-policy).

Add one curved elevated ramp and one stacked-road crossing before exercising all three complete fixture
regions. Resolve elevated merge/junction geometry where needed; preserve unresolved cases as diagnostics.
Add structural detail and ground curb geometry after the shared surface/attachment policies hold up.

**Bounded implementation:** the Sion compiler now unions connected deck footprints, partitions asphalt and
paint on the resulting shared triangles, and removes internal slab faces. Deck sheets that cannot be unioned
in XY because the network crosses itself retain separate geometry with matching join profiles and open
fascia. Railings follow exposed assembly edges, leaving connected roads and landings open; their dimensions
and material are recorded as inferred. A same-layer collision between unconnected, inferred-width edges is
trimmed explicitly instead of creating a graph connection. Railings are a visual prototype; mapped barriers,
lane-level ramp layouts and regional structure paging remain further work.

**Bridge family pass:** the compiler now selects basic beam, truss, arch, suspension, and cable-stayed
templates from retained road/outline attribution. Sion exercises beam supports, a truss cycle bridge,
and a shared cable-stayed assembly for two carriageways. Inferred dimensions, tower stations and support
placement remain separate from mapped family evidence. Unsupported families and unresolved mapped-support
associations defer. The cable-span profile pass removes interior DEM undulation while retaining landing
anchors and crossing clearance. See [the bridge-family report](../../tools/experiments/prestige_roads/BRIDGE_FAMILIES.md)
for the exact scope, checks, review scenes, and remaining foundation/water work.

**Structural edge policy:** truss, arch and cable families now replace generic railings within their own accepted
edge-girder coverage. Trusses default to an explicitly inferred Parker form with Pratt webs and seated
lower-chord gussets; cable stays anchor into blocks on slab-connected girders. Divided carriageways keep
separate edges and an open median. Failed replacements retain the original railings, and short landing
transitions remain bounded. Keep this policy when adding future bridge families; do not attach structural
members beside an unrelated railing template. Sion's deck, paint, grading profiles and approved fat merges
remain unchanged by this edge pass. London's through-arch template now uses slab-connected tie girders,
springing blocks and hanger anchorages, with required members and geometric seating checks. A common planner
gate rejects primary structures that omit their edge attachment. Arch ties preserve pavement holes and keep
out of approach junctions within a recorded bounded end transition. Mapped subtype selection remains further work.


**Deferred bridge paging:** add a Prestige bridge layer based on `TiledModelLayer` and its `SimplePager`.
Use independent minimum/maximum levels and distance or screen-size refinement for bridge content; retain
the single native grading LOD independently. Page current meshes first, then introduce compiler-generated
distant representations as a separate step; paging alone does not simplify geometry. Assign each connected
assembly explicit tile ownership and full three-dimensional bounds, including towers and cables, so
cross-tile bridges neither duplicate nor disappear. This work is deferred while grading is refined.

Page material control data, markings, and model geometry using bounded worker queues and residency budgets.
Skip empty pages, retain usable parents, cancel stale jobs, and schedule uploads without blocking the render
thread. Keep complete topology/profile context before spatially splitting render output.

**Deliverable:** all three 2 km fixture regions at street, aerial, and distant scales, including their structures.

**Gate:** report steady-state and peak memory, GPU/frame cost, draws, triangles, page-build/upload latency,
and a cold-cache traverse. Include parent/staging memory. No unbounded per-fragment search of all road
vectors, no per-dash drawables, and no project-wide high-resolution color pyramid.
State supported cases and unresolved structure layouts explicitly.

## Grading integration: centerline profiles on one native grid LOD

The user resumed integration. The other worktree's Prestige component is now `GradingLayer` in this
worktree, including the shared tile-fallback fix. The initial Boston junction-pad check is superseded by
the three-city road prototype in [ROAD_GRADING.md](../../tools/experiments/prestige_roads/ROAD_GRADING.md).
Boston, NYC, and London now pair complete source centerlines with authoritative compiled pavement masks.
The layer samples preceding ReadyMap elevation along each alignment and applies those longitudinal profiles
across the paired footprint. Grading uses sparse global-geodetic LOD 16 grids only, with finer terrain using
parent data and no-data falling through to the base. Asphalt and markings consume that same terrain surface.

The new opt-in footprint policy leaves existing line corridors and level pads available. The adapter bypasses
OSM width inference and keeps structure vectors out. Rounded shared-network fill has an explicitly inferred
height association. The NYC junction crease is now corrected: junctions fit a common terrain-derived plane,
shared by explicitly identified approach profiles, with thirty-meter transitions matching their height and
slope. The retained before/after comparison and transient regression cover that failure. Source elevation can
still imply unrealistic grades; grade limits and complex neighboring junctions need further policy work.
Boston bridge ground transitions now use the shared absolute-profile mode. Buildings and inland water remain deferred.

The three fixtures have passed footprint, sparse coverage, shared-edge and native-LOD fallback checks,
with close views saved. These bounded checks do not certify every junction across the regions.
Solved-profile input explicitly selects absolute elevations; existing terrain-derived inputs continue to ignore Z.
The Sion embankment correction splits geometry ownership inside a ground-tagged source way: only
lower-road exclusions and bounded landing windows use separate decks; the remaining approach uses
graded terrain. Internal handoffs open the slab/railing boundary and carry a small grid support apron.
Keep solved source profiles and attribution intact, and retain source evidence separately from these
inferred receiver boundaries. Absolute footprint weighting protects road interiors from neighboring
shoulders; native grids still require explicit handoff and lower-road clearance checks.

**First inland-water grading prototype:** Sion now has a separate compiler pass for downstream river
profiles and level pond references. It exports explicit inferred bed depths separately from future water
surface references, with indexed FlatGeobuf runtime inputs. GradingLayer applies inward bank transitions
and cut-only elevation at the same native LOD 16, 257-square resolution. Mapped culverts remain covered;
ground roads, landing supports, rail corridors, and foundations are protected. Existing decks, approved
fat merges, and road profiles are unchanged. Water rendering, confluence/depth transitions, surveyed
bathymetry, and narrow-channel/coarse-terrain refinement remain later work. See the Sion experiment report.

## Working rules and evidence

Sion's runtime packaging now uses spatially indexed FlatGeobuf, with expensive pavement polygons divided
on coverage-aligned grid boundaries and grading profiles retained intact. GeoJSON remains the inspection
output. The measured native tile costs, duplicate-query correction, and coverage/elevation comparisons are
recorded in [RUNTIME_VECTORS.md](../../tools/experiments/prestige_roads/RUNTIME_VECTORS.md).

* Save a short result, images, settings, and limitations at each gate. Add benchmark numbers when performance
  work resumes. A static overview
  does not substitute for a street-level movement/LOD check.
* Keep regression and benchmark harnesses transient outside the repository unless instructed otherwise.
  Compare identical inputs and validate behavior before claiming a performance improvement.
* Use `configure.bat` and `build.bat` for Windows configuration/builds. Initialize `osgearth_shell.bat`
  before running osgEarth tools; run relevant existing unit tests from `tests`.
* Preserve C++17 compatibility and repository precision/threading conventions. No public API is committed
  by the names in the design; bump the library ABI version when an actual public ABI change requires it.
* Use explicit quality and residency settings; record the GPU, viewport, antialiasing, and terrain detail
  with every comparison. Establish numerical budgets from the initial measurements, not guessed frame rates.
* The compiler's normalized design network is the geometry authority; the renderer consumes it. Correct
  demonstrated defects in the compiler with a visual regression case (a capture or a hillshade dump), judged by
  believability, not by fidelity to the source.
* Grading is the primary means of shaping the ground; tune its parameters before reaching for a constraint.
  `TerrainConstraint` layers (trench cut-outs, edge breaklines, structure end-cap pins) are separately toggleable
  refinements for what grading cannot draw; the scene should degrade gracefully, not break, with them off.

## Appendix: future nice-to-haves

These appearance improvements are deferred for a later revisit, not part of the current implementation gate.
The bridge slice now supplies basic procedural albedo and roughness for white/yellow paint, as part of the
user-authorized material creation. The normal relief and sparse wear below remain deferred.

* **Dedicated road-paint material:** subtle grain, distinct roughness, and restrained normal-map relief for
  white and yellow paint. Preserve the existing marking shapes and keep material scale consistent with asphalt.
* **Sparse flaking:** add an optional procedural wear mask separate from authoritative vector coverage.
  Anchor chips in stable metric road/surface coordinates so they do not swim with the camera or restart at
  tile borders. Apply the wear consistently to paint color and material response so exposed patches show asphalt.
  Keep damage restrained and filter fine wear with distance to preserve stripe, arrow, and crosswalk readability.
  Evaluate mask generation versus material-shader evaluation when this work resumes; no backend is chosen yet.
