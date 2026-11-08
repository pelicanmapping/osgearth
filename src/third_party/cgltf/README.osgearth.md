# cgltf (osgEarth copy)

`cgltf.h` and `cgltf_write.h` are upstream cgltf 1.15
(https://github.com/jkuhlmann/cgltf, MIT; commits `de9828bc` and `32457ff7` of
those files) with the osgEarth modifications below, each marked in the source.

- glTF 2.1 external assets: the root `files` and `externalAssets` arrays and
  the node `externalAsset` property are parsed into `cgltf_file`,
  `cgltf_external_asset` and `cgltf_node::external_asset` (search "glTF 2.1").
- `KHR_draco_mesh_compression` attribute values stay encoded as Draco unique
  ids (`id + 1` in `cgltf_attribute::data`) instead of being resolved as
  accessor pointers.
- Double node transforms: `cgltf_node` stores `translation`, `rotation`, `scale`
  and `matrix` as `cgltf_double`, and `cgltf_node_transform_local/world` and the
  writer use the same precision. A 32-bit float rounds the ECEF translation in a
  Google Photorealistic 3D Tiles node matrix by up to 0.25 m. Based on upstream
  PR 239 (https://github.com/jkuhlmann/cgltf/pull/239), which is unmerged.

`osgearth_patch.py <upstream cgltf.h> <output cgltf.h> <upstream cgltf_write.h>
<output cgltf_write.h>` re-applies these modifications to a new upstream release.
