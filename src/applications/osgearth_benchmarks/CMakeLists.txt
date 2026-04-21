find_package(benchmark CONFIG REQUIRED)

set(TARGET_SRC
    main.cpp)

add_osgearth_app(
    TARGET osgearth_benchmarks
    SOURCES ${TARGET_SRC}
    LIBRARIES benchmark::benchmark benchmark::benchmark_main
    FOLDER Tests)
