/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "MaterialArena"
#include "TextureArena"
#include <stdexcept>

using namespace osgEarth;

constexpr unsigned MaterialArena::CAPACITY;
constexpr unsigned MaterialArena::BINDING_POINT;

static_assert(sizeof(MaterialArena::GPU) == 48, "Material SSBO layout must match Chonk.glsl");
static_assert(offsetof(MaterialArena::GPU, extended) == 40, "Material SSBO member alignment");

// Keep textures alive independently of TextureArena's auto-release setting.
// Registration is deferred until getOrCreate has installed all lookup entries.
MaterialArena::Material::Material(MaterialArena* arena, GLushort id,
    const Indices& indices, const osg::Vec2i& ext,
    const std::array<std::shared_ptr<Texture>, 5>& refs) :
    index(id), textures(indices), extended(ext), _arena(arena), _textures(refs)
{
}

// Failed registration must not return an ID that was never acquired.
MaterialArena::Material::~Material()
{
    if (_registered) _arena->release(*this);
}

// Legacy shader IDs distinguish materials even when their texture slots match.
MaterialArena::Key
MaterialArena::key(const Indices& indices, const osg::Vec2i& extended)
{
    return {{indices[0], indices[1], indices[2], indices[3], indices[4], extended.x(), extended.y()}};
}

// Deduplicate materials across Chonks sharing a TextureArena and allocate bounded,
// reusable IDs. Each returned owner pins both the ID and its texture references.
MaterialArena::Material::Ptr
MaterialArena::getOrCreate(TextureArena& textures, const Indices& indices, const osg::Vec2i& extended)
{
    // Acquire texture references BEFORE our lock: TextureArena::apply locks in
    // the opposite direction. These references prevent texture-slot recycling.
    std::array<std::shared_ptr<Texture>, 5> refs;
    for (unsigned i = 0; i < refs.size(); ++i)
        if (indices[i] >= 0)
        {
            refs[i] = textures.find(unsigned(indices[i]));
            if (!refs[i]) throw std::invalid_argument("Material references an unregistered texture");
        }
    std::shared_ptr<Material> material; // destroy only after unlocking
    std::lock_guard<std::mutex> lock(_mutex);
    if (_textures && _textures != &textures)
        throw std::invalid_argument("MaterialArena cannot mix TextureArenas");
    _textures = &textures;
    const auto k = key(indices, extended);
    auto found = _lookup.find(k);
    if (found != _lookup.end())
    {
        auto existing = _materials[found->second].lock();
        if (existing) return existing;
        // The last owner is in its destructor and waiting for our lock.
        // Do not recycle its slot until release() has completed.
    }
    if (_free.empty() && _materials.size() == CAPACITY)
        throw std::length_error("MaterialArena exhausted its 65536 live material IDs");
    const GLushort index = _free.empty() ? GLushort(_materials.size()) : _free.back();
    // Allocate before registering the material. Returning an ID in its
    // destructor must never need to allocate memory.
    if (_free.capacity() <= _materials.size())
        _free.reserve(std::max(std::size_t(8), _free.capacity() * 2));
    if (_materials.size() == _materials.capacity())
        _materials.reserve(std::max(std::size_t(8), _materials.capacity() * 2));
    material.reset(new Material(this, index, indices, extended, refs));
    _lookup[k] = index;
    if (_free.empty()) _materials.emplace_back();
    else _free.pop_back();
    _materials[index] = material;
    material->_registered = true;
    ++_revision;
    return material;
}

// Release can race with a new registration of the same key; erase only the
// mapping for this ID, then make the slot reusable without allocating memory.
void MaterialArena::release(const Material& material)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto found = _lookup.find(key(material.textures, material.extended));
    if (found != _lookup.end() && found->second == material.index) _lookup.erase(found);
    _materials[material.index].reset();
    _free.push_back(material.index);
    ++_revision;
}

// Lock the weak reference while holding the arena mutex so a successful lookup
// extends the material's lifetime before the slot can be recycled.
MaterialArena::Material::Ptr MaterialArena::find(GLushort index) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return index < _materials.size() ? _materials[index].lock() : Material::Ptr();
}

// Report registered keys, rather than the high-water size of the slot vector.
std::size_t MaterialArena::size() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _lookup.size();
}

// Resolve handles only when a material or texture revision changes. Cache the
// resulting buffer per GL share group, but bind it in every calling context.
void MaterialArena::apply(osg::State& state, const std::vector<GLuint64>& handles,
    std::uint64_t texturesRevision, unsigned handleStride) const
{
    // Keep temporary owners alive until AFTER unlocking; their destructors
    // return IDs to this arena and therefore acquire the same mutex.
    std::vector<Material::Ptr> owners;
    std::lock_guard<std::mutex> lock(_mutex);
    if (_materials.empty()) return;
    auto& gl = GLObjects::get(_globjects, state);
    if (!gl.buffer || !gl.buffer->valid() ||
        gl.materialsRevision != _revision || gl.texturesRevision != texturesRevision)
    {
        std::vector<GPU> records(_materials.size());
        owners.reserve(_lookup.size());
        for (unsigned i = 0; i < _materials.size(); ++i)
        {
            auto material = _materials[i].lock();
            if (!material) continue;
            owners.push_back(material);
            // Missing or paged-out maps resolve to zero; respect UBO padding.
            auto handle = [&](unsigned slot) {
                int index = material->textures[slot];
                const auto offset = std::size_t(index) * handleStride;
                return index >= 0 && offset < handles.size() ? handles[offset] : GLuint64(0);
            };
            auto& record = records[i];
            record.albedo = handle(0); record.normal = handle(1); record.pbr = handle(2);
            record.material1 = handle(3); record.material2 = handle(4);
            record.extended[0] = material->extended.x(); record.extended[1] = material->extended.y();
        }
        if (!gl.buffer || !gl.buffer->valid())
        {
            gl.buffer = GLBuffer::create_shared(GL_SHADER_STORAGE_BUFFER, state);
            gl.buffer->bind();
            gl.buffer->debugLabel("MaterialArena", "Resolved materials");
            gl.buffer->unbind();
        }
        gl.buffer->uploadData(records);
        gl.materialsRevision = _revision;
        gl.texturesRevision = texturesRevision;
    }
    gl.buffer->bindBufferBase(BINDING_POINT);
}

// Follow OSG's context-cache sizing contract without creating GL objects here.
void MaterialArena::resizeGLObjectBuffers(unsigned size)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_globjects.size() < size) _globjects.resize(size);
}

// Preserve CPU material IDs while dropping cached buffers. The next apply
// reconstructs the affected share group's table from its current texture handles.
void MaterialArena::releaseGLObjects(osg::State* state) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (state) GLObjects::get(_globjects, *state) = GLObjects();
    else for (unsigned i = 0; i < _globjects.size(); ++i) _globjects[i] = GLObjects();
}
