@echo off
set PATH=%~dp0..\install\bin;%PATH%
set VCPKG_DIR=%~dp0..\build\vcpkg_installed
set PATH=%VCPKG_DIR%\x64-windows-release\bin;%PATH%
set PATH=%VCPKG_DIR%\x64-windows-release\plugins;%PATH%
set GDAL_DATA=%VCPKG_DIR%\x64-windows-release\share\gdal
