@echo off
setlocal
pushd "%~dp0" || exit /b 1

REM Get vcpkg if it has not already been cloned.
if not exist "vcpkg\.git" (
    git clone --branch 2026.02.27 git@github.com:microsoft/vcpkg.git vcpkg
    if errorlevel 1 (
        popd
        exit /b 1
    )
)

set "VCPKG_ROOT=%CD%\vcpkg"
call bootstrap-vcpkg.bat -S . -B build -I install -Y
set "CONFIGURE_RESULT=%ERRORLEVEL%"
popd
exit /b %CONFIGURE_RESULT%
