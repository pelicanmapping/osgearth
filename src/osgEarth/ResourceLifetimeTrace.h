/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarth/GLUtils>
#include <cstdint>
#include <string>
#include <optional>
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
#include <Superluminal/PerformanceAPI.h>
#endif

namespace osgEarth { namespace detail
{
    //! Process-wide opt-in, read once from OSGEARTH_RESOURCE_TRACE (an output JSONL path).
    bool resourceTraceEnabled() noexcept;

    /** Internal diagnostic scope; never retains a resource or changes GL state.
     * Call while the resource's normal ownership/locking rules guarantee stable metadata.
     * Records a snapshot before deletion, so the end event remains valid after GL name invalidation.
     * File access is serialized; failures disable diagnostics without failing the rendering operation.
     */
    class ResourceTraceScope
    {
    public:
        //! event must be a string literal; context is the executing context, or -1 if unavailable.
        ResourceTraceScope(const char* event, const GLObject* object = nullptr, int context = -1,
            bool newLifetime = false, std::int64_t frame = -1) noexcept;
        //! Ends the CPU scope; this does not assert completion of deferred driver/GPU cleanup.
        ~ResourceTraceScope() noexcept;
        ResourceTraceScope(const ResourceTraceScope&) = delete;
        ResourceTraceScope& operator=(const ResourceTraceScope&) = delete;

    private:
        std::string _data;
        std::uint64_t _span = 0;
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
        std::optional<PerformanceAPI::InstrumentationScope> _scope;
#endif
    };
} }
