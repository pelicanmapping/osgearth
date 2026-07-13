#pragma once

#include <osgEarthImGui/HeapHotspotReport>

#include <cstddef>

namespace HeapHotspots
{
    using Report = osgEarth::HeapHotspotReport;

    // Installs heap API interception. Modules already loaded are patched
    // immediately; modules loaded later (OpenGL drivers, osgDB plugins, GDAL
    // format drivers, ...) are patched automatically via a loader DLL-load
    // notification. Call this as early as possible in main().
    void install();

    // Captures a self-contained snapshot. This is the primary API for UIs,
    // telemetry, tests, and custom serializers.
    Report capture();

    // Writes a previously captured report without walking the heaps or
    // resolving symbols again. A maxResults value of zero writes every site.
    bool write(const Report& report,
               const char* filename,
               std::size_t maxResults = 50);

    // Convenience wrapper retained for command-line and existing callers.
    void dump(const char* filename = "heap-hotspots.txt",
              std::size_t maxResults = 50);
}
