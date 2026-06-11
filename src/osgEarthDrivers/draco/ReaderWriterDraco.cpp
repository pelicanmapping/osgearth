/* osgEarth
* Copyright 2020 Pelican Mapping
* MIT License
*/
#include <osg/Notify>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <draco/mesh/mesh.h>
#include <draco/compression/encode.h>
#include <draco/compression/decode.h>

#include <osgEarth/NodeUtils>

using namespace draco;

draco::DataType dracoDataType(GLenum glType)
{
    switch (glType)
    {
    case GL_BYTE:
        return DT_INT8;
    case GL_SHORT:
        return DT_INT16;
    case GL_INT:
        return DT_INT32;
    case GL_UNSIGNED_BYTE:
        return DT_UINT8;
    case GL_UNSIGNED_SHORT:
        return DT_UINT16;
    case GL_UNSIGNED_INT:
        return DT_UINT32;
    case GL_FLOAT:
        return DT_FLOAT32;
    case GL_DOUBLE:
        return DT_FLOAT64;
    default:
        return DT_INVALID;
    }    
}

osg::Array* dracoAttributeToOsgArray(draco::Mesh* mesh, const draco::PointAttribute* attribute)
{
    osg::Array* outArray = nullptr;

    if (attribute->data_type() == DT_FLOAT32)
    {
        if (attribute->num_components() == 1)      outArray = new osg::FloatArray(mesh->num_points());
        else if (attribute->num_components() == 2) outArray = new osg::Vec2Array(mesh->num_points());
        else if (attribute->num_components() == 3) outArray = new osg::Vec3Array(mesh->num_points());
        else if (attribute->num_components() == 4) outArray = new osg::Vec4Array(mesh->num_points());
    }
    else if (attribute->data_type() == DT_FLOAT64)
    {
        if (attribute->num_components() == 1)      outArray = new osg::DoubleArray(mesh->num_points());
        else if (attribute->num_components() == 2) outArray = new osg::Vec2dArray(mesh->num_points());
        else if (attribute->num_components() == 3) outArray = new osg::Vec3dArray(mesh->num_points());
        else if (attribute->num_components() == 4) outArray = new osg::Vec4dArray(mesh->num_points());
    }
    else if (attribute->data_type() == DT_UINT8)
    {
        if (attribute->num_components() == 1)      outArray = new osg::UByteArray(mesh->num_points());
        else if (attribute->num_components() == 2) outArray = new osg::Vec2ubArray(mesh->num_points());
        else if (attribute->num_components() == 3) outArray = new osg::Vec3ubArray(mesh->num_points());
        else if (attribute->num_components() == 4) outArray = new osg::Vec4ubArray(mesh->num_points());
    }
    else if (attribute->data_type() == DT_INT8)
    {
        if (attribute->num_components() == 1)      outArray = new osg::ByteArray(mesh->num_points());
        else if (attribute->num_components() == 2) outArray = new osg::Vec2bArray(mesh->num_points());
        else if (attribute->num_components() == 3) outArray = new osg::Vec3bArray(mesh->num_points());
        else if (attribute->num_components() == 4) outArray = new osg::Vec4bArray(mesh->num_points());
    }
    else if (attribute->data_type() == DT_UINT16)
    {
        if (attribute->num_components() == 1)      outArray = new osg::UShortArray(mesh->num_points());
        else if (attribute->num_components() == 2) outArray = new osg::Vec2usArray(mesh->num_points());
        else if (attribute->num_components() == 3) outArray = new osg::Vec3usArray(mesh->num_points());
        else if (attribute->num_components() == 4) outArray = new osg::Vec4usArray(mesh->num_points());
    }
    else if (attribute->data_type() == DT_INT16)
    {
        if (attribute->num_components() == 1)      outArray = new osg::ShortArray(mesh->num_points());
        else if (attribute->num_components() == 2) outArray = new osg::Vec2sArray(mesh->num_points());
        else if (attribute->num_components() == 3) outArray = new osg::Vec3sArray(mesh->num_points());
        else if (attribute->num_components() == 4) outArray = new osg::Vec4sArray(mesh->num_points());
    }
    else if (attribute->data_type() == DT_UINT32)
    {
        if (attribute->num_components() == 1)      outArray = new osg::UIntArray(mesh->num_points());
        else if (attribute->num_components() == 2) outArray = new osg::Vec2uiArray(mesh->num_points());
        else if (attribute->num_components() == 3) outArray = new osg::Vec3uiArray(mesh->num_points());
        else if (attribute->num_components() == 4) outArray = new osg::Vec4uiArray(mesh->num_points());
    }
    else if (attribute->data_type() == DT_INT32)
    {
        if (attribute->num_components() == 1)      outArray = new osg::IntArray(mesh->num_points());
        else if (attribute->num_components() == 2) outArray = new osg::Vec2iArray(mesh->num_points());
        else if (attribute->num_components() == 3) outArray = new osg::Vec3iArray(mesh->num_points());
        else if (attribute->num_components() == 4) outArray = new osg::Vec4iArray(mesh->num_points());
    }

    if (outArray)
    {
        outArray->setBinding(osg::Array::BIND_PER_VERTEX);

        unsigned char* data = new unsigned char[attribute->byte_stride()];
        for (draco::PointIndex i(0); i < mesh->num_points(); ++i) {
            const draco::AttributeValueIndex val_index = attribute->mapped_index(i);
            attribute->GetValue(val_index, data);
            GLvoid* dst = const_cast<GLvoid*>(outArray->getDataPointer(i.value()));
            memcpy(dst, data, attribute->byte_stride());
        }
        delete []data;
    }
    else
    {
        OSG_WARN << "Unsupported array type data_type=" << attribute->data_type() << " num_components=" << attribute->num_components() << std::endl;
    }

    return outArray;
}

bool dracoToGeometry(osg::Geometry* geometry, const char* data, unsigned int size)
{
    draco::DecoderBuffer buffer;
    buffer.Init(data, size);

    auto type_statusor = draco::Decoder::GetEncodedGeometryType(&buffer);
    if (!type_statusor.ok()) {
        OSG_WARN << "Error " << type_statusor.status() << std::endl;
        return false;
    }
    const draco::EncodedGeometryType geom_type = type_statusor.value();
    if (geom_type == draco::TRIANGULAR_MESH)
    {
        draco::Decoder decoder;
        auto statusor = decoder.DecodeMeshFromBuffer(&buffer);
        if (!statusor.ok()) {
            OSG_WARN << "Error decoding mesh " << statusor.status() << std::endl;
            return false;
        }

        std::unique_ptr<draco::Mesh> mesh = std::move(statusor).value();

        osg::DrawElementsUInt* de = new osg::DrawElementsUInt(GL_TRIANGLES, mesh->num_faces() * 3);
        unsigned int index = 0;
        for (draco::FaceIndex f(0); f < mesh->num_faces(); ++f) {
            const draco::Mesh::Face& face = mesh->face(f);
            (*de)[index++] = face[0].value();
            (*de)[index++] = face[1].value();
            (*de)[index++] = face[2].value();
        }
        geometry->addPrimitiveSet(de);

        auto positionAttribute = mesh->GetNamedAttribute(draco::GeometryAttribute::POSITION);
        if (positionAttribute)
        {
            geometry->setVertexArray(dracoAttributeToOsgArray(mesh.get(), positionAttribute));
        }

        auto normalAttribute = mesh->GetNamedAttribute(draco::GeometryAttribute::NORMAL);
        if (normalAttribute)
        {
            geometry->setNormalArray(dracoAttributeToOsgArray(mesh.get(), normalAttribute));
        }

        auto colorAttribute = mesh->GetNamedAttribute(draco::GeometryAttribute::COLOR);
        if (colorAttribute)
        {
            geometry->setColorArray(dracoAttributeToOsgArray(mesh.get(), colorAttribute));
        }

        auto texCoordAttribute = mesh->GetNamedAttribute(draco::GeometryAttribute::TEX_COORD);
        if (texCoordAttribute)
        {
            geometry->setTexCoordArray(0, dracoAttributeToOsgArray(mesh.get(), texCoordAttribute));
        }
        return true;
    }
    return false;
}

int addGeometryAttribute(draco::Mesh* mesh, draco::GeometryAttribute::Type type, const osg::Array* array)
{
    draco::DataType dt = dracoDataType(array->getDataType());
    GeometryAttribute attr;
    attr.Init(type, nullptr, array->getDataSize(), dt, false, DataTypeLength(dt) * array->getDataSize(), 0);
    return mesh->AddAttribute(attr, true, array->getNumElements());
}

void copyArrayToMeshAttribute(draco::Mesh* mesh, int id, const osg::Array* array)
{
    for (unsigned int i = 0; i < array->getNumElements(); ++i)
    {
        mesh->attribute(id)->SetAttributeValue(AttributeValueIndex(i), array->getDataPointer(i));
    }
}

std::unique_ptr< draco::Mesh > geometryToDraco(osg::Geometry* geometry, bool stripData = false)
{
    std::unique_ptr< draco::Mesh > mesh = std::unique_ptr<draco::Mesh>(new draco::Mesh());

    bool foundTriangles = false;
    unsigned int numTriangles = 0;
    for (unsigned int i = 0; i < geometry->getNumPrimitiveSets(); ++i)
    {
        const osg::DrawElements* de = dynamic_cast<const osg::DrawElements*>(geometry->getPrimitiveSet(i));
        if (de && de->getType() == GL_TRIANGLES && de->getNumInstances() == 0)
        {
            foundTriangles = true;
            mesh->SetNumFaces(de->getNumIndices() / 3);

            FaceIndex faceIndex(0);
            for (unsigned int j = 0; j < de->getNumIndices(); j += 3)
            {
                mesh->SetFace(faceIndex, { PointIndex(de->index(j)),
                                           PointIndex(de->index(j + 1)),
                                           PointIndex(de->index(j + 2)) });
                ++faceIndex;
            }
            if (stripData)
            {
                geometry->removePrimitiveSet(i);
            }
            break;
        }
    }

    if (!foundTriangles)
    {
        mesh = nullptr;
        return mesh;
    }
    
    const osg::Array* verts = geometry->getVertexArray();
    mesh->set_num_points(verts->getNumElements());

    int positionAttributeID = -1;    
    if (verts)
    {
        positionAttributeID = addGeometryAttribute(mesh.get(), GeometryAttribute::POSITION, verts);
    }

    const osg::Array* normals = geometry->getNormalArray();
    int normalAttributeID = -1;
    bool encodeNormals = false;
    if (normals && normals->getBinding() == osg::Array::BIND_PER_VERTEX)
    {
        encodeNormals = true;
        normalAttributeID = addGeometryAttribute(mesh.get(), GeometryAttribute::NORMAL, normals);
    }

    const osg::Array* colors = geometry->getColorArray();
    int colorAttributeID = -1;
    bool encodeColors = false;
    if (colors && colors->getBinding() == osg::Array::BIND_PER_VERTEX)
    {     
        encodeColors = true;
        colorAttributeID = addGeometryAttribute(mesh.get(), GeometryAttribute::COLOR, colors);
    }

    const osg::Array* texCoords = geometry->getTexCoordArray(0);
    int texCoordAttributeID = -1;
    bool encodeTexCoords = false;
    if (texCoords && texCoords->getBinding() == osg::Array::BIND_PER_VERTEX)
    {
        encodeTexCoords = true;
        texCoordAttributeID = addGeometryAttribute(mesh.get(), GeometryAttribute::TEX_COORD, texCoords);
    }

    {
        copyArrayToMeshAttribute(mesh.get(), positionAttributeID, verts);
        if (stripData)
        {
            geometry->setVertexArray(nullptr);
        }
    }

    if (encodeNormals)
    {
        copyArrayToMeshAttribute(mesh.get(), normalAttributeID, normals);
        if (stripData)
        {
            geometry->setNormalArray(nullptr);
        }
    }

    if (encodeTexCoords)
    {
        copyArrayToMeshAttribute(mesh.get(), texCoordAttributeID, texCoords);
        if (stripData)
        {
            geometry->setTexCoordArray(0, nullptr);
        }
    }    

    if (encodeColors)
    {            
        copyArrayToMeshAttribute(mesh.get(), colorAttributeID, colors);
        if (stripData)
        {
            geometry->setColorArray(nullptr);
        }
    }    

    return mesh;
}



void encodeMesh(Mesh* mesh, std::ostream& out, const osgDB::Options* options)
{
    int pos_quantization_bits = 11;
    int tex_coords_quantization_bits = 10;
    int normals_quantization_bits = 8;    
    int compression_level = 7;

    if (options) {
        std::istringstream iss(options->getOptionString());
        std::string opt;
        while (iss >> opt) {
            if (opt == "DRACO_COMPRESSION_LEVEL") {                
                iss >> compression_level;             
            }

            if (opt == "DRACO_POS_BITS") {
                iss >> pos_quantization_bits;
            }

            if (opt == "DRACO_TEXCOORD_BITS") {
                iss >> tex_coords_quantization_bits;
            }

            if (opt == "DRACO_NORMAL_BITS") {
                iss >> normals_quantization_bits;
            }
        }
    }

    const int speed = 10 - compression_level;
    draco::Encoder encoder;    
    encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, pos_quantization_bits);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, normals_quantization_bits);    
    encoder.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, tex_coords_quantization_bits);    

    // Check for a non 2d texture coordinate and disable the predicator as it is causing an issue with decompression
    auto texCoordAttribute = mesh->GetNamedAttribute(draco::GeometryAttribute::TEX_COORD);
    if (texCoordAttribute && texCoordAttribute->num_components() != 2)
    {
        encoder.SetAttributePredictionScheme(draco::GeometryAttribute::Type::TEX_COORD, PREDICTION_NONE);
    }
    encoder.SetSpeedOptions(speed, speed);

    EncoderBuffer buffer;
    Status status = encoder.EncodeMeshToBuffer(*mesh, &buffer);
    out.write(buffer.data(), buffer.size());    
}

class DracoCompressGeometryVisitor : public osg::NodeVisitor
{
public:
    DracoCompressGeometryVisitor(const osgDB::Options* options):
        osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
        _options(options)
    {
    }

    void apply(osg::Geometry& geometry)
    {
        std::unique_ptr< Mesh > mesh = geometryToDraco(&geometry, true);
        if (mesh)
        {
            std::stringstream buffer;
            encodeMesh(mesh.get(), buffer, _options);
            osg::ByteArray* data = new osg::ByteArray(buffer.str().size());
            memcpy(&data->front(), buffer.str().c_str(), buffer.str().size());
            geometry.setVertexArray(data);
        }
    }

    const osgDB::Options* _options;
};

class DracoDecompressGeometryVisitor : public osg::NodeVisitor
{
public:
    DracoDecompressGeometryVisitor() :
        osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
    {
    }

    void apply(osg::Geometry& geometry)
    {
        osg::ByteArray* meshData = dynamic_cast<osg::ByteArray*>(geometry.getVertexArray());
        if (meshData)
        {
            dracoToGeometry(&geometry, (const char*)(&meshData->front()), meshData->size());
        }        
    }
};

class DracoReaderWriter : public osgDB::ReaderWriter
{
public:
    DracoReaderWriter()
    {
        supportsExtension("drc", "Draco loader");
        supportsExtension("osgb_draco", "Draco encoded osgb");
    }

    virtual const char* className() const { return "draco plugin"; }

    ReadResult readObject(const std::string& location, const osgDB::Options* options) const
    {
        return readNode(location, options);
    }

    ReadResult readNode(const std::string& location, const osgDB::Options* options) const
    {
        std::string ext = osgDB::getFileExtension(location);
        if (!acceptsExtension(ext))
            return ReadResult::FILE_NOT_HANDLED;

        if (ext == "drc")
        {         
            std::ifstream in(location, std::ios_base::binary);
            if (in)
            {
                // Load the whole file into memory
                std::vector<char> data;
                in.seekg(0, std::ios::end);
                unsigned int len = in.tellg();
                in.seekg(0, std::ios::beg);
                data.resize(len);
                in.read(&data[0], len);
                // Convert the draco mesh to an osg::Geometry
                osg::ref_ptr< osg::Geometry > geometry = new osg::Geometry;
                if (dracoToGeometry(geometry, &data[0], len))
                {
                    return geometry.release();
                }
                return ReadResult::ERROR_IN_READING_FILE;
            }
            return ReadResult::FILE_NOT_FOUND;
        }
        else if (ext == "osgb_draco")
        {
            auto reader = osgDB::Registry::instance()->getReaderWriterForExtension("osgb");
            std::ifstream in(location, std::ios_base::binary);

            osg::ref_ptr< osgDB::Options > local_options = options ? options->cloneOptions() : new osgDB::Options;
            local_options->setObjectCacheHint(osgDB::Options::CACHE_IMAGES);
            local_options->getDatabasePathList().push_back(osgDB::getFilePath(location));

            osg::ref_ptr< osg::Node > node = reader->readNode(in, local_options).getNode();
            if (node.valid())
            {                
                DracoDecompressGeometryVisitor decompressVisitor;
                node->accept(decompressVisitor);
                return node.release();
            }
            return ReadResult::ERROR_IN_READING_FILE;
        }        
        else return ReadResult::FILE_NOT_HANDLED;
    }

    //! Read from a stream:
    ReadResult readNode(std::istream& inputStream, const osgDB::Options* options) const
    {
        // load entire stream into a buffer
        std::istreambuf_iterator<char> eof;
        std::string buffer(std::istreambuf_iterator<char>(inputStream), eof);
        return ReadResult::FILE_NOT_HANDLED;
    }

    WriteResult writeNode(const osg::Node& node, const std::string& location, const osgDB::Options* options) const
    {
        std::string ext = osgDB::getLowerCaseFileExtension(location);
        if (!acceptsExtension(ext))
            return WriteResult::FILE_NOT_HANDLED;

        if (ext == "drc")
        {
            const osg::Geometry* geometry = node.asGeometry();
            if (!geometry)
            {
                OSG_NOTICE << "Draco writing out first osg::Geometry in scene" << std::endl;
                // Find the first Geometry and write it out.
                osg::Node* n = const_cast<osg::Node*>(&node);
                geometry = osgEarth::findTopMostNodeOfType<osg::Geometry>(n);
            }
            if (geometry)
            {
                std::unique_ptr< Mesh > mesh = geometryToDraco(const_cast<osg::Geometry*>(geometry), false);
                osgDB::makeDirectoryForFile(location);
                std::ofstream out(location, std::ios_base::binary);
                encodeMesh(mesh.get(), out, options);
                out.close();
                return WriteResult::FILE_SAVED;
            }
        }
        else if (ext == "osgb_draco")
        {
            // Clone the incoming scene graph
            osg::ref_ptr< osg::Node > clone = static_cast<osg::Node*>(node.clone(osg::CopyOp::DEEP_COPY_ALL));
            DracoCompressGeometryVisitor compressVisitor(options);
            clone->accept(compressVisitor);

            std::stringstream buf;
            auto writer = osgDB::Registry::instance()->getReaderWriterForExtension("osgb");
            writer->writeObject(*clone.get(), buf, options);

            std::ofstream out(location, std::ios_base::binary);
            out.write(buf.str().c_str(), buf.str().size());
            out.close();

            return WriteResult::FILE_SAVED;
        }
        return WriteResult::ERROR_IN_WRITING_FILE;
    }
};

REGISTER_OSGPLUGIN(draco, DracoReaderWriter)
