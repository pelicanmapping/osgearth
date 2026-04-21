/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include "ObjectIndex"
#include "Capabilities"
#include "StringUtils"
#include "VirtualProgram"
#include "LineDrawable"

#include <osg/Geometry>

using namespace osgEarth;

#define LC "[ObjectIndex] "

//#undef OE_DEBUG
//#define OE_DEBUG OE_NOTICE

// Object IDs under this reserved
#define STARTING_OBJECT_ID 10

namespace
{
    const char* indexVertexInit = R"(
        #pragma vp_function oe_index_readObjectID, vertex_model, first

        uniform uint oe_index_objectid_uniform; // override objectid if > 0
        in uint      oe_index_objectid_attr;    // Vertex attribute containing the object ID.
        uint         oe_index_objectid;         // Stage global containing the Object ID.

        void oe_index_readObjectID(inout vec4 vertex)
        {
            if ( oe_index_objectid_uniform > 0u )
                oe_index_objectid = oe_index_objectid_uniform;
            else if ( oe_index_objectid_attr > 0u )
                oe_index_objectid = oe_index_objectid_attr;
            else
                oe_index_objectid = 0u;
        }
    )";
}

ObjectIndex::ObjectIndex() :
    _idGen(STARTING_OBJECT_ID)
{
    _attribName     = "oe_index_objectid_attr";
    _attribLocation = osg::Drawable::SECONDARY_COLORS;
    _oidUniformName = "oe_index_objectid_uniform";

    // set up the shader package.
    std::string source = Stringify()
        << "#version " << std::to_string(Capabilities::get().getGLSLVersionInt()) << "\n"
        << indexVertexInit;
    _shaders.add("ObjectIndex.vert.glsl", source);
}

bool
ObjectIndex::loadShaders(VirtualProgram* vp) const
{
    if ( vp )
    {
        _shaders.loadAll( vp );
        vp->addBindAttribLocation( getObjectIDAttribName(), getObjectIDAttribLocation() );
    }
    return vp != 0L;
}

void
ObjectIndex::setObjectIDAtrribLocation(int value)
{
    if ( _index.size() == 0 )
    {
        _attribLocation = value;
    } 
    else
    {
        OE_WARN << LC << "Illegal: Cannot change the attrib location once index is in use.\n";
    }
}

ObjectID
ObjectIndex::insert(osg::Referenced* object)
{
    std::lock_guard<std::mutex> excl( _mutex );
    return insertImpl( object );
}

ObjectID
ObjectIndex::insertImpl(osg::Referenced* object)
{
    // internal: assume mutex is locked
    ObjectID id = ++_idGen;
    _index[id] = object;
    return id;
}

osg::Referenced*
ObjectIndex::getImpl(ObjectID id) const
{
    // assume the mutex is locked
    IndexMap::const_iterator i = _index.find(id);
    return i != _index.end() ? i->second.get() : 0L;
}

void
ObjectIndex::remove(ObjectID id)
{
    std::lock_guard<std::mutex> excl(_mutex);
    removeImpl(id);
}

void
ObjectIndex::removeImpl(ObjectID id)
{
    // internal - assume mutex is locked
    _index.erase( id );
}

ObjectID
ObjectIndex::tagDrawable(osg::Drawable* drawable, osg::Referenced* object)
{
    std::lock_guard<std::mutex> lock(_mutex);
    ObjectID oid = insertImpl(object);
    tagDrawable(drawable, oid);
    return oid;
}

void
ObjectIndex::tagDrawable(osg::Drawable* drawable, ObjectID id) const
{
    if ( drawable == 0L )
        return;

    osg::ref_ptr<ObjectIDArray> ids = new ObjectIDArray();
    ids->setBinding(osg::Array::BIND_PER_VERTEX);
    ids->setNormalize(false);
    ids->setPreserveDataType(true);

    if (auto* geometry = drawable->asGeometry())
    {
        // add a new integer attributer to store the feautre ID per vertex.
        geometry->setVertexAttribArray(_attribLocation, ids);
        ids->assign(geometry->getVertexArray()->getNumElements(), id);
    }
    else if (auto* line = dynamic_cast<LineDrawable*>(drawable))
    {
        line->setVertexAttribArray(_attribLocation, ids);
        auto size = line->size();
        for (int i = 0; i < (int)size; ++i)
            line->pushVertexAttrib(ids.get(), id);
    }
}

ObjectID
ObjectIndex::tagRange(osg::Drawable* drawable, osg::Referenced* object, unsigned int start, unsigned int count)
{
    std::lock_guard<std::mutex> lock(_mutex);
    ObjectID oid = insertImpl(object);
    tagRange(drawable, oid, start, count);
    return oid;
}

void
ObjectIndex::tagRange(osg::Drawable* drawable, ObjectID id, unsigned int start, unsigned int count) const
{
    OE_SOFT_ASSERT_AND_RETURN(drawable, void());

    auto* geom = getGeometry(drawable);
    if (geom)
    {
        auto* oids = dynamic_cast<ObjectIDArray*>(geom->getVertexAttribArray(_attribLocation));

        if (!oids)
        {
            // add a new integer attribute array to store the feautre ID per vertex.
            oids = new ObjectIDArray();
            oids->setBinding(osg::Array::BIND_PER_VERTEX);
            oids->setNormalize(false);
            geom->setVertexAttribArray(_attribLocation, oids);
            oids->setPreserveDataType(true);
        }

        if (oids->size() < start + count)
        {
            oids->resize(start + count);
        }

        for (unsigned int i = 0; i < count; ++i)
        {
            (*oids)[start + i] = id;
        }

        oids->dirty();
    }
}



namespace
{
    struct FindAndTagDrawables : public osg::NodeVisitor
    {
        FindAndTagDrawables(const ObjectIndex* index, ObjectID id) : _index(index), _id(id)
        {
            setTraversalMode(TRAVERSE_ALL_CHILDREN);
            setNodeMaskOverride(~0);
        }

        void apply(osg::Drawable& drawable)
        {
            _index->tagDrawable(&drawable, _id);
            traverse(drawable);
        }

        const ObjectIndex* _index;
        ObjectID           _id;
    };
}

ObjectID
ObjectIndex::tagAllDrawables(osg::Node* node, osg::Referenced* object)
{
    std::lock_guard<std::mutex> lock(_mutex);
    ObjectID oid = insertImpl(object);
    tagAllDrawables(node, oid);
    return oid;
}

void
ObjectIndex::tagAllDrawables(osg::Node* node, ObjectID id) const
{
    if ( node )
    {
        FindAndTagDrawables visitor(this, id);
        node->accept( visitor );
    }
}

ObjectID
ObjectIndex::tagNode(osg::Node* node, osg::Referenced* object)
{
    std::lock_guard<std::mutex> lock(_mutex);
    ObjectID oid = insertImpl(object);
    tagNode(node, oid);
    return oid;
}

void
ObjectIndex::tagNode(osg::Node* node, ObjectID id) const
{
    if ( node )
    {
        osg::StateSet* stateSet = node->getOrCreateStateSet();
        stateSet->addUniform( new osg::Uniform(_oidUniformName.c_str(), id) );
    }
}

bool
ObjectIndex::getObjectIDs(const osg::Drawable* drawable, std::set<ObjectID>& output) const
{
    OE_SOFT_ASSERT_AND_RETURN(drawable, false);

    output.clear();

    auto* oids = getObjectIDArray(drawable);

    if (oids)
    {
        for(auto& oid : *oids)
        {
            output.insert(oid);
        }
    }

    return oids && !output.empty();
}

bool
ObjectIndex::getObjectID(osg::Node* node, ObjectID& output) const
{
    if (!node) return false;

    osg::StateSet* stateSet = node->getStateSet();
    if (!stateSet) return false;

    osg::Uniform* uniform = stateSet->getUniform(_oidUniformName.c_str());
    if ( !uniform ) return false;

    uniform->get(output);
    return true;
}

bool
ObjectIndex::updateObjectIDs(osg::Drawable* drawable,
                             std::unordered_map<ObjectID, ObjectID>& oldNewMap,
                             osg::Referenced* object)
{
    OE_SOFT_ASSERT_AND_RETURN(drawable, false);

    // in a drawable, replaces each OIDs in map.first with the corresponding OID in map.second

    auto* oids = getObjectIDArray(drawable);

    if (oids)
    {
        for (auto& oid : *oids)
        {
            ObjectID newoid;
            auto k = oldNewMap.find(oid);
            if (k != oldNewMap.end())
            {
                newoid = k->second;
            }
            else
            {
                newoid = insert(object);
                oldNewMap[oid] = newoid;
            }

            oid = newoid;
        }

        oids->dirty();
    }

    return oids != nullptr;
}

bool
ObjectIndex::updateObjectID(osg::Node* node,
                            std::unordered_map<ObjectID, ObjectID>& oldNewMap,
                            osg::Referenced* object)
{
    if (!node) return false;

    osg::StateSet* stateSet = node->getStateSet();
    if (!stateSet) return false;

    osg::Uniform* uniform = stateSet->getUniform(_oidUniformName.c_str());
    if ( !uniform ) return false;

    ObjectID oldoid;
    uniform->get(oldoid);

    ObjectID newoid;
    std::unordered_map<ObjectID, ObjectID>::iterator k = oldNewMap.find(oldoid);
    if (k != oldNewMap.end()) {
        newoid = k->second;
    }
    else {
        newoid = insert(object);
        oldNewMap[oldoid] = newoid;
    }

    uniform->set(newoid);

    return true;
}


osg::Geometry*
ObjectIndex::getGeometry(osg::Drawable* drawable) const
{
    if (auto* geometry = dynamic_cast<osg::Geometry*>(drawable))
    {
        return geometry;
    }
    else if (auto* line = dynamic_cast<LineDrawable*>(drawable))
    {
        return line->_geom;
    }
    return nullptr;
}

const osg::Geometry*
ObjectIndex::getGeometry(const osg::Drawable* drawable) const
{
    if (auto* geometry = dynamic_cast<const osg::Geometry*>(drawable))
    {
        return geometry;
    }
    else if (auto* line = dynamic_cast<const LineDrawable*>(drawable))
    {
        return line->_geom;
    }
    return nullptr;
}

ObjectIDArray*
ObjectIndex::getObjectIDArray(osg::Drawable* drawable) const
{
    auto* geom = getGeometry(drawable);
    return geom ? dynamic_cast<ObjectIDArray*>(geom->getVertexAttribArray(_attribLocation)) : nullptr;
}

const ObjectIDArray*
ObjectIndex::getObjectIDArray(const osg::Drawable* drawable) const
{
    auto* geom = getGeometry(drawable);
    return geom ? dynamic_cast<const ObjectIDArray*>(geom->getVertexAttribArray(_attribLocation)) : nullptr;
}