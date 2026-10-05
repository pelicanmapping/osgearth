add_osgearth_app(
    TARGET osgearth_procedural2
    SOURCES osgearth_procedural2.cpp AssetImport.cpp
    LIBRARIES osgEarthPrestige ${OPENGL_LIBRARIES}
    FOLDER Examples)
