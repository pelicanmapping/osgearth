set(TARGET_SRC
    main.cpp
    CacheTests.cpp
    ChonkTests.cpp
    ConfigTests.cpp
    CoverageTests.cpp
    EndianTests.cpp
    ExpressionTests.cpp
    GeoExtentTests.cpp
    FeatureTests.cpp
    ImageUtilsTests.cpp
    PathTests.cpp
    PBRMaterialTests.cpp
    ImageLayerTests.cpp
    SpatialReferenceTests.cpp
    StringUtilsTests.cpp
    ThreadingTests.cpp
    VirtualProgramTests.cpp)

add_osgearth_app(
    TARGET osgearth_tests
    SOURCES ${TARGET_SRC}
    LIBRARIES ${OPENGL_LIBRARIES}
    FOLDER Tests)

if(OSGEARTH_BUILD_IMGUI_NODEKIT)
    target_sources(osgearth_tests PRIVATE ImGuiTests.cpp)
    target_link_libraries(osgearth_tests PRIVATE osgEarthImGui)
endif()
