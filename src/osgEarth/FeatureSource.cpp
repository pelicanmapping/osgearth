/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "FeatureSource"
#include "Query"
#include "CropFilter"

#define LC "[FeatureSource] " << getName() << ": "

using namespace osgEarth;

//...................................................................

Config
FeatureSource::Options::getConfig() const
{
    Config conf = Layer::Options::getConfig();

    conf.set("open_write", openWrite());
    conf.set("profile", profile());
    conf.set("geo_interpolation", "great_circle", geoInterp(), GEOINTERP_GREAT_CIRCLE);
    conf.set("geo_interpolation", "rhumb_line", geoInterp(), GEOINTERP_RHUMB_LINE);
    conf.set("fid_attribute", fidAttribute());
    conf.set("rewind_polygons", rewindPolygons());
    conf.set("vdatum", vdatum());
    conf.set("buffer_width", bufferWidth(), bufferWidthAsPercentage());
    conf.set("auto_fid", autoFID());

    if (!filters().empty())
    {
        conf.set_with_function("filters", [this](Config& conf) {
            for (auto& filter : filters())
                conf.add(filter.getConfig());
        });
    }

    return conf;
}

void
FeatureSource::Options::fromConfig(const Config& conf)
{
    conf.get("open_write", openWrite());
    conf.get("profile", profile());
    conf.get("geo_interpolation", "great_circle", geoInterp(), GEOINTERP_GREAT_CIRCLE);
    conf.get("geo_interpolation", "rhumb_line", geoInterp(), GEOINTERP_RHUMB_LINE);
    conf.get("fid_attribute", fidAttribute());
    conf.get("rewind_polygons", rewindPolygons());
    conf.get("vdatum", vdatum());
    conf.get("buffer_width", bufferWidth(), bufferWidthAsPercentage());
    conf.get("auto_fid", autoFID());

    for(auto& filterConf : conf.child("filters").children())
        filters().push_back(filterConf);
}

//...................................................................

bool FeatureSource::getOpenWrite() const {
    return options().openWrite().value();
}
void FeatureSource::setOpenWrite(bool value) {
    options().openWrite() = value;
}
GeoInterpolation FeatureSource::getGeoInterpolation() const {
    return options().geoInterp().value();
}
void FeatureSource::setGeoInterpolation(GeoInterpolation value) {
    options().geoInterp() = value;
}
const std::string& FeatureSource::getFIDAttribute() const {
    return options().fidAttribute().value();
}
void FeatureSource::setFIDAttribute(const std::string& value) {
    options().fidAttribute() = value;
}
bool FeatureSource::getRewindPolygons() const {
    return options().rewindPolygons().value();
}
void FeatureSource::setRewindPolygons(bool value) {
    options().rewindPolygons() = value;
}


void
FeatureSource::init()
{
    super::init();
    _blacklistSize = 0u;
}

Status
FeatureSource::openImplementation()
{
    unsigned int l2CacheSize = 32u;

    if (options().l2CacheSize().isSet())
    {
        l2CacheSize = options().l2CacheSize().get();
    }

    if (l2CacheSize > 0)
    {
        // note: cannot use std::make_unique in C++11
        _featuresCache = std::unique_ptr<FeaturesLRU>(new FeaturesLRU(l2CacheSize));
    }

    Status parent = super::openImplementation();
    if (parent.isError())
        return parent;

    // Create and initialize the filters.
    _filters = FeatureFilterChain::create(options().filters(), getReadOptions());
    return _filters.getStatus();
}

const Status&
FeatureSource::create(
    const FeatureProfile* profile,
    const FeatureSchema& schema,
    const Geometry::Type& geometryType,
    const osgDB::Options* readOptions)
{
    return setStatus(Status::ResourceUnavailable, "Driver does not support create");
}

const FeatureProfile*
FeatureSource::setFeatureProfile(const FeatureProfile* fp)
{
    _featureProfile = fp;

    if (fp != nullptr && options().vdatum().isSet())
    {
        FeatureProfile* new_fp = new FeatureProfile(*fp);

        new_fp->setExtent(GeoExtent(
            SpatialReference::get(
                fp->getExtent().getSRS()->getHorizInitString(),
                options().vdatum().get()),
            fp->getExtent().bounds()));

        _featureProfile = new_fp;

        OE_DEBUG << LC << "Set vdatum = " << options().vdatum().get() << std::endl;
    }

    return _featureProfile.get();
}

const FeatureProfile*
FeatureSource::getFeatureProfile() const
{
    return _featureProfile.get();
}

const FeatureFilterChain&
FeatureSource::getFilters() const
{
    return _filters;
}

void
FeatureSource::addToBlacklist( FeatureID fid )
{
    Threading::ScopedWriteLock exclusive( _blacklistMutex );
    _blacklist.insert( fid );
    _blacklistSize = _blacklist.size();
}

void
FeatureSource::removeFromBlacklist( FeatureID fid )
{
    Threading::ScopedWriteLock exclusive( _blacklistMutex );
    _blacklist.erase( fid );
    _blacklistSize = _blacklist.size();
}

void
FeatureSource::clearBlacklist()
{
    Threading::ScopedWriteLock exclusive( _blacklistMutex );
    _blacklist.clear();
    _blacklistSize = 0u;
}

bool
FeatureSource::isBlacklisted( FeatureID fid ) const
{
    // help reduce mutex contention
    if (_blacklistSize == 0u)
        return false;
    Threading::ScopedReadLock lock(_blacklistMutex);
    return _blacklist.find( fid ) != _blacklist.end();
}

void
FeatureSource::applyFilters(FeatureList& features, const GeoExtent& extent) const
{
    // apply filters before returning.
    if (!_filters.empty())
    {
        FilterContext cx;
        cx.setFeatureProfile(getFeatureProfile());
        cx.setWorkingExtent(extent);

        for(auto& filter : _filters)
        {
            cx = filter->push(features, cx);
        }
    }
}

const GeoExtent&
FeatureSource::getExtent() const
{
    if (_featureProfile.valid())
        return _featureProfile->getExtent();
    else
        return Layer::getExtent();
}

namespace
{
    struct MultiCursor : public FeatureCursor
    {
        using Cursors = std::vector<osg::ref_ptr<FeatureCursor>>;

        Cursors _cursors;
        Cursors::iterator _iter;

        MultiCursor(ProgressCallback* progress) :
            FeatureCursor(progress), _iter(_cursors.end()) { }

        void finish()
        {
            _iter = _cursors.begin();
            while (_iter != _cursors.end() && !_iter->get()->hasMore())
                _iter++;
        }

        bool hasMore() const
        {
            return _iter != _cursors.end() && _iter->get()->hasMore();
        }

        Feature* nextFeature()
        {
            Feature* f = _iter->get()->nextFeature();

            while (_iter != _cursors.end() && !_iter->get()->hasMore())
                _iter++;

            return f;
        }
    };
}

void
FeatureSource::dirty()
{
    if (_featuresCache)
    {
        _featuresCache->clear();
    }

    super::dirty();
}

osg::ref_ptr<FeatureCursor>
FeatureSource::createFeatureCursor(const Query& in_query, const FeatureFilterChain& post_filters, FilterContext* context, ProgressCallback* progress) const
{
    osg::ref_ptr<FeatureCursor> result;

    std::string cache_key;
    bool fromCache = false;

    FilterContext temp_cx;
    if (context)
        temp_cx = *context;

    if (temp_cx.featureProfile() == nullptr)
        temp_cx.setFeatureProfile(getFeatureProfile());

    // make a copy so we can override the buffer if necessary
    Query query = in_query;

    // clear a buffer that's set to zero units.
    if (query.buffer()->getValue() == 0.0)
        query.buffer().clear();

    // No buffer? Check the options on the layer itself:
    if (!query.buffer().isSet())
    {
        if (options().bufferWidth().isSet())
        {
            query.buffer() = options().bufferWidth().value();
        }
        else if (options().bufferWidthAsPercentage().isSet())
        {
            if (query.bounds().isSet())
            {
                double w = width(query.bounds().value());
                double h = height(query.bounds().value());
                double buffer = sqrt(w*h) * options().bufferWidthAsPercentage().value();
                query.buffer() = Distance(buffer, getFeatureProfile()->getSRS()->getUnits());
            }
            else if (query.tileKey().isSet())
            {
                double w = query.tileKey()->getExtent().width();
                double h = query.tileKey()->getExtent().height();
                double buffer = sqrt(w*h) * options().bufferWidthAsPercentage().value();
                query.buffer() = Distance(buffer, query.tileKey()->getProfile()->getSRS()->getUnits());
            }
            else
            {
                OE_WARN << LC << "Requested a buffer width as a percentage but no bounds or tilekey was set" << std::endl;
            }
        }
    }

    // TileKey path:
    if (query.tileKey().isSet())
    {
        if (!temp_cx.workingExtent().isSet())
        {
            temp_cx.setWorkingExtent(query.tileKey()->getExtent());
        }

        if (!result.valid())
        {
            std::set<TileKey> keys;

            getKeys(query.tileKey().value(), query.buffer().value(), keys);

            // Try reading from the cache first if we have a TileKey.
            if (_featuresCache)
            {
                for (auto& key : keys)
                    cache_key += key.str() + ',';

                auto cached_entry = _featuresCache->get(cache_key);

                if (cached_entry.has_value())
                {
                    FeatureList copy(cached_entry.value().size());
                    std::transform(cached_entry.value().begin(), cached_entry.value().end(), copy.begin(),
                        [&](auto& feature) { return new Feature(*feature); });
                    result = new FeatureListCursor(std::move(copy));
                    fromCache = true;
                }
            }

            if (!result.valid())
            {
                if (keys.size() == 1)
                {
                    Query query(*keys.begin());
                    osg::ref_ptr<FeatureCursor> cursor;
                    result = createPatchFeatureCursor(query, progress);
                    if (!result.valid())
                        result = createFeatureCursorImplementation(query, progress);
                }
                else
                {
                    osg::ref_ptr<MultiCursor> multi = new MultiCursor(progress);

                    // Query and collect all the features we need for this tile.
                    for (auto& sub_key : keys)
                    {
                        auto sub_cursor = createPatchFeatureCursor(Query(sub_key), progress);

                        if (!sub_cursor)
                            sub_cursor = createFeatureCursorImplementation(Query(sub_key), progress);

                        if (sub_cursor)
                            multi->_cursors.emplace_back(sub_cursor);

                        if (progress && progress->isCanceled())
                            return {};
                    }

                    if (multi->_cursors.empty())
                    {
                        return { };
                    }

                    multi->finish();
                    result = multi;
                }
            }
        }
    }

    else
    {
        //OE_SOFT_ASSERT(!query.buffer().isSet(), "Buffer not supported for non-tilekey queries; ignoring");

        if (!temp_cx.workingExtent().isSet() && _featureProfile.valid())
        {
            if (query.bounds().isSet())
                temp_cx.setWorkingExtent(GeoExtent(_featureProfile->getSRS(), query.bounds().get()));
            else
                temp_cx.setWorkingExtent(_featureProfile->getExtent());
        }

        result = createFeatureCursorImplementation(query, progress);
    }

    if (result.valid())
    {
        if (!fromCache)
        {
            if (!_filters.empty() ||
                options().fidAttribute().isSet() ||
                options().autoFID() == true ||
                query.tileKey().isSet() ||
                _featuresCache != nullptr)
            {
                FeatureList features;
                result->fill(features, [](const Feature* f) { return f != nullptr; });

                // doesn't really work...dunno why
                //CropFilter crop(CropFilter::METHOD_BBOX);
                //temp_cx = crop.push(features, temp_cx);

                // run the filters:
                if (!_filters.empty())
                {
                    temp_cx = _filters.push(features, temp_cx);
                }

                // apply a fid attribute:
                if (options().fidAttribute().isSet())
                {
                    for (auto& feature : features)
                    {
                        feature->setFID(feature->getInt(options().fidAttribute().get(), 0));
                    }
                }

                // apply and auto-fid:
                else if (options().autoFID() == true)
                {
                    static FeatureID generator = 0;
                    for (auto& feature : features)
                    {
                        feature->setFID(generator++);
                    }
                }

                // Insert the tile level for tiled features:
                if (query.tileKey().isSet())
                {
                    for (auto& feature : features)
                    {
                        feature->set(".tile_level", (std::int64_t)query.tileKey()->getLOD());
                    }
                }

                // Write the feature set to the L2 cache.
                // TODO: If we have a persistent cache, write to that as well here
                if (_featuresCache)
                {
                    // clone the list for caching:
                    FeatureList clone(features.size());
                    std::transform(features.begin(), features.end(), clone.begin(),
                        [&](auto& feature) { return new Feature(*feature); });

                    _featuresCache->insert(cache_key, clone);
                }

                result = new FeatureListCursor(std::move(features));
            }
        }

        // apply caller's filters. These are NOT cached by this class because the 
        // modifications are the resposibility of the caller.
        if (!post_filters.empty())
        {
            FeatureList features;
            result->fill(features);
            temp_cx = post_filters.push(features, temp_cx);
            result = new FeatureListCursor(std::move(features));
        }

        // copy out temporary context back out.
        if (context)
            *context = temp_cx;
    }

    //if (fromCache) hits += 1;
    //OE_INFO << LC << "cache hits = " << 100.0f * (hits / reads) << "%" << std::endl;
    
    return result;
}

unsigned
FeatureSource::getKeys(const TileKey& key, const Distance& buffer, std::set<TileKey>& output) const
{
    if (_featureProfile.valid())
    {
        unsigned firstLevel = _featureProfile->getFirstLevel();
        unsigned maxLevel = _featureProfile->getMaxLevel() >= 0u ? _featureProfile->getMaxLevel() : UINT_MAX;

        // We need to translate the caller's tilekey into feature source
        // tilekeys and combine multiple queries into one.
        const Profile* profile =
            _featureProfile->isTiled() ? _featureProfile->getTilingProfile() :
            key.getProfile();

        if (profile)
        {
            std::vector<TileKey> intersectingKeys;
            if (buffer.as(Units::METERS) == 0.0)
            {
                profile->getIntersectingTiles(key, intersectingKeys);
            }
            else
            {
                GeoExtent extent = key.getExtent();
                double d = extent.getSRS()->transformDistance(
                    buffer,
                    extent.getSRS()->getUnits(),
                    0.5*(extent.yMin() + extent.yMax()));
                //double d = buffer.asDistance(extent.getSRS()->getUnits(), 0.5*(extent.yMin() + extent.yMax()));
                extent.expand(d, d);
                unsigned lod = profile->getEquivalentLOD(key.getProfile(), key.getLOD());
                profile->getIntersectingTiles(extent, lod, intersectingKeys);
            }

            for (int i = 0; i < intersectingKeys.size(); ++i)
            {
                auto lod = intersectingKeys[i].getLOD();
                if (lod > maxLevel)
                {
                    // fall back to the max level if necessary:
                    output.insert(intersectingKeys[i].createAncestorKey(maxLevel));
                }
                else if (lod >= firstLevel)
                {
                    // must be at least the first available level
                    output.insert(intersectingKeys[i]);
                }
            }
        }
        else
        {
            // plan B
            if (key.getLOD() >= firstLevel && key.getLOD() <= maxLevel)
            {
                output.insert(key);
            }
        }
    }

    return output.size();
}

void
FeatureSource::addedToMap(const Map* map)
{
    for(auto& filter : _filters)
    {
        filter->addedToMap(map);
    }

    super::addedToMap(map);
}

void
FeatureSource::removedFromMap(const Map* map)
{
    super::removedFromMap(map);
}

Config
TiledFeatureSource::Options::getConfig() const
{
    auto conf = super::getConfig();
    conf.set("min_level", minLevel());
    conf.set("max_level", maxLevel());
    conf.set("layers", layers());
    patch().set(conf, "patch");
    return conf;
}

void
TiledFeatureSource::Options::fromConfig(const Config& conf)
{
    conf.get("min_level", minLevel());
    conf.get("max_level", maxLevel());
    conf.get("layers", layers());
    patch().get(conf, "patch");
}

Status
TiledFeatureSource::openImplementation()
{
    Status parent = super::openImplementation();
    if (parent.isError())
        return parent;

#if 0
    if (patch.isSet() && (patch.embeddedOptions() != nullptr))
    {
        auto* layer = patch.create(getReadOptions());
        if (layer && getFeatureProfile() && getFeatureProfile()->getTilingProfile())
        {
            layer->setMinLevel(getMinLevel());
            layer->setMaxLevel(getMaxLevel());
            layer->setFeatureProfile(getFeatureProfile());
            layer->setFIDAttribute(getFIDAttribute());
            layer->setGeoInterpolation(getGeoInterpolation());
            parent = layer->open(getReadOptions());

            if (parent.isError())
            {
                OE_WARN << LC << "Failed to open patch: " << parent.message() << std::endl;
            }
        }
    }
#endif

    if (parent.isError())
    {
        OE_WARN << LC << "Failed to open patch: " << parent.message() << std::endl;
    }


    return super::openImplementation();
}

void
TiledFeatureSource::addedToMap(const Map* map)
{
    options().patch().addedToMap(map);

    if (options().patch().isSet())
    {
        if (options().patch().getLayer() == nullptr)
        {
            OE_WARN << LC << "Patch layer was set, but the layer was not found in the Map." << std::endl;
        }
        else if (options().patch().getLayer()->getStatus().isError())
        {
            OE_WARN << LC << "Patch layer was set, but the layer failed to open: " << options().patch().getLayer()->getStatus().message() << std::endl;
        }
    }

    super::addedToMap(map);
}

Status
TiledFeatureSource::closeImplementation()
{
    return super::closeImplementation();
}

FeatureCursor*
TiledFeatureSource::createPatchFeatureCursor(const Query& query, ProgressCallback* progress) const
{
    // If we have a patch, create a cursor for it as well.
    if (options().patch().isOpen())
    {
        auto patch_cursor = options().patch().getLayer()->createFeatureCursor(query, {}, {}, progress);
        if (patch_cursor.valid() && patch_cursor->hasMore())
        {
            return patch_cursor.release();
        }
    }

    return {};
}

void
TiledFeatureSource::setPatchFeatureSource(TiledFeatureSource* tfs)
{
    OE_SOFT_ASSERT_AND_RETURN(tfs, void());

    options().patch().setLayer(tfs);
}

bool
TiledFeatureSource::hasTilePatch() const
{
    return options().patch().isOpen();
}

Status
TiledFeatureSource::insertPatch(const TileKey& key, const FeatureList& features, bool overwrite)
{
    if (hasTilePatch())
    {
        return options().patch().getLayer()->insert(key, features, overwrite);
    }

    return Status::ServiceUnavailable;
}

void
TiledFeatureSource::dirty()
{
    super::dirty();
    if (hasTilePatch())
    {
        options().patch().getLayer()->dirty();
    }
}

void
TiledFeatureSource::setMinLevel(int value) {
    options().minLevel() = value;
}
int
TiledFeatureSource::getMinLevel() const {
    return options().minLevel().value();
}

void
TiledFeatureSource::setMaxLevel(int value) {
    options().maxLevel() = value;
}
int
TiledFeatureSource::getMaxLevel() const {
    return options().maxLevel().value();
}
