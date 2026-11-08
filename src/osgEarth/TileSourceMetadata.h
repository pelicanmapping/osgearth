/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#pragma once

#include <osgEarth/TileLayer>

namespace osgEarth { namespace detail
{
    // Read source geometry without opening a dataset. Older caches store it with the map-profile tile bin.
    inline bool readSourceMetadata(
        CacheSettings* settings, const osgDB::Options* options, const char* key,
        osg::ref_ptr<const Profile>& profile, DataExtentList& extents, unsigned* tileSize = nullptr)
    {
        if (!settings || !settings->isCacheEnabled() || !settings->getCacheBin())
            return false;

        auto* bin = settings->getCacheBin();
        ReadResult record;
        if (key)
            record = bin->readString(key, options);
        if (!record.succeeded() && options)
        {
            auto signature = options->getPluginStringData("osgEarth.mapProfileSignature");
            if (!signature.empty())
                record = bin->readString(signature + "_metadata", options);
        }

        const auto& policy = settings->cachePolicy().get();
        Config conf;
        if (!record.succeeded() || (!policy.isCacheOnly() && policy.isExpired(record.lastModifiedTime())) ||
            !conf.fromJSON(record.getString()))
            return false;

        TileLayer::CacheBinMetadata metadata(conf);
        if (!metadata.isOK() || metadata._sourceTileSize.get() <= 0)
            return false;
        osg::ref_ptr<const Profile> cached = Profile::create(metadata._sourceProfile.get());
        if (!cached.valid())
            return false;

        for (const auto& extent : metadata._dataExtents)
            if (!extent.isValid())
                return false;
        if (tileSize)
            *tileSize = metadata._sourceTileSize.get();
        profile = cached;
        extents.swap(metadata._dataExtents);
        return true;
    }

    // Store source discovery results independently of the profile in which tiles will later be requested.
    inline void writeSourceMetadata(
        CacheSettings* settings, const osgDB::Options* options, const char* key,
        const Profile* profile, unsigned tileSize, const DataExtentList& extents)
    {
        if (!settings || !profile || !settings->isCacheEnabled() ||
            !settings->cachePolicy()->isCacheWriteable() || !settings->getCacheBin())
            return;

        TileLayer::CacheBinMetadata metadata;
        metadata._sourceProfile = profile->toProfileOptions();
        metadata._cacheProfile = metadata._sourceProfile;
        metadata._sourceTileSize = tileSize;
        metadata._dataExtents = extents;
        osg::ref_ptr<StringObject> json = new StringObject(metadata.getConfig().toJSON());
        settings->getCacheBin()->write(key, json.get(), options);
    }
} }
