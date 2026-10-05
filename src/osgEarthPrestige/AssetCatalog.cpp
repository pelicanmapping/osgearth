/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarthPrestige/AssetCatalog>
#include "PlaceholderAssets.h"
#include "Grass.h"
#include "TreeCards.h"
#include <osgEarth/Registry>
#include <osgEarth/VirtualProgram>
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/Geometry>
#include <osg/Program>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

using namespace osgEarth;
using namespace osgEarthPrestige;

ScatterAsset::ScatterAsset(const Config& conf)
{
    conf.get("name", name);
    conf.get("canopy_trees", canopyTrees);
    optional<URI> nearURI, coarseURI, canopyURI;
    if (conf.get("near", nearURI)) nearModel = nearURI.get();
    if (conf.get("coarse", coarseURI)) coarseModel = coarseURI.get();
    if (conf.get("canopy", canopyURI)) canopyModel = canopyURI.get();
}

Config ScatterAsset::getConfig() const
{
    Config conf("asset");
    conf.set("name", name);
    conf.set("canopy_trees", canopyTrees);
    conf.set("near", nearModel.getConfig());
    conf.set("coarse", coarseModel.getConfig());
    if (!canopyModel.empty()) conf.set("canopy", canopyModel.getConfig());
    return conf;
}

namespace
{
    //! Rejects dynamic/custom-shader scene graphs rather than silently losing their rendering semantics.
    struct StaticModel : osg::NodeVisitor
    {
        bool valid = true;
        //! Traverses even masked children to validate the complete asset.
        StaticModel() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { setNodeMaskOverride(~0u); }
        //! Accepts groups, affine transforms, geodes, and geometry without callbacks or custom programs.
        void apply(osg::Node& node) override
        {
            const std::string type = node.className();
            auto state = node.getStateSet();
            if ((type != "Group" && type != "MatrixTransform" && type != "Geode" && type != "Geometry") ||
                node.getNodeMask() != ~0u || node.getDataVariance() == osg::Object::DYNAMIC ||
                node.getUpdateCallback() || node.getCullCallback() || node.getEventCallback() ||
                (state && (state->getAttribute(osg::StateAttribute::PROGRAM) || VirtualProgram::get(state))))
                valid = false;
            if (auto transform = dynamic_cast<osg::MatrixTransform*>(&node))
            {
                const auto& m = transform->getMatrix();
                if (transform->getReferenceFrame() != osg::Transform::RELATIVE_RF ||
                    m(0,3) != 0.0 || m(1,3) != 0.0 || m(2,3) != 0.0 || m(3,3) != 1.0) valid = false;
            }
            if (state)
            {
                if (state->getDataVariance() == osg::Object::DYNAMIC || state->requiresUpdateTraversal() ||
                    state->requiresEventTraversal() || (state->getMode(GL_BLEND) & osg::StateAttribute::ON)) valid = false;
                for (const auto& unit : state->getTextureAttributeList())
                    for (const auto& attribute : unit)
                        if (auto texture = dynamic_cast<osg::Texture*>(attribute.second.first.get()))
                            if (!texture->getImage(0) || !texture->getImage(0)->data()) valid = false;
            }
            traverse(node);
        }
    };

    //! Shared independently of the cache so a pinned Chonk may safely outlive its layer.
    struct Counts
    {
        std::atomic<std::size_t> bytes{0};
        std::atomic<unsigned> assets{0}, failures{0}, denials{0};
        std::mutex errorMutex;
        std::string error;
    };

    //! Aliased shared ownership ties accounting to the Chonk's actual final drawable owner.
    struct Resident
    {
        Chonk::Ptr model;
        osg::ref_ptr<TextureArena> arena;
        std::shared_ptr<Counts> counts;
        std::size_t bytes = 0;
        std::vector<Chonk::Ptr> sources; // retain source texture/material accounting once across all templates
        //! Drops model ownership before returning its admitted bytes to the budget.
        ~Resident()
        {
            model.reset();
            sources.clear();
            arena = nullptr;
            counts->bytes.fetch_sub(bytes);
            counts->assets.fetch_sub(1u);
        }
    };

    //! Counts canonical mesh/material data and decoded texture images, including estimated generated mip levels.
    std::size_t contentBytes(const Chonk& model, TextureArena& arena)
    {
        std::size_t result = model._vbo_store.size() * sizeof(Chonk::VertexGPU) +
            model._ebo_store.size() * sizeof(Chonk::element_t) + model._materials.size() * sizeof(MaterialArena::GPU);
        std::set<const osg::Image*> images;
        for (const auto& material : model._materials)
        {
            std::vector<int> indices(material->textures.begin(), material->textures.end());
            indices.push_back(material->occlusion);
            for (int index : indices)
            {
                auto texture = index < 0 ? Texture::Ptr() : arena.find(unsigned(index));
                if (!texture || !texture->osgTexture()) continue;
                for (unsigned i = 0; i < texture->osgTexture()->getNumImages(); ++i)
                {
                    auto image = texture->osgTexture()->getImage(i);
                    if (image && images.insert(image).second)
                    {
                        const std::size_t bytes = image->getTotalSizeInBytesIncludingMipmaps();
                        result += texture->mipmap() && !image->isMipmap() ? bytes + (bytes + 2u) / 3u : bytes;
                    }
                }
            }
        }
        return result;
    }
}

struct AssetCatalog::Impl
{
    struct Entry
    {
        Chonk::WeakPtr model;
        std::chrono::steady_clock::time_point retry;
    };
    std::map<std::string, ScatterAsset> catalog;
    std::map<std::string, Entry> cache;
    osg::ref_ptr<osgDB::Options> readOptions;
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    ChonkFactory factory{arena.get()};
    std::shared_ptr<Counts> counts = std::make_shared<Counts>();
    std::size_t budget;
    std::mutex mutex;

    //! Records a failed admission once per retry window; callbacks/UI never hold the I/O mutex.
    void error(const std::string& message, bool denied)
    {
        if (denied) ++counts->denials;
        else ++counts->failures;
        std::lock_guard<std::mutex> lock(counts->errorMutex);
        counts->error = message;
        OE_WARN << "[Prestige vegetation assets] " << message << std::endl;
    }
};

AssetCatalog::AssetCatalog(const std::vector<ScatterAsset>& entries, std::size_t budget, const osgDB::Options* options) :
    _impl(new Impl())
{
    for (const auto& entry : entries) _impl->catalog.emplace(entry.name, entry);
    _impl->budget = budget;
    _impl->readOptions = Registry::cloneOrCreateOptions(options);
    _impl->readOptions->setObjectCacheHint(osgDB::Options::CACHE_NONE);
    _impl->readOptions->setPluginData("osgEarth::URIResultCache", nullptr);
    _impl->arena->setAutoRelease(true);
}

AssetCatalog::~AssetCatalog() = default;

TextureArena* AssetCatalog::textures() const { return _impl->arena.get(); }

AssetResidency AssetCatalog::residency() const
{
    AssetResidency result;
    auto& impl = *_impl;
    result.bytes = impl.counts->bytes.load();
    result.budget = impl.budget;
    result.assets = impl.counts->assets.load();
    result.failedLoads = impl.counts->failures.load();
    result.budgetDenials = impl.counts->denials.load();
    std::lock_guard<std::mutex> lock(impl.counts->errorMutex);
    result.lastError = impl.counts->error;
    return result;
}

Chonk::Ptr AssetCatalog::acquire(const ScatterGroup& group, const std::string& name, ProgressCallback* progress)
{
    return acquireModel(group,name,false,progress);
}

Chonk::Ptr AssetCatalog::acquireImpostor(const ScatterGroup& group, const std::string& name, ProgressCallback* progress)
{
    ScatterGroup proxy = group;
    proxy.lodPixels = proxy.minPixels = 0.0f;
    proxy.proceduralGrass = false;
    return acquireModel(proxy,name,false,progress,true);
}

Chonk::Ptr AssetCatalog::acquireStand(const ScatterGroup& group, const std::string& name, ProgressCallback* progress)
{
    ScatterGroup proxy = group;
    proxy.lodPixels = proxy.minPixels = 0.0f;
    proxy.proceduralGrass = false;
    return acquireModel(proxy,name,true,progress);
}

Chonk::Ptr AssetCatalog::acquireTreeCards(const ScatterGroup& group, const std::string& name, unsigned slots,
    ProgressCallback* progress, bool stands)
{
    if (slots < 8u || slots > 256u || (progress && progress->isCanceled())) return {};
    auto source = stands ? acquireStand(group,name,progress) : acquireImpostor(group,name,progress);
    if (!source) return {};
    auto& impl = *_impl;
    const auto identity = std::string(stands ? "stand-cards:" : "tree-cards:")+group.asset+":"+
        std::to_string(name.size())+":"+name+":"+std::to_string(slots);
    std::lock_guard<std::mutex> lock(impl.mutex);
    auto& entry = impl.cache[identity];
    if (auto resident = entry.model.lock()) return resident;
    Chonk::Ptr model;
    const auto status = createTreeCardTemplate(*source,slots,model);
    if (progress && progress->isCanceled()) return {};
    const std::size_t bytes = model ? model->_vbo_store.size()*sizeof(Chonk::VertexGPU)+
        model->_ebo_store.size()*sizeof(Chonk::element_t) : 0u;
    const bool denied = status.isOK() && bytes > impl.budget-impl.counts->bytes.load();
    if (status.isError() || denied)
    {
        impl.error(denied ? "Tree card template exceeds content budget" : status.message(),denied);
        return {};
    }
    model->name() = identity;
    auto resident = std::make_shared<Resident>();
    resident->model = model; resident->arena = impl.arena; resident->counts = impl.counts; resident->bytes = bytes;
    resident->sources.push_back(source);
    impl.counts->bytes.fetch_add(bytes); ++impl.counts->assets;
    Chonk::Ptr alias(resident,model.get());
    entry.model = alias;
    return alias;
}

Chonk::Ptr AssetCatalog::acquireModel(const ScatterGroup& group, const std::string& name, bool canopySource,
    ProgressCallback* progress, bool coarseOnly)
{
    auto& impl = *_impl;
    // Only representation-affecting fields participate: density/range/placement edits reuse the same geometry.
    std::ostringstream key;
    key.imbue(std::locale::classic());
    key << "model:" << canopySource << ':' << coarseOnly << ':' << std::setprecision(9) << name.size() << ':' << name << ':' <<
        group.asset << ':' << group.lodPixels << ':' << group.minPixels << ':' << group.proceduralGrass;
    if (group.proceduralGrass)
        key << ':' << group.grassBlades << ':' << group.grassRadius << ':' << group.grassHeight << ':' <<
            group.grassWidth << ':' << group.grassWind;
    std::lock_guard<std::mutex> lock(impl.mutex); // serialize CPU loads, never GL or update traversal
    if (progress && progress->isCanceled()) return {};
    const auto now = std::chrono::steady_clock::now();
    for (auto i = impl.cache.begin(); i != impl.cache.end(); )
        if (i->second.model.expired() && i->second.retry <= now) i = impl.cache.erase(i);
        else ++i;
    auto& entry = impl.cache[key.str()];
    if (auto resident = entry.model.lock()) return resident;
    if (entry.retry > now) return {};
    Chonk::Ptr model = Chonk::create();
    model->name() = "Prestige vegetation " + (name.empty() ? group.asset : name) + (canopySource ? " canopy source" : "");
    std::string failure;
    try
    {
        auto found = impl.catalog.find(name);
        for (unsigned lod = 0; lod < (!canopySource && !coarseOnly && group.lodPixels > 0.0f ? 2u : 1u); ++lod)
        {
            osg::ref_ptr<osg::Node> node;
            if (name.empty()) node = group.proceduralGrass ? createGrassPatch(group, lod) :
                createPlaceholderAsset(group.asset, coarseOnly || canopySource ? 1u : lod);
            else if (found != impl.catalog.end())
            {
                const URI& uri = canopySource ? (found->second.canopyModel.empty() ?
                    found->second.coarseModel : found->second.canopyModel) :
                    (lod == 0u && !coarseOnly ? found->second.nearModel : found->second.coarseModel);
                // This catalog supplies timed negative caching; a corrected file must not remain in URI's 404 cache.
                Registry::instance()->unblacklist(uri.full());
                node = uri.getNode(impl.readOptions, progress);
            }
            if (progress && progress->isCanceled()) return {};
            StaticModel check;
            if (node) node->accept(check);
            if (!node || !check.valid || !model->add(node,
                !canopySource && !coarseOnly && lod == 0u && group.lodPixels > 0.0f ? group.lodPixels : group.minPixels,
                lod == 0u ? FLT_MAX : group.lodPixels, impl.factory))
            {
                failure = "Cannot load static " + std::string(canopySource ? "canopy" : (lod ? "coarse" : "near")) +
                    " model: " + model->name();
                break;
            }
        }
    }
    catch (const std::exception& e) { failure = model->name() + ": " + e.what(); }
    if (progress && progress->isCanceled()) return {};
    if (failure.empty())
    {
        for (const auto& vertex : model->_vbo_store)
            if (!std::isfinite(vertex.position.length2()) || !std::isfinite(vertex.normal.length2()))
            {
                failure = "Nonfinite geometry in " + model->name();
                break;
            }
    }
    model->getBound(); // initialize immutable bounds before sharing across paging workers
    const auto bytes = contentBytes(*model, *impl.arena);
    const bool denied = failure.empty() && bytes > impl.budget - impl.counts->bytes.load();
    if (denied || !failure.empty())
    {
        entry.retry = now + std::chrono::seconds(5);
        impl.error(denied ? "Content budget exceeded by " + model->name() + "" : failure, denied);
        return {};
    }
    auto resident = std::make_shared<Resident>();
    resident->model = model;
    resident->arena = impl.arena;
    resident->counts = impl.counts;
    resident->bytes = bytes;
    impl.counts->bytes.fetch_add(bytes);
    ++impl.counts->assets;
    Chonk::Ptr alias(resident, model.get());
    entry.model = alias;
    return alias;
}
