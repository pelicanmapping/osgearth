add_osgearth_app(TARGET osgearth_labels SOURCES osgearth_labels.cpp
    LIBRARIES osgEarthPrestige FOLDER Examples)
if(MSVC)
    target_compile_options(osgearth_labels PRIVATE /utf-8)
endif()
