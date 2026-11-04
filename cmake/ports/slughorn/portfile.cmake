vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
include("${CMAKE_CURRENT_LIST_DIR}/source.cmake")
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO AlphaPixel/slughorn
    REF "${SLUGHORN_REVISION}"
    SHA512 "${SLUGHORN_SHA512}")

vcpkg_cmake_configure(
    SOURCE_PATH "${CURRENT_PORT_DIR}"
    OPTIONS "-DSLUGHORN_SOURCE_DIR=${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/Slughorn)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
