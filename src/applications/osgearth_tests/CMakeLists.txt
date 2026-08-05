set(TARGET_SRC
    main.cpp
    CacheTests.cpp
    CoverageTests.cpp
    EndianTests.cpp
    ExpressionTests.cpp
    GeoExtentTests.cpp
    GLTFReaderTests.cpp
    FeatureTests.cpp
    ImageUtilsTests.cpp
    KitTests.cpp
    PathTests.cpp
    ImageLayerTests.cpp
    SpatialReferenceTests.cpp
    ThreadingTests.cpp)

add_osgearth_app(
    TARGET osgearth_tests
    SOURCES ${TARGET_SRC}
    FOLDER Tests)

target_include_directories(osgearth_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/osgEarthDrivers/gltf
    ${OSGEARTH_EMBEDDED_THIRD_PARTY_DIR}/tinygltf)
