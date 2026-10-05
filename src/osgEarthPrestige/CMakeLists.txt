#
# osgEarth Prestige NodeKit
#
if(OSGEARTH_BUILD_SHARED_LIBS)
    add_definitions(-DOSGEARTHPRESTIGE_LIBRARY)
endif()

set(LIB_NAME osgEarthPrestige)

set(OSGEARTH_HAVE_PRESTIGE_NODEKIT ON PARENT_SCOPE) # for BuildConfig

set(TARGET_GLSL
    GrimeLayer.glsl
    Sky.Common.glsl
    Sky.Lighting.glsl
    Sky.LUT.glsl
    Sky.Background.glsl
    CloudLayer.Common.glsl
    CloudLayer.Density.glsl
    CloudLayer.Compute.glsl
    CloudLayer.Rays.glsl
    CloudLayer.Light.glsl)

set(SHADERS_CPP "${CMAKE_CURRENT_BINARY_DIR}/AutoGenShaders.cpp")
configure_shaders(PrestigeShaders.cpp.in ${SHADERS_CPP} ${TARGET_GLSL})

set(TARGET_IN
    PrestigeShaders.cpp.in)

set(TARGET_SRC
    SkyNode.cpp
    CloudLayer.cpp
    GrimeLayer.cpp
    PrestigeLayer.cpp
    ${SHADERS_CPP})

set(TARGET_H
    SkyNode
    SkyAtmosphere.h
    CloudLayer
    Export
    GrimeLayer
    PrestigeLayer
    PrestigeShaders)

add_osgearth_library(
    TARGET ${LIB_NAME}
    SOURCES ${TARGET_SRC}
    HEADERS CloudLayerRenderer.h
    PUBLIC_HEADERS ${TARGET_H}
    SHADERS ${TARGET_GLSL}
    TEMPLATES ${TARGET_IN}
    FOLDER "NodeKits")
