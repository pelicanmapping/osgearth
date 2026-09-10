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
