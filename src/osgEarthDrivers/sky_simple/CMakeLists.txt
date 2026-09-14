#
# SimpleSky plugin
#

set(TARGET_GLSL
    SimpleSky.Environment.glsl
    SimpleSky.Atmosphere.frag.glsl
    SimpleSky.Atmosphere.vert.glsl
    SimpleSky.Ground.ONeil.frag.glsl
    SimpleSky.Ground.ONeil.vert.glsl
    SimpleSky.Moon.frag.glsl
    SimpleSky.Moon.vert.glsl
    SimpleSky.Stars.frag.glsl
    SimpleSky.Stars.vert.glsl
    SimpleSky.Stars.GLES.frag.glsl
    SimpleSky.Stars.GLES.vert.glsl
    SimpleSky.Sun.frag.glsl
    SimpleSky.Sun.vert.glsl
)


set(TARGET_IN
    SimpleSkyShaders.cpp.in)

set(SHADERS_CPP "${CMAKE_CURRENT_BINARY_DIR}/AutoGenShaders.cpp")

configure_shaders(
    SimpleSkyShaders.cpp.in
    ${SHADERS_CPP}
    ${TARGET_GLSL} )

set(TARGET_SRC
    SkyEnvironment.cpp
    SimpleSkyExtension.cpp
    SimpleSkyNode.cpp
    BrunetonImpl.cpp
    eb_atmosphere_model.cpp
    eb_texture_buffer.cpp
    eb_utility.cpp
    eb_ogl.cpp
    eb_shaders.cpp
    ${SHADERS_CPP} )

set(TARGET_H
    SkyEnvironment
    SimpleSkyOptions
	SimpleSkyNode
    SimpleSkyShaders
    BrunetonImpl
    eb_atmosphere_model.h
    eb_constants.h
    eb_texture_buffer.h
    eb_utility.h
    eb_macros.h
    eb_ogl.h
    eb_shaders.h)

set(INCLUDED_SRC_FILES
    eb_function_shaders.cpp
    eb_compute_shaders.cpp
    eb_osgearth_shaders.cpp)

add_osgearth_plugin(
    TARGET osgdb_osgearth_sky_simple
    SOURCES ${TARGET_SRC}
    HEADERS ${TARET_H}
    PUBLIC_HEADERS SimpleSkyOptions
    SHADERS ${TARGET_GLSL}
    TEMPLATES ${TARGET_IN}
    LIBRARIES OpenGL::GL )

