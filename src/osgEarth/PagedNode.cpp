/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "PagedNode"
#include "GLUtils"
#include "NodeUtils"
#include "Progress"
#include "MapNode"

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
    _load_function = value;
}

void
PagedNode2::traverse(osg::NodeVisitor& nv)
{
    // locate the paging manager if there is one
    if (!_pagingManager_weak.valid())
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (!_pagingManager_weak.valid())
        {
            osg::ref_ptr<PagingManager> pm;
            if (ObjectStorage::get(&nv, pm))
            {
                _pagingManager_weak = pm.get();
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
                    float pixelError = _pagingManager_weak.valid() ? _pagingManager_weak->sse() : 0.0f;
                    float pixels = Util::getPixelSize(cullStack, getBound().center(), getBound().radius()) / cullStack->getLODScale();
                    inRange = (pixels >= _minPixels + pixelError) && (pixels <= _maxPixels + pixelError);
                    _priority = pixels * _priorityScale;
                }
            }

            if (_refinementFunction) inRange = _refinementFunction(nv, inRange);

            if (_replacementFunction && _refinePolicy == REFINE_REPLACE)
            {
                if (inRange && _load_function && _loaded.empty() && !_loadGate.exchange(true)) startLoad(&nv);
                const bool retain = _replacementFunction(nv,_merged.has_value(true) ? _payload.get() : nullptr,inRange);
                if (inRange || retain) touch();
                return;
            }

            if (inRange)
            {
                if (_load_function && _loaded.empty() && !_loadGate.exchange(true))
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
                //auto paged_child = _merged.has_value(true) ? _loaded.value() : nullptr;
                auto paged_child = _merged.has_value(true) ? _payload.get() : nullptr;

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
    if (_refinePolicy == REFINE_REPLACE && _merged.has_value(true))
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
    if (_pagingManager_weak.lock(pagingManager))
    {
        _token = pagingManager->use(this, _token);
    }
}

bool
PagedNode2::merge(int revision, double referenceTime)
{
    // Check the revision, b/c it is possible for a node in the merge queue
    // to be expired before it pops to the front of the merge queue and
    // this method gets invoked.
    if (_revision == revision)
    {
        // This is called from PagingManager.
        // We're in the UPDATE traversal.
        // None of these should even happen since we check for them before we enqueue the merge.
        // (See merge_job)
        OE_SOFT_ASSERT_AND_RETURN(_loaded.available(), false);
        OE_SOFT_ASSERT_AND_RETURN(_loaded.value().valid(), false);
        OE_SOFT_ASSERT_AND_RETURN(_loaded.value()->getNumParents() == 0, false);
        OE_SOFT_ASSERT_AND_RETURN(_payload.valid() && _payload->getNumParents() == 0, false);

        _payload->removeChildren(0, _payload->getNumChildren());
        _payload->addChild(_loaded.value());
        this->addChild(_payload);
        
        if (_callbacks.valid())
            _callbacks->firePostMergeNode(_loaded.value().get());

        // If we have an owner, install its cull callback as the first callback!
        //if (_owner && _owner->visibilityCallback())
        //{
        //    auto cb = _owner->visibilityCallback();
        //    //NO, this is wrong, refactor IF necessary:
        //    //cb->addNestedCallback(_loaded.value()->getCullCallback());
        //    OE_SOFT_ASSERT(_loaded.value()->getCullCallback() == nullptr, "PagedNode2 does not support an existing cull callback on the loaded node");
        //    _loaded.value()->setCullCallback(cb);
        //}

        _mergeTime = referenceTime;
        _merged.resolve(true);
        return true;
    }
    else
    {
        _merged.resolve(false);
        return false;
    }
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

        if (!_merged.available() && _loaded.available() && _loaded.value().valid())
        {
            bs.expandBy(_loaded.value()->computeBound());
        }

        return bs;
    }
}

void
PagedNode2::startLoad(const osg::Object* host)
{
    OE_SOFT_ASSERT_AND_RETURN(_load_function != nullptr, void());

    auto pnode_weak = osg::observer_ptr<PagedNode2>(this);

    auto poolName = _pagingManager_weak.valid() ? _pagingManager_weak->_jobpoolName : _jobpoolName;
    if (poolName.empty())
        poolName = DEFAULT_JOBPOOL_NAME;

    jobs::context context;
    context.pool = jobs::get_pool(poolName);
    context.priority = [pnode_weak]() {
        osg::ref_ptr<PagedNode2> pnode;
        return pnode_weak.lock(pnode) ? pnode->getPriority() : -FLT_MAX;
    };

    const int load_revision = _revision;

    auto load_and_compile_job = [pnode_weak, host](auto& promise)
    {
        osg::ref_ptr<osg::Node> result;
        osg::ref_ptr<ProgressCallback> progress = new ProgressCallback(&promise);

        osg::ref_ptr<PagedNode2> pnode;
        if (pnode_weak.lock(pnode))
        {
            result = pnode->_load_function(progress.get());

            if (result.valid())
            {
                if (pnode->_callbacks.valid())
                    pnode->_callbacks->firePreMergeNode(result.get());

                if (pnode->_preCompile && result->getBound().valid())
                {
                    GLObjectsCompiler compiler;
                    auto state = compiler.collectState(result.get());
                    compiler.requestIncrementalCompile(result, state.get(), host, promise);
                    return;
                }
            }
        }

        promise.resolve(result);
    };

    auto merge_job = [pnode_weak, load_revision](const osg::ref_ptr<osg::Node>& node, auto& promise)
    {
        osg::ref_ptr<PagedNode2> pnode;
        osg::ref_ptr<PagingManager> pagingManager;

        if (!node.valid() ||
            !pnode_weak.lock(pnode) ||
            !pnode->_pagingManager_weak.lock(pagingManager))
        {
            promise.resolve(false);
            return;
        }

        // Reject stale completion from older load cycle
        if (pnode->_revision != load_revision)
        {
            promise.resolve(false);
            return;
        }

        // Guard against duplicate merge of same child
        if (node->getNumParents() > 0)
        {
            promise.resolve(false);
            return;
        }

        pnode->dirtyBound();
        pagingManager->merge(pnode);
    };

    _loaded = jobs::dispatch(load_and_compile_job, _loaded, context);
    _merged = _loaded.then_dispatch<bool>(merge_job, context);
}

void
PagedNode2::load()
{
    if (_load_function && _loaded.empty() && !_loadGate.exchange(true))
    {
        startLoad(nullptr);
    }
}

void PagedNode2::unload()
{
    if (_merged.has_value(true))
    {
        _payload->removeChildren(0, _payload->getNumChildren());
        removeChild(_payload.get());
        //removeChild(_loaded.value());
    }

    _loaded.reset();
    _merged.reset();

    _loadGate.exchange(false);
    _token = nullptr;

    // prevents a node in the PagingManager's merge queue from being merged with old data.
    _revision++;
}

bool
PagedNode2::isLoadComplete() const
{
    return _merged.available() || (_load_function == nullptr);
}

bool
PagedNode2::isHighestResolution() const
{
    return getLoadFunction() == nullptr;
}

PagingManager::PagingManager(const std::string& jobpoolname) :
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
                    float pixels = Util::getPixelSize(nv.asCullStack(), entry._data->getBound().center(), entry._data->getBound().radius()) / nv.asCullStack()->getLODScale();
                    entry._data->_lastPixelSize = std::max(entry._data->_lastPixelSize, pixels);
                }
                //entry._data->_lastTime = nv.getFrameStamp() ? nv.getFrameStamp()->getReferenceTime() : 0.0;
            }
        }
    }
#endif
}

void
PagingManager::merge(PagedNode2* host)
{
    scoped_lock_if lock(_mergeMutex, _threadsafe);

    _mergeQueue.emplace(ToMerge{ host, host->_revision, host->getBound().radius() });
    _metrics->postprocessing++;
}

void
PagingManager::update(osg::NodeVisitor* nv)
{
    {
        scoped_lock_if lock(_trackerMutex, _threadsafe);

        double now = nv && nv->getFrameStamp() ? nv->getFrameStamp()->getReferenceTime() : 0.0;

        auto checkForDispoal = [this, now](osg::ref_ptr<PagedNode2>& node)
        {
            // if the node is no longer in the scene graph, expunge it
            if (node->referenceCount() == 1)
            {
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
                node->_lastPixelSize >= node->getMinPixels() + _sse && node->_lastPixelSize < node->getMaxPixels() + _sse)
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
                entry._data->_lastPixelSize = 0.0f;
            }
        }
    }

    // Handle merges
    // thread-local vector to prevent unnecessary reallocations
    static thread_local std::vector<ToMerge> toMerge;
    toMerge.clear();

    {
        scoped_lock_if lock(_mergeMutex, _threadsafe);
        unsigned count = 0u;
        osg::ref_ptr<PagedNode2> next;
        while (!_mergeQueue.empty() && count < _mergesPerFrame)
        {
            auto& entry = _mergeQueue.front();

            if (entry._node.lock(next) &&
                next->_loaded.available() && next->_loaded.value().valid() &&
                next->_loaded.value()->getNumParents() == 0)
            {
                // only tiles with actual geometry count towards the limit;
                // intermediate tiles do not since they are fast mergers
                if (entry._radius > 0.0)
                    count++;

                toMerge.emplace_back(std::move(entry));
            }

            _mergeQueue.pop();
            _metrics->postprocessing--;
        }
    }

    // todo: just move this into the loop above? why not?
    for(auto& entry : toMerge)
    {
        osg::ref_ptr<PagedNode2> next;
        if (entry._node.lock(next))
        {
            next->merge(entry._revision,nv && nv->getFrameStamp() ? nv->getFrameStamp()->getReferenceTime() : 0.0);
        }
    }
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
