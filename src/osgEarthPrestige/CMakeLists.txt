#
# osgEarth Prestige NodeKit
#
if(OSGEARTH_BUILD_SHARED_LIBS)
    add_definitions(-DOSGEARTHPRESTIGE_LIBRARY)
endif()

set(LIB_NAME osgEarthPrestige)

set(OSGEARTH_HAVE_PRESTIGE_NODEKIT ON PARENT_SCOPE) # for BuildConfig

set(TARGET_GLSL
    SurfaceSplattingLayer.glsl
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
    LabelLayer.cpp
    LabelNode.cpp
    LabelEngineInternal.cpp
    LabelDeclutterInternal.cpp
    LabelMotionInternal.cpp
    LabelPathInternal.cpp
    GradingLayer.cpp
    OSMRoadWidthFilter.cpp
    SelectFilter.cpp
    SkyNode.cpp
    CloudLayer.cpp
    SurfaceSplattingLayer.cpp
    GrimeLayer.cpp
    PrestigeLayer.cpp
    AggregateCoverage.cpp
    Canopy.cpp
    CanopyTransition.cpp
    TreeCards.cpp
    FeaturePlacement.cpp
    Scatter.cpp
    PlaceholderAssets.cpp
    Grass.cpp
    AssetLighting.cpp
    AssetCatalog.cpp
    VegetationLayer.cpp
    FeatureOverlay.cpp
    ${SHADERS_CPP})

set(TARGET_H
    LabelLayer
    LabelNode
    GradingLayer
    OSMRoadWidthFilter
    SelectFilter
    SkyNode
    SkyAtmosphere.h
    CloudLayer
    Export
    SurfaceSplattingLayer
    GrimeLayer
    PrestigeLayer
    PrestigeShaders
    AggregateCoverage
    Canopy
    FeaturePlacement
    FeatureOverlay
    Scatter
    AssetCatalog
    VegetationLayer)

set(TARGET_PRIVATE_H
    CloudLayerRenderer.h PlacementLimits.h TreeCards.h PlaceholderAssets.h Grass.h AssetLighting.h
    CanopyTransition.h OverlayPager.h Grading.h LabelEngineInternal.h)

# LabelLayer and LabelNode are public; shaping, placement and renderer headers remain private.
if(OSGEARTH_ENABLE_SLUGHORN)
    find_package(Slughorn REQUIRED)
    find_package(Freetype REQUIRED)
    find_package(harfbuzz CONFIG REQUIRED)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/LabelText.vert.glsl" LABEL_TEXT_VERTEX)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/LabelText.frag.glsl" LABEL_TEXT_FRAGMENT)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/LabelPath.comp.glsl" LABEL_PATH_COMPUTE)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/LabelLeader.vert.glsl" LABEL_LEADER_VERTEX)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/LabelLeader.frag.glsl" LABEL_LEADER_FRAGMENT)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        LabelText.vert.glsl LabelText.frag.glsl LabelPath.comp.glsl LabelLeader.vert.glsl LabelLeader.frag.glsl)
    configure_file(LabelTextShaders.h.in "${CMAKE_CURRENT_BINARY_DIR}/LabelTextShaders.h" @ONLY)
    list(APPEND TARGET_SRC LabelText_Slughorn.cpp LabelTilesInternal.cpp LabelDynamicInternal.cpp
        LabelRendererInternal.cpp LabelSceneInternal.cpp)
    list(APPEND TARGET_PRIVATE_H LabelStyleInternal.h LabelTextInternal.h LabelTilesInternal.h
        LabelDynamicInternal.h LabelRendererInternal.h LabelSceneInternal.h "${CMAKE_CURRENT_BINARY_DIR}/LabelTextShaders.h")
    list(APPEND TARGET_GLSL LabelText.vert.glsl LabelText.frag.glsl LabelPath.comp.glsl
        LabelLeader.vert.glsl LabelLeader.frag.glsl)
    list(APPEND TARGET_IN LabelTextShaders.h.in)
endif()

add_osgearth_library(
    TARGET ${LIB_NAME}
    SOURCES ${TARGET_SRC}
    HEADERS ${TARGET_PRIVATE_H}
    PUBLIC_HEADERS ${TARGET_H}
    SHADERS ${TARGET_GLSL}
    TEMPLATES ${TARGET_IN}
    FOLDER "NodeKits")

if(OSGEARTH_ENABLE_SLUGHORN)
    target_include_directories(${LIB_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(${LIB_NAME} PRIVATE Slughorn::Slughorn Freetype::Freetype harfbuzz::harfbuzz)
    if(MSVC)
        set_source_files_properties(LabelText_Slughorn.cpp PROPERTIES COMPILE_OPTIONS "/std:c++20")
    else()
        set_source_files_properties(LabelText_Slughorn.cpp PROPERTIES COMPILE_OPTIONS "-std=c++20")
    endif()
endif()
