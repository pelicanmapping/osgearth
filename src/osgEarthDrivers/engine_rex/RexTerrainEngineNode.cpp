/* osgEarth
* Copyright 2008-2014 Pelican Mapping
* MIT License
*/
#include "RexTerrainEngineNode"
#include "Shaders"
#include "TerrainCuller"
#include "GeometryPool"
#include "CreateTileImplementation"

#include <osgEarth/ImageUtils>
#include <osgEarth/Registry>
#include <osgEarth/VirtualProgram>
#include <osgEarth/MapModelChange>
#include <osgEarth/Threading>
#include <osgEarth/ShaderLoader>
#include <osgEarth/ObjectIndex>
#include <osgEarth/Metrics>
#include <osgEarth/Elevation>
#include <osgEarth/LandCover>
#include <osgEarth/ShaderFactory>

#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/CullFace>
#include <osg/Stats>

#include <cstdlib> // for getenv

#define LC "[RexTerrainEngineNode] "

using namespace osgEarth::REX;
using namespace osgEarth;

#define DEFAULT_MAX_LOD 19u

//------------------------------------------------------------------------

namespace
{
    // adapter that lets RexTerrainEngineNode listen to Map events
    struct RexTerrainEngineNodeMapCallbackProxy : public MapCallback
    {
        RexTerrainEngineNodeMapCallbackProxy(RexTerrainEngineNode* node) : _node(node) { }
        osg::observer_ptr<RexTerrainEngineNode> _node;

        void onMapModelChanged(const MapModelChange& change) override {
            osg::ref_ptr<RexTerrainEngineNode> node;
            if (_node.lock(node))
                node->onMapModelChanged(change);
        }
    };


    /**
    * Run this visitor whenever you remove a layer, so that each
    * TileNode can update its render model and get rid of passes
    * that no longer exist.
    */
    struct PurgeOrphanedLayers : public osg::NodeVisitor
    {
        const Map* _map;
        const RenderBindings& _bindings;
        unsigned _count;

        PurgeOrphanedLayers(const Map* map, RenderBindings& bindings) : _map(map), _bindings(bindings), _count(0u)
        {
            setTraversalMode(TRAVERSE_ALL_CHILDREN);
            setNodeMaskOverride(~0);
        }

        void apply(osg::Node& node)
        {
            TileNode* tileNode = dynamic_cast<TileNode*>(&node);
            if (tileNode)
            {
                apply(*tileNode);
            }
            traverse(node);
        }

        void apply(TileNode& tileNode)
        {
            TileRenderModel& model = tileNode.renderModel();

            for (int p = 0; p < model._passes.size(); ++p)
            {
                RenderingPass& pass = model._passes[p];

                // if the map doesn't contain a layer with a matching UID,
                // or if the layer is now disabled, remove it from the render model.
                Layer* layer = _map->getLayerByUID(pass.sourceUID());
                if (layer == nullptr || layer->isOpen() == false)
                {
                    model._passes.erase(model._passes.begin() + p);
                    --p;
                    _count++;
                }
            }

            // For shared samplers we need to refresh the list if one of them
            // goes inactive (as is the case when removing a shared layer)
            tileNode.refreshSharedSamplers(_bindings);
        }
    };
}

//------------------------------------------------------------------------

RexTerrainEngineNode::RexTerrainEngineNode() :
    TerrainEngineNode(),
    _terrain(0L),
    _batchUpdateInProgress(false),
    _refreshRequired(false),
    _stateUpdateRequired(false),
    _renderModelUpdateRequired(false),
    _morphTerrainSupported(true),
    _frameLastUpdated(0u),
    _ppUID(0)
{
    // activate update traversals for this node.
    ADJUST_UPDATE_TRAV_COUNT(this, +1);

    // Necessary for pager object data
    // Note: Do not change this value. Apps depend on it to
    // detect being inside a terrain traversal.
    this->setName("rex");

    // unique ID for this engine:
    _uid = osgEarth::createUID();

    // always require elevation.
    _requirements.elevationTextures = true;

    // static shaders.
    osg::StateSet* stateset = getOrCreateStateSet();
    stateset->setName("Terrain node");

    _surfaceSS = new osg::StateSet();
    _surfaceSS->setName("Terrain surface");

    _imageLayerSS = new osg::StateSet();
    _imageLayerSS->setName("Terrain image layer");

    _terrain = new osg::Group();
    _terrainSS = _terrain->getOrCreateStateSet();
    _terrainSS->setName("Terrain terrain");

    addChild(_terrain.get());

    _cachedLayerExtentsComputeRequired = true;

    _updatedThisFrame = false;
}

RexTerrainEngineNode::~RexTerrainEngineNode()
{
    if (_unloader.valid())
        _unloader->drain();

    if (_ppUID > 0)
        Registry::instance()->getShaderFactory()->removePreProcessorCallback(_ppUID);
}

void
RexTerrainEngineNode::resizeGLObjectBuffers(unsigned maxSize)
{
    TerrainEngineNode::resizeGLObjectBuffers(maxSize);

    getStateSet()->resizeGLObjectBuffers(maxSize);
    _terrainSS->resizeGLObjectBuffers(maxSize);
    _surfaceSS->resizeGLObjectBuffers(maxSize);
    _imageLayerSS->resizeGLObjectBuffers(maxSize);

    // TODO: where should this live? MapNode?
    LayerVector layers;
    getMap()->getLayers(layers);
    for (LayerVector::const_iterator i = layers.begin(); i != layers.end(); ++i)
    {
        if ((*i)->getStateSet()) {
            (*i)->getStateSet()->resizeGLObjectBuffers(maxSize);
        }
    }
}

void
RexTerrainEngineNode::releaseGLObjects(osg::State* state) const
{
    if (_imageLayerSS.valid())
        _imageLayerSS->releaseGLObjects(state);

    if (_surfaceSS.valid())
        _surfaceSS->releaseGLObjects(state);

    // release the LayerDrawables
    for (auto& p : _persistent)
    {
        for (auto& d : p.second._drawables)
        {
            d.second->releaseGLObjects(state);
        }
    }

    if (_engineContext.valid())
        _engineContext->_textures->releaseGLObjects(state);

    TerrainEngineNode::releaseGLObjects(state);
}

void
RexTerrainEngineNode::shutdown()
{
    TerrainEngineNode::shutdown();
    _merger->clear();
}

std::string
RexTerrainEngineNode::getJobArenaName() const
{
    return ARENA_LOAD_TILE;
}

unsigned
RexTerrainEngineNode::getNumResidentTiles() const
{
    return _tiles ? _tiles->size() : 0u;
}

void
RexTerrainEngineNode::onSetMap()
{
    OE_SOFT_ASSERT_AND_RETURN(_map.valid(), void());

    _morphingSupported = true;
    auto options = getOptions();
    if (options.getLODMethod() == LODMethod::SCREEN_SPACE)
    {
        OE_INFO << LC << "LOD method = pixel size; pixel tile size = " << options.getTilePixelSize() << std::endl;

        // force morphing off for PSOS mode
        _morphingSupported = false;
    }

    // tessellation check
    if (_optionsConcrete.gpuTessellation() == true &&
        GLUtils::useNVGL() == false)
    {
        OE_WARN << LC << "GPU tessellation is only supported in NVGL mode. Disabling." << std::endl;
    }

    // morphing imagery LODs requires we bind parent textures to their own unit.
    if (options.getMorphImagery() && _morphingSupported)
    {
        _requirements.parentTextures = true;
    }

    // Terrain morphing doesn't work in projected maps:
    if (_map->getSRS()->isProjected())
    {
        _morphTerrainSupported = false;
    }

    // check for normal map generation (required for lighting).
    _requirements.normalTextures = (options.getUseNormalMaps() == true);

    // don't know how to set this up so just do it
    _requirements.landCoverTextures = (options.getUseLandCover() == true);

    // ensure we get full coverage at the first LOD.
    _requirements.fullDataAtFirstLod = true;

    // A shared registry for tile nodes in the scene graph. Enable revision tracking
    // if requested in the options. Revision tracking lets the registry notify all
    // live tiles of the current map revision so they can inrementally update
    // themselves if necessary.
    _tiles = std::make_shared<TileNodeRegistry>();
    _tiles->setFrameClock(&_clock);
    _tiles->setNotifyNeighbors(options.getNormalizeEdges() == true);

    // A shared geometry pool.
    _geometryPool = new GeometryPool();
    this->addChild(_geometryPool.get());

    // Geometry compiler/merger
    _merger = new Merger();
    _merger->setMergesPerFrame(options.getMergesPerFrame());
    this->addChild(_merger.get());

    // Loader concurrency (size of the thread pool)
    unsigned concurrency = options.getConcurrency();
    const char* concurrency_str = ::getenv("OSGEARTH_TERRAIN_CONCURRENCY");
    if (concurrency_str)
        concurrency = Strings::as<unsigned>(concurrency_str, concurrency);
    jobs::get_pool(ARENA_LOAD_TILE)->set_concurrency(concurrency);

    // Make a tile unloader
    _unloader = new UnloaderGroup(_tiles.get(), getOptions());
    _unloader->setFrameClock(&_clock);
    this->addChild(_unloader.get());

    // Initialize the core render bindings.
    setupRenderBindings();

    // install a layer callback for processing further map actions:
    _map->addMapCallback(new RexTerrainEngineNodeMapCallbackProxy(this));

    // Prime with existing layers:
    _batchUpdateInProgress = true;

    LayerVector layers;
    _map->getLayers(layers);
    for (LayerVector::const_iterator i = layers.begin(); i != layers.end(); ++i)
        addLayer(i->get());

    _batchUpdateInProgress = false;

    // Establish a new engine context
    _engineContext = new EngineContext(
        _map.get(),
        this, // engine
        _geometryPool.get(),
        _merger.get(),
        _tiles,
        _renderBindings,
        _selectionInfo,
        &_clock);

    // Calculate the LOD morphing parameters:
    unsigned maxLOD = options.getMaxLOD();

    _selectionInfo.initialize(
        0u, // always zero, not the terrain options firstLOD
        maxLOD,
        _map->getProfile(),
        options.getMinTileRangeFactor(),
        options.getRestrictPolarSubdivision());

    TerrainResources* res = getResources();
    for (unsigned lod = 0; lod <= maxLOD; ++lod)
        res->setVisibilityRangeHint(lod, _selectionInfo.getLOD(lod)._visibilityRange);

    // set up the initial graph
    refresh();

    // now that we have a map, set up to recompute the bounds
    dirtyBound();

    // preprocess shaders to parse the "oe_use_shared_layer" directive
    // for shared layer samplers
    if (_ppUID > 0)
    {
        Registry::instance()->getShaderFactory()->removePreProcessorCallback(_ppUID);
    }

    _ppUID = Registry::instance()->getShaderFactory()->addPreProcessorCallback(
        this,
        [](std::string& source, osg::Referenced* host)
        {
            RexTerrainEngineNode* rex = dynamic_cast<RexTerrainEngineNode*>(host);
            if (!rex) return;

            std::string line;
            std::vector<std::string> tokens;

            while (ShaderLoader::getPragmaValueAsTokens(
                source,
                "#pragma oe_use_shared_layer",
                line,
                tokens))
            {
                if (tokens.size() == 2)
                {
                    std::ostringstream buf;

                    if (GLUtils::useNVGL())
                    {
                        ShadersGL4 sh;
                        std::string incStrGL4 = ShaderLoader::load(sh.ENGINE_TYPES, sh);
                        if (source.find(incStrGL4) == std::string::npos)
                        {
                            buf << incStrGL4 << "\n";
                        }

                        // find the shared index.
                        int index = -1;
                        const RenderBindings& bindings = rex->_renderBindings;
                        for (int i = SamplerBinding::SHARED; i < (int)bindings.size() && index < 0; ++i)
                        {
                            if (bindings[i].samplerName() == tokens[0])
                            {
                                index = i - SamplerBinding::SHARED;
                            }
                        }
                        if (index < 0)
                        {
                            OE_WARN << LC << "Cannot find a shared sampler binding for " << tokens[0] << std::endl;
                            Strings::replaceIn(source, line, "// error, no matching sampler binding");
                            continue;
                        }

                        buf << "#define " << tokens[0] << "_HANDLE oe_terrain_tex[oe_tile[oe_tileID].sharedIndex[" << index << "]]\n"
                            << "#define " << tokens[0] << " sampler2D(" << tokens[0] << "_HANDLE)\n"
                            << "#define " << tokens[1] << " oe_tile[oe_tileID].sharedMat[" << index << "]\n";
                    }
                    else
                    {
                        buf << "uniform sampler2D " << tokens[0] << ";\n"
                            << "uniform mat4 " << tokens[1] << ";\n";
                    }

                    Strings::replaceIn(source, line, buf.str());
                }
                else
                {
                    Strings::replaceIn(source, line, "// error, missing token(s)");
                }
            }
        }
    );
}


osg::BoundingSphere
RexTerrainEngineNode::computeBound() const
{
    return TerrainEngineNode::computeBound();
}

void
RexTerrainEngineNode::invalidateRegion(
    const GeoExtent& extent,
    unsigned         minLevel,
    unsigned         maxLevel)
{
    if (_tiles)
    {
        GeoExtent extentLocal = extent;

        if (extent.isValid() && !extent.getSRS()->isHorizEquivalentTo(this->getMap()->getSRS()))
        {
            extent.transform(this->getMap()->getSRS(), extentLocal);
        }

        // Add the entire map to the manifest :)
        CreateTileManifest manifest;

        // When updating a subset of layers, override progressive mode
        // so that the visible LOD gets updated first:
        manifest.setProgressive(false);

        LayerVector layers;
        _map->getLayers(layers);
        for (LayerVector::const_iterator i = layers.begin(); i != layers.end(); ++i)
        {
            manifest.insert(i->get());
        }

        _tiles->setDirty(extentLocal, minLevel, maxLevel, manifest);
    }
}

void
RexTerrainEngineNode::invalidateRegion(
    const std::vector<const Layer*> layers,
    const GeoExtent& extent,
    unsigned minLevel,
    unsigned maxLevel)
{
    if (_tiles)
    {
        GeoExtent extentLocal = extent;

        if (extent.isValid() && !extent.getSRS()->isHorizEquivalentTo(this->getMap()->getSRS()))
        {
            extent.transform(this->getMap()->getSRS(), extentLocal);
        }

        CreateTileManifest manifest;

        // When updating a subset of layers, override progressive mode
        // so that the visible LOD gets updated first:
        manifest.setProgressive(false);

        for (std::vector<const Layer*>::const_iterator i = layers.begin();
            i != layers.end();
            ++i)
        {
            if (*i)
            {
                manifest.insert(*i);
            }
        }

        _tiles->setDirty(extentLocal, minLevel, maxLevel, manifest);
    }
}

void
RexTerrainEngineNode::refresh(bool forceDirty)
{
    if (_batchUpdateInProgress)
    {
        _refreshRequired = true;
    }
    else
    {
        _refreshRequired = false;

        if (_terrain.valid())
        {
            _terrain->releaseGLObjects();
            _terrain->removeChildren(0, _terrain->getNumChildren());
        }

        // clear the loader:
        _merger->clear();

        // clear out the tile registry:
        if (_tiles)
        {
            _tiles->releaseAll(nullptr);
        }

        // scrub the geometry pool:
        _geometryPool->clear();

        // Build the first level of the terrain.
        // Collect the tile keys comprising the root tiles of the terrain.
        std::vector<TileKey> keys;
        getMap()->getProfile()->getAllKeysAtLOD(getOptions().getFirstLOD(), keys);

        // create a root node for each root tile key.
        OE_DEBUG << LC << "Creating " << keys.size() << " root keys." << std::endl;

        // We need to take a self-ref here to ensure that the TileNode's data loader
        // can use its observer_ptr back to the terrain engine.
        this->ref();

        // Load all the root key tiles.
        jobs::context context;
        context.group = jobs::jobgroup::create();
        context.pool = jobs::get_pool(ARENA_LOAD_TILE);

        for (unsigned i = 0; i < keys.size(); ++i)
        {
            TileNode* tileNode = new TileNode(
                keys[i],
                nullptr, // parent
                _engineContext.get(),
                nullptr); // progress

            // Root nodes never expire
            tileNode->setDoNotExpire(true);

            // Add it to the scene graph
            _terrain->addChild(tileNode);

            // Post-add initialization:
            tileNode->initializeData();

            // And load the tile's data
            jobs::dispatch([tileNode]() { tileNode->loadSync(); }, context);
        }

        // wait for all loadSync calls to complete
        context.group->join();

        // release the self-ref.
        this->unref_nodelete();

        // Set up the state sets.
        updateState();
    }
}

osg::StateSet*
RexTerrainEngineNode::getTerrainStateSet()
{
    OE_SOFT_ASSERT_AND_RETURN(_terrain.valid(), nullptr);
    return _terrainSS.get();
}

osg::StateSet*
RexTerrainEngineNode::getSurfaceStateSet()
{
    return _surfaceSS.get();
}

void
RexTerrainEngineNode::setupRenderBindings()
{
    // Release any pre-existing bindings:
    for (unsigned i = 0; i < _renderBindings.size(); ++i)
    {
        SamplerBinding& b = _renderBindings[i];
        if (b.isActive())
        {
            getResources()->releaseTextureImageUnit(b.unit());
        }
    }
    _renderBindings.clear();

    // "SHARED" is the start of shared layers, so we always want the bindings
    // vector to be at least that size.
    _renderBindings.resize(SamplerBinding::SHARED);

    SamplerBinding& color = _renderBindings[SamplerBinding::COLOR];
    color.usage() = SamplerBinding::COLOR;
    color.samplerName() = "oe_layer_tex";
    color.matrixName() = "oe_layer_texMatrix";
    color.setDefaultTexture(new osg::Texture2D(ImageUtils::createEmptyImage(1, 1)));
    color.getDefaultTexture()->setName("terrain default color");

    if (!GLUtils::useNVGL())
        getResources()->reserveTextureImageUnit(color.unit(), "Terrain Color");

    if (_requirements.elevationTextures)
    {
        SamplerBinding& elevation = _renderBindings[SamplerBinding::ELEVATION];
        elevation.usage() = SamplerBinding::ELEVATION;
        elevation.samplerName() = "oe_tile_elevationTex";
        elevation.matrixName() = "oe_tile_elevationTexMatrix";
        elevation.setDefaultTexture(osgEarth::createEmptyElevationTile());
        elevation.getDefaultTexture()->setName("terrain default elevation");

        if (!GLUtils::useNVGL())
            getResources()->reserveTextureImageUnit(elevation.unit(), "Terrain Elevation");
    }

    if (_requirements.normalTextures)
    {
        SamplerBinding& normal = _renderBindings[SamplerBinding::NORMAL];
        normal.usage() = SamplerBinding::NORMAL;
        normal.samplerName() = "oe_tile_normalTex";
        normal.matrixName() = "oe_tile_normalTexMatrix";
        normal.setDefaultTexture(osgEarth::createEmptyNormalMapTexture());
        normal.getDefaultTexture()->setName("terrain default normalmap");

        if (!GLUtils::useNVGL())
            getResources()->reserveTextureImageUnit(normal.unit(), "Terrain Normals");
    }

    if (_requirements.parentTextures)
    {
        SamplerBinding& colorParent = _renderBindings[SamplerBinding::COLOR_PARENT];
        colorParent.usage() = SamplerBinding::COLOR_PARENT;
        colorParent.samplerName() = "oe_layer_texParent";
        colorParent.matrixName() = "oe_layer_texParentMatrix";

        if (!GLUtils::useNVGL())
            getResources()->reserveTextureImageUnit(colorParent.unit(), "Terrain Parent Color");
    }

    if (_requirements.landCoverTextures)
    {
        SamplerBinding& landCover = _renderBindings[SamplerBinding::LANDCOVER];
        landCover.usage() = SamplerBinding::LANDCOVER;
        landCover.samplerName() = "oe_tile_landCoverTex";
        landCover.matrixName() = "oe_tile_landCoverTexMatrix";
        landCover.setDefaultTexture(LandCover::createEmptyTexture());
        landCover.getDefaultTexture()->setName("terrain default landcover");
        getOrCreateStateSet()->setDefine("OE_LANDCOVER_TEX", landCover.samplerName());
        getOrCreateStateSet()->setDefine("OE_LANDCOVER_TEX_MATRIX", landCover.matrixName());

        if (!GLUtils::useNVGL())
            getResources()->reserveTextureImageUnit(landCover.unit(), "Terrain Land Cover");
    }

    // Apply a default, empty texture to each render binding.
    if (!GLUtils::useNVGL())
    {
        OE_DEBUG << LC << "Render Bindings:\n";
        for (unsigned i = 0; i < _renderBindings.size(); ++i)
        {
            SamplerBinding& b = _renderBindings[i];
            if (b.isActive())
            {
                _terrainSS->addUniform(new osg::Uniform(b.samplerName().c_str(), b.unit()));
                _terrainSS->setTextureAttribute(b.unit(), b.getDefaultTexture());
                OE_DEBUG << LC << " > Bound \"" << b.samplerName() << "\" to unit " << b.unit() << "\n";
            }
        }
    }
}

void
RexTerrainEngineNode::dirtyState()
{
    // TODO: perhaps defer this until the next update traversal so we don't
    // reinitialize the state multiple times unnecessarily.
    updateState();
}

void
RexTerrainEngineNode::dirtyTerrainOptions()
{
    TerrainEngineNode::dirtyTerrainOptions();

    auto options = getOptions();

    auto& arena = getEngineContext()->_textures;
    if (arena)
    {
        arena->setMaxTextureSize(options.getMaxTextureSize());
        arena->setAutoPaging(options.getGPUPaging());
    }

    _tiles->setNotifyNeighbors(options.getNormalizeEdges() == true);

    _merger->setMergesPerFrame(options.getMergesPerFrame());

    jobs::get_pool(ARENA_LOAD_TILE)->set_concurrency(options.getConcurrency());

    //_engineContext->getSelectionInfo().setMTRFMappingFunction(options.getMTRFMappingFunction());

    updateState();
}

void
RexTerrainEngineNode::cacheAllLayerExtentsInMapSRS()
{
    // Only call during update
    LayerVector layers;
    getMap()->getLayers(layers);
    for (LayerVector::const_iterator i = layers.begin();
        i != layers.end();
        ++i)
    {
        cacheLayerExtentInMapSRS(i->get());
    }
}

void
RexTerrainEngineNode::cull_traverse(osg::NodeVisitor& nv)
{
    OE_PROFILING_ZONE;

    osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(&nv);

    // fetch the persistent data associated with this traversal.
    _persistent.lock();
    TerrainRenderData::PersistentData& pd = _persistent[cv->getCurrentCamera()];
    pd._lastCull = *nv.getFrameStamp();
    _persistent.unlock();

    // Prepare the culler:
    TerrainCuller culler;
    culler.reset(cv, pd, getEngineContext(), _cachedLayerExtents);

    // install the SSE:
    float global_sse = 0.0f;
    nv.getUserValue("oe_sse", global_sse);
    culler.setScreenSpaceError(global_sse);
    culler.setTerrainScreenSpaceError(getScreenSpaceError(*cv));

    // Assemble the terrain drawables:
    _terrain->accept(culler);

    // Optional per-camera diagnostics; counts surface tiles submitted by this cull,
    // once each regardless of imagery passes, not shared resident tiles or GPU draws.
    auto* stats = cv->getCurrentCamera() ? cv->getCurrentCamera()->getStats() : nullptr;
    if (stats && stats->collectStats("osgEarth.SSE") && nv.getFrameStamp())
    {
        unsigned frame = nv.getFrameStamp()->getFrameNumber();
        stats->setAttribute(frame, "osgEarth.MapSSE", global_sse);
        stats->setAttribute(frame, "osgEarth.TerrainSSE", culler.getTerrainScreenSpaceError());
        stats->setAttribute(frame, "osgEarth.SurfaceTiles", culler._surfaceTiles);
    }

    // If we're using geometry pooling, optimize the drawable forf shared state
    // by sorting the draw commands.
    // Skip if using GL4/indirect rendering. Actually seems to hurt?
    // TODO: benchmark this further to see whether it's worthwhile
    if (!GLUtils::useNVGL() &&
        getEngineContext()->getGeometryPool()->isEnabled())
    {
        culler._terrain.sortDrawCommands();
    }

    // The common stateset for the terrain group:
    cv->pushStateSet(_terrainSS.get());

    // Push all the layers to draw on to the cull visitor in the order in which
    // they appear in the map.
    LayerDrawable* lastLayer = nullptr;
    unsigned order = 0;
    bool surfaceStateSetPushed = false;
    bool imageLayerStateSetPushed = false;
    int layersDrawn = 0;
    unsigned surfaceDrawOrder = 0;

    std::vector<LayerDrawable*> patchLayers;

    for (auto layerDrawable : culler._terrain._layerList)
    {
        if (layerDrawable->_tiles.empty() == false &&
            layerDrawable->_patchLayer)
        {
            patchLayers.push_back(layerDrawable);
        }
    }

    for (auto layerDrawable : culler._terrain._layerList)
    {
        if (!layerDrawable->_tiles.empty())
        {
            // skip patch layers for now
            if (layerDrawable->_patchLayer)
                continue;

            lastLayer = layerDrawable;

            // if this is a RENDERTYPE_TERRAIN_SURFACE, we need to activate either the
            // default surface state set or the image layer state set.
            if (layerDrawable->_renderType == Layer::RENDERTYPE_TERRAIN_SURFACE)
            {
                layerDrawable->_surfaceDrawOrder = surfaceDrawOrder++;

                if (!surfaceStateSetPushed)
                {
                    cv->pushStateSet(_surfaceSS.get());
                    surfaceStateSetPushed = true;
                }

                if (layerDrawable->_imageLayer || layerDrawable->_layer == nullptr)
                {
                    if (!imageLayerStateSetPushed)
                    {
                        cv->pushStateSet(_imageLayerSS.get());
                        imageLayerStateSetPushed = true;
                    }
                }
                else
                {
                    if (imageLayerStateSetPushed)
                    {
                        cv->popStateSet();
                        imageLayerStateSetPushed = false;
                    }
                }
            }

            else
            {
                if (imageLayerStateSetPushed)
                {
                    cv->popStateSet();
                    imageLayerStateSetPushed = false;
                }
                if (surfaceStateSetPushed)
                {
                    cv->popStateSet();
                    surfaceStateSetPushed = false;
                }
            }

            if (layerDrawable->_layer)
            {
                layerDrawable->_layer->apply(layerDrawable, cv);
            }
            else
            {
                layerDrawable->accept(*cv);
            }

            ++layersDrawn;
        }
    }

    // clear out the statesets:
    if (imageLayerStateSetPushed)
    {
        cv->popStateSet();
        imageLayerStateSetPushed = false;
    }

    if (surfaceStateSetPushed)
    {
        cv->popStateSet();
        surfaceStateSetPushed = false;
    }

    // patch layers go last
    for (auto layerDrawable : patchLayers)
    {
        lastLayer = layerDrawable;

        TileBatch batch(nullptr);
        for (auto& tile : layerDrawable->_tiles)
            batch._tiles.push_back(&tile);

        if (layerDrawable->_patchLayer->getStateSet())
            cv->pushStateSet(layerDrawable->_patchLayer->getStateSet());

        if (layerDrawable->_patchLayer->getCullCallback()) // backwards compat
            layerDrawable->_patchLayer->apply(layerDrawable, cv);
        else
            layerDrawable->_patchLayer->cull(batch, *cv);

        if (layerDrawable->_patchLayer->getStateSet())
            cv->popStateSet();
    }

    // The last layer to render must clear up the OSG state,
    // otherwise it will be corrupt and can lead to crashing.
    if (lastLayer)
    {
        lastLayer->_clearOsgState = true;
    }

    // pop the common terrain state set
    cv->popStateSet();

    // If the culler found any orphaned data, we need to update the render model
    // during the next update cycle.
    if (culler._orphanedPassesDetected > 0u)
    {
        _renderModelUpdateRequired = true;
    }

    // we don't call this b/c we don't want _terrain
    //TerrainEngineNode::traverse(nv);

    // traverse all the other children (geometry pool, loader/unloader, etc.)
    _geometryPool->accept(nv);
    _merger->accept(nv);
    _unloader->accept(nv);
}

void
RexTerrainEngineNode::update_traverse(osg::NodeVisitor& nv)
{
    OE_PROFILING_ZONE;

    if (_renderModelUpdateRequired)
    {
        OE_PROFILING_ZONE_NAMED("PurgeOrphanedLayers");

        PurgeOrphanedLayers visitor(getMap(), _renderBindings);
        _terrain->accept(visitor);
        _renderModelUpdateRequired = false;
    }

    // Called once on the first update pass to ensure that all existing
    // layers have their extents cached properly
    if (_cachedLayerExtentsComputeRequired)
    {
        cacheAllLayerExtentsInMapSRS();
        _cachedLayerExtentsComputeRequired = false;
    }
    else
    {
        OE_PROFILING_ZONE_NAMED("Update cached layer extents");

        // Update the cached layer extents as necessary.
        osg::ref_ptr<const Layer> layer;
        for (auto& layerExtent : _cachedLayerExtents)
        {
            layerExtent.second._layer.lock(layer);
            if (layer.valid() && layer->getRevision() > layerExtent.second._revision)
            {
                layerExtent.second._extent = _map->getProfile()->clampAndTransformExtent(layer->getExtent());
                layerExtent.second._revision = layer->getRevision();
            }
        }
    }

    {
        OE_PROFILING_ZONE_NAMED("Update open layers");

        // Call update() on all open layers
        LayerVector layers;
        _map->getLayers(layers, [&](const Layer* layer) {
            return layer->isOpen();
            });

        for (auto& layer : layers)
        {
            layer->update(nv);
        }
    }

    // Call update on the tile registry
    _tiles->update(nv);

    // check on the persistent data cache
    _persistent.lock();
    const osg::FrameStamp* fs = nv.getFrameStamp();
    for (auto iter : _persistent)
    {
        if (fs->getFrameNumber() - iter.second._lastCull.getFrameNumber() > 60)
        {
            _persistent.erase(iter.first);
            break;
        }
    }
    _persistent.unlock();

#if 1
    // traverse the texture arena since it's not in the scene graph.
    auto* arena = getEngineContext()->textures();
    if (arena)
        arena->update(nv);
#endif
}

void
RexTerrainEngineNode::traverse(osg::NodeVisitor& nv)
{
    if (nv.getVisitorType() == nv.UPDATE_VISITOR)
    {
        if (!_updatedThisFrame.exchange(true))
        {
            _clock.update();
            update_traverse(nv);
            TerrainEngineNode::traverse(nv);
        }
    }

    else if (nv.getVisitorType() == nv.CULL_VISITOR)
    {
        _updatedThisFrame.exchange(false);
        _clock.cull();
        cull_traverse(nv);
    }

    else
    {
        TerrainEngineNode::traverse(nv);
    }
}

void
RexTerrainEngineNode::onMapModelChanged(const MapModelChange& change)
{
    if (change.getAction() == MapModelChange::BEGIN_BATCH_UPDATE)
    {
        _batchUpdateInProgress = true;
    }

    else if (change.getAction() == MapModelChange::END_BATCH_UPDATE)
    {
        _batchUpdateInProgress = false;

        if (_refreshRequired)
            refresh();

        if (_stateUpdateRequired)
            updateState();
    }

    else
    {

        // dispatch the change handler
        if (change.getLayer())
        {
            // then apply the actual change:
            switch (change.getAction())
            {
            case MapModelChange::ADD_LAYER:
            case MapModelChange::OPEN_LAYER:
                addLayer(change.getLayer());
                break;

            case MapModelChange::REMOVE_LAYER:
            case MapModelChange::CLOSE_LAYER:
                if (change.getImageLayer())
                    removeImageLayer(change.getImageLayer());
                else if (change.getElevationLayer() || change.getConstraintLayer())
                    removeElevationLayer(change.getLayer());
                break;

            case MapModelChange::MOVE_LAYER:
                if (change.getElevationLayer())
                    moveElevationLayer(change.getElevationLayer());
                break;

            default:
                break;
            }
        }
    }
}

void
RexTerrainEngineNode::cacheLayerExtentInMapSRS(Layer* layer)
{
    OE_SOFT_ASSERT_AND_RETURN(layer != nullptr, void());

    // Store the layer's extent in the map's SRS:
    LayerExtent& le = _cachedLayerExtents[layer->getUID()];
    le._layer = layer;
    le._extent = getMap()->getProfile()->clampAndTransformExtent(layer->getExtent());
}

void
RexTerrainEngineNode::addLayer(Layer* layer)
{
    if (layer)
    {
        if (layer->isOpen())
        {
            if (layer->getRenderType() == Layer::RENDERTYPE_TERRAIN_SURFACE)
                addSurfaceLayer(layer);
            else if (dynamic_cast<ElevationLayer*>(layer) || dynamic_cast<TerrainConstraintLayer*>(layer))
                addElevationLayer(layer);
        }

        cacheLayerExtentInMapSRS(layer);
    }
}

void
RexTerrainEngineNode::addSurfaceLayer(Layer* layer)
{
    if (layer && layer->isOpen())
    {
        ImageLayer* imageLayer = dynamic_cast<ImageLayer*>(layer);
        if (imageLayer)
        {
            // for a shared layer, allocate a shared image unit if necessary.
            if (imageLayer->isShared())
            {
                if (!imageLayer->sharedImageUnit().isSet() && !GLUtils::useNVGL())
                {
                    int temp;
                    if (getResources()->reserveTextureImageUnit(temp, imageLayer->getName().c_str()))
                    {
                        imageLayer->sharedImageUnit() = temp;
                        //OE_INFO << LC << "Image unit " << temp << " assigned to shared layer " << imageLayer->getName() << std::endl;
                    }
                    else
                    {
                        OE_WARN << LC << "Insufficient GPU image units to share layer " << imageLayer->getName() << std::endl;
                    }
                }

                // Build a sampler binding for the shared layer.
                if (imageLayer->sharedImageUnit().isSet() || GLUtils::useNVGL())
                {
                    // Find the next empty SHARED slot:
                    unsigned newIndex = SamplerBinding::SHARED;
                    while (_renderBindings[newIndex].isActive())
                        ++newIndex;

                    // Put the new binding there:
                    SamplerBinding& newBinding = _renderBindings[newIndex];
                    newBinding.usage() = SamplerBinding::SHARED;
                    newBinding.sourceUID() = imageLayer->getUID();
                    newBinding.unit() = imageLayer->sharedImageUnit().get();
                    newBinding.samplerName() = imageLayer->getSharedTextureUniformName();
                    newBinding.matrixName() = imageLayer->getSharedTextureMatrixUniformName();

                    OE_DEBUG << LC
                        << "Shared Layer \"" << imageLayer->getName() << "\" : sampler=\"" << newBinding.samplerName() << "\", "
                        << "matrix=\"" << newBinding.matrixName() << "\", "
                        << "unit=" << newBinding.unit() << "\n";

                    // Install an empty texture for this binding at the top of the graph, so that
                    // a texture is always defined even when the data source supplies no real data.
                    if (newBinding.isActive() && !GLUtils::useNVGL())
                    {
                        osg::ref_ptr<osg::Texture> tex;
                        if (osg::Image* emptyImage = imageLayer->getEmptyImage())
                        {
                            if (emptyImage->r() > 1)
                            {
                                tex = ImageUtils::makeTexture2DArray(emptyImage);
                            }
                            else
                            {
                                tex = new osg::Texture2D(emptyImage);
                            }
                        }
                        else
                        {
                            tex = new osg::Texture2D(ImageUtils::createEmptyImage(1, 1));
                        }
                        tex->setName("default:" + imageLayer->getName());
                        tex->setUnRefImageDataAfterApply(Registry::instance()->unRefImageDataAfterApply().get());
                        _terrainSS->addUniform(new osg::Uniform(newBinding.samplerName().c_str(), newBinding.unit()));
                        _terrainSS->setTextureAttribute(newBinding.unit(), tex.get(), 1);
                        OE_DEBUG << LC << "Bound shared sampler " << newBinding.samplerName() << " to unit " << newBinding.unit() << std::endl;
                    }
                }
            }
        }

        else
        {
            // non-image tile layer.
        }

        if (_terrain)
        {
            // Update the existing render models, and trigger a data reload.
            // Later we can limit the reload to an update of only the new data.
            std::vector<const Layer*> layers;
            layers.push_back(layer);
            invalidateRegion(layers, GeoExtent::INVALID, 0u, INT_MAX);
        }

        updateState();
    }
}


void
RexTerrainEngineNode::removeImageLayer(ImageLayer* layerRemoved)
{
    if (layerRemoved)
    {
        // release its layer drawable
        _persistent.scoped_lock([&]() {
            for (auto& e : _persistent)
                e.second._drawables.erase(layerRemoved);
            });

        // for a shared layer, release the shared image unit.
        if (layerRemoved->isOpen() && layerRemoved->isShared())
        {
            if (layerRemoved->sharedImageUnit().isSet())
            {
                getResources()->releaseTextureImageUnit(*layerRemoved->sharedImageUnit());
                layerRemoved->sharedImageUnit().unset();
            }

            // Remove from RenderBindings (mark as unused)
            for (unsigned i = 0; i < _renderBindings.size(); ++i)
            {
                SamplerBinding& binding = _renderBindings[i];
                if (binding.isActive() && binding.sourceUID() == layerRemoved->getUID())
                {
                    OE_DEBUG << LC << "Binding (" << binding.samplerName() << " unit " << binding.unit() << ") cleared\n";
                    binding.usage().clear();
                    binding.unit() = -1;
                    binding.sourceUID().clear();

                    // Request an update to reset the shared sampler in the scene graph
                    // GW: running this anyway below (PurgeOrphanedLayers), so no need..?
                    _renderModelUpdateRequired = true;
                }
            }
        }

        updateState();
    }

    if (_terrain)
    {
        // Run the update visitor, which will clean out any rendering passes
        // associated with the layer we just removed. This would happen
        // automatically during cull/update anyway, but it's more efficient
        // to do it all at once.
        PurgeOrphanedLayers updater(getMap(), _renderBindings);
        _terrain->accept(updater);
    }

    //OE_INFO << LC << " Updated " << updater._count << " tiles\n";
}

void
RexTerrainEngineNode::addElevationLayer(Layer* layer)
{
    if (layer && layer->isOpen())
    {
        std::vector<const Layer*> layers;
        layers.push_back(layer);
        invalidateRegion(layers, GeoExtent::INVALID, 0u, INT_MAX);
    }
}

void
RexTerrainEngineNode::removeElevationLayer(Layer* layer)
{
    // only need to refresh is the elevation layer is visible.
    if (layer)
    {
        std::vector<const Layer*> layers;
        layers.push_back(layer);
        invalidateRegion(layers, GeoExtent::INVALID, 0u, INT_MAX);
    }
}

void
RexTerrainEngineNode::moveElevationLayer(Layer* layer)
{
    if (layer && layer->isOpen())
    {
        std::vector<const Layer*> layers;
        layers.push_back(layer);
        invalidateRegion(layers, GeoExtent::INVALID, 0u, INT_MAX);
    }
}

// Generates the main shader code for rendering the terrain.
void
RexTerrainEngineNode::updateState()
{
    if (_batchUpdateInProgress)
    {
        _stateUpdateRequired = true;
    }
    else
    {
        // Load up the appropriate shader package:
        REXShaders& shaders = REXShadersFactory::get(GLUtils::useNVGL());

        auto options = getOptions();

        // State that affects any terrain layer (surface, patch, other)
        // AND compute shaders
        {
            // activate standard mix blending.
            _terrainSS->setAttributeAndModes(
                new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA),
                osg::StateAttribute::ON);

            VirtualProgram* terrainVP = VirtualProgram::getOrCreate(_terrainSS.get());
            shaders.load(terrainVP, shaders.sdk());

            // GL4 rendering?
            if (GLUtils::useNVGL())
            {
                _terrainSS->setDefine("OE_USE_GL4");
            }

            // vertex-dimension of each standard terrain tile.
            _terrainSS->setDefine("OE_TILE_SIZE",
                std::to_string(options.getTileSize()));

            // uniform that conveys the layer UID to the shaders; necessary
            // for per-layer branching (like color filters)
            // UID -1 => no image layer (no texture)
            _terrainSS->addUniform(new osg::Uniform(
                "oe_layer_uid", (int)-1));

            // uniform that conveys the render order, since the shaders
            // need to know which is the first layer in order to blend properly
            _terrainSS->addUniform(new osg::Uniform(
                "oe_layer_order", (int)0));

            if (_requirements.elevationTextures)
            {
                // Compute an elevation texture sampling scale/bias so we sample elevation data on center
                // instead of on edge (as we do with color, etc.)
                float bias = getEngineContext()->getUseTextureBorder() ? 1.5 : 0.5;
                float size = (float)ELEVATION_TILE_SIZE;

                _terrainSS->addUniform(new osg::Uniform(
                    "oe_tile_elevTexelCoeff",
                    osg::Vec2f((size - (2.0 * bias)) / size, bias / size)));
            }
        }


        // State that affects surface layers only:
        {
            // required for multipass tile rendering to work
            _surfaceSS->setAttributeAndModes(
                new osg::Depth(osg::Depth::LEQUAL, 0, 1, true));

            // backface culling on
            _surfaceSS->setAttributeAndModes(
                new osg::CullFace(), osg::StateAttribute::ON);

            // untextured terrain skin color
            _surfaceSS->addUniform(new osg::Uniform(
                "oe_terrain_color", options.getColor()));

            // vertical offset of the terrain verts (cloud layer e.g.)
            _surfaceSS->addUniform(new osg::Uniform(
                "oe_terrain_altitude", (float)0.0f));

            // exists do you can override it from above
            _surfaceSS->setDefine("OE_TERRAIN_RENDER_IMAGERY");

            // RENDERTYPE_TERRAIN_SURFACE shaders
            VirtualProgram* surfaceVP = VirtualProgram::getOrCreate(_surfaceSS.get());
            shaders.load(surfaceVP, shaders.vert());
            shaders.load(surfaceVP, shaders.elevation());
            shaders.load(surfaceVP, shaders.normal_map());

            // GPU tessellation:
            if (options.getGPUTessellation())
            {
                shaders.load(surfaceVP, shaders.tessellation());

                _surfaceSS->addUniform(new osg::Uniform("oe_terrain_tessResolutionMeters", options.getTessellationResolution()));
                _surfaceSS->addUniform(new osg::Uniform("oe_terrain_tessMinLevel", options.getTessellationMinLevel()));
                _surfaceSS->addUniform(new osg::Uniform("oe_terrain_tessMaxLevel", options.getTessellationMaxLevel()));


#ifdef HAVE_PATCH_PARAMETER
                // backwards compatibility
                _surfaceSS->setAttributeAndModes(new osg::PatchParameter(3));
#endif
            }

            // Elevation
            if (_requirements.elevationTextures)
            {
                _surfaceSS->setDefine("OE_TERRAIN_RENDER_ELEVATION");
            }

            // Normal mapping
            if (_requirements.normalTextures)
            {
                _surfaceSS->setDefine("OE_TERRAIN_RENDER_NORMAL_MAP");
            }

            // Imagery blending
            if (options.getEnableBlending())
            {
                _surfaceSS->setDefine("OE_TERRAIN_BLEND_IMAGERY");
            }

            // Morphing (imagery and terrain)
            if (_morphingSupported)
            {
                if ((options.getMorphTerrain() && _morphTerrainSupported) ||
                    (options.getMorphImagery()))
                {
                    // GL4 morphing is built into another shader (vert.GL4.glsl)
                    if (!GLUtils::useNVGL())
                        shaders.load(surfaceVP, shaders.morphing());

                    if ((options.getMorphTerrain() && _morphTerrainSupported))
                    {
                        _surfaceSS->setDefine("OE_TERRAIN_MORPH_GEOMETRY");
                    }
                    if (options.getMorphImagery())
                    {
                        _surfaceSS->setDefine("OE_TERRAIN_MORPH_IMAGERY");
                    }
                }
            }

            // Shadowing
            if (options.getCastShadows())
            {
                _surfaceSS->setDefine("OE_TERRAIN_CAST_SHADOWS");
            }

            // Assemble color filter code snippets.
            // Deprecate this someday.
            installColorFilters(surfaceVP);

            // special object ID that denotes the terrain surface.
            _surfaceSS->addUniform(new osg::Uniform(
                Registry::objectIndex()->getObjectIDUniformName().c_str(),
                OSGEARTH_OBJECTID_TERRAIN));
        }

        // STATE for image layers
        VirtualProgram* vp = VirtualProgram::getOrCreate(_imageLayerSS.get());
        shaders.load(vp, shaders.imagelayer());

        // The above shader will integrate opacity itself.
        _imageLayerSS->setDefine("OE_SELF_MANAGE_LAYER_OPACITY");

        _stateUpdateRequired = false;
    }
}

void
RexTerrainEngineNode::installColorFilters(
    VirtualProgram* surfaceVP)
{
    // TODO: DEPRECATE THESE
    bool haveColorFilters = false;
    {
        // Color filter frag function:
        std::string fs_colorfilters =
            "uniform int oe_layer_uid; \n"
            "$COLOR_FILTER_HEAD"
            "void oe_rexEngine_applyFilters(inout vec4 color) \n"
            "{ \n"
            "$COLOR_FILTER_BODY"
            "} \n";

        std::stringstream cf_head;
        std::stringstream cf_body;
        const char* I = "    ";

        bool ifStarted = false;
        ImageLayerVector imageLayers;
        getMap()->getLayers(imageLayers);

        for (int i = 0; i < imageLayers.size(); ++i)
        {
            ImageLayer* layer = imageLayers[i].get();
            if (layer->isOpen())
            {
                // install Color Filter function calls:
                const ColorFilterChain& chain = layer->getColorFilters();
                if (chain.size() > 0)
                {
                    haveColorFilters = true;
                    if (ifStarted) cf_body << I << "else if ";
                    else             cf_body << I << "if ";
                    cf_body << "(oe_layer_uid == " << layer->getUID() << ") {\n";
                    for (ColorFilterChain::const_iterator j = chain.begin(); j != chain.end(); ++j)
                    {
                        const ColorFilter* filter = j->get();
                        cf_head << "void " << filter->getEntryPointFunctionName() << "(inout vec4 color);\n";
                        cf_body << I << I << filter->getEntryPointFunctionName() << "(color);\n";
                        filter->install(_surfaceSS.get());
                    }
                    cf_body << I << "}\n";
                    ifStarted = true;
                }
            }
        }

        if (haveColorFilters)
        {
            std::string cf_head_str, cf_body_str;
            cf_head_str = cf_head.str();
            cf_body_str = cf_body.str();

            replaceIn(fs_colorfilters, "$COLOR_FILTER_HEAD", cf_head_str);
            replaceIn(fs_colorfilters, "$COLOR_FILTER_BODY", cf_body_str);

            surfaceVP->setFunction(
                "oe_rexEngine_applyFilters",
                fs_colorfilters,
                VirtualProgram::LOCATION_FRAGMENT_COLORING,
                0.6);
        }
    }
}

osg::Node*
RexTerrainEngineNode::createStandaloneTile(
    const TerrainTileModel* model,
    int createTileFlags,
    unsigned referenceLOD,
    const TileKey& subRegion)
{
    CreateTileImplementation impl;
    return impl.createTile(getEngineContext(), model, createTileFlags, referenceLOD, subRegion);
}
