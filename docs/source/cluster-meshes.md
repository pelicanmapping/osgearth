# Draw dense models with cluster meshes

`osgEarth::ClusterMesh` and `osgEarth::ClusterMeshNode` draw models that have far more
triangles than the screen has pixels. The approach follows Nanite. Each model is cut into
clusters of up to 128 triangles. Groups of neighboring clusters are simplified into
coarser clusters, level by level, until one group remains. Every frame, a compute shader
picks the clusters whose simplification error is under a pixel threshold and draws only
those. The triangle count then follows the number of pixels a model covers, not the size
of the source file.

## Requirements

- OpenGL 4.6 with bindless textures, the same path that Chonk uses. Run the examples with
  `--nvgl`.
- osgEarth built with meshoptimizer. Without it, `ClusterMesh::create` returns an empty
  pointer.

## Convert a model

`ClusterMesh::create` converts any loaded model. Textures and materials go into the
`TextureArena` of the `ChonkFactory` that you pass. Albedo, normal, metallic-roughness,
and occlusion maps are used.

```cpp
osg::ref_ptr<TextureArena> arena = new TextureArena();
arena->setBindingPoint(1);
ChonkFactory factory(arena.get());

osg::ref_ptr<osg::Node> model = osgDB::readRefNodeFile("boulder.glb");
ClusterMesh::Ptr mesh = ClusterMesh::create(model.get(), factory, "boulder.glb.oecm");
```

The third argument names a cache file. The build takes 1.5 to 3 seconds per million
triangles, and the cache makes later loads skip it. `create` ignores a cache file that was
built from different geometry or options, and rebuilds.

A mesh is immutable and holds its vertices and indices in memory for as long as it lives.

## Draw instances

Add the mesh to a `ClusterMeshNode` once per instance. Install the arena in a `StateSet`
at or above the node.

```cpp
osg::ref_ptr<ClusterMeshNode> node = new ClusterMeshNode();
node->getOrCreateStateSet()->setAttribute(arena);

std::vector<osg::Matrixf> transforms = ...; // relative to the node
node->add(mesh, transforms);
```

Place the node under a `GeoTransform` or another local frame. Instance transforms are
single precision.

The node takes its lighting from the scene above it. Under a `MapNode` with a sky, the
instances receive the sky's PBR lighting. To make the instances cast shadows, add the node
to `ShadowCaster::getShadowCastingGroup()`.

## Cover a large area with batches

Instances added with `add` share the node's frame, so they must stay within a few
kilometers of the node's origin. To cover more ground, or to page instances in and out,
add them in batches. A batch is a set of instances with a frame of its own.

```cpp
osg::Matrixd frame;                     // tile center to world, rotation and translation only
tileCenter.createLocalToWorld(frame);

std::vector<ClusterMeshNode::Instance> instances = ...; // mesh and transform relative to the frame
osg::ref_ptr<osg::Node> tile = node->createBatch(frame, instances);
```

The node combines each batch's frame with the camera in double precision. One node can
therefore hold batches anywhere on the globe, and it still culls and draws all of them
together.

`createBatch` returns a node that stands for the batch in the scene graph:

- The batch draws in each frame in which a cull traversal reaches that node. Add it under
  a pager and the pager decides what is visible.
- The batch's instances are removed when that node is destroyed.
- Add that node in the same coordinate system as the `ClusterMeshNode`. Its bound is the
  bound of the instances in the `ClusterMeshNode`'s coordinates.

You can call `createBatch` from any thread, for example from a paging thread.

## Set the detail

`ClusterMeshNode::setErrorThreshold` sets the largest simplification error, in pixels,
that the node accepts on screen. The default is 1. At 1 pixel the result is hard to tell
from the source model. A threshold of 2 or 4 draws fewer triangles and shows coarser
silhouettes.

`ClusterMeshNode::setMinInstancePixels` skips instances whose bounding sphere has a
smaller radius on screen than the given number of pixels. Every instance in view draws at
least its coarsest level, which is 20 to 120 triangles, so a scene with many distant
instances needs this limit. The default is 0, which draws every instance.

The number of clusters a view needs grows with the window size and with the content. The
GPU command buffers start at 262,144 clusters per list and grow when a frame needs more.
Growing takes a few frames, during which some clusters can be missing.
`ClusterMeshNode::setMaxDrawnClusters` sets the limit of that growth. The default is
4,194,304 clusters per list, and each entry costs about 150 bytes of GPU memory.
`ClusterMeshNode::Stats::overflowed` reports a frame that needed more than the buffers
held.

## Occlusion culling

The node skips instances and clusters that other geometry hides. Occlusion culling is on
by default, and `ClusterMeshNode::setOcclusionCulling(false)` turns it off. It applies to
perspective color cameras. Shadow, depth, and pick cameras draw everything in their view.

Each frame has two phases. The early phase draws what the previous frame's depth buffer
predicts to be visible. The node then reduces the current depth buffer to a pyramid, tests
everything the prediction rejected against that pyramid, and draws the part that is visible
after all. A wrong prediction costs a late draw. It does not make an object disappear for
a frame.

Terrain and other geometry occlude the instances only if they draw before the node's late
phase. The late phase draws in a nested render bin numbered 1, after the other drawables of
the bin that holds the node.

The depth pyramid is `osgEarth::DepthPyramid`, the same one that Chonk uses.

## Inspect the result

`ClusterMeshNode::setDebug` colors the geometry by cluster, by triangle, or by instance.
`ClusterMeshNode::setCollectStats(true)` enables `getStats`, which returns the number of
clusters and triangles that the last frame drew and the number that occlusion culling
skipped.

## Scatter models over the terrain

`osgEarthPrestige::ClusterMeshLayer` is a demonstration layer that scatters models over
the terrain. It pages tiles at one level. Each tile places its share of every model at
random, clamps the placements to the map's elevation, and adds them to one
`ClusterMeshNode` as a batch.

```xml
<map xmlns:prestige="osgEarthPrestige">
    <prestige:clustermesh name="Rocks">
        <level>17</level>
        <range>1600</range>
        <models>
            <model url="boulder.glb" density="20" min_scale="0.5" max_scale="2" sink="0.2"/>
        </models>
    </prestige:clustermesh>
</map>
```

| Property | Meaning | Default |
| --- | --- | --- |
| `level` | Level of the tiles that hold the placements | 17 |
| `range` | Distance in meters within which tiles of that level load | 1000 |
| `density` | Multiplier on every model's density | 1 |
| `seed` | Seed of the random placement | 1 |
| `error_threshold` | Largest simplification error on screen, in pixels | 1 |
| `min_instance_pixels` | Smallest on-screen radius of an instance that draws, in pixels | 2 |
| `occlusion_culling` | Skip what other geometry hides | true |
| `max_drawn_clusters` | Limit on the size of the GPU command buffers; see `setMaxDrawnClusters` | 4194304 |
| `max_texture_size` | Largest texture dimension to upload | 4096 |
| `cluster_cache` | Store each converted model next to its file as `.oecm` | true |

Each `model` element takes these attributes:

| Attribute | Meaning | Default |
| --- | --- | --- |
| `url` | Model file. A file with several separate objects gives one mesh per object | required |
| `density` | Placements per hectare where the model is thickest | 10 |
| `min_scale`, `max_scale` | Range of the random uniform scale | 1, 1 |
| `sink` | Fraction of the model's height that goes underground | 0 |
| `tilt` | Largest random lean from vertical, in degrees | 0 |
| `clumping` | 0 spreads placements evenly, 1 gathers them in sparse patches | 0.5 |
| `patch_size` | Size of those patches, in meters | 40 |
| `level_offset` | Use tiles this many levels finer than `level` | 0 |

Each step of `level_offset` halves the tile size and the range. Give small, dense models
such as plants an offset of 2 or 3. They then exist only near the camera, and the number
of loaded instances stays bounded while rocks remain visible from far away.

`tests/clustermesh.earth` uses the layer with Quixel Megascans rocks and plants:

```
osgearth_imgui clustermesh.earth --nvgl
```

In `osgearth_imgui`, open **Prestige > Cluster Meshes** to change the settings and to see
the counters.

## Limits

- Alpha-tested materials draw two-sided with a hard cutoff. Blended transparency is not
  supported.
- Meshes do not animate. Skins and morph targets are ignored.
- The geometry of every mesh stays in GPU memory. The node does not stream geometry.
  Instances page in and out only when you add them in batches.
- Intersection tests and picking do not see the instances.
- For a model made of many small unconnected pieces, such as grass blades, the builder
  falls back to a simplifier that ignores texture coordinates. Distant instances of such a
  model can show stretched textures.
