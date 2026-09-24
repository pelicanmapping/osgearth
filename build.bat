@echo off
cmake --build "%~dp0build" --config RelWithDebInfo --parallel --target install
