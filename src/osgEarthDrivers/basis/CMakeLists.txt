find_package(basisu CONFIG QUIET)

if(basisu_FOUND)
    # .basis reader
    add_osgearth_plugin(
        TARGET osgdb_basis
        SOURCES ReaderWriterBasis.cpp)
    # The vcpkg package includes the transcoder in the encoder library and
    # propagates its dependencies (including zstd) through this imported target.
    target_link_libraries(osgdb_basis PRIVATE basisu::basisu_encoder)
endif()
