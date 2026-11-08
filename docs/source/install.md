# Getting started with osgEarth

## Installation
Building osgEarth and using its SDK require C++17 or later.

The easiest way to install and use osgEarth is with the `vcpkg` package manager. There are two ways to go about it. But first...

### Method 1: Manual install
You can install osgEarth on Windows manually using this command:
```
vcpkg install osgearth:x64-windows
```
If you also want the command-line tools (like `osgearth_imgui`) use this
```
vcpkg install osgearth[tools]:x64-windows
```
Then you can set up your CMake configuration to point at the installed libraries.

You may also need to add some runtime folders to your path, including:

* [vcpkg_root]\installed\x64-windows\bin
* [vcpkg_root]\installed\x64-windows\plugins
* [vcpkg_root]\installed\x64-windows\tools

### Method 2: Use the vcpkg toolchain
This approach sets up CMake to use the vcpkg toolchain, automating the installation of dependencies and making them local to your project. First you need a manifest file in your repository that looks like this:
#### vcpkg.json
```json
{
    "name": "myApp",
    "version-string": "0.1.0",
    "port-version": 1,
    "description": "Simple osgEarth demo application",
    "supports": "!(x86 | wasm32)",
    "dependencies": [ "osgearth" ]
}
```
You can add `vcpkg` dependencies as needed later on.

Next you need to bootstrap CMake to use the vcpkg toolchain, using a command like this. *Note, this is written as a Windows batch file.*
```bat
cmake ^
    -S %SOURCE_DIR% ^
    -B %BUILD_DIR% ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    -DCMAKE_INSTALL_PREFIX=%INSTALL_DIR% ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_TOOLCHAIN_FILE%
```
Customize this command and replace the variables above as needed:
* `SOURCE_DIR` - location of your root CMakeLists.txt file
* `BUILD_DIR` - out-of-source folder for build files
* `INSTALL_DIR` - location to install final executables and libraries
* `VCPKG_TOOLCHAIN_FILE` - The toolchain file found at `<vcpkg_install_dir>/scripts/buildsystems/vcpkg.cmake`.

Finally, run cmake to build your dependencies and generate your project files. This will take some time the first time you do it since it needs to download and compile osgEarth and all of its dependencies.
```
cmake <build_dir>
```

## Sample CMakeLists.txt
```cmake
cmake_minimum_required(VERSION 3.20)
project(myApp)
find_package(osgEarth CONFIG REQUIRED)
add_executable(myApp main.cpp)
target_link_libraries(myApp PRIVATE osgEarth::osgEarth)
install(TARGETS myApp RUNTIME DESTINATION bin)
```

## Sample main.cpp
```c++
#include <osgEarth/MapNode>
#include <osgEarth/TMS>
#include <osgEarth/EarthManipulator>
#include <osg/ArgumentParser>
#include <osgViewer/Viewer>

int main(int argc, char** argv)
{
    osgEarth::initialize();
    
    osg::ArgumentParser args(&argc, argv);
    osgViewer::Viewer viewer(args);
    
    auto imagery = new osgEarth::TMSImageLayer();
    imagery->setURL("https://readymap.org/readymap/tiles/1.0.0/7/");
    
    auto mapNode = new osgEarth::MapNode();
    mapNode->getMap()->addLayer(imagery);
    
    viewer.setSceneData(mapNode);
    viewer.setCameraManipulator(new osgEarth::EarthManipulator(args));
    
    return viewer.run();
}
```
