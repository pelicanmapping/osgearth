/* osgEarth
* Copyright 2026 Pelican Mapping
* MIT License
*/
#pragma once

/**
 * Writes an OSG scene graph as glTF 2.0 (JSON or GLB) with cgltf.
 *
 * - Every OSG node becomes a glTF node; transforms carry their local matrix
 *   and a root node rotates OSG's Z-up into glTF's Y-up.
 * - Each osg::Geometry becomes a mesh with POSITION (with bounds), NORMAL,
 *   COLOR_0 (float or normalized byte/short colors), TEXCOORD_0/TEXCOORD_1
 *   (Vec2, or the xy of Vec3 coordinates) and one primitive per DrawArrays
 *   or DrawElements set.
 * - The nearest texture on unit 0 (an osg::Texture, or the albedo of an
 *   osgEarth PBRTexture) becomes a PNG-encoded base color texture with its
 *   sampler settings. Texture coordinates pass through unchanged, so the
 *   image is written with memory row 0 on top; OSG-native and reader-loaded
 *   images both round-trip. Materials are metallic 0 / roughness 1,
 *   doubleSided unless the stateset enables face culling, and BLEND when it
 *   enables blending.
 * - Everything is self-contained: one buffer (the GLB BIN chunk, or a base64
 *   data URI in .gltf) holds vertex, index and image data.
 */

#include <cgltf_write.h>

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/Texture>
#include <osgDB/ConvertBase64>
#include <osgDB/FileUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgDB/fstream>
#include <osgEarth/Notify>
#include <osgEarth/PBRMaterial>

#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#undef LC
#define LC "[gltf] "

class GLTFWriter
{
public:
    osgDB::ReaderWriter::WriteResult write(const osg::Node& node, const std::string& location, bool binary, const osgDB::Options*) const
    {
        Builder builder;
        // accept() is non-const, but the visitor never modifies the graph.
        const_cast<osg::Node&>(node).accept(builder);

        std::string json;
        std::vector<unsigned char> bin;
        if (!builder.serialize(&node, binary, json, bin))
        {
            OE_WARN << LC << "Nothing to write to " << location << std::endl;
            return osgDB::ReaderWriter::WriteResult::ERROR_IN_WRITING_FILE;
        }

        osgDB::ofstream out(location.c_str(), std::ios::out | std::ios::binary);
        if (!out)
            return osgDB::ReaderWriter::WriteResult::ERROR_IN_WRITING_FILE;

        if (binary)
        {
            // GLB: 12-byte header, JSON chunk padded with spaces, BIN chunk padded with zeros.
            while (json.size() % 4) json += ' ';
            const std::size_t binPadded = (bin.size() + 3) & ~std::size_t(3);
            const std::size_t total = 12 + 8 + json.size() + (bin.empty() ? 0 : 8 + binPadded);
            writeUInt32(out, 0x46546C67u);
            writeUInt32(out, 2u);
            writeUInt32(out, static_cast<std::uint32_t>(total));
            writeUInt32(out, static_cast<std::uint32_t>(json.size()));
            writeUInt32(out, 0x4E4F534Au);
            out.write(json.data(), json.size());
            if (!bin.empty())
            {
                writeUInt32(out, static_cast<std::uint32_t>(binPadded));
                writeUInt32(out, 0x004E4942u);
                out.write(reinterpret_cast<const char*>(bin.data()), bin.size());
                for (std::size_t i = bin.size(); i < binPadded; ++i) out.put('\0');
            }
        }
        else
        {
            out.write(json.data(), json.size());
        }

        return out.good() ? osgDB::ReaderWriter::WriteResult::FILE_SAVED : osgDB::ReaderWriter::WriteResult::ERROR_IN_WRITING_FILE;
    }

private:
    static void writeUInt32(std::ostream& out, std::uint32_t value)
    {
        const unsigned char bytes[4] = {
            static_cast<unsigned char>(value & 0xFFu), static_cast<unsigned char>((value >> 8) & 0xFFu),
            static_cast<unsigned char>((value >> 16) & 0xFFu), static_cast<unsigned char>((value >> 24) & 0xFFu) };
        out.write(reinterpret_cast<const char*>(bytes), 4);
    }

    /**
     * Collects the scene into plain index-based records during traversal,
     * then materializes cgltf's pointer-linked arrays in serialize().
     */
    class Builder : public osg::NodeVisitor
    {
    public:
        Builder() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
            setNodeMaskOverride(~0u);
        }

        // ------------------------------------------------------------ traversal

        void apply(osg::Node& node) override
        {
            const bool pushed = pushState(node.getStateSet());
            traverse(node);
            if (pushed) _stateSets.pop_back();
            finish(node);
        }

        void apply(osg::Group& group) override
        {
            apply(static_cast<osg::Node&>(group));
            Node& record = _nodes[_nodeOf[&group]];
            for (unsigned i = 0; i < group.getNumChildren(); ++i)
            {
                auto child = _nodeOf.find(group.getChild(i));
                if (child != _nodeOf.end()) record.children.push_back(child->second);
            }
        }

        void apply(osg::Transform& transform) override
        {
            apply(static_cast<osg::Group&>(transform));
            Node& record = _nodes[_nodeOf[&transform]];
            osg::Matrixd matrix;
            transform.computeLocalToWorldMatrix(matrix, this);
            if (!matrix.isIdentity())
            {
                record.hasMatrix = true;
                record.matrix = matrix;
            }
        }

        void apply(osg::Geometry& geometry) override
        {
            const bool pushed = pushState(geometry.getStateSet());
            const int mesh = addMesh(geometry);
            if (pushed) _stateSets.pop_back();
            finish(geometry);
            _nodes[_nodeOf[&geometry]].mesh = mesh;
        }

        // ---------------------------------------------------------- serialize

        //! Builds the JSON document (and the binary buffer) for the visited graph.
        bool serialize(const osg::Node* top, bool binary, std::string& json, std::vector<unsigned char>& bin)
        {
            auto topNode = _nodeOf.find(top);
            if (topNode == _nodeOf.end()) return false;

            // glTF is Y-up; OSG content is Z-up.
            Node root;
            root.hasMatrix = true;
            root.matrix = osg::Matrixd::rotate(osg::Vec3d(0.0, 0.0, 1.0), osg::Vec3d(0.0, 1.0, 0.0));
            root.children.push_back(topNode->second);
            _nodes.push_back(root);
            const int rootIndex = static_cast<int>(_nodes.size()) - 1;

            // Images live in the buffer for GLB and in data URIs for JSON.
            bin.swap(_bin);
            std::vector<std::string> imageURIs(_images.size());
            std::vector<int> imageViews(_images.size(), -1);
            for (std::size_t i = 0; i < _images.size(); ++i)
            {
                if (binary)
                {
                    imageViews[i] = static_cast<int>(_views.size());
                    _views.push_back(appendToBuffer(bin, _images[i].png.data(), _images[i].png.size(), 0));
                }
                else
                {
                    imageURIs[i] = "data:" + _images[i].mimeType + ";base64," + base64(_images[i].png.data(), _images[i].png.size());
                }
            }
            std::string bufferURI;
            if (!binary && !bin.empty())
                bufferURI = "data:application/octet-stream;base64," + base64(bin.data(), bin.size());

            // cgltf links records by pointer, so size every array before taking addresses.
            cgltf_buffer buffer = {};
            buffer.size = bin.size();
            buffer.uri = binary ? nullptr : mutableString(bufferURI);

            std::vector<cgltf_buffer_view> views(_views.size());
            for (std::size_t i = 0; i < _views.size(); ++i)
            {
                views[i] = cgltf_buffer_view();
                views[i].buffer = &buffer;
                views[i].offset = _views[i].offset;
                views[i].size = _views[i].size;
                views[i].stride = _views[i].stride;
            }

            std::vector<cgltf_accessor> accessors(_accessors.size());
            for (std::size_t i = 0; i < _accessors.size(); ++i)
            {
                const Accessor& source = _accessors[i];
                cgltf_accessor& accessor = accessors[i];
                accessor = cgltf_accessor();
                accessor.buffer_view = &views[source.view];
                accessor.component_type = source.component;
                accessor.type = source.type;
                accessor.normalized = source.normalized ? 1 : 0;
                accessor.offset = source.offset;
                accessor.count = source.count;
                if (source.hasBounds)
                {
                    accessor.has_min = accessor.has_max = 1;
                    for (int c = 0; c < 3; ++c) { accessor.min[c] = source.min[c]; accessor.max[c] = source.max[c]; }
                }
            }

            std::vector<cgltf_image> images(_images.size());
            std::vector<cgltf_sampler> samplers(_images.size());
            std::vector<cgltf_texture> textures(_images.size());
            for (std::size_t i = 0; i < _images.size(); ++i)
            {
                images[i] = cgltf_image();
                images[i].mime_type = mutableString(_images[i].mimeType);
                if (binary) images[i].buffer_view = &views[imageViews[i]];
                else images[i].uri = mutableString(imageURIs[i]);
                samplers[i] = cgltf_sampler();
                samplers[i].wrap_s = static_cast<cgltf_wrap_mode>(_images[i].wrapS);
                samplers[i].wrap_t = static_cast<cgltf_wrap_mode>(_images[i].wrapT);
                samplers[i].min_filter = static_cast<cgltf_filter_type>(_images[i].minFilter);
                samplers[i].mag_filter = static_cast<cgltf_filter_type>(_images[i].magFilter);
                textures[i] = cgltf_texture();
                textures[i].image = &images[i];
                textures[i].sampler = &samplers[i];
            }

            std::vector<cgltf_material> materials(_materials.size());
            for (std::size_t i = 0; i < _materials.size(); ++i)
            {
                cgltf_material& material = materials[i];
                material = cgltf_material();
                material.name = mutableString(_materials[i].name);
                material.has_pbr_metallic_roughness = 1;
                cgltf_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;
                pbr.base_color_texture.texture = &textures[_materials[i].texture];
                pbr.base_color_texture.scale = 1.0f;
                for (int c = 0; c < 4; ++c) pbr.base_color_factor[c] = 1.0f;
                pbr.metallic_factor = 0.0f;
                pbr.roughness_factor = 1.0f;
                material.normal_texture.scale = 1.0f;
                material.occlusion_texture.scale = 1.0f;
                material.emissive_texture.scale = 1.0f;
                material.alpha_cutoff = 0.5f;
                material.alpha_mode = _materials[i].blend ? cgltf_alpha_mode_blend : cgltf_alpha_mode_opaque;
                material.double_sided = _materials[i].doubleSided ? 1 : 0;
            }

            std::size_t primitiveCount = 0, attributeCount = 0;
            for (const Mesh& mesh : _meshes)
                for (const Primitive& primitive : mesh.primitives)
                {
                    ++primitiveCount;
                    attributeCount += primitive.attributes.size();
                }
            std::vector<cgltf_primitive> primitives(primitiveCount);
            std::vector<cgltf_attribute> attributes(attributeCount);
            std::vector<cgltf_mesh> meshes(_meshes.size());
            std::size_t primitiveIndex = 0, attributeIndex = 0;
            for (std::size_t i = 0; i < _meshes.size(); ++i)
            {
                meshes[i] = cgltf_mesh();
                meshes[i].name = mutableString(_meshes[i].name);
                meshes[i].primitives = &primitives[primitiveIndex];
                meshes[i].primitives_count = _meshes[i].primitives.size();
                for (const Primitive& source : _meshes[i].primitives)
                {
                    cgltf_primitive& primitive = primitives[primitiveIndex++];
                    primitive = cgltf_primitive();
                    primitive.type = source.mode;
                    primitive.indices = source.indices >= 0 ? &accessors[source.indices] : nullptr;
                    primitive.material = source.material >= 0 ? &materials[source.material] : nullptr;
                    primitive.attributes = &attributes[attributeIndex];
                    primitive.attributes_count = source.attributes.size();
                    for (const Attribute& attribute : source.attributes)
                    {
                        cgltf_attribute& target = attributes[attributeIndex++];
                        target = cgltf_attribute();
                        target.name = mutableString(attribute.name);
                        target.data = &accessors[attribute.accessor];
                    }
                }
            }

            std::size_t childCount = 0;
            for (const Node& node : _nodes) childCount += node.children.size();
            std::vector<cgltf_node*> children(childCount);
            std::vector<cgltf_node> nodes(_nodes.size());
            std::size_t childIndex = 0;
            for (std::size_t i = 0; i < _nodes.size(); ++i)
            {
                const Node& source = _nodes[i];
                cgltf_node& node = nodes[i];
                node = cgltf_node();
                node.name = mutableString(source.name);
                node.mesh = source.mesh >= 0 ? &meshes[source.mesh] : nullptr;
                node.children = &children[childIndex];
                node.children_count = source.children.size();
                for (int child : source.children) children[childIndex++] = &nodes[child];
                node.rotation[3] = 1.0f;
                node.scale[0] = node.scale[1] = node.scale[2] = 1.0f;
                if (source.hasMatrix)
                {
                    node.has_matrix = 1;
                    for (int c = 0; c < 16; ++c) node.matrix[c] = static_cast<cgltf_float>(source.matrix.ptr()[c]);
                }
                else
                {
                    node.matrix[0] = node.matrix[5] = node.matrix[10] = node.matrix[15] = 1.0f;
                }
            }

            cgltf_node* sceneRoot = &nodes[rootIndex];
            cgltf_scene scene = {};
            scene.nodes = &sceneRoot;
            scene.nodes_count = 1;

            std::string version = "2.0", generator = "osgEarth";
            cgltf_data data = {};
            data.asset.version = mutableString(version);
            data.asset.generator = mutableString(generator);
            data.buffers = &buffer; data.buffers_count = 1;
            data.buffer_views = views.data(); data.buffer_views_count = views.size();
            data.accessors = accessors.data(); data.accessors_count = accessors.size();
            data.images = images.data(); data.images_count = images.size();
            data.samplers = samplers.data(); data.samplers_count = samplers.size();
            data.textures = textures.data(); data.textures_count = textures.size();
            data.materials = materials.data(); data.materials_count = materials.size();
            data.meshes = meshes.data(); data.meshes_count = meshes.size();
            data.nodes = nodes.data(); data.nodes_count = nodes.size();
            data.scenes = &scene; data.scenes_count = 1;
            data.scene = &scene;

            cgltf_options options = {};
            options.type = binary ? cgltf_file_type_glb : cgltf_file_type_gltf;
            const cgltf_size size = cgltf_write(&options, nullptr, 0, &data);
            if (size == 0) return false;
            json.assign(size, '\0');
            const cgltf_size written = cgltf_write(&options, &json[0], size, &data);
            if (written == 0) return false;
            json.resize(written - 1); // drop the terminator
            return true;
        }

    private:
        struct View { std::size_t offset = 0, size = 0, stride = 0; };
        struct Accessor
        {
            int view = -1;
            cgltf_component_type component = cgltf_component_type_invalid;
            cgltf_type type = cgltf_type_invalid;
            bool normalized = false;
            std::size_t offset = 0, count = 0;
            bool hasBounds = false;
            osg::Vec3f min, max;
        };
        struct Attribute { std::string name; int accessor = -1; };
        struct Primitive
        {
            cgltf_primitive_type mode = cgltf_primitive_type_triangles;
            int indices = -1, material = -1;
            std::vector<Attribute> attributes;
        };
        struct Mesh { std::string name; std::vector<Primitive> primitives; };
        struct Node
        {
            std::string name;
            bool hasMatrix = false;
            osg::Matrixd matrix;
            int mesh = -1;
            std::vector<int> children;
        };
        struct Image
        {
            std::vector<unsigned char> png;
            std::string mimeType = "image/png";
            int wrapS = 10497, wrapT = 10497, minFilter = 0, magFilter = 0;
        };
        struct Material { std::string name; int texture = -1; bool doubleSided = false, blend = false; };

        std::vector<unsigned char> _bin;
        std::vector<View> _views;
        std::vector<Accessor> _accessors;
        std::vector<Mesh> _meshes;
        std::vector<Node> _nodes;
        std::vector<Image> _images;
        std::vector<Material> _materials;
        std::vector<osg::StateSet*> _stateSets;
        std::vector<osg::ref_ptr<osg::Array>> _converted; // Vec3 texcoords converted to Vec2

        std::map<const osg::Node*, int> _nodeOf;
        std::map<const osg::BufferData*, int> _viewOf;
        std::map<std::tuple<const osg::Array*, std::size_t, std::size_t>, int> _accessorOf;
        std::map<const osg::DrawElements*, int> _indexAccessorOf;
        std::map<const osg::StateAttribute*, int> _materialOf;

        static char* mutableString(const std::string& value)
        {
            // cgltf_write only reads these; the storage outlives the write call.
            return value.empty() ? nullptr : const_cast<char*>(value.c_str());
        }

        static std::string base64(const unsigned char* data, std::size_t size)
        {
            std::string encoded;
            osgDB::Base64encoder().encode(reinterpret_cast<const char*>(data), static_cast<int>(size), encoded);
            encoded.erase(std::remove(encoded.begin(), encoded.end(), '\n'), encoded.end());
            encoded.erase(std::remove(encoded.begin(), encoded.end(), '\r'), encoded.end());
            return encoded;
        }

        //! Only statesets that carry a unit-0 texture take part in material lookup.
        bool pushState(osg::StateSet* stateSet)
        {
            if (!stateSet || !stateSet->getTextureAttribute(0, osg::StateAttribute::TEXTURE)) return false;
            _stateSets.push_back(stateSet);
            return true;
        }

        void finish(osg::Node& node)
        {
            Node record;
            record.name = node.getName();
            _nodes.push_back(record);
            _nodeOf[&node] = static_cast<int>(_nodes.size()) - 1;
        }

        // ------------------------------------------------------------ buffers

        static View appendToBuffer(std::vector<unsigned char>& bin, const void* data, std::size_t size, std::size_t stride)
        {
            while (bin.size() % 4) bin.push_back(0);
            View view;
            view.offset = bin.size();
            view.size = size;
            view.stride = stride;
            bin.insert(bin.end(), static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + size);
            return view;
        }

        int viewOf(const osg::BufferData* data, std::size_t stride)
        {
            auto found = _viewOf.find(data);
            if (found != _viewOf.end()) return found->second;
            _views.push_back(appendToBuffer(_bin, data->getDataPointer(), data->getTotalDataSize(), stride));
            return _viewOf[data] = static_cast<int>(_views.size()) - 1;
        }

        //! Maps an OSG array to a glTF accessor layout; returns false for unsupported arrays.
        static bool layout(const osg::Array* array, cgltf_component_type& component, cgltf_type& type)
        {
            switch (array->getDataType())
            {
            case GL_FLOAT: component = cgltf_component_type_r_32f; break;
            case GL_UNSIGNED_BYTE: component = cgltf_component_type_r_8u; break;
            case GL_UNSIGNED_SHORT: component = cgltf_component_type_r_16u; break;
            case GL_UNSIGNED_INT: component = cgltf_component_type_r_32u; break;
            case GL_BYTE: component = cgltf_component_type_r_8; break;
            case GL_SHORT: component = cgltf_component_type_r_16; break;
            default: return false;
            }
            switch (array->getDataSize())
            {
            case 1: type = cgltf_type_scalar; break;
            case 2: type = cgltf_type_vec2; break;
            case 3: type = cgltf_type_vec3; break;
            case 4: type = cgltf_type_vec4; break;
            default: return false;
            }
            return true;
        }

        int accessorOf(const osg::Array* array, std::size_t first, std::size_t count, bool bounds)
        {
            const auto key = std::make_tuple(array, first, count);
            auto found = _accessorOf.find(key);
            if (found != _accessorOf.end()) return found->second;

            Accessor accessor;
            if (!layout(array, accessor.component, accessor.type)) return -1;
            const std::size_t elementSize = array->getElementSize();
            // A stride lets several accessors slice one view; glTF requires a multiple of 4.
            accessor.view = viewOf(array, elementSize % 4 == 0 ? elementSize : 0);
            accessor.normalized = array->getNormalize();
            accessor.offset = first * elementSize;
            accessor.count = count;
            if (bounds && array->getType() == osg::Array::Vec3ArrayType && count > 0)
            {
                const auto& positions = *static_cast<const osg::Vec3Array*>(array);
                accessor.hasBounds = true;
                accessor.min = accessor.max = positions[first];
                for (std::size_t i = first; i < first + count; ++i)
                    for (int c = 0; c < 3; ++c)
                    {
                        accessor.min[c] = std::min(accessor.min[c], positions[i][c]);
                        accessor.max[c] = std::max(accessor.max[c], positions[i][c]);
                    }
            }
            _accessors.push_back(accessor);
            return _accessorOf[key] = static_cast<int>(_accessors.size()) - 1;
        }

        int indexAccessorOf(const osg::DrawElements* elements)
        {
            auto found = _indexAccessorOf.find(elements);
            if (found != _indexAccessorOf.end()) return found->second;

            Accessor accessor;
            accessor.type = cgltf_type_scalar;
            if (dynamic_cast<const osg::DrawElementsUByte*>(elements)) accessor.component = cgltf_component_type_r_8u;
            else if (dynamic_cast<const osg::DrawElementsUShort*>(elements)) accessor.component = cgltf_component_type_r_16u;
            else if (dynamic_cast<const osg::DrawElementsUInt*>(elements)) accessor.component = cgltf_component_type_r_32u;
            else return -1;
            accessor.view = viewOf(elements, 0);
            accessor.count = elements->getNumIndices();
            _accessors.push_back(accessor);
            return _indexAccessorOf[elements] = static_cast<int>(_accessors.size()) - 1;
        }

        // ------------------------------------------------------------- meshes

        static cgltf_primitive_type primitiveType(GLenum mode)
        {
            switch (mode)
            {
            case GL_POINTS: return cgltf_primitive_type_points;
            case GL_LINES: return cgltf_primitive_type_lines;
            case GL_LINE_LOOP: return cgltf_primitive_type_line_loop;
            case GL_LINE_STRIP: return cgltf_primitive_type_line_strip;
            case GL_TRIANGLES: return cgltf_primitive_type_triangles;
            case GL_TRIANGLE_STRIP: return cgltf_primitive_type_triangle_strip;
            case GL_TRIANGLE_FAN: return cgltf_primitive_type_triangle_fan;
            default: return cgltf_primitive_type_invalid;
            }
        }

        //! A per-vertex array usable as the named attribute, or null.
        const osg::Array* vertexAttribute(const osg::Array* array, unsigned vertexCount, bool floatOnly)
        {
            if (!array || array->getNumElements() != vertexCount) return nullptr;
            if (floatOnly && array->getDataType() != GL_FLOAT) return nullptr;
            cgltf_component_type component; cgltf_type type;
            return layout(array, component, type) ? array : nullptr;
        }

        int addMesh(osg::Geometry& geometry)
        {
            const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
            if (!positions || positions->empty())
            {
                OE_WARN << LC << "Skipping geometry \"" << geometry.getName() << "\" without Vec3 vertices" << std::endl;
                return -1;
            }
            const unsigned vertexCount = positions->size();
            const osg::Array* normals = vertexAttribute(geometry.getNormalArray(), vertexCount, true);
            if (normals && normals->getDataSize() != 3) normals = nullptr;
            const osg::Array* colors = vertexAttribute(geometry.getColorArray(), vertexCount, false);
            if (colors && (colors->getDataSize() < 3 || colors->getDataType() == GL_UNSIGNED_INT ||
                colors->getDataType() == GL_BYTE || colors->getDataType() == GL_SHORT))
                colors = nullptr;
            const osg::Array* texcoords[2] = { nullptr, nullptr };
            for (unsigned unit = 0; unit < 2; ++unit)
            {
                const osg::Array* source = geometry.getTexCoordArray(unit);
                if (auto* xyz = dynamic_cast<const osg::Vec3Array*>(source))
                {
                    osg::ref_ptr<osg::Vec2Array> xy = new osg::Vec2Array(xyz->size());
                    for (unsigned i = 0; i < xyz->size(); ++i) (*xy)[i].set((*xyz)[i].x(), (*xyz)[i].y());
                    _converted.push_back(xy);
                    source = xy.get();
                }
                texcoords[unit] = vertexAttribute(source, vertexCount, true);
                if (texcoords[unit] && texcoords[unit]->getDataSize() != 2) texcoords[unit] = nullptr;
            }

            const int material = currentMaterial();
            Mesh mesh;
            mesh.name = geometry.getName();
            for (unsigned i = 0; i < geometry.getNumPrimitiveSets(); ++i)
            {
                const osg::PrimitiveSet* set = geometry.getPrimitiveSet(i);
                Primitive primitive;
                primitive.mode = primitiveType(set->getMode());
                std::size_t first = 0, count = vertexCount;
                if (const auto* elements = dynamic_cast<const osg::DrawElements*>(set))
                {
                    primitive.indices = indexAccessorOf(elements);
                    if (primitive.indices < 0) primitive.mode = cgltf_primitive_type_invalid;
                }
                else if (const auto* arrays = dynamic_cast<const osg::DrawArrays*>(set))
                {
                    first = static_cast<std::size_t>(std::max(0, arrays->getFirst()));
                    count = static_cast<std::size_t>(std::max(0, arrays->getCount()));
                    if (first + count > vertexCount) primitive.mode = cgltf_primitive_type_invalid;
                }
                else primitive.mode = cgltf_primitive_type_invalid;

                if (primitive.mode == cgltf_primitive_type_invalid)
                {
                    OE_WARN << LC << "Skipping unsupported primitive set in \"" << geometry.getName() << "\"" << std::endl;
                    continue;
                }

                primitive.attributes.push_back({ "POSITION", accessorOf(positions, first, count, true) });
                if (normals) primitive.attributes.push_back({ "NORMAL", accessorOf(normals, first, count, false) });
                if (colors) primitive.attributes.push_back({ "COLOR_0", accessorOf(colors, first, count, false) });
                for (unsigned unit = 0; unit < 2; ++unit)
                    if (texcoords[unit])
                        primitive.attributes.push_back({ unit == 0 ? "TEXCOORD_0" : "TEXCOORD_1", accessorOf(texcoords[unit], first, count, false) });
                // A textured material needs texture coordinates to sample with.
                if (material >= 0 && texcoords[0]) primitive.material = material;
                mesh.primitives.push_back(primitive);
            }
            if (mesh.primitives.empty()) return -1;
            _meshes.push_back(mesh);
            return static_cast<int>(_meshes.size()) - 1;
        }

        // ---------------------------------------------------------- materials

        //! Material for the nearest stateset with a unit-0 texture, or -1.
        int currentMaterial()
        {
            if (_stateSets.empty()) return -1;
            osg::StateSet* stateSet = _stateSets.back();
            osg::StateAttribute* attribute = stateSet->getTextureAttribute(0, osg::StateAttribute::TEXTURE);
            auto found = _materialOf.find(attribute);
            if (found != _materialOf.end()) return found->second;

            osg::Texture* texture = dynamic_cast<osg::Texture*>(attribute);
            if (!texture)
                if (auto* pbr = dynamic_cast<osgEarth::PBRTexture*>(attribute)) texture = pbr->albedo.get();
            const osg::Image* image = texture ? texture->getImage(0) : nullptr;

            int index = -1;
            Image record;
            if (image && encodePNG(*image, record.png))
            {
                record.wrapS = wrapMode(texture->getWrap(osg::Texture::WRAP_S));
                record.wrapT = wrapMode(texture->getWrap(osg::Texture::WRAP_T));
                record.minFilter = texture->getFilter(osg::Texture::MIN_FILTER);
                record.magFilter = texture->getFilter(osg::Texture::MAG_FILTER);
                _images.push_back(record);

                Material material;
                material.name = texture->getName();
                material.texture = static_cast<int>(_images.size()) - 1;
                material.doubleSided = (stateSet->getMode(GL_CULL_FACE) & osg::StateAttribute::ON) == 0;
                material.blend = (stateSet->getMode(GL_BLEND) & osg::StateAttribute::ON) != 0;
                _materials.push_back(material);
                index = static_cast<int>(_materials.size()) - 1;
            }
            return _materialOf[attribute] = index;
        }

        static int wrapMode(osg::Texture::WrapMode mode)
        {
            switch (mode)
            {
            case osg::Texture::REPEAT: return 10497;
            case osg::Texture::MIRROR: return 33648;
            default: return 33071; // every clamp variant becomes CLAMP_TO_EDGE
            }
        }

        /**
         * Encodes an 8-bit image as PNG. Texture coordinates are written
         * unchanged, and both OSG (t = 0) and glTF (v = 0) sample memory row 0,
         * so the PNG's top row must be memory row 0. OSG's PNG plugin writes
         * the last row first, hence the flip before encoding.
         */
        static bool encodePNG(const osg::Image& image, std::vector<unsigned char>& png)
        {
            osgDB::ReaderWriter* writer = osgDB::Registry::instance()->getReaderWriterForExtension("png");
            if (!writer)
            {
                OE_WARN << LC << "PNG plugin unavailable; texture not written" << std::endl;
                return false;
            }
            if (image.isCompressed() || image.getDataType() != GL_UNSIGNED_BYTE || image.r() != 1)
            {
                OE_WARN << LC << "Texture \"" << image.getFileName() << "\" is not an 8-bit image; not written" << std::endl;
                return false;
            }
            osg::ref_ptr<osg::Image> flipped = new osg::Image(image, osg::CopyOp::DEEP_COPY_ALL);
            flipped->flipVertical();
            std::ostringstream out(std::ios::out | std::ios::binary);
            if (!writer->writeImage(*flipped, out, nullptr).success())
            {
                OE_WARN << LC << "Cannot encode texture \"" << image.getFileName() << "\" as PNG" << std::endl;
                return false;
            }
            const std::string bytes = out.str();
            png.assign(bytes.begin(), bytes.end());
            return true;
        }
    };
};
