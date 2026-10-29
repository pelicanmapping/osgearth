# Build only the pinned SDK core. Its optional backends, tools, and Python are
# unnecessary for FeatureImageLayer; no C++20 usage requirements reach clients.
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
string(CONCAT _slughorn_sha512
    "4fe83c8a211ac72989375de10d4d918b2c24f27ba1d8eb7a5a5fd0f3d1ce40a4"
    "0ca1fcfa0fb55d0c09dbe32e246607790ffc7dc0b7d6b6d5907edb40e27260b0")
FetchContent_Declare(osgearth_slughorn
    URL https://codeload.github.com/AlphaPixel/slughorn/tar.gz/1b203c191bd152c20685f8a896d22dcff974cec4
    URL_HASH SHA512=${_slughorn_sha512}
    SOURCE_SUBDIR osgearth-unused)
FetchContent_MakeAvailable(osgearth_slughorn)

add_library(osgEarthSlughorn STATIC "${osgearth_slughorn_SOURCE_DIR}/slughorn/slughorn.cpp")
target_compile_features(osgEarthSlughorn PRIVATE cxx_std_20)
set_target_properties(osgEarthSlughorn PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_EXTENSIONS OFF)
# A static osgEarth SDK needs this archive too, but never the SDK headers.
install(TARGETS osgEarthSlughorn EXPORT osgEarthTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES "${osgearth_slughorn_SOURCE_DIR}/LICENSE"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/osgearth/licenses RENAME Slughorn-LICENSE)
