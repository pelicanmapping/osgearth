/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "PagedNode"
#include "GLUtils"
#include "NodeUtils"
#include "Progress"
#include "MapNode"
#include <osgUtil/IncrementalCompileOperation>

#define LC "[PagedNode] "

using namespace osgEarth;
using namespace osgEarth::Util;

#define DEFAULT_JOBPOOL_NAME "oe.nodepager"

// If we do this, a low LOD scale caused by magnification will prevent anything
// from even paging out...
//#define KEEP_NODES_THAT_ARE_CULLED_BUT_IN_RANGE

namespace
{
    struct VisibilityCallback : public osg::NodeCallback {
    };

    // Only the update thread can mutate a page. A scoped batch preserves virtual unload dispatch
    // without submitting one cleanup job per page; nested manager updates restore the outer scope.
    struct RetirementBatch;
    thread_local RetirementBatch* s_retirementBatch = nullptr;
    struct RetirementBatch
    {
        PagingManager* manager;
        ObjectReleaser::Objects& objects;
        RetirementBatch* previous;

        //! Collects this manager's base-class unload releases on the current update thread.
        RetirementBatch(PagingManager* owner, ObjectReleaser::Objects& retired) :
            manager(owner), objects(retired), previous(s_retirementBatch)
        {
            s_retirementBatch = this;
        }

        //! Restores the previous scope, including when a subclass unload throws.
        ~RetirementBatch() { s_retirementBatch = previous; }
    };
}

PagedNode2::PagedNode2()
{
    _job.name = (typeid(*this).name());
    _payload = new osg::Group(); // parent of _loaded
}

PagedNode2::~PagedNode2()
{
    //nop
}

void
PagedNode2::setOwner(PagedNodeManager* owner)
{
    _owner = owner;

    if (_owner->visibilityCallback())
    {
        OE_SOFT_ASSERT_AND_RETURN(_payload.valid(), void());
        _payload->setCullCallback(_owner->visibilityCallback());
    }        
}

void
PagedNode2::setLoadFunction(const Loader& value)
{
    std::lock_guard<Mutex> lock(_mutex);
    _load_function = value;
}

void
PagedNode2::traverse(osg::NodeVisitor& nv)
{
    osg::ref_ptr<PagingManager> pagingManager;
    {
        std::lock_guard<Mutex> lock(_mutex);
        if (!_pagingManager_weak.lock(pagingManager))
        {
            if (ObjectStorage::get(&nv, pagingManager))
            {
                // Tokens belong to one tracker and cannot survive replacement of its manager.
                _token = nullptr;
                _pagingManager_weak = pagingManager.get();
            }
        }
    }

    if (nv.getTraversalMode() == nv.TRAVERSE_ACTIVE_CHILDREN)
    {
        if (nv.getVisitorType() == nv.CULL_VISITOR)
        {
            bool inRange = false;

            _lastTime = nv.getFrameStamp() ? nv.getFrameStamp()->getReferenceTime() : 0.0;

            if (_lodMethod == LODMethod::CAMERA_DISTANCE)
            {
                float range = std::max(0.0f, nv.getDistanceToViewPoint(getBound().center(), true) - getBound().radius());
                inRange = (range >= _minRange && range <= _maxRange);
                _priority = -range * _priorityScale;
            }
            else // if (_lodMethod == LODMethod::SCREEN_SPACE)
            {
                osg::CullStack* cullStack = nv.asCullStack();
                if (cullStack != nullptr && cullStack->getLODScale() > 0.0f)
                {
                    float pixelError = pagingManager.valid() ? pagingManager->sse() : 0.0f;
                    nv.getUserValue("oe_sse", pixelError);
                    float pixels = Util::getPixelSize(cullStack, getBound().center(), getBound().radius()) / cullStack->getLODScale();
                    inRange = (pixels >= _minPixels + pixelError) && (pixels <= _maxPixels + pixelError);
                    _priority = pixels * _priorityScale;
                }
            }

            if (_refinementFunction) inRange = _refinementFunction(nv, inRange);

            if (_replacementFunction && _refinePolicy == REFINE_REPLACE)
            {
                // startLoad owns the load gate and snapshots state under the node mutex.
                if (inRange && !_loadGate.load()) startLoad(&nv);
                const bool retain = _replacementFunction(nv, isMerged() ? _payload.get() : nullptr, inRange);
                if (inRange || retain) touch();
                return;
            }

            if (inRange)
            {
                if (!_loadGate.load())
                {
                    startLoad(&nv);
                }

                // traverse children
                traverseChildren(nv);

                // stay alive
                touch();
            }
            else
            {
                // child out of range; just accept static children
                auto paged_child = isMerged() ? _payload.get() : nullptr;

                for (auto& child : _children)
                {
                    if (child.get() != paged_child)
                    {
                        child->accept(nv);
                    }
                }
            }
        }
        else
        {
            // Only traverse the highest res children otherwise
            traverseChildren(nv);
        }
    }

    else if (nv.getTraversalMode() == nv.TRAVERSE_ALL_CHILDREN)
    {
        for (auto& child : _children)
        {
            if (child.valid())
                child->accept(nv);
        }
    }
}

void
PagedNode2::traverseChildren(osg::NodeVisitor& nv)
{
    if (_refinePolicy == REFINE_REPLACE && isMerged())
    {
        _payload->accept(nv);
    }
    else
    {
        for (auto& child : _children)
        {
            child->accept(nv);
        }
    }
}

void
PagedNode2::touch()
{
    // tell the paging manager this node is still alive
    // (and should not be removed from the scene graph)
    osg::ref_ptr<PagingManager> pagingManager;
    {
        std::lock_guard<Mutex> lock(_mutex);
        _pagingManager_weak.lock(pagingManager);
    }
    if (pagingManager.valid())
    {
        // Use the same lock order as expiration: tracker first, then node state.
        scoped_lock_if trackerLock(pagingManager->_trackerMutex, pagingManager->_threadsafe);
        std::lock_guard<Mutex> lock(_mutex);
        _token = pagingManager->_tracker.use(this, _token);
    }
}

bool
PagedNode2::isMerged() const
{
    std::lock_guard<Mutex> lock(_mutex);
    return _merged.has_value(true);
}

bool
PagedNode2::merge(int revision, jobs::promise<bool> promise, double referenceTime)
{
    osg::ref_ptr<osg::Node> node;
    osg::ref_ptr<SceneGraphCallbacks> callbacks;
    {
        std::lock_guard<Mutex> lock(_mutex);
        if (_revision == revision && _loaded.available())
        {
            node = _loaded.value();
            callbacks = _callbacks;
        }
    }

    // All scene graph access happens in the update traversal. A stale completion must
    // resolve its original promise, never the current load's _merged future.
    if (!node.valid() || node->getNumParents() != 0 || !_payload.valid() || _payload->getNumParents() != 0)
    {
        promise.resolve(false);
        return false;
    }

    _payload->removeChildren(0, _payload->getNumChildren());
    _payload->addChild(node);
    this->addChild(_payload);

    // Publish transition timing with this payload before notifying callbacks, which may unload the node.
    _mergeTime = referenceTime;
    promise.resolve(true);
    if (callbacks.valid())
        callbacks->firePostMergeNode(node.get());

    return true;
}

osg::BoundingSphere
PagedNode2::computeBound() const
{
    if (_userBS.isSet() && _userBS->radius() >= 0.0f)
    {
        return _userBS.get();
    }

    else
    {
        osg::BoundingSphere bs = osg::Group::computeBound();

        osg::ref_ptr<osg::Node> loaded;
        {
            std::lock_guard<Mutex> lock(_mutex);
            if (!_merged.available() && _loaded.available())
                loaded = _loaded.value();
        }
        if (loaded.valid())
            bs.expandBy(loaded->computeBound());

        return bs;
    }
}

void
PagedNode2::startLoad(const osg::Object* host)
{
    std::unique_lock<Mutex> stateLock(_mutex);
    if (!_load_function || _loadGate.load() || !_loaded.empty())
        return;

    auto pnode_weak = osg::observer_ptr<PagedNode2>(this);
    osg::ref_ptr<PagingManager> pagingManager;
    auto loaded = _loaded;
    auto loader = _load_function;
    auto callbacks = _callbacks;
    const bool preCompile = _preCompile;
    const int load_revision = _revision;
    _loadGate = true;
    _pagingManager_weak.lock(pagingManager);
    auto poolName = pagingManager.valid() ? pagingManager->_jobpoolName : _jobpoolName;

    // The continuation only validates identity and enqueues; it never touches live graph bounds.
    auto merge_job = [pnode_weak, load_revision](const osg::ref_ptr<osg::Node>& node, auto& promise)
    {
        osg::ref_ptr<PagedNode2> pnode;
        osg::ref_ptr<PagingManager> manager;
        if (node.valid() && pnode_weak.lock(pnode))
        {
            // Manual loads can start before traversal discovers the manager.
            std::lock_guard<Mutex> lock(pnode->_mutex);
            if (pnode->_revision == load_revision)
                pnode->_pagingManager_weak.lock(manager);
        }
        if (manager.valid())
            manager->merge(pnode, load_revision, promise);
        else
            promise.resolve(false);
    };

    // Publish both handles before the worker can resolve the load, including immediate completion.
    _merged = loaded.then_dispatch<bool>(merge_job);
    stateLock.unlock();

    if (poolName.empty())
        poolName = DEFAULT_JOBPOOL_NAME;

    // Read visitor user data only on its traversal thread, and observe the ICO while it is alive.
    osg::ref_ptr<osgUtil::IncrementalCompileOperation> ico;
    if (preCompile)
        ObjectStorage::get(host, ico);
    auto ico_weak = osg::observer_ptr<osgUtil::IncrementalCompileOperation>(ico);

    jobs::context context;
    context.pool = jobs::get_pool(poolName);
    context.priority = [pnode_weak]() {
        osg::ref_ptr<PagedNode2> pnode;
        return pnode_weak.lock(pnode) ? pnode->getPriority() : -FLT_MAX;
    };

    // Load and compile only detached content; the visitor and mutable node configuration are not captured.
    auto load_and_compile_job = [pnode_weak, loader, callbacks, preCompile, ico_weak, load_revision](auto& promise)
    {
        osg::ref_ptr<osg::Node> result;
        osg::ref_ptr<ProgressCallback> progress = new ProgressCallback(&promise);

        osg::ref_ptr<PagedNode2> pnode;
        if (pnode_weak.lock(pnode) && pnode->_revision == load_revision)
        {
            result = loader(progress.get());

            if (result.valid())
            {
                if (callbacks.valid())
                    callbacks->firePreMergeNode(result.get());

                if (preCompile && result->getBound().valid())
                {
                    GLObjectsCompiler compiler;
                    auto state = compiler.collectState(result.get());
                    compiler.requestIncrementalCompileWithICO(result, state.get(), ico_weak, promise);
                    return;
                }
            }
        }

        promise.resolve(result);
    };

    (void)jobs::dispatch(load_and_compile_job, loaded, context);
}

void
PagedNode2::load()
{
    startLoad(nullptr);
}

void PagedNode2::unload()
{
    osg::ref_ptr<PagingManager> manager;
    {
        std::lock_guard<Mutex> lock(_mutex);
        _pagingManager_weak.lock(manager);
    }
    if (s_retirementBatch && s_retirementBatch->manager == manager.get())
    {
        unload(s_retirementBatch->objects);
        return;
    }
    auto releaser = manager.valid() ? manager->_releaser :
        Registry::instance()->getObjectReleaser()->createClient("PagedNode2 explicit unload");
    ObjectReleaser::Objects retired;
    unload(retired);
    releaser.release(retired);
}

void PagedNode2::unload(ObjectReleaser::Objects& retired)
{
    // Scene graph mutations, including explicit unloads, must be serialized on the update thread.
    bool merged;
    {
        std::lock_guard<Mutex> lock(_mutex);
        merged = _merged.has_value(true);
        // Retain the handle, not just an available value: completion may race with reset.
        retired.emplace_back(ObjectReleaser::hold(_loaded));
        ++_revision;
        _loaded.reset();
        _merged.reset();
        _token = nullptr;
    }
    if (merged)
    {
        for (unsigned i = 0; i < _payload->getNumChildren(); ++i)
            retired.emplace_back(_payload->getChild(i));
        _payload->removeChildren(0, _payload->getNumChildren());
        removeChild(_payload.get());
    }

    _loadGate = false;
}

bool
PagedNode2::isLoadComplete() const
{
    std::lock_guard<Mutex> lock(_mutex);
    return _merged.available() || (_load_function == nullptr);
}

bool
PagedNode2::isHighestResolution() const
{
    std::lock_guard<Mutex> lock(_mutex);
    return _load_function == nullptr;
}

PagingManager::PagingManager(const std::string& jobpoolname) :
    _releaser(Registry::instance()->getObjectReleaser()->createClient("PagingManager")),
    _jobpoolName(jobpoolname)
{
    setCullingActive(false);
    ADJUST_UPDATE_TRAV_COUNT(this, +1);

    if (_jobpoolName.empty())
    {
        // If no job pool name is specified, use the default.
        _jobpoolName = DEFAULT_JOBPOOL_NAME;
    }

    _metrics = jobs::get_pool(_jobpoolName)->metrics();
}

PagingManager::~PagingManager()
{
    _releaser.drain();
    if (_mergeQueue.size() > 0)
    {
        _metrics->postprocessing.exchange(_metrics->postprocessing - _mergeQueue.size());
    }
}

void
PagingManager::traverse(osg::NodeVisitor& nv)
{
    ObjectStorage::set(&nv, this);

    if (nv.getVisitorType() == nv.CULL_VISITOR)
    {
        _newFrame.exchange(true);
    }

    else if (nv.getVisitorType() == nv.UPDATE_VISITOR)
    {
        if (_newFrame.exchange(false) == true)
        {
            update(&nv);
        }

        osg::ref_ptr<MapNode> mapNode;
        if (ObjectStorage::get(&nv, mapNode))
        {
            _sse = mapNode->getScreenSpaceError();
        }
    }

    osg::Group::traverse(nv);

#ifdef KEEP_NODES_THAT_ARE_CULLED_BUT_IN_RANGE
    if (nv.getVisitorType() == nv.CULL_VISITOR)
    {
        // After culling is complete, update all of the metrics for all of the nodes
        scoped_lock_if lock(_trackerMutex, _threadsafe);

        for (auto& entry : _tracker._list)
        {
            if (entry._data.valid())
            {         
                if (entry._data->_lodMethod == LODMethod::CAMERA_DISTANCE)
                {
                    //float range = std::max(0.0f, nv.getDistanceToViewPoint(entry._data->getBound().center(), false) - entry._data->getBound().radius());
                    float range = std::max(0.0f, nv.getDistanceToViewPoint(entry._data->getBound().center(), true) - entry._data->getBound().radius());
                    entry._data->_lastRange = std::min(entry._data->_lastRange, range);
                }
                else // LODMethod::SCREEN_SPACE
                {
                    auto* stack = nv.asCullStack();
                    if (!stack || stack->getLODScale() <= 0.0f)
                        continue;
                    float pixels = Util::getPixelSize(stack, entry._data->getBound().center(), entry._data->getBound().radius()) / stack->getLODScale();
                    float sse = _sse;
                    nv.getUserValue("oe_sse", sse);
                    // Retain if ANY view needs this page, including bounded pixel ranges.
                    entry._data->_inPixelRange |= pixels >= entry._data->getMinPixels() + sse &&
                        pixels <= entry._data->getMaxPixels() + sse;
                }
                //entry._data->_lastTime = nv.getFrameStamp() ? nv.getFrameStamp()->getReferenceTime() : 0.0;
            }
        }
    }
#endif
}

void
PagingManager::merge(PagedNode2* host, int revision, jobs::promise<bool> promise)
{
    scoped_lock_if lock(_mergeMutex, _threadsafe);

    _mergeQueue.emplace(ToMerge{ host, revision, promise });
    _metrics->postprocessing++;
}

void
PagingManager::update(osg::NodeVisitor* nv)
{
    ObjectReleaser::Objects retired;
    const bool canRetire = _releaser.available();
    {
        scoped_lock_if lock(_trackerMutex, _threadsafe);

        double now = nv && nv->getFrameStamp() ? nv->getFrameStamp()->getReferenceTime() : 0.0;

        RetirementBatch batch(this, retired);
        auto checkForDispoal = [this, now, canRetire, &retired](osg::ref_ptr<PagedNode2>& node)
        {
            if (!canRetire) return false;

            // if the node is no longer in the scene graph, expunge it
            if (node->referenceCount() == 1)
            {
                retired.emplace_back(node.get());
                // Invalidate pending merges and retire the payload separately. A queued completion
                // can still lock the node's observer while the disposal worker retains the node.
                node->unload(retired);
                return true;
            }

#ifdef KEEP_NODES_THAT_ARE_CULLED_BUT_IN_RANGE
            // Don't expire nodes that are still within range even if they haven't passed cull.
            if (node->getLODMethod() == LODMethod::CAMERA_DISTANCE &&
                node->_lastRange < node->getMaxRange())
            {
                return false;
            }
            else if (node->getLODMethod() == LODMethod::SCREEN_SPACE &&
                node->_inPixelRange)
            {
                return false;
            }
#endif

            // respect the min lifespan of the node to prevent thrashing
            if (now > 0.0 && now - node->_lastTime < node->getTimeoutSeconds())
            {
                return false;
            }

            if (node->getAutoUnload())
            {
                node->unload();
                return true;
            }

            return false;
        };

        _tracker.flush(_mergesPerFrame, checkForDispoal);

        // Reset the lastRange on the nodes for the next frame.
        for (auto& entry : _tracker._list)
        {
            if (entry._data.valid())
            {
                entry._data->_lastRange = FLT_MAX;
                entry._data->_inPixelRange = false;
            }
        }
    }

    // Pop before calling into the scene graph or application callbacks. Each completion retains
    // its original revision and promise, even if the node has since unloaded or started again.
    std::size_t remaining;
    {
        scoped_lock_if lock(_mergeMutex, _threadsafe);
        remaining = _mergeQueue.size();
    }
    unsigned count = 0u;
    while (remaining > 0u && count < _mergesPerFrame)
    {
        --remaining;
        ToMerge entry;
        {
            scoped_lock_if lock(_mergeMutex, _threadsafe);
            if (_mergeQueue.empty())
                break;
            entry = std::move(_mergeQueue.front());
            _mergeQueue.pop();
            _metrics->postprocessing--;
        }
        osg::ref_ptr<PagedNode2> next;
        if (entry._node.lock(next))
        {
            // Only nodes with nonempty bounds count towards the limit. Bounds are evaluated
            // here on the update thread, after the node has joined the live graph.
            const double referenceTime = nv && nv->getFrameStamp() ? nv->getFrameStamp()->getReferenceTime() : 0.0;
            if (next->merge(entry._revision, entry._promise, referenceTime) && next->getBound().radius() > 0.0)
                ++count;
        }
        else
            entry._promise.resolve(false);
    }
    // All update-local node references must be gone before the worker can release these batches.
    _releaser.release(retired);
}

namespace
{
    std::string buildName(osg::Node* node) {
        std::string str;
        while (node) {
            if (!node->getName().empty())
                str += node->getName() + " ";
            node = node->getNumParents() > 0 ? node->getParent(0) : nullptr;
        } 
        return str;
    }
}


std::vector<PagingManager::Stats>
PagingManager::dumpStats()
{
    std::vector<Stats> result;
    scoped_lock_if lock(_trackerMutex, _threadsafe);
    result.reserve(_tracker.size());
    for (auto& entry : _tracker._list)
    {
        if (entry._data.valid())
        {
            Stats stats;
            stats.name = buildName(entry._data);
            stats.maxRange = entry._data->_maxRange;
            stats.lastRange = -entry._data->_priority;
            result.emplace_back(stats);
        }
    }
    std::sort(result.begin(), result.end(), [](const Stats& a, const Stats& b) { return a.lastRange < b.lastRange; });
    return result;
}
