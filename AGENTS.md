# Overview

This project (osgEarth) is a 3D geospatial rendering engine. In other words it renders 3D maps and globes complete with 
imagery, elevation, and GIS feature data.

The underlying scene graph is OpenSceneGraph (OSG). Under that is OpenGL.

Performance and scalability (e.g., handling huge amounts of data) are of paramout importance.
Geospatial accurancy and precision are also high priority.


# Building

This project uses CMake to build.

Do not attempt to figure out how to build this project on your own.  Use these instructions.

The build folder is (usually) in ../build (but the user MAY have overriden that with the bootstrap script).
Out-of-source builds are preferred.

To configure this project you can run
```
configure.bat
```
This will create an out of source cmake build in a directory called "../build".

To build the project run this command
```
build.bat
```
Keep in mind that it could take a long time to build the project from scratch.


# Tests

Before running any osgearth commands you need to run the osgearth_shell.bat script to setup your PATH correctly.

To run unit tests run this command from the tests directory
```
osgearth_tests
```

We use Google Benchmark to do microbenchmarks of functions in the src/applications/osgearth_benchmarks project.  When you are asked to improve performance, prove that you actually improved performance by writing a relevant benchmark and providing before and after numbers while also validating that behavior of a function did not change.

To run benchmarks run this command from the tests directory
```
osgearth_benchmarks
```

# Coding Standards

Code needs to be C++14 compliant.
Code needs to build on various platforms, so don't write code for which MSVC has "relaxed rules".
Indent with 4 spaces. No tabs.
Line break at 128 characters.
New code should use the same EOL style (CRLF versus LF) as the existing code in the same file. When in doubt, or for new files, prefer CRLF.

# Documentation

Document every new function with a concise comment describing its purpose and,
where relevant, its ownership, threading, preconditions, and failure behavior.
Include new helpers and test/benchmark functions; explain non-obvious reasoning
instead of merely restating the function name.
