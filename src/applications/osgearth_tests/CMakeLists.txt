set(TARGET_SRC
    main.cpp
    CacheTests.cpp
    ChonkTests.cpp
    ChonkRenderPassTests.cpp
    ChonkPageTests.cpp
    ChonkMaterialTests.cpp
    CompressedURITests.cpp
    ConfigTests.cpp
    CoverageTests.cpp
    EndianTests.cpp
    ElevationPoolTests.cpp
    ExpressionTests.cpp
    GeoExtentTests.cpp
    FeatureTests.cpp
    ImageUtilsTests.cpp
    PathTests.cpp
    PagedNodeTests.cpp
    PBRMaterialTests.cpp
    MaterialTexturePreparationTests.cpp
    ImageLayerTests.cpp
    SpatialReferenceTests.cpp
    StringUtilsTests.cpp
    ThreadingTests.cpp
    TextureArenaTests.cpp
    TextureArenaUploadTests.cpp
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
