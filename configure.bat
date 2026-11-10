@echo off
setlocal
pushd "%~dp0" || exit /b 1

REM Honor an existing installation; only clone when no root was supplied.
if defined VCPKG_ROOT goto validate_vcpkg
set "VCPKG_ROOT=%CD%\vcpkg"
if not exist "%VCPKG_ROOT%\.git" (
    git clone --branch 2026.02.27 git@github.com:microsoft/vcpkg.git vcpkg
    if errorlevel 1 (
        popd
        exit /b 1
    )
)

:validate_vcpkg
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    echo Error: VCPKG_ROOT does not contain a vcpkg toolchain: "%VCPKG_ROOT%"
    popd
    exit /b 1
)

call bootstrap-vcpkg.bat -S . -B build -I install -Y
set "CONFIGURE_RESULT=%ERRORLEVEL%"
popd
exit /b %CONFIGURE_RESULT%
