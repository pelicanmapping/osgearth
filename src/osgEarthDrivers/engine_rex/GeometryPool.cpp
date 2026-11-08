/* osgEarth
* Copyright 2008-2014 Pelican Mapping
* MIT License
*/
#include "GeometryPool"
#include <osgEarth/Metrics>
#include <osgEarth/NodeUtils>
#include <osgEarth/TerrainMeshLayer>
#include <osgEarth/TerrainConstraintLayer>
#include <osgEarth/Registry>
#include <cstdlib> // for getenv

using namespace osgEarth;
using namespace osgEarth::REX;

#define LC "[GeometryPool] "


GeometryPool::GeometryPool() :
    _releaser(Registry::instance()->getObjectReleaser()->createClient("REX geometry pool")),
    _kdTreeReleaser(Registry::instance()->getObjectReleaser()->createClient("REX replaced KdTrees")),
    _kdTreePool(std::make_shared<KdTreePool>(_kdTreeReleaser)),
    _childReleaser(Registry::instance()->getObjectReleaser()->createClient("REX stale child preparation")),
    _enabled(true),
    _debug(false)
{
    ADJUST_UPDATE_TRAV_COUNT(this, +1);

    // activate debugging mode
    if ( getenv("OSGEARTH_DEBUG_REX_GEOMETRY_POOL") != 0L )
    {
        _debug = true;
    }

    if ( ::getenv("OSGEARTH_REX_NO_POOL") )
    {
        _enabled = false;
        OE_INFO << LC << "Geometry pool disabled (environment)" << std::endl;
    }
}

GeometryPool::~GeometryPool()
{
    _releaser.drain();
    _childReleaser.drain();
    _kdTreeReleaser.drain();
}

void
GeometryPool::getPooledGeometry(
    const TileKey& tileKey,
    unsigned tileSize,
    const Map* map,
    const TerrainOptionsAPI& options,
    osg::ref_ptr<SharedGeometry>& out,
    Cancelable* cancelable)
{
    TileMesher mesher;
    mesher.setTerrainOptions(options);

    // make our globally shared EBO if we need it
    {
        std::lock_guard<std::mutex> lock(_geometryMapMutex);
        if (!_defaultPrimSet.valid())
        {
            // convert the mesher's indices to a SharedDrawElements
            auto indices = mesher.getOrCreateStandardIndices();

            GLenum mode = options.getGPUTessellation() == true ? GL_PATCHES : GL_TRIANGLES;

            _defaultPrimSet = new SharedDrawElements(mode);

            for (unsigned i = 0; i < indices->getNumIndices(); ++i)
                _defaultPrimSet->addElement(indices->getElement(i));

            _defaultPrimSet->setElementBufferObject(new osg::ElementBufferObject());
        }
    }

    osg::ref_ptr<ProgressCallback> progress = new ProgressCallback(cancelable);

    // First check the map for a terrain mesh layer. If one exists
    // simply pull the final tile mesh from there.
    // TODO: support adding additional constraints to the mesh layer
    // result? Or should the mesh layer itself worry aboug that?
    if (map)
    {
        auto meshlayer = map->getLayer<TerrainMeshLayer>();
        if (meshlayer)
        {
            auto mesh = meshlayer->createTile(tileKey, progress);
            out = convertTileMeshToSharedGeometry(mesh);
            if (out.valid())
            {
                // done!
                return;
            }
        }
    }

    // convert to a unique-geometry key:
    GeometryKey geomKey;
    createKeyForTileKey(tileKey, tileSize, geomKey);

    // see if there are any constraints:
    TerrainConstraintQuery query(map);
    MeshConstraints edits;
    query.getConstraints(tileKey, edits, progress);

    if ( _enabled )
    {
        // Protect access on a per key basis to prevent the same key from being created twice.  
        // This was causing crashes with multiple windows opening and closing.
        ScopedGate<GeometryKey> gatelock(_keygate, geomKey);

        // first check the sharing cache (note: tiles with edits are not cached)
        if (edits.empty())
        {
            std::lock_guard<std::mutex> lock(_geometryMapMutex);
            GeometryMap::iterator i = _geometryMap.find(geomKey);
            if (i != _geometryMap.end())
            {
                // found it:
                out = i->second.get();
            }
        }

        if (!out.valid())
        {
            auto mesh = mesher.createMesh(tileKey, edits, progress);
            out = convertTileMeshToSharedGeometry(mesh);

            // only store as a shared geometry if there are no constraints.
            if (out.valid() && !out->hasConstraints())
            {
                std::lock_guard<std::mutex> lock(_geometryMapMutex);
                _geometryMap[geomKey] = out.get();
            }
        }
    }

    else
    {
        auto mesh = mesher.createMesh(tileKey, edits, progress);
        out = convertTileMeshToSharedGeometry(mesh);
    }
}

void
GeometryPool::createKeyForTileKey(const TileKey& tileKey,
                                  unsigned tileSize,
                                  GeometryKey& out) const
{
    out.lod  = tileKey.getLOD();
    out.tileY = tileKey.getProfile()->getSRS()->isGeographic()? tileKey.getTileY() : 0;
    out.size = tileSize;
}

osg::ref_ptr<SharedGeometry>
GeometryPool::convertTileMeshToSharedGeometry(const TileMesh& mesh) const
{
    if (!mesh.verts.valid())
        return nullptr;

    osg::ref_ptr<SharedGeometry> shared = new SharedGeometry();

    shared->setVertexArray(mesh.verts);
    shared->setNormalArray(mesh.normals);
    shared->setTexCoordArray(mesh.uvs);
    shared->setNeighborArray(mesh.vert_neighbors);
    shared->setNeighborNormalArray(mesh.normal_neighbors);

    if (mesh.indices.valid())
    {
        auto de = new SharedDrawElements(mesh.indices->getMode());
        de->setElementBufferObject(new osg::ElementBufferObject());
        de->reserveElements(mesh.indices->getNumIndices());
        for (auto i = 0u; i < mesh.indices->getNumIndices(); ++i)
            de->addElement(mesh.indices->getElement(i));
        shared->setDrawElements(de);
    }
    else
    {
        OE_HARD_ASSERT(_defaultPrimSet.valid());
        shared->setDrawElements(_defaultPrimSet);
    }

    shared->setHasConstraints(mesh.hasConstraints);

    // if we are using GL4, create the GL4 tile model.
    if (/*using GL4*/true)
    {
        unsigned size = shared->getVertexArray()->size();

        shared->_verts.reserve(size);

        for (unsigned i = 0; i < size; ++i)
        {
            GL4Vertex v;

            v.position = (*shared->getVertexArray())[i];
            v.normal = (*shared->getNormalArray())[i];
            v.uv = (*shared->getTexCoordArray())[i];

            if (shared->getNeighborArray())
                v.neighborPosition = (*shared->getNeighborArray())[i];

            if (shared->getNeighborNormalArray())
                v.neighborNormal = (*shared->getNeighborNormalArray())[i];

            shared->_verts.emplace_back(std::move(v));
        }
    }

    return shared;
}

void
GeometryPool::traverse(osg::NodeVisitor& nv)
{
    if (nv.getVisitorType() == nv.UPDATE_VISITOR && _enabled && _releaser.available())
    {
        ObjectReleaser::Objects retired;
        {
            std::lock_guard<std::mutex> lock(_geometryMapMutex);
            for (auto i = _geometryMap.begin(); i != _geometryMap.end();)
            {
                if (i->second->referenceCount() == 1)
                {
                    // Retain before erasing: SharedGeometry's CPU arrays must not die in the frame traversal.
                    retired.emplace_back(i->second.get());
                    i = _geometryMap.erase(i);
                }
                else
                {
                    ++i;
                }
            }
        }
        // Submission can release synchronously during shutdown; never hold the pool lock here.
        _releaser.release(retired);
    }

    osg::Group::traverse(nv);
}

void
GeometryPool::clear()
{
    releaseGLObjects(nullptr);
    ObjectReleaser::Objects retired;
    {
        std::lock_guard<std::mutex> lock(_geometryMapMutex);
        retired.reserve(_geometryMap.size());
        for (auto& entry : _geometryMap)
            retired.emplace_back(entry.second.get());
        _geometryMap.clear();
    }
    _releaser.release(retired);
}

void
GeometryPool::resizeGLObjectBuffers(unsigned maxsize)
{
    if (!_enabled)
        return;

    // collect all objects in a thread safe manner
    std::lock_guard<std::mutex> lock(_geometryMapMutex);

    for (GeometryMap::const_iterator i = _geometryMap.begin(); i != _geometryMap.end(); ++i)
    {
        i->second->resizeGLObjectBuffers(maxsize);
    }

    // the shared primitive set
    if (_defaultPrimSet.valid())
    {
        _defaultPrimSet->resizeGLObjectBuffers(maxsize);
    }
}

void
GeometryPool::releaseGLObjects(osg::State* state) const
{
    if (!_enabled)
        return;

    // collect all objects in a thread safe manner
    std::lock_guard<std::mutex> lock(_geometryMapMutex);

    for (auto& entry : _geometryMap)
    {
        entry.second->releaseGLObjects(state);
    }

    // the shared primitive set
    if (_defaultPrimSet.valid())
    {
        _defaultPrimSet->releaseGLObjects(state);
    }
}

//.........................................................................
// Code mostly adapted from osgTerrain SharedGeometry.

SharedGeometry::SharedGeometry() :
    osg::Drawable(),
    _hasConstraints(false)
{
    _supportsVertexBufferObjects = true;
    _ptype.resize(64u);
    _ptype.setAllElementsTo(GL_TRIANGLES);
    _hasConstraints = false;
    setSupportsDisplayList(false);
    setUseDisplayList(false);
    setUseVertexBufferObjects(true);
}

SharedGeometry::SharedGeometry(const SharedGeometry& rhs,const osg::CopyOp& copyop):
    osg::Drawable(rhs, copyop),
    _vertexArray(rhs._vertexArray),
    _normalArray(rhs._normalArray),
    _texcoordArray(rhs._texcoordArray),
    _neighborArray(rhs._neighborArray),
    _neighborNormalArray(rhs._neighborNormalArray),
    _drawElements(rhs._drawElements),
    _hasConstraints(rhs._hasConstraints)
{
    _ptype.resize(64u);
    _ptype.setAllElementsTo(GL_TRIANGLES);
}

SharedGeometry::~SharedGeometry()
{
    releaseGLObjects(nullptr);
}

const DrawElementsIndirectBindlessCommandNV&
SharedGeometry::getOrCreateNVGLCommand(osg::State& state)
{
    OE_SOFT_ASSERT(_verts.size() > 0);
    OE_SOFT_ASSERT(_drawElements->size() > 0);

    bool dirty = false;

    // first the drawelements
    auto& de = SharedDrawElements::GLObjects::get(_drawElements->_globjects, state);

    if (de._ebo == nullptr || !de._ebo->valid())
    {
        de._ebo = GLBuffer::create_shared(GL_ELEMENT_ARRAY_BUFFER_ARB, state);
        de._ebo->bind();
        de._ebo->debugLabel("Terrain geometry", "Shared EBO");
        de._ebo->bufferStorage(_drawElements->getTotalDataSize(), _drawElements->getDataPointer(), 0);
        de._ebo->unbind();

        dirty = true;
    }

    GLObjects& gs = GLObjects::get(_globjects, state);

    if (gs._vbo == nullptr || !gs._vbo->valid())
    {
        // supply a "size hint" for unconstrained tiles to the GLBuffer so it can try to re-use
        GLsizei size = _verts.size() * sizeof(GL4Vertex);
        if (_hasConstraints)
            gs._vbo = GLBuffer::create_shared(GL_ARRAY_BUFFER_ARB, state);
        else
            gs._vbo = GLBuffer::create_shared(GL_ARRAY_BUFFER_ARB, state, size);

        gs._vbo->bind();
        gs._vbo->debugLabel("Terrain geometry", "Shared VBO");
        gs._vbo->bufferStorage(size, _verts.data());
        gs._vbo->unbind();

        dirty = true;
    }

    // make them resident in each context separately
    de._ebo->makeResident(state);
    gs._vbo->makeResident(state);

    OE_SOFT_ASSERT(de._ebo->address() != 0);
    OE_SOFT_ASSERT(de._ebo->size() > 0);

    OE_SOFT_ASSERT(gs._vbo->address() != 0);
    OE_SOFT_ASSERT(gs._vbo->size() > 0);

    if (dirty)
    {
        gs._command.cmd.count = _drawElements->size();
        gs._command.cmd.instanceCount = 1;
        gs._command.cmd.firstIndex = 0;
        gs._command.cmd.baseVertex = 0;
        gs._command.cmd.baseInstance = 0;

        gs._command.reserved = 0;

        gs._command.indexBuffer.index = 0;
        gs._command.indexBuffer.reserved = 0;
        gs._command.indexBuffer.address = de._ebo->address();
        gs._command.indexBuffer.length = de._ebo->size();

        gs._command.vertexBuffer.index = 0;
        gs._command.vertexBuffer.reserved = 0;
        gs._command.vertexBuffer.address = gs._vbo->address();
        gs._command.vertexBuffer.length = gs._vbo->size();
    }

    return gs._command;
}

bool
SharedGeometry::empty() const
{
    return
        (_drawElements.valid() == false || _drawElements->getNumIndices() == 0);
}

#if OSG_MIN_VERSION_REQUIRED(3,5,9)
osg::VertexArrayState* SharedGeometry::createVertexArrayStateImplementation(osg::RenderInfo& renderInfo) const
#else
osg::VertexArrayState* SharedGeometry::createVertexArrayState(osg::RenderInfo& renderInfo) const
#endif
{
    osg::State& state = *renderInfo.getState();

    osg::VertexArrayState* vas = new osg::VertexArrayState(&state);

    if (_vertexArray.valid()) vas->assignVertexArrayDispatcher();
    if (_normalArray.valid()) vas->assignNormalArrayDispatcher();
    unsigned texUnits = 0;
    if (_neighborArray.valid())
    {
        texUnits = 3;
    }
    else if (_texcoordArray.valid())
    {
        texUnits = 1;
    }
    if (texUnits)
        vas->assignTexCoordArrayDispatcher(texUnits);

    if (state.useVertexArrayObject(_useVertexArrayObject))
    {
        vas->generateVertexArrayObject();
    }

    return vas;
}

void SharedGeometry::resizeGLObjectBuffers(unsigned int maxSize)
{
    osg::Drawable::resizeGLObjectBuffers(maxSize);

    if (_vertexArray.valid()) _vertexArray->resizeGLObjectBuffers(maxSize);
    if (_normalArray.valid()) _normalArray->resizeGLObjectBuffers(maxSize);
    if (_colorArray.valid()) _colorArray->resizeGLObjectBuffers(maxSize);
    if (_texcoordArray.valid()) _texcoordArray->resizeGLObjectBuffers(maxSize);
    if (_neighborArray.valid()) _neighborArray->resizeGLObjectBuffers(maxSize);
    if (_neighborNormalArray.valid()) _neighborNormalArray->resizeGLObjectBuffers(maxSize);

    if (maxSize > _globjects.size())
        _globjects.resize(maxSize);

    if (_drawElements.valid())
        _drawElements->resizeGLObjectBuffers(maxSize);
}

void SharedGeometry::releaseGLObjects(osg::State* state) const
{
    osg::Drawable::releaseGLObjects(state);

    if (_vertexArray.valid()) _vertexArray->releaseGLObjects(state);
    if (_normalArray.valid()) _normalArray->releaseGLObjects(state);
    if (_colorArray.valid()) _colorArray->releaseGLObjects(state);
    if (_texcoordArray.valid()) _texcoordArray->releaseGLObjects(state);
    if (_neighborArray.valid()) _neighborArray->releaseGLObjects(state);
    if (_neighborNormalArray.valid()) _neighborNormalArray->releaseGLObjects(state);

    if (state)
    {
        auto& gl = GLObjects::get(_globjects, *state);
        gl._vbo = nullptr;
    }

    // Do nothing if state is nullptr!
    // Let nature take its course and let the GLObjectPool deal with it
}

void
SharedGeometry::drawVertexArraysImplementation(osg::RenderInfo& renderInfo) const
{
    osg::State& state = *renderInfo.getState();
    osg::VertexArrayState* vas = state.getCurrentVertexArrayState();

    osg::AttributeDispatchers& attributeDispatchers = state.getAttributeDispatchers();

    attributeDispatchers.reset();
    attributeDispatchers.setUseVertexAttribAlias(state.getUseVertexAttributeAliasing());

    // activate or dispatch any attributes that are bound overall
    attributeDispatchers.activateNormalArray(_normalArray.get());
    attributeDispatchers.activateColorArray(_colorArray.get());

    // if we are using VAOs, but the VAO is already recorded, bail out here.
    bool usingVAOs = state.useVertexArrayObject(_useVertexArrayObject);

    // if the VAO has already been recorded, we're done.
    if (usingVAOs && vas->getRequiresSetArrays() == false)
    {
        return;
    }


    vas->lazyDisablingOfVertexAttributes();

    // set up arrays
    if (_vertexArray.valid())
        vas->setVertexArray(state, _vertexArray.get());

    if (_normalArray.valid() && _normalArray->getBinding() == osg::Array::BIND_PER_VERTEX)
        vas->setNormalArray(state, _normalArray.get());

    if (_colorArray.valid() && _colorArray->getBinding() == osg::Array::BIND_PER_VERTEX)
        vas->setColorArray(state, _colorArray.get());

    if (_texcoordArray.valid() && _texcoordArray->getBinding() == osg::Array::BIND_PER_VERTEX)
        vas->setTexCoordArray(state, 0, _texcoordArray.get());

    if (_neighborArray.valid() && _neighborArray->getBinding() == osg::Array::BIND_PER_VERTEX)
        vas->setTexCoordArray(state, 1, _neighborArray.get());

    if (_neighborNormalArray.valid() && _neighborNormalArray->getBinding() == osg::Array::BIND_PER_VERTEX)
        vas->setTexCoordArray(state, 2, _neighborNormalArray.get());

    vas->applyDisablingOfVertexAttributes(state);

    // if we're using a VAO, bind the EBO to the VAO here.
    if (usingVAOs)
    {
        osg::GLBufferObject* ebo = _drawElements->getOrCreateGLBufferObject(GLUtils::getSharedContextID(state));
        if (ebo)
        {
            state.bindElementBufferObject(ebo);
        }
    }
}


void
SharedGeometry::drawPrimitivesImplementation(osg::RenderInfo& renderInfo) const
{
    OE_SOFT_ASSERT_AND_RETURN(_drawElements.valid(), void(), "null drawelements");

    if (_drawElements->getNumIndices() == 0u)
        return;

    osg::State& state = *renderInfo.getState();
    GLenum primitiveType = _ptype[GLUtils::getSharedContextID(state)];

    const void* indices;
    bool usingVAO = state.useVertexArrayObject(_useVertexArrayObject);
    osg::GLBufferObject* ebo = _drawElements->getOrCreateGLBufferObject(GLUtils::getSharedContextID(state));

    if (usingVAO)
    {
        // for a VAO, the EBO is already bound, and the indices is
        // an offset into the GPU buffer.
        indices = (const GLvoid*)ebo->getOffset(_drawElements->getBufferIndex());
    }
    else if (ebo)
    {
        // for non-VAO, we must bind the EBO each time.
        state.bindElementBufferObject(ebo);
        indices = (const GLvoid*)ebo->getOffset(_drawElements->getBufferIndex());
    }
    else
    {
        // for legacy GL, use the actual CPU memory address
        indices = _drawElements->getDataPointer();
    }

    glDrawElements(
        primitiveType,
        _drawElements->getNumIndices(),
        _drawElements->getDataType(),
        indices);
}

void SharedGeometry::drawImplementation(osg::RenderInfo& renderInfo) const
{
    osg::State& state = *renderInfo.getState();

    bool usingVertexBufferObjects = state.useVertexBufferObject(_supportsVertexBufferObjects && _useVertexBufferObjects);
    bool usingVertexArrayObjects = usingVertexBufferObjects && state.useVertexArrayObject(_useVertexArrayObject);

    osg::VertexArrayState* vas = state.getCurrentVertexArrayState();
    vas->setVertexBufferObjectSupported(usingVertexBufferObjects);

    bool checkForGLErrors = state.getCheckForGLErrors() == osg::State::ONCE_PER_ATTRIBUTE;
    if (checkForGLErrors) state.checkGLErrors("start of SharedGeometry::drawImplementation()");

    drawVertexArraysImplementation(renderInfo);

    drawPrimitivesImplementation(renderInfo);

    if (usingVertexBufferObjects && !usingVertexArrayObjects)
    {
        // unbind the VBO's if any are used.
        vas->unbindVertexBufferObject();
        vas->unbindElementBufferObject();
    }
}

void SharedGeometry::accept(osg::Drawable::AttributeFunctor& af)
{
    osg::AttributeFunctorArrayVisitor afav(af);

    afav.applyArray(VERTICES,_vertexArray.get());
    afav.applyArray(NORMALS, _normalArray.get());
    afav.applyArray(TEXTURE_COORDS_0,_texcoordArray.get());
    afav.applyArray(TEXTURE_COORDS_1,_neighborArray.get());
    afav.applyArray(TEXTURE_COORDS_2,_neighborNormalArray.get());
}

void SharedGeometry::accept(osg::Drawable::ConstAttributeFunctor& af) const
{
    osg::ConstAttributeFunctorArrayVisitor afav(af);

    afav.applyArray(VERTICES,_vertexArray.get());
    afav.applyArray(NORMALS, _normalArray.get());
    afav.applyArray(TEXTURE_COORDS_0,_texcoordArray.get());
    afav.applyArray(TEXTURE_COORDS_1,_neighborArray.get());
    afav.applyArray(TEXTURE_COORDS_2,_neighborNormalArray.get());
}

void SharedGeometry::accept(osg::PrimitiveFunctor& pf) const
{
    pf.setVertexArray(_vertexArray->getNumElements(),static_cast<const osg::Vec3*>(_vertexArray->getDataPointer()));
    _drawElements->accept(pf);
}

void SharedGeometry::accept(osg::PrimitiveIndexFunctor& pif) const
{
    pif.setVertexArray(_vertexArray->getNumElements(),static_cast<const osg::Vec3*>(_vertexArray->getDataPointer()));
    _drawElements->accept(pif);
}

osg::Geometry*
SharedGeometry::makeOsgGeometry()
{
    osg::Geometry* geom = new osg::Geometry();
    geom->setName(typeid(*this).name());
    geom->setUseVertexBufferObjects(true);
    geom->setUseDisplayList(false);

    geom->setVertexArray(getVertexArray());
    geom->setNormalArray(getNormalArray());
    geom->setTexCoordArray(0, getTexCoordArray());
    if (getDrawElements())
    {
        if (getDrawElements()->getMode() == GL_PATCHES)
            getDrawElements()->setMode(GL_TRIANGLES);
        geom->addPrimitiveSet(getDrawElements());
    }

    return geom;
}
