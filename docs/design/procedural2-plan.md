# Prestige Vegetation Master Plan

Updated October 5, 2026. **Current checkpoint: Steps 4A/4B cleanup complete. Simulation placement-query API remains Later.**
The earlier experiments and visual checkpoints are retained in [development history](procedural2-plan-history.md).
This document describes the current direction; historical examples are not a configuration reference.

## Architecture and feature set

`osgEarthPrestige` contains the vegetation implementation (`OSGEARTH_BUILD_PRESTIGE_NODEKIT`).
`osgEarthPrestige::VegetationLayer`, registered as `prestige:vegetation`, consumes placement policy and renders through Chonk and SimplePager.
Vegetation source cells and render pages are independent of terrain tiles. The current rendering path requires NVGL.
Include `<osgEarthPrestige/VegetationLayer>` and link `osgEarthPrestige` for direct C++ use.
Earth files load `<libraries>osgEarthPrestige</libraries>` and configure `<prestige:vegetation>`.
The demo executable remains `osgearth_procedural2`; assets and example filenames retain their existing paths.

- Independent trees, shrubs, grass, undergrowth, and rocks populations with density, scale, assets, and range.
- Deterministic natural scatter, explicit source points, and registered land-use placement strategies (rows/grids today).
  Vineyard/crop/orchard layouts use stable metric frames across page boundaries. Lawn/playground and other strategies
  can be added without teaching the OSM adapter about their algorithms.
- Multiple geographic inputs: osgEarth FeatureSources, including tiled OSM, and application feature providers.
  Attribute rules compile inclusion, fractional density, hard polygon exclusions, and buffered line exclusions into
  a common CPU coverage field. Generated placements respect road buffers and polygon exclusions. Explicit points
  bypass line buffers but still respect building/water/clearing polygons in both exact and coverage tiers.
- CPU elevation attachment on paging workers; double-precision geographic positions become page-local transforms.
- Asset catalog with demand loading and ownership-based release, PBR/VRV packed material support, source-derived
  impostors, normal maps, and precompressed DDS mip chains. External VRV assets remain local dependencies.
- A2C alpha ramps for visual fades, hard cutouts for single-sample targets, and leaf-bearing shadows. No stochastic
  fade dithering. Vegetation inherits application multisampling; launch viewers with `--samples 4`.
- Optional procedural grass remains an isolated experiment; ordinary asset grass remains supported.

### Source composition and editable exclusions

Pipeline: **named inputs -> attribute rules / coverage field -> placement strategy -> final elevation attachment -> rendering**.

Editable features are another named `PlacementFeatureProvider`, implemented by `EditableFeatureProvider` over a
standalone `FeatureOverlay` document. `VegetationLayer` neither owns an edit document nor exposes an editing API.
The reusable `FeatureOverlayGUI` demo edits a document directly and can be used by another application.
`OverlayStorage` owns persistence; GeoJSON is the initial adapter, not the production storage contract.

Providers expose revisions, immutable snapshots, and changed geographic extents. The layer consumes those generic
notifications to invalidate affected pages and their aggregate ancestors. Moves invalidate both old and new regions;
worker revisions prevent stale results from repopulating cleared areas. A source field query retains one immutable
editable-input snapshot. Custom placement-generator overrides remain supported.

The authoring vocabulary supplies ordinary attributes; shared rules determine their effect. The layer does not
hard-code footpaths, clearing tools, or editor policy. Catalog editing, feathered painting, and parameter modifiers
remain Later. Land cover describes natural surface cover; land use describes human use such as vineyards or roads.

See [composition, persistence, and editor details](procedural2-feature-overlays.md) and
[coverage/placement design](procedural2-coverage-design.md).

### Distance representation and controls

Near rendering uses source models and their impostors. Two optional distant tiers remain available:

| Mode | Medium | Far | Tradeoff |
| --- | --- | --- | --- |
| Exact tree cards | Groups of placed-tree impostors | Larger groups of the same impostors | Preserves placements; still pays for each retained tree |
| Coverage stands (experimental) | Representative cards generated from coverage | Reusable multi-tree stands at authored size; smaller cards at exclusion edges | Faster-to-generate approximation; spacing/species need not match exact trees |

A tree card is textured polygon geometry. Exact grouping reduces per-instance bookkeeping/culling; it does not by
itself simplify every tree. Coverage stands use existing baked art; neither mode bakes textures while paging.
Keep both modes available for comparison until the approximate representation is judged satisfactory.
Defaults are exact medium clusters at 90% retention, far coverage stands at 50% retention, and a quality offset of
128 pixels. The main and portable forest demos use these values explicitly. Medium and far representation selectors
are independent, so exact medium clusters can hand off to far coverage stands. Apply rebuilds the population pages. The legacy `canopy_strategy` key
initializes both tiers; `canopy_mid_strategy` and `canopy_far_strategy` override their respective tiers.
Validation: optimized build and 91 selected vegetation/Chonk cases pass, including all four strategy combinations,
independent retention, medium-only operation, and legacy configuration round trips. A mixed exact-medium / coverage-far
forest capture loaded both tiers with no vegetation asset failures or budget denials (`build/tier-strategies-mixed.png`).

- **Maximum range** caps how far trees remain visible, with a distance fade near that limit.
- **Global SSE + population quality offset** selects individual LOD and medium/far handovers. Higher error brings
  coarser representations closer. Trees are not removed merely because each member projects below that budget.
- **Tier selection**: individuals, medium, or medium + far. Turning a tier off substitutes the remaining representation;
  it does not remove distant forest.
- **Medium/far retention** selects stable percentages before packing. Exact mode retains a subset of tree placements;
  coverage mode thins representative cards or whole stands. Stand removal can produce larger gaps. Density controls
  the underlying population separately. Non-tree distance thinning remains available where applicable.
- **Advanced**: source/render levels, group sizes, GPU culling, and transition settings. Group size changes batching,
  not tree dimensions. Debug tier/cluster colors and residency diagnostics are collapsed, optional tools.
- Quality and debug edits apply live. Other population changes use Apply; coarse-model threshold remains an Apply
  setting. Editor/source edits invalidate geographic regions, independently of population Apply.

### Accepted placement caps and generation work

`max_per_cell` and `max_per_batch` cap **accepted** placements. Built-in generators apply exclusions, ownership,
duplicate-ID removal and request/tier retention first, then choose a stable hash-ranked subset. A cap is successful
truncation, not a missing page. Ranking uses a stream independent of coordinates and retention; it avoids choosing the
first side of a cell/batch visited, while leaving surviving positions, scales, rotations and identities unchanged.
It is statistical spatial coverage, not a guaranteed minimum-spacing algorithm. Capped results report
`Placement cap reached` through the request ProgressCallback message.

`max_candidates_per_cell` is a separate pre-filter generation safeguard, default 2,000,000 (allowed 1..2,000,000).
It bounds natural candidate arrays, row candidate grids/region output, and coverage-page representatives including
boundary expansion. `max_work_per_request` adds a cumulative page budget (default 16,000,000; allowed up to
64,000,000). It counts generated candidate slots and subsequent processing at composition boundaries, so it is a
work-unit ceiling rather than a final instance count. Natural and region generators also retain their local bounds.
These are request-local safeguards, not a total per-frame or globally shared budget. Existing feature, vertex and
source-cell-count safeguards remain. Exhausting work guards,
source errors and cancellation still fail atomically; they never publish a partly evaluated forest as complete.

The panel separates accepted caps from **Generation safeguards (advanced)**. Apply reloads the population. Density,
range, quality and tier retention remain the ordinary visual controls. Coverage mode counts representative trees and
whole stands together against its batch cap; exact mode counts retained tree placements before grouping.

Validation: optimized RelWithDebInfo build and 90 selected vegetation/Chonk tests pass after this change. Tests cover
post-exclusion/retention caps, row and coverage-stand limits, source-order invariance, duplicate points, spatial spread,
cancellation, and cumulative work exhaustion. A four-sample render with 8/source-cell and 32/batch caps retained 64
visible-area resident instances across two batches, with successful paging and no asset-load failures. The fixture's
normal configuration was unchanged; the low-cap scene and capture are temporary build outputs.


## Cleanup checkpoint

- Removed retired stretched/nine-crown canopy assembly and its obsolete tests.
- Removed inert `canopy_mid_patch_scale`, `canopy_far_patch_scale`, `canopy_cover_scale`, `canopy_height`, and
  `canopy_pixels` compatibility settings. Tree `min_pixels` is no longer parsed or emitted.
- Simplified the panel, retained real tuning controls under Advanced, and removed the misleading normal-grass
  procedural toggle. Procedural grass is selected explicitly in its dedicated configuration.
- Removed layer-specific overlay ownership/edit methods. Local documents are configured as ordinary source inputs.
- Placeholder vegetation is automatic when the asset catalog is empty; no `demo` property is needed. Opening the
  layer logs a console notice when using placeholders. With neither a catalog nor a groups element, configuration
  supplies the five default populations; an explicitly empty groups element remains empty.
- A configured catalog requires valid model selections. Missing or unloadable named assets report errors and never
  silently substitute placeholders. With no geographic/custom source, synthetic scatter covers the layer profile;
  this also logs a notice and can cover oceans. A profile is still required.
  Automatic-placeholder validation: optimized build and 90 selected regression cases pass, including real page creation
  without an asset catalog, configured missing-model failure without fallback, and the emitted console notice.
- Migrated owned examples to the new source composition and automatic placeholder behavior. Focused art, rows, grass,
  transition, and legacy static-stand scenes remain test fixtures, not competing recommendations for latest testing.
- Consolidated current design here; retained historical notes separately instead of silently losing decisions.

## Which demo to use

| Scene (under `tests`) | Purpose |
| --- | --- |
| `a.earth` | Main working scene, including the external `D:/data/assets/vrv-vegetation` library |
| `procedural2-canopy-osm.earth` | Portable OSM forest and exact/coverage comparison with bundled assets |
| `procedural2-coverage.earth` | Offline geographic coverage/exclusion fixture and local editing |
| `procedural2-rows.earth` | Structured-layout fixture |
| `procedural2-grass.earth` | Optional procedural-grass experiment |
| `procedural2-art.earth` | Focused material/asset inspection |
| `procedural2.earth` | Explicit synthetic whole-earth paging smoke test |

`procedural2-osm.earth` exercises multiple OSM populations. `prestige.earth` is the combined building/vegetation
integration scene. Other named transition/static-stand scenes are historical or focused diagnostics.

From `tests`, after the optimized build:

```bat
call ..\osgearth_shell.bat
osgearth_imgui a.earth --prestige-sky --shadows --nvgl --samples 4
```

Open **Prestige > Vegetation** for the tuning panel. It is available when a Prestige vegetation layer is present.
All commands follow the repository build scripts; the current optimized configuration is RelWithDebInfo.


### Cleanup validation

RelWithDebInfo build passed. `osgearth_tests "[procedural2]~[lighting],[chonk]"` passed 88 cases, including source
snapshot/edit invalidation, custom generator overrides, default population behavior, tier modes, and GPU A2C/MSAA checks.
The separate known lighting-tolerance suite was not part of this cleanup selection. No benchmarks were generated.

Four-sample captures of the geographic fixture and `a.earth` passed paging checks with zero vegetation asset-load
failures or budget denials. The main view contained individual, medium, and far content. Its separate Prestige layer
reported missing external GLB building files. Captures are local build outputs (`build/cleanup-coverage.png` and
`build/cleanup-forest.png`); no new render artwork is required by the source changes.

Test-source cleanup, October 2, 2026: at the user's request, dedicated Prestige vegetation regression source files and
their helper/build registration were removed. Test counts above describe validation performed before removal;
the application/demo, asset tools, and shared engine tests remain available.

## Plan and TODO

| Step | State |
| --- | --- |
| 1: separate NodeKit, global independent paging | Implemented |
| 2: sources/placement foundation, assets, elevation, controls | Implemented |
| Optional procedural grass | Experimental, isolated |
| 3: detailed/coarse art, LOD, residency, fades | Implemented; loading smoothness remains a follow-up |
| 4A: OSM composition, exclusions, extensible rows | Implemented; document authoring modularized in cleanup |
| 4B: distant representations | Exact baseline + optional coverage experiment; visual/performance tradeoffs still open |
| Cleanup | Complete: optimized build, 88 selected regression cases, fixture and main-scene render checks |
| Simulation placement query | Next planned API after cleanup; design below, not implemented |
| Dense-area coarse-page overflow | Later / TODO: oversized source queries must not block detailed descendants |

Known follow-ups: page activation/retirement bursts and incomplete ICO preparation remain separate investigations.
Do not claim that this cleanup fixes frame stalls. Source adapters, asset residency, and bounded caches must scale
without storing unique worldwide vegetation art. No benchmarks are requested; use correctness tests and visual demos.

### Later -- dense-area coarse-page query overflow

**TODO / deferred October 2, 2026; no fix implemented.** Solve missing vegetation caused by an oversized ancestor
request, separately from exclusion policy, thinning, or frame-stall investigations.

User-visible symptoms:

- Whole rows or patches of explicitly mapped street trees are absent even close to the camera. The OSM background
  map shows tree symbols along the sidewalks, but no corresponding 3D trees appear between the buildings.
- Proximity to buildings or paths makes this look like overly aggressive exclusions. It can persist after explicitly
  mapped trees bypass linear buffers, even when the source points are outside every building footprint.
- Fine placement queries can succeed while the rendered hierarchy has no resident trees for the area. The parent
  page fails before its detailed descendants become available; this is not ordinary distance fading or intentional
  90%/50% tier retention. Removing building exclusions is not the remedy for this failure.

Reproduction/evidence: Avenue de la Bourdonnais, Paris, near longitude 2.3024, latitude 48.8562, using the main
`tests/a.earth` scene and its ReadyMap OSM level-14 PBF source. Inspection of the downloaded source tiles found
257 tree points within 20 meters of the avenue, none inside the source building polygons. A separate, smaller
source-cell trace through osgEarth decoded 327 nearby tree records, owned 310 points in the placement field,
rejected zero by polygon exclusion, and accepted all 310. Those counts cover different extents and are not a
one-to-one comparison. The close-view paging reproduction reported zero resident vegetation and:

```text
[Prestige vegetation coverage] Resource unavailable: Coverage query exceeds 100,000 features
```

The failure is the geographic provider's retained-feature query safeguard, BEFORE accepted-placement limits.
A coarse coverage page spans a much larger urban area than its fine descendants and can accumulate excessive
building, road, tree, and other matching feature records. Rejecting that ancestor can block all refinement below
it, even where each child request would fit. Local evidence is in `build/paris-tree-trace.log`,
`build/paris-mapped-trees.log`, `build/paris-mapped-trees.png.txt`, and `build/paris-avenue-tree-check.json`.
The temporary Paris placement trace verified decoding/placement; its source was removed with the test-only
cleanup. It did not verify recovery of an overflowing coarse page or establish that rendered paging is fixed.

Proposed approach to evaluate later:

- Preserve bounded, cancelable background source work. Let oversized coarse requests subdivide into bounded
  subqueries and/or let the paging hierarchy refine past an unavailable coarse representation. Choose the mechanism
  explicitly; an empty successful leaf must not permanently hide populated descendants.
- Keep polygon exclusions, linear buffers for generated placements, feature namespaces, point ownership, and stable
  IDs correct across subdivisions and halos. Do not truncate arbitrary input features: dropping exclusions could
  place vegetation in buildings/water, and dropping tree records would reproduce the missing-row symptom.
- Keep this separate from accepted-placement caps. Do not simply raise or remove the global query safeguard, or
  move large queries onto the update/draw thread. Expose a diagnostic distinguishing overflow from empty coverage.
- Validate with an offline dense-feature fixture whose coarse query exceeds the limit but whose children fit,
  plus the Paris scene. Verify close-up trees become reachable, page-edge points are neither lost nor duplicated,
  exclusions remain correct, cancellation stays atomic, and reloads remain deterministic. Cover exact/coverage
  tier combinations and check the distant fallback/transition behavior. Use correctness checks and a visual demo;
  no benchmarks are requested.

### Later -- simulation placement queries

Agreed October 2, 2026. **TODO; deferred until after cleanup.** This is a design checkpoint only, not an
implemented API. The primary consumer is a simulation backend performing collisions, independently of rendering.

Use a public external `PlacementGenerator`, shared by detailed vegetation rendering and simulation queries.
Keep `ScatterSource` responsible for source-space placement. Extract final asset selection, elevation attachment,
and placement resolution from Prestige vegetation's rendering implementation into the generator. Detailed rendering
consumes these same resolved records before building Chonk/GPU resources; do not duplicate placement logic.

The backend can construct the generator from the placement configuration, geographic sources, and map elevation
services without creating a VegetationLayer, viewer, camera, or graphics context. The layer may expose its
configured generator as a convenience, but layer membership is not required by the query API.

Initial contract:

- Query a geographic extent and named population. Return only placements whose roots lie inside that extent.
  Do not expand the requested area using asset bounds or include neighboring roots for collision purposes.
  Existing geographic exclusion rules and their configured buffers still apply. Collision handling belongs
  to the application, not the placement query.
- Return real detailed placements after configured density, source composition, placement strategies, and
  exclusions, including editable overlays. Support the same explicit points, natural scatter, and registered
  row/region strategies as detailed rendering.
- Positions are always FINAL: apply the same elevation sampling, altitude handling, scale, and orientation as
  detailed rendering, matching the intent of VegetationLayer::getAssetPlacements. No optional unclamped mode.
  If required source/elevation data cannot be resolved, report failure instead of returning an unclamped position
  as final. Use the configured elevation resolution, independently of camera or terrain-rendering LOD.
- Each record contains a stable placement ID, population, asset identity, final GeoPoint with explicit SRS/height
  meaning, scale, and rotation. Provide a double-precision asset-to-world transform or a helper to derive it.
  IDs remain stable across paging and rendering changes for unchanged placement inputs. Collision geometry can
  be resolved separately by asset identity; no Chonk or resident model pointer is required in the result.
- Ignore camera position, visibility, rendering residency, SSE, viewing distance, aggregate representation, and
  medium/far retention percentages. Approximate tree cards/coverage stands never substitute for real placements.
  Procedural grass queries describe placement patches, not shader-generated blades.
- Provide a bounded, cancelable worker operation. Source/elevation I/O must stay off the frame thread; no GPU
  readback or graphics context is needed. Failure/cancellation publishes no partial output. Use a consistent
  configuration/overlay snapshot for each request and identify its revision so callers can recognize stale results.

Illustrative API shape; names/details remain subject to implementation review:

```cpp
// Blocking, cancelable worker operation; returns only final placements rooted in area.
// Failure or cancellation leaves output empty; no viewer or graphics context is required.
Status PlacementGenerator::getAssetPlacements(
    const GeoExtent& area,
    const std::string& population,
    std::vector<Placement>& output,
    ProgressCallback* progress = nullptr) const;
```

Validate parity with detailed placement (asset, final position, scale, and rotation), root-only area filtering,
cell-boundary ownership, exclusions/overlay revisions, structured rows, deterministic reloads, and source/elevation
failure/cancellation. Verify operation without a viewer and invariance under rendering-quality/range/retention changes.
Keep the first implementation scoped to this getter and shared resolution path; no collision system or aggregate-query
API is part of this TODO. No benchmarks are requested.

### Later -- broader runtime area modifiers

Local hard-exclusion document editing is implemented. Brush density, feathering, local size/appearance overrides,
priority/multiplication semantics, vertex handles, dictionary editing, and production persistence are deferred.
These must compose through source/coverage interfaces, not a layer-specific editing subsystem. A DecalLayer or raster
adapter is possible; it must not require rendered terrain tiles. Edits survive paging; changing/moving/removing them
must update affected placements and all representation tiers consistently.

### Later -- elevation textures independent of terrain tiles

Evaluate a shared elevation-texture service/cache for GPU instance attachment, separately from osgEarth GPU clamping.
Account for dataset resolution, geospatial precision, page lifetime, and non-rendering final-placement queries.
CPU terrain attachment remains the current implementation. This is a possible future step, not a committed migration.

### Later -- additional inputs, appearance, and ground systems

- Derive land cover from imagery/rasters where OSM is sparse. Keep natural cover and land-use classifications separate.
- Add placement strategies and source adapters without changing the renderer; species selection can become richer
  when useful. At distance, preserve footprint, gaps, height, silhouette, and color before exact species identities.
- Evaluate approximate coverage/stand representations and optional retention with the same exclusion and quality
  interfaces; avoid globally unique baked textures. Large uniform forest and small boundary pieces may use different
  approximations. Additional tiers require a demonstrated visual/performance benefit.
- Wind, improved art, terrain materials/splatting, and possibly water can use the broader NodeKit later. Rocks and
  ground clutter already fit populations; avoid expanding the vegetation cleanup into those systems.
