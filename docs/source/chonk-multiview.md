# Chonk multi-view rendering

`osgEarth::ChonkRenderPass` is an optional OpenGL 4.6 contract for rendering Chonk
geometry into several views. It has no dependency on `ShadowCaster`, sky nodes,
cascade fitting, or shadow shader functions. Applications own their cameras,
render targets, projection shaders, and target reuse policy. Ordinary Chonk draws
and application shadow cameras can continue without adopting this API.

## Output modes

| Mode | Visibility records | GPU commands |
| --- | --- | --- |
| `PER_VIEW` | One record per surviving instance/LOD/view | Independent mesh/LOD commands for each view |
| `UNION` | One record per instance/LOD overlapping any active view | One mesh/LOD command; caller handles replication |
| `MERGED` | One record per surviving instance/LOD/view | One mesh/LOD command containing all surviving views |

All modes classify against up to eight orthographic, perspective, or oblique
OpenGL clip volumes. LOD and distance fading use a common reference camera.
`retainOutsideLODView` retains the coarsest LOD outside that reference camera's XY
bounds, for example to include offscreen shadow casters. Target-volume tests still
apply. A zero `activeViews` mask produces no commands.

Merged output reduces actual GPU commands, not merely API calls. If a mesh/LOD has
instances visible in four views, it produces one indirect command rather than four.
Vertex processing and rasterization still run for every surviving instance/view
pair, so command merging does not guarantee a GPU speedup for every scene.

## Supplying views

Allocate a stable `viewID` with `createViewID()` for each independent view group and
cull visitor. Publish a new immutable `Batch` for each submission, sharing that
batch between its separate passes. Its serial distinguishes submissions even when
they have the same frame number. Chonk retains GPU storage by view identity and
drawable placement; `frameNumber` controls eviction of unused storage.

```cpp
#include <osgEarth/ChonkRenderPass>
using osgEarth::ChonkRenderPass;

ChonkRenderPass::Parameters p;
p.viewID = rendererViewID; // Allocate once with ChonkRenderPass::createViewID().
p.frameNumber = frameNumber;
p.count = viewCount;      // 1..ChonkRenderPass::MAX_VIEWS
p.activeViews = (1u << viewCount) - 1u;
p.output = ChonkRenderPass::MERGED;
p.lodView = referenceView;
p.lodProjection = referenceProjection;
p.lodViewport.set(referenceWidth, referenceHeight);
for (unsigned i = 0; i < viewCount; ++i)
    p.clipFromWorld[i] = targetViews[i] * targetProjections[i]; // OSG row-vector convention

auto batch = ChonkRenderPass::createBatch(p);
if (batch)
{
    osg::ref_ptr<ChonkRenderPass> pass =
        new ChonkRenderPass(batch, 0, renderCamera->getViewMatrix());
    ChonkRenderPass::set(renderCamera->getOrCreateStateSet(), pass);
}
```

`createBatch` copies and validates its inputs; invalid inputs return null. For
`PER_VIEW`, install a pass with its view index on each camera. Other modes require
index zero. The render view must match the camera drawing the geometry, so Chonk
can reconstruct local-to-world coordinates. Use a conservative camera cull volume
containing all required target volumes, or arrange OSG culling appropriately: GPU
classification cannot restore drawables rejected earlier by the scene graph.

Install packets using the same update/cull synchronization and camera buffering
as the rest of the application's render state. Batches are immutable and retained
by their passes. Do not concurrently reuse a camera StateSet while a previous draw
traversal is consuming it. Distinct in-flight view groups require distinct IDs;
submit and consume a batch before reusing its persistent storage for another batch.

`set` stores a named user object without replacing application user data. Passing
null explicitly disables inherited multi-view state. The function also controls
`OE_CHONK_MULTIVIEW`; applications should not manipulate that define separately.
GPU culling must be enabled on participating Chonk drawables.

## Application vertex shader

After Chonk's `vertex_model` function at order `0.0`, the vertex-stage global
`int oe_chonk_view_index` identifies the selected view. It is zero for ordinary
draws and `UNION` output. A caller can use it to select a matrix, viewport, or layer.
Chonk itself neither writes `gl_Layer` nor applies a target projection.

For layered rendering, attach a layered texture array to the framebuffer and add
a `vertex_clip` function after the ordinary projection:

```glsl
#version 460
#extension GL_ARB_shader_viewport_layer_array : require
#pragma vp_function application_project, vertex_clip, 1.0
uniform mat4 application_clip_from_render_clip[8];
int oe_chonk_view_index;

// The application supplies inverse(renderView * renderProjection) * targetClip
// in OSG matrix convention, and owns the layer-to-target mapping.
void application_project(inout vec4 vertex)
{
    vertex = application_clip_from_render_clip[oe_chonk_view_index] * vertex;
    gl_Layer = oe_chonk_view_index;
}
```

This example assumes ordinary linear projection before the function. Custom
projection/deformation shaders must preserve the coordinates expected by their
target transform. Application fragment discard/alpha-cutout shaders remain active.
Because merged draws contain several views, use `oe_chonk_view_index` rather than
`gl_DrawID` or `gl_InstanceID` to identify a view. The compact visibility record
remains 16 bytes: the low 16 bits of `lod` hold the LOD, the high 16 hold the view.

`coreDraws` selects core indirect-count submission grouped by shared vertex/index
buffers. Otherwise Chonk prefers NVIDIA bindless indirect-count submission when
available. Chonk's existing geometry/material backend still requires NVGL; this
interface does not make the complete Chonk backend vendor-independent.

`OE_IS_SHADOW_CAMERA` and `OE_IS_DEPTH_CAMERA` keep their existing meanings. They
are not required for multi-view rendering. Application-owned shadow cameras that
use the established primary-view uniforms continue to work without a batch.
`ShadowCaster` uses this same public contract as a client, requesting `PER_VIEW`
for separate depth passes. `UNION` and `MERGED` remain available to
application-owned renderers.
