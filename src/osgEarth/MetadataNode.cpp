/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/MetadataNode>
#include <osgEarth/VirtualProgram>
#include <osgEarth/Registry>
#include <osgEarth/Shaders>
#include <osgDB/ObjectWrapper>

using namespace osgEarth;

inline void packInt64(int64_t value, uint32_t& a, uint32_t& b)
{ 
    a = (uint32_t)((value & 0XFFFFFFFF00000000LL) >> 32);
    b = (uint32_t)(value & 0XFFFFFFFFLL);
}

inline int64_t unpackInt64(uint32_t a, uint32_t b)
{
    return ((int64_t)a) << 32 | b;
}

int64_t readInt64(osgDB::InputStream& is)
{
    uint32_t a, b;
    is >> a >> b;
    return unpackInt64(a, b);
}

void writeInt64(osgDB::OutputStream& os, int64_t value)
{
    uint32_t a, b;
    packInt64(value, a, b);
    os << a << b;
}

MetadataNode::MetadataNode()
{
    init();
}

MetadataNode::MetadataNode(const MetadataNode& rhs, const osg::CopyOp& copyop) :
    _features(rhs._features)
{
    init();
}

void MetadataNode::init()
{
    _instances = new osg::Vec2uiArray;

    auto vp = Registry::instance()->getOrCreate<VirtualProgram>("vp.MetadataNode", []()
        {
            osgEarth::VirtualProgram* vp = new VirtualProgram();
            vp->setInheritShaders(true);
            vp->setName("MetadataNode");
            Shaders pkg;
            pkg.load(vp, pkg.MetadataNode);
            vp->addBindAttribLocation("oe_index_attr", osg::Drawable::SECONDARY_COLORS);
            return vp;
        });
    osg::StateSet* ss = getOrCreateStateSet();
    ss->setAttribute(vp);
}

ObjectID MetadataNode::add(Feature* feature, bool visible)
{
    for (unsigned int i = 0; i < _features.size(); ++i)
    {
        if (_features[i]->getFID() == feature->getFID())
        {
            return i;
        }
    }

    _features.push_back(feature);

    // Store the index information
    osg::Vec2ui instance;
    instance.y() = visible;
    //instance.objectID = osgEarth::Registry::instance()->getObjectIndex()->insert(feature);
    instance.x() = osgEarth::Registry::instance()->getObjectIndex()->insert(this);
    _instances->push_back(instance);
    _instances->dirty();

    return _features.size() - 1;
}

void MetadataNode::tagDrawable(osg::Drawable* drawable, ObjectID id) const
{
    if (drawable == 0L)
        return;

    osg::Geometry* geom = drawable->asGeometry();
    if (!geom)
        return;

    // add a new integer attributer to store the feautre ID per vertex.
    ObjectIDArray* ids = new ObjectIDArray();
    ids->reserve(geom->getVertexArray()->getNumElements());
    ids->setBinding(osg::Array::BIND_PER_VERTEX);
    ids->setNormalize(false);
    geom->setVertexAttribArray(osg::Geometry::SECONDARY_COLORS, ids);
    ids->setPreserveDataType(true);    
    // Add 1 to the index to indicate that there is something there.
    ids->assign(geom->getVertexArray()->getNumElements(), id+1);
}

MetadataNode::~MetadataNode()
{
    for (auto &i : *_instances)
    {
        osgEarth::Registry::instance()->getObjectIndex()->remove(i.x());
    }
}

void MetadataNode::tagNode(osg::Node* node, ObjectID id) const
{
    osg::StateSet* stateSet = node->getOrCreateStateSet();
    // TODO:  Change this name
    stateSet->addUniform(new osg::Uniform("oe_index_objectid_uniform", id + 1));
}


void MetadataNode::finalize()
{
    osg::ShaderStorageBufferObject* ssbo = new osg::ShaderStorageBufferObject;
    ssbo->setUsage(GL_DYNAMIC_DRAW);

    _instances->setBufferObject(ssbo);

    osg::ShaderStorageBufferBinding* ssbb = new osg::ShaderStorageBufferBinding(0, _instances.get());

    getOrCreateStateSet()->setAttributeAndModes(ssbb, osg::StateAttribute::ON);

}

unsigned int MetadataNode::getNumFeatures() const
{
    return _features.size();
}

bool MetadataNode::getVisible(unsigned int index) const
{
    return (*_instances)[index].y();
}

void MetadataNode::setVisible(unsigned int index, bool value)
{
    (*_instances)[index].y() = value;
    _instances->dirty();
}

ObjectID MetadataNode::getObjectID(unsigned index) const
{
    return (*_instances)[index].x();
}

const Feature* MetadataNode::getFeature(unsigned int index) const
{
    return _features[index];
}

Feature* MetadataNode::getFeature(unsigned int index)
{
    return _features[index];
}

int MetadataNode::getIndexFromObjectID(ObjectID id) const
{
    for (unsigned int i = 0; i < _instances->size(); ++i)
    {
        if ((*_instances)[i].x() == id)
        {
            return i;
        }
    }
    return -1;
}


namespace osgEarth
{
    namespace Serializers
    {
        namespace MetadataNode
        {
            static bool checkFeatures(const osgEarth::MetadataNode& g)
            {             
                return g.getNumFeatures() > 0;
            }

            static bool readFeatures(osgDB::InputStream& is, osgEarth::MetadataNode& g)
            {
                // Outer object
                is >> is.BEGIN_BRACKET;

                std::vector< std::string > keys;
                is >> is.PROPERTY("Keys");
                unsigned int numKeys = is.readSize();

                is >> is.BEGIN_BRACKET;
                for (unsigned int i = 0; i < numKeys; ++i)
                {
                    std::string key;
                    is >> key;
                    keys.push_back(key);
                }
                is >> is.END_BRACKET;

                is >> is.PROPERTY("Features");
                unsigned int numFeatures = is.readSize(); is >> is.BEGIN_BRACKET;

                for (unsigned int i = 0; i < numFeatures; ++i)
                {
                    is >> is.BEGIN_BRACKET;

                    FeatureID fid = readInt64(is);                    

                    bool visible;
                    is >> visible;

                    osg::ref_ptr< Feature > feature = new Feature(nullptr, nullptr);
                    feature->setFID(fid);

                    is >> is.PROPERTY("Attrs");
                    unsigned int numAttributes = is.readSize(); is >> is.BEGIN_BRACKET;
                    if (numAttributes > 0)
                    {
                        for (unsigned int j = 0; j < numAttributes; ++j)
                        {
                            unsigned int attrKey;
                            is >> attrKey;
                            std::string attrName = keys[attrKey];
                            unsigned int attrTypeInt;
                            osgEarth::AttributeType attrType;

                            is >> attrTypeInt;
                            attrType = (osgEarth::AttributeType)attrTypeInt;
                            switch (attrType)
                            {
                            case osgEarth::ATTRTYPE_BOOL:
                            {
                                bool v;
                                is >> v;
                                feature->set(attrName, v);
                                break;
                            }
                            case osgEarth::ATTRTYPE_STRING:
                            {
                                std::string v;
                                is.readWrappedString(v);
                                feature->set(attrName, v);
                                break;
                            }
                            case osgEarth::ATTRTYPE_DOUBLE:
                            {
                                double v;
                                is >> v;
                                feature->set(attrName, v);
                                break;
                            }
                            case osgEarth::ATTRTYPE_INT:
                            {
                                int64_t v = readInt64(is);
                                feature->set(attrName, static_cast<std::int64_t>(v));
                                break;
                            }
                            default:
                                break;
                            }
                        }
                        is >> is.END_BRACKET; // end attributes
                    }

                    g.add(feature.get(), visible);
                    is >> is.END_BRACKET; // end feature
                }
                is >> is.END_BRACKET; // end features

                is >> is.END_BRACKET; // Outer object

                g.finalize();

                return true;
            }

            static bool writeFeatures(osgDB::OutputStream& os, const osgEarth::MetadataNode& g)
            {
                os << os.BEGIN_BRACKET << std::endl;

                // First collect all the keys
                std::vector< std::string > keys;
                std::map<std::string, unsigned int> keysToIndex;
                for (unsigned int i = 0; i < g.getNumFeatures(); ++i)
                {
                    const Feature* feature = g.getFeature(i);
                    for (auto& attr : feature->getAttrs())
                    {
                        std::string key = attr.first;
                        auto itr = keysToIndex.find(key);
                        if (itr == keysToIndex.end())
                        {
                            keysToIndex[key] = keys.size();                                                        
                            keys.push_back(key);
                        }
                    }
                }

                os << os.PROPERTY("Keys");
                os.writeSize(keys.size());
                os << os.BEGIN_BRACKET << std::endl;
                {
                    for (auto& key : keys)
                    {
                        os.writeWrappedString(key);
                        os << std::endl;
                    }
                }
                os << os.END_BRACKET << std::endl; // End keys

                os << os.PROPERTY("Features");
                os.writeSize(g.getNumFeatures());                
                os << os.BEGIN_BRACKET << std::endl;
                for (unsigned int i = 0; i < g.getNumFeatures(); ++i)
                {
                    const Feature* feature = g.getFeature(i);

                    os << os.BEGIN_BRACKET << std::endl;
                    writeInt64(os, feature->getFID()); os << std::endl;

                    os << g.getVisible(i) << std::endl;

                    os << os.PROPERTY("Attrs");
                    os.writeSize(feature->getAttrs().size());
                    os << os.BEGIN_BRACKET << std::endl;
                    for (auto& attr : feature->getAttrs())
                    {
                        auto type = attr.second.getType();
                        os << (unsigned int)keysToIndex[attr.first] << std::endl;
                        os << (unsigned int)type << std::endl;
                        switch (type)
                        {
                        case osgEarth::ATTRTYPE_STRING:
                            os.writeWrappedString(attr.second.getString());
                            break;
                        case osgEarth::ATTRTYPE_DOUBLE:
                            os << attr.second.getDouble();
                            break;
                        case osgEarth::ATTRTYPE_INT:
                            writeInt64(os, attr.second.getInt());
                            break;
                        case osgEarth::ATTRTYPE_BOOL:
                            os << attr.second.getBool();
                            break;
                        default:
                            break;
                        }
                        os << std::endl;
                    }
                    os << os.END_BRACKET << std::endl; // End attribute

                    os << os.END_BRACKET << std::endl; // end Feature
                }

                os << os.END_BRACKET << std::endl; // End features


                os << os.END_BRACKET << std::endl; // End outer object

                return true;
            }

            REGISTER_OBJECT_WRAPPER(
                MetadataNode,
                new osgEarth::MetadataNode,
                osgEarth::MetadataNode,
                // Purposely disable osg::Node serialization to avoid saving out the StateSet which will have the ShaderStorageBufferObject on it
                // as well as the VirtualProgram.  We want to rehydrate those on load ourselves on load.
                "osg::Object osg::Group osgEarth::MetadataNode")
            {
                ADD_USER_SERIALIZER(Features);
            }
        }
    }
}