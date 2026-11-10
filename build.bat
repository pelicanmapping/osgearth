@echo off
setlocal
REM Pass an explicit job count when requested; bare --parallel uses the native build tool's default.
if defined CMAKE_BUILD_PARALLEL_LEVEL (
    cmake --build "%~dp0build" --config RelWithDebInfo --parallel "%CMAKE_BUILD_PARALLEL_LEVEL%" --target install
) else (
    cmake --build "%~dp0build" --config RelWithDebInfo --parallel --target install
)
exit /b %ERRORLEVEL%
