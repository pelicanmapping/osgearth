FeatureImageLayer
=================

``FeatureImageLayer`` reads vector features from a feature source and draws
them as an image layer using an osgEarth style sheet. By default it rasterizes
each tile on the CPU with Blend2D (or AGG Lite when Blend2D is unavailable).

Slug rendering (prototype)
--------------------------

Set the layer's ``rendering`` property to ``slug`` to use the Slughorn SDK
instead. This path builds a Slug curve/band atlas for each terrain tile and
evaluates the vector coverage in a terrain fragment shader. For example::

    <FeatureImage name="Slug vectors" rendering="slug">
        <ogrfeatures url="../data/world.shp"/>
        <styles>
            <style type="text/css">
                default {
                    fill: #ff770080;
                    stroke: #ffff00;
                    stroke-width: 5km;
                }
            </style>
        </styles>
    </FeatureImage>

The equivalent programmatic API is::

    featureImageLayer->setRenderingTechnique("slug");

Call ``setRenderingTechnique`` before opening or adding the layer to a map.
The default value is ``raster`` and preserves the existing behavior.

The prototype supports polygon fills and outlines, line strokes (including
outline width, join, and cap), point circles, per-feature style overrides, and
the layer background color. Text, icon/skin, coverage, gradient, and stippled
line symbols are not yet supported. Slug imagery morphing is disabled because
parent and child tiles contain independently packed atlases. A tile is
currently limited to 64 Slug style passes.

Build configuration
-------------------

Slug support is enabled by the ``OSGEARTH_ENABLE_SLUGHORN`` CMake option. The
build fetches a pinned Slughorn revision. Turn the option off to build osgEarth
without the dependency; a layer configured with ``rendering="slug"`` will then
return a configuration error when opened.
