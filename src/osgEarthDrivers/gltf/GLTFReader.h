/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#ifndef OSGEARTH_GLTF_READER_H
#define OSGEARTH_GLTF_READER_H

#include <osg/Node>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osg/CullFace>
#include <osg/FrontFace>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/ObjectWrapper>
#include <osgDB/Registry>
#include <osgUtil/SmoothingVisitor>
#include <osgEarth/Notify>
#include <osgEarth/NodeUtils>
#include <osgEarth/URI>
#include <osgEarth/Containers>
#include <osgEarth/Registry>
#include <osgEarth/ShaderUtils>
#include <osgEarth/InstanceBuilder>
#include <osgEarth/StateTransition>

using namespace osgEarth;
using namespace osgEarth::Util;


#undef LC
#define LC "[GLTFWriter] "

class GLTFReader
{
public:
    using TextureCache = osgEarth::Threading::Mutexed<
        std::unordered_map<std::string, osg::ref_ptr<osg::Texture2D>> >;

    struct NodeBuilder;

    static std::string ExpandFilePath(const std::string &filepath, void * userData)
    {
        const std::string& referrer = *(const std::string*)userData;
        URIContext context(referrer);
        osgEarth::URI uri(filepath, context);
        std::string path = uri.full();
        OSG_NOTICE << "ExpandFilePath: expanded " << filepath << " to " << path << std::endl;
        return path;
    }

    static bool ReadWholeFile(std::vector<unsigned char> *out, std::string *err,
        const std::string &filepath, void *)
    {
        auto result = URI(filepath).readString();
        if (result.failed())
        {
            return false;
        }

        std::string str = result.getString();
        out->resize(str.size());
        memcpy(out->data(), str.c_str(), str.size());
        return true;
    }

    static bool FileExists(const std::string &abs_filename, void *)
    {
        if (osgDB::containsServerAddress(abs_filename))
        {
            return true;
        }
        return osgDB::fileExists(abs_filename);
    }

    struct Env
    {
        Env(const std::string& loc, const osgDB::Options* opt) : referrer(loc), readOptions(opt) { }
        const std::string referrer;
        const osgDB::Options* readOptions;
    };

public:
    mutable TextureCache* _texCache;

    GLTFReader() : _texCache(NULL)
    {
        //NOP
    }

    void setTextureCache(TextureCache* cache) const
    {
        _texCache = cache;
    }

    osgDB::ReaderWriter::ReadResult read(const std::string& location,
                                         bool isBinary,
                                         const osgDB::Options* readOptions) const
    {
        std::string err, warn;
        tinygltf::Model model;
        tinygltf::TinyGLTF loader;

        tinygltf::FsCallbacks fs;
        fs.FileExists = &GLTFReader::FileExists;
        fs.ExpandFilePath = &GLTFReader::ExpandFilePath;
        fs.ReadWholeFile = &GLTFReader::ReadWholeFile;
        fs.WriteWholeFile = &tinygltf::WriteWholeFile;
        fs.user_data = (void*)&location;
        loader.SetFsCallbacks(fs);

        tinygltf::Options opt;
        opt.skip_imagery = readOptions && readOptions->getOptionString().find("gltfSkipImagery") != std::string::npos;

        if (osgDB::containsServerAddress(location))
        {
            osgEarth::ReadResult rr = osgEarth::URI(location).readString(readOptions);
            if (rr.failed())
            {
                return osgDB::ReaderWriter::ReadResult::FILE_NOT_FOUND;
            }

            std::string baseDir = osgDB::getFilePath(location);

            std::string mem = rr.getString();

            if (isBinary)
            {
                loader.LoadBinaryFromMemory(&model, &err, &warn, (const unsigned char*)mem.data(), mem.size(), baseDir, REQUIRE_VERSION, &opt);
            }
            else
            {
                loader.LoadASCIIFromString(&model, &err, &warn, mem.data(), mem.size(), baseDir, REQUIRE_VERSION, &opt);
            }
        }
        else
        {
            if (isBinary)
            {
                loader.LoadBinaryFromFile(&model, &err, &warn, location, REQUIRE_VERSION, &opt);
            }
            else
            {
                loader.LoadASCIIFromFile(&model, &err, &warn, location, REQUIRE_VERSION, &opt);
            }
        }

        if (!err.empty()) {
            OE_WARN << LC << "gltf Error loading " << location << std::endl;
            OE_WARN << LC << err << std::endl;
            return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
        }

        Env env(location, readOptions);
        return makeNodeFromModel(model, env);
    }

    osg::Node* read(const std::string& location, const std::string& inputStream, const osgDB::Options* readOptions) const
    {
        std::string err, warn;
        tinygltf::Model model;
        tinygltf::TinyGLTF loader;

        tinygltf::FsCallbacks fs;
        fs.FileExists = &GLTFReader::FileExists;
        fs.ExpandFilePath = &GLTFReader::ExpandFilePath;
        fs.ReadWholeFile = &GLTFReader::ReadWholeFile;
        fs.WriteWholeFile = &tinygltf::WriteWholeFile;
        fs.user_data = (void*)&location;
        loader.SetFsCallbacks(fs);

        tinygltf::Options opt;
        opt.skip_imagery = readOptions && readOptions->getOptionString().find("gltfSkipImagery") != std::string::npos;

        std::string decompressedData;
        const std::string* data = &inputStream;

        osg::ref_ptr<osgDB::BaseCompressor> compressor = osgDB::Registry::instance()->getObjectWrapperManager()->findCompressor("zlib");
        if (compressor.valid())
        {
            std::stringstream in_data(inputStream);
            if (compressor->decompress(in_data, decompressedData))
            {
                data = &decompressedData;
            }
        }

        std::string magic(*data, 0, 4);
        if (magic == "glTF")
        {
            loader.LoadBinaryFromMemory(&model, &err, &warn, reinterpret_cast<const unsigned char*>(data->c_str()), data->size(), "", REQUIRE_VERSION, &opt);
        }
        else
        {
            loader.LoadASCIIFromString(&model, &err, &warn, data->c_str(), data->size(), "", REQUIRE_VERSION, &opt);
        }

        if (!err.empty()) {
            OE_WARN << LC << "gltf Error loading " << location << std::endl;
            OE_WARN << LC << err << std::endl;
            return 0;
        }

        Env env(location, readOptions);
        return makeNodeFromModel(model, env);
    }


    /**
     * Node to support the OWT_State extension.
     */
    class StateTransitionNode : public osg::Group, public StateTransition
    {
    public:

        virtual std::vector< std::string > getStates()
        {
            std::vector< std::string > states;
            for (auto& s : _stateToNode)
            {
                states.push_back(s.first);
            }
            return states;
        }

        virtual void transitionToState(const std::string& state)
        {
            auto itr = _stateToNode.find(state);
            if (itr != _stateToNode.end())
            {
                osg::ref_ptr< osg::Node > node;
                itr->second.lock(node);
                if (node.valid())
                {
                    // Turn the destination node on.
                    node->setNodeMask(~0);

                    // Turn this node off
                    setNodeMask(0);
                }
            }
        }

        typedef std::map< std::string, osg::observer_ptr< osg::Node > > StateToNodeMap;
        typedef std::map< std::string, std::string > StateToNodeName;

        StateToNodeMap _stateToNode;
        StateToNodeName _stateToNodeName;
    };

    osg::Node* makeNodeFromModel(const tinygltf::Model &model, const Env& env) const
    {
        NodeBuilder builder(this, model, env);
        bool zUp = env.readOptions && env.readOptions->getOptionString().find("gltfZUp") != std::string::npos;

        // Rotate y-up to z-up if necessary
        osg::MatrixTransform* transform = new osg::MatrixTransform;
        if (!zUp)
        {
            transform->setMatrix(osg::Matrixd::rotate(osg::Vec3d(0.0, 1.0, 0.0), osg::Vec3d(0.0, 0.0, 1.0)));
        }

        for (unsigned int i = 0; i < model.scenes.size(); i++)
        {
            const tinygltf::Scene &scene = model.scenes[i];

            for (size_t j = 0; j < scene.nodes.size(); j++) {
                osg::Node* node = builder.createNode(model.nodes[scene.nodes[j]], false);
                if (node)
                {
                    transform->addChild(node);
                }
            }
        }

        // Enable backface culling on the nodes
        transform->getOrCreateStateSet()->setAttributeAndModes(new osg::CullFace(osg::CullFace::BACK), osg::StateAttribute::ON);

        // Find all the StateTransitionNodes that were created and try to establishs links between the nodes.
        osgEarth::FindNodesVisitor<StateTransitionNode> findStateTransitions;
        transform->accept(findStateTransitions);

        for (auto& st : findStateTransitions._results)
        {
            for (auto& stateToNodeName : st->_stateToNodeName)
            {
                std::string state = stateToNodeName.first;
                std::string name = stateToNodeName.second;

                // Find the named node
                osg::Node* node = findNamedNode(transform, name);
                if (node)
                {
                    st->_stateToNode[state] = node;
                }
                else
                {
                    OE_WARN << LC << "Failed to find transition state node " << state << "=" << name << std::endl;
                }
            }
        }

        return transform;
    }


    struct NodeBuilder
    {
        const GLTFReader* reader;
        const tinygltf::Model &model;
        const Env& env;
        std::vector< osg::ref_ptr< osg::Array > > arrays;

        NodeBuilder(const GLTFReader* reader_, const tinygltf::Model &model_, const Env& env_)
            : reader(reader_), model(model_), env(env_)
        {
            extractArrays(arrays);
        }

        osg::Node* createNode(const tinygltf::Node& node, bool parentReversesWinding) const
        {
            osg::MatrixTransform* mt = new osg::MatrixTransform;
            if (node.matrix.size() == 16)
            {
                osg::Matrixd mat;
                mat.set(node.matrix.data());
                mt->setMatrix(mat);
            }

            if (mt->getMatrix().isIdentity())
            {
                osg::Matrixd scale, translation, rotation;
                if (node.scale.size() == 3)
                {
                    scale = osg::Matrixd::scale(node.scale[0], node.scale[1], node.scale[2]);
                }

                if (node.rotation.size() == 4) {
                    osg::Quat quat(node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]);
                    rotation.makeRotate(quat);
                }

                if (node.translation.size() == 3) {
                    translation = osg::Matrixd::translate(node.translation[0], node.translation[1], node.translation[2]);
                }

                mt->setMatrix(scale * rotation * translation);
            }

            const osg::Matrixd& matrix = mt->getMatrix();
            const double determinant =
                matrix(0, 0) * (matrix(1, 1) * matrix(2, 2) - matrix(1, 2) * matrix(2, 1)) -
                matrix(0, 1) * (matrix(1, 0) * matrix(2, 2) - matrix(1, 2) * matrix(2, 0)) +
                matrix(0, 2) * (matrix(1, 0) * matrix(2, 1) - matrix(1, 1) * matrix(2, 0));
            const bool localReversesWinding = determinant < 0.0;
            const bool reversesWinding = parentReversesWinding != localReversesWinding;

            // glTF permits transforms with a negative determinant. They mirror
            // the geometry, so compensate for the resulting winding reversal.
            // Set the counter-clockwise state as well when a second mirrored
            // transform restores the original winding.
            if (localReversesWinding)
            {
                mt->getOrCreateStateSet()->setAttribute(
                    new osg::FrontFace(reversesWinding ?
                        osg::FrontFace::CLOCKWISE : osg::FrontFace::COUNTER_CLOCKWISE));
            }


            // todo transformation
            if (node.mesh >= 0)
            {
                osg::Group* meshNode = nullptr;
                if (node.extensions.find("EXT_mesh_gpu_instancing") != node.extensions.end())
                {
                    meshNode = makeMesh(model.meshes[node.mesh], true);
                    makeInstancedMeshNode(node, meshNode);
                }
                else
                {
                    meshNode = makeMesh(model.meshes[node.mesh], false);
                }
                mt->addChild(meshNode);
            }

            // Load any children.
            for (unsigned int i = 0; i < node.children.size(); i++)
            {
                osg::Node* child = createNode(model.nodes[node.children[i]], reversesWinding);
                if (child)
                {
                    mt->addChild(child);
                }
            }

            osg::Node* top = mt;

            // If we have an OWT_state extension setup all the state names
            if (node.extensions.find("OWT_state") != node.extensions.end())
            {
                StateTransitionNode* st = new StateTransitionNode;
                st->addChild(mt);

                auto ext = node.extensions.find("OWT_state")->second;
                for (auto& key : ext.Keys())
                {
                    std::string value = ext.Get(key).Get<std::string>();
                    st->_stateToNodeName[key] = value;
                }
                top = st;
            }

            top->setName(node.name);

            return top;
        }

        osg::Texture2D* makeTextureFromModel(const tinygltf::Texture& texture) const

        {
            const tinygltf::Image& image = model.images[texture.source];
            bool imageEmbedded =
                tinygltf::IsDataURI(image.uri) ||
                image.image.size() > 0;

            osgEarth::URI imageURI(image.uri, env.referrer);

            osg::ref_ptr<osg::Texture2D> tex;

            //OE_DEBUG << "New Texture: " << imageURI.full() << ", embedded=" << imageEmbedded << std::endl;

            // First load the image
            osg::ref_ptr<osg::Image> img;

            if (image.image.size() > 0)
            {
                GLenum format = GL_RGB, texFormat = GL_RGB8;
                if (image.component == 4) format = GL_RGBA, texFormat = GL_RGBA8;

                img = new osg::Image();
                //OE_NOTICE << "Loading image of size " << image.width << "x" << image.height << " components = " << image.component << " totalSize=" << image.image.size() << std::endl;
                unsigned char *imgData = new unsigned char[image.image.size()];
                memcpy(imgData, &image.image[0], image.image.size());
                img->setImage(image.width, image.height, 1, texFormat, format, GL_UNSIGNED_BYTE, imgData, osg::Image::AllocationMode::USE_NEW_DELETE);
            }

            else if (!imageEmbedded) // load from URI
            {
                osgEarth::ReadResult rr = imageURI.readImage(env.readOptions);
                if(rr.succeeded())
                {
                    img = rr.releaseImage();
                    if (img.valid())
                    {
                        img->flipVertical();
                    }
                }
            }

            // If the image loaded OK, create the texture
            if (img.valid())
            {
                if(img->getPixelFormat() == GL_RGB)
                    img->setInternalTextureFormat(GL_RGB8);
                else if (img->getPixelFormat() == GL_RGBA)
                    img->setInternalTextureFormat(GL_RGBA8);

                tex = new osg::Texture2D(img.get());
                //tex->setUnRefImageDataAfterApply(imageEmbedded);
                tex->setResizeNonPowerOfTwoHint(false);
                tex->setDataVariance(osg::Object::STATIC);

                if (texture.sampler >= 0 && texture.sampler < model.samplers.size())
                {
                    const tinygltf::Sampler& sampler = model.samplers[texture.sampler];
                    //tex->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)sampler.minFilter);
                    //tex->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)sampler.magFilter);
                    tex->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR_MIPMAP_LINEAR); //sampler.minFilter);
                    tex->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR); //sampler.magFilter);
                    tex->setWrap(osg::Texture::WRAP_S, (osg::Texture::WrapMode)sampler.wrapS);
                    tex->setWrap(osg::Texture::WRAP_T, (osg::Texture::WrapMode)sampler.wrapT);
                    tex->setWrap(osg::Texture::WRAP_R, (osg::Texture::WrapMode)sampler.wrapR);
                }
                else
                {
                    tex->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR_MIPMAP_LINEAR);
                    tex->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR);
                    tex->setWrap(osg::Texture::WRAP_S, (osg::Texture::WrapMode)osg::Texture::CLAMP_TO_EDGE);
                    tex->setWrap(osg::Texture::WRAP_T, (osg::Texture::WrapMode)osg::Texture::CLAMP_TO_EDGE);
                }
            }
            return tex.release();
        }

        osg::Group* makeMesh(const tinygltf::Mesh& mesh, bool prepInstancing) const
        {
            osg::Group *group = new osg::Group;

            //OE_DEBUG << "Drawing " << mesh.primitives.size() << " primitives in mesh" << std::endl;

            for (size_t i = 0; i < mesh.primitives.size(); i++) {

                //OE_DEBUG << " Processing primitive " << i << std::endl;
                const tinygltf::Primitive &primitive = mesh.primitives[i];
                if (primitive.indices < 0)
                {
                    // Hmm, should delete group here
                    return 0;
                }

                osg::ref_ptr< osg::Geometry > geom;
                if (prepInstancing)
                {
                    geom = osgEarth::InstanceBuilder::createGeometry();
                }
                else
                {
                    geom = new osg::Geometry;
                }
                geom->setName(typeid(*this).name());
                geom->setUseVertexBufferObjects(true);

                osg::Geode* geode = new osg::Geode;
                geode->addDrawable(geom);
                group->addChild(geode);

                // The base color factor of the material
                osg::Vec4 baseColorFactor(1.0f, 1.0f, 1.0f, 1.0f);

                if (primitive.material >= 0 && primitive.material < model.materials.size())
                {
                    const tinygltf::Material& material = model.materials[primitive.material];

                    if (material.doubleSided)
                    {
                        geom->getOrCreateStateSet()->setMode(
                            GL_CULL_FACE, osg::StateAttribute::OFF);
                    }

                    /*
                      OSG_NOTICE << "extCommonValues=" << material.extCommonValues.size() << std::endl;
                      for (ParameterMap::iterator paramItr = material.extCommonValues.begin(); paramItr != material.extCommonValues.end(); ++paramItr)
                      {
                      OSG_NOTICE << paramItr->first << "=" << paramItr->second.string_value << std::endl;
                      }
                    */

                    //OE_DEBUG << "additionalValues=" << material.additionalValues.size() << std::endl;
                    for (tinygltf::ParameterMap::const_iterator paramItr = material.additionalValues.begin(); paramItr != material.additionalValues.end(); ++paramItr)
                    {
                        //OE_DEBUG << "    " << paramItr->first << "=" << paramItr->second.string_value << std::endl;
                    }

                    //OSG_NOTICE << "values=" << material.values.size() << std::endl;
                    for (tinygltf::ParameterMap::const_iterator paramItr = material.values.begin(); paramItr != material.values.end(); ++paramItr)
                    {
                        if (paramItr->first == "baseColorFactor")
                        {
                            tinygltf::ColorValue color = paramItr->second.ColorFactor();
                            baseColorFactor = osg::Vec4(color[0], color[1], color[2], color[3]);
                        }
                        else
                        {
                            //OE_DEBUG << "    " << paramItr->first << "=" << paramItr->second.string_value << std::endl;
                        }

                    }
                    /*
                      OSG_NOTICE << "extPBRValues=" << material.extPBRValues.size() << std::endl;
                      for (ParameterMap::iterator paramItr = material.extPBRValues.begin(); paramItr != material.extPBRValues.end(); ++paramItr)
                      {
                      OSG_NOTICE << paramItr->first << "=" << paramItr->second.string_value << std::endl;
                      }
                    */

                    for (tinygltf::ParameterMap::const_iterator paramItr = material.values.begin(); paramItr != material.values.end(); ++paramItr)
                    {
                        if (paramItr->first == "baseColorTexture")
                        {
                            std::map< std::string, double>::const_iterator i = paramItr->second.json_double_value.find("index");
                            if (i != paramItr->second.json_double_value.end())
                            {
                                int index = i->second;
                                const tinygltf::Texture& texture = model.textures[index];
                                const tinygltf::Image& image = model.images[texture.source];
                                // don't cache embedded textures!
                                bool imageEmbedded =
                                    tinygltf::IsDataURI(image.uri) ||
                                    image.image.size() > 0;
                                osgEarth::URI imageURI(image.uri, env.referrer);
                                osg::ref_ptr<osg::Texture2D> tex;
                                bool cachedTex = false;
                                TextureCache* texCache = reader->_texCache;
                                if (!imageEmbedded && texCache)
                                {
                                    std::lock_guard<std::mutex> lock(texCache->mutex());
                                    auto texItr = texCache->find(imageURI.full());
                                    if (texItr != texCache->end())
                                    {
                                        tex = texItr->second;
                                        cachedTex = true;
                                    }
                                }

                                if (!tex.valid())
                                {
                                    tex = makeTextureFromModel(texture);
                                }

                                if (tex.valid())
                                {
                                    if (!imageEmbedded && texCache && !cachedTex)
                                    {
                                        std::lock_guard<std::mutex> lock(texCache->mutex());
                                        auto insResult = texCache->insert(TextureCache::value_type(imageURI.full(), tex));
                                        if (insResult.second)
                                        {
                                            // Some other loader thread beat us in the cache
                                            tex = insResult.first->second;
                                        }
                                    }
                                    //geom->getOrCreateStateSet()->setTextureAttributeAndModes(0, tex.get());
                                    geom->getOrCreateStateSet()->setTextureAttribute(0, tex.get());
                                }

                                if (material.alphaMode != "OPAQUE")
                                {
                                    if (material.alphaMode == "BLEND")
                                    {
                                        geom->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
                                        geom->getOrCreateStateSet()->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                                        osgEarth::Util::DiscardAlphaFragments().install(geom->getOrCreateStateSet(), 0.15);
                                    }
                                    else if (material.alphaMode == "MASK")
                                    {
                                        geom->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
                                        geom->getOrCreateStateSet()->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                                        osgEarth::Util::DiscardAlphaFragments().install(geom->getOrCreateStateSet(), material.alphaCutoff);
                                    }
                                }
                            }
                        }
                    }
                }

                std::map<std::string, int>::const_iterator it(primitive.attributes.begin());
                std::map<std::string, int>::const_iterator itEnd(
                    primitive.attributes.end());

                for (; it != itEnd; it++)
                {
                    const tinygltf::Accessor &accessor = model.accessors[it->second];

                    if (it->first.compare("POSITION") == 0)
                    {
                        geom->setVertexArray(arrays[it->second].get());
                    }
                    else if (it->first.compare("NORMAL") == 0)
                    {
                        geom->setNormalArray(arrays[it->second].get());
                    }
                    else if (it->first.compare("TEXCOORD_0") == 0)
                    {
                        geom->setTexCoordArray(0, arrays[it->second].get());
                    }
                    else if (it->first.compare("TEXCOORD_1") == 0)
                    {
                        geom->setTexCoordArray(1, arrays[it->second].get());
                    }
                    else if (it->first.compare("COLOR_0") == 0)
                    {
                        // TODO:  Multipy by the baseColorFactor here?
                        //OE_DEBUG << "Setting color array " << arrays[it->second].get() << std::endl;
                        geom->setColorArray(arrays[it->second].get());
                    }
                    else
                    {
                        //OE_DEBUG << "Skipping array " << it->first << std::endl;
                    }
                }

                // If there is no color array just add one that has the base color factor in it.
                if (!geom->getColorArray())
                {
                    osg::Vec4Array* colors = new osg::Vec4Array();
                    osg::Vec3Array* verts = static_cast<osg::Vec3Array*>(geom->getVertexArray());
                    for (unsigned int i = 0; i < verts->size(); i++)
                    {
                        colors->push_back(baseColorFactor);
                    }
                    geom->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
                }

                int mode = -1;
                if (primitive.mode == TINYGLTF_MODE_TRIANGLES) {
                    mode = GL_TRIANGLES;
                }
                else if (primitive.mode == TINYGLTF_MODE_TRIANGLE_STRIP) {
                    mode = GL_TRIANGLE_STRIP;
                }
                else if (primitive.mode == TINYGLTF_MODE_TRIANGLE_FAN) {
                    mode = GL_TRIANGLE_FAN;
                }
                else if (primitive.mode == TINYGLTF_MODE_POINTS) {
                    mode = GL_POINTS;
                }
                else if (primitive.mode == TINYGLTF_MODE_LINE) {
                    mode = GL_LINES;
                }
                else if (primitive.mode == TINYGLTF_MODE_LINE_LOOP) {
                    mode = GL_LINE_LOOP;
                }

                if (primitive.indices < 0)
                {
                    osg::Array* vertices = geom->getVertexArray();
                    if (vertices)
                    {
                        osg::DrawArrays *drawArrays
                            = new osg::DrawArrays(mode, 0, vertices->getNumElements());
                        geom->addPrimitiveSet(drawArrays);
                    }
                    // Otherwise we can't draw anything!
                }
                else
                {
                    const tinygltf::Accessor &indexAccessor = model.accessors[primitive.indices];

                    if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                    {
                        osg::UShortArray* indices = static_cast<osg::UShortArray*>(arrays[primitive.indices].get());
                        osg::DrawElementsUShort* drawElements
                            = new osg::DrawElementsUShort(mode, indices->begin(), indices->end());
                        geom->addPrimitiveSet(drawElements);
                    }
                    else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
                    {
                        osg::UIntArray* indices = static_cast<osg::UIntArray*>(arrays[primitive.indices].get());
                        osg::DrawElementsUInt* drawElements
                            = new osg::DrawElementsUInt(mode, indices->begin(), indices->end());
                        geom->addPrimitiveSet(drawElements);
                    }
                    else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
                    {
                        osg::UByteArray* indices = static_cast<osg::UByteArray*>(arrays[primitive.indices].get());
                        // Sigh, DrawElementsUByte doesn't have the constructor with iterator arguments.
                        osg::DrawElementsUByte* drawElements = new osg::DrawElementsUByte(mode, indexAccessor.count);
                        std::copy(indices->begin(), indices->end(), drawElements->begin());
                        geom->addPrimitiveSet(drawElements);
                    }
                    else
                    {
                        OE_WARN << LC << "primitive indices are not unsigned.\n";
                    }
                }

                if (!env.readOptions || env.readOptions->getOptionString().find("gltfSkipNormals") == std::string::npos)
                {
                    // Generate normals automatically if we're not given any in the file itself.
                    if (!geom->getNormalArray())
                    {
                        osgUtil::SmoothingVisitor sv;
                        geode->accept(sv);
                    }
                }

                osgEarth::Registry::shaderGenerator().run(geom.get());
            }

            return group;
        }

        // Parameterize the creation of OSG arrays from glTF
        // accessors. It's a bit gratuitous to make ComponentType and
        // AccessorType template parameters. The thought was that the
        // memcpy could be optimized if these were constants in the
        // copyData() function, but that's debatable.

        template<typename OSGArray, int ComponentType, int AccessorType>
        class ArrayBuilder
        {
        public:
            static OSGArray* makeArray(unsigned int size)
            {
                return new OSGArray(size);
            }
            static void copyData(OSGArray* dest, const unsigned char* src, size_t viewOffset,
                                 size_t byteStride,  size_t accessorOffset, size_t count)
            {
                int32_t componentSize = tinygltf::GetComponentSizeInBytes(ComponentType);
                int32_t numComponents = tinygltf::GetNumComponentsInType(AccessorType);
                if (byteStride == 0)
                {
                    memcpy(&(*dest)[0], src + accessorOffset + viewOffset, componentSize * numComponents * count);
                }
                else
                {
                    const unsigned char* ptr = src + accessorOffset + viewOffset;
                    for (int i = 0; i < count; ++i, ptr += byteStride)
                    {
                        memcpy(&(*dest)[i], ptr, componentSize * numComponents);
                    }
                }
            }
            static void copyData(OSGArray* dest, const tinygltf::Buffer& buffer, const tinygltf::BufferView& bufferView,
                                 const tinygltf::Accessor& accessor)
            {
                copyData(dest, &buffer.data.at(0), bufferView.byteOffset,
                         bufferView.byteStride, accessor.byteOffset, accessor.count);
            }
            static OSGArray* makeArray(const tinygltf::Buffer& buffer, const tinygltf::BufferView& bufferView,
                                       const tinygltf::Accessor& accessor)
            {
                OSGArray* result = new OSGArray(accessor.count);
                copyData(result, buffer, bufferView, accessor);
                return result;
            }
        };

        // Take all of the accessors and turn them into arrays
        void extractArrays(std::vector<osg::ref_ptr<osg::Array>> &arrays) const
        {
            for (unsigned int i = 0; i < model.accessors.size(); i++)
            {
                const tinygltf::Accessor& accessor = model.accessors[i];
                const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
                const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
                osg::ref_ptr< osg::Array > osgArray;

                switch (accessor.componentType)
                {
                case TINYGLTF_COMPONENT_TYPE_BYTE:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::ByteArray,
                                                TINYGLTF_COMPONENT_TYPE_BYTE,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2bArray,
                                                TINYGLTF_COMPONENT_TYPE_BYTE,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3bArray,
                                                TINYGLTF_COMPONENT_TYPE_BYTE,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4bArray,
                                                TINYGLTF_COMPONENT_TYPE_BYTE,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::UByteArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2ubArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3ubArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4ubArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_SHORT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::ShortArray,
                                                TINYGLTF_COMPONENT_TYPE_SHORT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2sArray,
                                                TINYGLTF_COMPONENT_TYPE_SHORT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3sArray,
                                                TINYGLTF_COMPONENT_TYPE_SHORT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4sArray,
                                                TINYGLTF_COMPONENT_TYPE_SHORT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::UShortArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2usArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3usArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4usArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_INT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::IntArray,
                                                TINYGLTF_COMPONENT_TYPE_INT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2uiArray,
                                                TINYGLTF_COMPONENT_TYPE_INT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3uiArray,
                                                TINYGLTF_COMPONENT_TYPE_INT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4uiArray,
                                                TINYGLTF_COMPONENT_TYPE_INT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::UIntArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2iArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3iArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4iArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_FLOAT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::FloatArray,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2Array,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3Array,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4Array,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                default:
                    break;
                }
                if (osgArray.valid())
                {
                    osgArray->setBinding(osg::Array::BIND_PER_VERTEX);
                    osgArray->setNormalize(accessor.normalized);
                }
                else
                {
                    OSG_DEBUG << "Adding null array for " << i << std::endl;
                }
                arrays.push_back(osgArray);
            }
        }

        static bool null(const tinygltf::Value& val)
        {
            return val.Type() == tinygltf::NULL_TYPE;
        }

        void makeInstancedMeshNode(const tinygltf::Node& node, osg::Group* meshGroup) const
        {
            auto itr = node.extensions.find("EXT_mesh_gpu_instancing");
            if (itr == node.extensions.end() || !itr->second.IsObject())
                return;
            OE_NOTICE << "Found instance " << std::endl;
            auto& extObj = itr->second;
            auto& attributes = extObj.Get("attributes");
            if (null(attributes))
                return;
            osgEarth::InstanceBuilder builder;
            auto& translations = attributes.Get("TRANSLATION");
            auto& rotations = attributes.Get("ROTATION");
            auto& scales = attributes.Get("SCALE");
            if (!null(translations) && translations.IsInt())
            {
                osg::Vec3Array* array = dynamic_cast<osg::Vec3Array*>(arrays[translations.Get<int>()].get());
                if (array)
                {
                    builder.setPositions(array);
                }
            }
            if (!null(rotations) && rotations.IsInt())
            {
                osg::Vec4Array* array = dynamic_cast<osg::Vec4Array*>(arrays[rotations.Get<int>()].get());
                if (array)
                {
                    builder.setRotations(array);
                }
            }
            if (!null(scales) && scales.IsInt())
            {
                osg::Vec3Array* array = dynamic_cast<osg::Vec3Array*>(arrays[scales.Get<int>()].get());
                if (array)
                {
                    builder.setScales(array);
                }
            }
            for (unsigned int i = 0; i < meshGroup->getNumChildren(); ++i)
            {
                osg::Geometry* geom = dynamic_cast<osg::Geometry*>(meshGroup->getChild(i));
                if (geom)
                {
                    builder.installInstancing(geom);
                }
            }

        }
    };
};

#endif // OSGEARTH_GLTF_READER_H
