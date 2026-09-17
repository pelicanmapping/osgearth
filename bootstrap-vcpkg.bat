@echo off
setlocal
set ERROR_MSG=

if "%VCPKG_ROOT%" == "" (
    FOR /F "tokens=*" %%X IN ('where vcpkg.exe') do (SET VCPKG_ROOT=%%~dpX)
)

:: Verify vcpkg is available
set VCPKG_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"

if not exist %VCPKG_TOOLCHAIN_FILE% (
    set ERROR_MSG=Cannot find vcpkg. Please ensure vcpkg.exe is in your PATH, or set the VCPKG_ROOT environment variable to your vcpkg install location
    goto :usage
)

:: Pre-scan args for the standalone -Y flag (auto-accept confirmation)
set AUTO_YES=0
for %%A in (%*) do (
    if /I "%%~A"=="-Y" set AUTO_YES=1
)

:: Argument parser from: https://stackoverflow.com/a/8162578
setlocal enableDelayedExpansion
set "options=-S:. -B:..\build -I:..\install -G:"Visual Studio 17 2022" -A:x64"
for %%O in (%options%) do for /f "tokens=1,* delims=:" %%A in ("%%O") do set "%%A=%%~B"
:loopArgs
    if not "%~1"=="" (
      if /I "%~1"=="-Y" (
          shift /1
          goto :loopArgs
      )
      set "test=!options:*%~1:=! "
      if "!test!"=="!options! " (
          echo Error: Invalid option %~1
      ) else if "!test:~0,1!"==" " (
          set "%~1=1"
      ) else (
          setlocal disableDelayedExpansion
          set "val=%~2"
          call :escapeVal
          setlocal enableDelayedExpansion
          for /f delims^=^ eol^= %%A in ("!val!") do endlocal&endlocal&set "%~1=%%A" !
          shift /1
      )
      shift /1
      goto :loopArgs
    )
    goto :endArgs
:escapeVal
    set "val=%val:^=^^%"
    set "val=%val:!=^!%"
    exit /b
:endArgs

set COMPILER=!-G!
set ARCHITECTURE=!-A!

:: Expand to absolute paths
call :realpath !-S!
set SOURCE_DIR=%RETVAL%
call :realpath !-B!
set BUILD_DIR=%RETVAL%
call :realpath !-I!
set INSTALL_DIR=%RETVAL%

:: Ask for confirmation:
echo VCPKG_ROOT       = %VCPKG_ROOT%
echo Source location  = %SOURCE_DIR%
echo Build location   = %BUILD_DIR%
echo Install location = %INSTALL_DIR%
echo Compiler         = %COMPILER%
echo Architecture     = %ARCHITECTURE%
if "%AUTO_YES%"=="1" (
    echo Continue? [Y/N]Y
) else (
    choice /C:YN /M Continue?
    if ERRORLEVEL 2 goto :usage
)

set MANIFEST_DIR="%SOURCE_DIR%"

if not exist "%MANIFEST_DIR%\vcpkg.json" (
    set ERROR_MSG=No vcpkg.json manifest found. Run this script from the root folder of the git repository
    goto usage
)

:: Run CMAKE
mkdir %BUILD_DIR%

if exist %BUILD_DIR%\CMakeCache.txt (
    del %BUILD_DIR%\CMakeCache.txt
)

cmake ^
    -S "%SOURCE_DIR%" ^
    -B "%BUILD_DIR%" ^
    -G "%COMPILER%" ^
    -A %ARCHITECTURE% ^
    -DOSGEARTH_BUILD_BENCHMARKS=ON ^
    -DOSGEARTH_BUILD_TESTS=ON ^
    -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    -DCMAKE_INSTALL_PREFIX="%INSTALL_DIR%" ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_TOOLCHAIN_FILE% ^
    -DVCPKG_MANIFEST_DIR="%MANIFEST_DIR%" ^
    -DVCPKG_MANIFEST_FEATURES=benchmarks ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-release

goto end

:usage
  if not "%ERROR_MSG%" == "" (
      echo Error: %ERROR_MSG%
  )
  echo Usage: vcpkg-bootstrap.bat -S source_folder -B build_folder -I install_folder -G compiler -A architecture [-Y]
  echo   -Y  Skip the confirmation prompt and proceed automatically
  echo Example:
  echo    vcpkg-bootstrap.bat -S . -B ..\build -I ..\install -G "Visual Studio 17 2022" -A x64

:end
  endlocal
  exit /B


:: Converts a relative path to an absolute path
:realpath
  set RETVAL=%~f1
  exit /B 
