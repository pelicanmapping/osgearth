/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include "Chonk"
#include "Color"
#include "GLUtils"
#include "Metrics"
#include "VirtualProgram"
#include "Shaders"
#include "Utils"
#include "DrawInstanced"
#include "Registry"
#include "PBRMaterial"
#include "Notify"
#include "ImageUtils"
#include "Math"
#include "ExternalNode"
#include "InstancedExternalNode"
#include <osg/MatrixTransform>
#include <osg/CullFace>
#include <osg/FrontFace>

#include <osgUtil/Optimizer>

#undef LC
#define LC "[Chonk] "

using namespace osgEarth;

#define MAX_NEAR_PIXEL_SCALE FLT_MAX

// note: this MUST match the local_size product in Chonk.Culling.glsl
#define GPU_CULLING_LOCAL_WG_SIZE 32

// Uncomment this to reset all buffer base index bindings after rendering.
// It's unlikely this is necessary, but it's here just we find otherwise.
//#define RESET_BUFFER_BASE_BINDINGS

// Chunk sizes for the various GL buffers that the culling system will allocate.
// In theory chunked allocation can make object recycling more efficient.
// These are all in bytes
#define COMMAND_BUF_CHUNK_SIZE 512
#define INPUT_BUF_CHUNK_SIZE (1024 * 512)
#define OUTPUT_BUF_CHUNK_SIZE (INPUT_BUF_CHUNK_SIZE * 2)
#define CHONK_BUF_CHUNK_SIZE 256

namespace
{
    struct SendIndices
    {
        std::function<void(unsigned i0, unsigned i1, unsigned i2)> func;

        void operator()(unsigned i0, unsigned i1, unsigned i2) const {
            func(i0, i1, i2);
        }
    };

    /**
     * Visitor that counts the verts and elements in a scene graph.
     */
    struct Counter : public osg::NodeVisitor
    {
        unsigned _numVerts = 0u;
        unsigned _numElements = 0u;

        Counter()
        {
            // Use the "active chidren" mode to only bring in default switch
            // and osgSim::MultiSwitch children for now. -gw
            setTraversalMode(TRAVERSE_ACTIVE_CHILDREN);
            setNodeMaskOverride(~0);
        }

        void apply(osg::Geometry& node) override
        {
            auto instanced = dynamic_cast<DrawInstanced::InstanceGeometry*>(&node);
            if (instanced)
            {
                return; // skip these.
                //apply(*instanced);
                //return;
            }

            auto verts = dynamic_cast<osg::Vec3Array*>(node.getVertexArray());
            if (verts)
            {
                _numVerts += verts->size();
            }
            
            for (unsigned i = 0; i < node.getNumPrimitiveSets(); ++i)
            {
                auto p = node.getPrimitiveSet(i);
                if (p)
                {
                    if (p->getMode() == p->TRIANGLES ||
                        p->getMode() == p->TRIANGLES_ADJACENCY ||
                        p->getMode() == p->TRIANGLE_FAN ||
                        p->getMode() == p->TRIANGLE_STRIP ||
                        p->getMode() == p->QUADS ||
                        p->getMode() == p->QUAD_STRIP)
                    {
                        _numElements += p->getNumIndices();
                    }
                }
            }
        }
    };

    /**
     * Visitor that traverses a graph and generates a Chonk,
     * storing any discovered textures in the provided arena.
     */
    struct Ripper : public osg::NodeVisitor
    {
        Chonk* _chonk = nullptr;
        ChonkDrawable* _drawable = nullptr; // optional.
        float _far_pixel_scale = 0.0f;
        float _near_pixel_scale = MAX_NEAR_PIXEL_SCALE;
        TextureArena* _textures = nullptr;
        ChonkFactory::GetOrCreateFunction _getOrCreateTexture;
        std::list<ChonkMaterial::Ptr> _materialCache;
        std::stack<ChonkMaterial::Ptr> _materialStack;
        std::stack<osg::Matrix> _transformStack;
        std::unordered_map<osg::Texture*, Texture::Ptr> _textureLUT;

        const unsigned ALBEDO_UNIT = 0;
        const unsigned NORMAL_UNIT = 1;
        const unsigned PBR_UNIT = 2;

        const unsigned MAT1_SLOT = 0;
        const unsigned MAT2_SLOT = 1;

        const unsigned FLEXOR_SLOT = 3;
        const unsigned NORMAL_TECHNIQUE_SLOT = 6;
        const unsigned EXTENDED_MATERIAL_SLOT = Chonk::MATERIAL_VERTEX_SLOT;

        ChonkMaterial::Ptr reuseOrCreateMaterial(
            Texture::Ptr albedo_tex,
            Texture::Ptr normal_tex,
            Texture::Ptr pbr_tex,
            Texture::Ptr mat1_tex,
            Texture::Ptr mat2_tex,
            Texture::Ptr occlusion_tex = nullptr,
            GLubyte gltf_material = 0,
            const osg::Vec4f& pbr_factors = osg::Vec4f(1, 1, 1, 1))
        {
            int albedo_index = _textures->find(albedo_tex);
            int normal_index = _textures->find(normal_tex);
            int pbr_index = _textures->find(pbr_tex);
            int mat1_index = _textures->find(mat1_tex);
            int mat2_index = _textures->find(mat2_tex);
            int occlusion_index = _textures->find(occlusion_tex);

            for (auto& m : _materialCache)
            {
                if (m->albedo_index == albedo_index &&
                    m->normal_index == normal_index &&
                    m->pbr_index == pbr_index &&
                    m->material1_index == mat1_index &&
                    m->material2_index == mat2_index &&
                    m->occlusion_index == occlusion_index &&
                    m->gltf_material == gltf_material &&
                    m->pbr_factors == pbr_factors)
                {
                    return m;
                }
            }

            auto material = ChonkMaterial::create();
            material->albedo_index = albedo_index;
            material->normal_index = normal_index;
            material->pbr_index = pbr_index;
            material->material1_index = mat1_index;
            material->material2_index = mat2_index;
            material->occlusion_index = occlusion_index;
            material->gltf_material = gltf_material;
            material->pbr_factors = pbr_factors;

            // If our arena is in auto-release mode, we need to 
            // store a pointer to each texture we use so they do not
            // get deleted while the material is still in business:
            if (_textures && _textures->getAutoRelease() == true)
            {
                material->albedo_tex = albedo_tex;
                material->normal_tex = normal_tex;
                material->pbr_tex = pbr_tex;
                material->material1_tex = mat1_tex;
                material->material2_tex = mat2_tex;
                material->occlusion_tex = occlusion_tex;
            }

            _materialCache.push_back(material);
            return material;
        }

        // Constructor.
        // Pointer to the "current base chonk" is required.
        // Pointer to the IG chonk vector is optional; populate this if you want the ripper to rip InstanceGeometry nodes
        //   and add them to the vector.
        Ripper(Chonk* chonk, ChonkDrawable* drawable, TextureArena* textures, ChonkFactory::GetOrCreateFunction func,
            float far_pixel_scale, float near_pixel_scale) :
            _chonk(chonk),
            _drawable(drawable),
            _textures(textures),
            _getOrCreateTexture(func),
            _far_pixel_scale(far_pixel_scale),
            _near_pixel_scale(near_pixel_scale)
        {
            // Use the "active chidren" mode to only bring in default switch
            // and osgSim::MultiSwitch children for now. -gw
            setTraversalMode(TRAVERSE_ACTIVE_CHILDREN);
            setNodeMaskOverride(~0);

            _materialStack.push(reuseOrCreateMaterial(nullptr, nullptr, nullptr, nullptr, nullptr));
            _transformStack.push(osg::Matrix());
        }

        std::stack<int> _materialIndexStack;

        Texture::Ptr addTexture(osg::Texture* tex)
        {
            Texture::Ptr arena_tex;
            if (tex && tex->getImage(0))
            {
                auto i = _textureLUT.find(tex);

                if (i == _textureLUT.end())
                {
                    if (_getOrCreateTexture)
                    {
                        bool isNew = true;
                        arena_tex = _getOrCreateTexture(tex, isNew);
                    }
                    else
                    {
                        arena_tex = Texture::create(tex);
                    }

                    arena_tex->category() = "Chonk texture";

                    int index = _textures->add(arena_tex);
                    if (index >= 0)
                    {
                        _textureLUT[tex] = arena_tex;
                    }
                }
                else
                {
                    arena_tex = i->second;
                }
            }
            return arena_tex;
        }

        // adds a teture to the arena and returns its index
        Texture::Ptr addTexture(unsigned slot, osg::StateSet* stateset)
        {
            OE_SOFT_ASSERT_AND_RETURN(_textures != nullptr, {});

            // if the slot isn't mapped, bail out
            if (slot < 0)
                return {};

            Texture::Ptr arena_tex;

            osg::Texture* tex = dynamic_cast<osg::Texture*>(
                stateset->getTextureAttribute(slot, osg::StateAttribute::TEXTURE));

            if (tex && tex->getImage(0))
            {
                arena_tex = addTexture(tex);
            }

            return arena_tex;
        }

        // find texture with CHONK_HINT_EXTENDED_MATERIAL_SLOT set to the target slot
        Texture::Ptr findExternalTexture(unsigned slot, osg::StateSet* stateset)
        {
           const unsigned count = static_cast<unsigned>(stateset->getTextureAttributeList().size() );

           for (unsigned index = 0; index < count; ++index)
           {
              osg::Texture* tex = dynamic_cast<osg::Texture*>(
                 stateset->getTextureAttribute(index, osg::StateAttribute::TEXTURE));

              int value = -1;
              if (tex && tex->getUserValue(CHONK_HINT_EXTENDED_MATERIAL_SLOT, value) && value == slot )
              {
                 return addTexture(index, stateset);
              }
           }

           return nullptr;
        }

        // record materials, and return true if we pushed one.
        bool pushStateSet(osg::StateSet* stateset)
        {
            bool pushed = false;
            if (stateset)
            {
                Texture::Ptr albedo_tex, normal_tex, pbr_tex;
                Texture::Ptr material_tex1, material_tex2;
                Texture::Ptr occlusion_tex;
                GLubyte gltf_material = 0;
                osg::Vec4f factors(1, 1, 1, 1), flags;
                auto* gltfFlags = stateset->getUniform("oe_gltf_pbr_flags");
                auto* gltfFactors = stateset->getUniform("oe_gltf_pbr_factors");
                if (gltfFlags && gltfFactors && gltfFlags->get(flags) && gltfFactors->get(factors))
                {
                    gltf_material = 1u | (flags.w() > 0.5f ? 2u : 0u);
                    occlusion_tex = addTexture(3u, stateset);
                }

                auto combo = dynamic_cast<PBRTexture*>(stateset->getTextureAttribute(ALBEDO_UNIT, osg::StateAttribute::TEXTURE));
                if (combo)
                {
                    albedo_tex = addTexture(combo->albedo);
                    normal_tex = addTexture(combo->normal);
                    pbr_tex = addTexture(combo->pbr);
                }
                else
                {
                    albedo_tex = addTexture(ALBEDO_UNIT, stateset);
                    normal_tex = addTexture(NORMAL_UNIT, stateset);
                    pbr_tex = addTexture(PBR_UNIT, stateset);
                }

                material_tex1 = findExternalTexture(MAT1_SLOT, stateset);
                material_tex2 = findExternalTexture(MAT2_SLOT, stateset);

                if (albedo_tex || normal_tex || pbr_tex || gltf_material)
                {
                    ChonkMaterial::Ptr material = reuseOrCreateMaterial(
                        albedo_tex, normal_tex, pbr_tex, material_tex1, material_tex2,
                        occlusion_tex, gltf_material, factors);
                    _materialStack.push(material);
                    pushed = true;
                }
            }
            return pushed;
        }

        void popStateSet()
        {
            _materialStack.pop();
        }

        void apply(osg::Node& node)
        {
            bool pushed = pushStateSet(node.getStateSet());
            traverse(node);
            if (pushed) popStateSet();
        }

        void apply(osg::Transform& node)
        {
            osg::Matrix m = _transformStack.empty() ? osg::Matrix() : _transformStack.top();
            node.computeLocalToWorldMatrix(m, this);
            _transformStack.push(m);
            apply(static_cast<osg::Group&>(node));
            _transformStack.pop();
        }

        void apply(osg::Geometry& node)
        {
            bool pushed = pushStateSet(node.getStateSet());

            Chonk* chonk = _chonk;
            osg::Matrixd matrix = _transformStack.top();

            // If this an instanced geometry, create a new chonk for it and
            // let the code below rip to that new chonk. Afterwards we will add that
            // new chonk to the drawable with each instance matrix.
            Chonk::Ptr ig_chonk;
            auto ig = dynamic_cast<DrawInstanced::InstanceGeometry*>(&node);
            if (_drawable && ig)
            {
                ig_chonk = Chonk::create();

                // preallocate for speed:
                Counter counter;
                node.accept(counter);
                if (counter._numElements > 0 && counter._numVerts > 0)
                {
                    ig_chonk->_vbo_store.reserve(counter._numVerts);
                    ig_chonk->_ebo_store.reserve(counter._numElements);
                }

                chonk = ig_chonk.get();
                matrix = osg::Matrixd::identity();
            }

            // rip the geometry into our chonk.
            if (chonk)
            {
                unsigned numVerts = node.getVertexArray()->getNumElements();

                unsigned vbo_offset = chonk->_vbo_store.size();

                auto verts = dynamic_cast<osg::Vec3Array*>(node.getVertexArray());
                auto colors = dynamic_cast<osg::Vec4Array*>(node.getColorArray());
                auto packed_colors = dynamic_cast<osg::Vec4ubArray*>(node.getColorArray());
                bool color_is_linear = false;
                node.getUserValue(CHONK_HINT_LINEAR_COLOR, color_is_linear);
                auto normals = dynamic_cast<osg::Vec3Array*>(node.getNormalArray());
                auto normal_techniques = dynamic_cast<osg::UByteArray*>(node.getVertexAttribArray(NORMAL_TECHNIQUE_SLOT));
                auto flexors = dynamic_cast<osg::Vec3Array*>(node.getTexCoordArray(FLEXOR_SLOT));
                auto extended_material = dynamic_cast<osg::ShortArray*>(node.getVertexAttribArray(EXTENDED_MATERIAL_SLOT));

                // support either 2- or 3-component tex coords, but only read the xy components!
                auto uv2s = dynamic_cast<osg::Vec2Array*>(node.getTexCoordArray(0));
                auto uv3s = dynamic_cast<osg::Vec3Array*>(node.getTexCoordArray(0));

                auto& material = _materialStack.top();
                osg::Vec3f n;
                const osg::Matrixd normalMatrix = osg::Matrixd::inverse(matrix);

                for (unsigned i = 0; i < numVerts; ++i)
                {
                    Chonk::VertexGPU v;
                    v.color_is_linear = color_is_linear ? 1 : 0;

                    if (verts)
                    {
                        v.position = (*verts)[i] * matrix;
                    }

                    if (colors)
                    {
                        int k = colors->getBinding() == osg::Array::BIND_PER_VERTEX ? i : 0;
                        v.color = Color((*colors)[k]).asNormalizedRGBA();
                    }
                    else if (packed_colors)
                    {
                        int k = packed_colors->getBinding() == osg::Array::BIND_PER_VERTEX ? i : 0;
                        v.color = (*packed_colors)[k];
                    }
                    else
                    {
                        v.color.set(255, 255, 255, 255);
                    }

                    if (normals)
                    {
                        int k = normals->getBinding() == osg::Array::BIND_PER_VERTEX ? i : 0;
                        // Column-vector overload is the inverse transpose in
                        // OSG's row-vector convention.
                        v.normal = osg::Matrixd::transform3x3(normalMatrix, osg::Vec3d((*normals)[k]));
                        v.normal.normalize();
                    }
                    else
                    {
                        v.normal.set(0, 0, 1);
                    }

                    if (normal_techniques)
                    {
                        int k = normal_techniques->getBinding() == osg::Array::BIND_PER_VERTEX ? i : 0;
                        v.normal_technique = (*normal_techniques)[k];
                    }
                    else
                    {
                        v.normal_technique = 0;
                    }

                    if (uv2s)
                    {
                        int k = uv2s->getBinding() == osg::Array::BIND_PER_VERTEX ? i : 0;
                        v.uv = (*uv2s)[k];
                    }
                    else if (uv3s)
                    {
                        int k = uv3s->getBinding() == osg::Array::BIND_PER_VERTEX ? i : 0;
                        v.uv.set((*uv3s)[k].x(), (*uv3s)[k].y());
                    }
                    else
                    {
                        v.uv.set(0.0f, 0.0f);
                    }

                    if (flexors)
                    {
                        int k = flexors->getBinding() == osg::Array::BIND_PER_VERTEX ? i : 0;
                        v.flex = osg::Matrix::transform3x3((*flexors)[k], matrix);
                    }
                    else
                    {
                        v.flex.set(0, 0, 1);
                    }

                    v.albedo_index = material ? material->albedo_index : -1;
                    v.normalmap_index = material ? material->normal_index : -1;
                    v.pbr_index = material ? material->pbr_index : -1;
                    v.occlusion_index = material ? material->occlusion_index : -1;
                    v.gltf_material = material ? material->gltf_material : 0;
                    if (material) v.pbr_factors = material->pbr_factors;

                    // prioritize material textures over vertex material ids.
                    v.extended_material_index = material ? osg::Vec2s(material->material1_index, material->material2_index) : osg::Vec2s(-1, -1);

                    // fallback is to use vertex stream to simulate material ids.
                    if (extended_material && v.extended_material_index[0] == -1 && v.extended_material_index[1] == -1)
                    {
                        int k = extended_material->getBinding() == osg::Array::BIND_PER_VERTEX ? i : 0;
                        v.extended_material_index = osg::Vec2s((*extended_material)[k], -1);
                    }

                    chonk->_vbo_store.emplace_back(std::move(v));

                    // per-vert materials:
                    chonk->_materials.push_back(_materialStack.top());
                }

                // assemble the elements set
                auto copy_indices = [this, chonk, vbo_offset](unsigned i0, unsigned i1, unsigned i2)
                    {
                        if (vbo_offset + i0 >= chonk->_vbo_store.size() ||
                            vbo_offset + i1 >= chonk->_vbo_store.size() ||
                            vbo_offset + i2 >= chonk->_vbo_store.size())
                        {
                            OE_WARN << LC << "Index out of range" << std::endl;
                            return;
                        }

                        chonk->_ebo_store.emplace_back(vbo_offset + i0);
                        chonk->_ebo_store.emplace_back(vbo_offset + i1);
                        chonk->_ebo_store.emplace_back(vbo_offset + i2);
                    };

                // we have to clone the geometry in order to get the indices.
                osg::ref_ptr<osg::Geometry> simple = new osg::Geometry(node, osg::CopyOp::SHALLOW_COPY);
                osg::TriangleIndexFunctor<SendIndices> sender;
                sender.func = copy_indices;
                simple->accept(sender);
            }

            if (_drawable && ig_chonk && ig)
            {
                ig_chonk->_lods.push_back({ 0u, ig_chonk->_ebo_store.size(), 
                    _far_pixel_scale, std::min(_near_pixel_scale, MAX_NEAR_PIXEL_SCALE) });

                ig_chonk->_box.init();

                for(const auto& encoded_matrix : ig->getMatrices())
                {
                    auto matrix = ig->decodeMatrix(encoded_matrix);
                    _drawable->add(ig_chonk, matrix * _transformStack.top());
                }
            }

            if (pushed) popStateSet();
        }
    };
}

namespace std {
    // std::hash specialization for ChonkMaterial
    template<> struct hash<ChonkMaterial> {
        inline size_t operator()(const ChonkMaterial& value) const {
            return hash_value_unsigned(
                value.albedo_index,
                value.normal_index,
                value.pbr_index,
                value.material1_index,
                value.material2_index);
        }
    };
}

GLubyte Chonk::NORMAL_TECHNIQUE_DEFAULT = 0;
GLubyte Chonk::NORMAL_TECHNIQUE_ZAXIS = 1;
GLubyte Chonk::NORMAL_TECHNIQUE_HEMISPHERE = 2;

unsigned Chonk::MATERIAL_VERTEX_SLOT = 7;

Chonk::Ptr
Chonk::create()
{
    return Ptr(new Chonk);
}

Chonk::Chonk()
{
    _globjects.resize(1);
}

bool
Chonk::add(osg::Node* node, ChonkFactory& factory)
{
    OE_SOFT_ASSERT_AND_RETURN(node != nullptr, false);
    OE_HARD_ASSERT(_lods.size() < 3);

    return factory.load(node, this, 1.0f, MAX_NEAR_PIXEL_SCALE);
}

bool
Chonk::add(
    osg::Node* node,
    float far_pixel_scale,
    float near_pixel_scale,
    ChonkFactory& factory)
{
    OE_SOFT_ASSERT_AND_RETURN(node != nullptr, false);
    OE_HARD_ASSERT(_lods.size() < 3);

    //unsigned offset = _ebo_store.size();
    return factory.load(node, this, far_pixel_scale, near_pixel_scale);
}

#define IMMUTABLE 0

const Chonk::DrawCommands&
Chonk::getOrCreateCommands(osg::State& state) const
{
    // all bindless objects that may be used across shared GCs:
    auto& gs = GLObjects::get(_globjects, state);

    if (gs.vbo == nullptr || !gs.vbo->valid())
    {
        // create_shared, because we will shares these static bindless GL objects across all OSG states.

        gs.vbo = GLBuffer::create_shared(GL_ARRAY_BUFFER_ARB, state);
        gs.vbo->bind();
        gs.vbo->debugLabel("Chonk geometry", "VBO " + _name);
        gs.vbo->bufferStorage(_vbo_store.size() * sizeof(VertexGPU), _vbo_store.data(), IMMUTABLE);

        gs.ebo = GLBuffer::create_shared(GL_ELEMENT_ARRAY_BUFFER_ARB, state);
        gs.ebo->bind();
        gs.ebo->debugLabel("Chonk geometry", "EBO " + _name);
        gs.ebo->bufferStorage(_ebo_store.size() * sizeof(element_t), _ebo_store.data(), IMMUTABLE);

        gs.commands.reserve(_lods.size());

        // for each variant:
        for (auto& lod : _lods)
        {
            if (gs.ebo->address() != 0 && gs.vbo->address() != 0)
            {
                DrawCommand command;

                command.cmd.count = lod.length;
                command.cmd.firstIndex = lod.offset;
                command.cmd.instanceCount = 1;
                command.cmd.baseInstance = 0;
                command.cmd.baseVertex = 0;
                command.indexBuffer.address = gs.ebo->address();
                command.indexBuffer.length = gs.ebo->size();
                command.vertexBuffer.address = gs.vbo->address();
                command.vertexBuffer.length = gs.vbo->size();

                gs.commands.emplace_back(std::move(command));
            }
        }

        gs.vbo->unbind();
        gs.ebo->unbind();
    }

    // Bindless buffers must be made resident in each context separately
    gs.vbo->makeResident(state);
    gs.ebo->makeResident(state);

    return gs.commands;
}

const osg::BoundingBoxf&
Chonk::getBound()
{
    if (!_box.valid())
    {
        for (auto index : _ebo_store)
            _box.expandBy(_vbo_store[index].position);
    }
    return _box;
}

ChonkFactory::ChonkFactory()
{
    getOrCreateTexture = getWeakTextureCacheFunction(_texcache, _texcache_mutex);
}

ChonkFactory::ChonkFactory(TextureArena* in_textures)
{
    textures = in_textures;
    getOrCreateTexture = getWeakTextureCacheFunction(_texcache, _texcache_mutex);
}

void
ChonkFactory::setGetOrCreateFunction(GetOrCreateFunction value)
{
    getOrCreateTexture = value;
}

namespace
{
    // Chonk records vertex/material data, not arbitrary OSG state. Only accept
    // the static, opaque subset for which dropping the source state is safe.
    struct ChonkEligibility : osg::NodeVisitor
    {
        bool valid = true;
        ChonkEligibility() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { }

        void inspect(osg::Node& node)
        {
            if (node.getNodeMask() != ~0u || node.getUpdateCallback() ||
                node.getEventCallback() || node.getCullCallback() ||
                node.getDataVariance() == osg::Object::DYNAMIC)
                valid = false;
            auto* ss = node.getStateSet();
            if (!ss) return;
            if (ss->getDataVariance() == osg::Object::DYNAMIC ||
                ss->requiresUpdateTraversal() || ss->requiresEventTraversal() ||
                (ss->getMode(GL_BLEND) & osg::StateAttribute::ON) ||
                ss->getRenderingHint() == osg::StateSet::TRANSPARENT_BIN)
                valid = false;
            auto cull = ss->getModeList().find(GL_CULL_FACE);
            if (cull != ss->getModeList().end() && !(cull->second & osg::StateAttribute::ON))
                valid = false;
            for (const auto& entry : ss->getAttributeList())
            {
                auto* attr = entry.second.first.get();
                if (auto* vp = dynamic_cast<VirtualProgram*>(attr))
                {
                    // The reader's color/PBR shaders and generated texture
                    // modulation are represented by Chonk's material fields.
                    // Other shader effects stay on the ordinary path.
                    bool gltf = false;
                    node.getUserValue(CHONK_HINT_LINEAR_COLOR, gltf);
                    if (!gltf) valid = false;
                    VirtualProgram::ShaderMap shaders;
                    vp->getShaderMap(shaders);
                    for (const auto& shader : shaders)
                    {
                        const auto& name = shader.second._shader->getName();
                        if (name != "oe_gltf_color_fs" && name != "oe_gltf_pbr_vs" && name != "oe_gltf_pbr_fs" &&
                            name != "oe_sg_vert_model" && name != "oe_sg_vert_view" && name != "oe_sg_frag")
                            valid = false;
                    }
                }
                else if (auto* face = dynamic_cast<osg::FrontFace*>(attr))
                {
                    if (face->getMode() != osg::FrontFace::COUNTER_CLOCKWISE) valid = false;
                }
                else if (auto* face = dynamic_cast<osg::CullFace*>(attr))
                {
                    if (face->getMode() != osg::CullFace::BACK) valid = false;
                }
                else valid = false;
            }
        }

        void apply(osg::Node& node) override
        {
            inspect(node);
            if (typeid(node) != typeid(osg::Group) && typeid(node) != typeid(osg::Geode) &&
                typeid(node) != typeid(osg::Node)) valid = false;
            if (valid) traverse(node);
        }

        void apply(osg::Transform& node) override
        {
            inspect(node);
            if (typeid(node) != typeid(osg::MatrixTransform) ||
                node.getReferenceFrame() != osg::Transform::RELATIVE_RF) valid = false;
            if (valid) traverse(node);
        }

        void apply(osg::Geometry& node) override
        {
            inspect(node);
            if (typeid(node) != typeid(osg::Geometry) ||
                !dynamic_cast<osg::Vec3Array*>(node.getVertexArray())) valid = false;
            for (auto& primitive : node.getPrimitiveSetList())
                if (primitive->getMode() != GL_TRIANGLES || primitive->getNumInstances() > 0)
                    valid = false;
        }
    };

    // The containing cell owns the source wrappers, so manager-wide reloads
    // and unloads still reach this consumer. Only rendering children change.
    class ExternalChonkGroup : public osg::Group
    {
    public:
        ExternalChonkGroup(osg::Node* source, std::shared_ptr<ChonkFactory> factory) :
            _source(source), _factory(std::move(factory))
        {
            setName(source->getName() + " Chonk instances");
            setUpdateCallback(new LambdaCallback<>([this](osg::NodeVisitor&)
                { refresh(); return true; }));
            rebuild();
        }

    private:
        struct Dependency {
            osg::ref_ptr<InstancedExternalNode> node;
            std::uint64_t revision;
        };
        osg::ref_ptr<osg::Node> _source;
        std::shared_ptr<ChonkFactory> _factory;
        std::vector<Dependency> _dependencies;

        static bool positiveAffine(const osg::Matrixd& m)
        {
            const osg::Vec3d a(m(0,0), m(0,1), m(0,2));
            const osg::Vec3d b(m(1,0), m(1,1), m(1,2));
            const osg::Vec3d c(m(2,0), m(2,1), m(2,2));
            // Keep mirrored/singular/projective placements on their original
            // path, which preserves branch-local front-face state.
            return std::isfinite((a ^ b) * c) && (a ^ b) * c > 1e-12 &&
                m(0,3) == 0.0 && m(1,3) == 0.0 && m(2,3) == 0.0 && m(3,3) == 1.0;
        }

        osg::ref_ptr<osg::Node> convert(osg::Node* node,
            const osg::Matrixd& parent, ChonkDrawable* drawable)
        {
            if (auto* external = dynamic_cast<InstancedExternalNode*>(node))
            {
                auto payload = external->getExternalNode();
                _dependencies.push_back({external, external->getRenderRevision()});
                if (!payload || !external->isUsingHardwareInstancing() || node->getNodeMask() != ~0u)
                    return node;
                for (const auto& matrix : external->getMatrices())
                    if (!positiveAffine(osg::Matrixd(matrix) * parent)) return node;
                auto chonk = _factory->getOrCreateChonk(payload.get());
                if (!chonk) return node;
                for (const auto& matrix : external->getMatrices())
                    drawable->add(chonk, osg::Matrixf(osg::Matrixd(matrix) * parent));
                return nullptr;
            }

            // Copy only structural nodes along converted paths. Ordinary
            // geometry and specialized nodes retain their original semantics.
            auto* group = node->asGroup();
            if (!group || node->getNodeMask() != ~0u || node->getUpdateCallback() ||
                node->getEventCallback() || node->getCullCallback() ||
                (typeid(*node) != typeid(osg::Group) && typeid(*node) != typeid(osg::MatrixTransform)))
                return node;
            ChonkEligibility eligibility;
            eligibility.inspect(*node);
            if (!eligibility.valid) return node;
            osg::Matrixd matrix = parent;
            if (auto* mt = dynamic_cast<osg::MatrixTransform*>(node))
            {
                if (mt->getReferenceFrame() != osg::Transform::RELATIVE_RF) return node;
                matrix = mt->getMatrix() * parent;
            }
            osg::ref_ptr<osg::Group> copy = dynamic_cast<osg::Group*>(node->clone(osg::CopyOp::SHALLOW_COPY));
            copy->removeChildren(0, copy->getNumChildren());
            for (unsigned i = 0; i < group->getNumChildren(); ++i)
            {
                auto child = convert(group->getChild(i), matrix, drawable);
                if (child) copy->addChild(child);
            }
            return copy->getNumChildren() ? copy.get() : nullptr;
        }

        void rebuild()
        {
            _dependencies.clear();
            osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable();
            drawable->setName(getName());
            auto residual = convert(_source.get(), osg::Matrixd(), drawable.get());
            // Do not change the shared Chonk render-bin StateSet.
            osg::ref_ptr<osg::Group> rendered = new osg::Group();
            rendered->getOrCreateStateSet()->setAttributeAndModes(new osg::CullFace(osg::CullFace::BACK));
            rendered->getOrCreateStateSet()->setAttribute(new osg::FrontFace(osg::FrontFace::COUNTER_CLOCKWISE));
            if (!drawable->empty()) rendered->addChild(drawable);
            removeChildren(0, getNumChildren());
            if (residual) addChild(residual);
            if (!drawable->empty()) addChild(rendered);
        }

        void refresh()
        {
            bool changed = false;
            for (auto& dependency : _dependencies)
            {
                dependency.node->refresh();
                changed |= dependency.node->getRenderRevision() != dependency.revision;
            }
            if (changed) rebuild();
        }
    };
}

Chonk::Ptr
ChonkFactory::getOrCreateChonk(osg::Node* node, float farScale, float nearScale)
{
    if (!node || !textures) return {};
    std::lock_guard<std::mutex> lock(_chonkCacheMutex);
    for (auto i = _chonkCache.begin(); i != _chonkCache.end(); )
    {
        auto cached = i->chonk.lock();
        if (!cached || !i->source.valid()) i = _chonkCache.erase(i);
        else
        {
            if (i->source.get() == node && i->farScale == farScale && i->nearScale == nearScale)
                return cached;
            ++i;
        }
    }
    ChonkEligibility eligibility;
    node->accept(eligibility);
    if (!eligibility.valid) return {};
    auto chonk = Chonk::create();
    chonk->name() = node->getName();
    // The manager owns node. Unlike load(Node*, Chonk*), this never runs an
    // optimizer on the canonical graph or changes its primitive arrays.
    Ripper ripper(chonk.get(), nullptr, textures.get(), getOrCreateTexture, farScale, nearScale);
    node->accept(ripper);
    if (chonk->_ebo_store.empty()) return {};
    chonk->_lods.push_back({0u, chonk->_ebo_store.size(), farScale, nearScale});
    chonk->getBound(); // publish an initialized immutable bound to paging threads
    _chonkCache.push_back({node, chonk, farScale, nearScale});
    return chonk;
}

osg::ref_ptr<osg::Node>
ChonkFactory::convertExternalInstances(osg::Node* node, std::shared_ptr<ChonkFactory> factory)
{
    if (!node || !factory || !factory->textures) return node;
    return new ExternalChonkGroup(node, std::move(factory));
}

bool
ChonkFactory::load(osg::Node* node, Chonk* chonk, float far_pixel_scale, float near_pixel_scale)
{
    OE_SOFT_ASSERT_AND_RETURN(node != nullptr, false);
    OE_SOFT_ASSERT_AND_RETURN(chonk != nullptr, false);
    OE_SOFT_ASSERT_AND_RETURN(textures.valid(), false, "ChonkFactory requires a valid TextureArena");
    
    OE_PROFILING_ZONE;

    // convert all primitive sets to indexed primitives
    osgUtil::Optimizer o;
    o.optimize(node, o.VERTEX_PRETRANSFORM | o.VERTEX_POSTTRANSFORM);
    
    // first count up the memory we need and allocate it
    Counter counter;
    node->accept(counter);

    chonk->_vbo_store.reserve(chonk->_vbo_store.size() + counter._numVerts);
    chonk->_ebo_store.reserve(chonk->_ebo_store.size() + counter._numElements);

    unsigned offset = chonk->_ebo_store.size();

    // rip geometry and textures into a new Asset object
    Ripper ripper(chonk, nullptr, textures.get(), getOrCreateTexture, far_pixel_scale, near_pixel_scale);
    node->accept(ripper);

    // dirty its bounding box
    if (chonk->_ebo_store.size() > 0)
    {
        chonk->_lods.push_back({ offset, chonk->_ebo_store.size(),
            far_pixel_scale, std::min(near_pixel_scale, MAX_NEAR_PIXEL_SCALE) });
    }
    chonk->_box.init();

    return (counter._numVerts > 0 && counter._numElements > 0);
}

bool
ChonkFactory::load(osg::Node* node, ChonkDrawable* drawable, float far_pixel_scale, float near_pixel_scale)
{
    OE_SOFT_ASSERT_AND_RETURN(node != nullptr, false);
    OE_SOFT_ASSERT_AND_RETURN(drawable != nullptr, false);
    OE_SOFT_ASSERT_AND_RETURN(textures.valid(), false, "ChonkFactory requires a valid TextureArena");

    OE_PROFILING_ZONE;

    Chonk::Ptr chonk;

    // first count up the memory we need and allocate it
    Counter counter;
    node->accept(counter);
    if (counter._numElements > 0 && counter._numVerts > 0)
    {
        chonk = Chonk::create();
        chonk->_vbo_store.reserve(counter._numVerts);
        chonk->_ebo_store.reserve(counter._numElements);
    }

    Ripper ripper(chonk.get(), drawable, textures.get(), getOrCreateTexture, far_pixel_scale, near_pixel_scale);
    node->accept(ripper);

    if (chonk && !chonk->_ebo_store.empty())
    {
        chonk->_lods.push_back({ 0u, chonk->_ebo_store.size(),
            far_pixel_scale, std::min(near_pixel_scale, MAX_NEAR_PIXEL_SCALE) });
        chonk->_box.init();
        drawable->add(chonk);
    }

    return true;
}


bool
ChonkDrawable::add(osg::Node* node, ChonkFactory& factory, float far_pixel_scale, float near_pixel_scale)
{
    OE_SOFT_ASSERT_AND_RETURN(node != nullptr, false);
    OE_PROFILING_ZONE;

    return factory.load(node, this, far_pixel_scale, near_pixel_scale);
}


#ifndef GL_VERTEX_ATTRIB_ARRAY_UNIFIED_NV
#define GL_VERTEX_ATTRIB_ARRAY_UNIFIED_NV 0x8F1E
#define GL_ELEMENT_ARRAY_UNIFIED_NV 0x8F1F
#endif

#ifndef GL_INT64_NV
#define GL_INT64_NV 0x140E
#define GL_UNSIGNED_INT64_NV 0x140F
#endif

namespace
{
    struct VADef {
        GLint size;
        GLenum type;
        GLboolean normalize;
        GLint offset;
    };
}

ChonkDrawable::ChonkDrawable(int renderBinNumber) :
    osg::Geometry(),
    _renderBinNumber(renderBinNumber)
{
    setName(typeid(*this).name());
    setUseDisplayList(false);
    setUseVertexArrayObject(false);

    // The ICO only accepts drawables for which VBOs or display lists are enabled:
    setUseVertexBufferObjects(true);
    installRenderBin(this);

    // stores the proxy geometry for intersections, etc.
    _proxy_verts = new osg::Vec3Array();
    setVertexArray(_proxy_verts);
    _proxy_indices = new osg::DrawElementsUInt(GL_TRIANGLES);
    addPrimitiveSet(_proxy_indices);

}

ChonkDrawable::~ChonkDrawable()
{
    //nop
}

void
ChonkDrawable::setRenderBinNumber(int value)
{
    _renderBinNumber = value;
    installRenderBin(this);
}

int
ChonkDrawable::getRenderBinNumber() const
{
    return _renderBinNumber;
}

void
ChonkDrawable::setBirthday(double value)
{
    _birthday = value;
    dirtyGLObjects();
}

double
ChonkDrawable::getBirthday() const
{
    return _birthday;
}

void
ChonkDrawable::setFadeNearFar(float nearRange, float farRange)
{
    _fadeNear = nearRange;
    _fadeFar = farRange;
    dirtyGLObjects();
}

void
ChonkDrawable::setAlphaCutoff(float value)
{
    _alphaCutoff = value;
    dirtyGLObjects();
}

void
ChonkDrawable::installRenderBin(ChonkDrawable* d)
{
    // map of render bin number to stateset
    static vector_map<int, osg::ref_ptr<osg::StateSet>> s_stateSets;

    static Mutex s_mutex;
    std::lock_guard<std::mutex> lock(s_mutex);

    auto& ss = s_stateSets[d->getRenderBinNumber()];
    if (!ss.valid())
    {
        ss = new osg::StateSet();
        ss->setDataVariance(ss->STATIC);
        ss->setRenderBinDetails(d->getRenderBinNumber(), "ChonkBin",
            (osg::StateSet::RenderBinMode)(ss->USE_RENDERBIN_DETAILS | ss->PROTECTED_RENDERBIN_DETAILS));
        
        // create the (shared) shader program if necessary:
        auto vp = Registry::instance()->getOrCreate<VirtualProgram>("vp.ChonkDrawable", []()
            {
                auto vp = new VirtualProgram();
                vp->setInheritShaders(true);
                vp->setName("ChonkDrawable");
                Shaders pkg;
                pkg.load(vp, pkg.Chonk);
                return vp;
            });

        ss->setAttribute(vp);
    }

    d->setStateSet(ss);
}

void
ChonkDrawable::setUseGPUCulling(bool value)
{
    if (_gpucull != value)
    {
        _gpucull = value;
        dirtyGLObjects();
    }
}

void
ChonkDrawable::dirtyGLObjects()
{
    // flag all graphics states as requiring an update:
    for (unsigned i = 0; i < _globjects.size(); ++i)
        _globjects[i]._dirty = true;
}

void
ChonkDrawable::add(Chonk::Ptr value)
{
    static const osg::Matrixf s_identity_xform;
    static const osg::Vec2f s_def_uv(0.0f, 0.0f);
    add(value, s_identity_xform, s_def_uv);
}

void
ChonkDrawable::add(Chonk::Ptr value, const osg::Matrixf& xform)
{
    static const osg::Vec2f s_def_uv(0.0f, 0.0f);
    add(value, xform, s_def_uv);
}

void
ChonkDrawable::add(Chonk::Ptr chonk, const osg::Matrixf& xform, const osg::Vec2f& local_uv)
{
    if (chonk)
    {
        std::lock_guard<std::mutex> lock(_m);

        Instance instance;
        instance.xform = xform;
        instance.uv = local_uv;
        instance.lod = 0;
        instance.visibility[0] = 0;
        instance.visibility[1] = 0;
        // A conservative spectral-norm bound: the largest absolute row sum
        // of A*A^T bounds its largest eigenvalue. Exact for orthogonal TRS
        // axes, and conservative for shear and reflections as well.
        osg::Vec3d axes[3] = {
            {xform(0,0), xform(0,1), xform(0,2)},
            {xform(1,0), xform(1,1), xform(1,2)},
            {xform(2,0), xform(2,1), xform(2,2)} };
        double maxRow = 0.0;
        for (unsigned i = 0; i < 3; ++i)
        {
            double row = 0.0;
            for (unsigned j = 0; j < 3; ++j)
                row += std::abs(axes[i] * axes[j]);
            maxRow = std::max(maxRow, row);
        }
        instance.radius = chonk->getBound().radius() * std::sqrt(maxRow);
        instance.alphaCutoff = 0.0f;
        instance.first_lod_cmd_index = 0;

        _batches[chonk].emplace_back(std::move(instance));

        // flag all graphics states as requiring an update:
        dirtyGLObjects();

        // flag the bounds for recompute
        dirtyBound();
    }
}

std::size_t ChonkDrawable::getNumInstances() const
{
    std::lock_guard<std::mutex> lock(_m);
    std::size_t count = 0;
    for (const auto& batch : _batches) count += batch.second.size();
    return count;
}

std::size_t ChonkDrawable::getNumBatches() const
{
    std::lock_guard<std::mutex> lock(_m);
    return _batches.size();
}

void
ChonkDrawable::drawImplementation(osg::RenderInfo& ri) const
{
    OE_HARD_ASSERT(false, "ChonkRenderBin::drawImplementation should never be called WHAT ARE YOU DOING");
}

void
ChonkDrawable::update_and_cull_batches(osg::State& state) const
{
    auto& globjects = GLObjects::get(_globjects, state);

    // if something changed, we need to refresh the GPU tables.
    update_gl_objects(globjects, state);

    if (_gpucull)
    {
        globjects.cull(state);
    }
}

void
ChonkDrawable::draw_batches(osg::State& state) const
{
    auto& globjects = GLObjects::get(_globjects, state);

    globjects.draw(state);
}


void
ChonkDrawable::update_gl_objects(GLObjects& globjects, osg::State& state) const
{
    // this method is called to pre-compile the drawable (usually by ICO)
    // or to update the GPU tables if something has changed.

    // if something changed, we need to refresh the GPU tables.
    if (globjects._dirty)
    {
        std::lock_guard<std::mutex> lock(_m);
        globjects._gpucull = _gpucull;
        globjects.update(_batches, this, _fadeNear, _fadeFar, _birthday, _alphaCutoff, state);
    }
}

osg::BoundingBox
ChonkDrawable::computeBoundingBox() const
{
    std::lock_guard<std::mutex> lock(_m);
    
    osg::BoundingBox result;

    for(auto& batch : _batches)
    {
        auto& chonk = batch.first;   

        auto& box = chonk->getBound();
        if (box.valid())
        {
            auto& instances = batch.second;
            for (auto& instance : instances)
            {
                for (unsigned i = 0; i < 8; ++i)
                {
                    result.expandBy(box.corner(i) * instance.xform);
                }
            }
        }
    }

    return result;
}

osg::BoundingSphere
ChonkDrawable::computeBound() const
{
    return osg::BoundingSphere(computeBoundingBox());
}

void
ChonkDrawable::compileGLObjects(osg::RenderInfo& ri) const
{
    auto& globjects = GLObjects::get(_globjects, *ri.getState());
    update_gl_objects(globjects, *ri.getState());
}

void
ChonkDrawable::resizeGLObjectBuffers(unsigned size)
{
    if (size > _globjects.size())
        _globjects.resize(size);
}

void
ChonkDrawable::releaseGLObjects(osg::State* state) const
{
    if (state)
    {
        GLObjects::get(_globjects, *state).release();
    }
    else
    {
        _globjects.setAllElementsTo(GLObjects());
    }
}

void
ChonkDrawable::refreshProxy() const
{
    if (_proxy_dirty)
    {
        std::lock_guard<std::mutex> lock(_m);

        _proxy_verts->clear();
        _proxy_indices->clear();

        // pre-allocate
        unsigned num_verts = 0, num_indices = 0;
        for (auto& batch : _batches)
        {
            Chonk::Ptr c = batch.first;
            for (auto& instance : batch.second)
            {
                num_verts += c->_vbo_store.size();
                num_indices += c->_ebo_store.size();
            }
        }
        _proxy_verts->reserve(num_verts);
        _proxy_indices->reserve(num_indices);

        // and populate
        unsigned offset = 0;
        for (auto& batch : _batches)
        {
            Chonk::Ptr c = batch.first;
            for (auto& instance : batch.second)
            {
                for (auto& vert : c->_vbo_store)
                {
                    _proxy_verts->push_back(vert.position * instance.xform);
                }
                for (auto& index : c->_ebo_store)
                {
                    _proxy_indices->push_back(index + offset);
                }
            }
            offset += c->_vbo_store.size();
        }

        _proxy_dirty = false;
    }
}

void
ChonkDrawable::accept(osg::PrimitiveFunctor& f) const
{
    if (!_batches.empty())
    {
        refreshProxy();
        osg::Geometry::accept(f);
    }
}

void
ChonkDrawable::accept(osg::PrimitiveIndexFunctor& f) const
{
    if (!_batches.empty())
    {
        refreshProxy();
        osg::Geometry::accept(f);
    }
}

void
ChonkDrawable::GLObjects::initialize(const osg::Object* host, osg::State& state)
{
    _ext = state.get<osg::GLExtensions>();

    void(GL_APIENTRY * gl_VertexAttribFormat)(GLuint, GLint, GLenum, GLboolean, GLuint);
    osg::setGLExtensionFuncPtr(gl_VertexAttribFormat, "glVertexAttribFormat");

    void(GL_APIENTRY * gl_VertexAttribIFormat)(GLuint, GLint, GLenum, GLuint);
    osg::setGLExtensionFuncPtr(gl_VertexAttribIFormat, "glVertexAttribIFormat");

    void(GL_APIENTRY * gl_VertexAttribLFormat)(GLuint, GLint, GLenum, GLuint);
    osg::setGLExtensionFuncPtr(gl_VertexAttribLFormat, "glVertexAttribLFormatNV");

    // Multidraw command:
    osg::setGLExtensionFuncPtr(
        _glMultiDrawElementsIndirectBindlessNV,
        "glMultiDrawElementsIndirectBindlessNV");
    OE_HARD_ASSERT(_glMultiDrawElementsIndirectBindlessNV != nullptr);

    void(GL_APIENTRY * glEnableClientState_)(GLenum);
    osg::setGLExtensionFuncPtr(glEnableClientState_, "glEnableClientState");
    OE_HARD_ASSERT(glEnableClientState_ != nullptr);

    // VAO:
    _vao = GLVAO::create(state);

    // start recording...
    state.bindVertexArrayObject(_vao->name());

    // must call AFTER bind
    _vao->debugLabel("Chonk drawable", "VAO " + host->getName());

    // required in order to use BindlessNV extension
    glEnableClientState_(GL_VERTEX_ATTRIB_ARRAY_UNIFIED_NV);
    glEnableClientState_(GL_ELEMENT_ARRAY_UNIFIED_NV);

    const VADef formats[] = {
        {3, GL_FLOAT,         GL_FALSE, offsetof(Chonk::VertexGPU, position)},
        {3, GL_FLOAT,         GL_FALSE, offsetof(Chonk::VertexGPU, normal)},
        {1, GL_UNSIGNED_BYTE, GL_FALSE, offsetof(Chonk::VertexGPU, normal_technique)},
        {4, GL_UNSIGNED_BYTE, GL_TRUE,  offsetof(Chonk::VertexGPU, color)},
        {2, GL_FLOAT,         GL_FALSE, offsetof(Chonk::VertexGPU, uv)},
        {3, GL_FLOAT,         GL_FALSE, offsetof(Chonk::VertexGPU, flex)},
        {1, GL_SHORT,         GL_FALSE, offsetof(Chonk::VertexGPU, albedo_index)},
        {1, GL_SHORT,         GL_FALSE, offsetof(Chonk::VertexGPU, normalmap_index)},
        {1, GL_SHORT,         GL_FALSE, offsetof(Chonk::VertexGPU, pbr_index)},
        {2, GL_SHORT,         GL_FALSE, offsetof(Chonk::VertexGPU, extended_material_index)},
        {1, GL_UNSIGNED_BYTE, GL_FALSE, offsetof(Chonk::VertexGPU, color_is_linear)},
        {1, GL_SHORT,         GL_FALSE, offsetof(Chonk::VertexGPU, occlusion_index)},
        {1, GL_UNSIGNED_BYTE, GL_FALSE, offsetof(Chonk::VertexGPU, gltf_material)},
        {4, GL_FLOAT,         GL_FALSE, offsetof(Chonk::VertexGPU, pbr_factors)}
    };

    // configure the format of each vertex attribute in our structure.
    for (unsigned location = 0; location < sizeof(formats)/sizeof(formats[0]); ++location)
    {
        const VADef& d = formats[location];
        if ((d.type == GL_INT) ||
            (d.type == GL_UNSIGNED_INT) ||
            (d.type == GL_SHORT) ||
            (d.type == GL_UNSIGNED_SHORT) ||
            (d.type == GL_BYTE && d.normalize == GL_FALSE) ||
            (d.type == GL_UNSIGNED_BYTE && d.normalize == GL_FALSE))
        {
            gl_VertexAttribIFormat(location, d.size, d.type, d.offset);
        }
        else
        {
            gl_VertexAttribFormat(location, d.size, d.type, d.normalize, d.offset);
        }
        _ext->glVertexAttribBinding(location, 0);
        _ext->glEnableVertexAttribArray(location);
    }

    // bind a "dummy buffer" that will record the stride, which is
    // simply the size of our vertex structure.
    _ext->glBindVertexBuffer(0, 0, 0, sizeof(Chonk::VertexGPU));

    // Finish recording
    state.unbindVertexArrayObject();
}

#define NEXT_MULTIPLE(X, Y) (((X+Y-1)/Y)*Y)

void
ChonkDrawable::GLObjects::update(
    const Batches& batches,
    const osg::Object* host,
    float fadeNear,
    float fadeFar,
    double birthday,
    float alphaCutoff,
    osg::State& state)
{
    OE_GL_ZONE_NAMED("update");

    if (_vao == nullptr || !_vao->valid())
    {
        initialize(host, state);
    }

    // build a list of draw commands, each of which will 
    // have N instances, one per chonk meta.
    _commands.clear();

    // record for each variant (LOD) of each chonk
    _chonk_lods.clear();

    // build a LUT of all instances by (gl_InstanceID + gl_BaseInstance).
    _all_instances.clear();

    std::size_t max_lod_count = 0;
    unsigned outputOffset = 0u;

    for (auto& batch : batches)
    {
        auto& chonk = batch.first;
        auto& instances = batch.second;

        unsigned first_lod_cmd_index = _commands.size();

        // For each chonk variant, set up an instanced draw command:
        const Chonk::DrawCommands& lod_commands = chonk->getOrCreateCommands(state);

        for(unsigned i=0; i< lod_commands.size(); ++i)
        {
            _commands.emplace_back(lod_commands[i]);
            // Each batch/LOD has room for every input instance. The compute
            // shader compacts inside this range, avoiding an atomic update of
            // every subsequent command for each visible instance.
            _commands.back().cmd.baseInstance = _gpucull ? outputOffset : _all_instances.size();
            if (!_gpucull)
                _commands.back().cmd.instanceCount = i == 0 ? instances.size() : 0;
            outputOffset += instances.size();

            if (_gpucull)
            {
                // record the bounding box of this chonk:
                auto& bs = chonk->getBound();

                ChonkLOD v;
                v.center = bs.center();
                v.radius = bs.radius();
                v.far_pixel_scale = chonk->_lods[i].far_pixel_scale;
                v.near_pixel_scale = chonk->_lods[i].near_pixel_scale;
                v.num_lods = chonk->_lods.size();
                v.alpha_cutoff = alphaCutoff;
                v.birthday = birthday;
                v.fade_near = fadeNear;
                v.fade_far = fadeFar;

                _chonk_lods.emplace_back(std::move(v));
            }
        }

        // append the instance data (transforms) and set
        // the index of the first lod command, which the compute
        // shader will need.
        for (auto& instance : instances)
        {
            _all_instances.push_back(instance);
            _all_instances.back().first_lod_cmd_index = first_lod_cmd_index;
            if (!_gpucull)
            {
                _all_instances.back().lod = 0;
                _all_instances.back().visibility[0] = 1.0f;
                _all_instances.back().alphaCutoff = alphaCutoff;
            }
        }

        // pad out the size of the instances array so it's a multiple of the
        // GPU culling workgroup size. Add "padding" instances will have the
        // first_lod_cmd_index member equal to -1, indicating an invalid instance.
        // The CS will check for this and discard them.
        unsigned workgroups = (_all_instances.size() + GPU_CULLING_LOCAL_WG_SIZE - 1) / GPU_CULLING_LOCAL_WG_SIZE;
        unsigned paddedSize = workgroups * GPU_CULLING_LOCAL_WG_SIZE;
        _all_instances.resize(paddedSize);

        max_lod_count = std::max(max_lod_count, lod_commands.size());
    }

    // set globals
    for (auto& lod : _chonk_lods)
    {
        lod.total_num_commands = _commands.size();
    }

    // Send to the GPU:
    if (!_instanceInputBuf)
    {
        // Per-culling instances:
        GLsizei sizeHint = _all_instances.size() * sizeof(Instance);
        _instanceInputBuf = GLBuffer::create(GL_SHADER_STORAGE_BUFFER, state, sizeHint, INPUT_BUF_CHUNK_SIZE);
        _instanceInputBuf->bind();
        _instanceInputBuf->debugLabel("Chonk drawable", "input " + host->getName());
        _instanceInputBuf->unbind();
    }
    _instanceInputBuf->uploadData(_all_instances, GL_STATIC_DRAW);

    // need to do this since it gets sent in cull() when gpu culling is on.
    if (!_commandBuf)
    {
        GLsizei sizeHint = _commands.size() * sizeof(Chonk::DrawCommand);
        _commandBuf = GLBuffer::create(GL_SHADER_STORAGE_BUFFER, state, sizeHint, COMMAND_BUF_CHUNK_SIZE);
        _commandBuf->bind();
        _commandBuf->debugLabel("Chonk drawable", "commands " + host->getName());
        _commandBuf->unbind();
    }

    if (_gpucull)
    {
        if (!_chonkBuf)
        {
            GLsizei sizeHint = _chonk_lods.size() * sizeof(ChonkLOD);
            _chonkBuf = GLBuffer::create(GL_SHADER_STORAGE_BUFFER, state, sizeHint, CHONK_BUF_CHUNK_SIZE);
            _chonkBuf->bind();
            _chonkBuf->debugLabel("Chonk drawable", "chonkbuf " + host->getName());
            _chonkBuf->unbind();
        }
        _chonkBuf->uploadData(_chonk_lods, GL_STATIC_DRAW);
        
        // just reserve space if necessary - make sure there's enough space
        // for 2 LODs for each instance so we can do transitioning!
        // If someday, we draw more than 2 LODs at a time, we'll need to
        // up this buffer size!!
        if (!_instanceOutputBuf)
        {
            GLsizei sizeHint = _all_instances.size() * sizeof(Instance) * 2;
            _instanceOutputBuf = GLBuffer::create(GL_SHADER_STORAGE_BUFFER, state, sizeHint, OUTPUT_BUF_CHUNK_SIZE);
            _instanceOutputBuf->bind();
            _instanceOutputBuf->debugLabel("Chonk drawable", "output " + host->getName());
            _instanceOutputBuf->unbind();
        }
        _instanceOutputBuf->uploadData(_instanceInputBuf->size() * 2, nullptr);
    }
    else
    {
        _commandBuf->uploadData(_commands);
    }

    _numInstances = _all_instances.size();
    _maxNumLODs = max_lod_count;

    _dirty = false;
}

void
ChonkDrawable::GLObjects::cull(osg::State& state)
{
    if (_commands.empty() || _commandBuf == nullptr)
        return;

    OE_GL_ZONE_NAMED("cull");

    // transmit the uniforms
    state.applyModelViewAndProjectionUniformsIfRequired();

    auto ext = _vao->ext();

    auto pcp = state.getLastAppliedProgramObject();
    OE_HARD_ASSERT(pcp != nullptr, "Check for shader errors!");
    PCPState& ps = _pcps[pcp];
    if (ps._passUL < 0)
    {
        ps._passUL = pcp->getUniformLocation("oe_pass");
    }

    // reset the command buffer values
    for (auto& command : _commands)
    {
        command.cmd.instanceCount = 0;
    }
    _commandBuf->uploadData(_commands);

    _instanceOutputBuf->bindBufferBase(0);
    _commandBuf->bindBufferBase(29);
    _chonkBuf->bindBufferBase(30);
    _instanceInputBuf->bindBufferBase(31);

    // 2-pass culling compute. 
    // Note. I tried separating this out so that all "pass ones"
    // would run, and then all "pass twos" afterwards, in an attempt
    // to avoid the memory barrier and multiple uniform sets.
    // It was slower. Maybe because if was offset by having to 
    // double-set the the matrix uniforms and the bindBufferBase
    // calls for each tile.
    // Also, removing the memory barrier seems to make no difference,
    // but it's the right thing to do

    // calculate number of workgroups.
    // (todo: this is probably unnecessary since we already padded the 
    // instances array before computing _numInstances)
    unsigned workgroups = (_numInstances + (GPU_CULLING_LOCAL_WG_SIZE-1)) / GPU_CULLING_LOCAL_WG_SIZE;

    // cull:
    ext->glUniform1i(ps._passUL, 0);
    ext->glDispatchCompute(workgroups, _maxNumLODs, 1);

    // compact:
    ext->glUniform1i(ps._passUL, 1);
    ext->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    ext->glDispatchCompute(workgroups, _maxNumLODs, 1);
}

void
ChonkDrawable::GLObjects::draw(osg::State& state)
{
    OE_GL_ZONE_NAMED("draw");

    if (_commandBuf == nullptr)
        return;

    // transmit the uniforms
    state.applyModelViewAndProjectionUniformsIfRequired();

    // bind the command list for drawing
    _commandBuf->bind(GL_DRAW_INDIRECT_BUFFER);

    // make the instance LUT visible in the shader
    // (use gl_InstanceID + gl_BaseInstance to access)
    if (_gpucull)
        _instanceOutputBuf->bindBufferBase(0);
    else
        _instanceInputBuf->bindBufferBase(0);

    // likely do not need this here since the cull leafs execute 
    // before the raw leafs. Reevaluate if necessary.
    //_ext->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    GLenum elementType = sizeof(Chonk::element_t) == sizeof(GLushort) ?
        GL_UNSIGNED_SHORT :
        GL_UNSIGNED_INT;

    _glMultiDrawElementsIndirectBindlessNV(
        GL_TRIANGLES,
        elementType,
        (const GLvoid*)0,
        _commands.size(),
        sizeof(Chonk::DrawCommand),
        1);
}

void
ChonkDrawable::GLObjects::release()
{
    _ext = nullptr;
    _vao = nullptr;
    _commandBuf = nullptr;
    _instanceInputBuf = nullptr;
    _instanceOutputBuf = nullptr;
    _chonkBuf = nullptr;
    _commands.clear();
    _pcps.clear(); // clear cached program-state map to prevent unbounded growth
    _dirty = true;
}

ChonkRenderBin::ChonkRenderBin() :
    osgUtil::RenderBin()
{
    setName("ChonkBin");
}

ChonkRenderBin::ChonkRenderBin(const ChonkRenderBin& rhs, const osg::CopyOp& op) :
    osgUtil::RenderBin(rhs, op),
    _cullSS(rhs._cullSS)
{
    if (!_cullSS.valid())
    {
        static Mutex m;
        std::lock_guard<std::mutex> lock(m);

        auto proto = static_cast<ChonkRenderBin*>(getRenderBinPrototype("ChonkBin"));
        if (!proto->_cullSS.valid())
        {
            proto->_cullSS = new osg::StateSet();

            // culling program
            Shaders pkg;
            std::string src = ShaderLoader::load(pkg.ChonkCulling, pkg);
            osg::Program* p = new osg::Program();
            p->addShader(new osg::Shader(osg::Shader::COMPUTE, src));
            proto->_cullSS->setAttribute(p);

            // Default far pixel scales per LOD.
            // We expect this to be overriden from above.
            proto->_cullSS->addUniform(new osg::Uniform("oe_lod_scale", osg::Vec4f(1, 1, 1, 1)));
        }
        _cullSS = proto->_cullSS;
    }

    // for each render bin instance, create a StateGraph that we
    // will use to track the OSG state properly when applying the
    // cull program
    _cull_sg = new osgUtil::StateGraph();
    _cull_sg->_stateset = _cullSS.get();
}


ChonkRenderBin::CullLeaf::CullLeaf(osgUtil::RenderLeaf* leaf) :
    CustomRenderLeaf(leaf)
{
    //nop
}

void 
ChonkRenderBin::CullLeaf::draw(osg::State& state)
{
    auto d = static_cast<const ChonkDrawable*>(getDrawable());
    d->update_and_cull_batches(state);
}

ChonkRenderBin::DrawLeaf::DrawLeaf(osgUtil::RenderLeaf* leaf, bool first, bool last) :
    CustomRenderLeaf(leaf),
    _first(first),
    _last(last)
{
    //nop
}

void
ChonkRenderBin::DrawLeaf::draw(osg::State& state)
{
    auto drawable = static_cast<const ChonkDrawable*>(getDrawable());

    if (_first)
    {
        auto& gl = ChonkDrawable::GLObjects::get(drawable->_globjects, state);
        state.bindVertexArrayObject(gl._vao->name());
        gl._vao->ext()->glMemoryBarrier(
            GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    }

    drawable->draw_batches(state);

    if (_last)
    {
        auto& gl = ChonkDrawable::GLObjects::get(drawable->_globjects, state);
        // Keep OSG's VAO cache in sync so conventional geometry can follow us.
        state.unbindVertexArrayObject();

#ifdef RESET_BUFFER_BASE_BINDINGS
        gl._ext->glBindBufferBase(GL_SHADER_STORAGE_BUFFER,  0, 0);
        gl._ext->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 29, 0);
        gl._ext->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 30, 0);
        gl._ext->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 31, 0);
#endif
    }
}

void
ChonkRenderBin::drawImplementation(
    osg::RenderInfo& ri,
    osgUtil::RenderLeaf*& previous)
{
    OE_GL_ZONE_NAMED("ChonkRenderBin");

    // copy everything to one level:
    copyLeavesFromStateGraphListToRenderLeafList();
    if (_renderLeafList.empty())
        return;

    osg::State& state = *ri.getState();

    // rearrange the bin so we have cull leaves followed by draw leaves,
    // each with the proper shader.

    // draw graph:
    osgUtil::StateGraph* draw_sg = _renderLeafList.front()->_parent;
    draw_sg->_leaves.clear();

    // cull graph:
    _cull_sg->_parent = draw_sg;
    _cull_sg->_leaves.clear();

    for(unsigned i=0; i<_renderLeafList.size(); ++i)
    {
        auto leaf = _renderLeafList[i];
        bool first = (i == 0);
        bool last = (i == _renderLeafList.size() - 1);

        _cull_sg->addLeaf(new CullLeaf(leaf));
        draw_sg->addLeaf(new DrawLeaf(leaf, first, last));
    }

    // install the new state graphs:
    _renderLeafList.clear();
    _stateGraphList.clear();
    _stateGraphList.push_back(_cull_sg.get());
    _stateGraphList.push_back(draw_sg);

    // dispatch.
    osgUtil::RenderBin::drawImplementation(ri, previous);
}

void
ChonkRenderBin::releaseSharedGLObjects(osg::State* state)
{
    auto proto = static_cast<ChonkRenderBin*>(osgUtil::RenderBin::getRenderBinPrototype("ChonkBin"));
    if (proto->_cullSS.valid())
        proto->_cullSS->releaseGLObjects(state);
}


ChonkFactory::GetOrCreateFunction ChonkFactory::getWeakTextureCacheFunction(
    std::vector<Texture::WeakPtr>& cache,
    std::mutex& cache_mutex)
{
    return [&cache, &cache_mutex](osg::Texture* osgTex, bool& isNew)
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto* image = osgTex->getImage(0);
            for (auto iter = cache.begin(); iter != cache.end(); )
            {
                Texture::Ptr cache_entry = iter->lock();
                if (cache_entry)
                {
                    auto* cached = cache_entry->osgTexture().get();
                    if (osgTex->getInternalFormat() == cached->getInternalFormat() &&
                        osgTex->getWrap(osg::Texture::WRAP_S) == cached->getWrap(osg::Texture::WRAP_S) &&
                        osgTex->getWrap(osg::Texture::WRAP_T) == cached->getWrap(osg::Texture::WRAP_T) &&
                        osgTex->getFilter(osg::Texture::MIN_FILTER) == cached->getFilter(osg::Texture::MIN_FILTER) &&
                        osgTex->getFilter(osg::Texture::MAG_FILTER) == cached->getFilter(osg::Texture::MAG_FILTER) &&
                        ImageUtils::areEquivalent(image, cached->getImage(0)))
                    {
                        isNew = false;
                        return cache_entry;
                    }
                    ++iter;
                }
                else
                {
                    // dead entry, remove it
                    //TODO this is a bad function to call on a vector, fix it
                    iter = cache.erase(iter);
                }
            }

            isNew = true;
            auto new_texture = Texture::create(osgTex);
            cache.emplace_back(Texture::WeakPtr(new_texture));
            return new_texture;
        };
}
