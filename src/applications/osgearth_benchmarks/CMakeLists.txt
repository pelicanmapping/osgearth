find_package(benchmark CONFIG REQUIRED)

set(TARGET_SRC
    main.cpp
    ChonkBenchmarks.cpp
    VirtualProgramBenchmarks.cpp
    VirtualProgramProfileBenchmarks.cpp
    CacheBenchmarks.cpp)

add_osgearth_app(
    TARGET osgearth_benchmarks
    SOURCES ${TARGET_SRC}
    LIBRARIES benchmark::benchmark benchmark::benchmark_main ${OPENGL_LIBRARIES}
    FOLDER Tests)

if(TARGET osgdb_osgearth_sky_simple)
    target_sources(osgearth_benchmarks PRIVATE SkyBenchmarks.cpp)
    add_dependencies(osgearth_benchmarks osgdb_osgearth_sky_simple)
endif()
