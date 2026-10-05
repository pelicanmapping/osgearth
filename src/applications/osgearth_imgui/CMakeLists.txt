add_osgearth_app(
    TARGET osgearth_imgui
    SOURCES osgearth_imgui.cpp
    LIBRARIES osgEarthImGui
    FOLDER Tools )

if(OSGEARTH_BUILD_PROCEDURAL_NODEKIT)
    target_link_libraries(osgearth_imgui PRIVATE osgEarthProcedural)
endif()

if(OSGEARTH_BUILD_PRESTIGE_NODEKIT)
    target_link_libraries(osgearth_imgui PRIVATE osgEarthPrestige)
endif()

if(OSGEARTH_BUILD_CESIUM_NODEKIT)
    target_link_libraries(osgearth_imgui PRIVATE osgEarthCesium)
endif()
