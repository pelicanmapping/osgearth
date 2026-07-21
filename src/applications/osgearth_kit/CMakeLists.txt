add_osgearth_app(
    TARGET osgearth_kit
    SOURCES osgearth_kit.cpp
    LIBRARIES osgEarthImGui ${OSGPARTICLE_LIBRARY}
    FOLDER Examples)

if(OSGEARTH_BUILD_PROCEDURAL_NODEKIT)
    target_link_libraries(osgearth_kit PRIVATE osgEarthProcedural)
endif()

if(OSGEARTH_BUILD_CESIUM_NODEKIT)
    target_link_libraries(osgearth_kit PRIVATE osgEarthCesium)
endif()
