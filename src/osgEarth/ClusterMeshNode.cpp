/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/ClusterMeshNode>
#include <osgEarth/CameraUtils>
#include <osgEarth/DepthPyramid>
#include <osgEarth/GLUtils>
#include <osgEarth/Registry>
#include <osgEarth/ShaderLoader>
#include <osgEarth/Shaders>
#include <osgEarth/VirtualProgram>

#include <osg/CullFace>
#include <osg/Drawable>
#include <osg/Program>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <mutex>
#include <unordered_map>

#define LC "[ClusterMeshNode] "

using namespace osgEarth;
using namespace osgEarth::Util;

#ifndef GL_PARAMETER_BUFFER
#define GL_PARAMETER_BUFFER 0x80EE
#endif
#ifndef GL_DISPATCH_INDIRECT_BUFFER
#define GL_DISPATCH_INDIRECT_BUFFER 0x90EE
#endif
#ifndef GL_INT_2_10_10_10_REV
#define GL_INT_2_10_10_10_REV 0x8D9F
#endif

namespace
{
    // Keep in sync with DISPATCH_WIDTH and local_size_x in ClusterMesh.Cull.glsl.
    const unsigned DISPATCH_WIDTH = 256u;
    const unsigned LOCAL_SIZE = 64u;

    // Closest distance, in view units, at which an error is projected. It bounds the detail
    // requested where the camera touches or enters a mesh. The projection's own near plane
    // cannot serve: with a logarithmic depth buffer it is often kilometers away.
    const float MIN_LOD_DISTANCE = 0.1f;

    // Buffer binding points, shared with ClusterMesh.Cull.glsl and ClusterMesh.glsl.
    // Zero, 23, and 31 are the three the draw program reads.
    enum Binding
    {
        DRAW_DATA = 0, BATCHES = 23, HIDDEN_CLUSTERS = 24, HIDDEN_INSTANCES = 25, COUNTERS = 26, COMMANDS = 27,
        CHUNKS = 28, CLUSTERS = 29, MESHES = 30, INSTANCES = 31
    };

    // Uniform locations declared in ClusterMesh.Cull.glsl.
    enum Location
    {
        PASS = 0, MIN_INSTANCE_PIXELS = 1, PLANES = 2, LOD = 7, SIZES = 8, VIEW_SCALE = 9, OCCLUSION = 10,
        TEST_PROJECTION = 12, HIZ_PARAMS = 13, HIZ_DEPTH_MAPPING = 14, HIZ = 15
    };

    // Entries per draw list before any cull has asked for more.
    const unsigned INITIAL_DRAW_CAPACITY = 262144u;

    // Mesh index of an instance slot that holds no instance.
    const GLuint NO_MESH = 0xffffffffu;

    // Values of oe_cm_pass and oe_cm_occlusion in ClusterMesh.Cull.glsl.
    enum CullPass { PASS_INSTANCES = 0, PASS_CLUSTERS = 1, PASS_LATE_INSTANCES = 2, PASS_LATE_CLUSTERS = 3 };
    enum Occlusion { OCCLUSION_OFF = 0, OCCLUSION_EARLY = 1, OCCLUSION_LATE = 2 };

    // Draw lists, in the order of Counters::drawCount. The late lists hold what the early
    // phase of occlusion culling wrongly predicted to be hidden.
    enum List { EARLY_OPAQUE = 0, EARLY_CUTOUT = 1, LATE_OPAQUE = 2, LATE_CUTOUT = 3, NUM_LISTS = 4 };

    struct MeshRecord
    {
        osg::Vec4f bounds;
        GLuint firstCluster = 0u;
        GLuint clusterCount = 0u;
        GLuint pad0 = 0u, pad1 = 0u;
    };

    struct InstanceRecord
    {
        osg::Matrixf xform;
        GLuint mesh = NO_MESH;
        GLfloat scale = 1.0f;
        GLuint batch = 0u;
        GLuint pad0 = 0u;
    };

    // Rebuilt for every cull, since both matrices depend on the camera.
    struct BatchRecord
    {
        osg::Matrixf modelView;     // all zeros hides the batch
        osg::Matrixf testModelView;
    };

    // Each X, Y, Z triple is the argument of an indirect compute dispatch.
    struct Counters
    {
        GLuint chunkCount = 0u;
        GLuint dispatchX = DISPATCH_WIDTH, dispatchY = 0u, dispatchZ = 1u;
        GLuint drawCount[NUM_LISTS] = { 0u, 0u, 0u, 0u };
        GLuint triangleCount = 0u;
        GLuint candidateCount = 0u;
        GLuint hiddenInstanceCount = 0u;
        GLuint hiddenInstanceX = DISPATCH_WIDTH, hiddenInstanceY = 0u, hiddenInstanceZ = 1u;
        GLuint hiddenClusterCount = 0u;
        GLuint hiddenClusterX = DISPATCH_WIDTH, hiddenClusterY = 0u, hiddenClusterZ = 1u;
        GLuint occludedInstanceCount = 0u;
        GLuint occludedClusterCount = 0u;
        // Not reset by a cull; see ClusterMesh.Cull.glsl.
        GLuint drawDemand = 0u;
        GLuint chunkDemand = 0u;
    };

    struct DrawRecord { GLuint instance, material, cluster, flags; };

    static_assert(sizeof(ClusterMesh::Vertex) == 28, "ClusterMesh vertex layout must match the VAO formats");
    static_assert(sizeof(ClusterMesh::Cluster) == 80, "ClusterMesh::Cluster must match ClusterMesh.Cull.glsl");
    static_assert(sizeof(MeshRecord) == 32, "Mesh record must match ClusterMesh.Cull.glsl");
    static_assert(sizeof(InstanceRecord) == 80, "Instance record must match the GLSL instance struct");
    static_assert(sizeof(BatchRecord) == 128, "Batch record must match the GLSL batch struct");
    static_assert(sizeof(Counters) == 88, "Counters must match ClusterMesh.Cull.glsl");
    static_assert(offsetof(Counters, drawDemand) == 80 && offsetof(Counters, chunkDemand) == 84,
        "The demand fields must end the counters");
    static_assert(sizeof(DrawElementsIndirectCommand) == 20, "Indirect command stride");

    //! Everything that decides the outcome of a cull; an unchanged key means the command
    //! buffers from the previous cull are still exactly right.
    struct CullKey
    {
        const osg::Camera* camera = nullptr; // identity only
        osg::Matrixd modelView, projection;
        int width = 0, height = 0;
        float threshold = 0.0f, minInstancePixels = 0.0f;
        bool occlusion = false; // whether occlusion culling applies to this camera
        std::uint64_t meshRevision = 0u, instanceRevision = 0u;
        std::uint64_t activity = 0u; // hash of the batches that draw

        bool operator == (const CullKey& rhs) const
        {
            return camera == rhs.camera && width == rhs.width && height == rhs.height &&
                threshold == rhs.threshold && minInstancePixels == rhs.minInstancePixels &&
                occlusion == rhs.occlusion && activity == rhs.activity &&
                meshRevision == rhs.meshRevision && instanceRevision == rhs.instanceRevision &&
                modelView == rhs.modelView && projection == rhs.projection;
        }
    };

    //! A set of instances that share a frame. The node's table, the scene graph node that
    //! stands for the batch, and each GL state's snapshot of the table all refer to it.
    //! Everything but lastTraversed is written under the node's mutex before the batch
    //! is published, or by removal.
    struct Batch
    {
        osg::Matrixd frame;       // batch coordinates to node coordinates
        osg::BoundingBox box;     // of its instances, in node coordinates
        unsigned slot = 0u;       // index of its record in the batch buffer
        unsigned first = 0u, count = 0u; // its run of instance slots; unused by the node's own batch
        std::uint64_t sourceTriangles = 0u;
        bool tracked = false;     // draws only in frames whose cull traversal reached its node
        bool removed = false;
        std::atomic<long long> lastTraversed{ -2 }; // frame number of that traversal
    };

    //! The compute program that culls, shared by every node.
    osg::Program* getCullProgram()
    {
        static osg::ref_ptr<osg::Program> s_program;
        static std::mutex s_mutex;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_program.valid())
        {
            Shaders pkg;
            s_program = new osg::Program();
            s_program->setName("ClusterMesh cull");
            s_program->addShader(new osg::Shader(osg::Shader::COMPUTE, ShaderLoader::load(pkg.ClusterMeshCull, pkg)));
        }
        return s_program.get();
    }

    //! The compute program that builds depth pyramids, shared by every node.
    osg::Program* getPyramidProgram()
    {
        static osg::ref_ptr<osg::Program> s_program;
        static std::mutex s_mutex;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_program.valid())
            s_program = DepthPyramid::createProgram();
        return s_program.get();
    }

    //! This context's instance of a shared program, linked on first use; null if it fails.
    //! The instance depends on the shader defines in effect, such as the depth buffer mode.
    osg::Program::PerContextProgram* link(osg::Program* program, osg::State& state)
    {
        auto* pcp = program->getPCP(state);
        if (pcp->needsLink())
            program->compileGLObjects(state);
        return pcp->isLinked() ? pcp : nullptr;
    }

    //! Occlusion culling is for color cameras; shadow, depth, and pick cameras render
    //! their own depth targets.
    bool isColorCamera(const osg::Camera* camera)
    {
        return camera && !CameraUtils::isShadowCamera(camera) && !CameraUtils::isDepthCamera(camera) &&
            !CameraUtils::isPickCamera(camera);
    }
}

struct ClusterMeshNode::Impl
{
    //! What occlusion culling remembers about one camera between frames.
    struct View
    {
        const osg::Camera* key = nullptr; // identity only
        osg::observer_ptr<const osg::Camera> camera;
        unsigned lastUsed = 0u;
        bool valid = false; // the pyramid below was built by this camera's last late phase
        std::uint64_t generation = 0u; // DepthPyramid::generation when it was built
        osg::Matrixd modelView, projection; // the camera that rendered the pyramid
        DepthPyramid::Result pyramid;
    };

    //! Buffers and cull state for one OSG state. Culling and drawing happen back to back
    //! on that state's draw thread, so one set of work buffers serves every camera.
    struct GLObjects : public PerStateGLObjects
    {
        osg::GLExtensions* ext = nullptr;
        GLVAO::Ptr vao;
        GLBuffer::Ptr vertices, indices, clusters, meshes, instances, batches;
        GLBuffer::Ptr chunks, commands, drawData, counters, statsStaging, hiddenInstances, hiddenClusters;
        GLBuffer::Ptr demandStaging;
        // The most draw-list entries and chunks that any cull on this state has wanted.
        unsigned drawDemand = 0u, chunkDemand = 0u;
        unsigned demandFrame = ~0u; // frame whose first draw read the demand
        std::uint64_t meshRevision = 0u, instanceRevision = 0u;
        unsigned numInstances = 0u;  // instance slots in use, free ones among them
        unsigned liveInstances = 0u; // instances in those slots
        unsigned instanceCapacity = 0u, batchCapacity = 0u;
        unsigned drawCapacity = 0u, chunkCapacity = 0u, hiddenInstanceCapacity = 0u;
        // The batch table as of the instances in the buffer, indexed by slot.
        std::vector<std::shared_ptr<Batch>> batchTable;
        std::vector<unsigned char> batchActive; // which of them draw in the frame being drawn
        std::vector<BatchRecord> batchRecords;  // scratch for the upload
        // Settings captured under the node's mutex for the draw in progress.
        float threshold = 1.0f;
        float minInstancePixels = 0.0f;
        bool collectStats = false;
        bool occlusion = true;
        unsigned statsFrame = ~0u; // frame whose first main-camera draw read the counters
        bool culled = false;     // the buffers hold the early phase for key
        unsigned cullFrame = 0u; // the frame that phase ran in
        bool lateNeeded = false; // that cull recorded hidden work for a late phase
        bool lateDone = false;   // the late phase for key has run
        CullKey key;
        std::vector<View> views;

        void(GL_APIENTRY * glMultiDrawElementsIndirectCount)
            (GLenum, GLenum, const GLvoid*, GLintptr, GLsizei, GLsizei) = nullptr;
        void(GL_APIENTRY * glDispatchComputeIndirect)(GLintptr) = nullptr;
        void(GL_APIENTRY * glVertexAttribFormat)(GLuint, GLint, GLenum, GLboolean, GLuint) = nullptr;
        void(GL_APIENTRY * glVertexArrayElementBuffer)(GLuint, GLuint) = nullptr;

        //! Drop every GL object; the next draw rebuilds them.
        void release()
        {
            *this = GLObjects();
        }

        //! The remembered state of a camera, created on first use. Also forgets cameras
        //! that were destroyed or have not drawn for a while.
        View& view(const osg::Camera* camera, unsigned frame)
        {
            for (auto i = views.begin(); i != views.end(); )
            {
                if (!i->camera.valid() || (i->key != camera && frame - i->lastUsed > 600u))
                    i = views.erase(i);
                else
                    ++i;
            }
            for (auto& v : views)
            {
                if (v.key == camera)
                {
                    v.lastUsed = frame;
                    return v;
                }
            }
            views.emplace_back();
            views.back().key = camera;
            views.back().camera = camera;
            views.back().lastUsed = frame;
            return views.back();
        }
    };

    //! A change to a run of instance slots that GL states still have to upload.
    struct DirtyRun
    {
        std::uint64_t revision;
        unsigned first, count;
    };

    mutable std::mutex mutex;
    std::vector<ClusterMesh::Ptr> meshes;
    std::vector<MeshRecord> meshRecords;
    std::vector<GLuint> meshFirstVertex, meshFirstIndex;
    std::vector<ClusterMesh::Cluster> clusters; // every mesh's clusters, rebased into the shared buffers
    std::size_t numVertices = 0u, numIndices = 0u;
    // Instance slots. Each add and each batch owns a contiguous run of them; the slots of
    // a removed batch are marked NO_MESH and listed in freeRuns for the next allocation.
    std::vector<InstanceRecord> instances;
    std::vector<std::pair<unsigned, unsigned>> freeRuns; // (first, count), ordered by first
    std::size_t liveInstances = 0u;
    // Slot zero is the node's own batch, which holds what add was given. Free slots are null.
    std::vector<std::shared_ptr<Batch>> batches{ std::make_shared<Batch>() };
    std::vector<unsigned> freeBatchSlots;
    // Changes since dirtyFloor, oldest first. A GL state behind the floor uploads everything.
    std::vector<DirtyRun> dirtyRuns;
    std::uint64_t dirtyFloor = 0u;
    std::uint64_t meshRevision = 1u, instanceRevision = 1u;
    std::uint64_t sourceTriangles = 0u;
    osg::BoundingBox box;
    bool boxDirty = false;  // box must be rebuilt from the batches
    bool hasAlphaTest = false;
    // Set by changes made off the update thread; the node's update traversal applies them
    // to the scene graph.
    std::atomic<bool> graphDirty{ false };

    float threshold = 1.0f;
    float minInstancePixels = 0.0f;
    unsigned maxDrawnClusters = 4194304u;
    bool collectStats = false;
    bool occlusion = true;
    Stats stats;

    mutable osg::buffered_object<GLObjects> globjects;

    //! The depth pyramids are kept by DepthPyramid under this object's address.
    ~Impl()
    {
        DepthPyramid::releaseOwner(this);
    }

    //! Index of the mesh in the shared tables, appending it on first use. Returns -1 when
    //! the shared buffers cannot hold it. Caller holds the mutex.
    int findOrAddMesh(const ClusterMesh::Ptr& mesh)
    {
        for (std::size_t i = 0; i < meshes.size(); ++i)
            if (meshes[i] == mesh)
                return int(i);

        const std::size_t limit = std::size_t(std::numeric_limits<GLsizei>::max());
        if ((numVertices + mesh->vertices().size()) * sizeof(ClusterMesh::Vertex) > limit ||
            (numIndices + mesh->indices().size()) * sizeof(GLuint) > limit ||
            (clusters.size() + mesh->clusters().size()) * sizeof(ClusterMesh::Cluster) > limit)
        {
            OE_WARN << LC << "Mesh does not fit; a single node holds at most 2 GB per buffer" << std::endl;
            return -1;
        }

        MeshRecord record;
        record.bounds = mesh->boundingSphere();
        record.firstCluster = GLuint(clusters.size());
        record.clusterCount = GLuint(mesh->clusters().size());

        for (auto cluster : mesh->clusters())
        {
            cluster.firstIndex += GLuint(numIndices);
            cluster.baseVertex = GLuint(numVertices);
            clusters.push_back(cluster);
        }

        meshFirstVertex.push_back(GLuint(numVertices));
        meshFirstIndex.push_back(GLuint(numIndices));
        numVertices += mesh->vertices().size();
        numIndices += mesh->indices().size();
        hasAlphaTest = hasAlphaTest || mesh->hasAlphaTest();
        meshes.push_back(mesh);
        meshRecords.push_back(record);
        ++meshRevision;
        return int(meshRecords.size()) - 1;
    }

    //! Record that a run of instance slots changed, as a new revision. Caller holds the mutex.
    void markDirty(unsigned first, unsigned count)
    {
        // Past this many pending changes, one whole upload is cheaper than tracking them.
        const std::size_t limit = 256u;
        if (dirtyRuns.size() >= limit)
        {
            dirtyFloor = dirtyRuns[limit / 2u - 1u].revision;
            dirtyRuns.erase(dirtyRuns.begin(), dirtyRuns.begin() + limit / 2u);
        }
        dirtyRuns.push_back({ ++instanceRevision, first, count });
    }

    //! Reserve a run of instance slots, reusing freed ones when a free run is long enough.
    //! Returns false when the instance buffer would pass 2 GB. Caller holds the mutex.
    bool allocateRun(unsigned count, unsigned& first)
    {
        for (auto run = freeRuns.begin(); run != freeRuns.end(); ++run)
        {
            if (run->second >= count)
            {
                first = run->first;
                run->first += count;
                run->second -= count;
                if (run->second == 0u)
                    freeRuns.erase(run);
                return true;
            }
        }
        if (instances.size() + count > std::size_t(std::numeric_limits<GLsizei>::max()) / sizeof(InstanceRecord))
            return false;
        first = unsigned(instances.size());
        instances.resize(instances.size() + count);
        return true;
    }

    //! Empty a run of instance slots and make it available again. Caller holds the mutex.
    void freeRun(unsigned first, unsigned count)
    {
        std::fill(instances.begin() + first, instances.begin() + first + count, InstanceRecord());
        markDirty(first, count);

        auto next = std::lower_bound(freeRuns.begin(), freeRuns.end(), std::make_pair(first, 0u));
        next = freeRuns.insert(next, std::make_pair(first, count));
        // Merge with the free run after it, then with the one before.
        if (next + 1 != freeRuns.end() && next->first + next->second == (next + 1)->first)
        {
            next->second += (next + 1)->second;
            freeRuns.erase(next + 1);
        }
        if (next != freeRuns.begin() && (next - 1)->first + (next - 1)->second == next->first)
        {
            (next - 1)->second += next->second;
            next = freeRuns.erase(next) - 1;
        }
        // Free slots at the end stop being slots, so that no pass visits them.
        if (next->first + next->second == instances.size())
        {
            instances.resize(next->first);
            freeRuns.erase(next);
        }
    }

    //! Add count instances to a batch, in one new run of slots, and grow the bounds. get(i)
    //! returns the i-th instance as a ClusterMeshNode::Instance. Changes nothing and returns
    //! false if any instance is unacceptable. Caller holds the mutex.
    template<typename GET>
    bool addInstances(Batch& batch, std::size_t count, GET&& get, unsigned& first)
    {
        if (count == 0u || count > std::size_t(std::numeric_limits<unsigned>::max()))
            return false;

        // A mirroring transform would turn the mesh inside out: its winding reverses, so
        // back-face culling removes the outside. Reject it along with degenerate input.
        for (std::size_t i = 0; i < count; ++i)
        {
            const ClusterMeshNode::Instance& instance = get(i);
            if (!instance.mesh || instance.mesh->clusters().empty())
                return false;
            const osg::Matrixf& m = instance.transform;
            const osg::Vec3f x(m(0, 0), m(0, 1), m(0, 2)), y(m(1, 0), m(1, 1), m(1, 2)), z(m(2, 0), m(2, 1), m(2, 2));
            const float determinant = (x ^ y) * z;
            if (!m.valid() || !std::isfinite(determinant) || determinant <= 0.0f)
                return false;
        }

        // Resolve the meshes first: a mesh that does not fit must not leave a run behind.
        std::unordered_map<const ClusterMesh*, int> meshIndex;
        for (std::size_t i = 0; i < count; ++i)
        {
            const ClusterMesh::Ptr& mesh = get(i).mesh;
            if (meshIndex.count(mesh.get()) == 0u)
            {
                const int index = findOrAddMesh(mesh);
                if (index < 0)
                    return false;
                meshIndex[mesh.get()] = index;
            }
        }
        if (!allocateRun(unsigned(count), first))
            return false;

        for (std::size_t i = 0; i < count; ++i)
        {
            const ClusterMeshNode::Instance& instance = get(i);
            const osg::Matrixf& m = instance.transform;
            InstanceRecord& record = instances[first + i];
            record.xform = m;
            record.mesh = GLuint(meshIndex[instance.mesh.get()]);
            record.batch = batch.slot;
            // Errors and radii are in mesh units; the largest axis keeps both conservative.
            record.scale = std::max(osg::Vec3f(m(0, 0), m(0, 1), m(0, 2)).length(), std::max(
                osg::Vec3f(m(1, 0), m(1, 1), m(1, 2)).length(),
                osg::Vec3f(m(2, 0), m(2, 1), m(2, 2)).length()));

            const auto& meshBox = instance.mesh->boundingBox();
            for (unsigned c = 0; c < 8; ++c)
                batch.box.expandBy(osg::Vec3d(meshBox.corner(c) * m) * batch.frame);
            batch.sourceTriangles += instance.mesh->numSourceTriangles();
            sourceTriangles += instance.mesh->numSourceTriangles();
        }
        liveInstances += count;
        box.expandBy(batch.box);
        markDirty(first, unsigned(count));
        return true;
    }

    //! Create a batch for the scene graph to stand for. Returns null if an instance is
    //! unacceptable. Any thread.
    std::shared_ptr<Batch> addBatch(const osg::Matrixd& frame, const std::vector<ClusterMeshNode::Instance>& list)
    {
        if (!frame.valid())
            return nullptr;

        std::lock_guard<std::mutex> lock(mutex);
        auto batch = std::make_shared<Batch>();
        batch->frame = frame;
        batch->tracked = true;
        batch->slot = freeBatchSlots.empty() ? unsigned(batches.size()) : freeBatchSlots.back();
        batch->count = unsigned(list.size());
        if (!addInstances(*batch, list.size(), [&](std::size_t i) -> const ClusterMeshNode::Instance& { return list[i]; },
            batch->first))
            return nullptr;

        if (freeBatchSlots.empty())
            batches.push_back(batch);
        else
        {
            batches[batch->slot] = batch;
            freeBatchSlots.pop_back();
        }
        graphDirty = true;
        return batch;
    }

    //! Remove a batch made by addBatch and free its slots. Does nothing for a batch that
    //! clear already removed. Any thread.
    void removeBatch(Batch& batch)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (batch.removed)
            return;
        batch.removed = true;
        freeRun(batch.first, batch.count);
        liveInstances -= batch.count;
        sourceTriangles -= batch.sourceTriangles;
        batches[batch.slot] = nullptr;
        freeBatchSlots.push_back(batch.slot);
        boxDirty = true;
        graphDirty = true;
    }

    //! The bounds of every instance, in node coordinates. Caller holds the mutex.
    const osg::BoundingBox& bounds()
    {
        if (boxDirty)
        {
            boxDirty = false;
            box.init();
            for (const auto& batch : batches)
                if (batch)
                    box.expandBy(batch->box);
        }
        return box;
    }

    //! Create a storage buffer and give it a debug label.
    static GLBuffer::Ptr createBuffer(osg::State& state, const char* label)
    {
        auto buffer = GLBuffer::create(GL_SHADER_STORAGE_BUFFER, state);
        // Binding materializes the name, which the named-buffer uploads below require.
        buffer->bind();
        buffer->debugLabel("ClusterMeshNode", label);
        return buffer;
    }

    //! Bring this state's buffers up to date with the CPU tables. Runs on the draw thread.
    void update(GLObjects& gl, osg::State& state)
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (!gl.vao)
        {
            gl.ext = state.get<osg::GLExtensions>();
            osg::setGLExtensionFuncPtr(gl.glMultiDrawElementsIndirectCount,
                "glMultiDrawElementsIndirectCount", "glMultiDrawElementsIndirectCountARB");
            osg::setGLExtensionFuncPtr(gl.glDispatchComputeIndirect,
                "glDispatchComputeIndirect", "glDispatchComputeIndirectARB");
            osg::setGLExtensionFuncPtr(gl.glVertexAttribFormat, "glVertexAttribFormat");
            osg::setGLExtensionFuncPtr(gl.glVertexArrayElementBuffer, "glVertexArrayElementBuffer");
            if (!gl.glMultiDrawElementsIndirectCount || !gl.glDispatchComputeIndirect ||
                !gl.glVertexAttribFormat || !gl.glVertexArrayElementBuffer || !gl.ext->glUniformHandleui64)
            {
                OE_WARN << LC << "OpenGL 4.6 with bindless textures is required; nothing will draw" << std::endl;
                gl.ext = nullptr;
                return;
            }

            // The geometry buffers are created as storage buffers and attached to the VAO
            // by name, which leaves OSG's cached array and element bindings untouched.
            gl.vertices = createBuffer(state, "vertices");
            gl.indices = createBuffer(state, "indices");
            gl.clusters = createBuffer(state, "clusters");
            gl.meshes = createBuffer(state, "meshes");
            gl.instances = createBuffer(state, "instances");
            gl.batches = createBuffer(state, "batches");
            gl.chunks = createBuffer(state, "chunks");
            gl.commands = createBuffer(state, "commands");
            gl.drawData = createBuffer(state, "draw data");
            gl.counters = createBuffer(state, "counters");
            gl.statsStaging = createBuffer(state, "stats staging");
            gl.hiddenInstances = createBuffer(state, "hidden instances");
            gl.hiddenClusters = createBuffer(state, "hidden clusters");
            gl.demandStaging = createBuffer(state, "demand staging");

            const Counters zero;
            gl.counters->bufferData(sizeof(Counters), &zero, GL_DYNAMIC_DRAW);
            gl.statsStaging->bufferData(sizeof(Counters), &zero, GL_DYNAMIC_READ);
            gl.demandStaging->bufferData(2 * sizeof(GLuint), &zero.drawDemand, GL_DYNAMIC_READ);

            gl.vao = GLVAO::create(state);
            state.bindVertexArrayObject(gl.vao->name());
            gl.vao->debugLabel("ClusterMeshNode", "VAO");
            struct Format { GLuint location; GLint size; GLenum type; GLboolean normalized; GLuint offset; };
            const Format formats[] = {
                { 0, 3, GL_FLOAT, GL_FALSE, GLuint(offsetof(ClusterMesh::Vertex, position)) },
                { 1, 4, GL_INT_2_10_10_10_REV, GL_TRUE, GLuint(offsetof(ClusterMesh::Vertex, normal)) },
                { 3, 4, GL_UNSIGNED_BYTE, GL_TRUE, GLuint(offsetof(ClusterMesh::Vertex, color)) },
                { 4, 2, GL_FLOAT, GL_FALSE, GLuint(offsetof(ClusterMesh::Vertex, uv)) }
            };
            for (const auto& f : formats)
            {
                gl.glVertexAttribFormat(f.location, f.size, f.type, f.normalized, f.offset);
                gl.ext->glVertexAttribBinding(f.location, 0);
                gl.ext->glEnableVertexAttribArray(f.location);
            }
            state.unbindVertexArrayObject();
        }
        if (!gl.ext)
            return;

        gl.threshold = threshold;
        gl.minInstancePixels = minInstancePixels;
        gl.collectStats = collectStats;
        gl.occlusion = occlusion;

        if (gl.meshRevision != meshRevision)
        {
            gl.meshRevision = meshRevision;
            gl.culled = false;
            if (!meshes.empty())
            {
                gl.vertices->bufferData(GLsizei(numVertices * sizeof(ClusterMesh::Vertex)), nullptr, GL_STATIC_DRAW);
                gl.indices->bufferData(GLsizei(numIndices * sizeof(GLuint)), nullptr, GL_STATIC_DRAW);
                for (std::size_t i = 0; i < meshes.size(); ++i)
                {
                    const auto& v = meshes[i]->vertices();
                    const auto& e = meshes[i]->indices();
                    gl.vertices->bufferSubData(GLintptr(meshFirstVertex[i]) * sizeof(ClusterMesh::Vertex),
                        GLsizei(v.size() * sizeof(ClusterMesh::Vertex)), v.data());
                    gl.indices->bufferSubData(GLintptr(meshFirstIndex[i]) * sizeof(GLuint),
                        GLsizei(e.size() * sizeof(GLuint)), e.data());
                }
                gl.clusters->uploadData(clusters, GL_STATIC_DRAW);
                gl.meshes->uploadData(meshRecords, GL_STATIC_DRAW);

                // Reallocation may have changed the stores, so attach them again.
                state.bindVertexArrayObject(gl.vao->name());
                gl.ext->glBindVertexBuffer(0, gl.vertices->name(), 0, sizeof(ClusterMesh::Vertex));
                gl.glVertexArrayElementBuffer(gl.vao->name(), gl.indices->name());
                state.unbindVertexArrayObject();
            }
        }

        if (gl.instanceRevision != instanceRevision)
        {
            // Upload the slots that changed since this state last looked, or all of them
            // when the buffer must grow or the record of changes no longer reaches back.
            const bool grow = instances.size() > gl.instanceCapacity;
            if (grow || gl.instanceRevision < dirtyFloor)
            {
                if (grow)
                {
                    const std::size_t limit = std::size_t(std::numeric_limits<GLsizei>::max()) / sizeof(InstanceRecord);
                    gl.instanceCapacity = unsigned(std::min(limit,
                        std::max<std::size_t>(instances.size() + instances.size() / 2u, 1024u)));
                    gl.instances->bufferData(GLsizei(gl.instanceCapacity * sizeof(InstanceRecord)), nullptr,
                        GL_DYNAMIC_DRAW);
                }
                if (!instances.empty())
                    gl.instances->bufferSubData(0, GLsizei(instances.size() * sizeof(InstanceRecord)), instances.data());
            }
            else
            {
                for (const auto& run : dirtyRuns)
                {
                    // A run may reach past the slots that are left after later removals.
                    const std::size_t end = std::min<std::size_t>(std::size_t(run.first) + run.count, instances.size());
                    if (run.revision > gl.instanceRevision && run.first < end)
                    {
                        gl.instances->bufferSubData(GLintptr(run.first) * sizeof(InstanceRecord),
                            GLsizei((end - run.first) * sizeof(InstanceRecord)), instances.data() + run.first);
                    }
                }
            }
            gl.instanceRevision = instanceRevision;
            gl.culled = false;
            gl.numInstances = unsigned(instances.size());
            gl.liveInstances = unsigned(liveInstances);
            gl.batchTable = batches;
        }

        // The early phase can judge every instance hidden, so that list holds them all.
        if (gl.hiddenInstanceCapacity < gl.numInstances)
        {
            gl.hiddenInstanceCapacity = gl.numInstances;
            gl.culled = false;
            gl.hiddenInstances->bufferData(GLsizei(gl.hiddenInstanceCapacity * sizeof(GLuint)), nullptr,
                GL_DYNAMIC_DRAW);
        }

        // The draw lists start at a modest size and grow, with headroom, when a cull has
        // wanted more than they hold. How many clusters a view needs depends on the window
        // size and on the content, so no fixed size is right; a list that is too small drops
        // clusters, and which ones changes from frame to frame.
        unsigned drawCapacity = gl.drawCapacity ? gl.drawCapacity : INITIAL_DRAW_CAPACITY;
        if (gl.drawDemand > drawCapacity)
            drawCapacity = std::max(gl.drawDemand + gl.drawDemand / 4u, drawCapacity + drawCapacity / 2u);
        drawCapacity = std::max(1024u, std::min(drawCapacity, maxDrawnClusters));

        // Every visible instance needs at least one chunk; the rest allows for many
        // more candidates than drawn clusters.
        const unsigned chunkCapacity = std::max(gl.numInstances + drawCapacity / 8u + 1024u,
            gl.chunkDemand + gl.chunkDemand / 4u);
        if (gl.drawCapacity != drawCapacity || gl.chunkCapacity < chunkCapacity)
        {
            gl.drawCapacity = drawCapacity;
            gl.chunkCapacity = chunkCapacity;
            gl.culled = false;
            gl.commands->bufferData(GLsizei(std::size_t(NUM_LISTS) * gl.drawCapacity *
                sizeof(DrawElementsIndirectCommand)), nullptr, GL_DYNAMIC_DRAW);
            gl.drawData->bufferData(GLsizei(std::size_t(NUM_LISTS) * gl.drawCapacity * sizeof(DrawRecord)), nullptr,
                GL_DYNAMIC_DRAW);
            gl.chunks->bufferData(GLsizei(gl.chunkCapacity * 4u * sizeof(GLuint)), nullptr, GL_DYNAMIC_DRAW);
            // Hidden clusters are (instance, cluster) pairs, one list's worth of them.
            gl.hiddenClusters->bufferData(GLsizei(gl.drawCapacity * 2u * sizeof(GLuint)), nullptr, GL_DYNAMIC_DRAW);
        }
    }

    //! Bind every buffer the cull program uses.
    static void bindCullBuffers(GLObjects& gl)
    {
        gl.drawData->bindBufferBase(DRAW_DATA);
        gl.batches->bindBufferBase(BATCHES);
        gl.hiddenClusters->bindBufferBase(HIDDEN_CLUSTERS);
        gl.hiddenInstances->bindBufferBase(HIDDEN_INSTANCES);
        gl.counters->bindBufferBase(COUNTERS);
        gl.commands->bindBufferBase(COMMANDS);
        gl.chunks->bindBufferBase(CHUNKS);
        gl.clusters->bindBufferBase(CLUSTERS);
        gl.meshes->bindBufferBase(MESHES);
        gl.instances->bindBufferBase(INSTANCES);
    }

    //! Send the camera of a cull key to the current cull program.
    static void setViewUniforms(GLObjects& gl, const CullKey& key)
    {
        const osg::Matrixd& p = key.projection;
        const bool orthographic = p(3, 3) != 0.0;

        // OSG matrices multiply row vectors, so clip = view * P and each clip coordinate
        // is the dot product with a column of P. Planes are kept inward-facing.
        auto column = [&](int c) { return osg::Vec4d(p(0, c), p(1, c), p(2, c), p(3, c)); };
        osg::Vec4d planes[5] = {
            column(3) + column(0), column(3) - column(0),
            column(3) + column(1), column(3) - column(1),
            // Depth conventions vary (reversed, logarithmic), so only reject what lies
            // behind the eye of a perspective camera and leave near clipping to the rasterizer.
            orthographic ? osg::Vec4d(0, 0, 0, 1) : osg::Vec4d(0, 0, -1, 0)
        };
        GLfloat planeData[20];
        for (int i = 0; i < 5; ++i)
        {
            const double length = osg::Vec3d(planes[i].x(), planes[i].y(), planes[i].z()).length();
            const osg::Vec4d plane = length > 0.0 ? planes[i] / length : planes[i];
            for (int c = 0; c < 4; ++c)
                planeData[i * 4 + c] = GLfloat(plane[c]);
        }

        // As for instances, the largest axis keeps radii and errors conservative.
        const osg::Matrixd& mv = key.modelView;
        const float viewScale = float(std::max(osg::Vec3d(mv(0, 0), mv(0, 1), mv(0, 2)).length(), std::max(
            osg::Vec3d(mv(1, 0), mv(1, 1), mv(1, 2)).length(),
            osg::Vec3d(mv(2, 0), mv(2, 1), mv(2, 2)).length())));

        auto* ext = gl.ext;
        ext->glUniform1f(MIN_INSTANCE_PIXELS, key.minInstancePixels);
        ext->glUniform4fv(PLANES, 5, planeData);
        ext->glUniform4f(LOD, float(p(1, 1) * 0.5 * key.height), key.threshold, MIN_LOD_DISTANCE,
            orthographic ? 1.0f : 0.0f);
        ext->glUniform4ui(SIZES, gl.numInstances, gl.chunkCapacity, gl.drawCapacity, 0u);
        ext->glUniform1f(VIEW_SCALE, viewScale);
    }

    //! Find which batches of this state's table draw in a frame, and return a hash of the
    //! answer. A batch that follows the scene graph draws when a cull traversal reached its
    //! node in that frame or the one before; a draw can trail the traversals by a frame.
    static std::uint64_t findActiveBatches(GLObjects& gl, unsigned frame)
    {
        std::uint64_t hash = 14695981039346656037ull;
        gl.batchActive.resize(gl.batchTable.size());
        for (std::size_t i = 0; i < gl.batchTable.size(); ++i)
        {
            const Batch* batch = gl.batchTable[i].get();
            const bool active = batch && (!batch->tracked || batch->lastTraversed.load() + 1 >= (long long)(frame));
            gl.batchActive[i] = active ? 1u : 0u;
            hash = (hash ^ (active ? i + 1u : 0u)) * 1099511628211ull;
        }
        return hash;
    }

    //! Upload each batch's transform to the view of the camera being drawn and to the view
    //! of the camera whose depth pyramid the early phase tests against. Both are composed in
    //! double precision. Hidden batches get zeros, which the cull pass skips.
    static void uploadBatches(GLObjects& gl, const osg::Matrixd& modelView, const osg::Matrixd& testModelView)
    {
        static const osg::Matrixf zero(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        gl.batchRecords.resize(gl.batchTable.size());
        for (std::size_t i = 0; i < gl.batchTable.size(); ++i)
        {
            BatchRecord& record = gl.batchRecords[i];
            if (gl.batchActive[i])
            {
                const osg::Matrixd& frame = gl.batchTable[i]->frame;
                record.modelView = osg::Matrixf(frame * modelView);
                record.testModelView = osg::Matrixf(frame * testModelView);
            }
            else
            {
                record.modelView = record.testModelView = zero;
            }
        }
        if (gl.batchCapacity < gl.batchRecords.size())
        {
            gl.batchCapacity = unsigned(gl.batchRecords.size() + gl.batchRecords.size() / 2u);
            gl.batches->bufferData(GLsizei(gl.batchCapacity * sizeof(BatchRecord)), nullptr, GL_DYNAMIC_DRAW);
        }
        gl.batches->bufferSubData(0, GLsizei(gl.batchRecords.size() * sizeof(BatchRecord)), gl.batchRecords.data());
    }

    //! Tell the current cull program which occlusion phase to run, and against which camera's
    //! pyramid. A null pyramid makes every occlusion test answer "visible".
    static void setOcclusionUniforms(GLObjects& gl, Occlusion mode, const osg::Matrixd& projection,
        const DepthPyramid::Result* pyramid)
    {
        auto* ext = gl.ext;
        ext->glUniform1i(OCCLUSION, int(mode));
        const osg::Matrixf p(projection);
        ext->glUniformMatrix4fv(TEST_PROJECTION, 1, GL_FALSE, p.ptr());
        const osg::Vec4f params = pyramid ? pyramid->params : osg::Vec4f();
        const osg::Vec2f mapping = pyramid ? pyramid->depthMapping : osg::Vec2f();
        ext->glUniform4fv(HIZ_PARAMS, 1, params.ptr());
        ext->glUniform2fv(HIZ_DEPTH_MAPPING, 1, mapping.ptr());
        // A sampler uniform must hold a valid handle or keep its initial zero.
        if (pyramid)
            ext->glUniformHandleui64(HIZ, pyramid->handle);
    }

    //! Run the early phase for the current camera unless the last cull on this state used
    //! identical inputs. It fills the early draw lists and, with occlusion culling, records
    //! what it predicts to be hidden for cullLate. Leaves the graphics program current.
    //! Returns false when nothing can be drawn.
    bool cull(GLObjects& gl, osg::RenderInfo& ri)
    {
        osg::State& state = *ri.getState();
        const osg::Viewport* viewport = state.getCurrentViewport();
        if (!viewport)
            return false;

        CullKey key;
        key.modelView = state.getModelViewMatrix();
        key.projection = state.getProjectionMatrix();
        key.width = int(viewport->width());
        key.height = int(viewport->height());
        key.threshold = gl.threshold;
        key.minInstancePixels = gl.minInstancePixels;
        key.meshRevision = gl.meshRevision;
        key.instanceRevision = gl.instanceRevision;

        // Occlusion needs a frame number to tell this frame's phases from last frame's.
        const osg::Camera* camera = ri.getCurrentCamera();
        const bool occlusion = gl.occlusion && state.getFrameStamp() != nullptr && key.projection(3, 3) == 0.0 &&
            isColorCamera(camera);
        const unsigned frame = state.getFrameStamp() ? state.getFrameStamp()->getFrameNumber() : 0u;
        key.camera = camera;
        key.occlusion = occlusion;
        key.activity = findActiveBatches(gl, frame);

        // The second pass of a frame always reuses the first one's cull. Later frames reuse
        // it for an unchanged view only without occlusion culling: with it, whatever else
        // is in the depth buffer decides the outcome too, and that can change under a
        // camera that stands still (terrain paging in, other objects moving).
        if (gl.culled && key == gl.key && (gl.cullFrame == frame || !occlusion))
            return true;

        // Predict from the previous frame only when this camera left a pyramid behind and
        // DepthPyramid has not discarded any since, which would leave the handle dangling.
        const View* previous = nullptr;
        if (occlusion)
        {
            const View& view = gl.view(camera, frame);
            if (view.valid && view.generation == DepthPyramid::generation())
                previous = &view;
        }

        auto* graphics = state.getLastAppliedProgramObject();
        auto* compute = link(getCullProgram(), state);
        if (!graphics || !compute)
            return false;

        OE_GL_ZONE_NAMED("ClusterMesh cull");

        auto* ext = gl.ext;
        ext->glUseProgram(compute->getHandle());

        // The demand fields at the end keep their values.
        const Counters reset;
        gl.counters->bufferSubData(0, offsetof(Counters, drawDemand), &reset);
        uploadBatches(gl, key.modelView, previous ? previous->modelView : key.modelView);
        bindCullBuffers(gl);
        setViewUniforms(gl, key);
        if (previous)
            setOcclusionUniforms(gl, OCCLUSION_EARLY, previous->projection, &previous->pyramid);
        else
            setOcclusionUniforms(gl, OCCLUSION_OFF, key.projection, nullptr);

        // The buffer update above must be visible to the first pass, and each pass's
        // writes to the next reader: the indirect dispatch, then the indirect draws.
        ext->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        ext->glUniform1i(PASS, PASS_INSTANCES);
        const unsigned groups = (gl.numInstances + LOCAL_SIZE - 1u) / LOCAL_SIZE;
        ext->glDispatchCompute(std::min(groups, DISPATCH_WIDTH), (groups + DISPATCH_WIDTH - 1u) / DISPATCH_WIDTH, 1);

        ext->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
        ext->glUniform1i(PASS, PASS_CLUSTERS);
        gl.counters->bind(GL_DISPATCH_INDIRECT_BUFFER);
        gl.glDispatchComputeIndirect(GLintptr(offsetof(Counters, dispatchX)));

        ext->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        ext->glUseProgram(graphics->getHandle());

        gl.key = key;
        gl.culled = true;
        gl.cullFrame = frame;
        gl.lateNeeded = occlusion;
        gl.lateDone = false;

        // Queue a copy of the counters for readStats; see there for why it is not read here.
        // With a late phase to come, that phase queues the copy instead.
        if (gl.collectStats && !occlusion && isMainCamera(ri))
            gl.counters->copyBufferSubData(gl.statsStaging, 0, 0, sizeof(Counters));
        return true;
    }

    //! Run the late phase of occlusion culling for the camera whose early phase is in the
    //! buffers: build this frame's depth pyramid from what has been drawn, retest what the
    //! early phase recorded as hidden, and fill the late draw lists with what is visible
    //! after all. Leaves the graphics program current. Returns false when there is nothing
    //! to draw late.
    bool cullLate(GLObjects& gl, osg::RenderInfo& ri)
    {
        // Normally an early list has drawn first and this returns at once. The early phase
        // runs now if a late list came first, because an override above the node flattened
        // the render bins, or if the buffers hold another path's cull, because the node has
        // several parents. Nothing is lost either way; the cost is a second cull, and in
        // the first case a pyramid that misses this node's own depth.
        if (!cull(gl, ri) || !gl.lateNeeded)
            return false;
        if (gl.lateDone)
            return true;

        osg::State& state = *ri.getState();

        auto* graphics = state.getLastAppliedProgramObject();
        auto* compute = link(getCullProgram(), state);
        auto* pyramid = link(getPyramidProgram(), state);
        if (!graphics || !compute || !pyramid)
            return false;

        OE_GL_ZONE_NAMED("ClusterMesh late cull");

        const osg::Camera* camera = ri.getCurrentCamera();
        const unsigned frame = state.getFrameStamp() ? state.getFrameStamp()->getFrameNumber() : 0u;
        auto* ext = gl.ext;

        // The depth buffer now holds whatever drew before this node plus the early lists.
        // The pyramid also serves next frame's early phase, together with this camera.
        View& view = gl.view(camera, frame);
        ext->glUseProgram(pyramid->getHandle());
        view.valid = DepthPyramid::build(state, pyramid, camera, this, frame, view.pyramid);
        view.generation = DepthPyramid::generation();
        view.modelView = gl.key.modelView;
        view.projection = gl.key.projection;

        // Without a pyramid nothing can be proven hidden, and everything recorded draws.
        ext->glUseProgram(compute->getHandle());
        bindCullBuffers(gl);
        setViewUniforms(gl, gl.key);
        setOcclusionUniforms(gl, OCCLUSION_LATE, gl.key.projection, view.valid ? &view.pyramid : nullptr);

        // Empty the chunk queue, which the early cluster pass has consumed.
        const GLuint emptyQueue[4] = { 0u, DISPATCH_WIDTH, 0u, 1u };
        gl.counters->bufferSubData(0, sizeof(emptyQueue), emptyQueue);

        ext->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
        gl.counters->bind(GL_DISPATCH_INDIRECT_BUFFER);
        ext->glUniform1i(PASS, PASS_LATE_INSTANCES);
        gl.glDispatchComputeIndirect(GLintptr(offsetof(Counters, hiddenInstanceX)));
        ext->glUniform1i(PASS, PASS_LATE_CLUSTERS);
        gl.glDispatchComputeIndirect(GLintptr(offsetof(Counters, hiddenClusterX)));

        // The instances that turned out visible queued their clusters as chunks.
        ext->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
        ext->glUniform1i(PASS, PASS_CLUSTERS);
        gl.glDispatchComputeIndirect(GLintptr(offsetof(Counters, dispatchX)));

        ext->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        ext->glUseProgram(graphics->getHandle());
        gl.lateDone = true;

        if (gl.collectStats && isMainCamera(ri))
            gl.counters->copyBufferSubData(gl.statsStaging, 0, 0, sizeof(Counters));
        return true;
    }

    //! Whether the camera being drawn is the one whose counters the node reports.
    static bool isMainCamera(osg::RenderInfo& ri)
    {
        const osg::Camera* camera = ri.getCurrentCamera();
        return camera && !camera->isRenderToTextureCamera();
    }

    //! Publish the counters of the main camera's previous cull. Reading the live counter
    //! buffer would stall on the GPU and, on NVIDIA, move the buffer to system memory, so
    //! this reads the copy that a cull queued. It must run before this frame's cull queues
    //! the next copy, or the read would wait for that cull to finish.
    void readStats(GLObjects& gl)
    {
        Counters last;
        gl.statsStaging->getBufferSubData(0, sizeof(Counters), &last);

        std::lock_guard<std::mutex> lock(mutex);
        stats.instances = gl.liveInstances;
        stats.candidates = last.candidateCount;
        stats.triangles = last.triangleCount;
        stats.sourceTriangles = sourceTriangles;
        stats.occludedInstances = last.occludedInstanceCount;
        stats.occludedClusters = last.occludedClusterCount;
        stats.clusters = 0u;
        stats.overflowed = last.chunkCount > gl.chunkCapacity;
        for (unsigned count : last.drawCount)
        {
            stats.clusters += std::min(count, gl.drawCapacity);
            stats.overflowed = stats.overflowed || count > gl.drawCapacity;
        }
    }

    //! Once per frame, learn whether any camera's cull wanted more room in the draw lists
    //! or the chunk queue than they have. update then enlarges them. Like readStats, this
    //! reads a copy that an earlier frame queued, so it does not wait for the GPU; an
    //! overflow is therefore noticed two frames after it happens.
    void readDemand(GLObjects& gl, unsigned frame)
    {
        if (gl.demandFrame == frame)
            return;
        gl.demandFrame = frame;

        GLuint demand[2] = { 0u, 0u };
        gl.demandStaging->getBufferSubData(0, sizeof(demand), demand);
        if (demand[0] > gl.drawDemand || demand[1] > gl.chunkDemand)
        {
            gl.drawDemand = std::max(gl.drawDemand, unsigned(demand[0]));
            gl.chunkDemand = std::max(gl.chunkDemand, unsigned(demand[1]));
            // Start counting again, or the same overflow would be reported next frame too.
            const GLuint zero[2] = { 0u, 0u };
            gl.counters->bufferSubData(GLintptr(offsetof(Counters, drawDemand)), sizeof(zero), zero);
        }
        gl.counters->copyBufferSubData(gl.demandStaging, GLintptr(offsetof(Counters, drawDemand)), 0, sizeof(demand));
    }

    //! Draw one list with the program OSG has applied, running its cull phase first when
    //! that has not happened for this camera.
    void draw(osg::RenderInfo& ri, int list)
    {
        osg::State& state = *ri.getState();
        auto& gl = GLObjects::get(globjects, state);
        const unsigned frame = state.getFrameStamp() ? state.getFrameStamp()->getFrameNumber() : 0u;
        // Before update, so that lists that were too small grow ahead of this frame's culls.
        if (gl.ext)
            readDemand(gl, frame);
        update(gl, state);
        if (!gl.ext || gl.numInstances == 0u)
            return;

        // Whichever pass draws first in a frame reads the counters, before it culls.
        if (gl.collectStats && isMainCamera(ri) && gl.statsFrame != frame)
        {
            gl.statsFrame = frame;
            readStats(gl);
        }

        const bool late = list == LATE_OPAQUE || list == LATE_CUTOUT;
        if (!(late ? cullLate(gl, ri) : cull(gl, ri)))
            return;

        OE_GL_ZONE_NAMED("ClusterMesh draw");

        state.applyModelViewAndProjectionUniformsIfRequired();
        state.bindVertexArrayObject(gl.vao->name());

        // Other drawables share these binding points, so bind for every draw.
        gl.drawData->bindBufferBase(DRAW_DATA);
        gl.batches->bindBufferBase(BATCHES);
        gl.instances->bindBufferBase(INSTANCES);
        gl.commands->bind(GL_DRAW_INDIRECT_BUFFER);
        gl.counters->bind(GL_PARAMETER_BUFFER);

        const std::size_t first = std::size_t(list) * gl.drawCapacity * sizeof(DrawElementsIndirectCommand);
        const GLintptr count = GLintptr(offsetof(Counters, drawCount) + list * sizeof(GLuint));
        gl.glMultiDrawElementsIndirectCount(GL_TRIANGLES, GL_UNSIGNED_INT, reinterpret_cast<const GLvoid*>(first),
            count, GLsizei(gl.drawCapacity), sizeof(DrawElementsIndirectCommand));

        gl.ext->glBindBuffer(GL_PARAMETER_BUFFER, 0);
        // Keep OSG's VAO cache in sync so conventional geometry can follow.
        state.unbindVertexArrayObject();
    }
};

/**
 * One of the node's draws. Each list is a separate drawable so that it can carry its own
 * state through OSG's ordinary state sorting, for every camera that renders the node: the
 * alpha-tested lists use a shader with discard and no face culling, and the late lists sit
 * in a nested render bin so that they draw after the early ones.
 */
class ClusterMeshNode::Pass : public osg::Drawable
{
public:
    Pass(std::shared_ptr<Impl> impl, int list) : _impl(impl), _list(list)
    {
        const char* names[NUM_LISTS] = { "ClusterMesh opaque", "ClusterMesh cutout",
            "ClusterMesh late opaque", "ClusterMesh late cutout" };
        setName(names[list]);
        setUseDisplayList(false);
        setSupportsDisplayList(false);
        setUseVertexArrayObject(false);
        // Instances can change between frames; OSG must not start the next update while
        // a draw thread is still reading the tables.
        setDataVariance(DYNAMIC);

        auto* ss = getOrCreateStateSet();
        if (list == EARLY_CUTOUT || list == LATE_CUTOUT)
        {
            ss->setDefine("OE_CLUSTERMESH_CUTOUT");
            ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
        }
        else
        {
            ss->setAttributeAndModes(new osg::CullFace(osg::CullFace::BACK));
        }
        // A nested bin draws after its parent bin's own drawables, which include the early
        // lists. The late phase needs their depth.
        if (list == LATE_OPAQUE || list == LATE_CUTOUT)
        {
            ss->setRenderBinDetails(1, "RenderBin", osg::StateSet::RenderBinMode(
                osg::StateSet::USE_RENDERBIN_DETAILS | osg::StateSet::PROTECTED_RENDERBIN_DETAILS));
        }
    }

    osg::BoundingBox computeBoundingBox() const override
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        return _impl->bounds();
    }

    osg::BoundingSphere computeBound() const override
    {
        // Node::getBound and Drawable::getBoundingBox share one computed flag.
        return osg::BoundingSphere(getBoundingBox());
    }

    void drawImplementation(osg::RenderInfo& ri) const override
    {
        _impl->draw(ri, _list);
    }

    void resizeGLObjectBuffers(unsigned size) override
    {
        osg::Drawable::resizeGLObjectBuffers(size);
        if (size > _impl->globjects.size())
            _impl->globjects.resize(size);
    }

    void releaseGLObjects(osg::State* state) const override
    {
        osg::Drawable::releaseGLObjects(state);
        if (state)
            Impl::GLObjects::get(_impl->globjects, *state).release();
        else
            _impl->globjects.setAllElementsTo(Impl::GLObjects());

        // The compute programs live outside the scene graph, so nothing else releases them.
        // A stale program object would survive into a new context that reuses this ID.
        getCullProgram()->releaseGLObjects(state);
        getPyramidProgram()->releaseGLObjects(state);
    }

private:
    std::shared_ptr<Impl> _impl;
    int _list;
};

/**
 * Stands in the scene graph for a batch of the node's instances. Its bound is theirs, so
 * ordinary culling of this node decides whether they draw; destroying it removes them.
 */
class ClusterMeshNode::BatchNode : public osg::Node
{
public:
    BatchNode(std::shared_ptr<Impl> impl, std::shared_ptr<Batch> batch) : _impl(impl), _batch(batch)
    {
        setName("ClusterMesh batch");
    }

    //! Marks the batch as visible in the frame of every cull traversal that gets here.
    void traverse(osg::NodeVisitor& nv) override
    {
        if (nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
        {
            // Without a frame number the batch just stays visible.
            _batch->lastTraversed = nv.getFrameStamp() ? (long long)(nv.getFrameStamp()->getFrameNumber()) :
                std::numeric_limits<long long>::max() / 2;
        }
    }

    osg::BoundingSphere computeBound() const override
    {
        return osg::BoundingSphere(_batch->box);
    }

protected:
    //! The ClusterMeshNode may be gone already, and its tables with it.
    virtual ~BatchNode()
    {
        if (auto impl = _impl.lock())
            impl->removeBatch(*_batch);
    }

private:
    std::weak_ptr<Impl> _impl;
    std::shared_ptr<Batch> _batch;
};

ClusterMeshNode::ClusterMeshNode() :
    _impl(std::make_shared<Impl>())
{
    setName("ClusterMeshNode");
    // traverse applies changes that createBatch and batch removal leave for the update thread.
    setNumChildrenRequiringUpdateTraversal(1u);

    auto* ss = getOrCreateStateSet();
    auto vp = Registry::instance()->getOrCreate<VirtualProgram>("vp.ClusterMesh", []()
        {
            auto vp = new VirtualProgram();
            vp->setInheritShaders(true);
            vp->setName("ClusterMesh");
            Shaders pkg;
            pkg.load(vp, pkg.ClusterMesh);
            return vp;
        });
    ss->setAttribute(vp);
    // Every fragment is opaque; alpha-tested ones are discarded instead of blended.
    ss->setMode(GL_BLEND, osg::StateAttribute::OFF);
    _debugUniform = new osg::Uniform("oe_clustermesh_debug", int(DEBUG_NONE));
    _debugUniform->setDataVariance(osg::Object::DYNAMIC);
    ss->addUniform(_debugUniform);

    contentChanged();
}

ClusterMeshNode::~ClusterMeshNode()
{
}

bool
ClusterMeshNode::add(ClusterMesh::Ptr mesh, const osg::Matrixf& transform)
{
    return add(mesh, std::vector<osg::Matrixf>(1u, transform));
}

bool
ClusterMeshNode::add(ClusterMesh::Ptr mesh, const std::vector<osg::Matrixf>& transforms)
{
    bool ok;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        // The node's own batch: identity frame, always drawn.
        Instance instance;
        instance.mesh = mesh;
        unsigned first = 0u;
        ok = _impl->addInstances(*_impl->batches[0], transforms.size(), [&](std::size_t i) -> const Instance&
            {
                instance.transform = transforms[i];
                return instance;
            }, first);
    }
    if (ok) contentChanged();
    return ok;
}

osg::ref_ptr<osg::Node>
ClusterMeshNode::createBatch(const osg::Matrixd& frame, const std::vector<Instance>& instances)
{
    auto batch = _impl->addBatch(frame, instances);
    if (!batch)
        return nullptr;
    return new BatchNode(_impl, batch);
}

void
ClusterMeshNode::clear()
{
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->meshes.clear();
        _impl->meshRecords.clear();
        _impl->meshFirstVertex.clear();
        _impl->meshFirstIndex.clear();
        _impl->clusters.clear();
        _impl->instances.clear();
        _impl->freeRuns.clear();
        _impl->liveInstances = 0u;
        // The nodes of the removed batches live on; their destructors must find nothing to do.
        for (auto& batch : _impl->batches)
            if (batch)
                batch->removed = true;
        _impl->batches.assign(1u, std::make_shared<Batch>());
        _impl->freeBatchSlots.clear();
        _impl->numVertices = _impl->numIndices = 0u;
        _impl->sourceTriangles = 0u;
        _impl->hasAlphaTest = false;
        _impl->box.init();
        _impl->boxDirty = false;
        ++_impl->meshRevision;
        // Every GL state starts over from the empty table.
        _impl->dirtyRuns.clear();
        _impl->dirtyFloor = ++_impl->instanceRevision;
    }
    contentChanged();
}

void
ClusterMeshNode::traverse(osg::NodeVisitor& nv)
{
    if (nv.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR && _impl->graphDirty.exchange(false))
        contentChanged();
    osg::Group::traverse(nv);
}

void
ClusterMeshNode::contentChanged()
{
    bool hasAlphaTest;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        hasAlphaTest = _impl->hasAlphaTest;
    }

    // The alpha-tested passes exist only while some mesh needs them, so opaque-only content
    // never pays for their state changes and draws.
    const unsigned wanted = hasAlphaTest ? 4u : 2u;
    if (getNumChildren() != wanted)
    {
        removeChildren(0u, getNumChildren());
        for (int list : { EARLY_OPAQUE, LATE_OPAQUE, EARLY_CUTOUT, LATE_CUTOUT })
            if (hasAlphaTest || list == EARLY_OPAQUE || list == LATE_OPAQUE)
                addChild(new Pass(_impl, list));
    }

    for (unsigned i = 0; i < getNumChildren(); ++i)
        getChild(i)->dirtyBound();
}

std::size_t
ClusterMeshNode::getNumInstances() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->liveInstances;
}

void
ClusterMeshNode::setMinInstancePixels(float pixels)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->minInstancePixels = std::max(pixels, 0.0f);
}

float
ClusterMeshNode::getMinInstancePixels() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->minInstancePixels;
}

void
ClusterMeshNode::setErrorThreshold(float pixels)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->threshold = std::max(pixels, 0.0f);
}

float
ClusterMeshNode::getErrorThreshold() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->threshold;
}

void
ClusterMeshNode::setMaxDrawnClusters(unsigned value)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    // Four lists of 20-byte commands must fit one buffer.
    _impl->maxDrawnClusters = std::max(1024u, std::min(value, 1u << 24));
}

unsigned
ClusterMeshNode::getMaxDrawnClusters() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->maxDrawnClusters;
}

void
ClusterMeshNode::setOcclusionCulling(bool value)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->occlusion = value;
}

bool
ClusterMeshNode::getOcclusionCulling() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->occlusion;
}

void
ClusterMeshNode::setDebug(Debug value)
{
    _debugUniform->set(int(value));
}

ClusterMeshNode::Debug
ClusterMeshNode::getDebug() const
{
    int value = 0;
    _debugUniform->get(value);
    return Debug(value);
}

void
ClusterMeshNode::setCollectStats(bool value)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->collectStats = value;
    if (!value)
        _impl->stats = Stats();
}

ClusterMeshNode::Stats
ClusterMeshNode::getStats() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->stats;
}
