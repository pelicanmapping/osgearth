![Windows](https://github.com/gwaldron/osgearth/actions/workflows/windows.yml/badge.svg)
![Linux](https://github.com/gwaldron/osgearth/actions/workflows/linux.yml/badge.svg)
![OSX](https://github.com/gwaldron/osgearth/actions/workflows/macos.yml/badge.svg)


## Welcome to osgEarth!

osgEarth adds geospatially accurate 3D maps to your C++ application.

<img src="https://github.com/user-attachments/assets/a0b1c650-442a-4e6d-88e6-42a5c92083b8" width="200" height="140"/>
<img src="https://github.com/user-attachments/assets/08d0f8c0-49e1-41a8-8b97-d663337f1cbb" width="200" height="140"/>
<img src="https://github.com/user-attachments/assets/575315e1-e2ae-43ec-8a97-83bafcfa9131" width="200" height="140"/>
<img src="https://github.com/user-attachments/assets/24971c79-f93c-48eb-ab79-161bb35beae4" width="200" height="140"/>
<img src="https://github.com/user-attachments/assets/cf40e4a9-429d-4cac-9464-f9825149e7f2" width="200" height="140"/>
<img src="https://github.com/user-attachments/assets/1cd49290-9b2d-42ec-a8c3-9c1c38eb673c" width="200" height="140"/>
<img src="https://github.com/user-attachments/assets/bfd869fd-32b5-48b5-a037-4951f812b757" width="200" height="140"/>
<img src="https://github.com/user-attachments/assets/1876fffb-e683-4fa9-9521-cdd9795dea85" width="200" height="140"/>

osgEarth builds on trusted open source technologies like OpenSceneGraph and GDAL to give you high-performance, accurate terrain and map rendering. It supports a myriad of geospatial data formats and map projections.

## Install the SDK

Building osgEarth or using its SDK requires C++17 or later.

Install the latest version of osgEarth from `vcpkg`:
```bat
vcpkg install osgearth:x64-windows
vcpkg install osgearth[tools]:x64-windows
```

## Check out some examples

`osgearth_imgui` is the main command-line viewer. `osgearth_viewer` is a stripped-down viewer without any GUI.
Both of these read "earth files", XML files that describe the contents of a map.

You can find example earth files in the `tests` folder of the repo.

```bat
cd tests

:: Online imagery and elevation:
osgearth_imgui readymap.earth

:: Local GeoTIFFs:
osgearth_imgui simple.earth 

:: OpenStreetMap:
osgearth_imgui osm.earth
```

## Integrate it into your project

CMakeLists.txt
```cmake
cmake_minimum_required(VERSION 3.20)
project(myApp)
find_package(osgEarth CONFIG REQUIRED)
add_executable(myApp main.cpp)
target_link_libraries(myApp PRIVATE osgEarth::osgEarth)
install(TARGETS myApp RUNTIME DESTINATION bin)
```
main.cpp
```c++
#include <osgEarth/MapNode>
#include <osgEarth/TMS>
#include <osgEarth/EarthManipulator>
#include <osgEarth/GLUtils>
#include <osg/ArgumentParser>
#include <osgViewer/Viewer>

int main(int argc, char** argv)
{
    osgEarth::initialize();
    
    osg::ArgumentParser args(&argc, argv);
    osgViewer::Viewer viewer(args);
    viewer.setRealizeOperation(new osgEarth::GL3RealizeOperation());
    
    auto imagery = new osgEarth::TMSImageLayer();
    imagery->setURL("https://readymap.org/readymap/tiles/1.0.0/7/");
    
    auto mapNode = new osgEarth::MapNode();
    mapNode->getMap()->addLayer(imagery);
    
    viewer.setSceneData(mapNode);
    viewer.setCameraManipulator(new osgEarth::EarthManipulator(args));
    
    return viewer.run();
}
```

## Build it yourself

To build osgEarth yourself, [follow the instructions here](https://docs.osgearth.org/en/latest/build.html).

## Resources

* [Documentation](http://docs.osgearth.org/en/latest/) 
* [Gallery](https://www.pelicanmapping.com/home-1/opensource) 
* [Custom Software Development](https://www.pelicanmapping.com/software)

---
© Copyright [Pelican Mapping](http://pelicanmapping.com)
