/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include "Chonk"
#include "Color"
#include "GLUtils"
#include "Metrics"
#include "VirtualProgram"
#include <osgEarth/ChonkRenderPass>
#include "Shaders"
#include "Utils"
#include "DrawInstanced"
#include "Registry"
#include "PBRMaterial"
#include "Notify"
#include "ImageUtils"
#include "Math"
#include "InstancedExternalNode"
#include "ExternalNode"
#include "CameraUtils"
#include <osg/MatrixTransform>
#include <osg/CullFace>
#include <osg/FrontFace>
#include <osg/FrameBufferObject>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <cstdlib>
#include <stdexcept>
#include <limits>
#include <algorithm>

#include <osgUtil/Optimizer>

#undef LC
#define LC "[Chonk] "

using namespace osgEarth;

static_assert(sizeof(Chonk::VertexGPU) == 52, "Chonk vertices must remain compact");
static_assert(offsetof(Chonk::VertexGPU, material_index) == 30, "Material ID occupies vertex padding");

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
#define OUTPUT_BUF_CHUNK_SIZE (128 * 1024)
#define CHONK_BUF_CHUNK_SIZE 256

namespace
{
    // Read the diagnostic storage policy once, before the first subtree load.
    // Only an explicit zero restores the original merged-geometry Ripper path.
    bool useChonkGeometryPages()
    {
        static const bool enabled = []()
        {
            const char* value = std::getenv("OSGEARTH_CHONK_GEOMETRY_PAGES");
            return !value || std::string(value) != "0";
        }();
        return enabled;
    }

    // Packed placements use affine, orientation-preserving transforms. Reject
    // singular/projective/mirrored matrices so normal and face semantics survive.
    bool validPageTransform(const osg::Matrixd& matrix)
    {
        for (unsigned i = 0; i < 16; ++i)
            if (!std::isfinite(matrix.ptr()[i])) return false;
        const osg::Vec3d a(matrix(0,0), matrix(0,1), matrix(0,2));
        const osg::Vec3d b(matrix(1,0), matrix(1,1), matrix(1,2));
        const osg::Vec3d c(matrix(2,0), matrix(2,1), matrix(2,2));
        return (a ^ b)*c > 1e-12 && matrix(0,3) == 0 && matrix(1,3) == 0 &&
            matrix(2,3) == 0 && matrix(3,3) == 1;
    }

    //! Scans one image for texels below full alpha. Exact for uncompressed and DXT data,
    //! which includes every Basis transcode target; unknown formats with alpha report true.
    bool imageMayBeTranslucent(const osg::Image& image)
    {
        switch (image.getPixelFormat())
        {
        case GL_RGB:
        case 0x80E0: // GL_BGR
        case GL_RED:
        case 0x8227: // GL_RG
        case GL_LUMINANCE:
        case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            return false; // no alpha channel: samples read alpha = 1
        case GL_RGBA:
        case GL_BGRA:
        case GL_ALPHA:
        case GL_LUMINANCE_ALPHA:
        case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
        case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
        case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
            return image.isImageTranslucent();
        default:
            return true;
        }
    }

    //! Conservatively reports whether sampling a texture can yield alpha below one.
    //! Thread-safe; caches per live image and modification count, since scans are O(texels).
    bool textureMayBeTranslucent(const osg::Texture* texture)
    {
        if (!texture) return false;
        struct Entry { osg::observer_ptr<const osg::Image> image; unsigned modified; bool translucent; };
        static std::mutex mutex;
        static std::unordered_map<const osg::Image*, Entry> cache;
        for (unsigned i = 0; i < texture->getNumImages(); ++i)
        {
            const osg::Image* image = texture->getImage(i);
            if (!image) continue;
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto entry = cache.find(image);
                if (entry != cache.end() && entry->second.image.get() == image &&
                    entry->second.modified == image->getModifiedCount())
                {
                    if (entry->second.translucent) return true;
                    continue;
                }
            }
            const bool translucent = imageMayBeTranslucent(*image);
            std::lock_guard<std::mutex> lock(mutex);
            // Drop entries for released images before they can alias a new allocation.
            if (cache.size() > 4096)
                for (auto e = cache.begin(); e != cache.end(); )
                    e = e->second.image.valid() ? std::next(e) : cache.erase(e);
            cache[image] = { image, image->getModifiedCount(), translucent };
            if (translucent) return true;
        }
        return false;
    }

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
        TextureArena* _textures = nullptr;
        ChonkFactory::GetOrCreateFunction _getOrCreateTexture;
        std::stack<ChonkMaterial::Ptr> _materialStack;
        std::stack<bool> _alphaStack; // parallel to _materialStack: albedo may be translucent
        std::stack<osg::Matrix> _transformStack;
        std::unordered_map<osg::Texture*, Texture::Ptr> _textureLUT;
        // Optional page writer runs with the current inherited material stack.
        std::function<void(osg::Geometry&, const osg::Matrixd&)> writeMesh;
        std::size_t maxBytes = std::numeric_limits<GLsizei>::max();

        const unsigned ALBEDO_UNIT = 0;
        const unsigned NORMAL_UNIT = 1;
        const unsigned PBR_UNIT = 2;

        const unsigned MAT1_SLOT = 0;
        const unsigned MAT2_SLOT = 1;

        const unsigned FLEXOR_SLOT = 3;
        const unsigned NORMAL_TECHNIQUE_SLOT = 6;
        const unsigned EXTENDED_MATERIAL_SLOT = Chonk::MATERIAL_VERTEX_SLOT;

        // Resolve optional texture references to arena slots and share one
        // material record with other Chonks using the same texture arena.
        ChonkMaterial::Ptr reuseOrCreateMaterial(
            Texture::Ptr albedo_tex,
            Texture::Ptr normal_tex,
            Texture::Ptr pbr_tex,
            Texture::Ptr mat1_tex,
            Texture::Ptr mat2_tex,
            Texture::Ptr ao_tex = nullptr,
            const osg::Vec4& layoutAndFactors = osg::Vec4(0, 1, 1, 1))
        {
            int albedo_index = _textures->find(albedo_tex);
            int normal_index = _textures->find(normal_tex);
            int pbr_index = _textures->find(pbr_tex);
            int mat1_index = _textures->find(mat1_tex);
            int mat2_index = _textures->find(mat2_tex);

            return _textures->getMaterialArena()->getOrCreate(*_textures,
                {{albedo_index, normal_index, pbr_index, mat1_index, mat2_index}},
                osg::Vec2i(mat1_index, mat2_index), _textures->find(ao_tex), layoutAndFactors);
        }

        // Pin each distinct material once for the lifetime of this Chonk,
        // avoiding a shared_ptr and reference-count update for every vertex.
        void retainMaterial(Chonk* chonk, const ChonkMaterial::Ptr& material)
        {
            if (std::find(chonk->_materials.begin(), chonk->_materials.end(), material) == chonk->_materials.end())
                chonk->_materials.push_back(material);
        }

        // Route geometry to one legacy asset or an optional page callback.
        // The texture arena and resolver must remain valid for this traversal.
        Ripper(Chonk* chonk, TextureArena* textures, ChonkFactory::GetOrCreateFunction func) :
            _chonk(chonk),
            _textures(textures),
            _getOrCreateTexture(func)
        {
            // Use the "active chidren" mode to only bring in default switch
            // and osgSim::MultiSwitch children for now. -gw
            setTraversalMode(TRAVERSE_ACTIVE_CHILDREN);
            setNodeMaskOverride(~0);

            _transformStack.push(osg::Matrix());
        }

        // Append to legacy storage transactionally. A page callback owns its
        // separate transaction; failures propagate as false without publishing it.
        bool rip(osg::Node& node)
        {
            const auto vertices = _chonk ? _chonk->_vbo_store.size() : 0;
            const auto elements = _chonk ? _chonk->_ebo_store.size() : 0;
            const auto materials = _chonk ? _chonk->_materials.size() : 0;
            osg::ref_ptr<MaterialArena> arena = _chonk ? _chonk->_materialArena.get() : nullptr;
            try
            {
                _materialStack.push(reuseOrCreateMaterial(nullptr, nullptr, nullptr, nullptr, nullptr));
                _alphaStack.push(false);
                node.accept(*this);
            }
            catch (const std::exception& error)
            {
                // A full arena must not publish truncated geometry or wrap a
                // 16-bit ID. Leave an existing Chonk/Drawable intact.
                if (_chonk)
                {
                    _chonk->_vbo_store.resize(vertices);
                    _chonk->_ebo_store.resize(elements);
                    _chonk->_materials.resize(materials);
                    _chonk->_materialArena = arena;
                }
                OE_WARN << LC << error.what() << std::endl;
                return false;
            }
            return true;
        }

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
                Texture::Ptr albedo_tex, normal_tex, pbr_tex, ao_tex;
                Texture::Ptr material_tex1, material_tex2;
                bool albedoAlpha = false;

                auto combo = dynamic_cast<PBRTexture*>(stateset->getTextureAttribute(ALBEDO_UNIT, osg::StateAttribute::TEXTURE));
                if (combo)
                {
                    albedo_tex = addTexture(combo->albedo);
                    normal_tex = addTexture(combo->normal);
                    pbr_tex = addTexture(combo->pbr);
                    ao_tex = addTexture(combo->occlusion);
                    albedoAlpha = albedo_tex && textureMayBeTranslucent(combo->albedo.get());
                }
                else
                {
                    albedo_tex = addTexture(ALBEDO_UNIT, stateset);
                    normal_tex = addTexture(NORMAL_UNIT, stateset);
                    pbr_tex = addTexture(PBR_UNIT, stateset);
                    albedoAlpha = albedo_tex && textureMayBeTranslucent(dynamic_cast<osg::Texture*>(
                        stateset->getTextureAttribute(ALBEDO_UNIT, osg::StateAttribute::TEXTURE)));
                }

                material_tex1 = findExternalTexture(MAT1_SLOT, stateset);
                material_tex2 = findExternalTexture(MAT2_SLOT, stateset);

                if (albedo_tex || normal_tex || pbr_tex || ao_tex || combo)
                {
                    ChonkMaterial::Ptr material = reuseOrCreateMaterial(
                        albedo_tex, normal_tex, pbr_tex, material_tex1, material_tex2,
                        ao_tex, combo ? combo->layoutAndFactors : osg::Vec4(0, 1, 1, 1));
                    _materialStack.push(material);
                    _alphaStack.push(albedoAlpha);
                    pushed = true;
                }
            }
            return pushed;
        }

        void popStateSet()
        {
            _materialStack.pop();
            _alphaStack.pop();
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

        // Encode directly into destination arrays using the inherited material.
        // Throws on invalid arrays/indices or overflow; the caller owns rollback.
        void appendGeometry(osg::Geometry& node, Chonk* chonk, const osg::Matrixd& matrix)
        {
            if (chonk)
            {
                if (chonk->_materialArena && chonk->_materialArena != _textures->getMaterialArena())
                    throw std::invalid_argument("A Chonk cannot mix MaterialArenas");
                chonk->_materialArena = _textures->getMaterialArena();
                auto verts = dynamic_cast<osg::Vec3Array*>(node.getVertexArray());
                if (!verts) throw std::invalid_argument("Chonk requires Vec3 vertices");
                const std::size_t numVerts = verts->size();
                if ((chonk->_vbo_store.size() + numVerts) >
                    (maxBytes - chonk->_ebo_store.size()*sizeof(Chonk::element_t))/sizeof(Chonk::VertexGPU))
                    throw std::length_error("Chonk geometry page is full");

                unsigned vbo_offset = chonk->_vbo_store.size();

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

                // Validate attribute lengths before indexing source arrays.
                const osg::Array* arrays[] = { colors, packed_colors, normals,
                    normal_techniques, flexors, extended_material, uv2s, uv3s };
                for (const auto* array : arrays)
                    if (array && array->getNumElements() <
                        (array->getBinding() == osg::Array::BIND_PER_VERTEX ? numVerts : 1u))
                        throw std::invalid_argument("Chonk attribute array is too short");

                auto& material = _materialStack.top();
                retainMaterial(chonk, material);
                // Record alpha-capable materials so each LOD can be classified after ripping.
                const bool alphaMaterial = _alphaStack.top();
                if (alphaMaterial)
                    chonk->_alphaMaterials.insert(material->index);
                std::unordered_map<GLshort, ChonkMaterial::Ptr> extendedVariants;
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

                    v.material_index = material->index;
                    // Legacy vertex IDs are not texture slots. Preserve their
                    // output separately from the material's resolved handles.
                    if (extended_material && material->extended == osg::Vec2i(-1, -1))
                    {
                        int k = extended_material->getBinding() == osg::Array::BIND_PER_VERTEX ? i : 0;
                        GLshort id = (*extended_material)[k];
                        auto& variant = extendedVariants[id];
                        if (!variant)
                        {
                            variant = _textures->getMaterialArena()->getOrCreate(*_textures,
                                material->textures, osg::Vec2i(id, -1), material->occlusion, material->layoutAndFactors);
                            retainMaterial(chonk, variant);
                            if (alphaMaterial) // same albedo as its base material
                                chonk->_alphaMaterials.insert(variant->index);
                        }
                        v.material_index = variant->index;
                    }

                    chonk->_vbo_store.emplace_back(std::move(v));

                }

                // assemble the elements set
                // Check mesh-local indices before rebasing, avoiding overflow
                // that could incorrectly address a previous mesh in this page.
                auto copy_indices = [this, chonk, vbo_offset, numVerts](unsigned i0, unsigned i1, unsigned i2)
                    {
                        if (i0 >= numVerts || i1 >= numVerts || i2 >= numVerts)
                            throw std::invalid_argument("Chonk index out of range");
                        if (chonk->_ebo_store.size() + 3 >
                            (maxBytes - chonk->_vbo_store.size()*sizeof(Chonk::VertexGPU))/sizeof(Chonk::element_t))
                            throw std::length_error("Chonk geometry page is full");

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

        }

        // Route each mesh to legacy storage or a page, with inherited materials.
        void apply(osg::Geometry& node)
        {
            bool pushed = pushStateSet(node.getStateSet());
            if (writeMesh)
                writeMesh(node, _transformStack.top());
            else
                appendGeometry(node, _chonk, _transformStack.top());

            if (pushed) popStateSet();
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

        gs.commands.clear();
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
                command.cmd.baseVertex = lod.base_vertex;
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

bool
Chonk::hasAlphaTest(std::size_t first, std::size_t end) const
{
    end = std::min(end, _vbo_store.size());
    for (std::size_t i = first; i < end; ++i)
    {
        const auto& v = _vbo_store[i];
        if (v.color.a() < 255 || (!_alphaMaterials.empty() && _alphaMaterials.count(v.material_index)))
            return true;
    }
    return false;
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
    bool enforceChonkEligibility()
    {
        // Startup-only restriction policy. Keep this fixed so cached Chonks
        // cannot outlive the eligibility policy under which they were built.
        static const bool enforce = []()
        {
            const char* value = std::getenv("OSGEARTH_CHONK_ENFORCE_ELIGIBILITY");
            return value && std::string(value) == "1";
        }();
        return enforce;
    }

    // Optional restrictions to the static, opaque subset whose scene/state
    // semantics Chonk can preserve. Report the first rejection per conversion.
    struct ChonkEligibility : osg::NodeVisitor
    {
        bool valid = true;
        ChonkEligibility() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { }

        void reject(const osg::Node& node, const std::string& reason)
        {
            if (!valid) return;
            valid = false;
            OE_WARN << LC << "Eligibility rejected " << node.className()
                << " \"" << node.getName() << "\": " << reason << std::endl;
        }

        void inspect(osg::Node& node)
        {
            if (!valid) return;
            if (node.getNodeMask() != ~0u)
                reject(node, "non-default node mask");
            else if (node.getUpdateCallback() || node.getEventCallback() || node.getCullCallback())
                reject(node, "node callbacks");
            else if (node.getDataVariance() == osg::Object::DYNAMIC)
                reject(node, "dynamic node");
            if (!valid) return;
            auto* ss = node.getStateSet();
            if (!ss) return;
            if (ss->getDataVariance() == osg::Object::DYNAMIC)
                reject(node, "dynamic StateSet");
            else if (ss->requiresUpdateTraversal() || ss->requiresEventTraversal())
                reject(node, "StateSet callbacks");
            else if ((ss->getMode(GL_BLEND) & osg::StateAttribute::ON) ||
                ss->getRenderingHint() == osg::StateSet::TRANSPARENT_BIN)
                reject(node, "blending or transparent render bin");
            auto cull = ss->getModeList().find(GL_CULL_FACE);
            if (cull != ss->getModeList().end() && !(cull->second & osg::StateAttribute::ON))
                reject(node, "face culling is disabled (two-sided rendering)");
            if (!valid) return;
            for (const auto& entry : ss->getAttributeList())
            {
                auto* attr = entry.second.first.get();
                if (auto* vp = dynamic_cast<VirtualProgram*>(attr))
                {
                    // ShaderGenerator's color and PBR functions and the
                    // standard PBRTexture program are represented by Chonk's
                    // material fields. Other shader effects stay on the
                    // ordinary path. The standard program always treats
                    // colors as linear; generated programs need the hint.
                    bool generated = false;
                    VirtualProgram::ShaderMap shaders;
                    vp->getShaderMap(shaders);
                    for (const auto& shader : shaders)
                    {
                        const auto& name = shader.second._shader->getName();
                        if (name == "oe_sg_vert_model" || name == "oe_sg_vert_view" || name == "oe_sg_frag")
                            generated = true;
                        else if (!PBRTexture::isStandardProgramFunction(name))
                            reject(node, "unsupported shader: " + name);
                    }
                    bool linearColor = false;
                    node.getUserValue(CHONK_HINT_LINEAR_COLOR, linearColor);
                    if (generated && !linearColor) reject(node, "shader state lacks the supported linear-color hint");
                }
                else if (auto* face = dynamic_cast<osg::FrontFace*>(attr))
                {
                    if (face->getMode() != osg::FrontFace::COUNTER_CLOCKWISE)
                        reject(node, "clockwise front faces");
                }
                else if (auto* face = dynamic_cast<osg::CullFace*>(attr))
                {
                    if (face->getMode() != osg::CullFace::BACK)
                        reject(node, "cull mode is not BACK");
                }
                else reject(node, "unsupported state attribute: " + std::string(attr->className()));
            }
        }

        void apply(osg::Node& node) override
        {
            inspect(node);
            if (typeid(node) != typeid(osg::Group) && typeid(node) != typeid(osg::Geode) &&
                typeid(node) != typeid(osg::Node)) reject(node, "specialized node type");
            if (valid) traverse(node);
        }

        void apply(osg::Transform& node) override
        {
            inspect(node);
            if (typeid(node) != typeid(osg::MatrixTransform) ||
                node.getReferenceFrame() != osg::Transform::RELATIVE_RF)
                reject(node, "specialized or absolute transform");
            if (valid) traverse(node);
        }

        void apply(osg::Geometry& node) override
        {
            inspect(node);
            if (typeid(node) != typeid(osg::Geometry) ||
                !dynamic_cast<osg::Vec3Array*>(node.getVertexArray()))
                reject(node, "geometry requires a plain osg::Geometry with Vec3 vertices");
            for (auto& primitive : node.getPrimitiveSetList())
                if (primitive->getMode() != GL_TRIANGLES || primitive->getNumInstances() > 0)
                    reject(node, "primitive is not an uninstanced triangle list");
        }
    };

    // Pages require an explicit static subset irrespective of the legacy opt-in
    // policy. Reject state whose inheritance the material-only Ripper cannot encode.
    struct PageEligibility : ChonkEligibility
    {
        // Validate local state while leaving the common ancestor state untouched.
        void inspectPage(osg::Node& node)
        {
            inspect(node);
            auto* ss = node.getStateSet();
            if (!ss) return;
            if (!ss->getUniformList().empty() || !ss->getDefineList().empty() ||
                ss->getRenderBinMode() != osg::StateSet::INHERIT_RENDERBIN_DETAILS)
                reject(node, "local uniforms, defines, or render bin");
            for (const auto& mode : ss->getModeList())
                if ((mode.second & (osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED)) ||
                    !((mode.first == GL_BLEND && !(mode.second & osg::StateAttribute::ON)) ||
                      (mode.first == GL_CULL_FACE && (mode.second & osg::StateAttribute::ON))))
                    reject(node, "local rendering mode");
            for (unsigned unit = 0; unit < ss->getTextureAttributeList().size(); ++unit)
                for (const auto& attribute : ss->getTextureAttributeList()[unit])
                {
                    if (attribute.second.second & (osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED))
                        reject(node, "texture override/protected inheritance");
                    auto* texture = dynamic_cast<osg::Texture2D*>(attribute.second.first.get());
                    auto* pbr = dynamic_cast<PBRTexture*>(attribute.second.first.get());
                    int extendedSlot = -1;
                    if (texture) texture->getUserValue(CHONK_HINT_EXTENDED_MATERIAL_SLOT, extendedSlot);
                    if ((!texture && !pbr) || (pbr && unit != 0) ||
                        (unit > 2 && extendedSlot != 0 && extendedSlot != 1))
                        reject(node, "unsupported texture state attribute");
                }
            for (const auto& unit : ss->getTextureModeList())
                for (const auto& mode : unit)
                    if (mode.first != GL_TEXTURE_2D || mode.second != osg::StateAttribute::ON)
                        reject(node, "unsupported texture mode");
        }

        // Only ordinary static containers are safe to flatten inside a mesh.
        void apply(osg::Node& node) override
        {
            inspectPage(node);
            if (typeid(node) != typeid(osg::Group) && typeid(node) != typeid(osg::Geode) &&
                typeid(node) != typeid(osg::Node)) reject(node, "specialized node type");
            if (valid) traverse(node);
        }

        // Preserve affine normal and winding semantics for transforms baked by Ripper.
        void apply(osg::Transform& node) override
        {
            inspectPage(node);
            auto* transform = dynamic_cast<osg::MatrixTransform*>(&node);
            if (typeid(node) != typeid(osg::MatrixTransform) ||
                node.getReferenceFrame() != osg::Transform::RELATIVE_RF ||
                !transform || !validPageTransform(transform->getMatrix()))
                reject(node, "unsupported mesh transform");
            if (valid) traverse(node);
        }

        // Defer detailed vertex/index checks to the transactional append writer.
        void apply(osg::Geometry& node) override
        {
            inspectPage(node);
            ChonkEligibility::apply(node);
            // The encoder supports overall or per-vertex values only. Other
            // bindings and custom attributes would silently change shading.
            const osg::Array* arrays[] = { node.getNormalArray(), node.getColorArray(),
                node.getTexCoordArray(0), node.getTexCoordArray(3),
                node.getVertexAttribArray(6), node.getVertexAttribArray(Chonk::MATERIAL_VERTEX_SLOT) };
            for (auto* array : arrays)
                if (array && (array->getDataVariance() == osg::Object::DYNAMIC ||
                    (array->getBinding() != osg::Array::BIND_OVERALL &&
                     array->getBinding() != osg::Array::BIND_PER_VERTEX)))
                    reject(node, "unsupported attribute binding or dynamic array");
            if ((node.getNormalArray() && !dynamic_cast<osg::Vec3Array*>(node.getNormalArray())) ||
                (node.getColorArray() && !dynamic_cast<osg::Vec4Array*>(node.getColorArray()) &&
                    !dynamic_cast<osg::Vec4ubArray*>(node.getColorArray())) ||
                (node.getTexCoordArray(0) && !dynamic_cast<osg::Vec2Array*>(node.getTexCoordArray(0)) &&
                    !dynamic_cast<osg::Vec3Array*>(node.getTexCoordArray(0))) ||
                (node.getTexCoordArray(3) && !dynamic_cast<osg::Vec3Array*>(node.getTexCoordArray(3))) ||
                (node.getVertexAttribArray(6) && !dynamic_cast<osg::UByteArray*>(node.getVertexAttribArray(6))) ||
                (node.getVertexAttribArray(Chonk::MATERIAL_VERTEX_SLOT) &&
                    !dynamic_cast<osg::ShortArray*>(node.getVertexAttribArray(Chonk::MATERIAL_VERTEX_SLOT))))
                reject(node, "unsupported vertex attribute type");
            if (node.getVertexArray() && node.getVertexArray()->getDataVariance() == osg::Object::DYNAMIC)
                reject(node, "dynamic vertex array");
            for (unsigned i = 0; i < node.getNumTexCoordArrays(); ++i)
                if (i != 0 && i != 3 && node.getTexCoordArray(i))
                    reject(node, "unsupported texture coordinate channel");
            for (unsigned i = 0; i < node.getNumVertexAttribArrays(); ++i)
                if (i != 6 && i != Chonk::MATERIAL_VERTEX_SLOT && node.getVertexAttribArray(i))
                    reject(node, "unsupported custom vertex attribute");
            if (node.getSecondaryColorArray() || node.getFogCoordArray())
                reject(node, "secondary colors or fog coordinates");
            for (const auto& primitive : node.getPrimitiveSetList())
                if (primitive->getDataVariance() == osg::Object::DYNAMIC)
                    reject(node, "dynamic primitive set");
        }
    };

    // Convert only the stock material state that Chonk encodes in its vertex
    // and material tables. Unknown effects stay on their original scene path.
    osg::ref_ptr<osg::Geometry> pageGeometry(osg::Geometry& source,
        const osg::StateSet* inherited, bool& twoSided)
    {
        if (typeid(source) != typeid(osg::Geometry) || source.getDrawCallback()) return {};
        // Own user values before adding encoding hints; vertex arrays stay shared.
        osg::ref_ptr<osg::Geometry> copy = new osg::Geometry(source,
            osg::CopyOp::DEEP_COPY_USERDATA | osg::CopyOp::DEEP_COPY_OBJECTS);
        osg::ref_ptr<osg::StateSet> state = inherited ?
            new osg::StateSet(*inherited, osg::CopyOp::SHALLOW_COPY) : new osg::StateSet();
        const auto special = osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
        for (const auto& attribute : state->getAttributeList())
            if (attribute.second.second & special) return {};
        for (const auto& unit : state->getTextureAttributeList())
            for (const auto& attribute : unit)
                if (attribute.second.second & special) return {};
        for (const auto& unit : state->getTextureModeList())
            for (const auto& mode : unit)
                if (mode.second & special) return {};
        for (const auto& uniform : state->getUniformList())
            if (uniform.second.second & special) return {};

        auto* program = state->getAttribute(osg::StateAttribute::PROGRAM);
        const bool standardPBR = program == PBRTexture::getOrCreateProgram();
        if (program && !standardPBR) return {};
        auto* pbr = dynamic_cast<PBRTexture*>(state->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
        if (standardPBR)
        {
            if (!pbr) return {};
            // The descriptor replaces exactly the standard program's bindings;
            // mismatched uniforms/maps may represent a caller's custom effect.
            const char* samplers[] = {"oe_pbr_texture_albedo", "oe_pbr_texture_normal",
                "oe_pbr_texture_pbr", "oe_pbr_texture_occlusion"};
            osg::Texture* maps[] = {pbr->albedo.get(), pbr->normal.get(), pbr->pbr.get(), pbr->occlusion.get()};
            for (unsigned i = 0; i < 4; ++i)
            {
                int unit = -1;
                auto* uniform = state->getUniform(samplers[i]);
                if (!uniform || !uniform->get(unit) || unit != int(i+1) ||
                    state->getTextureAttribute(i+1, osg::StateAttribute::TEXTURE) != maps[i]) return {};
                // Core-profile texture enables may be absent; the standard
                // PBR shader samples the validated binding regardless of them.
                state->removeUniform(samplers[i]);
                state->removeTextureAttribute(i+1, osg::StateAttribute::TEXTURE);
                state->removeTextureMode(i+1, GL_TEXTURE_2D);
            }
            osg::Vec4 factors;
            int flags = -1;
            auto* factorsUniform = state->getUniform("oe_pbr_texture_layoutAndFactors");
            auto* flagsUniform = state->getUniform("oe_pbr_texture_flags");
            if (!factorsUniform || !factorsUniform->get(factors) || factors != pbr->layoutAndFactors ||
                !flagsUniform || !flagsUniform->get(flags) ||
                flags != ((pbr->normal ? 1 : 0) | (pbr->pbr ? 2 : 0) | (pbr->occlusion ? 4 : 0))) return {};
            state->removeUniform("oe_pbr_texture_layoutAndFactors");
            state->removeUniform("oe_pbr_texture_flags");
            state->removeAttribute(osg::StateAttribute::PROGRAM);
            copy->setUserValue(CHONK_HINT_LINEAR_COLOR, true);
        }
        auto cull = state->getModeList().find(GL_CULL_FACE);
        twoSided = cull != state->getModeList().end() && !(cull->second & osg::StateAttribute::ON);
        if (twoSided)
        {
            if (cull->second & special) return {};
            // Preserve this raster state on a separate output group, instead of
            // rejecting the mesh or changing the shared Chonk render-bin state.
            state->removeMode(GL_CULL_FACE);
        }
        copy->setStateSet(state);
        return copy;
    }

    // Cheap, read-only gate for automatic scene conversion. A lone ordinary
    // draw cannot amortize Chonk's compute/indirect buffers. Count primitive
    // sets, not vertices: one Geometry with several sets can still combine draws.
    // This is deliberately conservative; normal conversion performs eligibility
    // checks after this gate finds a potential batch. Explicit Ripper calls do
    // not use this policy. No geometry/material encoding or GL work occurs here.
    struct SceneBatchingCheck : osg::NodeVisitor
    {
        struct Dependency {
            osg::ref_ptr<ExternalNode> node;
            osg::ref_ptr<osg::Node> payload;
        };
        unsigned draws = 0;
        bool instanced = false;
        std::vector<Dependency> dependencies;

        // Include masked branches conservatively; stop counting once batching
        // may help. Retain snapshots only when a singleton needs reload tracking.
        SceneBatchingCheck() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { setNodeMaskOverride(~0u); }
        // Existing instance batches keep their conversion policy, including a
        // single prototype used by many placements and an adjacent unique mesh.
        bool canBatch() const { return instanced || draws > 1; }
        // Count external payloads without flattening or modifying their wrappers.
        // Empty/singleton ordinary assets remain observable for later reloads.
        void apply(osg::Node& node) override
        {
            if (canBatch()) return;
            if (dynamic_cast<InstancedExternalNode*>(&node))
                instanced = true;
            else if (auto* external = dynamic_cast<ExternalNode*>(&node))
            {
                auto payload = external->getExternalNode();
                dependencies.push_back({external, payload});
                if (payload) payload->accept(*this);
            }
            else traverse(node);
        }
        // Two primitive sets suffice to retain the existing conversion path.
        void apply(osg::Geometry& geometry) override
        {
            draws = std::min(2u, draws + std::min(2u, geometry.getNumPrimitiveSets()));
        }
    };

    // Accumulate a cell's unique meshes in capped pages, retaining original
    // geometry on individual append failures. Used only during rebuild/update.
    struct ScenePages
    {
        ChonkFactory& factory;
        ChonkDrawable* drawable;
        std::unique_ptr<ChonkGeometryPageBuilder> builder;
        std::vector<ChonkDrawable::PagePlacement> placements;
        unsigned count = 0;
        bool valid = true;

        // Borrow the factory/output for this synchronous scene conversion.
        ScenePages(ChonkFactory& f, ChonkDrawable* d) : factory(f), drawable(d) { reset(); }
        // Start an empty page using the configured payload cap.
        void reset() { builder.reset(new ChonkGeometryPageBuilder(factory, factory.geometryPageSize)); }
        // Publish a completed page; a failure makes the whole conversion fall back.
        void finish()
        {
            auto page = builder->finish();
            if (page) valid &= drawable->add(page, placements);
            placements.clear();
        }
        // Keep object-local vertices and encode the accumulated cell transform.
        bool add(osg::Geometry* geometry, const osg::Matrixd& matrix)
        {
            osg::Matrixf placement(matrix);
            if (!validPageTransform(osg::Matrixd(placement))) return false;
            auto id = builder->addMesh(geometry);
            if (id == ChonkGeometryPage::INVALID_MESH && !placements.empty())
            {
                finish();
                reset();
                id = builder->addMesh(geometry);
            }
            if (id == ChonkGeometryPage::INVALID_MESH) return false;
            placements.push_back({id, placement, osg::Vec2f()});
            ++count;
            return true;
        }
    };

    // The containing cell owns the source wrappers, so manager-wide reloads
    // and unloads still reach this consumer. Only rendering children change.
    class ExternalChonkGroup : public osg::Group
    {
    public:
        // Own the borrowed graph/factory until all converted geometry is retired.
        ExternalChonkGroup(osg::Node* source, std::shared_ptr<ChonkFactory> factory, bool packUnique = false) :
            _source(source), _factory(std::move(factory)), _packUnique(packUnique)
        {
            setName(source->getName() + " Chonk geometry");
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
        std::vector<SceneBatchingCheck::Dependency> _ordinaryDependencies;
        bool _packUnique;

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

        // Remove only successfully packed leaves, cloning structural paths for
        // residual content and resolving local state without changing the input.
        osg::ref_ptr<osg::Node> convert(osg::Node* node, const osg::Matrixd& parent, ChonkDrawable* drawable,
            const osg::StateSet* inherited = nullptr, ScenePages* oneSided = nullptr, ScenePages* twoSided = nullptr)
        {
            if (auto* external = dynamic_cast<InstancedExternalNode*>(node))
            {
                auto payload = external->getExternalNode();
                _dependencies.push_back({external, external->getRenderRevision()});
                if (!payload || !external->isUsingHardwareInstancing() || node->getNodeMask() != ~0u)
                    return node;
                for (const auto& matrix : external->getMatrices())
                    if (!positiveAffine(osg::Matrixd(matrix) * parent)) return node;

                // TODO: if we want pixel-size culling, pass near/far scales to the factory.
                // example:
                // auto chonk = _factory->getOrCreateChonk(payload.get(), 1.0f, 100.0f);
                // By default no scales are set meaning no pixel-size culling.

                auto chonk = _factory->getOrCreateChonk(payload.get());
                if (!chonk) return node;
                for (const auto& matrix : external->getMatrices())
                    drawable->add(chonk, osg::Matrixf(osg::Matrixd(matrix) * parent));
                return nullptr;
            }

            if (node->getNodeMask() != ~0u || node->getUpdateCallback() ||
                node->getEventCallback() || node->getCullCallback() ||
                node->getDataVariance() == osg::Object::DYNAMIC) return node;
            osg::ref_ptr<osg::StateSet> effective;
            if (oneSided)
            {
                effective = inherited ? new osg::StateSet(*inherited, osg::CopyOp::SHALLOW_COPY) : new osg::StateSet();
                if (auto* local = node->getStateSet())
                {
                    auto* program = local->getAttribute(osg::StateAttribute::PROGRAM);
                    if (local->getDataVariance() == osg::Object::DYNAMIC || local->requiresUpdateTraversal() ||
                        local->requiresEventTraversal() || local->getRenderBinMode() != osg::StateSet::INHERIT_RENDERBIN_DETAILS ||
                        (program && program != PBRTexture::getOrCreateProgram()))
                    {
                        oneSided = twoSided = nullptr;
                    }
                    else effective->merge(*local);
                }
            }
            if (oneSided)
            {
                if (auto* geometry = dynamic_cast<osg::Geometry*>(node))
                {
                    bool doubleSided = false;
                    auto packed = pageGeometry(*geometry, effective, doubleSided);
                    if (packed && (doubleSided ? twoSided : oneSided)->add(packed, parent)) return nullptr;
                    return node;
                }
                if (auto* external = dynamic_cast<ExternalNode*>(node))
                {
                    auto payload = external->getExternalNode();
                    _ordinaryDependencies.push_back({external, payload});
                    if (!payload) return node;
                    auto residual = convert(payload, parent, drawable, effective, oneSided, twoSided);
                    if (!residual) return nullptr;
                    osg::ref_ptr<osg::Group> copy = new osg::Group(*external, osg::CopyOp::SHALLOW_COPY);
                    copy->removeChildren(0, copy->getNumChildren());
                    copy->addChild(residual);
                    return copy;
                }
            }

            // Copy only structural nodes along converted paths. Ordinary
            // geometry and specialized nodes retain their original semantics.
            auto* group = node->asGroup();
            if (!group || node->getNodeMask() != ~0u || node->getUpdateCallback() ||
                node->getEventCallback() || node->getCullCallback() ||
                (typeid(*node) != typeid(osg::Group) && typeid(*node) != typeid(osg::MatrixTransform) &&
                    !(oneSided && typeid(*node) == typeid(osg::Geode))))
                return node;
            if (enforceChonkEligibility())
            {
                ChonkEligibility eligibility;
                eligibility.inspect(*node);
                if (!eligibility.valid) return node;
            }
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
                auto child = convert(group->getChild(i), matrix, drawable, effective, oneSided, twoSided);
                if (child) copy->addChild(child);
            }
            return copy->getNumChildren() ? copy.get() : nullptr;
        }

        // Rebuild after load/reload, publishing only complete pages and retaining
        // fallback branches. Cull modes use separate ordinary OSG state groups.
        void rebuild()
        {
            _dependencies.clear();
            _ordinaryDependencies.clear();
            if (_packUnique)
            {
                SceneBatchingCheck check;
                _source->accept(check);
                if (!check.canBatch())
                {
                    // Keep the original graph/state and allocate no Chonk pages.
                    // A reload may turn this singleton into a useful batch, so
                    // retain its external wrappers and reconsider on update.
                    _ordinaryDependencies = std::move(check.dependencies);
                    removeChildren(0, getNumChildren());
                    addChild(_source);
                    return;
                }
            }
            osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable();
            drawable->setName(getName());
            osg::ref_ptr<ChonkDrawable> doubleSided;
            std::unique_ptr<ScenePages> frontPages, bothPages;
            if (_packUnique)
            {
                // ChonkBin applies one draw StateGraph to the entire bin. Keep
                // the two raster policies in distinct bins so back faces survive.
                doubleSided = new ChonkDrawable(drawable->getRenderBinNumber() + 1);
                doubleSided->setName(getName() + " two-sided pages");
                frontPages.reset(new ScenePages(*_factory, drawable));
                bothPages.reset(new ScenePages(*_factory, doubleSided));
            }
            auto residual = convert(_source.get(), osg::Matrixd(), drawable.get(), nullptr,
                frontPages.get(), bothPages.get());
            if (_packUnique)
            {
                frontPages->finish();
                bothPages->finish();
            }
            if (_packUnique && (!frontPages->valid || !bothPages->valid))
            {
                removeChildren(0, getNumChildren());
                addChild(_source);
                return;
            }
            // Do not change the shared Chonk render-bin StateSet.
            osg::ref_ptr<osg::Group> rendered = new osg::Group();
            rendered->getOrCreateStateSet()->setAttributeAndModes(new osg::CullFace(osg::CullFace::BACK));
            rendered->getOrCreateStateSet()->setAttribute(new osg::FrontFace(osg::FrontFace::COUNTER_CLOCKWISE));
            if (!drawable->empty()) rendered->addChild(drawable);
            removeChildren(0, getNumChildren());
            if (residual) addChild(residual);
            if (!drawable->empty()) addChild(rendered);
            if (doubleSided && !doubleSided->empty())
            {
                osg::ref_ptr<osg::Group> renderedBoth = new osg::Group();
                renderedBoth->getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
                renderedBoth->getOrCreateStateSet()->setAttribute(new osg::FrontFace(osg::FrontFace::COUNTER_CLOCKWISE));
                renderedBoth->addChild(doubleSided);
                addChild(renderedBoth);
            }
            if (_packUnique)
                OE_DEBUG << LC << getName() << ": packed " << frontPages->count + bothPages->count
                    << " ordinary meshes" << std::endl;
        }

        // Ordinary assets publish new payload pointers on reload/unload; retain
        // their wrappers so the manager can still notify this converted cell.
        void refresh()
        {
            bool changed = false;
            for (auto& dependency : _dependencies)
            {
                dependency.node->refresh();
                changed |= dependency.node->getRenderRevision() != dependency.revision;
            }
            for (auto& dependency : _ordinaryDependencies)
                changed |= dependency.node->getExternalNode() != dependency.payload;
            if (changed) rebuild();
        }
    };
}

ChonkGeometryPage::ChonkGeometryPage() : _storage(Chonk::create())
{
    _storage->name() = "Chonk geometry page";
}

const Chonk::DrawCommands&
ChonkGeometryPage::getOrCreateCommands(osg::State& state) const
{
    return _storage->getOrCreateCommands(state);
}

ChonkGeometryPageBuilder::ChonkGeometryPageBuilder(ChonkFactory& factory, std::size_t maxBytes) :
    _textures(factory.textures), _getOrCreateTexture(factory.getOrCreateTexture),
    _page(new ChonkGeometryPage()),
    _maxBytes(std::min(maxBytes, std::size_t(std::numeric_limits<GLsizei>::max())))
{
}

ChonkGeometryPageBuilder
ChonkFactory::createGeometryPageBuilder(std::size_t maxBytes)
{
    return ChonkGeometryPageBuilder(*this, maxBytes == 0 ? geometryPageSize : maxBytes);
}

ChonkGeometryPage::MeshId
ChonkGeometryPageBuilder::append(const std::function<bool(Chonk*)>& writer,
    float farScale, float nearScale)
{
    if (!_page || !_textures || !std::isfinite(farScale) || !std::isfinite(nearScale) ||
        farScale < 0 || nearScale < farScale)
        return ChonkGeometryPage::INVALID_MESH;
    auto& storage = *_page->_storage;
    const auto vertices = storage._vbo_store.size(), indices = storage._ebo_store.size();
    const auto materials = storage._materials.size(), lods = storage._lods.size();
    const auto meshes = _page->_meshes.size();
    auto arena = storage._materialArena;
    try
    {
        if (!writer(&storage) || storage._ebo_store.size() == indices)
            throw std::invalid_argument("Chonk mesh has no valid triangles");
        if (storage._vbo_store.size()*sizeof(Chonk::VertexGPU) +
            storage._ebo_store.size()*sizeof(Chonk::element_t) > _maxBytes)
            throw std::length_error("Chonk geometry page is full");
        ChonkGeometryPage::Mesh mesh;
        mesh.firstVertex = unsigned(vertices);
        mesh.vertexCount = unsigned(storage._vbo_store.size() - vertices);
        mesh.firstIndex = unsigned(indices);
        mesh.indexCount = unsigned(storage._ebo_store.size() - indices);
        for (std::size_t i = indices; i < storage._ebo_store.size(); ++i)
        {
            auto& index = storage._ebo_store[i];
            if (index < vertices || index >= storage._vbo_store.size())
                throw std::invalid_argument("Chonk mesh index escapes its vertex range");
            const auto& position = storage._vbo_store[index].position;
            if (!std::isfinite(position.x()) || !std::isfinite(position.y()) || !std::isfinite(position.z()))
                throw std::invalid_argument("Chonk mesh has non-finite positions");
            mesh.bounds.expandBy(position);
            index -= mesh.firstVertex;
        }
        if (!std::isfinite(mesh.bounds.radius()))
            throw std::invalid_argument("Chonk mesh bounds overflow");
        storage._lods.push_back({ mesh.firstIndex, mesh.indexCount, farScale, nearScale, mesh.firstVertex });
        storage._lods.back().alphaTested = storage.hasAlphaTest(mesh.firstVertex, mesh.firstVertex + mesh.vertexCount);
        _page->_meshes.push_back(mesh);
        return unsigned(meshes);
    }
    catch (const std::exception& error)
    {
        storage._vbo_store.resize(vertices);
        storage._ebo_store.resize(indices);
        storage._materials.resize(materials);
        storage._lods.resize(lods);
        storage._materialArena = arena;
        _page->_meshes.resize(meshes);
        OE_DEBUG << LC << error.what() << std::endl;
        return ChonkGeometryPage::INVALID_MESH;
    }
}

ChonkGeometryPage::MeshId
ChonkGeometryPageBuilder::addMesh(osg::Node* node, float farScale, float nearScale)
{
    if (!node || !_page || !_textures) return ChonkGeometryPage::INVALID_MESH;
    PageEligibility eligibility;
    node->accept(eligibility);
    if (!eligibility.valid) return ChonkGeometryPage::INVALID_MESH;
    // The same encoder writes legacy assets and pages, including all materials.
    return append([&](Chonk* storage)
    {
        Ripper ripper(storage, _textures, _getOrCreateTexture);
        ripper.maxBytes = _maxBytes;
        return ripper.rip(*node);
    }, farScale, nearScale);
}

ChonkGeometryPage::Ptr
ChonkGeometryPageBuilder::finish()
{
    auto page = std::move(_page);
    return page && !page->_meshes.empty() ? page : ChonkGeometryPage::Ptr();
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
    if (enforceChonkEligibility())
    {
        ChonkEligibility eligibility;
        node->accept(eligibility);
        if (!eligibility.valid) return {};
    }
    auto chonk = Chonk::create();
    chonk->name() = node->getName();
    // The manager owns node. Unlike load(Node*, Chonk*), this never runs an
    // optimizer on the canonical graph or changes its primitive arrays.
    Ripper ripper(chonk.get(), textures.get(), getOrCreateTexture);
    if (!ripper.rip(*node)) return {};
    if (chonk->_ebo_store.empty()) return {};
    chonk->_lods.push_back({0u, chonk->_ebo_store.size(), farScale, nearScale});
    chonk->_lods.back().alphaTested = chonk->hasAlphaTest(0, chonk->_vbo_store.size());
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

osg::ref_ptr<osg::Node>
ChonkFactory::convertScene(osg::Node* node, std::shared_ptr<ChonkFactory> factory)
{
    if (!node || !factory || !factory->textures) return node;
    const bool packUnique = useChonkGeometryPages();
    if (packUnique)
    {
        SceneBatchingCheck check;
        node->accept(check);
        // Static singleton models need neither conversion nor an update wrapper.
        // External assets retain the wrapper so reloads can change this decision.
        if (!check.canBatch() && check.dependencies.empty()) return node;
    }
    return new ExternalChonkGroup(node, std::move(factory), packUnique);
}

bool
ChonkFactory::load(osg::Node* node, Chonk* chonk, float far_pixel_scale, float near_pixel_scale)
{
    OE_SOFT_ASSERT_AND_RETURN(node != nullptr, false);
    OE_SOFT_ASSERT_AND_RETURN(chonk != nullptr, false);
    OE_SOFT_ASSERT_AND_RETURN(textures.valid(), false, "ChonkFactory requires a valid TextureArena");
    OE_SOFT_ASSERT_AND_RETURN(!chonk->_materialArena ||
        chonk->_materialArena == textures->getMaterialArena(), false, "A Chonk cannot mix MaterialArenas");
    
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
    const std::size_t firstVertex = chonk->_vbo_store.size();

    // rip geometry and textures into a new Asset object
    Ripper ripper(chonk, textures.get(), getOrCreateTexture);
    if (!ripper.rip(*node)) return false;

    // dirty its bounding box
    if (chonk->_ebo_store.size() > 0)
    {
        chonk->_lods.push_back({ offset, chonk->_ebo_store.size() - offset,
            far_pixel_scale, std::min(near_pixel_scale, MAX_NEAR_PIXEL_SCALE) });
        chonk->_lods.back().alphaTested = chonk->hasAlphaTest(firstVertex, chonk->_vbo_store.size());
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

    if (!useChonkGeometryPages())
    {
        // Original layout: merge ordinary geometry into one Chonk, and retain
        // a separate Chonk for each InstanceGeometry with all its placements.
        auto merged = Chonk::create();
        Counter counter;
        node->accept(counter);
        merged->_vbo_store.reserve(counter._numVerts);
        merged->_ebo_store.reserve(counter._numElements);
        ChonkDrawable::Batches staged;
        Ripper ripper(nullptr, textures.get(), getOrCreateTexture);
        // Finalize one legacy asset before encoding placements from its bound.
        auto finishChonk = [&](const Chonk::Ptr& chonk)
        {
            chonk->_lods.push_back({0u, chonk->_ebo_store.size(), far_pixel_scale, near_pixel_scale});
            chonk->_lods.back().alphaTested = chonk->hasAlphaTest(0, chonk->_vbo_store.size());
            chonk->getBound();
        };
        // Reuse the same encoder and material stack for both storage policies.
        ripper.writeMesh = [&](osg::Geometry& geometry, const osg::Matrixd& transform)
        {
            if (auto* instanced = dynamic_cast<DrawInstanced::InstanceGeometry*>(&geometry))
            {
                if (instanced->getMatrices().empty()) return;
                auto chonk = Chonk::create();
                ripper.appendGeometry(geometry, chonk.get(), osg::Matrixd());
                if (chonk->_ebo_store.empty()) return;
                finishChonk(chonk);
                auto& instances = staged[chonk];
                instances.reserve(instanced->getMatrices().size());
                for (const auto& encoded : instanced->getMatrices())
                    instances.push_back(ChonkDrawable::makeInstance(chonk->getBound(),
                        osg::Matrixf(instanced->decodeMatrix(encoded) * transform), osg::Vec2f()));
            }
            else
                ripper.appendGeometry(geometry, merged.get(), transform);
        };
        if (!ripper.rip(*node)) return false;
        if (!merged->_ebo_store.empty())
        {
            finishChonk(merged);
            staged[merged].push_back(ChonkDrawable::makeInstance(
                merged->getBound(), osg::Matrixf(), osg::Vec2f()));
        }
        if (staged.empty()) return false;
        std::lock_guard<std::mutex> lock(drawable->_m);
        if (!drawable->acceptsArena(textures->getMaterialArena())) return false;
        drawable->_batches.reserve(drawable->_batches.size() + staged.size());
        drawable->_batches.merge(staged);
        drawable->_materialArena = textures->getMaterialArena();
        drawable->_proxy_dirty = true;
        drawable->dirtyGLObjects();
        drawable->dirtyBound();
        return true;
    }

    auto builder = createGeometryPageBuilder();
    std::vector<ChonkDrawable::PagePlacement> placements;
    ChonkDrawable::PageBatches staged;
    // Stage all pages before publishing to keep failed conversions atomic.
    auto finishPage = [&]()
    {
        auto page = builder.finish();
        if (page && !placements.empty())
        {
            auto& output = staged[page];
            output.reserve(placements.size());
            for (const auto& placement : placements)
                output.push_back({placement.mesh, ChonkDrawable::makeInstance(
                    page->meshes()[placement.mesh].bounds, placement.transform, placement.tileUV)});
        }
        placements.clear();
    };
    Ripper ripper(nullptr, textures.get(), getOrCreateTexture);
    ripper.maxBytes = builder._maxBytes;
    // A discovered geometry is a culling unit. Parent transforms remain in its
    // placement instead of baking large translated coordinates into float vertices.
    ripper.writeMesh = [&](osg::Geometry& geometry, const osg::Matrixd& transform)
    {
        auto* instanced = dynamic_cast<DrawInstanced::InstanceGeometry*>(&geometry);
        if (instanced && instanced->getMatrices().empty()) return;
        if (!geometry.getVertexArray() || geometry.getVertexArray()->getNumElements() == 0) return;
        auto writer = [&](Chonk* storage)
        {
            ripper.appendGeometry(geometry, storage, osg::Matrixd());
            return true;
        };
        auto mesh = builder.append(writer, far_pixel_scale, near_pixel_scale);
        if (mesh == ChonkGeometryPage::INVALID_MESH && !builder._page->_meshes.empty())
        {
            finishPage();
            builder._page.reset(new ChonkGeometryPage());
            mesh = builder.append(writer, far_pixel_scale, near_pixel_scale);
        }
        if (mesh == ChonkGeometryPage::INVALID_MESH)
            throw std::invalid_argument("Cannot pack Chonk geometry (invalid or exceeds page capacity)");
        // Reject unsupported placement transforms before publishing any pages.
        auto place = [&](const osg::Matrixd& matrix)
        {
            const osg::Matrixf local(matrix);
            if (!validPageTransform(osg::Matrixd(local)))
                throw std::invalid_argument("Unsupported Chonk page placement transform");
            if (!std::isfinite(ChonkDrawable::makeInstance(
                builder._page->_meshes[mesh].bounds, local, osg::Vec2f()).radius))
                throw std::invalid_argument("Chonk page placement radius overflows");
            placements.push_back({mesh, local, osg::Vec2f()});
        };
        if (instanced)
            for (const auto& encoded : instanced->getMatrices())
                place(instanced->decodeMatrix(encoded) * transform);
        else
            place(transform);
    };
    if (!ripper.rip(*node)) return false;
    finishPage();
    if (staged.empty()) return false;
    std::lock_guard<std::mutex> lock(drawable->_m);
    if (!drawable->acceptsArena(textures->getMaterialArena())) return false;
    drawable->_pageBatches.reserve(drawable->_pageBatches.size() + staged.size());
    drawable->_pageBatches.merge(staged);
    drawable->_materialArena = textures->getMaterialArena();
    drawable->_proxy_dirty = true;
    drawable->dirtyGLObjects();
    drawable->dirtyBound();
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
    ++_contentRevision;
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
        if (!acceptsArena(chonk->_materialArena))
        {
            OE_WARN << LC << "A ChonkDrawable cannot mix MaterialArenas" << std::endl;
            return;
        }
        _batches[chonk].push_back(makeInstance(chonk->getBound(), xform, local_uv));
        _materialArena = chonk->_materialArena;
        _proxy_dirty = true;

        // flag all graphics states as requiring an update:
        dirtyGLObjects();

        // flag the bounds for recompute
        dirtyBound();
    }
}

ChonkDrawable::Instance
ChonkDrawable::makeInstance(const osg::BoundingBoxf& bounds,
    const osg::Matrixf& xform, const osg::Vec2f& uv)
{
    Instance instance;
    instance.xform = xform;
    instance.uv = uv;
    // The maximum absolute row sum of A*A^T bounds the squared spectral norm;
    // this is conservative for shear and exact for orthogonal TRS axes.
    const osg::Vec3d axes[3] = {
        {xform(0,0), xform(0,1), xform(0,2)},
        {xform(1,0), xform(1,1), xform(1,2)},
        {xform(2,0), xform(2,1), xform(2,2)} };
    double maxRow = 0;
    for (unsigned i = 0; i < 3; ++i)
    {
        double row = 0;
        for (unsigned j = 0; j < 3; ++j) row += std::abs(axes[i] * axes[j]);
        maxRow = std::max(maxRow, row);
    }
    instance.radius = bounds.radius() * std::sqrt(maxRow);
    instance.first_lod_cmd_index = 0;
    return instance;
}

bool
ChonkDrawable::acceptsArena(const MaterialArena* arena) const
{
    return empty() || _materialArena.get() == arena;
}

bool
ChonkDrawable::add(ChonkGeometryPage::Ptr page, ChonkGeometryPage::MeshId mesh,
    const osg::Matrixf& transform, const osg::Vec2f& tileUV)
{
    return add(std::move(page), std::vector<PagePlacement>{{mesh, transform, tileUV}});
}

bool
ChonkDrawable::add(ChonkGeometryPage::Ptr page, const std::vector<PagePlacement>& placements)
{
    if (!page) return false;
    std::vector<PageInstance> encoded;
    encoded.reserve(placements.size());
    for (const auto& placement : placements)
    {
        if (placement.mesh >= page->_meshes.size() ||
            !validPageTransform(osg::Matrixd(placement.transform)) ||
            !std::isfinite(placement.tileUV.x()) || !std::isfinite(placement.tileUV.y())) return false;
        auto instance = makeInstance(page->_meshes[placement.mesh].bounds, placement.transform, placement.tileUV);
        if (!std::isfinite(instance.radius)) return false;
        encoded.push_back({placement.mesh, instance});
    }
    std::lock_guard<std::mutex> lock(_m);
    if (!acceptsArena(page->_storage->_materialArena)) return false;
    if (encoded.empty()) return true;
    auto& output = _pageBatches[page];
    output.insert(output.end(), encoded.begin(), encoded.end());
    _materialArena = page->_storage->_materialArena;
    _proxy_dirty = true;
    dirtyGLObjects();
    dirtyBound();
    return true;
}

std::size_t ChonkDrawable::getNumInstances() const
{
    std::lock_guard<std::mutex> lock(_m);
    std::size_t count = 0;
    for (const auto& batch : _batches) count += batch.second.size();
    for (const auto& batch : _pageBatches) count += batch.second.size();
    return count;
}

std::size_t ChonkDrawable::getNumBatches() const
{
    std::lock_guard<std::mutex> lock(_m);
    std::size_t count = _batches.size();
    for (const auto& batch : _pageBatches)
    {
        std::unordered_set<ChonkGeometryPage::MeshId> meshes;
        for (const auto& placement : batch.second) meshes.insert(placement.mesh);
        count += meshes.size();
    }
    return count;
}

void
ChonkDrawable::drawImplementation(osg::RenderInfo& ri) const
{
    OE_HARD_ASSERT(false, "ChonkRenderBin::drawImplementation should never be called WHAT ARE YOU DOING");
}

void
ChonkDrawable::update_and_cull_batches(osg::State& state, int pass, const OcclusionFrame* frame) const
{
    auto& globjects = GLObjects::get(_globjects, state);

    if (pass == RESET)
    {
        // if something changed, we need to refresh the GPU tables.
        // Only here, so every later pass this frame sees the same tables.
        update_gl_objects(globjects, state);
    }

    if (_gpucull)
    {
        auto views = ChonkRenderPass::find(state);
        if (views)
        {
            // Multi-view submissions run their own reset/cull/compact sequence and keep a
            // single ordinary list.
            if (pass == RESET) globjects.cullViews(state,*views);
        }
        else if (pass == RESET) globjects.reset(state);
        else globjects.cull(state, pass, frame);
    }
}

void
ChonkDrawable::draw_batches(osg::State& state, int list) const
{
    auto& globjects = GLObjects::get(_globjects, state);

    auto views = _gpucull ? ChonkRenderPass::find(state) : nullptr;
    if (views)
    {
        if (list == EARLY_CUTOUT) globjects.drawViews(state,*views);
    }
    else
    {
        state.bindVertexArrayObject(globjects._vao->name());
        globjects.draw(state, list);
    }
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
        globjects.update(_batches, _pageBatches, this, _fadeNear, _fadeFar, _birthday, _alphaCutoff, state);
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

    for (const auto& batch : _pageBatches)
        for (const auto& placement : batch.second)
        {
            const auto& box = batch.first->_meshes[placement.mesh].bounds;
            for (unsigned i = 0; i < 8; ++i)
                result.expandBy(box.corner(i) * placement.instance.xform);
        }
    return result;
}

osg::BoundingSphere
ChonkDrawable::computeBound() const
{
    // Node::getBound and Drawable::getBoundingBox share the computed flag.
    // Populate both caches so a prior node-bound query cannot hide the box.
    return osg::BoundingSphere(getBoundingBox());
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

        // Include selected page ranges in the CPU intersection proxy.
        std::size_t num_verts = 0, num_indices = 0;
        for (auto& batch : _batches)
        {
            Chonk::Ptr c = batch.first;
            for (auto& instance : batch.second)
            {
                num_verts += c->_vbo_store.size();
                num_indices += c->_ebo_store.size();
            }
        }
        for (const auto& batch : _pageBatches)
            for (const auto& placement : batch.second)
            {
                const auto& mesh = batch.first->_meshes[placement.mesh];
                num_verts += mesh.vertexCount;
                num_indices += mesh.indexCount;
            }
        if (num_verts > std::numeric_limits<unsigned>::max())
            throw std::length_error("Chonk intersection proxy exceeds index capacity");
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
                offset += c->_vbo_store.size();
            }
        }
        for (const auto& batch : _pageBatches)
        {
            const auto& page = *batch.first;
            for (const auto& placement : batch.second)
            {
                const auto& mesh = page._meshes[placement.mesh];
                for (unsigned i = 0; i < mesh.vertexCount; ++i)
                    _proxy_verts->push_back(page.vertices()[mesh.firstVertex+i].position * placement.instance.xform);
                for (unsigned i = 0; i < mesh.indexCount; ++i)
                    _proxy_indices->push_back(page.indices()[mesh.firstIndex+i] + offset);
                offset += mesh.vertexCount;
            }
        }

        _proxy_dirty = false;
    }
}

void
ChonkDrawable::accept(osg::PrimitiveFunctor& f) const
{
    if (!empty())
    {
        refreshProxy();
        osg::Geometry::accept(f);
    }
}

void
ChonkDrawable::accept(osg::PrimitiveIndexFunctor& f) const
{
    if (!empty())
    {
        refreshProxy();
        osg::Geometry::accept(f);
    }
}

void
ChonkDrawable::GLObjects::initialize(const osg::Object* host, osg::State& state)
{
    _ext = state.get<osg::GLExtensions>();
    osg::setGLExtensionFuncPtr(_glMultiDrawElementsIndirectCount,"glMultiDrawElementsIndirectCount");
    osg::setGLExtensionFuncPtr(_glMultiDrawElementsIndirectBindlessCountNV,"glMultiDrawElementsIndirectBindlessCountNV");
    if (!osg::isGLExtensionSupported(state.getContextID(),"GL_NV_bindless_multi_draw_indirect_count"))
        _glMultiDrawElementsIndirectBindlessCountNV = nullptr;
    osg::setGLExtensionFuncPtr(_glMultiDrawElementsIndirectBindlessNV,"glMultiDrawElementsIndirectBindlessNV");
    _vao = createVAO(host,state,true);
    _coreVAO = createVAO(host,state,false);
}

//! Records independent layouts so core indexed draws never inherit NV unified-address state.
GLVAO::Ptr ChonkDrawable::GLObjects::createVAO(const osg::Object* host, osg::State& state, bool bindless)
{

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
    auto vao = GLVAO::create(state);

    // start recording...
    state.bindVertexArrayObject(vao->name());

    // must call AFTER bind
    vao->debugLabel("Chonk drawable", "VAO " + host->getName());

    // required in order to use BindlessNV extension
    if (bindless)
    {
        glEnableClientState_(GL_VERTEX_ATTRIB_ARRAY_UNIFIED_NV);
        glEnableClientState_(GL_ELEMENT_ARRAY_UNIFIED_NV);
    }

    const VADef formats[] = {
        {3, GL_FLOAT,         GL_FALSE, offsetof(Chonk::VertexGPU, position)},
        {3, GL_FLOAT,         GL_FALSE, offsetof(Chonk::VertexGPU, normal)},
        {1, GL_UNSIGNED_BYTE, GL_FALSE, offsetof(Chonk::VertexGPU, normal_technique)},
        {4, GL_UNSIGNED_BYTE, GL_TRUE,  offsetof(Chonk::VertexGPU, color)},
        {2, GL_FLOAT,         GL_FALSE, offsetof(Chonk::VertexGPU, uv)},
        {3, GL_FLOAT,         GL_FALSE, offsetof(Chonk::VertexGPU, flex)},
        {1, GL_UNSIGNED_SHORT, GL_FALSE, offsetof(Chonk::VertexGPU, material_index)},
        {1, GL_UNSIGNED_BYTE, GL_FALSE, offsetof(Chonk::VertexGPU, color_is_linear)}
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
    return vao;
}

#define NEXT_MULTIPLE(X, Y) (((X+Y-1)/Y)*Y)

void
ChonkDrawable::GLObjects::update(
    const Batches& batches,
    const PageBatches& pageBatches,
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
    _drawGroups.clear();
    ++_dataRevision;
    _viewBatches.clear();
    _visibility.clear(); // source indices change; cameras restart with everything visible

    // record for each variant (LOD) of each chonk
    _chonk_lods.clear();

    // Stable source records, addressed by each visibility record's sourceIndex.
    _all_instances.clear();

    // When GPU culling is disabled, build the same visible-list layout once
    // per update. Draw commands reference this dense list, skipping source padding.
    std::vector<VisibleInstance> unculledInstances;

    std::size_t max_lod_count = 0;
    unsigned outputOffset = 0u;

    // Both geometry sources lower to the same shader records. Dense source
    // indices may cross mesh boundaries; output ranges remain per mesh/LOD.
    auto appendBatch = [&](const Chonk::DrawCommand* commands, const Chonk::LOD* lods,
        std::size_t numLODs, const osg::BoundingBoxf& bounds, std::size_t count, auto instanceAt)
    {
        if (numLODs == 0 || count == 0) return;
        const auto limit = std::size_t(std::numeric_limits<GLsizei>::max());
        if (_all_instances.size() + count + GPU_CULLING_LOCAL_WG_SIZE > limit/sizeof(Instance) ||
            _commands.size() + numLODs > limit/sizeof(Chonk::DrawCommand) ||
            count > (limit/sizeof(VisibleInstance) - outputOffset)/numLODs)
            throw std::length_error("Chonk drawable exceeds GPU buffer capacity");
        const unsigned first = unsigned(_commands.size());
        for (unsigned lod = 0; lod < numLODs; ++lod)
        {
            _commands.push_back(commands[lod]);
            _commands.back().cmd.baseInstance = _gpucull ? outputOffset : unsigned(unculledInstances.size());
            _commands.back().cmd.instanceCount = !_gpucull && lod == 0 ? unsigned(count) : 0;
            outputOffset += unsigned(count);
            if (_gpucull)
            {
                ChonkLOD meta;
                meta.center = bounds.center();
                meta.radius = bounds.radius();
                meta.far_pixel_scale = lods[lod].far_pixel_scale;
                meta.near_pixel_scale = lods[lod].near_pixel_scale;
                meta.num_lods = unsigned(numLODs);
                meta.alpha_cutoff = alphaCutoff;
                meta.birthday = birthday;
                meta.fade_near = fadeNear;
                meta.fade_far = fadeFar;
                meta.draw_group = unsigned(_drawGroups.size()-1);
                meta.flags = lods[lod].alphaTested ? FLAG_ALPHA_TESTED : 0u;
                _chonk_lods.push_back(meta);
            }
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!_gpucull)
                unculledInstances.push_back({ GLuint(_all_instances.size()), 0u, 1.0f, alphaCutoff });
            _all_instances.push_back(instanceAt(i));
            _all_instances.back().first_lod_cmd_index = first;
        }
        max_lod_count = std::max(max_lod_count, numLODs);
    };

    for (const auto& batch : batches)
    {
        const auto& chonk = batch.first;
        const auto& commands = chonk->getOrCreateCommands(state);
        auto& buffers = Chonk::GLObjects::get(chonk->_globjects,state);
        _drawGroups.push_back({buffers.vbo,buffers.ebo,GLuint(_commands.size()),GLuint(commands.size())});
        // Supply stable legacy placements without allocating a temporary vector.
        appendBatch(commands.data(), chonk->_lods.data(), commands.size(), chonk->getBound(),
            batch.second.size(), [&](std::size_t i) -> const Instance& { return batch.second[i]; });
    }
    for (const auto& batch : pageBatches)
    {
        const auto& page = batch.first;
        const auto& commands = page->getOrCreateCommands(state);
        if (commands.empty()) continue;
        auto& buffers = Chonk::GLObjects::get(page->_storage->_globjects,state);
        _drawGroups.push_back({buffers.vbo,buffers.ebo,GLuint(_commands.size()),0u});
        auto placements = batch.second;
        // Contiguous runs avoid a separate allocation/map entry for every mesh.
        std::stable_sort(placements.begin(), placements.end(),
            [](const PageInstance& a, const PageInstance& b) { return a.mesh < b.mesh; });
        for (std::size_t first = 0; first < placements.size();)
        {
            const auto mesh = placements[first].mesh;
            auto end = first + 1;
            while (end < placements.size() && placements[end].mesh == mesh) ++end;
            appendBatch(&commands[mesh], &page->_storage->_lods[mesh], 1,
                page->_meshes[mesh].bounds, end - first,
                [&](std::size_t i) -> const Instance& { return placements[first+i].instance; });
            first = end;
        }
        _drawGroups.back().count = GLuint(_commands.size())-_drawGroups.back().first;
    }
    // Only the final workgroup needs invalid source records. No shader assumes
    // that a mesh starts on a workgroup boundary.
    const auto paddedSize = NEXT_MULTIPLE(_all_instances.size(), GPU_CULLING_LOCAL_WG_SIZE);
    _all_instances.resize(paddedSize);

    _outputCapacity = outputOffset;
    if (!_templateBuf) _templateBuf = GLBuffer::create(GL_SHADER_STORAGE_BUFFER,state);
    _templateBuf->bind();
    _templateBuf->uploadData(_commands,GL_STATIC_DRAW);
    std::vector<osg::Vec2ui> groups;
    for (const auto& group : _drawGroups) groups.emplace_back(group.first,group.count);
    if (!_groupBuf) _groupBuf = GLBuffer::create(GL_SHADER_STORAGE_BUFFER,state);
    _groupBuf->bind();
    _groupBuf->uploadData(groups,GL_STATIC_DRAW);

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

        // Four copies of the commands, one per List, each with its own output range. Culling
        // zeroes the counts on the GPU, so the templates upload only when the content changes.
        if (std::size_t(NUM_LISTS) * outputOffset > std::size_t(std::numeric_limits<GLsizei>::max()) / sizeof(VisibleInstance))
            throw std::length_error("Chonk drawable exceeds GPU buffer capacity");
        std::vector<Chonk::DrawCommand> lists;
        lists.reserve(NUM_LISTS * _commands.size());
        for (unsigned list = 0; list < NUM_LISTS; ++list)
        {
            for (auto command : _commands)
            {
                command.cmd.baseInstance += list * outputOffset;
                command.cmd.instanceCount = 0;
                lists.push_back(command);
            }
        }
        _commandBuf->uploadData(lists);
    }
    else
    {
        _commandBuf->uploadData(_commands);
    }

    // Reserve one small record for every possible survivor in the CPU-assigned
    // command ranges. Source-buffer allocation padding needs no output records.
    GLsizei outputSize = (_gpucull ? NUM_LISTS * outputOffset : unculledInstances.size()) * sizeof(VisibleInstance);
    if (!_instanceOutputBuf)
    {
        _instanceOutputBuf = GLBuffer::create(GL_SHADER_STORAGE_BUFFER, state, outputSize, OUTPUT_BUF_CHUNK_SIZE);
        _instanceOutputBuf->bind();
        _instanceOutputBuf->debugLabel("Chonk drawable", "visible instances " + host->getName());
        _instanceOutputBuf->unbind();
    }
    if (_gpucull)
        _instanceOutputBuf->uploadData(outputSize, nullptr);
    else
        _instanceOutputBuf->uploadData(unculledInstances, GL_STATIC_DRAW);

    _numInstances = _all_instances.size();
    _maxNumLODs = max_lod_count;

    _dirty = false;
}

void
ChonkDrawable::GLObjects::reset(osg::State& state)
{
    if (_commands.empty() || _commandBuf == nullptr)
        return;

    auto program = state.getLastAppliedProgramObject();
    OE_HARD_ASSERT(program != nullptr, "Check for shader errors!");
    auto location = [&](const char* name) { return program->getUniformLocation(osg::Uniform::getNameID(name)); };
    _ext->glUniform4ui(location("oe_chonk_views"),0,0,0,0);
    _ext->glUniform1ui(location("oe_chonk_list_stride"), GLuint(_commands.size()));
    _ext->glUniform1i(location("oe_chonk_pass"), RESET);
    _commandBuf->bindBufferBase(29);
    _ext->glDispatchCompute(GLuint((NUM_LISTS*_commands.size() + GPU_CULLING_LOCAL_WG_SIZE-1) /
        GPU_CULLING_LOCAL_WG_SIZE), 1, 1);
}

void
ChonkDrawable::GLObjects::cull(osg::State& state, int pass, const OcclusionFrame* frame)
{
    if (_commands.empty() || _commandBuf == nullptr)
        return;

    OE_GL_ZONE_NAMED("cull");

    // transmit the uniforms
    state.applyModelViewAndProjectionUniformsIfRequired();

    auto ext = _vao->ext();

    auto program = state.getLastAppliedProgramObject();
    OE_HARD_ASSERT(program != nullptr, "Check for shader errors!");
    // Resolve locations on the active program: define variants have different locations.
    auto location = [&](const char* name) { return program->getUniformLocation(osg::Uniform::getNameID(name)); };
    ext->glUniform4ui(location("oe_chonk_views"),0,0,0,0);
    ext->glUniform1ui(location("oe_chonk_list_stride"), GLuint(_commands.size()));
    ext->glUniform1ui(location("oe_chonk_max_lods"), GLuint(_maxNumLODs));

    _instanceOutputBuf->bindBufferBase(0);
    _commandBuf->bindBufferBase(29);
    _chonkBuf->bindBufferBase(30);
    _instanceInputBuf->bindBufferBase(31);

    if (pass != CULL && frame)
        visibility(state, *frame)->bindBufferBase(23);

    if (pass == LATE)
    {
        // The pyramid is optional: without it every instance counts as visible.
        const bool valid = frame && frame->hizValid && ext->glUniformHandleui64;
        if (valid) ext->glUniformHandleui64(location("oe_chonk_hiz"), frame->hiz);
        const osg::Vec4f params = valid ? frame->hizParams : osg::Vec4f();
        ext->glUniform4fv(location("oe_chonk_hiz_params"), 1, params.ptr());
    }

    // _numInstances is already padded to the workgroup size.
    unsigned workgroups = (_numInstances + (GPU_CULLING_LOCAL_WG_SIZE-1)) / GPU_CULLING_LOCAL_WG_SIZE;

    // Fixed CPU-assigned output ranges let each invocation cull and compact
    // independently. DrawLeaf publishes the results before the first draw.
    ext->glUniform1i(location("oe_chonk_pass"), pass);
    ext->glDispatchCompute(workgroups, _maxNumLODs, 1);
}

GLBuffer::Ptr
ChonkDrawable::GLObjects::visibility(osg::State& state, const OcclusionFrame& frame)
{
    for (auto i = _visibility.begin(); i != _visibility.end(); )
    {
        if (!i->camera.valid() || frame.frameNumber - i->lastUsed > 240u) i = _visibility.erase(i);
        else ++i;
    }
    for (auto& entry : _visibility)
    {
        if (entry.key == frame.camera)
        {
            entry.lastUsed = frame.frameNumber;
            return entry.bits;
        }
    }
    _visibility.emplace_back();
    auto& entry = _visibility.back();
    entry.key = frame.camera;
    entry.camera = frame.camera;
    entry.lastUsed = frame.frameNumber;
    entry.bits = GLBuffer::create(GL_SHADER_STORAGE_BUFFER, state);
    entry.bits->bind(); // Instantiate the generated name before GLBuffer's direct-state-access upload.
    // A new camera draws everything in its first early pass; the late pass then learns what is hidden.
    entry.bits->uploadData(std::vector<GLuint>(_numInstances * std::max<std::size_t>(_maxNumLODs, 1u), 1u));
    return entry.bits;
}

namespace
{
    //! Allows reconstruction roundoff when matching one drawable placement across independently fitted views.
    bool samePlacement(const osg::Matrixd& a, const osg::Matrixd& b)
    {
        for (unsigned row=0; row<4; ++row)
        for (unsigned col=0; col<4; ++col)
            if (std::abs(a(row,col)-b(row,col)) > (row == 3 ? 1e-7 : 1e-12)) return false;
        return true;
    }
}

//! Shares outputs across views, while keeping distinct copies for multiply parented drawable placements.
ChonkDrawable::GLObjects::ViewBatch&
ChonkDrawable::GLObjects::viewBatch(osg::State& state, const ChonkRenderPass& pass)
{
    const auto& frame = pass.getBatch()->parameters;
    osg::Matrixd local = state.getModelViewMatrix()*pass.getInverseRenderView();
    for (auto i=_viewBatches.begin(); i!=_viewBatches.end();)
    {
        if (frame.frameNumber-i->lastUsed > 8u) i = _viewBatches.erase(i);
        else
        {
            if (i->viewID == frame.viewID && samePlacement(i->localToWorld,local))
            {
                i->lastUsed = frame.frameNumber;
                return *i;
            }
            ++i;
        }
    }
    _viewBatches.emplace_back();
    auto& result = _viewBatches.back();
    result.viewID = frame.viewID;
    result.localToWorld = local;
    result.lastUsed = frame.frameNumber;
    return result;
}

//! Performs one reset/classify/compact sequence per drawable placement and immutable submission batch.
void ChonkDrawable::GLObjects::cullViews(osg::State& state, const ChonkRenderPass& pass)
{
    if (_commands.empty()) return;
    auto& batch = viewBatch(state,pass);
    const auto& frame = pass.getBatch()->parameters;
    if (batch.batchSerial == pass.getBatch()->serial && batch.revision == _dataRevision) return;
    auto program = state.getLastAppliedProgramObject();
    // Resolve locations on the active program: shader variants and recycled contexts may have different locations.
    auto location = [&](const char* name) { return program->getUniformLocation(osg::Uniform::getNameID(name)); };
    state.applyModelViewAndProjectionUniformsIfRequired();
    const unsigned count = unsigned(_commands.size());
    const unsigned groups = unsigned(_drawGroups.size());
    const unsigned lists = frame.output == ChonkRenderPass::PER_VIEW ? frame.count : 1u;
    const unsigned copies = frame.output == ChonkRenderPass::UNION ? 1u : frame.count;
    const unsigned counters = ChonkRenderPass::MAX_VIEWS * (1u+groups);
    // Each instance/LOD/view pair has reserved space, even when all view volumes overlap completely.
    auto allocate = [&](GLBuffer::Ptr& buffer, std::size_t bytes)
    {
        if (bytes > std::size_t(std::numeric_limits<GLsizei>::max()))
            throw std::length_error("Chonk multi-view results exceed GPU buffer capacity");
        if (!buffer) buffer = GLBuffer::create(GL_SHADER_STORAGE_BUFFER,state);
        buffer->bind(); // Instantiate generated names before GLBuffer's direct-state-access upload.
        buffer->uploadData(GLsizei(bytes),nullptr);
    };
    allocate(batch.commands,std::size_t(lists)*count*sizeof(Chonk::DrawCommand));
    allocate(batch.compact,std::size_t(lists)*count*sizeof(Chonk::DrawCommand));
    allocate(batch.coreCommands,std::size_t(lists)*count*5*sizeof(GLuint));
    allocate(batch.visible,std::size_t(copies)*_outputCapacity*sizeof(VisibleInstance));
    allocate(batch.counts,counters*sizeof(GLuint));
    allocate(batch.parameter,sizeof(GLuint));
    batch.visible->bindBufferBase(0);
    _groupBuf->bindBufferBase(24);
    batch.coreCommands->bindBufferBase(25);
    batch.compact->bindBufferBase(26);
    batch.counts->bindBufferBase(27);
    _templateBuf->bindBufferBase(28);
    batch.commands->bindBufferBase(29);
    _chonkBuf->bindBufferBase(30);
    _instanceInputBuf->bindBufferBase(31);
    // Extract local-space planes in double precision. This also supports perspective views and oblique frusta.
    std::array<osg::Vec4f,ChonkRenderPass::MAX_VIEWS*6> planes;
    unsigned orthographic = 0;
    for (unsigned i=0; i<frame.count; ++i)
    {
        osg::Matrixd clip = batch.localToWorld*frame.clipFromWorld[i];
        for (unsigned axis=0; axis<3; ++axis)
        for (unsigned side=0; side<2; ++side)
        {
            double sign = side == 0 ? 1.0 : -1.0;
            osg::Vec4d plane(clip(0,3)+sign*clip(0,axis),clip(1,3)+sign*clip(1,axis),
                clip(2,3)+sign*clip(2,axis),clip(3,3)+sign*clip(3,axis));
            double length = osg::Vec3d(plane.x(),plane.y(),plane.z()).length();
            planes[i*6+axis*2+side] = length > 0.0 ? osg::Vec4f(plane/length) :
                osg::Vec4f(0,0,0,plane.w() >= 0.0 ? FLT_MAX : -FLT_MAX);
        }
        if (clip(0,3) == 0.0 && clip(1,3) == 0.0 && clip(2,3) == 0.0 && clip(3,3) > 0.0)
        {
            // Opposite parallel planes become three slabs: one dot/absolute-value test per axis on the GPU.
            orthographic |= 1u << i;
            for (unsigned axis=0; axis<3; ++axis)
            {
                auto& lower = planes[i*6+axis*2];
                auto& upper = planes[i*6+axis*2+1];
                float center = 0.5f*lower.w()-0.5f*upper.w();
                upper.w() = 0.5f*lower.w()+0.5f*upper.w();
                lower.w() = center;
            }
        }
    }
    _ext->glUniform4fv(location("oe_chonk_view_planes"),frame.count*6,planes[0].ptr());
    _ext->glUniform1ui(location("oe_chonk_orthographic_views"),orthographic);
    osg::Matrixf lodView = batch.localToWorld*frame.lodView, lodProjection = frame.lodProjection;
    _ext->glUniformMatrix4fv(location("oe_chonk_lod_view"),1,GL_FALSE,lodView.ptr());
    _ext->glUniformMatrix4fv(location("oe_chonk_lod_projection"),1,GL_FALSE,lodProjection.ptr());
    _ext->glUniform2fv(location("oe_chonk_lod_viewport"),1,frame.lodViewport.ptr());
    _ext->glUniform1i(location("oe_chonk_retain_outside_lod_view"),frame.retainOutsideLODView ? 1 : 0);
    double norm1 = 0.0, normInf = 0.0;
    for (unsigned i=0; i<3; ++i)
    {
        double row = 0.0, column = 0.0;
        for (unsigned j=0; j<3; ++j)
        {
            row += std::abs(batch.localToWorld(i,j));
            column += std::abs(batch.localToWorld(j,i));
        }
        norm1 = std::max(norm1,column);
        normInf = std::max(normInf,row);
    }
    _ext->glUniform1f(location("oe_chonk_parent_scale"),float(std::sqrt(norm1*normInf)));
    _ext->glUniform4ui(location("oe_chonk_views"),frame.count,frame.activeViews,count,_outputCapacity);
    _ext->glUniform1ui(location("oe_chonk_view_groups"),groups);
    _ext->glUniform1i(location("oe_chonk_view_output"),int(frame.output));
    GLint phase = location("oe_chonk_view_phase");
    // The preceding frame may still read these counters as indirect parameters. Order all prior reads
    // before reusing the persistent buffers for shader writes (including views without an intervening draw).
    _ext->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    _ext->glUniform1i(phase,0);
    _ext->glDispatchCompute((std::max(lists*count,counters)+31)/32,1,1);
    _ext->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    _ext->glUniform1i(phase,1);
    _ext->glDispatchCompute(unsigned((_numInstances+31)/32),unsigned(_maxNumLODs),1);
    _ext->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    _ext->glUniform1i(phase,2);
    _ext->glDispatchCompute((lists*count+31)/32,1,1);
    // DrawLeaf publishes the compact list and parameter counters before indirect drawing.
    batch.batchSerial = pass.getBatch()->serial;
    batch.revision = _dataRevision;
}

//! Draws a dense per-layer or merged vertex-layer list; core mode groups commands by shared vertex/index buffers.
void ChonkDrawable::GLObjects::drawViews(osg::State& state, const ChonkRenderPass& pass)
{
    if (_commands.empty()) return;
    auto& batch = viewBatch(state,pass);
    state.applyModelViewAndProjectionUniformsIfRequired();
    batch.visible->bindBufferBase(0);
    _instanceInputBuf->bindBufferBase(31);
    constexpr GLenum parameterBuffer = 0x80EE; // GL_PARAMETER_BUFFER, core in OpenGL 4.6.
    batch.counts->bind(parameterBuffer);
    const GLenum type = sizeof(Chonk::element_t) == sizeof(GLushort) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    if (!pass.getBatch()->parameters.coreDraws && _glMultiDrawElementsIndirectBindlessCountNV)
    {
        state.bindVertexArrayObject(_vao->name());
        batch.compact->bind(GL_DRAW_INDIRECT_BUFFER);
        // Keep the NV parameter at offset zero. Some drivers validate nonzero byte offsets as draw counts.
        // The four-byte GPU copy avoids that validation bug without a CPU readback or oversized command lists.
        batch.counts->copyBufferSubData(batch.parameter,pass.getIndex()*sizeof(GLuint),0,sizeof(GLuint));
        batch.parameter->bind(parameterBuffer);
        _glMultiDrawElementsIndirectBindlessCountNV(GL_TRIANGLES,type,
            reinterpret_cast<const GLvoid*>(pass.getIndex()*_commands.size()*sizeof(Chonk::DrawCommand)),
            0,GLsizei(_commands.size()),sizeof(Chonk::DrawCommand),1);
    }
    else
    {
        state.bindVertexArrayObject(_coreVAO->name());
        batch.coreCommands->bind(GL_DRAW_INDIRECT_BUFFER);
        for (unsigned i=0; i<_drawGroups.size(); ++i)
        {
            const auto& group = _drawGroups[i];
            _ext->glBindVertexBuffer(0,group.vertices->name(),0,sizeof(Chonk::VertexGPU));
            group.indices->bind(GL_ELEMENT_ARRAY_BUFFER_ARB);
            _glMultiDrawElementsIndirectCount(GL_TRIANGLES,type,
                reinterpret_cast<const GLvoid*>((pass.getIndex()*_commands.size()+group.first)*5*sizeof(GLuint)),
                (ChonkRenderPass::MAX_VIEWS+pass.getIndex()*_drawGroups.size()+i)*sizeof(GLuint),
                group.count,5*sizeof(GLuint));
        }
    }
    _ext->glBindBuffer(parameterBuffer,0);
}

void
ChonkDrawable::GLObjects::draw(osg::State& state, int list)
{
    OE_GL_ZONE_NAMED("draw");

    if (_commandBuf == nullptr || _commands.empty())
        return;

    // Unculled drawables keep one precomputed list and always take the alpha-tested path.
    if (!_gpucull && list != EARLY_CUTOUT)
        return;

    // transmit the uniforms
    state.applyModelViewAndProjectionUniformsIfRequired();

    // bind the command list for drawing
    _commandBuf->bind(GL_DRAW_INDIRECT_BUFFER);

    // Rebind both tables: all cull leaves run before the draw leaves, so the
    // source binding left by the culler may belong to a different drawable.
    _instanceOutputBuf->bindBufferBase(0);
    _instanceInputBuf->bindBufferBase(31);

    GLenum elementType = sizeof(Chonk::element_t) == sizeof(GLushort) ?
        GL_UNSIGNED_SHORT :
        GL_UNSIGNED_INT;

    const std::size_t first = _gpucull ? std::size_t(list) * _commands.size() : 0u;
    _glMultiDrawElementsIndirectBindlessNV(
        GL_TRIANGLES,
        elementType,
        (const GLvoid*)(first * sizeof(Chonk::DrawCommand)),
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
    _templateBuf = nullptr;
    _groupBuf = nullptr;
    _coreVAO = nullptr;
    _viewBatches.clear();
    _visibility.clear();
    _drawGroups.clear();
    _commands.clear();
    _dirty = true;
}

// GL enums used by the occlusion pyramid that OSG's headers may not define.
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE
#define GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE 0x8211
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE
#define GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE 0x8216
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE
#define GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE 0x8217
#endif
#ifndef GL_DEPTH32F_STENCIL8
#define GL_DEPTH32F_STENCIL8 0x8CAD
#endif
#ifndef GL_READ_WRITE
#define GL_READ_WRITE 0x88BA
#endif

namespace
{
    //! Process-wide render-bin switches, all on by default; set through ChonkRenderBin.
    std::atomic<bool>& opaquePathFlag()
    {
        static std::atomic<bool> flag{true};
        return flag;
    }

    std::atomic<bool>& occlusionCullingFlag()
    {
        static std::atomic<bool> flag{true};
        return flag;
    }

    std::atomic<bool>& frontToBackFlag()
    {
        static std::atomic<bool> flag{true};
        return flag;
    }

    //! GL 4.5 direct-state-access and bindless entry points for the occlusion pyramid. None of them
    //! binds a texture unit, image unit or framebuffer, so OSG's state tracking stays valid.
    //! Load with the owning context current.
    struct HiZFunctions
    {
        void (GL_APIENTRY* createTextures)(GLenum, GLsizei, GLuint*) = nullptr;
        void (GL_APIENTRY* textureStorage2D)(GLuint, GLsizei, GLenum, GLsizei, GLsizei) = nullptr;
        void (GL_APIENTRY* textureParameteri)(GLuint, GLenum, GLint) = nullptr;
        void (GL_APIENTRY* createFramebuffers)(GLsizei, GLuint*) = nullptr;
        void (GL_APIENTRY* namedFramebufferTexture)(GLuint, GLenum, GLuint, GLint) = nullptr;
        void (GL_APIENTRY* namedFramebufferDrawBuffer)(GLuint, GLenum) = nullptr;
        void (GL_APIENTRY* namedFramebufferReadBuffer)(GLuint, GLenum) = nullptr;
        GLenum (GL_APIENTRY* checkNamedFramebufferStatus)(GLuint, GLenum) = nullptr;
        void (GL_APIENTRY* getNamedFramebufferAttachmentParameteriv)(GLuint, GLenum, GLenum, GLint*) = nullptr;
        void (GL_APIENTRY* blitNamedFramebuffer)(GLuint, GLuint, GLint, GLint, GLint, GLint,
            GLint, GLint, GLint, GLint, GLbitfield, GLenum) = nullptr;
        GLuint64 (GL_APIENTRY* getTextureHandle)(GLuint) = nullptr;
        void (GL_APIENTRY* makeTextureHandleResident)(GLuint64) = nullptr;
        GLuint64 (GL_APIENTRY* getImageHandle)(GLuint, GLint, GLboolean, GLint, GLenum) = nullptr;
        // Loaded here because OSG declares glMakeImageHandleResident without its access argument.
        void (GL_APIENTRY* makeImageHandleResident)(GLuint64, GLenum) = nullptr;
        void (GL_APIENTRY* uniformHandleui64)(GLint, GLuint64) = nullptr;
        bool valid = false;

        HiZFunctions()
        {
            osg::setGLExtensionFuncPtr(createTextures, "glCreateTextures");
            osg::setGLExtensionFuncPtr(textureStorage2D, "glTextureStorage2D");
            osg::setGLExtensionFuncPtr(textureParameteri, "glTextureParameteri");
            osg::setGLExtensionFuncPtr(createFramebuffers, "glCreateFramebuffers");
            osg::setGLExtensionFuncPtr(namedFramebufferTexture, "glNamedFramebufferTexture");
            osg::setGLExtensionFuncPtr(namedFramebufferDrawBuffer, "glNamedFramebufferDrawBuffer");
            osg::setGLExtensionFuncPtr(namedFramebufferReadBuffer, "glNamedFramebufferReadBuffer");
            osg::setGLExtensionFuncPtr(checkNamedFramebufferStatus, "glCheckNamedFramebufferStatus");
            osg::setGLExtensionFuncPtr(getNamedFramebufferAttachmentParameteriv,
                "glGetNamedFramebufferAttachmentParameteriv");
            osg::setGLExtensionFuncPtr(blitNamedFramebuffer, "glBlitNamedFramebuffer");
            osg::setGLExtensionFuncPtr(getTextureHandle, "glGetTextureHandleARB");
            osg::setGLExtensionFuncPtr(makeTextureHandleResident, "glMakeTextureHandleResidentARB");
            osg::setGLExtensionFuncPtr(getImageHandle, "glGetImageHandleARB");
            osg::setGLExtensionFuncPtr(makeImageHandleResident, "glMakeImageHandleResidentARB");
            osg::setGLExtensionFuncPtr(uniformHandleui64, "glUniformHandleui64ARB");
            valid = createTextures && textureStorage2D && textureParameteri && createFramebuffers &&
                namedFramebufferTexture && namedFramebufferDrawBuffer && namedFramebufferReadBuffer &&
                checkNamedFramebufferStatus && getNamedFramebufferAttachmentParameteriv &&
                blitNamedFramebuffer && getTextureHandle && makeTextureHandleResident && getImageHandle &&
                makeImageHandleResident && uniformHandleui64;
        }
    };

    //! A texture or framebuffer created with direct state access and handed to osgEarth's object
    //! pool, which deletes it on its own context once nothing else references it. Dropping the
    //! last reference is therefore safe on any thread.
    class HiZObject : public GLObject
    {
    public:
        using Ptr = std::shared_ptr<HiZObject>;

        //! Creates a 2D texture with immutable storage; call with the context current.
        static Ptr texture(osg::State& state, const HiZFunctions& gl,
            GLsizei levels, GLenum format, GLsizei width, GLsizei height, std::size_t bytes)
        {
            Ptr object(new HiZObject(GL_TEXTURE, state, bytes));
            gl.createTextures(GL_TEXTURE_2D, 1, &object->_name);
            gl.textureStorage2D(object->_name, levels, format, width, height);
            GLObjectPool::get(state)->watch(object);
            return object;
        }

        //! Creates a framebuffer object; call with the context current.
        static Ptr framebuffer(osg::State& state, const HiZFunctions& gl)
        {
            Ptr object(new HiZObject(GL_FRAMEBUFFER_EXT, state, 0));
            gl.createFramebuffers(1, &object->_name);
            GLObjectPool::get(state)->watch(object);
            return object;
        }

        //! Called by the pool with the context current. Deleting a texture also deletes its
        //! bindless handles.
        void release() override
        {
            if (_name == 0) return;
            if (ns() == GL_FRAMEBUFFER_EXT) ext()->glDeleteFramebuffers(1, &_name);
            else glDeleteTextures(1, &_name);
            _name = 0;
        }

        GLsizei size() const override { return _bytes; }

    private:
        HiZObject(GLenum ns, osg::State& state, std::size_t bytes) :
            GLObject(ns, state),
            _bytes(GLsizei(std::min<std::size_t>(bytes, std::size_t(std::numeric_limits<GLsizei>::max())))) { }
        GLsizei _bytes;
    };

    //! One camera's single-sample depth copy and max-depth pyramid in one graphics context.
    //! Only that context's draw thread uses it.
    struct HiZTarget
    {
        unsigned contextID = 0;
        const osg::Camera* key = nullptr; // identity only
        osg::observer_ptr<const osg::Camera> camera;
        unsigned lastUsed = 0;
        GLint source = -1; // framebuffer last copied from
        GLenum sourceFormat = 0; // its depth format, or zero when it cannot be copied
        GLint width = 0, height = 0, levels = 0;
        GLenum format = 0; // format of the depth copy
        HiZObject::Ptr depth, fbo, pyramid;
        GLuint64 depthHandle = 0, pyramidHandle = 0;
        std::vector<GLuint64> images; // one image handle per pyramid level
        bool validated = false; // the first blit into this allocation raised no GL error
        bool failed = false; // the camera's framebuffer cannot be copied: it keeps drawing everything

        //! Drops the GL objects for the pool to delete; safe on any thread.
        void release()
        {
            depth = fbo = pyramid = nullptr;
            depthHandle = pyramidHandle = 0;
            images.clear();
            width = height = levels = 0;
            format = 0;
            validated = false;
        }
    };

    //! Occlusion pyramids for every camera and context. The mutex guards the containers; entry
    //! points stay loaded for the life of the process, and targets are shared so a concurrent
    //! release cannot pull one out from under a draw.
    struct HiZRegistry
    {
        std::mutex mutex;
        std::unordered_map<unsigned, std::unique_ptr<HiZFunctions>> functions;
        std::vector<std::shared_ptr<HiZTarget>> targets;
    };

    HiZRegistry& hizRegistry()
    {
        static HiZRegistry registry;
        return registry;
    }

    //! Internal format matching a framebuffer's depth buffer, which a depth blit requires; zero when
    //! there is no depth buffer or its format is unusual.
    GLenum matchingDepthFormat(const HiZFunctions& gl, GLint fbo)
    {
        const GLenum depthAttachment = fbo ? GL_DEPTH_ATTACHMENT_EXT : GL_DEPTH;
        const GLenum stencilAttachment = fbo ? GL_STENCIL_ATTACHMENT_EXT : GL_STENCIL;
        GLint type = GL_NONE, bits = 0, component = GL_NONE, stencilType = GL_NONE, stencil = 0;
        gl.getNamedFramebufferAttachmentParameteriv(fbo, depthAttachment,
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT, &type);
        if (type == GL_NONE)
            return 0;
        gl.getNamedFramebufferAttachmentParameteriv(fbo, depthAttachment, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
        gl.getNamedFramebufferAttachmentParameteriv(fbo, depthAttachment,
            GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &component);
        gl.getNamedFramebufferAttachmentParameteriv(fbo, stencilAttachment,
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT, &stencilType);
        if (stencilType != GL_NONE)
            gl.getNamedFramebufferAttachmentParameteriv(fbo, stencilAttachment,
                GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencil);
        if (component == GL_FLOAT && bits == 32) return stencil ? GL_DEPTH32F_STENCIL8 : GL_DEPTH_COMPONENT32F;
        if (bits == 24) return stencil ? GL_DEPTH24_STENCIL8_EXT : GL_DEPTH_COMPONENT24;
        if (stencil == 0 && bits == 16) return GL_DEPTH_COMPONENT16;
        if (stencil == 0 && bits == 32) return GL_DEPTH_COMPONENT32;
        return 0;
    }

    //! (Re)allocates a target's depth copy and floor-sized R32F pyramid, with resident handles.
    //! Call with the context current; returns false when any piece is unavailable.
    bool allocateHiZ(HiZTarget& t, osg::State& state, const HiZFunctions& gl, GLint width, GLint height,
        GLenum format)
    {
        t.release();
        t.width = width;
        t.height = height;
        t.format = format;

        const std::size_t texels = std::size_t(width) * std::size_t(height);
        const std::size_t texelBytes = format == GL_DEPTH32F_STENCIL8 ? 8u : format == GL_DEPTH_COMPONENT16 ? 2u : 4u;
        t.depth = HiZObject::texture(state, gl, 1, format, width, height, texels * texelBytes);
        t.depth->debugLabel("Chonk", "occlusion depth copy");
        gl.textureParameteri(t.depth->name(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.textureParameteri(t.depth->name(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        t.fbo = HiZObject::framebuffer(state, gl);
        const bool stencil = format == GL_DEPTH24_STENCIL8_EXT || format == GL_DEPTH32F_STENCIL8;
        gl.namedFramebufferTexture(t.fbo->name(),
            stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT_EXT, t.depth->name(), 0);
        gl.namedFramebufferDrawBuffer(t.fbo->name(), GL_NONE);
        gl.namedFramebufferReadBuffer(t.fbo->name(), GL_NONE);
        if (gl.checkNamedFramebufferStatus(t.fbo->name(), GL_DRAW_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
            return false;

        // Level 0 halves the viewport; like GL mipmaps, sizes round down, to 1x1 at the top.
        const GLint w0 = std::max(1, width/2), h0 = std::max(1, height/2);
        t.levels = 1;
        while ((std::max(w0, h0) >> t.levels) > 0) ++t.levels;
        t.pyramid = HiZObject::texture(state, gl, t.levels, GL_R32F, w0, h0,
            std::size_t(w0) * std::size_t(h0) * 16u / 3u);
        t.pyramid->debugLabel("Chonk", "occlusion pyramid");
        gl.textureParameteri(t.pyramid->name(), GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        gl.textureParameteri(t.pyramid->name(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.textureParameteri(t.pyramid->name(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.textureParameteri(t.pyramid->name(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Handles freeze the sampler state set above.
        t.depthHandle = gl.getTextureHandle(t.depth->name());
        t.pyramidHandle = gl.getTextureHandle(t.pyramid->name());
        if (t.depthHandle == 0 || t.pyramidHandle == 0)
            return false;
        gl.makeTextureHandleResident(t.depthHandle);
        gl.makeTextureHandleResident(t.pyramidHandle);
        for (GLint level = 0; level < t.levels; ++level)
        {
            const GLuint64 image = gl.getImageHandle(t.pyramid->name(), level, GL_FALSE, 0, GL_R32F);
            if (image == 0)
                return false;
            gl.makeImageHandleResident(image, GL_READ_WRITE);
            t.images.push_back(image);
        }
        return true;
    }

    //! What buildHiZ publishes for the late culling pass.
    struct HiZResult
    {
        GLuint64 handle = 0; // resident sampler handle of the pyramid
        osg::Vec4f params; // viewport width and height in pixels, levels, 1
    };

    //! Copies the draw framebuffer's depth under the current viewport and reduces it into the camera's
    //! pyramid with the applied pyramid program. Call on the camera's draw thread. Returns false,
    //! publishing nothing, when anything is unavailable; the late pass then keeps every instance.
    bool buildHiZ(osg::State& state, const osg::Camera* camera, unsigned frameNumber, HiZResult& result)
    {
        const osg::Viewport* viewport = state.getCurrentViewport();
        auto program = state.getLastAppliedProgramObject();
        if (!camera || !viewport || !program || !program->isLinked())
            return false;

        // A missing uniform means some other program is active (a broken build or an override).
        auto location = [&](const char* name) { return program->getUniformLocation(osg::Uniform::getNameID(name)); };
        const GLint depthLoc = location("oe_hiz_depth");
        const GLint sourceLoc = location("oe_hiz_source");
        const GLint targetLoc = location("oe_hiz_target");
        const GLint sourceSizeLoc = location("oe_hiz_source_size");
        const GLint targetSizeLoc = location("oe_hiz_target_size");
        const GLint fromDepthLoc = location("oe_hiz_from_depth");
        if (depthLoc < 0 || sourceLoc < 0 || targetLoc < 0 || sourceSizeLoc < 0 || targetSizeLoc < 0 ||
            fromDepthLoc < 0)
            return false;

        const GLint x = GLint(viewport->x()), y = GLint(viewport->y());
        const GLint width = GLint(viewport->width()), height = GLint(viewport->height());
        if (width < 2 || height < 2)
            return false;

        auto& registry = hizRegistry();
        const unsigned contextID = state.getContextID();
        HiZFunctions* gl = nullptr;
        std::shared_ptr<HiZTarget> target;
        {
            std::lock_guard<std::mutex> lock(registry.mutex);
            auto& functions = registry.functions[contextID];
            if (!functions) functions.reset(new HiZFunctions());
            gl = functions.get();
            if (!gl->valid)
                return false;

            // Retire this context's targets for dead cameras (even one whose address was reused)
            // and for cameras that stopped drawing.
            auto& targets = registry.targets;
            for (auto i = targets.begin(); i != targets.end(); )
            {
                const HiZTarget& t = **i;
                if (t.contextID == contextID &&
                    (!t.camera.valid() || (t.key != camera && frameNumber - t.lastUsed > 600u)))
                    i = targets.erase(i);
                else
                    ++i;
            }
            for (auto& t : targets)
                if (t->contextID == contextID && t->key == camera) target = t;
            if (!target)
            {
                target = std::make_shared<HiZTarget>();
                target->contextID = contextID;
                target->key = camera;
                target->camera = camera;
                targets.push_back(target);
            }
        }

        HiZTarget& t = *target;
        t.lastUsed = frameNumber;
        if (t.failed)
            return false;

        GLint source = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &source);
        if (source != t.source)
        {
            t.source = source;
            t.sourceFormat = matchingDepthFormat(*gl, source);
        }
        if (!t.depth || width != t.width || height != t.height || t.sourceFormat != t.format)
        {
            // A target without a depth buffer (or with an unusual one) simply keeps drawing everything.
            if (t.sourceFormat == 0)
            {
                t.release();
                t.failed = true;
                OE_INFO << LC << "Occlusion culling off for camera \"" << camera->getName()
                    << "\": no copyable depth buffer" << std::endl;
                return false;
            }
            if (!allocateHiZ(t, state, *gl, width, height, t.sourceFormat))
            {
                t.release();
                t.failed = true;
                OE_WARN << LC << "Occlusion culling disabled for camera \"" << camera->getName()
                    << "\": depth pyramid allocation failed" << std::endl;
                return false;
            }
        }

        // Errors raised earlier by other code are not the blit's; drain them before checking it.
        if (!t.validated)
            for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) { }

        // A depth blit keeps one sample of each multisampled pixel. The scissor test is the only
        // fragment operation that applies to blits, so lift it for the copy.
        const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        if (scissor) glDisable(GL_SCISSOR_TEST);
        gl->blitNamedFramebuffer(source, t.fbo->name(), x, y, x + width, y + height, 0, 0, width, height,
            GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        if (scissor) glEnable(GL_SCISSOR_TEST);

        if (!t.validated)
        {
            if (glGetError() != GL_NO_ERROR)
            {
                t.release();
                t.failed = true;
                OE_WARN << LC << "Occlusion culling disabled for camera \"" << camera->getName()
                    << "\": its depth buffer cannot be copied" << std::endl;
                return false;
            }
            t.validated = true;
        }

        // Each dispatch halves the previous level, keeping the farthest depth.
        auto ext = state.get<osg::GLExtensions>();
        gl->uniformHandleui64(depthLoc, t.depthHandle);
        GLint sw = width, sh = height;
        GLint tw = std::max(1, sw/2), th = std::max(1, sh/2);
        for (GLint level = 0; level < t.levels; ++level)
        {
            ext->glUniform1i(fromDepthLoc, level == 0 ? 1 : 0);
            gl->uniformHandleui64(sourceLoc, t.images[level > 0 ? level - 1 : 0]);
            gl->uniformHandleui64(targetLoc, t.images[level]);
            ext->glUniform2i(sourceSizeLoc, sw, sh);
            ext->glUniform2i(targetSizeLoc, tw, th);
            ext->glDispatchCompute(GLuint((tw + 7)/8), GLuint((th + 7)/8), 1u);
            ext->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            sw = tw;
            sh = th;
            tw = std::max(1, tw/2);
            th = std::max(1, th/2);
        }
        ext->glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

        result.handle = t.pyramidHandle;
        result.params.set(float(width), float(height), float(t.levels), 1.0f);
        return true;
    }

    //! Forgets one context's occlusion pyramids (every context's when state is null). The object
    //! pool deletes their GL objects on the owning context, so any thread may call this.
    void releaseHiZ(osg::State* state)
    {
        auto& registry = hizRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        auto& targets = registry.targets;
        for (auto i = targets.begin(); i != targets.end(); )
        {
            if (state == nullptr || (*i)->contextID == state->getContextID())
                i = targets.erase(i);
            else
                ++i;
        }
    }
}

void
ChonkRenderBin::setOpaquePath(bool value)
{
    opaquePathFlag() = value;
}

bool
ChonkRenderBin::getOpaquePath()
{
    return opaquePathFlag();
}

void
ChonkRenderBin::setOcclusionCulling(bool value)
{
    occlusionCullingFlag() = value;
}

bool
ChonkRenderBin::getOcclusionCulling()
{
    return occlusionCullingFlag();
}

void
ChonkRenderBin::setFrontToBack(bool value)
{
    frontToBackFlag() = value;
}

bool
ChonkRenderBin::getFrontToBack()
{
    return frontToBackFlag();
}

ChonkRenderBin::ChonkRenderBin() :
    osgUtil::RenderBin()
{
    setName("ChonkBin");
}

ChonkRenderBin::ChonkRenderBin(const ChonkRenderBin& rhs, const osg::CopyOp& op) :
    osgUtil::RenderBin(rhs, op),
    _cullSS(rhs._cullSS),
    _hizSS(rhs._hizSS),
    _opaqueSS(rhs._opaqueSS)
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

            // Occlusion pyramid reduction program.
            proto->_hizSS = new osg::StateSet();
            osg::Program* hiz = new osg::Program();
            hiz->setName("Chonk occlusion pyramid");
            hiz->addShader(new osg::Shader(osg::Shader::COMPUTE, ShaderLoader::load(pkg.ChonkHiZ, pkg)));
            proto->_hizSS->setAttribute(hiz);

            // Opaque lists hold only instances that cannot fail the alpha test.
            proto->_opaqueSS = new osg::StateSet();
            proto->_opaqueSS->setDefine("OE_CHONK_OPAQUE");
        }
        _cullSS = proto->_cullSS;
        _hizSS = proto->_hizSS;
        _opaqueSS = proto->_opaqueSS;
    }

    // for each render bin instance, create the StateGraphs that we
    // will use to track the OSG state properly for each pass;
    // drawImplementation assigns their state sets every frame.
    for (auto* sg : {&_reset_sg, &_cull_sg, &_hiz_sg, &_cullLate_sg,
        &_early_sg[0], &_early_sg[1], &_late_sg[0], &_late_sg[1]})
    {
        *sg = new osgUtil::StateGraph();
    }
}


ChonkRenderBin::CullLeaf::CullLeaf(osgUtil::RenderLeaf* leaf, int pass,
    ChonkDrawable::OcclusionFrame* frame, GLbitfield barrier) :
    CustomRenderLeaf(leaf),
    _pass(pass),
    _barrier(barrier),
    _frame(frame)
{
    //nop
}

void
ChonkRenderBin::CullLeaf::draw(osg::State& state)
{
    if (_barrier)
        state.get<osg::GLExtensions>()->glMemoryBarrier(_barrier);
    auto d = static_cast<const ChonkDrawable*>(getDrawable());
    d->update_and_cull_batches(state, _pass, _frame.get());
}

ChonkRenderBin::DrawLeaf::DrawLeaf(osgUtil::RenderLeaf* leaf, int list, bool first, bool last, bool publish) :
    CustomRenderLeaf(leaf),
    _list(list),
    _first(first),
    _last(last),
    _publish(publish)
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
    }

    // Publish the preceding culling dispatches to both the vertex shader's instance
    // lookup and the indirect draw command reader before drawing their lists.
    if (_publish)
    {
        state.get<osg::GLExtensions>()->glMemoryBarrier(
            GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    }

    drawable->draw_batches(state, _list);

    if (_last)
    {
        // Keep OSG's VAO cache in sync so conventional geometry can follow us.
        state.unbindVertexArrayObject();

#ifdef RESET_BUFFER_BASE_BINDINGS
        auto& gl = ChonkDrawable::GLObjects::get(drawable->_globjects, state);
        gl._ext->glBindBufferBase(GL_SHADER_STORAGE_BUFFER,  0, 0);
        gl._ext->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 23, 0);
        gl._ext->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 29, 0);
        gl._ext->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 30, 0);
        gl._ext->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 31, 0);
#endif
    }
}

ChonkRenderBin::HiZLeaf::HiZLeaf(osgUtil::RenderLeaf* leaf, ChonkDrawable::OcclusionFrame* frame) :
    CustomRenderLeaf(leaf),
    _frame(frame)
{
    //nop
}

void
ChonkRenderBin::HiZLeaf::draw(osg::State& state)
{
    HiZResult result;
    _frame->hizValid = buildHiZ(state, _frame->camera, _frame->frameNumber, result);
    _frame->hiz = result.handle;
    _frame->hizParams = result.params;
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

    // draw graph: every pass draws under the first state graph's state, chosen before sorting.
    osgUtil::StateGraph* draw_sg = _renderLeafList.front()->_parent;
    draw_sg->_leaves.clear();

    // Nearest drawables first, so their depth rejects what they hide before it is shaded.
    // Stable, so equal depths keep the cull order.
    if (getFrontToBack())
    {
        std::stable_sort(_renderLeafList.begin(), _renderLeafList.end(),
            [](const osgUtil::RenderLeaf* a, const osgUtil::RenderLeaf* b) { return a->_depth < b->_depth; });
    }

    // Occlusion culling is for color cameras; shadow, depth and pick cameras render their own
    // depth targets.
    const osg::Camera* camera = ri.getCurrentCamera();
    const bool color = camera &&
        !CameraUtils::isShadowCamera(camera) &&
        !CameraUtils::isDepthCamera(camera) &&
        !CameraUtils::isPickCamera(camera);
    // Occlusion culling works on GPU-culled drawables; skip its passes when there are none.
    bool gpuCulled = false;
    for (auto* leaf : _renderLeafList)
        gpuCulled = gpuCulled || static_cast<const ChonkDrawable*>(leaf->getDrawable())->_gpucull;
    const bool occlusion = color && gpuCulled && getOcclusionCulling();
    const bool opaque = getOpaquePath();

    osg::ref_ptr<ChonkDrawable::OcclusionFrame> frame = new ChonkDrawable::OcclusionFrame();
    frame->camera = camera;
    frame->frameNumber = state.getFrameStamp() ? state.getFrameStamp()->getFrameNumber() : 0u;

    // Every pass graph hangs off the draw graph and adds only its own state. With the opaque path
    // off, the opaque lists draw with the alpha-tested shader, as everything did before.
    auto prepare = [draw_sg](osgUtil::StateGraph* sg, const osg::StateSet* ss)
    {
        sg->_parent = draw_sg;
        sg->_stateset = ss;
        sg->_leaves.clear();
    };
    const osg::StateSet* opaqueSS = opaque ? _opaqueSS.get() : nullptr;
    prepare(_reset_sg.get(), _cullSS.get());
    prepare(_cull_sg.get(), _cullSS.get());
    prepare(_hiz_sg.get(), _hizSS.get());
    prepare(_cullLate_sg.get(), _cullSS.get());
    for (auto* sg : {_early_sg[0].get(), _late_sg[0].get()})
        prepare(sg, opaqueSS);
    for (auto* sg : {_early_sg[1].get(), _late_sg[1].get()})
        prepare(sg, nullptr);

    const unsigned count = unsigned(_renderLeafList.size());
    _stateGraphList.clear();

    // One cull leaf per drawable; the first issues the pass's barrier.
    auto cullPass = [&](osgUtil::StateGraph* sg, int pass, GLbitfield barrier)
    {
        for (unsigned i = 0; i < count; ++i)
            sg->addLeaf(new CullLeaf(_renderLeafList[i], pass, frame.get(), i == 0 ? barrier : 0));
        _stateGraphList.push_back(sg);
    };
    // One draw leaf per drawable for a list; the first publishes culling results when asked.
    auto drawPass = [&](osgUtil::StateGraph* sg, int list, bool publish)
    {
        for (unsigned i = 0; i < count; ++i)
            sg->addLeaf(new DrawLeaf(_renderLeafList[i], list, false, false, publish && i == 0));
    };
    // Installs a draw graph; its first leaf binds a VAO and its last unbinds it.
    auto finish = [&](osgUtil::StateGraph* sg)
    {
        static_cast<DrawLeaf*>(sg->_leaves.front().get())->_first = true;
        static_cast<DrawLeaf*>(sg->_leaves.back().get())->_last = true;
        _stateGraphList.push_back(sg);
    };

    // Zero every drawable's lists, then cull them. The first barrier orders the previous
    // frame's indirect reads and visibility writes before this frame's writes.
    cullPass(_reset_sg.get(), ChonkDrawable::RESET, GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    cullPass(_cull_sg.get(), occlusion ? ChonkDrawable::EARLY : ChonkDrawable::CULL, GL_SHADER_STORAGE_BARRIER_BIT);
    drawPass(_early_sg[0].get(), ChonkDrawable::EARLY_OPAQUE, true);
    finish(_early_sg[0].get());
    drawPass(_early_sg[1].get(), ChonkDrawable::EARLY_CUTOUT, false);
    finish(_early_sg[1].get());

    if (occlusion)
    {
        // Build the pyramid from everything drawn so far, including earlier bins and terrain,
        // then test every instance against it and draw what the early pass missed.
        _hiz_sg->addLeaf(new HiZLeaf(_renderLeafList.front(), frame.get()));
        _stateGraphList.push_back(_hiz_sg.get());
        cullPass(_cullLate_sg.get(), ChonkDrawable::LATE, 0);
        drawPass(_late_sg[0].get(), ChonkDrawable::LATE_OPAQUE, true);
        finish(_late_sg[0].get());
        drawPass(_late_sg[1].get(), ChonkDrawable::LATE_CUTOUT, false);
        finish(_late_sg[1].get());
    }

    // install the new state graphs:
    _renderLeafList.clear();

    // dispatch.
    osgUtil::RenderBin::drawImplementation(ri, previous);
}

void
ChonkRenderBin::releaseSharedGLObjects(osg::State* state)
{
    auto proto = static_cast<ChonkRenderBin*>(osgUtil::RenderBin::getRenderBinPrototype("ChonkBin"));
    if (proto->_cullSS.valid())
        proto->_cullSS->releaseGLObjects(state);
    if (proto->_hizSS.valid())
        proto->_hizSS->releaseGLObjects(state);
    releaseHiZ(state);
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
