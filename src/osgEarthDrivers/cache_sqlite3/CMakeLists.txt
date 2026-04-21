find_package(SQLite3 QUIET)

if(SQLite3_FOUND)

    add_osgearth_plugin(
        TARGET osgdb_osgearth_cache_sqlite3
        SOURCES
            SQLite3Cache.cpp
        PUBLIC_HEADERS
            SQLite3Cache)

    target_include_directories(osgdb_osgearth_cache_sqlite3 PRIVATE ${SQLite3_INCLUDE_DIRS})
    target_link_libraries(osgdb_osgearth_cache_sqlite3 PRIVATE ${SQLite3_LIBRARIES})

endif()
