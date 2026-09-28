# Structured land-use fixtures

Original synthetic GeoJSON, MIT license. These parcels do not describe actual farms at either location.
The existing starter models are placeholders for crops/vines. Rule-configured geographic origins and per-feature
heading attributes make patterns independent of paging.

- `fields.geojson` and `exclusions.geojson`: isolated orchard, vineyard, crop-row, and natural woodland fixture,
  used by `tests/procedural2-rows.earth`. A separate source supplies building, water, and road exclusions.
- `helsinki-fields.geojson` and `helsinki-exclusions.geojson`: authored copies of the three agricultural parcels
  beside the Helsinki viewpoint, retained from the earlier integration demonstration. The main OSM demo no longer loads them. Latitude translation and
  cosine-adjusted longitude offsets approximately preserve fixture dimensions. During that demonstration the live OSM source supplied background woodland and additional exclusions. The local exclusion source also clears the orchard hole, preventing OSM forest
  scatter from filling it. These are demonstration inputs, not surveyed or inferred land-use data.

The current main `tests/procedural2-osm.earth` demo uses real OSM agricultural polygons with automatic world row frames.
Its separate orchard, vineyard, and crop-row populations each have ImGui row controls and share OSM hard exclusions.
