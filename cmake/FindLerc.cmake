#
# FindLerc.cmake
#
# Outputs:
#   Lerc_FOUND (boolean variable)
#   Lerc::Lerc (imported link library)
#

find_package(unofficial-lerc CONFIG QUIET)

if(TARGET unofficial::Lerc::Lerc)
    set(Lerc_LIBRARY unofficial::Lerc::Lerc)
    set(Lerc_INCLUDE_DIR "provided by unofficial::Lerc::Lerc")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(Lerc QUIET IMPORTED_TARGET Lerc)
    endif()

    if(TARGET PkgConfig::Lerc)
        set(Lerc_LIBRARY PkgConfig::Lerc)
        set(Lerc_INCLUDE_DIR "provided by PkgConfig::Lerc")
    else()
        find_path(Lerc_INCLUDE_DIR NAMES Lerc_c_api.h)
        # Search for both Release and Debug libraries separately
        find_library(Lerc_LIBRARY_RELEASE NAMES Lerc lerc)
        find_library(Lerc_LIBRARY_DEBUG NAMES Lercd lercd Lerc_d lerc_d)

        # Automatically handles setting Lerc_LIBRARY based on what was found
        include(SelectLibraryConfigurations)
        select_library_configurations(Lerc)
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Lerc DEFAULT_MSG Lerc_LIBRARY Lerc_INCLUDE_DIR)

if(Lerc_FOUND AND NOT TARGET Lerc::Lerc)
    if(TARGET unofficial::Lerc::Lerc)
        add_library(Lerc::Lerc INTERFACE IMPORTED)
        set_property(TARGET Lerc::Lerc PROPERTY
            INTERFACE_LINK_LIBRARIES unofficial::Lerc::Lerc)
    elseif(TARGET PkgConfig::Lerc)
        add_library(Lerc::Lerc INTERFACE IMPORTED)
        set_property(TARGET Lerc::Lerc PROPERTY
            INTERFACE_LINK_LIBRARIES PkgConfig::Lerc)
    else()
        add_library(Lerc::Lerc UNKNOWN IMPORTED)
        set_target_properties(Lerc::Lerc PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Lerc_INCLUDE_DIR}")

        # Explicitly map the Release library if it was found
        if(Lerc_LIBRARY_RELEASE)
            set_property(TARGET Lerc::Lerc APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
            set_target_properties(Lerc::Lerc PROPERTIES IMPORTED_LOCATION_RELEASE "${Lerc_LIBRARY_RELEASE}")
        endif()

        # Explicitly map the Debug library if it was found
        if(Lerc_LIBRARY_DEBUG)
            set_property(TARGET Lerc::Lerc APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
            set_target_properties(Lerc::Lerc PROPERTIES IMPORTED_LOCATION_DEBUG "${Lerc_LIBRARY_DEBUG}")
        endif()

        # Provide a fallback for single-config generators or if only one lib exists
        if(Lerc_LIBRARY)
            set_target_properties(Lerc::Lerc PROPERTIES IMPORTED_LOCATION "${Lerc_LIBRARY}")
        endif()

        if(LERC_STATIC)
            set_property(TARGET Lerc::Lerc APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS LERC_STATIC)
        endif()
    endif()
endif()
