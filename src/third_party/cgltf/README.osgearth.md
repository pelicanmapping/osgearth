# cgltf (osgEarth copy)

`cgltf.h` is upstream cgltf 1.15 (https://github.com/jkuhlmann/cgltf, MIT)
with two osgEarth modifications, each marked in the source. `cgltf_write.h`
is the unmodified upstream writer of the same release.

- glTF 2.1 external assets: the root `files` and `externalAssets` arrays and
  the node `externalAsset` property are parsed into `cgltf_file`,
  `cgltf_external_asset` and `cgltf_node::external_asset` (search "glTF 2.1").
- `KHR_draco_mesh_compression` attribute values stay encoded as Draco unique
  ids (`id + 1` in `cgltf_attribute::data`) instead of being resolved as
  accessor pointers.

`osgearth_patch.py <upstream cgltf.h> <output cgltf.h>` re-applies both
modifications to a new upstream release.
