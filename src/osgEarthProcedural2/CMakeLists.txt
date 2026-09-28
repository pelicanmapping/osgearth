# Landscape generation and rendering; independent of osgEarthProcedural.
set(OSGEARTH_HAVE_PROCEDURAL2_NODEKIT ON PARENT_SCOPE)

add_osgearth_library(
    TARGET osgEarthProcedural2
    SOURCES Canopy.cpp CanopyAssets.cpp CanopyTransition.cpp FeaturePlacement.cpp Scatter.cpp
        PlaceholderAssets.cpp Grass.cpp AssetLighting.cpp AssetCatalog.cpp VegetationLayer2.cpp
    PUBLIC_HEADERS Canopy Export FeaturePlacement Scatter AssetCatalog VegetationLayer2
    HEADERS CanopyAssets.h PlaceholderAssets.h Grass.h AssetLighting.h CanopyTransition.h
    PUBLIC_LIBRARIES osgEarth
    FOLDER NodeKits)

if(OSGEARTH_BUILD_SHARED_LIBS)
    target_compile_definitions(osgEarthProcedural2 PRIVATE OSGEARTHPROCEDURAL2_LIBRARY)
endif()
