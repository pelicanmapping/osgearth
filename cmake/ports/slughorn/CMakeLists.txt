cmake_minimum_required(VERSION 3.20)
project(SlughornCore LANGUAGES CXX)
include(GNUInstallDirs)

# Upstream's standalone project also requires Python and its submodules.
add_library(Slughorn STATIC "${SLUGHORN_SOURCE_DIR}/slughorn/slughorn.cpp")
add_library(Slughorn::Slughorn ALIAS Slughorn)
target_compile_features(Slughorn PRIVATE cxx_std_20)
set_target_properties(Slughorn PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_EXTENSIONS OFF)
target_include_directories(Slughorn PUBLIC
    "$<BUILD_INTERFACE:${SLUGHORN_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

install(TARGETS Slughorn EXPORT SlughornTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${SLUGHORN_SOURCE_DIR}/slughorn/slughorn.hpp" "${SLUGHORN_SOURCE_DIR}/slughorn/canvas.hpp"
    "${SLUGHORN_SOURCE_DIR}/slughorn/freetype.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/slughorn")
install(EXPORT SlughornTargets FILE SlughornConfig.cmake NAMESPACE Slughorn::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Slughorn")
