option(OSGEARTH_ENABLE_STBDXT "Set to ON to build optional STB DXT image compressor." ON)
if(OSGEARTH_ENABLE_STBDXT)

    add_osgearth_plugin(
        TARGET osgdb_stbdxt
        HEADERS
            stb_dxt.h
        SOURCES
            STBDXTImageProcessor.cpp )

ENDIF()
