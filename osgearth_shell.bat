set PATH=%CD%\install\bin;%PATH%
set VCPKG_DIR=%CD%\build\vcpkg_installed
set PATH=%VCPKG_DIR%\x64-windows-release\bin;%PATH%
set PATH=%VCPKG_DIR%\x64-windows-release\plugins;%PATH%
set GDAL_DATA=%VCPKG_DIR%\x64-windows-release\share\gdal
set OSGEARTH_CACHE=c:\osgearth_cache