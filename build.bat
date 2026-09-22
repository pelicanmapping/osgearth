@echo off
setlocal
if not defined CMAKE_BUILD_PARALLEL_LEVEL set CMAKE_BUILD_PARALLEL_LEVEL=8
cmake --build "%~dp0..\build" --config RelWithDebInfo --parallel %CMAKE_BUILD_PARALLEL_LEVEL% --target install
