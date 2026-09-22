set(TARGET_SRC
    main.cpp
    CacheTests.cpp
    ChonkTests.cpp
    ChonkRenderPassTests.cpp
    ChonkPageTests.cpp
    ChonkMaterialTests.cpp
    ConfigTests.cpp
    CoverageTests.cpp
    EndianTests.cpp
    ExpressionTests.cpp
    GeoExtentTests.cpp
    FeatureTests.cpp
    ImageUtilsTests.cpp
    PathTests.cpp
    PBRMaterialTests.cpp
    SkyNode2Tests.cpp
    CloudLayerTests.cpp
    CloudRayReferenceTests.cpp
    ShadowTests.cpp
    Shadow46Tests.cpp
    ImageLayerTests.cpp
    SpatialReferenceTests.cpp
    StringUtilsTests.cpp
    ThreadingTests.cpp
    TextureArenaTests.cpp
    VirtualProgramTests.cpp)

add_osgearth_app(
    TARGET osgearth_tests
    SOURCES ${TARGET_SRC}
    LIBRARIES ${OPENGL_LIBRARIES}
    FOLDER Tests)

if(TARGET osgdb_osgearth_sky_simple)
    target_sources(osgearth_tests PRIVATE SkyEnvironmentTests.cpp)
    add_dependencies(osgearth_tests osgdb_osgearth_sky_simple)
endif()

if(OSGEARTH_BUILD_IMGUI_NODEKIT)
    target_sources(osgearth_tests PRIVATE ImGuiTests.cpp)
    target_link_libraries(osgearth_tests PRIVATE osgEarthImGui)
endif()
