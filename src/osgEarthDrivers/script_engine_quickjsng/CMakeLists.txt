include(qjs.cmake)

set(MY_TARGET osgdb_osgearth_scriptengine_javascript_qjs)

add_osgearth_plugin(
    TARGET
        ${MY_TARGET}
    HEADERS
        QuickJSNGEngine            
    SOURCES
        QuickJSNGEngine.cpp
        Plugin.cpp
        cutils.c dtoa.c libregexp.c libunicode.c quickjs.c)
        
target_compile_definitions(${MY_TARGET} PRIVATE ${qjs_defines})
target_link_libraries(${MY_TARGET} PRIVATE ${qjs_libs})