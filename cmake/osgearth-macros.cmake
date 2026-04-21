# .................................................................
# Create a library for either an osgdb plugin or an osgearth plugin.
#
# example usage:
#
#   add_osgearth_plugin(
#       TARGET [name of plugin with 'osgdb_' or 'osgdb_osgearth_' prefix, REQUIRED]
#       SOURCES [list of source files, REQUIRED]
#       HEADERS [list of header files to include in the project]
#       PUBLIC_HEADERS [list of header files to install]
#       TEMPLATES [list of cmake .in files]
#       SHADERS [list of GLSL shader files]
#       LIBRARIES [list of additional libraries to link with]
#       INCLUDE_DIRECTORIES [list of additional include folders to use]
#       FOLDER [name of IDE folder in which to place target]
#
# Note: osgEarth and OpenScenegraph libraries will automatically link to all plugins.
#
macro(add_osgearth_plugin)

    set(options "")
    set(oneValueArgs TARGET)
    set(multiValueArgs SOURCES HEADERS PUBLIC_HEADERS SHADERS TEMPLATES LIBRARIES INCLUDE_DIRECTORIES)
    cmake_parse_arguments(MY "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    set(ALL_HEADERS ${MY_HEADERS} ${MY_PUBLIC_HEADERS})
    
    include_directories(${MY_INCLUDE_DIRECTORIES})
    
    if(OSGEARTH_BUILD_SHARED_LIBS)
        add_library(${MY_TARGET} MODULE ${MY_SOURCES} ${ALL_HEADERS} ${MY_SHADERS} ${MY_TEMPLATES})
    else()
        add_library(${MY_TARGET} STATIC ${MY_SOURCES} ${ALL_HEADERS} ${MY_SHADERS} ${MY_TEMPLATES})
    endif()

    if (${MY_TARGET} MATCHES "^osgdb_osgearth_")
        string(SUBSTRING ${MY_TARGET} 15 -1 MY_TARGET_SUFFIX)
        set_target_properties(${MY_TARGET} PROPERTIES PROJECT_LABEL "osgEarth ${MY_TARGET_SUFFIX}")
    elseif(${MY_TARGET} MATCHES "^osgdb_")
        string(SUBSTRING ${MY_TARGET} 6 -1 MY_TARGET_SUFFIX)
        set_target_properties(${MY_TARGET} PROPERTIES PROJECT_LABEL "osg ${MY_TARGET_SUFFIX}")
    else()
        message(FATAL_ERROR "add_library_as_plugin: name '${MY_TARGET}' must begin with 'osgdb_' or 'osgdb_osgearth_'")
    endif()
    
    if (WIN32 AND MINGW)
        set_target_properties(${MY_TARGET} PROPERTIES PREFIX "mingw_")
    else()
        # no prefixes on plugin library files, please.
        # without this, plugins will have names like "libosgdb_osgearth.so" and OSG doesn't want the "lib"
        set_target_properties(${MY_TARGET} PROPERTIES PREFIX "")
    endif()
    
    # soversions - append SO version to shared object files on unix (e.g., osgearth.so.123)
    if (NOT APPLE AND OSGEARTH_SONAMES)
        set_target_properties(${MY_TARGET} PROPERTIES VERSION ${OSGEARTH_VERSION} SOVERSION ${OSGEARTH_SOVERSION})
    endif()
   
    # install the shader source files, if requested:
    if(OSGEARTH_INSTALL_SHADERS)
        install(FILES ${MY_SHADERS} DESTINATION ${OSGEARTH_INSTALL_DATADIR}/shaders)
    endif()

    # macos-specific
    if(OSG_BUILD_PLATFORM_IPHONE)
        set_target_properties(${TARGET_TARGETNAME} PROPERTIES XCODE_ATTRIBUTE_ENABLE_BITCODE ${IPHONE_ENABLE_BITCODE})
    endif()
    
    # install the dynamic libraries.
    install(TARGETS ${MY_TARGET}
        LIBRARY DESTINATION ${OSGEARTH_INSTALL_PLUGINSDIR}/${OSG_PLUGINS})
    
    # organize the files in the IDE
    source_group( "Header Files"   FILES ${ALL_HEADERS} )
    source_group( "Shader Files"   FILES ${MY_SHADERS} )
    source_group( "Template Files" FILES ${MY_TEMPLATES} )
    set_property(TARGET ${MY_TARGET} PROPERTY FOLDER "Plugins")
    
    # We always need these so just link them here.
    target_link_libraries(${MY_TARGET} PRIVATE osgEarth ${OPENSCENEGRAPH_LIBRARIES} ${MY_LIBRARIES})

    if(SuperluminalAPI_FOUND)
        #message(STATUS "Found SuperLuminalAPI.")
        target_link_libraries(${MY_TARGET} PRIVATE SuperluminalAPI)    
    endif()

    # Install any public headers
    if (MY_PUBLIC_HEADERS)
        install(FILES ${MY_PUBLIC_HEADERS} DESTINATION include/osgEarthDrivers/${MY_TARGET_SUFFIX})
    endif()

endmacro()





# ..........................................................
# Create a library for an osgEarth executable application.
#
# example usage:
#
#   add_osgearth_app(
#       TARGET [name of the application, REQUIRED]
#       SOURCES [list of source files, REQUIRED]
#       HEADERS [list of header files to include in the project]
#       TEMPLATES [list of cmake .in files]
#       SHADERS [list of GLSL shader files]
#       LIBRARIES [list of additional libraries to link with]
#       INCLUDE_DIRECTORIES [list of additional include folders to use]
#       FOLDER [name of IDE folder in which to place target]
#
# Note: osgEarth and OpenScenegraph libraries will automatically link to all apps.
#
macro(add_osgearth_app)
    set(options USE_IMGUI)
    set(oneValueArgs TARGET FOLDER)
    set(multiValueArgs SOURCES HEADERS SHADERS TEMPLATES LIBRARIES INCLUDE_DIRECTORIES)
    cmake_parse_arguments(MY "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    set(ALL_HEADERS ${MY_HEADERS})
    
    include_directories(${MY_INCLUDE_DIRECTORIES})

    add_executable(${MY_TARGET} ${MY_SOURCES} ${ALL_HEADERS} ${MY_SHADERS} ${MY_TEMPLATES})
        
    # We always need these so just link them here.
    target_link_libraries(${MY_TARGET} PRIVATE osgEarth ${OPENSCENEGRAPH_LIBRARIES} ${MY_LIBRARIES})

    if(SuperluminalAPI_FOUND)
        #message(STATUS "Found SuperLuminalAPI.")
        target_link_libraries(${MY_TARGET} PRIVATE SuperluminalAPI)    
    endif()
    
    set_target_properties(${MY_TARGET} PROPERTIES PROJECT_LABEL "${MY_TARGET}")
    
    # macos-specific
    if(OSG_BUILD_PLATFORM_IPHONE)
        set_target_properties(${TARGET_TARGETNAME} PROPERTIES XCODE_ATTRIBUTE_ENABLE_BITCODE ${IPHONE_ENABLE_BITCODE})
    endif()

    install(TARGETS ${MY_TARGET})

    if(NOT MY_FOLDER)
        set(MY_FOLDER "Ungrouped")
    endif()

    set_target_properties(${MY_TARGET} PROPERTIES
        FOLDER "${MY_FOLDER}"
        DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}"
        RELEASE_POSTFIX "${CMAKE_RELEASE_POSTFIX}"
        RELWITHDEBINFO_POSTFIX "${CMAKE_RELWITHDEBINFO_POSTFIX}"
        MINSIZEREL_POSTFIX "${CMAKE_MINSIZEREL_POSTFIX}"
    )

endmacro(add_osgearth_app)



# ...........................................................
# Adds the targets for an osgEarth library.
#
# example usage:
#
#   add_osgearth_library(
#       TARGET [name of the application]
#       STATIC
#       HEADERS [list of private header files to include in the project]
#       PUBLIC_HEADERS [list of public headers to be installed]
#       SOURCES [list of source files]
#       TEMPLATES [list of cmake .in files]
#       SHADERS [list of GLSL shader files]
#       LIBRARIES [list of additional private libraries to link with]
#       PUBLIC_LIBRARIES [list of additional public libraries to link with]
#       INCLUDE_DIRECTORIES [list of additional include folders to use]
#       FOLDER [name of IDE folder in which to place target]
#
# Note: osgEarth and OpenScenegraph libraries will automatically link to all apps.
#
macro(add_osgearth_library)

    set(options "")
    set(oneValueArgs TARGET FOLDER STATIC)
    set(multiValueArgs SOURCES HEADERS PUBLIC_HEADERS SHADERS TEMPLATES LIBRARIES PUBLIC_LIBRARIES INCLUDE_DIRECTORIES)
    cmake_parse_arguments(MY "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    set(INSTALL_INCLUDE_FOLDER ${CMAKE_INSTALL_INCLUDEDIR}/${MY_TARGET})

    set(ALL_HEADERS ${MY_HEADERS} ${MY_IMGUI_HEADERS} ${MY_PUBLIC_HEADERS})
    include_directories(${MY_INCLUDE_DIRECTORIES})        

    # IDE setup
    source_group("Sources" FILES ${MY_SOURCES})
    source_group("Headers" FILES ${ALL_HEADERS})
    source_group("Shaders" FILES ${MY_SHADERS} )
    source_group("Templates" FILES ${MY_TEMPLATES} )

    if(NOT MY_STATIC)
        set(MY_STATIC ${OSGEARTH_DYNAMIC_OR_STATIC})
    endif()

    # create the library.
    add_library(
        ${MY_TARGET}
        ${MY_STATIC}
        ${ALL_HEADERS}
        ${MY_SOURCES}
        ${MY_SHADERS}
        ${MY_TEMPLATES} )

    # Link:
    if(NOT "${MY_TARGET}" STREQUAL "osgEarth")
        target_link_libraries(${MY_TARGET} PRIVATE osgEarth ${OPENSCENEGRAPH_LIBRARIES} ${MY_LIBRARIES})
    else()
        target_link_libraries(${MY_TARGET} PRIVATE ${OPENSCENEGRAPH_LIBRARIES} ${MY_LIBRARIES})
    endif()

    if(MY_PUBLIC_LIBRARIES)
        target_link_libraries(${MY_TARGET} PUBLIC ${MY_PUBLIC_LIBRARIES})
    endif()
    
    if(Tracy_FOUND)
        target_link_libraries(${MY_TARGET} PUBLIC Tracy::TracyClient)
    endif()

    if(SuperluminalAPI_FOUND)
        #message(STATUS "Found SuperLuminalAPI.")
        target_link_libraries(${MY_TARGET} PRIVATE SuperluminalAPI)    
    endif()
    
    # soversions - append SO version to shared object files on unix (e.g., osgearth.so.123)
    if (OSGEARTH_SONAMES)
        set_target_properties(${MY_TARGET} PROPERTIES VERSION ${OSGEARTH_VERSION} SOVERSION ${OSGEARTH_SOVERSION})
    endif()

    # library install and target exports for the cmake config packaging.
    install(
        TARGETS ${MY_TARGET}
        EXPORT ${MY_TARGET}Targets
        INCLUDES DESTINATION include)

    # deploy the shaders for this library, if requested.
    if(OSGEARTH_INSTALL_SHADERS)
        install(
            FILES ${MY_SHADERS}
            DESTINATION ${OSGEARTH_INSTALL_DATADIR}/shaders )
    endif()
    
    # on Windows, install pdb files
    # https://stackoverflow.com/a/40860436/4218920
    if(OSGEARTH_INSTALL_PDBS)
        install(
            FILES $<TARGET_PDB_FILE:${MY_TARGET}>
            DESTINATION ${CMAKE_INSTALL_LIBDIR} OPTIONAL)
    endif()


    # Public Header File install:
    IF(NOT OSGEARTH_BUILD_FRAMEWORKS)
        # normal path to install the public header files:
        install(
            FILES ${MY_PUBLIC_HEADERS}
            DESTINATION ${INSTALL_INCLUDE_FOLDER})
    else()
        # macos-specific
        if(OSG_BUILD_PLATFORM_IPHONE)
            set_target_properties(${TARGET_TARGETNAME} PROPERTIES XCODE_ATTRIBUTE_ENABLE_BITCODE ${IPHONE_ENABLE_BITCODE})
        endif()
    
        set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
        set(CMAKE_INSTALL_RPATH "${OSGEARTH_BUILD_FRAMEWORKS_INSTALL_NAME_DIR}")
        
        set_target_properties(${LIB_NAME} PROPERTIES
             FRAMEWORK TRUE
             FRAMEWORK_VERSION ${OSGEARTH_MAJOR_VERSION}
             PUBLIC_HEADER "${MY_PUBLIC_HEADERS}"
             INSTALL_NAME_DIR "${OSGEARTH_BUILD_FRAMEWORKS_INSTALL_NAME_DIR}"
        )
    endif()
        
    # generate and install a packaging target for this library:
    osgearth_package_install_library_target(${MY_TARGET})
    
    # IDE location:
    if (MY_FOLDER)
        set_property(TARGET ${MY_TARGET} PROPERTY FOLDER "${MY_FOLDER}")
    endif()
    
endmacro()




# -----------------------------------------------------------------------
# configure_shaders -gw
#
# Bakes GLSL shaders to make into a CPP file at runtime.
# Example:
#
#   configure_shaders( MyTemplate.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/AutoGen.cpp file1.glsl file2.glsl )
#
macro(configure_shaders templateFile autoGenCppFile)

	# set up configure variables:
	set(TEMPLATE_FILE   ${templateFile} )
	set(GLSL_FILES      ${ARGN} )
	set(OUTPUT_CPP_FILE ${autoGenCppFile})
	
	# generate the build-time script that will create out cpp file with inline shaders:
	configure_file(
	    "${PROJECT_SOURCE_DIR}/cmake/ConfigureShaders.cmake.in"
	    "${CMAKE_CURRENT_BINARY_DIR}/ConfigureShaders.cmake"
	    @ONLY)
	
	# add the custom build-time command to run the script:
	add_custom_command(
	    OUTPUT
	        "${autoGenCppFile}"
	    COMMAND
	        "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/ConfigureShaders.cmake"
	    DEPENDS
	        ${GLSL_FILES}
	        "${TEMPLATE_FILE}"
	        "${PROJECT_SOURCE_DIR}/cmake/ConfigureShaders.cmake.in" )

endmacro(configure_shaders)


# -----------------------------------------------------------------------
#  detect_osg_version
#
#  Detect OpenSceneGraph version and set variables accordingly:
#
#    OSG_INCLUDE_DIR (if not already set)
#    OPENSCENEGRAPH_VERSION
#    OSG_PLUGINS
#
macro(detect_osg_version)

    # Fall back to OSG_DIR if OSG_INCLUDE_DIR is not defined
    if(OSG_DIR AND NOT OSG_INCLUDE_DIR AND EXISTS "${OSG_DIR}/include/osg/Version")
        set(OSG_INCLUDE_DIR "${OSG_DIR}/include")
    endif()
    option(APPEND_OPENSCENEGRAPH_VERSION "Append the OSG version number to the osgPlugins directory" ON)

    # Try to ascertain the version...
    # (Taken from CMake's FindOpenSceneGraph.cmake)
    if(OSG_INCLUDE_DIR)
        if(OpenSceneGraph_DEBUG)
            message(STATUS "[ FindOpenSceneGraph.cmake:${CMAKE_CURRENT_LIST_LINE} ] "
                "Detected OSG_INCLUDE_DIR = ${OSG_INCLUDE_DIR}")
        endif()

        set(_osg_Version_file "${OSG_INCLUDE_DIR}/osg/Version")
        if("${OSG_INCLUDE_DIR}" MATCHES "\\.framework$" AND NOT EXISTS "${_osg_Version_file}")
            set(_osg_Version_file "${OSG_INCLUDE_DIR}/Headers/Version")
        endif()

        if(EXISTS "${_osg_Version_file}")
          file(STRINGS "${_osg_Version_file}" _osg_Version_contents
               REGEX "#define (OSG_VERSION_[A-Z]+|OPENSCENEGRAPH_[A-Z]+_VERSION)[ \t]+[0-9]+")
        else()
          set(_osg_Version_contents "unknown")
        endif()

        string(REGEX MATCH ".*#define OSG_VERSION_MAJOR[ \t]+[0-9]+.*"
            _osg_old_defines "${_osg_Version_contents}")
        string(REGEX MATCH ".*#define OPENSCENEGRAPH_MAJOR_VERSION[ \t]+[0-9]+.*"
            _osg_new_defines "${_osg_Version_contents}")
        if(_osg_old_defines)
            string(REGEX REPLACE ".*#define OSG_VERSION_MAJOR[ \t]+([0-9]+).*"
                "\\1" _osg_VERSION_MAJOR ${_osg_Version_contents})
            string(REGEX REPLACE ".*#define OSG_VERSION_MINOR[ \t]+([0-9]+).*"
                "\\1" _osg_VERSION_MINOR ${_osg_Version_contents})
            string(REGEX REPLACE ".*#define OSG_VERSION_PATCH[ \t]+([0-9]+).*"
                "\\1" _osg_VERSION_PATCH ${_osg_Version_contents})
        elseif(_osg_new_defines)
            string(REGEX REPLACE ".*#define OPENSCENEGRAPH_MAJOR_VERSION[ \t]+([0-9]+).*"
                "\\1" _osg_VERSION_MAJOR ${_osg_Version_contents})
            string(REGEX REPLACE ".*#define OPENSCENEGRAPH_MINOR_VERSION[ \t]+([0-9]+).*"
                "\\1" _osg_VERSION_MINOR ${_osg_Version_contents})
            string(REGEX REPLACE ".*#define OPENSCENEGRAPH_PATCH_VERSION[ \t]+([0-9]+).*"
                "\\1" _osg_VERSION_PATCH ${_osg_Version_contents})
        else()
            message(WARNING "[ FindOpenSceneGraph.cmake:${CMAKE_CURRENT_LIST_LINE} ] "
                "Failed to parse version number, please report this as a bug")
        endif()
        unset(_osg_Version_contents)

        set(OPENSCENEGRAPH_VERSION "${_osg_VERSION_MAJOR}.${_osg_VERSION_MINOR}.${_osg_VERSION_PATCH}"
                                    CACHE INTERNAL "The version of OSG which was detected")
        if(OpenSceneGraph_DEBUG)
            message(STATUS "[ FindOpenSceneGraph.cmake:${CMAKE_CURRENT_LIST_LINE} ] "
                "Detected version ${OPENSCENEGRAPH_VERSION}")
        endif()
    endif()
	mark_as_advanced(OPENSCENEGRAPH_VERSION)


    if(APPEND_OPENSCENEGRAPH_VERSION AND OPENSCENEGRAPH_VERSION)
        SET(OSG_PLUGINS "osgPlugins-${OPENSCENEGRAPH_VERSION}"  CACHE STRING "" FORCE)
        #MESSAGE(STATUS "Plugins will be installed under osgPlugins-${OPENSCENEGRAPH_VERSION} directory.")
	else()
		SET(OSG_PLUGINS  CACHE STRING "" FORCE)
    endif()
	mark_as_advanced(OSG_PLUGINS)

endmacro(detect_osg_version)
