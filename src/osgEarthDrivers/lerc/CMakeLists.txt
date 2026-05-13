find_package(Lerc CONFIG QUIET)
if(NOT Lerc_FOUND)
    find_package(Lerc MODULE QUIET)
endif()

if(TARGET Lerc::Lerc)
    message(STATUS "Found LERC")
    add_osgearth_plugin(
        TARGET osgdb_lerc
        SOURCES ReaderWriterLERC.cpp )
    target_link_libraries(osgdb_lerc PRIVATE Lerc::Lerc)
endif()
