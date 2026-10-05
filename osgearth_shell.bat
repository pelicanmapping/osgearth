@echo off
set "PATH=%~dp0install\bin;%PATH%"
set "VCPKG_DIR=%~dp0build\vcpkg_installed"
set "PATH=%VCPKG_DIR%\x64-windows-release\bin;%PATH%"
set "PATH=%VCPKG_DIR%\x64-windows-release\plugins;%PATH%"
set "GDAL_DATA=%VCPKG_DIR%\x64-windows-release\share\gdal"
set "OSG_GL_CONTEXT_VERSION=4.6"
