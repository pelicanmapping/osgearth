# Procedural2 coverage and exclusions -- Step 4A design

September 28 addition: [editable feature overlays](procedural2-feature-overlays.md) adds a catalog-driven local provider
through these same coverage rules, with modular storage, ImGui polygon/path authoring and regional canopy invalidation.
The broader runtime density/parameter modifier design remains Later.

The initial source is the existing osgEarth FeatureSource configuration in tests/osm.earth. Its tiled OSM data
supplies mapped polygons, lines, points, and attributes. Land cover and land use remain distinct attributes. Other
feature sources can participate through the same adapter; raster inference is deferred.

## Data flow

Feature sources -> attribute rules -> geographic coverage field -> placement strategy -> elevation -> Chonk batches.
The OSM source grid, vegetation source grid, render batches, and terrain grid have separate responsibilities.
Source tile queries include the exclusion buffer so roads just outside a vegetation cell can still affect its edge.

## First checkpoint: CPU field with exact boundary queries

Initially the coverage field is a CPU object built by paging workers, not a GPU render-to-texture pass. A density
sample answers how much vegetation is permitted at a geographic point; hard exclusion answers whether placement
is forbidden. Keep these separate: overlapping forest polygons must not double density, and soft density filtering
must never soften a building or water exclusion back into valid placement.

Compile matching polygons and buffered lines into a spatial grid of candidate geometry references. A sample visits
only relevant grid entries and performs polygon-with-holes or line-distance tests. This represents a sampled field
without quantizing exclusion edges to texels. It preserves narrow roads/small clearings and avoids placement changes
caused solely by a render-cell size change. The initial approach filters deterministic scatter candidates and admits
explicit tree points through the same hard exclusions. Named region strategies now supply rows/grids and
application-defined layouts through the same field.

This is a deliberate first implementation of the field interface, not a requirement to retain vector tests forever.
Measure correctness and inspect visual output before selecting raster resolution or accepting boundary approximation.

## Density/exclusion textures

A compact raster density field plus a separate hard-exclusion channel is a candidate acceleration and rendering
representation. It can be rasterized on CPU during paging and uploaded only when a renderer needs it; it does not
require a terrain-tile texture or a GPU rasterization pass. Reuse osgEarth's feature rasterization facilities when
introducing that representation. Fields should use metric resolution targets, bounded dimensions, and neighbor
halos; narrow features need conservative coverage or precise boundary fallback. Ordinary averaged mask mipmaps must
not silently erase exclusions. Density can interpolate smoothly; hard exclusions require their own sampling policy.

Future sources can contribute raster fields directly. Future canopy levels can summarize coverage under a stated
screen-space error tolerance. An exclusion-only texture can also support rapid suppression while replacement
geometry is built, but cutting pixels from one flat baked forest card does not reconstruct hidden trees or correct
its footprint automatically. Representation-specific masking remains part of Step 4B's visual experiment.

## Runtime area modifiers (Later; deferred September 27)

Application-owned geographic modifiers compose over base fields with explicit priorities and population filters.
They support add/update/remove and density/parameter overrides as well as clearing. DecalLayer can supply a mask,
but the interface must also accept geometry/raster inputs independently of terrain rendering. Moving a modifier
dirties both its old and new footprints; modifiers outlive resident pages. Session persistence is an application choice.

Use immutable query snapshots and spatial revisions. Rebuild affected vegetation pages and derived aggregate parents;
reject stale worker results. Near instances, coarse representations, and shadow rendering must observe the same edit.
A currently resident parent that still fills a new clearing is not a valid fallback. Immediate GPU masking is an
option to evaluate for update latency, not a prerequisite or a claim that regional live edits already exist.

## Validation

Use offline fixtures for polygon holes, overlapping sources, buffered lines across page boundaries, explicit point
ownership/deduplication, cancellation, missing data, and batching-invariant placement. The live OSM demo verifies the
real FeatureSource path with original starter art and elevation. The row fixture now covers structured placement,
fragment/cell continuity, and custom application algorithms. Runtime area-edit validation is deferred to Later.
All launches use four samples; no benchmarks are generated.

## Initial implementation and configuration

The public FeaturePlacement header separates PlacementFeatureProvider (data access), PlacementField (coverage),
and PlacementStrategy (candidate generation). MapFeatureProvider adapts existing open FeatureSources. The default
MixedPlacementStrategy dispatches NaturalPlacementStrategy for scatter/explicit points and named region strategies for
structured layouts. Applications register RegionPlacementStrategy implementations on the layer before open; advanced
sources can still replace the whole dispatcher through FeatureScatterSource.

Vegetation2 accepts a sources collection of named feature-layer references and a coverage collection of attribute
rules. Match source and group by name or '*'; match a non-null attribute key against a value or '*'. The optional
except value supports tags such as building=no. Actions are include (polygon), exclude (polygon or buffered line),
and points. Matching inclusions use priority and region ownership; ties for the same region/scatter combine density by
maximum. Any matched hard exclusion wins. The initial demos use
binary coverage. Constant fractional densities are supported and tested; spatially feathered masks are later work.
Buffer is a line's half-width in meters, with round segment caps. The demo's four-meter road half-width is a policy
placeholder, not a claim to reproduce surveyed road widths. Polygon exclusions retain their actual holes.

Each mapped point is owned by one source cell. Duplicate point records within a named input collapse by stable ID;
source namespaces separate unrelated IDs. A polygon's scatter and its explicitly mapped trees currently coexist;
reserving tree spacing around mapped points or assigning point-over-area priority is a subsequent policy refinement.
Masks test plant roots/patch centers; they do not clip branches, crowns, or grass blades that overhang a boundary.
Density controls change polygon scatter; explicit source points remain authoritative unless the population is disabled
or density is set to zero. Model choices remain independent of positions and coverage.

The adapter queries native source tiles with a road-buffer halo. A native tile shares its local metric road-buffer
projection, independently of vegetation batching. Preflight rejects requests spanning more than 64 native tiles per
input before enumerating them. Requests also cap features, transformed vertices, acceleration-grid references, and
instances; errors/cancellation discard partial results. Existing FeatureSource caching handles decoded native tiles;
this checkpoint creates no global collection of unique vegetation assets. Coverage predicates are released after
page generation. A missing/failed request is not permission to fill the area with synthetic vegetation.

Sources and coverage rules are configured before opening the layer. The existing Reload population control reruns
that population's queries; spatially scoped revisions and application-owned area modifiers are deferred to Later; they do not block 4B.

Run from tests after building and calling osgearth_shell.bat:

```bat
osgearth_imgui procedural2-osm.earth --sky2 --shadows --nvgl --samples 4 --vegetation2
osgearth_imgui procedural2-coverage.earth --sky2 --shadows --nvgl --samples 4 --vegetation2
osgearth_tests "[procedural2]"
osgearth_tests "[procedural2-osm]"
```

The last test is an explicitly selected network smoke test; the regular suite uses only the local synthetic coverage
fixture. OSM coverage is sparse in some regions, and this source's Mercator profile does not extend to the poles.
No imagery-derived fallback is enabled. Both earth files use fixed daylight hours for their initial locations.

## Structured placement and application extensions

Land-use types are data; they are not a closed enum in the provider or renderer. Rules select named algorithms:

```xml
<rule name="vineyard-layout" source="fields" group="shrubs"
      key="landuse" value="vineyard" strategy="rows" priority="10">
    <placement origin_longitude="-75" origin_latitude="40.65"
               heading_attribute="row_heading" row_spacing="8" plant_spacing="3"/>
</rule>
```

The built-in row strategy is reusable for orchards, vines, or crops. `heading` is clockwise degrees from local north;
`row_spacing` and `plant_spacing` are meters. Optional `longitude_attribute`, `latitude_attribute`, `heading_attribute`,
`row_spacing_attribute`, and `plant_spacing_attribute` bindings override rule defaults when present. Malformed values
fail the request. Absent attributes fall back to configured values. The default `origin_mode="explicit"` needs the same
anchor metadata and feature ID on every clipped fragment, with bounds within 100km in each metric direction of the anchor.
A source-tile centroid is never used because it would restart rows across native tile boundaries.

For sources without anchors, `origin_mode="world"` uses fixed six-degree longitude strips with transverse-Mercator metric
frames. Origins are on the equator at each strip's central meridian, including in the southern hemisphere. Workers enumerate
only strips intersecting the query and enforce half-open geographic ownership. The lattice and IDs depend on the fixed strip,
feature/rule identity, heading, and spacing; they do not depend on page levels, fragment bounds, request order, or a growing
cache of per-feature anchors. Explicit origin values/bindings cannot be combined with world mode.

World mode covers the OSM Mercator latitude domain. Spacing is in projected meters; headings are clockwise from grid north,
so true geographic direction has a small convergence difference. A parcel crossing a fixed six-degree strip boundary can have
a row phase/direction seam there. These are deliberate approximation limits of the fallback, not source/terrain tile seams.
Full-feature layout descriptors can replace the fallback later through explicit anchors or another registered strategy.

The main OSM demo selects real `landuse=vineyard`, `orchard`, and `farmland` polygons. Vineyards bind the optional numeric
`vine_row_orientation` attribute; absent values use zero degrees, while orchard/crop defaults are configured in the earth file.
Farmland without crop detail receives a generic crop proxy; the scene does not claim a specific species, season, or surveyed
planting layout. A more specific orchard/vineyard polygon suppresses crops where it overlaps a broad farmland polygon.
See the OSM [vineyard](https://wiki.openstreetmap.org/wiki/Tag:landuse%3Dvineyard),
[orchard](https://wiki.openstreetmap.org/wiki/Tag:landuse%3Dorchard), and
[farmland](https://wiki.openstreetmap.org/wiki/Tag:landuse%3Dfarmland) tag documentation.

Applications can install their own pathways before opening a layer:

```cpp
layer->registerPlacementStrategy("my-orchard", std::make_shared<MyOrchardStrategy>());
// A rule with strategy="my-orchard" passes its <placement> config to that algorithm.
layer->open();
```

Implement `RegionPlacementStrategy::validate` and `generate`. The first validates strategy-owned settings; the second emits
finite, stable candidate placements in the requested cell's SRS. `PlacementRegion` carries namespaced identity, unchanged
source feature fragments/attributes, generic configuration, and bounds. Registered instances are retained by shared pointer
and called concurrently by paging workers. They must be immutable or synchronized, honor cancellation, and bound their work
and output. Registration is per layer and frozen while open; automatic plugin-library discovery is not part of this checkpoint.

`MixedPlacementStrategy` combines registered algorithms with natural scatter and explicit points. The shared dispatcher
applies half-open cell ownership, winning-region membership, fractional coverage, hard exclusions, cancellation, and output
limits to their candidates. Algorithms cannot bypass those masks by simply emitting points inside a building. Unknown
strategy names fail at layer open; programmatic sources fail explicitly when they try to use an unregistered algorithm.
The row generator caps a request at two million lattice slots and the dispatcher caps returned region candidates per cell.
Existing per-cell/per-batch instance limits still apply after filtering. Errors publish no partial page.

This is a placement extension point, not a claim that lawn simulation or complete playground scene generation exists today.
Asset selection and terrain attachment remain separate. A richer pathway can later add semantic asset roles or non-instance
outputs without teaching the OSM decoder about each land-use type. Regional runtime modifiers will wrap the common field so
new strategies inherit application edits as well as static exclusions.

Inclusion rules combine by priority. Higher priority wins; equal-priority named regions take precedence over natural scatter.
Equal-priority different regions select the lowest stable region ID; duplicates of one region combine density by maximum.
Exclusions veto all inclusion priorities. Explicit source points remain authoritative except for hard exclusions, as before.
Changing a mask removes slots without reseeding the lattice. Changing cell or render levels also leaves row identity/phase
unchanged; natural scatter retains its existing source-cell-based identity policy.

Population controls `row_density` (0..1), `row_spacing_scale`, and `plant_spacing_scale` (0.1..10) provide live tuning through
ImGui. Occupancy thins a stable set of slots and restoring it restores those exact plants. Spacing multiplies the rule/feature
spacing and intentionally moves plants. `density` still controls natural scatter; zero retains its existing whole-population
disable behavior. Source and region rules are still configured before open; runtime area edits are deferred to Later.

Run the synthetic multi-source demonstration:

```bat
call osgearth_shell.bat
cd tests
osgearth_imgui procedural2-rows.earth --sky2 --shadows --nvgl --samples 4 --vegetation2
```

The main `procedural2-osm.earth` demo now uses the live OSM source for agricultural parcels as well as woodland and explicit
trees. Its startup viewpoint visits mapped vineyards near Sion; named orchard/crop viewpoints show the other pathways.
Separate `orchards`, `vineyards`, and `crop_rows` populations make row controls independent of natural vegetation.
Higher-priority agricultural coverage suppresses random woodland scatter inside fields. All populations share OSM road,
building, and water exclusions; explicit mapped trees remain authoritative except where excluded. No synthetic parcel or
exclusion source is loaded by this main demo. The original synthetic fixture remains an isolated regression/visual example.

```bat
call osgearth_shell.bat
cd tests
osgearth_imgui procedural2-osm.earth --sky2 --shadows --nvgl --samples 4 --vegetation2
```

Structured checkpoint validation: all 28 Procedural2 cases passed (214,160 assertions), including row continuity,
clipped/duplicated feature records, concurrent reloads, inclusion priority, occupancy restoration, strict metadata,
request limits, custom dispatch, and registration before open. The isolated grass GPU worker also passed 5,467 assertions.
The top-down GL capture loaded 7,038 instances across 24 population drawables, with three asset bundles accounting for
9,702,850 bytes and no load failures/budget denials. These are correctness/residency observations, not benchmark results.


World fallback validation: the updated suite passes all 29 Procedural2 cases (397,666 assertions) and the separate grass GPU
worker (5,467 assertions). The new row test exercises clipped fragments, independent paging levels, a projection boundary,
the equator, northern/southern latitudes, stable occupancy, metadata overrides, and invalid modes. An explicit network test,
`osgearth_tests "[procedural2-osm-rows]"`, verifies nonempty accepted vineyard/orchard/farmland placement near Sion through the
configured XYZFeatures service and common exclusion rules. It is excluded from ordinary offline test runs.


## Step 4B coverage summaries (first implementation)

Aggregate pages query the same coverage rules directly, without creating a detailed placement list. Feature selection
and the buffered page extent are applied before copying native source records. Retained feature, vertex, native tile,
and canopy subdivision limits remain explicit; no cap is raised merely because the camera moved outward.
`PlacementField::uniform` conservatively certifies a whole footprint. The vector implementation tests polygon edges,
holes, and metric buffered lines; mixed cells subdivide. At terminal cells, individually certified crown rectangles
can survive through a compact nine-bit instance mask. A point sample alone never authorizes a crown: its entire footprint
must be permitted. Crowns crossing an unresolved boundary are omitted. Raster adapters can
provide a conservative min/max or occupancy hierarchy under the same contract; ordinary averaged mips are insufficient.

Aggregation is intended as the default policy for all populations, independently of how they were placed. Preserving
rows at long distance is an option, not a mandatory constraint. The current natural-tree backend is the first 4B
checkpoint; structured layout summaries and grass/shrub/clutter representations remain subsequent 4B work.

Aggregate appearance refinement keeps the partition separate from visual placement: 256 nonoverlapping base rectangles
use deterministic unequal splits, then retain the existing bounded boundary/terrain subdivision. Interior rectangles
are certified as a whole; mixed terminal rectangles certify their crowns separately. Four irregular crown arrangements replace the visible 3x3 lattice;
all visible crowns remain within their certified footprints, including after quarter turns and terrain fitting.
A boundary patch's bounding rectangle can contain exclusions; only its mask-selected crown footprints are certified. Variation never
jumps over an exclusion or requires per-page unique art. Partition tests check total area, disjoint interiors, transformed
bounds, and repeatability; art tests cover all 32 layout/occupancy combinations and their shared residency.

Page transitions are independent of the source adapter and placement algorithm. The optional PagedNode2 replacement
traversal preserves loading/cancellation behavior; merge reference time permits a short arrival fade while retaining the
parent. Screen-error overlap handles continuous approach and retreat. Complementary 64-step pixel-coverage intervals
compose through parent/child/grandchild handovers. They are visitor-local during cull and immutable snapshots during draw,
so camera traversal does not mutate a shared alpha Uniform. ChonkRenderBin carries the interval across its flattened state
graphs and admits only fully covered instances to the opaque path. Other Chonk users default to complete coverage.

Load priority stays distance-based across the entire hierarchy, including nodes whose refinement callback uses
projected error. Mixing positive pixel priorities for detailed requests with negative distance priorities for coverage
ancestors can starve the first coarse coverage. The distance blend does not change that scheduling unit.

This adds temporary overlap residency and recovers more small boundary pieces. It is bounded per page, but a global
resident-page/upload budget remains necessary. A smooth handover alone is not evidence of faster rendering.
