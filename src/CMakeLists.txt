
# A list to collect all libraries for packaging:
set(OSGEARTH_COMPONENTS)
list(APPEND OSGEARTH_COMPONENTS osgEarth)

# A list of public-facing dependencies for the packaging targets
set(OSGEARTH_PUBLIC_DEPENDENCIES)

# For a static build, we set the define OSGEARTH_STATIC_LIBRARY
# for both building the osgEarth.lib AND for building consumer libraries.
if(NOT OSGEARTH_BUILD_SHARED_LIBS)
    add_definitions(-DOSGEARTH_LIBRARY_STATIC)
endif()

if(OSGEARTH_BUILD_IMGUI_NODEKIT)
    add_subdirectory(osgEarthImGui)
    list(APPEND OSGEARTH_COMPONENTS osgEarthImGui)
    list(APPEND OSGEARTH_PUBLIC_DEPENDENCIES OpenGL)
endif()

if(OSGEARTH_BUILD_PROCEDURAL_NODEKIT)
    add_subdirectory(osgEarthProcedural)
    list(APPEND OSGEARTH_COMPONENTS osgEarthProcedural)
endif()

if(OSGEARTH_BUILD_PROCEDURAL2_NODEKIT)
    add_subdirectory(osgEarthProcedural2)
    list(APPEND OSGEARTH_COMPONENTS osgEarthProcedural2)
endif()

if(OSGEARTH_BUILD_CESIUM_NODEKIT)
    add_subdirectory(osgEarthCesium)
    list(APPEND OSGEARTH_COMPONENTS osgEarthCesium)
endif()

if(OSGEARTH_BUILD_TRITON_NODEKIT)
    add_subdirectory(osgEarthTriton)
    list(APPEND OSGEARTH_COMPONENTS osgEarthTriton)
endif()

if(OSGEARTH_BUILD_SILVERLINING_NODEKIT)
    add_subdirectory(osgEarthSilverLining)
    list(APPEND OSGEARTH_COMPONENTS osgEarthSilverLining)
endif()

if(OSGEARTH_BUILD_LEGACY_SPLAT_NODEKIT)
    add_subdirectory(osgEarthSplat)
    list(APPEND OSGEARTH_COMPONENTS osgEarthSplat)
endif()

# plugins:
add_subdirectory(osgEarthDrivers)

# apps and tools:
if(OSGEARTH_BUILD_TOOLS OR OSGEARTH_BUILD_EXAMPLES)
    add_subdirectory(applications)
endif()

# Do this LAST since we are generating the BuildConfig header
# based on everything that's happened so far
add_subdirectory(osgEarth)
set_property(TARGET osgEarth PROPERTY FOLDER "Core")

# Finally, generate and install the packaging config file (osgEarth-config.cmake)
osgearth_package_install_config_files()
