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
