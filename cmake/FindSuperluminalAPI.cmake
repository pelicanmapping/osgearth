# Locate the installed SDK's own finder, preserving its architecture and runtime selection.
# Superluminal remains optional; SuperluminalAPI_ROOT can point to a relocated SDK.
if(WIN32)
    find_path(SuperluminalAPI_ROOT FindSuperluminalAPI.cmake
        HINTS ENV SuperluminalAPI_ROOT
        PATHS "$ENV{ProgramFiles}/Superluminal/Performance/API")
    if(SuperluminalAPI_ROOT)
        include("${SuperluminalAPI_ROOT}/FindSuperluminalAPI.cmake")
    endif()
endif()
