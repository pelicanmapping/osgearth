/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/MemCache>
#include <algorithm>

using namespace osgEarth;

#define LC "[MemCacheBin] "

//#define CLONE_DATA

//------------------------------------------------------------------------

namespace
{
    struct MemCacheEntry
    {
        osg::ref_ptr<const osg::Object> object;
        Config metadata;
        TimeStamp written;
    };
    using MemCacheLRU = LRUCache<std::string, MemCacheEntry>;

    struct MemCacheBin : public CacheBin
    {
        MemCacheBin(const std::string& id, unsigned maxSize)
            : CacheBin(id, true),
            _lru(maxSize)
        {
            //nop
        }

        ReadResult readObject(const std::string& key, const osgDB::Options*) override
        {
            auto cached = _lru.get(key);

            // clone required since the cache is in memory

            if (cached.has_value())
            {
                const auto& entry = cached.value();
#ifdef CLONE_DATA
                ReadResult result(osg::clone(entry.object.get(), osg::CopyOp::DEEP_COPY_ALL), entry.metadata);
#else
                ReadResult result(const_cast<osg::Object*>(entry.object.get()), entry.metadata);
#endif
                result.setLastModifiedTime(entry.written);
                return result;
            }
            else
            {
                return ReadResult();
            }
        }

        ReadResult readImage(const std::string& key, const osgDB::Options* readOptions) override
        {
            return readObject(key, readOptions);
        }

        ReadResult readString(const std::string& key, const osgDB::Options* readOptions) override
        {
            return readObject(key, readOptions);
        }

        bool write( const std::string& key, const osg::Object* object, const Config& meta, const osgDB::Options* writeOptions) override
        {
            if ( object ) 
            {
#ifdef CLONE_DATA
                osg::ref_ptr<const osg::Object> cloned = osg::clone(object, osg::CopyOp::DEEP_COPY_ALL);
                _lru.insert( key, MemCacheEntry{ cloned.get(), meta, ::time(nullptr) } );
#else
                _lru.insert( key, MemCacheEntry{ object, meta, ::time(nullptr) } );
#endif
                return true;
            }
            else
                return false;
        }

        bool remove(const std::string& key) override
        {
            _lru.erase(key);
            return true;
        }

        bool touch(const std::string& key) override
        {
            return _lru.touch(key, [](MemCacheEntry& entry) { entry.written = ::time(nullptr); });
        }

        RecordStatus getRecordStatus( const std::string& key ) override
        {
            // Record status only reports existence; callers apply their own freshness policy.
            return _lru.get(key).has_value() ? STATUS_OK : STATUS_NOT_FOUND;
        }

        MemCacheLRU _lru;
    };
    

    static std::mutex s_defaultBinMutex;
}

//------------------------------------------------------------------------

MemCache::MemCache(unsigned maxBinSize) :
    _maxBinSize(std::max(maxBinSize, 1u))
{
    //nop
}

CacheBin*
MemCache::addBin( const std::string& binID )
{
    return _bins.getOrCreate( binID, new MemCacheBin(binID, _maxBinSize) );
}

CacheBin*
MemCache::getOrCreateDefaultBin()
{
    if ( !_defaultBin.valid() )
    {
        std::lock_guard<std::mutex> lock( s_defaultBinMutex );
        // double check
        if ( !_defaultBin.valid() )
        {
            _defaultBin = new MemCacheBin("__default", _maxBinSize);
        }
    }

    return _defaultBin.get();
}
