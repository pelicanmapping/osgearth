add_osgearth_plugin(
    TARGET
        osgdb_osgearth_scriptengine_javascript_duktape
    HEADERS
        DuktapeEngine
        JSGeometry
        duktape.h
        duk_config.h 
    SOURCES
        Plugin.cpp
        DuktapeEngine.cpp
        duktape.c )

