option(OSGEARTH_ENABLE_STBDXT "Set to ON to build optional STB DXT image compressor." ON)
if(OSGEARTH_ENABLE_STBDXT)

    add_osgearth_plugin(
        TARGET osgdb_stbdxt
        HEADERS
            stb_dxt.h
        SOURCES
            STBDXTImageProcessor.cpp )

    if(OSGEARTH_HAVE_BASISU)
        # Reader-only builds can lack the block encoders even when the package exists.
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_LIBRARIES basisu::basisu_encoder)
        set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
        if(NOT CMAKE_TRY_COMPILE_CONFIGURATION AND CMAKE_CONFIGURATION_TYPES)
            if(CMAKE_BUILD_TYPE)
                set(CMAKE_TRY_COMPILE_CONFIGURATION "${CMAKE_BUILD_TYPE}")
            elseif("Release" IN_LIST CMAKE_CONFIGURATION_TYPES)
                set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
            endif()
        endif()
        unset(OSGEARTH_BASISU_HAS_BC_ENCODERS CACHE)
        check_cxx_source_compiles([[
            #include <basisu/transcoder/basisu_transcoder.h>
            #include <basisu/transcoder/basisu_transcoder_uastc.h>
            int main() {
                unsigned char pixels[64] = {}, block[16] = {};
                basist::basisu_transcoder_init();
                basist::encode_bc4(block, pixels + 3, 4);
                basist::encode_bc1(block + 8, pixels, 0);
            }
        ]] OSGEARTH_BASISU_HAS_BC_ENCODERS)
        if(OSGEARTH_BASISU_HAS_BC_ENCODERS)
            target_link_libraries(osgdb_stbdxt PRIVATE basisu::basisu_encoder)
        endif()
    endif()
endif()
