/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#ifndef OSGEARTH_GLTF_WRITER_H
#define OSGEARTH_GLTF_WRITER_H

#include <osg/Node>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osgDB/FileNameUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/FileUtils>
#include <osgDB/WriteFile>
#include <osgEarth/Notify>
#include <osgEarth/StringUtils>
#include <stack>

using namespace osgEarth;
using namespace osgEarth::Util;

#undef LC
#define LC "[GLTFWriter] "

//! Visitor that builds a GLTF data model from an OSG scene graph.
class OSGtoGLTF : public osg::NodeVisitor
{
private:
    typedef std::map<osg::ref_ptr< const osg::Node >, int> OsgNodeSequenceMap;
    typedef std::map<osg::ref_ptr<const osg::BufferData>, int> ArraySequenceMap;
    typedef std::map< osg::ref_ptr<const osg::Array>, int> AccessorSequenceMap;
    typedef std::vector< osg::ref_ptr< osg::StateSet > > StateSetStack;

    std::vector< osg::ref_ptr< osg::Texture > > _textures;

    tinygltf::Model& _model;
    std::stack<tinygltf::Node*> _gltfNodeStack;
    OsgNodeSequenceMap _osgNodeSeqMap;
    ArraySequenceMap _buffers;
    ArraySequenceMap _bufferViews;
    ArraySequenceMap _accessors;
    StateSetStack _ssStack;

public:
    OSGtoGLTF(tinygltf::Model& model) : _model(model)
    {
        setTraversalMode(TRAVERSE_ALL_CHILDREN);
        setNodeMaskOverride(~0);

        // default root scene:
        _model.scenes.push_back(tinygltf::Scene());
        tinygltf::Scene& scene = _model.scenes.back();
        _model.defaultScene = 0;
    }

    void push(tinygltf::Node& gnode)
    {
        _gltfNodeStack.push(&gnode);
    }

    void pop()
    {
        _gltfNodeStack.pop();
    }

    bool pushStateSet(osg::StateSet* stateSet)
    {
        osg::Texture* osgTexture = dynamic_cast<osg::Texture*>(stateSet->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
        if (!osgTexture)
        {
            return false;
        }

        _ssStack.push_back(stateSet);
        return true;
    }

    void popStateSet()
    {
        _ssStack.pop_back();
    }


    void apply(osg::Node& node)
    {
        bool isRoot = _model.scenes[_model.defaultScene].nodes.empty();
        if (isRoot)
        {
            // put a placeholder here just to prevent any other nodes
            // from thinking they are the root
            _model.scenes[_model.defaultScene].nodes.push_back(-1);
        }

        bool pushedStateSet = false;
        osg::ref_ptr< osg::StateSet > ss = node.getStateSet();
        if (ss)
        {
            pushedStateSet = pushStateSet(ss.get());
        }

        traverse(node);

        if (ss && pushedStateSet)
        {
            popStateSet();
        }

        _model.nodes.push_back(tinygltf::Node());
        tinygltf::Node& gnode = _model.nodes.back();
        int id = _model.nodes.size() - 1;
        gnode.name = Strings::Stringify() << "_gltfNode_" << id;
        _osgNodeSeqMap[&node] = id;

        if (isRoot)
        {
            // replace the placeholder with the actual root id.
            _model.scenes[_model.defaultScene].nodes.back() = id;
        }
    }

    void apply(osg::Group& group)
    {
        apply(static_cast<osg::Node&>(group));

        for (unsigned i = 0; i < group.getNumChildren(); ++i)
        {
            int id = _osgNodeSeqMap[group.getChild(i)];
            _model.nodes.back().children.push_back(id);
        }
    }

    void apply(osg::Transform& xform)
    {
        apply(static_cast<osg::Group&>(xform));

        osg::Matrix matrix;
        xform.computeLocalToWorldMatrix(matrix, this);
        const double* ptr = matrix.ptr();
        for (unsigned i = 0; i < 16; ++i)
            _model.nodes.back().matrix.push_back(*ptr++);
    }

    unsigned getBytesInDataType(GLenum dataType)
    {
        return
            dataType == GL_BYTE || dataType == GL_UNSIGNED_BYTE ? 1 :
            dataType == GL_SHORT || dataType == GL_UNSIGNED_SHORT ? 2 :
            dataType == GL_INT || dataType == GL_UNSIGNED_INT || dataType == GL_FLOAT ? 4 :
            0;
    }

    unsigned getBytesPerElement(const osg::Array* data)
    {
        return data->getDataSize() * getBytesInDataType(data->getDataType());
    }

    unsigned getBytesPerElement(const osg::DrawElements* data)
    {
        return
            dynamic_cast<const osg::DrawElementsUByte*>(data) ? 1 :
            dynamic_cast<const osg::DrawElementsUShort*>(data) ? 2 :
            4;
    }

    int getOrCreateBuffer(const osg::BufferData* data, GLenum type)
    {
        ArraySequenceMap::iterator a = _buffers.find(data);
        if (a != _buffers.end())
            return a->second;

        _model.buffers.push_back(tinygltf::Buffer());
        tinygltf::Buffer& buffer = _model.buffers.back();
        int id = _model.buffers.size() - 1;
        _buffers[data] = id;

        int bytes = getBytesInDataType(type);
        buffer.data.resize(data->getTotalDataSize());

        //TODO: account for endianess
        unsigned char* ptr = (unsigned char*)(data->getDataPointer());
        for (unsigned i = 0; i < data->getTotalDataSize(); ++i)
            buffer.data[i] = *ptr++;

        return id;
    }

    int getOrCreateBufferView(const osg::BufferData* data, GLenum type, GLenum target)
    {
        ArraySequenceMap::iterator a = _bufferViews.find(data);
        if (a != _bufferViews.end())
            return a->second;

        int bufferId = -1;
        ArraySequenceMap::iterator buffersIter = _buffers.find(data);
        if (buffersIter != _buffers.end())
            bufferId = buffersIter->second;
        else
            bufferId = getOrCreateBuffer(data, type);

        _model.bufferViews.push_back(tinygltf::BufferView());
        tinygltf::BufferView& bv = _model.bufferViews.back();
        int id = _model.bufferViews.size() - 1;
        _bufferViews[data] = id;

        bv.buffer = bufferId;
        bv.byteLength = data->getTotalDataSize();
        bv.byteOffset = 0;
        bv.target = target;

        //ONLY used for vertex attrbs, I guess:
        //unsigned bytesPerComponent = getBytesPerComponent(data->getDataType());
        //unsigned componentsPerElement = data->getDataSize();
        //bv.byteStride = bytesPerComponent * componentsPerElement;

        return id;
    }

    int getOrCreateAccessor(osg::Array* data, osg::PrimitiveSet* pset, tinygltf::Primitive& prim, const std::string& attr)
    {
        ArraySequenceMap::iterator a = _accessors.find(data);
        if (a != _accessors.end())
            return a->second;

        ArraySequenceMap::iterator bv = _bufferViews.find(data);
        if (bv == _bufferViews.end())
            return -1;

        _model.accessors.push_back(tinygltf::Accessor());
        tinygltf::Accessor& accessor = _model.accessors.back();
        int accessorId = _model.accessors.size() - 1;
        prim.attributes[attr] = accessorId;

        accessor.type =
            data->getDataSize() == 1 ? TINYGLTF_TYPE_SCALAR :
            data->getDataSize() == 2 ? TINYGLTF_TYPE_VEC2 :
            data->getDataSize() == 3 ? TINYGLTF_TYPE_VEC3 :
            data->getDataSize() == 4 ? TINYGLTF_TYPE_VEC4 :
            TINYGLTF_TYPE_SCALAR;

        accessor.bufferView = bv->second;
        accessor.byteOffset = 0;
        accessor.componentType = data->getDataType();
        accessor.count = data->getNumElements();
        accessor.normalized = data->getNormalize();

        const osg::DrawArrays* da = dynamic_cast<const osg::DrawArrays*>(pset);
        if (da)
        {
            accessor.byteOffset = da->getFirst() * getBytesPerElement(data);
            accessor.count = da->getCount();
        }

        //TODO: indexed elements
        osg::DrawElements* de = dynamic_cast<osg::DrawElements*>(pset);
        if (de)
        {
            _model.accessors.push_back(tinygltf::Accessor());
            tinygltf::Accessor& idxAccessor = _model.accessors.back();
            prim.indices = _model.accessors.size() - 1;

            idxAccessor.type = TINYGLTF_TYPE_SCALAR;
            idxAccessor.byteOffset = 0;
            idxAccessor.componentType = de->getDataType();
            idxAccessor.count = de->getNumIndices();

            getOrCreateBuffer(de, idxAccessor.componentType);
            int idxBV = getOrCreateBufferView(de, idxAccessor.componentType, GL_ELEMENT_ARRAY_BUFFER_ARB);

            idxAccessor.bufferView = idxBV;
        }

        return accessorId;
    }

    int getCurrentMaterial()
    {
        if (_ssStack.size() > 0)
        {
            osg::ref_ptr< osg::StateSet > stateSet = _ssStack.back();

            // Try to get the current texture
            osg::Texture* osgTexture = dynamic_cast<osg::Texture*>(stateSet->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
            if (osgTexture)
            {
                // Try to find the existing texture, which corresponds to a material index
                for (unsigned int i = 0; i < _textures.size(); i++)
                {
                    if (_textures[i].get() == osgTexture)
                    {
                        return i;
                    }
                }

                osg::ref_ptr< const osg::Image > osgImage = osgTexture->getImage(0);
                if (osgImage)
                {
                    int index = _textures.size();

                    _textures.push_back(osgTexture);

                                     
                    // Flip the image before writing
                    osg::ref_ptr< osg::Image > flipped = new osg::Image(*osgImage.get());
                    flipped->flipVertical();

                    std::string filename;

                    std::string ext = "png";// osgDB::getFileExtension(osgImage->getFileName());

                    // If the image has a filename try to hash it so we only write out one copy of it.  
                    if (!osgImage->getFileName().empty())
                    {
                        filename = Stringify() << std::hex << ::Strings::hashString(osgImage->getFileName()) << "." << ext;                        

                        if (!osgDB::fileExists(filename))
                        {
                            osgDB::writeImageFile(*flipped.get(), filename);
                        }                        
                    }
                    else
                    {                      
                        // Otherwise just find a filename that doesn't exist
                        int fileNameInc = 0;
                        do
                        {
                            std::stringstream ss;
                            ss << fileNameInc << "." << ext;
                            filename = ss.str();
                            fileNameInc++;
                        } while (osgDB::fileExists(filename));
                        osgDB::writeImageFile(*flipped.get(), filename);
                    }
                                   
                    // Add the image
                    // TODO:  Find a better way to write out the image url.  Right now it's assuming a ../.. scheme.
                    Image image;
                    std::stringstream buf;
                    buf << "../../" << filename;
                    image.uri = buf.str();//filename;
                    _model.images.push_back(image);

                    // Add the sampler
                    Sampler sampler;
                    osg::Texture::WrapMode wrapS = osgTexture->getWrap(osg::Texture::WRAP_S);
                    osg::Texture::WrapMode wrapT = osgTexture->getWrap(osg::Texture::WRAP_T);
                    osg::Texture::WrapMode wrapR = osgTexture->getWrap(osg::Texture::WRAP_R);

                    // Validate the clamp mode to be compatible with webgl
                    if ((wrapS == osg::Texture::CLAMP) || (wrapS == osg::Texture::CLAMP_TO_BORDER))
                    {                     
                        wrapS = osg::Texture::CLAMP_TO_EDGE;
                    }
                    if ((wrapT == osg::Texture::CLAMP) || (wrapT == osg::Texture::CLAMP_TO_BORDER))
                    {                     
                        wrapT = osg::Texture::CLAMP_TO_EDGE;
                    }
                    if ((wrapR == osg::Texture::CLAMP) || (wrapR == osg::Texture::CLAMP_TO_BORDER))
                    {                     
                        wrapR = osg::Texture::CLAMP_TO_EDGE;
                    }                    
                    sampler.wrapS = wrapS;
                    sampler.wrapT = wrapT;
                    sampler.wrapR = wrapR;
                    sampler.minFilter = osgTexture->getFilter(osg::Texture::MIN_FILTER);
                    sampler.magFilter = osgTexture->getFilter(osg::Texture::MAG_FILTER);

                    _model.samplers.push_back(sampler);

                    // Add the texture
                    tinygltf::Texture texture;
                    texture.source = index;
                    texture.sampler = index;
                    _model.textures.push_back(texture);

                    // Add the material
                    Material mat;
                    Parameter textureParam;
                    textureParam.json_double_value["index"] = index;
                    textureParam.json_double_value["texCoord"] = 0;
                    mat.values["baseColorTexture"] = textureParam;

                    Parameter colorFactor;
                    colorFactor.number_array.push_back(1.0);
                    colorFactor.number_array.push_back(1.0);
                    colorFactor.number_array.push_back(1.0);
                    colorFactor.number_array.push_back(1.0);

                    Parameter metallicFactor;
                    metallicFactor.has_number_value = true;
                    metallicFactor.number_value = 0.0;
                    mat.values["metallicFactor"] = metallicFactor;

                    Parameter roughnessFactor;
                    roughnessFactor.number_value = 1.0;
                    roughnessFactor.has_number_value = true;
                    mat.values["roughnessFactor"] = roughnessFactor;

                    mat.doubleSided = ((stateSet->getMode(GL_CULL_FACE) & osg::StateAttribute::ON) == 0);

                    if (stateSet->getMode(GL_BLEND) & osg::StateAttribute::ON) {
                        mat.alphaMode = "BLEND";
                    }
                    
                    _model.materials.push_back(mat);
                    return index;
                }
            }
        }
        return -1;
    }

    void apply(osg::Drawable& drawable)
    {
        if (drawable.asGeometry())
        {
            apply(static_cast<osg::Node&>(drawable));

            osg::ref_ptr< osg::StateSet > ss = drawable.getStateSet();
            bool pushedStateSet = false;
            if (ss.valid())
            {
                pushedStateSet = pushStateSet(ss.get());
            }

            osg::Geometry* geom = drawable.asGeometry();

            _model.meshes.push_back(tinygltf::Mesh());
            tinygltf::Mesh& mesh = _model.meshes.back();
            _model.nodes.back().mesh = _model.meshes.size() - 1;

            osg::Vec3f posMin(FLT_MAX, FLT_MAX, FLT_MAX);
            osg::Vec3f posMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            osg::Vec3Array* positions = dynamic_cast<osg::Vec3Array*>(geom->getVertexArray());
            if (positions)
            {
                getOrCreateBufferView(positions, GL_FLOAT, GL_ARRAY_BUFFER_ARB);
                for (unsigned i = 0; i < positions->size(); ++i)
                {
                    const osg::Vec3f& v = (*positions)[i];
                    posMin.x() = osg::minimum(posMin.x(), v.x());
                    posMin.y() = osg::minimum(posMin.y(), v.y());
                    posMin.z() = osg::minimum(posMin.z(), v.z());
                    posMax.x() = osg::maximum(posMax.x(), v.x());
                    posMax.y() = osg::maximum(posMax.y(), v.y());
                    posMax.z() = osg::maximum(posMax.z(), v.z());
                }
            }

            osg::Vec3Array* normals = dynamic_cast<osg::Vec3Array*>(geom->getNormalArray());
            if (normals)
            {
                getOrCreateBufferView(normals, GL_FLOAT, GL_ARRAY_BUFFER_ARB);
            }

            osg::Vec4Array* colors = dynamic_cast<osg::Vec4Array*>(geom->getColorArray());
            if (colors)
            {
                getOrCreateBufferView(colors, GL_FLOAT, GL_ARRAY_BUFFER_ARB);
            }

            osg::ref_ptr< osg::Vec2Array > texCoords = dynamic_cast<osg::Vec2Array*>(geom->getTexCoordArray(0));
            if (!texCoords.valid())
            {                
                // See if we have 3d texture coordinates and convert them to vec2
                osg::Vec3Array* texCoords3 = dynamic_cast<osg::Vec3Array*>(geom->getTexCoordArray(0));
                if (texCoords3)
                {
                    texCoords = new osg::Vec2Array;
                    for (unsigned int i = 0; i < texCoords3->size(); i++)
                    {
                        texCoords->push_back(osg::Vec2((*texCoords3)[i].x(), (*texCoords3)[i].y()));
                    }
                    //geom->setTexCoordArray(0, texCoords.get());
                }
            }

            if (texCoords.valid())
            {
                getOrCreateBufferView(texCoords.get(), GL_FLOAT, GL_ARRAY_BUFFER_ARB);
            }

            for (unsigned i = 0; i < geom->getNumPrimitiveSets(); ++i)
            {
                osg::PrimitiveSet* pset = geom->getPrimitiveSet(i);

                mesh.primitives.push_back(tinygltf::Primitive());
                tinygltf::Primitive& primitive = mesh.primitives.back();

                int currentMaterial = getCurrentMaterial();
                if (currentMaterial >= 0)
                {
                    // Cesium may crash if using texture without texCoords
                    // gltf_validator will report it as errors
                    // ThreeJS seems to be fine though
                    // TODO: check if the material actually has any texture in it
                    // TODO: the material should not be added if not used anywhere
                    if (texCoords.valid()) {
                        primitive.material = currentMaterial;
                    }
                }

                primitive.mode = pset->getMode();

                int a = getOrCreateAccessor(positions, pset, primitive, "POSITION");

                // record min/max for position array (required):
                tinygltf::Accessor& posacc = _model.accessors[a];
                posacc.minValues.push_back(posMin.x());
                posacc.minValues.push_back(posMin.y());
                posacc.minValues.push_back(posMin.z());
                posacc.maxValues.push_back(posMax.x());
                posacc.maxValues.push_back(posMax.y());
                posacc.maxValues.push_back(posMax.z());                

                getOrCreateAccessor(normals, pset, primitive, "NORMAL");

                getOrCreateAccessor(colors, pset, primitive, "COLOR_0");
                getOrCreateAccessor(texCoords.get(), pset, primitive, "TEXCOORD_0");
            }

            if (pushedStateSet)
            {
                popStateSet();
            }
        }
    }
};

class GLTFWriter
{
public:
    osgDB::ReaderWriter::WriteResult write(const osg::Node& node,
                                           const std::string& location,
                                           bool isBinary,
                                           const osgDB::Options* options) const
    {
        tinygltf::Model model;
        convertOSGtoGLTF(node, model);

        tinygltf::TinyGLTF writer;

        writer.WriteGltfSceneToFile(
            &model,
            location,
            true,           // embedImages
            true,           // embedBuffers
            true,           // prettyPrint
            isBinary);      // writeBinary

        return osgDB::ReaderWriter::WriteResult::FILE_SAVED;
    }

    void convertOSGtoGLTF(const osg::Node& node, tinygltf::Model& model) const
    {
        model.asset.version = "2.0";

        osg::Node& nc_node = const_cast<osg::Node&>(node); // won't change it, promise :)
        nc_node.ref();

        // GLTF uses a +X=right +y=up -z=forward coordinate system
        osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform;
        transform->setMatrix(osg::Matrixd::rotate(osg::Vec3d(0.0, 0.0, 1.0), osg::Vec3d(0.0, 1.0, 0.0)));
        transform->addChild(&nc_node);

        OSGtoGLTF converter(model);
        transform->accept(converter);

        transform->removeChild(&nc_node);
        nc_node.unref_nodelete();
    }
};

#endif // OSGEARTH_GLTF_WRITER_H
