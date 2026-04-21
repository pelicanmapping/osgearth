/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "Decals"
#include "CullingUtils"
#include "Shaders"

#include <osg/ComputeBoundsVisitor>
#include <osg/ShapeDrawable>
#include <osg/PolygonMode>

#define TILES_PER_THREAD_GROUP 16

using namespace osgEarth;


void
DecalNode::traverse(osg::NodeVisitor& nv)
{
    if (!_decals.empty())
    {
        if (nv.getVisitorType() == nv.CULL_VISITOR)
        {
            osgUtil::CullVisitor* cv = Culling::asCullVisitor(nv);
            if (cv->getCurrentRenderBin()->getName() != "OE_EMPTY_RENDER_BIN")
            {
                // SSE culling:
                if (minPixels().isSet())
                {
                    float sse = 0.0f;
                    if (nv.getUserValue("oe_sse", sse))
                    {
                        auto pixels = cv->clampedPixelSize(getBound()) / cv->getLODScale();
                        if (pixels < minPixels().value() * sse)
                            return;
                    }
                }

                std::shared_ptr<detail::DecalDrawList> drawList;
                if (ObjectStorage::get(&nv, drawList))
                {
                    auto& leaves = drawList->_perCamera.get(cv->getState()).leaves;
                    auto invview = cv->getCurrentCamera()->getInverseViewMatrix();
                    auto& mvm = *cv->getModelViewMatrix();
                    osg::Matrix localToWorld = mvm * invview;
                    for (auto& decal : _decals)
                    {
                        Decal leaf = decal;
                        leaf.matrix = localToWorld * decal.matrix;
                        leaves.emplace_back(std::move(leaf));
                    }
                }

                if (_debug.valid())
                {
                    _debug->accept(nv);
                }
            }
        }
    }
    osg::Node::traverse(nv);
}

void
DecalNode::debug(bool value)
{
    _debug = nullptr;
    if (value)
    {
        _debug = new osg::Group();

        for (auto& decal : _decals) {
            auto mt = new osg::MatrixTransform(decal.matrix);
            mt->addChild(new osg::ShapeDrawable(new osg::Box(osg::Vec3(0, 0, 0), decal.size.x(), decal.size.y(), decal.size.z())));
            _debug->addChild(mt);
        }
        _debug->getOrCreateStateSet()->setAttribute(new osg::PolygonMode(
            osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::LINE));
    }
}

osg::BoundingSphere
DecalNode::computeBound() const
{
    osg::BoundingSphere bs;
    for (const auto& decal : _decals) 
    {
        bs.expandBy(osg::BoundingSphere(
            osg::Vec3(0, 0, 0) * decal.matrix,
            std::max(decal.size.x(), decal.size.y()) * 0.7071f));
    }
    return bs;
}



void
DecalRTTNode::traverse(osg::NodeVisitor& nv)
{
    if (getNumChildren() > 0)
    {
        if (nv.getVisitorType() == nv.CULL_VISITOR)
        {
            osgUtil::CullVisitor* cv = Culling::asCullVisitor(nv);
            if (cv->getCurrentRenderBin()->getName() != "OE_EMPTY_RENDER_BIN")
            {
                if (_needsRTT)
                {
                    std::lock_guard<std::mutex> lock(_rttMutex);

                    if (!_rtt.valid())
                    {
                        _tex = new osg::Texture2D();
                        _tex->setTextureSize(_texWidth, _texHeight);
                        _tex->setSourceFormat(GL_RGBA);
                        _tex->setInternalFormat(GL_RGBA8);
                        _tex->setSourceType(GL_UNSIGNED_BYTE);

                        // create RTT camera
                        _rtt = new osg::Camera();
                        _rtt->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
                        _rtt->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
                        _rtt->setRenderOrder(osg::Camera::POST_RENDER);
                        _rtt->setClearColor(osg::Vec4(0, 0, 0, 0));
                        _rtt->setClearDepth(1.0f);
                        _rtt->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                        _rtt->setViewport(0, 0, _texWidth, _texHeight);
                        _rtt->setViewMatrix(osg::Matrix::identity());
                        _rtt->setSmallFeatureCullingPixelSize(0.0f);
                        _rtt->attach(osg::Camera::COLOR_BUFFER0, _tex.get());   
                    }

                    if (_rtt->getNumChildren() == 0)
                    {
                        for (unsigned i = 0; i < getNumChildren(); ++i)
                            _rtt->addChild(getChild(i));
                    }

                    osg::ComputeBoundsVisitor cbv;
                    _rtt->accept(cbv);
                    auto& bbox = cbv.getBoundingBox();

                    _rtt->setProjectionMatrixAsOrtho2D(bbox.xMin(), bbox.xMax(), bbox.yMin(), bbox.yMax());

                    _rtt->accept(nv);

                    if (_autoSize)
                    {
                        _decal.size.x() = bbox.xMax() - bbox.xMin();
                        _decal.size.y() = bbox.yMax() - bbox.yMin();
                        _decal.textureSize->set(_decal.size.x(), _decal.size.y());
                    }

                    _needsRTT = getDynamic();

                    this->dirtyBound();
                }

                // wait until the texture gets compiled, and then wrap it to make it bindless
                if (_decal.texture == nullptr && _tex->areAllTextureObjectsLoaded())
                {
                    _decal.texture = Texture::create(_tex.get());

                    if (!getDynamic())
                        _rtt = nullptr; // release the RTT camera
                }

                std::shared_ptr<detail::DecalDrawList> drawList;
                if (ObjectStorage::get(&nv, drawList))
                {
                    auto& leaves = drawList->_perCamera.get(cv->getState()).leaves;
                    const auto& mm = *cv->getModelViewMatrix() * cv->getCurrentCamera()->getInverseViewMatrix();
                    Decal leaf = _decal;
                    leaf.matrix = mm * _decal.matrix;
                    leaves.emplace_back(std::move(leaf));
                }
            }
        }
    }
    osg::Node::traverse(nv); // correct; do NOT call osg::Group::traverse!
}


void
DecalRTTNode::dirty()
{
    OE_SOFT_ASSERT_AND_RETURN(getDynamic(), void(), "DecalRTTNode::dirty() called on non-dynamic node");

    std::lock_guard<std::mutex> lock(_rttMutex);
    
    if (_rtt.valid())
    {
        _rtt->removeChildren(0, _rtt->getNumChildren());
    }
    _needsRTT = true;
}


osg::BoundingSphere
DecalRTTNode::computeBound() const
{
    _needsRTT = true;
    osg::BoundingSphere bs;
    bs.expandBy(osg::BoundingSphere(
        osg::Vec3(0, 0, 0) * _decal.matrix,
        std::max(_decal.size.x(), _decal.size.y()) * 0.7071f));
    return bs;
}


DecalGroup::DecalGroup(DecalDecorator* decorator)
{
    OE_SOFT_ASSERT(decorator);
    if (decorator)
        _drawList = decorator->_drawList;
}




DecalDecorator*
DecalDecorator::getOrCreate(osg::Group* target)
{
    OE_SOFT_ASSERT_AND_RETURN(target, nullptr);

    // is one already there?
    for (size_t i = 0; i < target->getNumChildren(); ++i)
    {
        auto dec = dynamic_cast<DecalDecorator*>(target->getChild(i));
        if (dec)
            return dec;
    }

    // add our decorator as a child of the target
    auto dec = new DecalDecorator();
    target->addChild(dec);

    // add the applier to the target's stateset
    auto applier = new DecalApplier();
    applier->setDecorator(dec);
    auto targetSS = target->getOrCreateStateSet();
    targetSS->setAttribute(applier);

    // load up the decal application shaders on the target:
    auto* vp = VirtualProgram::getOrCreate(targetSS);
    Shaders package;
    dec->setBindings(package).load(vp, package.Decals);

    return dec;
}

void
DecalDecorator::remove(osg::Group* target)
{
    OE_SOFT_ASSERT_AND_RETURN(target, void());

    for (size_t i = 0; i < target->getNumChildren(); ++i)
    {
        auto dec = dynamic_cast<DecalDecorator*>(target->getChild(i));
        if (dec)
        {
            auto ss = target->getStateSet();
            if (ss)
                ss->removeAttribute(OE_DECAL_APPLIER_ATTR_TYPE);

            target->getChild(i)->releaseGLObjects(nullptr);
            target->removeChild(i);
            return;
        }
    }
}

DecalDecorator::DecalDecorator()
{
    // set up the pre-render camera to force our decal culling pass
    // to happen before any main scene rendering
    this->setCullingActive(false);
    this->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
    this->setRenderOrder(osg::Camera::PRE_RENDER);
    this->setFinalDrawCallback(new CameraCallback(this));
    this->setImplicitBufferAttachmentMask(0, 0);

    // set up our compute shaders
    Shaders shaders;

    _computeFrustumsProgram = new osg::Program();
    _computeFrustumsProgram->addShader(new osg::Shader(osg::Shader::COMPUTE,
        ShaderLoader::load(shaders.FrustumGridComputer, setBindings(shaders))));

    _cullProgram = new osg::Program();
    _cullProgram->addShader(new osg::Shader(osg::Shader::COMPUTE,
        ShaderLoader::load(shaders.DecalsCulling, setBindings(shaders))));

    _drawList = std::make_shared<detail::DecalDrawList>();
    _textures = new TextureArena();
    _textures->setBindingPoint(_texturesBinding);
}

Util::ShaderPackage&
DecalDecorator::setBindings(Util::ShaderPackage& package) const
{
    package.replace("OE_BINDING_FRUSTUM_PARAMS", std::to_string(_paramsBinding));
    package.replace("OE_BINDING_FRUSTUMS", std::to_string(_frustumsBinding));
    package.replace("OE_BINDING_DECAL_TILES", std::to_string(_tilesBinding));
    package.replace("OE_BINDING_DECALS", std::to_string(_decalsBinding));
    package.replace("OE_BINDING_DECAL_TEXTURES", std::to_string(_texturesBinding));
    return package;
}

void
DecalDecorator::dirtyUniforms()
{
    for (unsigned i = 0; i < _globjects.size(); ++i)
    {
        _globjects[i].dirty = true;
    }
}

void
DecalDecorator::accept(osg::NodeVisitor& nv)
{
    if (nv.getVisitorType() == nv.CULL_VISITOR)
    {
        auto* cv = dynamic_cast<osgUtil::CullVisitor*>(&nv);
        if (cv)
        {
            auto& gc = GLObjects::get(_globjects, *cv->getState());
            gc.camera = cv->getCurrentCamera();
            gc.mvm = *cv->getModelViewMatrix();
        }
    }
    osg::Camera::accept(nv);
}

void
DecalDecorator::operator()(osg::RenderInfo& ri) const
{
    auto& state = *ri.getState();
    auto& leaves = _drawList->_perCamera.get(&state).leaves;

    auto& gc = GLObjects::get(_globjects, state);
    if (!gc.decalsBuffer)
    {
        // SSBO holding ALL known decal instances
        gc.decalsBuffer = GLBuffer::create_shared(GL_SHADER_STORAGE_BUFFER, state);
        gc.decalsBuffer->bind();
        gc.decalsBuffer->debugLabel("Decals", "Decal instance buffer");
        gc.decalsBuffer->unbind();

        // UBO holding the paramater uniforms
        gc.paramsBuffer = GLBuffer::create(GL_UNIFORM_BUFFER, state);
        gc.paramsBuffer->bind();
        gc.paramsBuffer->debugLabel("Decals", "Decal parameters buffer");
        gc.paramsBuffer->unbind();

        // SSBO holding the tiled frustums (GPU-side only)
        gc.frustumsBuffer = GLBuffer::create(GL_SHADER_STORAGE_BUFFER, state);
        gc.frustumsBuffer->bind();
        gc.frustumsBuffer->debugLabel("Decals", "Decal frustums buffer");
        gc.frustumsBuffer->unbind();

        // SSBO holding the tile decal render lists (GPU-side only)
        gc.tilesBuffer = GLBuffer::create(GL_SHADER_STORAGE_BUFFER, state);
        gc.tilesBuffer->bind();
        gc.tilesBuffer->debugLabel("Decals", "Decal tiles buffer");
        gc.tilesBuffer->unbind();
    }

    if (leaves.empty() && gc.decals.size() == 1) // there's always at least 1
        return;

    // compute a new set of frustums for the current view if neccesary
    bool newGrid = computeFrustumGrid(state, gc);

    gc.decals.resize(1);
    gc.decals[0].count = (std::uint32_t)(leaves.size());

    // update the buffer
    for (auto& leaf : leaves)
    {
        gc.decals.emplace_back();
        GPUDecal& instance = gc.decals.back();
        instance.mvm = leaf.matrix * gc.mvm;
        instance.mvmInverse = osg::Matrix::inverse(instance.mvm);
        instance.halfX = 0.5f * leaf.size.x();
        instance.halfY = 0.5f * leaf.size.y();
        instance.halfZ = 0.5f * leaf.size.z();
        instance.textureIndex = leaf.texture ? (std::int32_t)_textures->add(leaf.texture) : -1;
        instance.opacity = leaf.opacity;
    }

    // send it to the GPU
    gc.decalsBuffer->uploadData(gc.decals);

    // if we generated a new frustum tile grid, sync the output for the culling shader
    if (newGrid)
    {
        gc.ext()->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }

    // run the culling shader
    cull(state, gc);

    // reset the draw list. note: this is not multi-GC friendly yet.
    leaves.clear();
}

bool
DecalDecorator::computeFrustumGrid(osg::State& state, GLObjects& gc) const
{
    osg::Vec4i viewport(
        gc.camera->getViewport()->x(),
        gc.camera->getViewport()->y(),
        gc.camera->getViewport()->width(),
        gc.camera->getViewport()->height());

    if (gc.dirty || viewport != gc.params.viewport)
    {
        gc.params.invProjMatrix = osg::Matrix::inverse(gc.camera->getProjectionMatrix());
        gc.params.viewport = viewport;
        gc.params.numTiles.x() = (int)((viewport[2] + _pixelsPerTile - 1) / _pixelsPerTile);
        gc.params.numTiles.y() = (int)((viewport[3] + _pixelsPerTile - 1) / _pixelsPerTile);
        gc.params.debugTiles = _debugTiles ? 1.0f : 0.0f;

        // upload
        gc.paramsBuffer->uploadData(sizeof(GPUParams), &gc.params);
        _uniformsDirty = false;

        // ensure enough storage space for the frustums grid:
        gc.frustumsBuffer->bind();
        gc.frustumsBuffer->bufferData(
            sizeof(GPUFrustum) * gc.params.numTiles.x() * gc.params.numTiles.y(),
            nullptr,
            GL_DYNAMIC_DRAW);

        // ensure enough storage space for the tiles index:
        gc.tilesBuffer->bind();
        gc.tilesBuffer->bufferData(
            sizeof(GPUTile) * gc.params.numTiles.x() * gc.params.numTiles.y(),
            nullptr,
            GL_DYNAMIC_DRAW);

        // activate the program
        _computeFrustumsProgram->apply(state);
        OE_HARD_ASSERT(state.getLastAppliedProgramObject(), "Shader compilation error!!");

        // assign binding points for the buffers we will use in this shader:
        gc.paramsBuffer->bindBufferBase(_paramsBinding);
        gc.frustumsBuffer->bindBufferBase(_frustumsBinding);

        // run it!
        GLuint numGroupsX = (gc.params.numTiles.x() + TILES_PER_THREAD_GROUP - 1) / TILES_PER_THREAD_GROUP;
        GLuint numGroupsY = (gc.params.numTiles.y() + TILES_PER_THREAD_GROUP - 1) / TILES_PER_THREAD_GROUP;
        
        gc.ext()->glDispatchCompute(numGroupsX, numGroupsY, 1);

        gc.dirty = false;

        // grid changed:
        return true;
    }

    // grid did not change:
    return false;
}

void
DecalDecorator::cull(osg::State& state, GLObjects& gc) const
{
    _cullProgram->apply(state);
    OE_HARD_ASSERT(state.getLastAppliedProgramObject(), "Shader compilation error!!");

    // bind all buffers in the shader:
    gc.decalsBuffer->bindBufferBase(_decalsBinding);
    gc.paramsBuffer->bindBufferBase(_paramsBinding);
    gc.frustumsBuffer->bindBufferBase(_frustumsBinding);
    gc.tilesBuffer->bindBufferBase(_tilesBinding);

    GLuint numGroupsX = (gc.params.numTiles.x() + TILES_PER_THREAD_GROUP - 1) / TILES_PER_THREAD_GROUP;
    GLuint numGroupsY = (gc.params.numTiles.y() + TILES_PER_THREAD_GROUP - 1) / TILES_PER_THREAD_GROUP;

    gc.ext()->glDispatchCompute(numGroupsX, numGroupsY, 1);
}

void
DecalDecorator::applyRenderingState(osg::State& state) const
{
    auto& gc = GLObjects::get(_globjects, state);

    gc.decalsBuffer->bindBufferBase(_decalsBinding);
    gc.paramsBuffer->bindBufferBase(_paramsBinding);
    gc.tilesBuffer->bindBufferBase(_tilesBinding);

    // ensure the deal data generated by the culling shader is ready:
    gc.ext()->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    _textures->apply(state);

    // ready to render!
}

void
DecalDecorator::releaseGLObjects(osg::State* state) const
{
    if (state)
    {
        auto& gc = GLObjects::get(_globjects, *state);
        gc.decalsBuffer = nullptr;
        gc.paramsBuffer = nullptr;
        gc.frustumsBuffer = nullptr;
        gc.tilesBuffer = nullptr;
    }
    else
    {
        _globjects.clear();
    }
}


void
DecalApplier::apply(osg::State& state) const
{
    if (_decorator)
    {
        _decorator->applyRenderingState(state);
    }
}
