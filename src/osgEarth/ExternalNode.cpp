/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */

#include <osgEarth/ExternalNode>
#include <osgEarth/InstancedExternalNode>
#include <osgEarth/Notify>
#include <osgEarth/Registry>
#include <osgEarth/URI>

#include <osg/observer_ptr>
#include <osg/PrimitiveSet>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ObjectWrapper>
#include <osgDB/Registry>

#include <algorithm>
#include <cctype>
#include <exception>
#include <map>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

using namespace osgEarth;

#define LC "[ExternalNode] "

namespace
{
    std::string resolveFilename(
        const std::string& filename,
        const osgDB::Options* options)
    {
        if (filename.empty())
            return filename;

        std::string resolved;
        const URIContext context(options);
        if (!context.empty())
        {
            resolved = URI(filename, context).full();
        }
        else if (osgDB::containsServerAddress(filename) ||
                 osgDB::isAbsolutePath(filename))
        {
            resolved = filename;
        }
        else
        {
            resolved = osgDB::findDataFile(filename, options);
            if (resolved.empty() && options &&
                !options->getDatabasePathList().empty())
            {
                // osgDB readers put the containing file's directory first.
                // Preserve that intended location even when the referenced
                // file does not exist yet during an authoring workflow.
                const std::string& base =
                    options->getDatabasePathList().front();
                if (!base.empty())
                    resolved = osgDB::concatPaths(base, filename);
            }
            if (resolved.empty())
            {
                resolved = osgDB::concatPaths(
                    osgDB::getCurrentWorkingDirectory(), filename);
            }
        }

        if (!osgDB::containsServerAddress(resolved))
            resolved = osgDB::getRealPath(resolved);

        return osgDB::convertFileNameToUnixStyle(resolved);
    }

    std::string canonicalFilename(const std::string& resolved)
    {
        std::string canonical = resolved;
#ifdef _WIN32
        // Only local Windows filenames are case-insensitive. Server paths,
        // queries, and tokens can be case-sensitive even on Windows.
        if (!osgDB::containsServerAddress(canonical))
            canonical = osgDB::convertToLowerCase(canonical);
#endif
        return canonical;
    }

    std::string makeKey(
        const std::string& resolved,
        const osgDB::Options* options)
    {
        std::ostringstream key;
        key << canonicalFilename(resolved) << '\x1f';
        if (options)
        {
            key << options->getOptionString() << '\x1e'
                << static_cast<const void*>(options->getFindFileCallback())
                << '\x1e'
                << static_cast<const void*>(options->getReadFileCallback())
                << '\x1e'
                << static_cast<const void*>(options->getAuthenticationMap())
                << '\x1e'
                << static_cast<const void*>(URIAliasMap::from(options))
                << '\x1e'
                << static_cast<const void*>(URIPostReadCallback::from(options));
        }
        return key.str();
    }

    bool optionPresent(const std::string& options, const std::string& option)
    {
        std::string::size_type pos = 0;
        while ((pos = options.find(option, pos)) != std::string::npos)
        {
            const bool startsToken =
                pos == 0 || std::isspace(static_cast<unsigned char>(options[pos - 1]));
            const std::string::size_type end = pos + option.size();
            const bool endsToken =
                end == options.size() ||
                std::isspace(static_cast<unsigned char>(options[end]));
            if (startsToken && endsToken)
                return true;
            pos = end;
        }
        return false;
    }

    void appendOption(osgDB::Options* options, const std::string& option)
    {
        if (!options || optionPresent(options->getOptionString(), option))
            return;

        std::string value = options->getOptionString();
        if (!value.empty() && !std::isspace(static_cast<unsigned char>(value.back())))
            value += ' ';
        value += option;
        options->setOptionString(value);
    }

    thread_local std::vector<std::string> s_loadStack;

    struct LoadStackGuard
    {
        explicit LoadStackGuard(const std::string& key)
        {
            s_loadStack.push_back(key);
        }

        ~LoadStackGuard()
        {
            s_loadStack.pop_back();
        }
    };

    bool isLoadingOnThisThread(const std::string& identity)
    {
        return std::find(s_loadStack.begin(), s_loadStack.end(), identity) !=
            s_loadStack.end();
    }

    std::uint64_t saturatedAdd(std::uint64_t lhs, std::uint64_t rhs)
    {
        const std::uint64_t maximum =
            std::numeric_limits<std::uint64_t>::max();
        return rhs > maximum - lhs ? maximum : lhs + rhs;
    }

    std::uint64_t saturatedMultiply(std::uint64_t lhs, std::uint64_t rhs)
    {
        const std::uint64_t maximum =
            std::numeric_limits<std::uint64_t>::max();
        if (lhs == 0u || rhs == 0u)
            return 0u;
        return lhs > maximum / rhs ? maximum : lhs * rhs;
    }

    std::uint64_t triangleCountForMode(
        GLenum mode,
        std::uint64_t elementCount)
    {
        switch (mode)
        {
        case GL_TRIANGLES:
            return elementCount / 3u;
        case GL_TRIANGLE_STRIP:
        case GL_TRIANGLE_FAN:
        case GL_POLYGON:
            return elementCount >= 3u ? elementCount - 2u : 0u;
        case GL_QUADS:
            return (elementCount / 4u) * 2u;
        case GL_QUAD_STRIP:
            return elementCount >= 4u ?
                ((elementCount / 2u) - 1u) * 2u : 0u;
        default:
            return 0u;
        }
    }

    std::uint64_t triangleCount(const osg::PrimitiveSet& primitiveSet)
    {
        std::uint64_t result = 0u;
        if (const auto* lengths =
                dynamic_cast<const osg::DrawArrayLengths*>(&primitiveSet))
        {
            for (const auto length : *lengths)
            {
                result = saturatedAdd(
                    result,
                    triangleCountForMode(
                        primitiveSet.getMode(),
                        static_cast<std::uint64_t>(length)));
            }
            return result;
        }
#ifdef OSG_HAS_MULTIDRAWARRAYS
        if (const auto* draws =
                dynamic_cast<const osg::MultiDrawArrays*>(&primitiveSet))
        {
            for (const auto count : draws->getCounts())
            {
                result = saturatedAdd(
                    result,
                    triangleCountForMode(
                        primitiveSet.getMode(),
                        static_cast<std::uint64_t>(count)));
            }
            return result;
        }
#endif
        return triangleCountForMode(
            primitiveSet.getMode(),
            static_cast<std::uint64_t>(primitiveSet.getNumIndices()));
    }

    /** Counts unique geometry owned directly by one canonical asset. Nested
     *  ExternalNodes are separate managed assets and are deliberately not
     *  folded into their parent's cached geometry statistics. */
    struct ExternalGeometryStatsVisitor : public osg::NodeVisitor
    {
        ExternalGeometryStatsVisitor() :
            osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            setNodeMaskOverride(~0u);
        }

        void apply(osg::Group& group) override
        {
            if (dynamic_cast<ExternalNode*>(&group) ||
                dynamic_cast<InstancedExternalNode*>(&group))
            {
                return;
            }
            traverse(group);
        }

        void apply(osg::Geometry& geometry) override
        {
            if (!geometries.insert(&geometry).second)
                return;

            if (geometry.getVertexArray())
            {
                vertices = saturatedAdd(
                    vertices,
                    static_cast<std::uint64_t>(
                        geometry.getVertexArray()->getNumElements()));
            }

            for (const auto& primitiveSet : geometry.getPrimitiveSetList())
            {
                if (primitiveSet.valid())
                {
                    triangles = saturatedAdd(
                        triangles, triangleCount(*primitiveSet));
                }
            }
        }

        std::set<const osg::Geometry*> geometries;
        std::uint64_t vertices = 0u;
        std::uint64_t triangles = 0u;
    };
}

namespace osgEarth
{
    // This distinct type survives osgUtil::Optimizer's empty-Group removal.
    // Each ExternalNode owns one slot; all slots point at the same payload.
    class ExternalAssetSlot : public osg::Group
    {
    public:
        struct Usage
        {
            bool instanced = false;
            std::uint64_t instanceCount = 1u;
            bool hardware = false;
            bool fallback = false;
        };

        ExternalAssetSlot() = default;
        ExternalAssetSlot(
            const ExternalAssetSlot& rhs,
            const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY) :
            osg::Group(rhs, copyop)
        {
            const Usage usage = rhs.getUsage();
            setUsage(
                usage.instanced,
                usage.instanceCount,
                usage.hardware,
                usage.fallback);
        }

        META_Node(osgEarth, ExternalAssetSlot);

        void setNode(osg::Node* node)
        {
            if (getNumChildren() == 1u && getChild(0u) == node)
                return;
            if (getNumChildren() > 0u)
                removeChildren(0u, getNumChildren());
            if (node)
                addChild(node);
        }

        void setUsage(
            bool instanced,
            std::uint64_t instanceCount,
            bool hardware,
            bool fallback)
        {
            std::lock_guard<std::mutex> lock(_usageMutex);
            _usage.instanced = instanced;
            _usage.instanceCount = instanceCount;
            _usage.hardware = hardware;
            _usage.fallback = fallback;
        }

        Usage getUsage() const
        {
            std::lock_guard<std::mutex> lock(_usageMutex);
            return _usage;
        }

    private:
        mutable std::mutex _usageMutex;
        Usage _usage;
    };

    class ExternalAsset : public osg::Referenced
    {
    public:
        ExternalAsset(
            const std::string& key,
            const std::string& resolvedFilename,
            osgDB::Options* options) :
            _key(key),
            _resolvedFilename(resolvedFilename),
            _options(options)
        {
        }

        osg::ref_ptr<osg::Node> node() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return _node;
        }

        std::string error() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return _error;
        }

        void snapshot(
            osg::ref_ptr<osg::Node>& node,
            std::uint64_t& revision) const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            node = _node;
            revision = _revision;
        }

        void getInfo(ExternalAssetManager::AssetInfo& info) const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            info = ExternalAssetManager::AssetInfo();
            info.filename = _resolvedFilename;
            info.lastError = _error;
            info.loaded = _node.valid();
            info.revision = _revision;
            info.modelVertexCount = _vertexCount;
            info.modelTriangleCount = _triangleCount;

            for (const auto& weakClient : _clients)
            {
                osg::ref_ptr<ExternalAssetSlot> client;
                if (!weakClient.lock(client))
                    continue;

                ++info.referenceCount;
                const ExternalAssetSlot::Usage usage = client->getUsage();
                if (!usage.instanced)
                {
                    ++info.ordinaryReferenceCount;
                    continue;
                }

                ++info.instancedBatchCount;
                info.instanceCount += usage.instanceCount;
                if (usage.hardware)
                    ++info.hardwareInstancedBatchCount;
                if (usage.fallback)
                    ++info.fallbackInstancedBatchCount;
            }

            info.placementCount = saturatedAdd(
                info.ordinaryReferenceCount, info.instanceCount);
            info.totalVertexCount = saturatedMultiply(
                info.modelVertexCount, info.placementCount);
            info.totalTriangleCount = saturatedMultiply(
                info.modelTriangleCount, info.placementCount);
        }

        void payloadSnapshot(
            osg::ref_ptr<osg::Node>& node,
            std::uint64_t& vertexCount,
            std::uint64_t& triangleCount) const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            node = _node;
            vertexCount = _vertexCount;
            triangleCount = _triangleCount;
        }

        bool getInstancingTemplate(
            std::uint64_t revision,
            bool& ready,
            osg::ref_ptr<osg::Node>& node,
            unsigned& geometryCount,
            std::string& error) const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (revision != _revision)
                return false;
            ready = _instancingTemplateReady;
            node = _instancingTemplate;
            geometryCount = _instancingTemplateGeometryCount;
            error = _instancingTemplateError;
            return true;
        }

        bool installInstancingTemplate(
            std::uint64_t revision,
            osg::Node* candidate,
            unsigned candidateGeometryCount,
            const std::string& candidateError,
            osg::ref_ptr<osg::Node>& installed,
            unsigned& installedGeometryCount,
            std::string& installedError)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (revision != _revision)
                return false;
            if (!_instancingTemplateReady)
            {
                _instancingTemplate = candidate;
                _instancingTemplateGeometryCount = candidateGeometryCount;
                _instancingTemplateError = candidateError;
                _instancingTemplateReady = true;
            }
            installed = _instancingTemplate;
            installedGeometryCount = _instancingTemplateGeometryCount;
            installedError = _instancingTemplateError;
            return true;
        }

        void addClient(ExternalAssetSlot* client)
        {
            if (!client)
                return;

            osg::ref_ptr<osg::Node> current;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                bool found = false;
                for (auto i = _clients.begin(); i != _clients.end();)
                {
                    if (!i->valid())
                    {
                        i = _clients.erase(i);
                    }
                    else
                    {
                        found = found || i->get() == client;
                        ++i;
                    }
                }
                if (!found)
                    _clients.emplace_back(client);
                current = _node;
            }
            client->setNode(current.get());
        }

        void removeClient(ExternalAssetSlot* client)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _clients.erase(
                std::remove_if(
                    _clients.begin(),
                    _clients.end(),
                    [client](const osg::observer_ptr<ExternalAssetSlot>& value)
                    {
                        return !value.valid() || value.get() == client;
                    }),
                _clients.end());
        }

        void publish(
            osg::Node* node,
            const std::string& error,
            std::uint64_t vertexCount,
            std::uint64_t triangleCount)
        {
            std::vector<osg::ref_ptr<ExternalAssetSlot>> clients;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                const bool preserveInstancingTemplate =
                    !error.empty() && node != nullptr && node == _node.get();
                _node = node;
                _error = error;
                _vertexCount = vertexCount;
                _triangleCount = triangleCount;
                if (!preserveInstancingTemplate)
                {
                    _instancingTemplate = nullptr;
                    _instancingTemplateGeometryCount = 0u;
                    _instancingTemplateError.clear();
                    _instancingTemplateReady = false;
                }
                ++_revision;
                for (auto i = _clients.begin(); i != _clients.end();)
                {
                    osg::ref_ptr<ExternalAssetSlot> client;
                    if (i->lock(client))
                    {
                        clients.push_back(client);
                        ++i;
                    }
                    else
                    {
                        i = _clients.erase(i);
                    }
                }
            }

            for (auto& client : clients)
                client->setNode(node);
        }

        std::string _key;
        std::string _resolvedFilename;
        osg::ref_ptr<osgDB::Options> _options;

    private:
        mutable std::mutex _mutex;
        osg::ref_ptr<osg::Node> _node;
        std::vector<osg::observer_ptr<ExternalAssetSlot>> _clients;
        std::string _error;
        std::uint64_t _revision = 0u;
        std::uint64_t _vertexCount = 0u;
        std::uint64_t _triangleCount = 0u;
        osg::ref_ptr<osg::Node> _instancingTemplate;
        unsigned _instancingTemplateGeometryCount = 0u;
        std::string _instancingTemplateError;
        bool _instancingTemplateReady = false;
    };
}

struct ExternalAssetManager::Impl
{
    mutable std::mutex assetsMutex;
    mutable std::recursive_mutex loadMutex;
    mutable std::map<std::string, osg::observer_ptr<ExternalAsset>> assets;
    mutable std::map<std::string, std::set<std::string>> dependencies;

    bool hasPath(
        const std::string& start,
        const std::string& target) const
    {
        std::set<std::string> visited;
        std::vector<std::string> pending(1u, start);
        while (!pending.empty())
        {
            const std::string current = pending.back();
            pending.pop_back();
            if (current == target)
                return true;
            if (!visited.insert(current).second)
                continue;

            auto i = dependencies.find(current);
            if (i != dependencies.end())
                pending.insert(pending.end(), i->second.begin(), i->second.end());
        }
        return false;
    }

    void removeDependencies(const std::string& key) const
    {
        dependencies.erase(key);
        for (auto& entry : dependencies)
            entry.second.erase(key);
    }

    std::vector<osg::ref_ptr<ExternalAsset>> liveAssets() const
    {
        std::vector<osg::ref_ptr<ExternalAsset>> result;
        // Keep lock ordering consistent with acquire(): load, then assets.
        std::lock_guard<std::recursive_mutex> loadLock(loadMutex);
        std::lock_guard<std::mutex> assetsLock(assetsMutex);
        for (auto i = assets.begin(); i != assets.end();)
        {
            osg::ref_ptr<ExternalAsset> asset;
            if (i->second.lock(asset))
            {
                result.push_back(asset);
                ++i;
            }
            else
            {
                removeDependencies(i->first);
                i = assets.erase(i);
            }
        }
        return result;
    }
};

ExternalAssetManager&
ExternalAssetManager::instance()
{
    static ExternalAssetManager s_instance;
    return s_instance;
}

ExternalAssetManager::ExternalAssetManager() :
    _impl(new Impl())
{
}

ExternalAssetManager::~ExternalAssetManager()
{
    delete _impl;
}

void
ExternalAssetManager::getAssets(AssetInfoList& output) const
{
    output.clear();

    // Do not take loadMutex here. A read can hold it for the duration of a
    // slow file or network request, and a diagnostics panel must never wait
    // for that operation just to list the already-known cache variants.
    std::vector<osg::ref_ptr<ExternalAsset>> assets;
    {
        std::lock_guard<std::mutex> lock(_impl->assetsMutex);
        assets.reserve(_impl->assets.size());
        for (const auto& entry : _impl->assets)
        {
            osg::ref_ptr<ExternalAsset> asset;
            if (entry.second.lock(asset))
                assets.push_back(asset);
        }
    }

    output.reserve(assets.size());
    for (const auto& asset : assets)
    {
        output.emplace_back();
        asset->getInfo(output.back());
    }

    // Dependency edges are protected by loadMutex. Report them when the lock
    // is immediately available and leave them explicitly unavailable while a
    // load is in progress instead of stalling the caller.
    std::unique_lock<std::recursive_mutex> loadLock(
        _impl->loadMutex, std::try_to_lock);
    if (!loadLock.owns_lock())
        return;

    for (std::size_t index = 0u; index < assets.size(); ++index)
    {
        AssetInfo& info = output[index];
        const std::string& key = assets[index]->_key;
        const auto dependencies = _impl->dependencies.find(key);
        if (dependencies != _impl->dependencies.end())
            info.dependencyCount = dependencies->second.size();

        for (const auto& entry : _impl->dependencies)
        {
            if (entry.second.find(key) != entry.second.end())
                ++info.dependentCount;
        }
        info.dependencyStatsAvailable = true;
    }
}

osg::ref_ptr<ExternalAsset>
ExternalAssetManager::acquire(
    const std::string& filename,
    const osgDB::Options* options,
    std::string& error)
{
    error.clear();
    if (filename.empty())
    {
        error = "External asset filename is empty.";
        return nullptr;
    }

    osg::ref_ptr<osgDB::Options> localOptions =
        Registry::cloneOrCreateOptions(options);
    // This manager is the authoritative node cache. A URI result cache would
    // strongly retain and replay stale graphs after unload/reload, so do not
    // retain it in the managed options or pass it to nested readers.
    localOptions->removePluginData("osgEarth::URIResultCache");
    const std::string resolved = resolveFilename(filename, localOptions.get());
    const std::string key = makeKey(resolved, localOptions.get());

    // Loads are serialized, so this lock also protects the dependency graph.
    // Tracking committed edges catches a new cycle during hot reload even
    // when the referenced asset is already loaded and causes no recursion.
    std::lock_guard<std::recursive_mutex> loadLock(_impl->loadMutex);
    std::lock_guard<std::mutex> assetsLock(_impl->assetsMutex);

    auto i = _impl->assets.find(key);
    osg::ref_ptr<ExternalAsset> existing;
    if (i != _impl->assets.end() && !i->second.lock(existing))
    {
        _impl->removeDependencies(key);
        _impl->assets.erase(i);
    }

    const bool recursive = isLoadingOnThisThread(key);
    const bool closesCommittedCycle =
        !s_loadStack.empty() && _impl->hasPath(key, s_loadStack.back());
    if (recursive || closesCommittedCycle)
    {
        error = "Cyclical external asset reference to " + filename;
        OE_WARN << LC << error << std::endl;
        return nullptr;
    }

    if (!s_loadStack.empty())
        _impl->dependencies[s_loadStack.back()].insert(key);

    if (existing.valid())
        return existing;

    osg::ref_ptr<ExternalAsset> asset =
        new ExternalAsset(key, resolved, localOptions.get());
    _impl->assets[key] = asset.get();
    return asset;
}

bool
ExternalAssetManager::load(ExternalAsset* asset, bool forceReload)
{
    if (!asset)
        return false;

    // Serializing load operations avoids cross-thread dependency deadlocks and
    // makes recursion-stack cycle detection deterministic. Ordinary traversal
    // and cache lookups do not take this lock.
    std::lock_guard<std::recursive_mutex> loadLock(_impl->loadMutex);

    if (!forceReload && asset->node().valid())
        return true;

    if (isLoadingOnThisThread(asset->_key))
        return false;

    // Dependency discovery happens through acquire() calls made by the nested
    // reader. Commit those edges only if the replacement graph loads fully;
    // a failed authoring reload keeps both the last-good graph and its edges.
    const auto oldDependencies = _impl->dependencies.find(asset->_key);
    const bool hadOldDependencies =
        oldDependencies != _impl->dependencies.end();
    const std::set<std::string> previousDependencies =
        hadOldDependencies ? oldDependencies->second : std::set<std::string>();
    _impl->dependencies[asset->_key].clear();

    LoadStackGuard stackGuard(asset->_key);

    osg::ref_ptr<osgDB::Options> options =
        Registry::cloneOrCreateOptions(asset->_options.get());
    const unsigned int cacheHints =
        static_cast<unsigned int>(options->getObjectCacheHint());
    options->setObjectCacheHint(
        static_cast<osgDB::Options::CacheHintOptions>(
            cacheHints & ~static_cast<unsigned int>(osgDB::Options::CACHE_NODES)));

    // A previous not-found result may have put this URI on osgEarth's
    // blacklist. Every call that reaches this point is an explicit request to
    // read the asset (including load() after an author recreates a file), so
    // let it retry without clearing unrelated blacklist entries.
    Registry::instance()->unblacklist(asset->_resolvedFilename);
    if (forceReload)
    {
        osgDB::Registry::instance()->removeFromObjectCache(
            asset->_resolvedFilename, options.get());
        appendOption(options.get(), "gltfForceReload");
    }

    ReadResult result;
    try
    {
        result = URI(asset->_resolvedFilename).readNode(options.get());
    }
    catch (const std::exception& e)
    {
        result = ReadResult(std::string("Reader exception: ") + e.what());
    }
    catch (...)
    {
        result = ReadResult("Unknown reader exception");
    }

    if (result.succeeded() && result.getNode())
    {
        ExternalGeometryStatsVisitor stats;
        result.getNode()->accept(stats);
        asset->publish(
            result.getNode(),
            std::string(),
            stats.vertices,
            stats.triangles);
        return true;
    }

    if (hadOldDependencies)
        _impl->dependencies[asset->_key] = previousDependencies;
    else
        _impl->dependencies.erase(asset->_key);

    std::string error = result.errorDetail();
    if (error.empty())
        error = result.getResultCodeString();
    error = "Failed to load external asset " + asset->_resolvedFilename +
        ": " + error;
    // A transient authoring error should not make every live reference
    // disappear. Forced reloads retain the last good graph and expose the
    // failure through getLastError(); first loads still remain empty.
    osg::ref_ptr<osg::Node> lastGood;
    std::uint64_t vertexCount = 0u;
    std::uint64_t triangleCount = 0u;
    if (forceReload)
    {
        asset->payloadSnapshot(lastGood, vertexCount, triangleCount);
    }
    asset->publish(lastGood.get(), error, vertexCount, triangleCount);
    OE_WARN << LC << error << std::endl;
    return false;
}

bool
ExternalAssetManager::unload(ExternalAsset* asset)
{
    if (!asset)
        return false;
    std::lock_guard<std::recursive_mutex> loadLock(_impl->loadMutex);
    osgDB::Registry::instance()->removeFromObjectCache(
        asset->_resolvedFilename, asset->_options.get());
    asset->publish(nullptr, std::string(), 0u, 0u);
    _impl->dependencies.erase(asset->_key);
    return true;
}

unsigned
ExternalAssetManager::reload(
    const std::string& filename,
    const osgDB::Options* options)
{
    const std::string resolved = resolveFilename(filename, options);
    const std::string canonical = canonicalFilename(resolved);
    unsigned count = 0u;
    for (auto& asset : _impl->liveAssets())
    {
        if (canonicalFilename(asset->_resolvedFilename) == canonical &&
            load(asset.get(), true))
            ++count;
    }
    return count;
}

unsigned
ExternalAssetManager::unload(
    const std::string& filename,
    const osgDB::Options* options)
{
    const std::string resolved = resolveFilename(filename, options);
    const std::string canonical = canonicalFilename(resolved);
    unsigned count = 0u;
    for (auto& asset : _impl->liveAssets())
    {
        if (canonicalFilename(asset->_resolvedFilename) == canonical &&
            unload(asset.get()))
            ++count;
    }
    return count;
}

unsigned
ExternalAssetManager::reloadAll()
{
    unsigned count = 0u;
    for (auto& asset : _impl->liveAssets())
    {
        if (load(asset.get(), true))
            ++count;
    }
    return count;
}

void
ExternalAssetManager::unloadAll()
{
    for (auto& asset : _impl->liveAssets())
        unload(asset.get());
}

unsigned
ExternalAssetManager::getNumAssets() const
{
    return static_cast<unsigned>(_impl->liveAssets().size());
}

unsigned
ExternalAssetManager::getNumLoadedAssets() const
{
    unsigned count = 0u;
    for (auto& asset : _impl->liveAssets())
    {
        if (asset->node().valid())
            ++count;
    }
    return count;
}

ExternalNode::ExternalNode() :
    _slot(new ExternalAssetSlot())
{
    _slot->setName("osgEarth::ExternalAssetSlot");
    ensureSlot();
}

ExternalNode::ExternalNode(
    const std::string& filename,
    const osgDB::Options* options) :
    ExternalNode()
{
    setFileName(filename, options);
}

ExternalNode::ExternalNode(
    const ExternalNode& rhs,
    const osg::CopyOp& copyop) :
    // External identity remains shared even when a caller requests DEEP_COPY.
    // Copy the ordinary Group state shallowly, discard its transient runtime
    // child, and register this wrapper with the same managed asset below.
    osg::Group(rhs, osg::CopyOp::SHALLOW_COPY),
    _filename(rhs._filename),
    _readOptionsString(rhs._readOptionsString),
    _readOptions(Registry::cloneOrCreateOptions(rhs._readOptions.get())),
    _slot(new ExternalAssetSlot()),
    _localError(rhs._localError)
{
    (void)copyop;
    _slot->setName("osgEarth::ExternalAssetSlot");
    ensureSlot();
    attach(rhs._asset.get());
}

ExternalNode::~ExternalNode()
{
    detach();
}

bool
ExternalNode::setFileName(
    const std::string& filename,
    const osgDB::Options* options)
{
    detach();
    ensureSlot();
    _filename = filename;
    _readOptions = Registry::cloneOrCreateOptions(options);
    _readOptionsString = _readOptions.valid() ?
        _readOptions->getOptionString() : std::string();
    _localError.clear();

    if (_filename.empty())
        return true;

    osg::ref_ptr<ExternalAsset> asset =
        ExternalAssetManager::instance().acquire(
            _filename, _readOptions.get(), _localError);
    if (!asset.valid())
        return false;

    attach(asset.get());
    return ExternalAssetManager::instance().load(asset.get(), false);
}

bool
ExternalNode::load()
{
    if (!_asset.valid())
    {
        if (_filename.empty())
            return false;

        osg::ref_ptr<ExternalAsset> asset =
            ExternalAssetManager::instance().acquire(
                _filename, _readOptions.get(), _localError);
        if (!asset.valid())
            return false;
        attach(asset.get());
    }
    const bool loaded =
        ExternalAssetManager::instance().load(_asset.get(), false);
    if (loaded)
    {
        _slot->setNode(_asset->node().get());
        ensureSlot();
    }
    return loaded;
}

bool
ExternalNode::reload()
{
    ensureSlot();
    return _asset.valid() &&
        ExternalAssetManager::instance().load(_asset.get(), true);
}

void
ExternalNode::unload()
{
    ensureSlot();
    if (_asset.valid())
        ExternalAssetManager::instance().unload(_asset.get());
}

bool
ExternalNode::isLoaded() const
{
    return _asset.valid() && _asset->node().valid();
}

osg::ref_ptr<osg::Node>
ExternalNode::getExternalNode() const
{
    return _asset.valid() ? _asset->node() : osg::ref_ptr<osg::Node>();
}

std::string
ExternalNode::getLastError() const
{
    if (!_localError.empty())
        return _localError;
    return _asset.valid() ? _asset->error() : std::string();
}

void
ExternalNode::getAssetSnapshot(
    osg::ref_ptr<osg::Node>& node,
    std::uint64_t& revision) const
{
    if (_asset.valid())
    {
        _asset->snapshot(node, revision);
    }
    else
    {
        node = nullptr;
        revision = 0u;
    }
}

bool
ExternalNode::getInstancingTemplate(
    std::uint64_t revision,
    bool& ready,
    osg::ref_ptr<osg::Node>& node,
    unsigned& geometryCount,
    std::string& error) const
{
    if (!_asset.valid())
        return false;
    return _asset->getInstancingTemplate(
        revision, ready, node, geometryCount, error);
}

bool
ExternalNode::installInstancingTemplate(
    std::uint64_t revision,
    osg::Node* candidate,
    unsigned candidateGeometryCount,
    const std::string& candidateError,
    osg::ref_ptr<osg::Node>& installed,
    unsigned& installedGeometryCount,
    std::string& installedError)
{
    if (!_asset.valid())
        return false;
    return _asset->installInstancingTemplate(
        revision,
        candidate,
        candidateGeometryCount,
        candidateError,
        installed,
        installedGeometryCount,
        installedError);
}

void
ExternalNode::setInstancingStats(
    std::uint64_t instanceCount,
    bool usingHardwareInstancing,
    bool usingFallback)
{
    if (_slot.valid())
    {
        _slot->setUsage(
            true,
            instanceCount,
            usingHardwareInstancing,
            usingFallback);
    }
}

void
ExternalNode::attach(ExternalAsset* asset)
{
    if (_asset.get() == asset)
    {
        ensureSlot();
        if (_asset.valid())
            _asset->addClient(_slot.get());
        return;
    }
    detach();
    ensureSlot();
    _asset = asset;
    if (_asset.valid())
        _asset->addClient(_slot.get());
}

void
ExternalNode::detach()
{
    if (_asset.valid())
        _asset->removeClient(_slot.get());
    if (_slot.valid())
        _slot->setNode(nullptr);
    _asset = nullptr;
}

void
ExternalNode::ensureSlot()
{
    if (!_slot.valid() ||
        (getNumChildren() == 1u && getChild(0u) == _slot.get()))
        return;

    if (getNumChildren() > 0u)
        removeChildren(0u, getNumChildren());
    osg::Group::addChild(_slot.get());
}

//...........................................................................

#undef LC
#define LC "[ExternalNode Serializer] "

namespace osgEarth { namespace Serializers { namespace ExternalNode
{
    static bool checkExternalAsset(const osgEarth::ExternalNode& node)
    {
        return !node.getFileName().empty();
    }

    static bool readExternalAsset(
        osgDB::InputStream& input,
        osgEarth::ExternalNode& node)
    {
        input >> input.BEGIN_BRACKET;
        std::string filename;
        std::string readOptions;
        input >> input.PROPERTY("FileName");
        input.readWrappedString(filename);
        input >> input.PROPERTY("ReadOptions");
        input.readWrappedString(readOptions);
        input >> input.END_BRACKET;

        osg::ref_ptr<osgDB::Options> options =
            osgEarth::Registry::cloneOrCreateOptions(input.getOptions());
        options->setOptionString(readOptions);
        node.setFileName(filename, options.get());
        return true;
    }

    static bool writeExternalAsset(
        osgDB::OutputStream& output,
        const osgEarth::ExternalNode& node)
    {
        output << output.BEGIN_BRACKET << std::endl;
        output << output.PROPERTY("FileName");
        output.writeWrappedString(node.getFileName());
        output << std::endl;
        output << output.PROPERTY("ReadOptions");
        output.writeWrappedString(node.getReadOptionsString());
        output << std::endl;
        output << output.END_BRACKET << std::endl;
        return true;
    }

    REGISTER_OBJECT_WRAPPER(
        ExternalNode,
        new osgEarth::ExternalNode,
        osgEarth::ExternalNode,
        "osg::Object osg::Node osgEarth::ExternalNode")
    {
        ADD_USER_SERIALIZER(ExternalAsset);
    }
}}}
