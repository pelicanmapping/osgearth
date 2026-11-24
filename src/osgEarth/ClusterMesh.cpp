/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/ClusterMesh>
#include <osgEarth/Notify>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>

#ifdef OSGEARTH_HAVE_MESH_OPTIMIZER
#include <meshoptimizer.h>
// clusterlod.h needs the cluster partitioning and permissive simplification of 0.25.
#if MESHOPTIMIZER_VERSION >= 250
#define OE_HAVE_CLUSTERLOD
#define CLUSTERLOD_IMPLEMENTATION
#include <cassert>
#include <meshoptimizer/clusterlod.h>
#endif
#endif

#define LC "[ClusterMesh] "

using namespace osgEarth;

namespace
{
    const char CACHE_MAGIC[4] = { 'O', 'E', 'C', 'M' };
    const std::uint32_t CACHE_VERSION = 3u;

    //! Pack a unit vector as signed normalized 10:10:10:2, the layout of GL_INT_2_10_10_10_REV.
    GLuint packNormal(const osg::Vec3f& n)
    {
        auto quantize = [](float v)
            {
                v = std::max(-1.0f, std::min(1.0f, v));
                return GLuint(int(std::round(v * 511.0f))) & 1023u;
            };
        return quantize(n.x()) | (quantize(n.y()) << 10) | (quantize(n.z()) << 20);
    }

    //! Inverse of packNormal; the result is not renormalized.
    osg::Vec3f unpackNormal(GLuint packed)
    {
        auto expand = [](GLuint bits)
            {
                int v = int(bits & 1023u);
                if (v > 511) v -= 1024;
                return float(v) / 511.0f;
            };
        return osg::Vec3f(expand(packed), expand(packed >> 10), expand(packed >> 20));
    }

    //! Incremental 64-bit FNV-1a, used to tie a cache file to the inputs that produced it.
    struct Hash
    {
        std::uint64_t value = 14695981039346656037ull;

        void add(const void* data, std::size_t bytes)
        {
            auto p = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < bytes; ++i)
                value = (value ^ p[i]) * 1099511628211ull;
        }

        template<typename T> void add(const T& v) { add(&v, sizeof(T)); }
    };

    //! Identify a build by everything that affects its output.
    std::uint64_t hashInputs(const std::vector<ClusterMesh::Vertex>& vertices, const std::vector<GLuint>& indices,
        const std::vector<ClusterMesh::Section>& sections, const ClusterMesh::BuildOptions& options)
    {
        Hash hash;
        hash.add(CACHE_VERSION);
        hash.add(vertices.data(), vertices.size() * sizeof(ClusterMesh::Vertex));
        hash.add(indices.data(), indices.size() * sizeof(GLuint));
        for (const auto& section : sections)
        {
            hash.add(section.firstIndex);
            hash.add(section.indexCount);
        }
        hash.add(options.maxTrianglesPerCluster);
        hash.add(options.normalWeight);
        return hash.value;
    }

    //! Checksum of a cache file's payload, so that a damaged file is rebuilt, not drawn.
    std::uint64_t hashContents(const std::vector<ClusterMesh::Vertex>& vertices, const std::vector<GLuint>& indices,
        const std::vector<ClusterMesh::Cluster>& clusters)
    {
        Hash hash;
        hash.add(vertices.data(), vertices.size() * sizeof(ClusterMesh::Vertex));
        hash.add(indices.data(), indices.size() * sizeof(GLuint));
        hash.add(clusters.data(), clusters.size() * sizeof(ClusterMesh::Cluster));
        return hash.value;
    }

    template<typename T> void writeValue(std::ostream& out, const T& v)
    {
        out.write(reinterpret_cast<const char*>(&v), sizeof(T));
    }

    template<typename T> void writeVector(std::ostream& out, const std::vector<T>& v)
    {
        writeValue(out, std::uint64_t(v.size()));
        out.write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size() * sizeof(T)));
    }

    template<typename T> bool readValue(std::istream& in, T& v)
    {
        return bool(in.read(reinterpret_cast<char*>(&v), sizeof(T)));
    }

    //! Read a length-prefixed array, rejecting lengths the remaining stream cannot hold.
    template<typename T> bool readVector(std::istream& in, std::vector<T>& v, std::uint64_t remainingBytes)
    {
        std::uint64_t count = 0;
        if (!readValue(in, count) || count > remainingBytes / sizeof(T))
            return false;
        v.resize(std::size_t(count));
        return bool(in.read(reinterpret_cast<char*>(v.data()), std::streamsize(count * sizeof(T))));
    }
}

ClusterMesh::Ptr
ClusterMesh::build(std::vector<Vertex> vertices, const std::vector<GLuint>& indices,
    const std::vector<Section>& sections, const BuildOptions& options)
{
    return build(std::move(vertices), indices, sections, options, std::string());
}

ClusterMesh::Ptr
ClusterMesh::build(std::vector<Vertex> vertices, const std::vector<GLuint>& indices,
    const std::vector<Section>& sections, const BuildOptions& options, const std::string& cachePath)
{
    std::shared_ptr<ClusterMesh> mesh(new ClusterMesh());

    const std::uint64_t hash = cachePath.empty() ? 0u : hashInputs(vertices, indices, sections, options);
    bool cached = false;
    if (!cachePath.empty())
    {
        std::ifstream in(cachePath, std::ios::binary);
        cached = in.is_open() && mesh->read(in, hash, sections.size());
    }

    if (!cached)
    {
        if (!mesh->buildClusters(std::move(vertices), indices, sections, options))
            return nullptr;

        if (!cachePath.empty())
        {
            std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
            if (!out.is_open() || !mesh->write(out, hash))
                OE_WARN << LC << "Could not write cache \"" << cachePath << "\"" << std::endl;
        }
    }

    mesh->bindSections(sections);
    return mesh;
}

ClusterMesh::Ptr
ClusterMesh::create(osg::Node* node, ChonkFactory& factory, const BuildOptions& options)
{
    return create(node, factory, std::string(), options);
}

ClusterMesh::Ptr
ClusterMesh::create(osg::Node* node, ChonkFactory& factory, const std::string& cachePath, const BuildOptions& options)
{
    OE_SOFT_ASSERT_AND_RETURN(node != nullptr, nullptr);

    // The Chonk ripper already flattens transforms and resolves every material and
    // texture into the factory's arena, so let it do that and re-shape its output.
    // Its vertex-cache pass is skipped: the hierarchy build orders every cluster itself.
    Chonk::Ptr chonk = Chonk::create();
    if (!factory.load(node, chonk.get(), 0.0f, FLT_MAX, false))
        return nullptr;

    std::vector<Vertex> vertices(chonk->_vbo_store.size());
    for (std::size_t i = 0; i < vertices.size(); ++i)
    {
        const auto& source = chonk->_vbo_store[i];
        vertices[i].position = source.position;
        vertices[i].normal = packNormal(source.normal);
        vertices[i].uv = source.uv;
        vertices[i].color = source.color;
    }

    // Group the triangles by material so that every cluster carries exactly one.
    std::map<GLushort, std::vector<GLuint>> byMaterial;
    const auto& elements = chonk->_ebo_store;
    for (std::size_t i = 0; i + 2 < elements.size(); i += 3)
    {
        auto& bucket = byMaterial[chonk->_vbo_store[elements[i]].material_index];
        bucket.insert(bucket.end(), elements.begin() + i, elements.begin() + i + 3);
    }

    std::vector<GLuint> indices;
    indices.reserve(elements.size());
    std::vector<Section> sections;
    for (const auto& entry : byMaterial)
    {
        Section section;
        section.firstIndex = unsigned(indices.size());
        section.indexCount = unsigned(entry.second.size());
        for (const auto& material : chonk->_materials)
            if (material->index == entry.first)
                section.material = material;
        section.alphaTested = chonk->_alphaMaterials.count(entry.first) > 0;
        section.linearColor = chonk->_vbo_store[entry.second.front()].color_is_linear != 0;
        sections.push_back(section);
        indices.insert(indices.end(), entry.second.begin(), entry.second.end());
    }

    return build(std::move(vertices), indices, sections, options, cachePath);
}

bool
ClusterMesh::buildClusters(std::vector<Vertex> vertices, const std::vector<GLuint>& sourceIndices,
    const std::vector<Section>& sections, const BuildOptions& options)
{
#ifndef OE_HAVE_CLUSTERLOD
    OE_WARN << LC << "osgEarth was built without meshoptimizer 0.25 or newer; cannot build cluster hierarchies"
        << std::endl;
    return false;
#else
    if (vertices.empty() || sourceIndices.size() < 3)
        return false;

    // Weld identical vertices so that clusters share them and the hierarchy sees the
    // connectivity; unindexed or per-primitive sources would otherwise never simplify.
    std::vector<GLuint> indices(sourceIndices);
    {
        for (auto index : indices)
            if (index >= vertices.size())
                return false;

        std::vector<unsigned> remap(vertices.size());
        const std::size_t unique = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(),
            vertices.data(), vertices.size(), sizeof(Vertex));
        meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
        std::vector<Vertex> welded(unique);
        meshopt_remapVertexBuffer(welded.data(), vertices.data(), vertices.size(), sizeof(Vertex), remap.data());
        vertices.swap(welded);
    }

    // Normals guide the simplifier; texture coordinates only mark the seams it must keep.
    struct Attributes { float nx, ny, nz, u, v; };
    std::vector<Attributes> attributes(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i)
    {
        osg::Vec3f n = unpackNormal(vertices[i].normal);
        n.normalize();
        attributes[i] = { n.x(), n.y(), n.z(), vertices[i].uv.x(), vertices[i].uv.y() };
    }
    const float weights[3] = { options.normalWeight, options.normalWeight, options.normalWeight };

    struct Pending
    {
        clodBounds bounds;
        int group = -1;   // group this cluster belongs to
        int refined = -1; // group whose simplification produced it
        unsigned section = 0u;
        std::size_t firstIndex = 0u, indexCount = 0u;
    };
    std::vector<Pending> pending;
    std::vector<clodBounds> groups;
    std::vector<GLuint> clusterIndices;

    const std::size_t maxTriangles = std::max(8u, std::min(256u, options.maxTrianglesPerCluster));
    _numSourceTriangles = 0u;

    // Each section is simplified on its own and picks its level of detail on its own, so
    // an edge where two sections meet would open into a crack. Pin every position that
    // more than one section uses; those vertices then survive unchanged at every level.
    std::vector<unsigned char> sectionLocks;
    if (sections.size() > 1u)
    {
        std::vector<unsigned> canonical(vertices.size());
        meshopt_generatePositionRemap(canonical.data(), vertices[0].position.ptr(), vertices.size(), sizeof(Vertex));

        const unsigned UNUSED = ~0u, SHARED = ~0u - 1u;
        std::vector<unsigned> owner(vertices.size(), UNUSED);
        for (std::size_t s = 0; s < sections.size(); ++s)
        {
            const std::size_t first = sections[s].firstIndex;
            const std::size_t end = std::min(indices.size(), first + sections[s].indexCount);
            for (std::size_t i = first; i < end; ++i)
            {
                unsigned& o = owner[canonical[indices[i]]];
                if (o == UNUSED) o = unsigned(s);
                else if (o != unsigned(s)) o = SHARED;
            }
        }

        sectionLocks.resize(vertices.size(), 0);
        for (std::size_t i = 0; i < vertices.size(); ++i)
            if (owner[canonical[i]] == SHARED)
                sectionLocks[i] = meshopt_SimplifyVertex_Lock;
    }

    for (std::size_t s = 0; s < sections.size(); ++s)
    {
        const auto& section = sections[s];
        const std::size_t count = section.indexCount - section.indexCount % 3u;
        if (count == 0u || std::size_t(section.firstIndex) + count > indices.size())
            continue;

        clodConfig config = clodDefaultConfig(maxTriangles);
        // Tight per-cluster spheres cull better than the merged group spheres.
        config.optimize_bounds = true;

        clodMesh mesh = {};
        mesh.indices = &indices[section.firstIndex];
        mesh.index_count = count;
        mesh.vertex_count = vertices.size();
        mesh.vertex_positions = vertices[0].position.ptr();
        mesh.vertex_positions_stride = sizeof(Vertex);
        mesh.vertex_attributes = &attributes[0].nx;
        mesh.vertex_attributes_stride = sizeof(Attributes);
        mesh.attribute_weights = weights;
        mesh.attribute_count = 3;
        mesh.attribute_protect_mask = (1u << 3) | (1u << 4);
        mesh.vertex_lock = sectionLocks.empty() ? nullptr : sectionLocks.data();

        // Group IDs are global across sections, so "refined" needs no translation.
        clodBuild(config, mesh, [&](clodGroup group, const clodCluster* clusters, size_t clusterCount)
            {
                const int id = int(groups.size());
                groups.push_back(group.simplified);
                for (size_t i = 0; i < clusterCount; ++i)
                {
                    Pending p;
                    p.bounds = clusters[i].bounds;
                    p.group = id;
                    p.refined = clusters[i].refined;
                    p.section = unsigned(s);
                    p.firstIndex = clusterIndices.size();
                    p.indexCount = clusters[i].index_count;
                    clusterIndices.insert(clusterIndices.end(),
                        clusters[i].indices, clusters[i].indices + clusters[i].index_count);
                    pending.push_back(p);
                }
                return id;
            });

        _numSourceTriangles += unsigned(count / 3u);
    }

    if (pending.empty())
        return false;

    auto sphere = [](const clodBounds& b) { return osg::Vec4f(b.center[0], b.center[1], b.center[2], b.radius); };

    std::vector<Cluster> clusters(pending.size());
    for (std::size_t i = 0; i < pending.size(); ++i)
    {
        const auto& p = pending[i];
        auto& c = clusters[i];
        c.bounds = sphere(p.bounds);
        if (p.refined >= 0)
        {
            c.lodSelf = sphere(groups[p.refined]);
            c.selfError = groups[p.refined].error;
        }
        else
        {
            c.lodSelf = c.bounds;
            c.selfError = 0.0f;
        }
        c.lodParent = sphere(groups[p.group]);
        c.parentError = std::min(groups[p.group].error, ROOT_ERROR);
        c.material = p.section;
    }

    // Coarsest first: a renderer can then bound the clusters worth testing for an
    // instance with two binary searches instead of visiting the whole hierarchy.
    std::vector<unsigned> order(clusters.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b)
        {
            if (clusters[a].parentError != clusters[b].parentError)
                return clusters[a].parentError > clusters[b].parentError;
            return clusters[a].selfError > clusters[b].selfError;
        });

    _clusters.clear();
    _clusters.reserve(clusters.size());
    _indices.clear();
    _indices.reserve(clusterIndices.size());
    float prefixMin = FLT_MAX;
    for (unsigned source : order)
    {
        Cluster c = clusters[source];
        const auto& p = pending[source];
        c.firstIndex = GLuint(_indices.size());
        c.indexCount = GLuint(p.indexCount);
        prefixMin = std::min(prefixMin, c.selfError);
        c.prefixMinSelfError = prefixMin;
        _indices.insert(_indices.end(),
            clusterIndices.begin() + p.firstIndex, clusterIndices.begin() + p.firstIndex + p.indexCount);
        _clusters.push_back(c);
    }

    // Order the vertices by first use. The coarse levels come first in the index buffer,
    // so the vertices that distant instances touch end up packed together.
    {
        std::vector<unsigned> remap(vertices.size());
        const std::size_t used = meshopt_optimizeVertexFetchRemap(remap.data(), _indices.data(), _indices.size(),
            vertices.size());
        meshopt_remapIndexBuffer(_indices.data(), _indices.data(), _indices.size(), remap.data());
        _vertices.resize(used);
        meshopt_remapVertexBuffer(_vertices.data(), vertices.data(), vertices.size(), sizeof(Vertex), remap.data());
    }

    return true;
#endif
}

void
ClusterMesh::bindSections(const std::vector<Section>& sections)
{
    _materials.clear();
    _hasAlphaTest = false;
    for (auto& cluster : _clusters)
    {
        const auto& section = sections[cluster.material];
        cluster.material = section.material ? GLuint(section.material->index) : NO_MATERIAL;
        cluster.flags = (section.alphaTested ? FLAG_ALPHA_TESTED : 0u) | (section.linearColor ? FLAG_LINEAR_COLOR : 0u);
        _hasAlphaTest = _hasAlphaTest || section.alphaTested;
    }
    for (const auto& section : sections)
        if (section.material)
            _materials.push_back(section.material);

    _box.init();
    for (const auto& vertex : _vertices)
        _box.expandBy(vertex.position);

    // The instance-level LOD bounds assume that every cluster's LOD spheres lie inside
    // this sphere, so grow it to hold them rather than only the triangles.
    const osg::Vec3f center = _box.center();
    float radius = _box.radius();
    auto enclose = [&](const osg::Vec4f& s)
        {
            radius = std::max(radius, (osg::Vec3f(s.x(), s.y(), s.z()) - center).length() + s.w());
        };
    for (const auto& cluster : _clusters)
    {
        enclose(cluster.bounds);
        enclose(cluster.lodSelf);
        enclose(cluster.lodParent);
    }
    _sphere.set(center.x(), center.y(), center.z(), radius);
}

void
ClusterMesh::selectCut(float error, std::vector<unsigned>& output) const
{
    for (unsigned i = 0; i < _clusters.size(); ++i)
        if (_clusters[i].selfError <= error && _clusters[i].parentError > error)
            output.push_back(i);
}

bool
ClusterMesh::write(std::ostream& out, std::uint64_t sourceHash) const
{
    out.write(CACHE_MAGIC, sizeof(CACHE_MAGIC));
    writeValue(out, CACHE_VERSION);
    writeValue(out, sourceHash);
    writeValue(out, hashContents(_vertices, _indices, _clusters));
    writeValue(out, std::uint32_t(_numSourceTriangles));
    writeVector(out, _vertices);
    writeVector(out, _indices);
    writeVector(out, _clusters);
    return bool(out);
}

bool
ClusterMesh::read(std::istream& in, std::uint64_t sourceHash, std::size_t numSections)
{
    in.seekg(0, std::ios::end);
    const std::uint64_t size = std::uint64_t(in.tellg());
    in.seekg(0, std::ios::beg);

    char magic[4];
    std::uint32_t version = 0, triangles = 0;
    std::uint64_t hash = 0, contents = 0;
    bool ok = in.read(magic, sizeof(magic)) && std::memcmp(magic, CACHE_MAGIC, sizeof(magic)) == 0 &&
        readValue(in, version) && version == CACHE_VERSION &&
        readValue(in, hash) && hash == sourceHash &&
        readValue(in, contents) && readValue(in, triangles) &&
        readVector(in, _vertices, size) && readVector(in, _indices, size) && readVector(in, _clusters, size) &&
        contents == hashContents(_vertices, _indices, _clusters);

    // The checksum catches damage; these checks also keep a file from another build of
    // osgEarth, or a deliberately crafted one, from producing out-of-range draws.
    for (std::size_t i = 0; ok && i < _clusters.size(); ++i)
    {
        const auto& c = _clusters[i];
        ok = c.material < numSections && c.indexCount % 3u == 0u &&
            std::uint64_t(c.firstIndex) + c.indexCount <= _indices.size();
    }
    for (std::size_t i = 0; ok && i < _indices.size(); ++i)
        ok = _indices[i] < _vertices.size();

    if (!ok || _clusters.empty())
    {
        _vertices.clear();
        _indices.clear();
        _clusters.clear();
        return false;
    }

    _numSourceTriangles = triangles;
    return true;
}
