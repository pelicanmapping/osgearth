# Provide Slughorn::Slughorn from an installation, or build the pinned core.
find_package(Slughorn CONFIG QUIET)
if(NOT TARGET Slughorn::Slughorn)
    include(FetchContent)
    include("${CMAKE_CURRENT_LIST_DIR}/ports/slughorn/source.cmake")
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(osgearth_slughorn
        URL "https://codeload.github.com/AlphaPixel/slughorn/tar.gz/${SLUGHORN_REVISION}"
        URL_HASH "SHA512=${SLUGHORN_SHA512}"
        SOURCE_SUBDIR osgearth-unused)
    FetchContent_MakeAvailable(osgearth_slughorn)

    set(SLUGHORN_SOURCE_DIR "${osgearth_slughorn_SOURCE_DIR}")
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/ports/slughorn" "${osgearth_slughorn_BINARY_DIR}")
    install(FILES "${SLUGHORN_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/osgearth/licenses" RENAME Slughorn-LICENSE)
endif()

if(TARGET Slughorn::Slughorn)
    set(Slughorn_FOUND TRUE)
endif()
