add_osgearth_app(
    TARGET osgearth_procedural2
    SOURCES osgearth_procedural2.cpp
    LIBRARIES osgEarthProcedural2 ${OPENGL_LIBRARIES}
    FOLDER Examples)
