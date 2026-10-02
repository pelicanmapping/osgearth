# Vegetation2 editable feature overlays

October 2, 2026: source-composition cleanup of Step 4A. Catalog-driven local hard exclusions now compose with base features.
Density painting, feathering, parameter modifiers, and a catalog-definition editor remain Later.

## Modular boundaries

| Component | Responsibility |
| --- | --- |
| `OverlayType` | Configuration-only type name, label, permitted geometry, and attribute assignments |
| `FeatureOverlay` | Document edits, stable IDs, geometry validation, immutable snapshots, bounded undo/redo |
| `OverlayStorage` | Blocking read/write of detached documents; independent of placement, rendering and UI |
| `GeoJSONOverlayStorage` | Initial local-file adapter; replaceable with SQLite/MVT, project database, or service |
| `EditableFeatureProvider` | Expose a standalone document as a named source, immutable snapshots, revisions and changed extents |
| `MapFeatureProvider` | Compose named FeatureSources and application providers through ordinary coverage rules |
| Source revision consumer | Refresh affected pages, including aggregate ancestors; no editor-specific layer API |
| `FeatureOverlayGUI` | Picking, outline preview, catalog selection, history controls, and asynchronous storage I/O |

The demo passes `FeatureOverlayGUI::draw(document, mapNode, view)` a shared document; mutations are serialized on the
application/update thread. It can be embedded without a VegetationLayer2. Storage completion targets the document that
started the operation, even if the user selects another source.

Large databases can also implement `PlacementFeatureProvider` directly; they need not load a world's edits into the
small local document. Changing persistence does not require a renderer or placement algorithm change.

```xml
<sources>
    <source name="osm"><features>data:osm</features></source>
    <source name="edits">
        <overlay>
            <type name="path" label="Footpath" geometry="line">
                <attribute key="highway" value="path"/>
            </type>
            <type name="clearing" label="Clear vegetation" geometry="polygon">
                <attribute key="vegetation:exclude" value="yes"/>
            </type>
        </overlay>
    </source>
</sources>
<coverage>
    <!-- This wildcard-source rule applies to OSM and authored paths. Buffer is half-width. -->
    <rule key="highway" value="*" except="no" action="exclude" buffer="4"/>
    <rule source="edits" key="vegetation:exclude" value="yes" action="exclude"/>
</coverage>
```

The catalog assigns tags; coverage rules own actions, buffers, population filters and placement strategies. The editor
does not duplicate that policy. Hard exclusions form a union regardless of source order. Deleting a local clearing
restores what remaining inputs permit; it cannot remove an OSM road/water exclusion. Unchanged placement IDs stay stable.

These are authoring types, not all land cover: preserve the agreed distinction between natural land cover, human land
use, and infrastructure. `vegetation:exclude=yes` is our explicit clearing attribute, not an OSM taxonomy assertion.
Playground/lawn/orchard tools can extend this vocabulary and use the existing strategy registry. The provider validates authoring vocabulary independently
of placement policy. A tool may additionally validate that its types match applicable exclusion rules. The current
ImGui demo authors hard exclusions; inclusion/replacement tools need their own composition and parameter-design checkpoint. The catalog-definition UI is expressly deferred.

## Paging and concurrency

Each coverage field query captures immutable editable-input snapshots before querying source cells. Individual instances and both
aggregate tiers use the same composed field. Document state outlives resident pages; flying away does not lose edits.

After an edit, the source reports changed extents through the generic revision interface. The layer expands
**old and new** bounds by each population's largest rule buffer, plus a conservative stand-footprint halo in coverage
mode. This may refresh extra nearby pages; it avoids coupling invalidation to editor types. `OverlayPager` extends SimplePager with geographic node identities; its
invalidation visitor unloads intersecting branches immediately before their first content level, including pending
ancestors. PagedNode2's existing revision gate prevents pre-edit workers from merging after invalidation.

The refresh unit is a **four-sibling content block**, including coarsest canopies and all finer descendants. Unrelated
loaded branches, shared asset residency and population policies remain intact. Affected blocks temporarily disappear
while reloading, including their shadows. They cannot fall back to an old parent canopy that fills the new clearing.
Finer content replacement or an immediate GPU exclusion mask could reduce this temporary gap later.

## Demo and workflow

The main working scene is `tests/a.earth`; the portable forest scene is `tests/procedural2-canopy-osm.earth`. Its catalog offers clearing, building footprint,
water area, and buffered path. The same catalog is in `tests/procedural2-osm.earth` and the offline geographic fixture
`tests/procedural2-coverage.earth`. Launch ImGui with `--sky2 --shadows --nvgl --samples 4 --vegetation2`.

Under **Vegetation2 > Feature overlays**:

- If several editable sources are configured, select the source document. Select a type; its assigned tags are shown.
  Coverage policy stays in the shared rules, outside the editor.
- **Draw new outline**, then click terrain for vertices. **Enter / Finish** commits, **Backspace** removes the last
  point, and **Escape / Cancel** abandons the sketch. Polygon outlines close automatically.
- Select a feature from the list to highlight it. **Redraw selected** replaces geometry while retaining ID/type.
  **Delete selected**, **Undo**, and **Redo** use the same document API.
- **Show outlines** controls display only. Yellow marks selection/sketch, cyan other local features. Path previews
  show centerlines; the source-composition rule defines their exclusion half-width.
- **Save overlay** saves the chosen local file. **Load / replace** validates and replaces the local document as one
  undoable edit. Neither action changes OSM or the earth file. I/O runs off the scene thread.

The default `vegetation2-edits.geojson` path is relative to the application working directory. Use an absolute path to
choose a project location. Session persistence is explicit: save before closing, and load in a new session.

Files store WGS84 geometry, string IDs preserving all 64 bits, `p2:type`, and catalog attributes. Import rejects unknown
types, mismatched tags, unsupported geometry and duplicate IDs atomically. Saving writes a sibling temporary file before
replacement, preserving the previous destination on failure. Ordinary GeoJSON viewers can read these files; importing
arbitrary external GeoJSON requires adaptation to this catalog/document schema.

## Initial limits

- Simple polygons without holes and polylines; reject polygon self-crossings/touches.
- 4,096 local features, 2,048 vertices per feature, 32 history snapshots, 16 MiB input files.
- Local edits span at most two degrees, between 85S and 85N; date-line crossings are rejected. Base vegetation remains
  global. Larger edits need a dedicated tiling/authoring adapter.
- The local provider performs a bounded extent scan. Production datasets should use an indexed provider/database.
- Brush painting, feathered density, local parameter overrides, vertex handles, a catalog editor, and production
  database/versioning policy remain Later.

Correctness tests cover snapshots, composition, persistence, invalid imports, stable placements, both canopy tiers,
regional refresh, and a worker held across invalidation. No benchmarks are generated for this checkpoint.
