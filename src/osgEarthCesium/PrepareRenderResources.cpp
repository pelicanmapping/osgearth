/* osgEarth - Geospatial SDK for OpenSceneGraph
* Copyright 2008-2012 Pelican Mapping
* MIT License
*/
#include "PrepareRenderResources"
#include <CesiumGltfContent/GltfUtilities.h>
#include <CesiumGltf/AccessorView.h>

#include <osg/Texture2D>
#include <osg/Geometry>
#include <osgEarth/ImageUtils>
#include <osgEarth/Lighting>
#include <osgEarth/Notify>
#include <osgEarth/Registry>
#include <osg/MatrixTransform>
#include <glm/gtc/type_ptr.hpp>

using namespace osgEarth::Cesium;

namespace
{
    #ifdef GL_R
    const GLenum redFormat = GL_R;
    #else
    const GLenum redFormat = GL_RED;
    #endif

    osg::Image* getOsgImage(CesiumGltf::ImageAsset& image)
    {
        GLenum format = GL_RGB;
        GLenum texFormat = GL_RGB8;
        GLenum type = GL_UNSIGNED_BYTE;

        switch (image.compressedPixelFormat)
        {
        case CesiumGltf::GpuCompressedPixelFormat::NONE:
            switch (image.channels)
            {
            case 1:
                format = redFormat;
                texFormat = GL_R8;
                break;
            case 2:
                format = GL_RG;
                texFormat = GL_RG8;
                break;
            case 3:
                format = GL_RGB;
                texFormat = GL_RGB8;
                break;
            case 4:
                format = GL_RGBA;
                texFormat = GL_RGBA8;
                break;
            }
            break;
        case CesiumGltf::GpuCompressedPixelFormat::ETC1_RGB:
            format = GL_RGB;
            texFormat = GL_ETC1_RGB8_OES;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::ETC2_RGBA:
            format = GL_RGBA;
            texFormat = GL_COMPRESSED_RGBA8_ETC2_EAC;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::BC1_RGB:
            format = GL_RGB;
            texFormat = GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::BC3_RGBA:
            format = GL_RGB;
            texFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::BC4_R:
            format = redFormat;
            texFormat = GL_COMPRESSED_RED_RGTC1_EXT;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::BC5_RG:
            format = GL_RG;
            texFormat = GL_COMPRESSED_RED_GREEN_RGTC2_EXT;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::BC7_RGBA:
            OE_NOTICE << "Unsuported compressed format BC7_RGBA" << std::endl;
            return nullptr;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::PVRTC1_4_RGB:
            format = GL_RGB;
            texFormat = GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::PVRTC1_4_RGBA:
            format = GL_RGBA;
            texFormat = GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::ASTC_4x4_RGBA:
            format = GL_RGBA;
            texFormat = GL_KHR_texture_compression_astc_hdr;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::PVRTC2_4_RGB:
            OE_NOTICE << "Unsuported compressed format PVRTC2_4_RGB" << std::endl;
            return nullptr;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::PVRTC2_4_RGBA:
            OE_NOTICE << "Unsuported compressed format PVRTC2_4_RGBA" << std::endl;
            return nullptr;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::ETC2_EAC_R11:
            format = redFormat;
            texFormat = GL_COMPRESSED_R11_EAC;
            break;
        case CesiumGltf::GpuCompressedPixelFormat::ETC2_EAC_RG11:
            format = GL_RG;
            texFormat = GL_COMPRESSED_RG11_EAC;
            break;
        default:
            break;
        }

        osg::Image* osgImage = new osg::Image;
        unsigned char* imgData = new unsigned char[image.pixelData.size()];
        memcpy(imgData, &image.pixelData[0], image.pixelData.size());
        osgImage->setImage(image.width, image.height, 1, texFormat, format, type, imgData, osg::Image::AllocationMode::USE_NEW_DELETE);
        return osgImage;
    }
}


LoadThreadResult::LoadThreadResult()
{
}

LoadThreadResult::~LoadThreadResult()
{    
}

MainThreadResult::MainThreadResult()
{
}

MainThreadResult::~MainThreadResult()
{
}


LoadRasterThreadResult::LoadRasterThreadResult()
{
}

LoadRasterThreadResult::~LoadRasterThreadResult()
{
}

LoadRasterMainThreadResult::LoadRasterMainThreadResult()
{
}

LoadRasterMainThreadResult::~LoadRasterMainThreadResult()
{
}


/********/
namespace {

    template<typename T>
    osg::Array* accessorViewToArray(const CesiumGltf::AccessorView<T>& accessorView)
    {
        return nullptr;
    }

    // TODO:  The rest of the accessor types.

    template<>
    osg::Array* accessorViewToArray(const CesiumGltf::AccessorView<CesiumGltf::AccessorTypes::SCALAR<uint16_t>>& accessorView)
    {
        osg::UShortArray* result = new osg::UShortArray(accessorView.size());
        for (unsigned int i = 0; i < accessorView.size(); ++i)
        {
            auto& data = accessorView[i];
            (*result)[i] = data.value[0];
        }
        return result;
    }

    template<>
    osg::Array* accessorViewToArray(const CesiumGltf::AccessorView<CesiumGltf::AccessorTypes::SCALAR<uint32_t>>& accessorView)
    {
        osg::UIntArray* result = new osg::UIntArray(accessorView.size());
        for (unsigned int i = 0; i < accessorView.size(); ++i)
        {
            auto& data = accessorView[i];
            (*result)[i] = data.value[0];
        }
        return result;
    }

    template<>
    osg::Array* accessorViewToArray(const CesiumGltf::AccessorView<CesiumGltf::AccessorTypes::SCALAR<uint8_t>>& accessorView)
    {
        osg::UByteArray* result = new osg::UByteArray(accessorView.size());
        for (unsigned int i = 0; i < accessorView.size(); ++i)
        {
            auto& data = accessorView[i];
            (*result)[i] = data.value[0];
        }
        return result;
    }

    template<>
    osg::Array* accessorViewToArray(const CesiumGltf::AccessorView<CesiumGltf::AccessorTypes::VEC3<float>>& accessorView)
    {
        osg::Vec3Array* result = new osg::Vec3Array(accessorView.size());
        for (unsigned int i = 0; i < accessorView.size(); ++i)
        {
            auto& data = accessorView[i];
            (*result)[i].set(data.value[0], data.value[1], data.value[2]);
        }
        return result;
    }

    template<>
    osg::Array* accessorViewToArray(const CesiumGltf::AccessorView<CesiumGltf::AccessorTypes::VEC2<float>>& accessorView)
    {
        osg::Vec2Array* result = new osg::Vec2Array(accessorView.size());
        for (unsigned int i = 0; i < accessorView.size(); ++i)
        {
            auto& data = accessorView[i];
            (*result)[i].set(data.value[0], data.value[1]);
        }
        return result;
    }

    template<>
    osg::Array* accessorViewToArray(const CesiumGltf::AccessorView<CesiumGltf::AccessorTypes::SCALAR<float>>& accessorView)
    {
        osg::FloatArray* result = new osg::FloatArray(accessorView.size());
        for (unsigned int i = 0; i < accessorView.size(); ++i)
        {
            auto& data = accessorView[i];
            (*result)[i] = data.value[0];
        }
        return result;
    }

    class NodeBuilder
    {
    public:
        NodeBuilder(CesiumGltf::Model* model, const glm::dmat4& transform) :
            _model(model),
            _transform(transform)
        {
            loadArrays();
            loadTextures();        
        }

        osg::Node* build()
        {
            osg::MatrixTransform* root = new osg::MatrixTransform;
            osg::Matrixd matrix;

            glm::dmat4x4 rootTransform = _transform;
            rootTransform = CesiumGltfContent::GltfUtilities::applyRtcCenter(*_model, rootTransform);
            rootTransform = CesiumGltfContent::GltfUtilities::applyGltfUpAxisTransform(*_model, rootTransform);
            matrix.set(glm::value_ptr(rootTransform));

            //matrix.set(glm::value_ptr(_transform));
            root->setMatrix(matrix);
        
            if (!_model->scenes.empty())
            {
                for (auto& scene : _model->scenes)
                {
                    for (int node : scene.nodes)
                    {
                        root->addChild(createNode(_model->nodes[node]));
                    }
                }
            }
            else
            {
                for (auto itr = _model->nodes.begin(); itr != _model->nodes.end(); ++itr)
                {
                    root->addChild(createNode(*itr));
                }
            }

            osgEarth::Registry::shaderGenerator().run(root);
            osg::Group* container = new osg::Group;
            container->addChild(root);
            return container;
        }

        osg::Node* createNode(const CesiumGltf::Node& node)
        {
            osg::MatrixTransform* root = new osg::MatrixTransform;
            if (node.matrix.size() == 16)
            {
                osg::Matrixd matrix;
                matrix.set(node.matrix.data());
                root->setMatrix(matrix);
            }


            if (root->getMatrix().isIdentity())
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

                root->setMatrix(scale * rotation * translation);
            }

            if (node.mesh >= 0)
            {
                // Build the mesh and add it.
                // TODO:  This mesh needs cached since it can be reused and referenced elsewhere.
                root->addChild(createMesh(_model->meshes[node.mesh]));
            }

            for (int child : node.children)
            {
                root->addChild(createNode(_model->nodes[child]));
            }

            return root;
        }    

        void loadArrays()
        {
            for (unsigned int i = 0; i < _model->accessors.size(); ++i)
            {
                osg::ref_ptr< osg::Array > osgArray = nullptr;
                CesiumGltf::createAccessorView(*this->_model, i, [&](const auto& accessorView) {
                    osgArray = accessorViewToArray(accessorView);
                    });

                if (osgArray)
                {
                    osgArray->setBinding(osg::Array::BIND_PER_VERTEX);
                    osgArray->setNormalize(_model->accessors[i].normalized);
                    _arrays.push_back(osgArray);
                }
                else
                {
                    _arrays.push_back(nullptr);
                }
            }
        }

        void loadTextures()
        {
            for (unsigned int i = 0; i < _model->textures.size(); ++i)
            {
                auto& texture = _model->textures[i];

                osg::Texture2D* osgTexture = new osg::Texture2D;
                osgTexture->setResizeNonPowerOfTwoHint(false);
                osgTexture->setDataVariance(osg::Object::STATIC);
                // Sampler settings
                if (texture.sampler >= 0 && texture.sampler < _model->samplers.size())
                {
                    auto& sampler = _model->samplers[texture.sampler];

                    // TODO?  Actually set filter?
                    osgTexture->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR_MIPMAP_LINEAR); //sampler.minFilter);
                    osgTexture->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR); //sampler.magFilter);
                    osgTexture->setWrap(osg::Texture::WRAP_S, (osg::Texture::WrapMode)sampler.wrapS);
                    osgTexture->setWrap(osg::Texture::WRAP_T, (osg::Texture::WrapMode)sampler.wrapT);
                    //osgTexture->setWrap(osg::Texture::WRAP_R, (osg::Texture::WrapMode)sampler.wrapR);
                }
                else
                {
                    osgTexture->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR_MIPMAP_LINEAR);
                    osgTexture->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR);
                    osgTexture->setWrap(osg::Texture::WRAP_S, (osg::Texture::WrapMode)osg::Texture::CLAMP_TO_EDGE);
                    osgTexture->setWrap(osg::Texture::WRAP_T, (osg::Texture::WrapMode)osg::Texture::CLAMP_TO_EDGE);
                }

                // Load the image
                auto image = _model->images[texture.source].pAsset;

                osg::Image* osgImage = getOsgImage(*image);

                if (osgImage)
                {
                    osgTexture->setImage(osgImage);
                }

                _textures.push_back(osgTexture);
            }
        }

        osg::Node* createMesh(const CesiumGltf::Mesh& mesh)
        {
            osg::Group* geode = new osg::Group;
            for (auto primitive : mesh.primitives)
            {
                osg::ref_ptr< osg::Geometry> geom = new osg::Geometry;
                geom->setUseDisplayList(false);
                geom->setUseVertexBufferObjects(true);

                for (auto& attribute : primitive.attributes)
                {
                    const std::string& name = attribute.first;
                    osg::Array* osgArray = _arrays[attribute.second];
                    if (name == "POSITION")
                    {
                        geom->setVertexArray(osgArray);
                    }
                    else if (name == "COLOR_0")
                    {
                        geom->setColorArray(osgArray);
                    }
                    else if (name == "NORMAL")
                    {
                        geom->setNormalArray(osgArray);
                    }
                    else if (name == "TEXCOORD_0")
                    {
                        geom->setTexCoordArray(0, osgArray);
                    }
                    else if (name == "TEXCOORD_1")
                    {
                        geom->setTexCoordArray(1, osgArray);
                    }

                    // Look for Cesium Overlay texture coordinates.
                    const std::string CESIUM_OVERLAY = "_CESIUMOVERLAY_";
                    if (osgEarth::startsWith(name, CESIUM_OVERLAY))
                    {
                        int index = std::stoi(name.substr(CESIUM_OVERLAY.length()));

                        // Not sure how many overlays or tex coords we should support.  For now we'll support 2 texture coords and two overlays so stick the 
                        // overlay texture coordinates at an index of index + 2.  Revisit this later.
                        geom->setTexCoordArray(index + 2, osgArray);
                    }
                }

                // If there is no color array just add one
                if (!geom->getColorArray())
                {
                    osg::Vec4Array* colors = new osg::Vec4Array();
                    osg::Vec3Array* verts = static_cast<osg::Vec3Array*>(geom->getVertexArray());
                    for (unsigned int i = 0; i < verts->size(); i++)
                    {
                        colors->push_back(osg::Vec4(1, 1, 1, 1));
                    }
                    geom->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
                }

                GLenum mode = GL_TRIANGLES;
                switch (primitive.mode)
                {
                case CesiumGltf::MeshPrimitive::Mode::TRIANGLES:
                    mode = GL_TRIANGLES;
                    break;
                case CesiumGltf::MeshPrimitive::Mode::TRIANGLE_FAN:
                    mode = GL_TRIANGLE_FAN;
                    break;
                case CesiumGltf::MeshPrimitive::Mode::TRIANGLE_STRIP:
                    mode = GL_TRIANGLE_STRIP;
                    break;
                case CesiumGltf::MeshPrimitive::Mode::LINES:
                    mode = GL_LINES;
                    break;
                case CesiumGltf::MeshPrimitive::Mode::LINE_LOOP:
                    mode = GL_LINES;
                    break;
                case CesiumGltf::MeshPrimitive::Mode::LINE_STRIP:
                    mode = GL_LINE_STRIP;
                    break;
                case CesiumGltf::MeshPrimitive::Mode::POINTS:
                    mode = GL_POINTS;
                    break;
                }

                if (primitive.indices >= 0)
                {
                    osg::Array* primitiveArray = _arrays[primitive.indices];                    

                    switch (primitiveArray->getType())
                    {
                    case osg::Array::UShortArrayType:
                    {
                        osg::UShortArray* indices = static_cast<osg::UShortArray*>(primitiveArray);
                        osg::DrawElementsUShort* drawElements = new osg::DrawElementsUShort(mode, indices->begin(), indices->end());
                        geom->addPrimitiveSet(drawElements);
                        break;
                    }
                    case osg::Array::UIntArrayType:
                    {
                        osg::UIntArray* indices = static_cast<osg::UIntArray*>(primitiveArray);
                        osg::DrawElementsUInt* drawElements
                            = new osg::DrawElementsUInt(mode, indices->begin(), indices->end());
                        geom->addPrimitiveSet(drawElements);
                        break;
                    }
                    case osg::Array::UByteArrayType:
                    {
                        osg::UByteArray* indices = static_cast<osg::UByteArray*>(primitiveArray);
                        // DrawElementsUByte doesn't have the constructor with iterator arguments.
                        osg::DrawElementsUByte* drawElements = new osg::DrawElementsUByte(mode, indices->size());
                        std::copy(indices->begin(), indices->end(), drawElements->begin());
                        geom->addPrimitiveSet(drawElements);
                        break;
                    }
                    }
                }
                else
                {
                    // If there are no primitives, then it is non-indexed geometry                    
                    geom->addPrimitiveSet(new osg::DrawArrays(mode, 0, geom->getVertexArray()->getNumElements()));
                }


                if (primitive.material >= 0 && primitive.material < _model->materials.size())
                {
                    osg::StateSet* stateSet = geom->getOrCreateStateSet();
                    auto& material = _model->materials[primitive.material];
                    auto pbr = material.pbrMetallicRoughness;
                    if (pbr)
                    {
                        if (pbr->baseColorFactor.size() > 0) {
                            osgEarth::MaterialGL3* material = new osgEarth::MaterialGL3();
                            osg::Vec4d color(pbr->baseColorFactor[0], pbr->baseColorFactor[1], pbr->baseColorFactor[2], pbr->baseColorFactor[3]);
                            material->setDiffuse(osg::Material::FRONT_AND_BACK, color);
                            stateSet->setAttributeAndModes(material);
                        }
                        if (pbr->baseColorTexture.has_value()) {
                            unsigned int baseColorTexture = pbr->baseColorTexture->index;
                            if (baseColorTexture >= 0 && baseColorTexture < _textures.size())
                            {
                                stateSet->setTextureAttributeAndModes(0, _textures[baseColorTexture], osg::StateAttribute::ON);
                            }
                        }
                    }
                }

                geode->addChild(geom);


            }
            return geode;
        }

        glm::dmat4 _transform;
        CesiumGltf::Model* _model;

        std::vector< osg::ref_ptr< osg::Array> > _arrays;
        std::vector< osg::ref_ptr< osg::Texture2D > > _textures;
    };
}
/********/

CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResultAndRenderResources>
PrepareRendererResources::prepareInLoadThread(
    const CesiumAsync::AsyncSystem& asyncSystem,
    Cesium3DTilesSelection::TileLoadResult&& tileLoadResult,
    const glm::dmat4& transform,
    const std::any& rendererOptions)
{
    CesiumGltf::Model* model = std::get_if<CesiumGltf::Model>(&tileLoadResult.contentKind);
    if (!model)
    {
        return asyncSystem.createResolvedFuture(
            Cesium3DTilesSelection::TileLoadResultAndRenderResources{
                std::move(tileLoadResult),
                nullptr });
    }


    NodeBuilder builder(model, transform);
    LoadThreadResult* result = new LoadThreadResult;
    result->node = builder.build();
    //result->node->setName(tileLoadResult.pCompletedRequest->url());
    return asyncSystem.createResolvedFuture(
        Cesium3DTilesSelection::TileLoadResultAndRenderResources{
            std::move(tileLoadResult),
            result });
}

void* PrepareRendererResources::prepareInMainThread(Cesium3DTilesSelection::Tile& tile, void* pLoadThreadResult)
{    
    LoadThreadResult* loadThreadResult = reinterpret_cast<LoadThreadResult*>(pLoadThreadResult);
    MainThreadResult* mainThreadResult = new MainThreadResult();
    mainThreadResult->node = loadThreadResult->node;

    loadThreadResult->node = nullptr;
    delete loadThreadResult;

    return mainThreadResult;    
}

void PrepareRendererResources::free(
    Cesium3DTilesSelection::Tile& tile,
    void* pLoadThreadResult,
    void* pMainThreadResult) noexcept
{ 
    LoadThreadResult* loadThreadResult = reinterpret_cast<LoadThreadResult*>(pLoadThreadResult);
    if (loadThreadResult)
    {
        if (loadThreadResult->node.valid())
        {
            //result->node->releaseGLObjects();
            loadThreadResult->node = nullptr;
        }
        delete loadThreadResult;
    }
 
    MainThreadResult* mainThreadResult = reinterpret_cast<MainThreadResult*>(pMainThreadResult);
    if (mainThreadResult)
    {
        if (mainThreadResult->node.valid())
        {
            mainThreadResult->node = nullptr;
        }
        delete mainThreadResult;
    }    
}

void* PrepareRendererResources::prepareRasterInLoadThread(
    CesiumGltf::ImageAsset& image,
    const std::any& rendererOptions)
{
    osg::Image* osgImage = getOsgImage(image);

    LoadRasterThreadResult* result = new LoadRasterThreadResult;
    result->image = osgImage;
    return result;
}

void* PrepareRendererResources::prepareRasterInMainThread(
    CesiumRasterOverlays::RasterOverlayTile& rasterTile,
    void* pLoadThreadResult)
{
    LoadRasterThreadResult* loadThreadResult = reinterpret_cast<LoadRasterThreadResult*>(pLoadThreadResult);

    if (!loadThreadResult)
    {
        return nullptr;
    }

    LoadRasterMainThreadResult* result = new LoadRasterMainThreadResult;
    result->image = loadThreadResult->image;
    loadThreadResult->image = nullptr;
    delete loadThreadResult;
    return result;
}

void PrepareRendererResources::freeRaster(
    const CesiumRasterOverlays::RasterOverlayTile& rasterTile,
    void* pLoadThreadResult,
    void* pMainThreadResult) noexcept
{
    LoadRasterThreadResult* loadThreadResult = reinterpret_cast<LoadRasterThreadResult*>(pLoadThreadResult);
    if (loadThreadResult)
    {
        loadThreadResult->image = nullptr;
        delete loadThreadResult;
    }

    LoadRasterMainThreadResult* loadMainThreadResult = reinterpret_cast<LoadRasterMainThreadResult*>(pMainThreadResult);
    if (loadMainThreadResult)
    {
        loadMainThreadResult->image = nullptr;
        delete loadMainThreadResult;
    }
}

const char* raster_overlay_vs = R"(
            #version 330
            out vec4 oe_raster_overlay_coords;
            uniform int oe_raster_overlay_coord;
            void raster_overlay_model(inout vec4 vertex)
            {
                switch (oe_raster_overlay_coord) {
                    case 0:
                        oe_raster_overlay_coords = gl_MultiTexCoord0;
                        break;
                    case 1:
                        oe_raster_overlay_coords = gl_MultiTexCoord1;
                        break;
                    case 2:
                        oe_raster_overlay_coords = gl_MultiTexCoord2;
                        break;
                    case 3:
                        oe_raster_overlay_coords = gl_MultiTexCoord3;
                        break;
                }
            }
        )";

const char* raster_overlay_fs = R"(
            #version 330
            in vec4 oe_raster_overlay_coords;
            uniform sampler2D oe_raster_overlay_sampler;
            uniform vec4 oe_translation_scale;
            
            void raster_overlay_color(inout vec4 color)
            {
                vec4 texel;
                vec2 trans = oe_translation_scale.xy;
                vec2 scale = oe_translation_scale.zw;

                vec2 texcoords = oe_raster_overlay_coords.xy * scale + trans;
                texcoords.t = 1.0 - texcoords.t;
                texel = texture(oe_raster_overlay_sampler, texcoords);
                color = color * texel;
            }
        )";

void PrepareRendererResources::attachRasterInMainThread(
    const Cesium3DTilesSelection::Tile& tile,
    int32_t overlayTextureCoordinateID,
    const CesiumRasterOverlays::RasterOverlayTile& rasterTile,
    void* pMainThreadRendererResources,
    const glm::dvec2& translation,
    const glm::dvec2& scale)
{
    LoadRasterMainThreadResult* loadMainThreadResult = reinterpret_cast<LoadRasterMainThreadResult*>(pMainThreadRendererResources);
    if (!loadMainThreadResult)
    {
        return;
    }

    const Cesium3DTilesSelection::TileContent& content = tile.getContent();
    const Cesium3DTilesSelection::TileRenderContent* renderContent = content.getRenderContent();

    MainThreadResult* renderResult = reinterpret_cast<MainThreadResult*>(renderContent->getRenderResources());
    if (renderResult)
    {
        int overlayUnit = overlayTextureCoordinateID + 2;
        osg::StateSet* stateset = renderResult->node->getOrCreateStateSet();
        osg::Texture2D* osgTexture = new osg::Texture2D(loadMainThreadResult->image.get());
        osgTexture->setResizeNonPowerOfTwoHint(false);
        osgTexture->setDataVariance(osg::Object::STATIC);
        osgTexture->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR_MIPMAP_LINEAR);
        osgTexture->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR);
        osgTexture->setWrap(osg::Texture::WRAP_S, (osg::Texture::WrapMode)osg::Texture::CLAMP_TO_EDGE);
        osgTexture->setWrap(osg::Texture::WRAP_T, (osg::Texture::WrapMode)osg::Texture::CLAMP_TO_EDGE);
        stateset->setTextureAttributeAndModes(overlayUnit, osgTexture, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);


        stateset->getOrCreateUniform("oe_raster_overlay_coord", osg::Uniform::INT)->set(overlayUnit);        
        stateset->getOrCreateUniform("oe_raster_overlay_sampler", osg::Uniform::SAMPLER_2D)->set(overlayUnit);

        osg::Uniform* uniform_and_scale = new osg::Uniform("oe_translation_scale", osg::Vec4f(translation.x, translation.y, scale.x, scale.y));
        stateset->addUniform(uniform_and_scale);

        VirtualProgram::getOrCreate(stateset)->setFunction("raster_overlay_color", raster_overlay_fs, VirtualProgram::LOCATION_FRAGMENT_COLORING);
        VirtualProgram::getOrCreate(stateset)->setFunction("raster_overlay_model", raster_overlay_vs, VirtualProgram::LOCATION_VERTEX_MODEL);

    }
}

void PrepareRendererResources::detachRasterInMainThread(
    const Cesium3DTilesSelection::Tile& tile,
    int32_t overlayTextureCoordinateID,
    const CesiumRasterOverlays::RasterOverlayTile& rasterTile,
    void* pMainThreadRendererResources) noexcept
{
}