find_package(RocksDB QUIET)

if(RocksDB_FOUND)

    message(STATUS "Found rocksdb")

    add_osgearth_plugin(
        TARGET 
            osgdb_osgearth_cache_rocksdb
        PUBLIC_HEADERS
            RocksDBCacheOptions
        HEADERS
            RocksDBCache
            RocksDBCacheBin
            Tracker
        SOURCES 
            RocksDBCache.cpp
            RocksDBCacheBin.cpp
            RocksDBCacheDriver.cpp )
            
    target_link_libraries(osgdb_osgearth_cache_rocksdb
        PRIVATE RocksDB::rocksdb)    
        
endif()
