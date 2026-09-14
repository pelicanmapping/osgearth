This project uses CMake to build.

Do not attempt to figure out how to build this project on your own.  Use these instructions explicitly.

To configure this project you can run
```
configure.bat
```
This will create an out of source cmake build in a directory called "build".

To build the project run this command
```
build.bat
```
Keep in mind that it could take a long time to build the project from scratch.


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

Document every new function with a concise comment describing its purpose and,
where relevant, its ownership, threading, preconditions, and failure behavior.
Include new helpers and test/benchmark functions; explain non-obvious reasoning
instead of merely restating the function name.
