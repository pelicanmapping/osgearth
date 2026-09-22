# Sun shadows

`osgEarth::Util::ShadowCaster` provides stable cascaded shadow maps for a distant sun.
It works with SkyNode2 and the existing lighting shaders. Run an example with
`--shadows`, or use **Install shadows** in EnvironmentGUI after installing a sky.
The panel's **Details / Shadows** controls cover distance, cascade count, resolution,
filtering, softness, sunlight visibility, bias, caster reach, and cascade blending.

## Installing in an application

```cpp
#include <osgEarth/Shadowing>
#include <osgEarth/TerrainEngineNode>

int unit;
auto resources = mapNode->getTerrainEngine()->getResources();
if (resources->reserveTextureImageUnit(unit, "Application shadows"))
{
    osg::ref_ptr<osgEarth::Util::ShadowCaster> shadows = new osgEarth::Util::ShadowCaster;
    shadows->setTextureImageUnit(unit);
    shadows->setLight(sky->getSunLight());
    shadows->getShadowCastingGroup()->addChild(mapNode->getLayerNodeGroup());
    shadows->getShadowCastingGroup()->addChild(mapNode->getTerrainEngine()->getNode());
    // Place shadows above the visible map, below the sky. Both caster references
    // and visible children must use the same coordinate system.
    sky->removeChild(mapNode);
    shadows->addChild(mapNode);
    sky->addChild(shadows);
}
```

The snippet assumes the map is a direct child of the sky. Reserve the texture unit
from the map's terrain resources: the default unit 7 is only a convenience for
standalone scenes and can collide with terrain or atmospheric textures.

Caster registration does not make geometry visible. Geometry also needs to appear
in the ordinary scene below the shadow node. Keep the caster graph free of the
ShadowCaster itself. Terrain casting still uses `OE_TERRAIN_CAST_SHADOWS`; the terrain
can receive model shadows without casting its own. Cutout/discard shaders remain
active in the depth passes, including vegetation and Chonk geometry.

## Quality and coverage

The defaults are three 2048 x 2048 maps, 2500 meters of receiver coverage, a 1.5-texel
four-tap filter, 1000 meters of upstream caster reach, and 10% cascade blending.
Use `setCascades(count, distance, lambda)` for one to four practical splits. Lambda
blends uniform and logarithmic spacing; the default is 0.8. `setRanges` accepts
explicit, increasing view-depth boundaries instead. Splits stay fixed as terrain
loads and the camera's near/far planes change. Cascade fits cover the complete
receiver intervals, including foreground geometry exposed by logarithmic depth
in front of the camera's nominal near plane.

| Control | Meaning |
| --- | --- |
| Resolution | Width and height of each map; 256-8192, bounded by hardware |
| Fast / `HARD` | One hardware bilinear depth comparison (2 x 2 PCF) |
| Balanced / `PCF` | Continuous box filter; four bilinear comparisons at the default radius |
| Soft / `SOFT` | Continuous tent filter; smooth, weighted edges |
| Softness | Filter radius in texels, independent of resolution; 0-8 |
| Sunlight in shadow | Minimum direct-sun visibility; 0 blocks sunlight completely |
| Depth bias | Receiver bias in world texel units, adjusted for surface slope |
| Caster reach | Upstream distance admitting objects outside the receiver frustum |
| Cascade blend | Overlap fraction and fade width at maximum distance; 0 disables it |

Longer caster reach can increase the amount of geometry rendered and reduce depth
precision. Use coverage appropriate for the scene. Bias trades self-shadow acne
against detached contact shadows. Large filter radii or shallow sunlight can require
more bias. This is fixed-radius PCF, not physically varying penumbra width.
Both filtered modes cover every texel in the footprint, combining adjacent weighted
texels into hardware bilinear comparisons. Wider softness settings add samples
(up to 81 comparisons at eight texels) instead of separating a few taps into duplicate
shadow edges. Radii up to half a texel use the hardware's single bilinear comparison.

## Architecture and compatibility

Each camera/cull-visitor pair owns a depth texture array. Camera and uniform state
are double-buffered for OSG cull/draw overlap; the GPU depth target is reused within
the view's ordered command stream. Expired cameras are removed on subsequent culls.
Resources are allocated lazily and rebuilt only for resolution, cascade count, or
texture-unit changes. Changing darkness, filtering, bias, or ranges with the same
count does not rebuild maps. By default every active cascade updates every frame.

### OpenGL 4.6 submission

Owned shadow cameras share one Chonk classification per drawable placement and primary
view. The compute shader selects LOD using the primary view, then tests each surviving
instance against all cascade light volumes. A second dispatch compacts nonempty draw
commands. OpenGL 4.6 indirect-count drawing consumes those counts directly on the GPU.
The automatic path uses NVIDIA bindless indirect-count drawing when available; the
core path groups commands by shared vertex/index buffers. This does not remove the
existing NVGL requirements of Chonk geometry and materials.
Request an OpenGL 4.6 context in the application (for the examples, set
`OSG_GL_CONTEXT_VERSION=4.6` before launch). Lower-version ordinary OSG scenes retain
the ordinary per-cascade draw path; the shadow node does not change a window's context version.

`setSubmission(ShadowCaster::CORE)` selects core indirect-count submission;
`LEGACY` retains independent per-cascade Chonk culling for comparisons. Ordinary OSG
casters keep their application shaders. External shadow cameras keep working
through the existing defines and primary-view uniforms. GPU visibility buffers are
isolated by view and drawable placement, so the main pass and other views cannot
overwrite queued shadow results.

Each active cascade uses its own depth camera and the application's ordinary draw
path. This preserves custom shaders, primitive types, instancing and draw callbacks
without geometry wrappers or layered-rendering compatibility checks.

The shadow node supplies shared culling through the optional, public
[ChonkRenderPass contract](chonk-multiview.md), using its `PER_VIEW` output.
Chonk owns visibility and submission; the shadow node owns cascade fitting, cameras,
render targets and map reuse. External renderers can use the same contract for their
own views or continue using ordinary Chonk cameras. Neither Chonk nor its multi-view
culler depends on private shadow state. Shared classification reduces culling work;
geometry is still processed and depth rasterized in each overlapping cascade.

### Static map reuse

`setCacheEnabled(true)` is an application declaration that caster shaders and custom
render state are static. With this opt-in, an unchanged view can reuse completed maps.
Camera/projection changes, light-space matrix changes, resolution changes, graph
membership, transforms, Chonk revisions, geometry-array edits and ordinary uniform
changes invalidate reuse. Animated callbacks, `DYNAMIC` nodes and paged nodes bypass
the cache. The cache is off by default for arbitrary application shaders.
OSG's automatically installed frame-clock uniforms are ignored under this explicit
static-shader contract; Chonk's finite fade-in period still prevents reuse.

Call `invalidateCache()` after changing custom state attributes, shader source,
application-managed GPU data, or other inputs without tracked revisions. Leave caching
disabled for time-dependent shaders such as wind. Receiver-only changes such as darkness
and filtering do not inherently require rendering the maps again. The diagnostics
`getRenderedCascades()` and `getReusedCascades()` report the most recently culled view.

Eight analytic frustum corners replace the former heap-allocated convex polyhedron.
Double-precision fitting uses a rotation-invariant bounding sphere and a world-anchored
texel grid. Asymmetric perspective, orthographic, and reverse-Z input projections are
supported; shadow depth maps use conventional depth. Receiver coordinates are composed
on the CPU before conversion to shader floats, retaining precision in ECEF scenes.
Cascade depth selection, overlap blending, and a final distance fade prevent abrupt
coverage changes. Stable fitting and interval selection follow the techniques described
in Microsoft's [cascaded shadow maps guide](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps).
Hardware comparison performs depth tests before interpolation; the prior renderer
filtered raw depth and used 16 manually rotated samples.

Shadow, depth, and picking cameras bypass recursive shadow generation without hiding
the visible subgraph. The depth passes preserve these public defines unchanged:

* `OE_IS_SHADOW_CAMERA`
* `OE_IS_DEPTH_CAMERA`

The `oe_shadowToPrimaryMatrix`, `oe_primaryProjectionMatrix`, and `oe_primaryViewport`
uniforms still supply the primary view to terrain morphing and Chonk LOD selection.
The lighting contract remains `OE_SHADOWING` plus `oe_shadow_visibility`, affecting
direct solar lighting while retaining ambient and environment contributions.

The `ShadowCaster` API remains available. `setBlurFactor`/`getBlurFactor` use normalized
texture coordinates for compatibility; new code should use `setFilterRadius` in texels.
Invalid ranges are rejected before they can disrupt live resources. Make graph and
configuration changes during synchronized update traversal; EnvironmentGUI queues its
edits there. A directional sun is supported; local point/spot shadow maps are not.
