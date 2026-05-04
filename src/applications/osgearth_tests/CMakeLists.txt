set(TARGET_SRC
    main.cpp
    CacheTests.cpp
    EndianTests.cpp
    ExpressionTests.cpp
    GeoExtentTests.cpp
    FeatureTests.cpp
    ImageUtilsTests.cpp
    PathTests.cpp
    ImageLayerTests.cpp
    SpatialReferenceTests.cpp
    ThreadingTests.cpp)

add_osgearth_app(
    TARGET osgearth_tests
    SOURCES ${TARGET_SRC}
    FOLDER Tests)
