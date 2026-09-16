set(TARGET_H
    GLTFReader.h
    GLTFWriter.h
)

set(TARGET_SRC
    ReaderWriterGLTF.cpp
)

add_osgearth_plugin(
    TARGET osgdb_gltf
    HEADERS ${TARGET_H}
    SOURCES ${TARGET_SRC} )

# cgltf parses and writes glTF; stb_image (in the third_party root) decodes
# embedded PNG/JPEG images.
target_include_directories(osgdb_gltf PRIVATE
    ${OSGEARTH_EMBEDDED_THIRD_PARTY_DIR}/cgltf
    ${OSGEARTH_EMBEDDED_THIRD_PARTY_DIR})

find_package(meshoptimizer QUIET)
if(TARGET meshoptimizer::meshoptimizer)
    target_link_libraries(osgdb_gltf PRIVATE meshoptimizer::meshoptimizer)
    target_compile_definitions(osgdb_gltf PRIVATE OSGEARTH_GLTF_HAVE_MESHOPT)
endif()

find_package(draco QUIET)
if(TARGET draco::draco)
    target_link_libraries(osgdb_gltf PRIVATE draco::draco)
    target_compile_definitions(osgdb_gltf PRIVATE OSGEARTH_GLTF_HAVE_DRACO)
endif()

find_package(WebP QUIET)
if(TARGET WebP::webp)
    target_link_libraries(osgdb_gltf PRIVATE WebP::webp)
    target_compile_definitions(osgdb_gltf PRIVATE OSGEARTH_GLTF_HAVE_WEBP)
endif()
