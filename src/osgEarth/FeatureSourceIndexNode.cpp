/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/FeatureSourceIndexNode>
#include <osgEarth/NodeUtils>
#include <osgEarth/Metrics>
#include <iterator>

using namespace osgEarth;

// for testing:
//#undef  OE_DEBUG
//#define OE_DEBUG OE_INFO

namespace
{
    // Project a map iterator onto its keys for bulk index removal.
    template<typename T>
    struct KeyIter
    {
        using iterator_category = std::input_iterator_tag;
        using value_type = typename T::key_type;
        using difference_type = typename T::difference_type;
        using pointer = const value_type*;
        using reference = const value_type&;

        typename T::const_iterator it;
        KeyIter(typename T::const_iterator i) : it(i) { }
        KeyIter& operator++() { ++it; return *this; }
        KeyIter operator++(int) { KeyIter t = *this; ++*this; return t; }

        pointer operator->() const { return &(it->first); }
        reference operator*() const { return it->first; }
        friend bool operator==(KeyIter const &a, KeyIter const &b) { return a.it == b.it; }
        friend bool operator!=(KeyIter const &a, KeyIter const &b) { return a.it != b.it; }
    };
}

//-----------------------------------------------------------------------------


FeatureSourceIndexOptions::FeatureSourceIndexOptions(const Config& conf)
{
    conf.get("enabled", _enabled);
    conf.get("embed_features", _embedFeatures);
}

Config
FeatureSourceIndexOptions::getConfig() const
{
    Config conf("feature_indexing");
    conf.set("enabled", _enabled);
    conf.set("embed_features", _embedFeatures);
    return conf;
}

//-----------------------------------------------------------------------------

#undef  LC
#define LC "[FeatureSourceIndexNode] "

FeatureSourceIndexNode::FeatureSourceIndexNode()
{
    //nop
}

FeatureSourceIndexNode::FeatureSourceIndexNode(const FeatureSourceIndexNode& rhs, const osg::CopyOp& copy) :
osg::Group(rhs, copy)
{
    _index = rhs._index.get();
    _fids  = rhs._fids;
}

FeatureSourceIndexNode::FeatureSourceIndexNode(FeatureSourceIndex* index) :
_index( index )
{
    //nop
}

FeatureSourceIndexNode::~FeatureSourceIndexNode()
{
    OE_PROFILING_ZONE;    
    if ( _index.valid() )
    {
        // must copy and clear the original list first to dereference the RefIDPair instances.
        std::set<FeatureID> fidsToRemove;
        fidsToRemove.insert(KeyIter<FID_to_RefIDPair>(_fids.begin()), KeyIter<FID_to_RefIDPair>(_fids.end()));
        _fids.clear();
        _index->removeFIDs( fidsToRemove.begin(), fidsToRemove.end() );
    }
}

ObjectID
FeatureSourceIndexNode::tagDrawable(osg::Drawable* drawable, Feature* feature)
{
    if ( !feature || !_index.valid() ) return OSGEARTH_OBJECTID_EMPTY;
    RefIDPair* r = _index->tagDrawable( drawable, feature );
    if ( r ) _fids[ feature->getFID() ] = r;
    return r ? r->_oid : OSGEARTH_OBJECTID_EMPTY;
}

ObjectID
FeatureSourceIndexNode::tagAllDrawables(osg::Node* node, Feature* feature)
{
    if ( !feature || !_index.valid() ) return OSGEARTH_OBJECTID_EMPTY;
    RefIDPair* r = _index->tagAllDrawables( node, feature );
    if ( r ) _fids[ feature->getFID() ] = r;
    return r ? r->_oid : OSGEARTH_OBJECTID_EMPTY;
}

ObjectID
FeatureSourceIndexNode::tagNode(osg::Node* node, Feature* feature)
{
    if ( !feature || !_index.valid() ) return OSGEARTH_OBJECTID_EMPTY;
    RefIDPair* r = _index->tagNode( node, feature );
    if ( r ) _fids[ feature->getFID() ] = r;
    return r ? r->_oid : OSGEARTH_OBJECTID_EMPTY;
}

ObjectID
FeatureSourceIndexNode::tagRange(osg::Drawable* drawable, Feature* feature, unsigned int start, unsigned int count)
{
    if (!feature || !_index.valid()) return OSGEARTH_OBJECTID_EMPTY;
    RefIDPair* r = _index->tagRange(drawable, feature, start, count);
    if (r) _fids[feature->getFID()] = r;
    return r ? r->_oid : OSGEARTH_OBJECTID_EMPTY;
}

bool
FeatureSourceIndexNode::getAllFIDs(std::vector<FeatureID>& output) const
{
    for (auto& iter : _fids)
    {
        output.push_back(iter.first);
    }

    return true;
}

void
FeatureSourceIndexNode::setFIDMap(const FeatureSourceIndexNode::FID_to_RefIDPair& fids)
{
    _fids = fids;
}

namespace
{
    /** Visitor that finds FeatureSourceIndexNodes, assigns their index, and
     *  reconstitutes their object ID maps. */
    struct Reconstitute : public osg::NodeVisitor
    {
        FeatureSourceIndex* _index;
        std::unordered_map<ObjectID,ObjectID> _oldToNew;

        Reconstitute(FeatureSourceIndex* index) :
            _index(index)
        {
            setNodeMaskOverride(~0);
            setTraversalMode(TRAVERSE_ALL_CHILDREN);
        }

        void apply(osg::Node& node)
        {
            FeatureSourceIndexNode* indexNode = dynamic_cast<FeatureSourceIndexNode*>(&node);
            if (indexNode)
            {
                indexNode->setIndex(_index);
                indexNode->reIndex(_oldToNew);
            }
            traverse(node);
        }
    };

    /** Visitor that re-indexes objects after deserialization. */
    struct ReIndex : public osg::NodeVisitor
    {
        FeatureSourceIndexNode* _indexNode;
        FeatureSourceIndexNode::FID_to_RefIDPair _newFIDMap;
        std::unordered_map<ObjectID,ObjectID>&   _oldToNew;

        ReIndex(FeatureSourceIndexNode* indexNode, std::unordered_map<ObjectID,ObjectID>& oldToNew) :
            _indexNode(indexNode), _oldToNew(oldToNew)
        {
            setTraversalMode(TRAVERSE_ALL_CHILDREN);
            setNodeMaskOverride(~0);
        }

        void apply(osg::Node& node)
        {
            _indexNode->reIndexNode(&node, _oldToNew, _newFIDMap);
            traverse(node);
        }

        void apply(osg::Geode& geode)
        {
            _indexNode->reIndexNode(&geode, _oldToNew, _newFIDMap);
            for (unsigned i = 0; i < geode.getNumDrawables(); ++i)
            {
                _indexNode->reIndexDrawable(geode.getDrawable(i), _oldToNew, _newFIDMap);
            }
            traverse(geode);
        }
    };
}

void
FeatureSourceIndexNode::reconstitute(osg::Node* graph, FeatureSourceIndex* index)
{
    if ( !graph || !index )
    {
        OE_WARN << LC << "INTERNAL ERROR cannot call reconsitute with null graph or null index\n";
        return;
    }

    Reconstitute visitor(index);
    graph->accept(visitor);
}

void
FeatureSourceIndexNode::reIndex(std::unordered_map<ObjectID,ObjectID>& oidmappings)
{
    ReIndex visitor(this, oidmappings);
    this->accept(visitor);
    _fids = visitor._newFIDMap;
    //OE_INFO << LC << "Reindexed " << _fids.size() << " mappings\n";
}

void
FeatureSourceIndexNode::reIndexDrawable(osg::Drawable* drawable, OID_to_OID& oldNew, FID_to_RefIDPair& newFIDMap)
{
    if ( !drawable || !_index.valid() ) return;

    _index->update(drawable, oldNew, _fids, newFIDMap);
}

void
FeatureSourceIndexNode::reIndexNode(osg::Node* node, OID_to_OID& oldNew, FID_to_RefIDPair& newFIDMap)
{
    if (!node || !_index.valid()) return;

    _index->update(node, oldNew, _fids, newFIDMap);
}

FeatureSourceIndexNode* FeatureSourceIndexNode::get(osg::Node* graph)
{
    return graph ? osgEarth::findTopMostNodeOfType<FeatureSourceIndexNode>(graph) : 0L;
}

//-----------------------------------------------------------------------------

#undef  LC
#define LC "[FeatureSourceIndex Serializer] "

// OSG SERIALIZER for FeatureSourceIndexNode
#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>

namespace osgEarth { namespace Serializers { namespace FeatureSourceIndexNodeClass
{
    using namespace osgEarth;

    bool checkFIDMap(const FeatureSourceIndexNode& node)
    {
        return !node.getFIDMap().empty();
    }

    bool writeFIDMap(osgDB::OutputStream& os, const FeatureSourceIndexNode& node)
    {
        const FeatureSourceIndexNode::FID_to_RefIDPair& fids = node.getFIDMap();

        os.writeSize(fids.size());
        os << os.BEGIN_BRACKET << std::endl;
        {
            for (FeatureSourceIndexNode::FID_to_RefIDPair::const_iterator i = fids.begin(); i != fids.end(); ++i)
            {
                const RefIDPair* idPair = i->second.get();
                os << (double)idPair->_fid << idPair->_oid;
            }
        }
        os << os.END_BRACKET << std::endl;

        return true;
    }

    bool readFIDMap(osgDB::InputStream& is, FeatureSourceIndexNode& node)
    {
        FeatureSourceIndexNode::FID_to_RefIDPair fids;
        //FeatureID fid;
        double fid;
        ObjectID oid;

        unsigned size = is.readSize();
        is >> is.BEGIN_BRACKET;
        {
            for (unsigned i=0; i<size; ++i)
            {
                is >> fid >> oid;
                fids[fid] = new RefIDPair(fid, oid);
            }
        }
        is >> is.END_BRACKET;
        node.setFIDMap(fids);

        return true;
    }

    REGISTER_OBJECT_WRAPPER(
        FeatureSourceIndexNode,
        new osgEarth::FeatureSourceIndexNode,
        osgEarth::FeatureSourceIndexNode,
        "osg::Object osg::Node osg::Group osgEarth::FeatureSourceIndexNode")
    {
        ADD_USER_SERIALIZER(FIDMap);
    }

} } }

//-----------------------------------------------------------------------------

#undef  LC
#define LC "[FeatureSourceIndex] "

FeatureSourceIndex::FeatureSourceIndex(FeatureSource* featureSource, ObjectIndex* index, const FeatureSourceIndexOptions& options) :
    _featureSource(featureSource),
    _masterIndex(index),
    _options(options)
{
    _embed =
        _options.embedFeatures() == true ||
        featureSource == 0L ||
        featureSource->supportsGetFeature() == false;
}

FeatureSourceIndex::~FeatureSourceIndex()
{
    if ( _masterIndex.valid() && !_oids.empty() )
    {
        // remove all OIDs from the master index.
        _masterIndex->remove( KeyIter<OID_to_FID>(_oids.begin()), KeyIter<OID_to_FID>(_oids.end()) );
    }

    _oids.clear();
    _fids.clear();
    _embeddedFeatures.clear();
}

RefIDPair*
FeatureSourceIndex::tagDrawable(osg::Drawable* drawable, Feature* feature)
{
    if ( !feature ) return 0L;

    std::lock_guard<std::mutex> lock(_mutex);

    RefIDPair* p = 0L;
    FeatureID fid = feature->getFID();

    FID_to_RefIDPair::const_iterator f = _fids.find( fid );
    if ( f != _fids.end() )
    {
        ObjectID oid = f->second->_oid;
        _masterIndex->tagDrawable( drawable, oid );
        p = f->second.get();
    }
    else
    {
        ObjectID oid = _masterIndex->tagDrawable( drawable, this );
        p = new RefIDPair( fid, oid );
        _fids[fid] = p;
        _oids[oid] = fid;

        if ( _embed )
        {
            _embeddedFeatures[fid] = feature;
        }
    }

    return p;
}

RefIDPair*
FeatureSourceIndex::tagAllDrawables(osg::Node* node, Feature* feature)
{
    if ( !feature ) return 0L;

    std::lock_guard<std::mutex> lock(_mutex);

    RefIDPair* p = 0L;
    FeatureID fid = feature->getFID();

    FID_to_RefIDPair::const_iterator f = _fids.find( fid );
    if ( f != _fids.end() )
    {
        ObjectID oid = f->second->_oid;
        _masterIndex->tagAllDrawables( node, oid );
        p = f->second.get();
    }
    else
    {
        ObjectID oid = _masterIndex->tagAllDrawables( node, this );
        p = new RefIDPair( fid, oid );
        _fids[fid] = p;
        _oids[oid] = fid;

        if ( _embed )
        {
            _embeddedFeatures[fid] = feature;
        }
    }

    return p;
}

RefIDPair*
FeatureSourceIndex::tagRange(osg::Drawable* drawable, Feature* feature, unsigned int start, unsigned int count)
{
    if (!feature) return 0L;

    std::lock_guard<std::mutex> lock(_mutex);

    RefIDPair* p = 0L;
    FeatureID fid = feature->getFID();

    FID_to_RefIDPair::const_iterator f = _fids.find(fid);
    if (f != _fids.end())
    {
        ObjectID oid = f->second->_oid;
        _masterIndex->tagRange(drawable, oid, start, count);
        p = f->second.get();
    }
    else
    {
        ObjectID oid = _masterIndex->tagRange(drawable, this, start, count);
        p = new RefIDPair(fid, oid);
        _fids[fid] = p;
        _oids[oid] = fid;

        if (_embed)
        {
            _embeddedFeatures[fid] = feature;
        }
    }

    return p;
}

RefIDPair*
FeatureSourceIndex::tagNode(osg::Node* node, Feature* feature)
{
    if ( !feature ) return 0L;

    std::lock_guard<std::mutex> lock(_mutex);

    RefIDPair* p = 0L;
    FeatureID fid = feature->getFID();
    ObjectID oid;

    FID_to_RefIDPair::const_iterator f = _fids.find( fid );
    if ( f != _fids.end() )
    {
        oid = f->second->_oid;
        _masterIndex->tagNode( node, oid );
        p = f->second.get();
    }
    else
    {
        oid = _masterIndex->tagNode( node, this );
        p = new RefIDPair( fid, oid );
        _fids[fid] = p;
        _oids[oid] = fid;

        if ( _embed )
        {
            _embeddedFeatures[fid] = feature;
        }
    }

    return p;
}

Feature*
FeatureSourceIndex::getFeature(ObjectID oid) const
{
    Feature* feature = 0L;
    std::lock_guard<std::mutex> lock(_mutex);
    OID_to_FID::const_iterator i = _oids.find( oid );
    if ( i != _oids.end() )
    {
        FeatureID fid = i->second;

        if ( _embed )
        {
            FID_to_Feature::const_iterator j = _embeddedFeatures.find( fid );
            feature = j != _embeddedFeatures.end() ? j->second.get() : 0L;
        }
        else if ( _featureSource.valid() && _featureSource->supportsGetFeature() )
        {
            feature = _featureSource->getFeature( fid );
        }
    }
    return feature;
}

ObjectID
FeatureSourceIndex::getObjectID(FeatureID fid) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    FID_to_RefIDPair::const_iterator i = _fids.find(fid);
    if ( i != _fids.end() )
        return i->second->_oid;
    else
        return OSGEARTH_OBJECTID_EMPTY;
}

// When Feature index data is deserialized, the old serialized ObjectIDs are
// no longer valid. This method will re-install the mappings in the master index
// and write new local mappings with new ObjectIDs.
void
FeatureSourceIndex::update(osg::Drawable* drawable, OID_to_OID& oldToNew, const FID_to_RefIDPair& oldFIDMap, FID_to_RefIDPair& newFIDMap)
{
    unsigned count = 0;
    if (_masterIndex->updateObjectIDs(drawable, oldToNew, this))
    {
        for (std::unordered_map<ObjectID, ObjectID>::const_iterator i = oldToNew.begin(); i != oldToNew.end(); ++i)
        {
            const ObjectID& oldoid = i->first;
            const ObjectID& newoid = i->second;

            for (FID_to_RefIDPair::const_iterator j = oldFIDMap.begin(); j != oldFIDMap.end(); ++j)
            {
                const RefIDPair* rip = j->second.get();
                if (rip && rip->_oid == oldoid)
                {
                    RefIDPair* newrip = new RefIDPair(rip->_fid, newoid);
                    _oids[newoid] = rip->_fid;
                    _fids[rip->_fid] = newrip;
                    newFIDMap[rip->_fid] = newrip;
                    ++count;
                }
            }
        }
    }
}

// When Feature index data is deserialized, the old serialized ObjectIDs are
// no longer valid. This method will re-install the mappings in the master index
// and write new local mappings with new ObjectIDs.
void
FeatureSourceIndex::update(osg::Node* node, OID_to_OID& oldToNew, const FID_to_RefIDPair& oldFIDMap, FID_to_RefIDPair& newFIDMap)
{
    unsigned count = 0;
    if (_masterIndex->updateObjectID(node, oldToNew, this))
    {
        for (std::unordered_map<ObjectID, ObjectID>::const_iterator i = oldToNew.begin(); i != oldToNew.end(); ++i)
        {
            const ObjectID& oldoid = i->first;
            const ObjectID& newoid = i->second;

            for (FID_to_RefIDPair::const_iterator j = oldFIDMap.begin(); j != oldFIDMap.end(); ++j)
            {
                const RefIDPair* rip = j->second.get();
                if (rip && rip->_oid == oldoid)
                {
                    RefIDPair* newrip = new RefIDPair(rip->_fid, newoid);
                    _oids[newoid] = rip->_fid;
                    _fids[rip->_fid] = newrip;
                    newFIDMap[rip->_fid] = newrip;
                    ++count;
                }
            }
        }
    }
}
