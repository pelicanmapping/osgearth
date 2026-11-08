/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "ResourceLifetimeTrace.h"
#include <osg/Timer>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace
{
    //! Encode arbitrary UTF-8 labels and control characters as a JSON string.
    std::string quote(const std::string& value)
    {
        std::ostringstream out;
        out << '"';
        for (unsigned char c : value)
        {
            if (c == '"' || c == '\\') out << '\\' << c;
            else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
            else out << c;
        }
        out << '"';
        return out.str();
    }

    //! One sink per core library; records are buffered and flushed at normal process teardown.
    struct TraceState
    {
        std::mutex mutex;
        std::ofstream file;
        std::uint64_t nextSpan = 0, nextResource = 0, nextEvent = 0;
        std::unordered_map<const osgEarth::GLObject*, std::uint64_t> lifetimes;

        //! Open only on explicit opt-in; a bad path leaves tracing disabled and reports the failure once.
        TraceState()
        {
            const char* path = std::getenv("OSGEARTH_RESOURCE_TRACE");
            if (!path || !*path) return;
            file.open(path, std::ios::out | std::ios::trunc);
            if (!file)
            {
                std::fprintf(stderr, "osgEarth: cannot open resource trace '%s'\n", path);
                return;
            }
            file.imbue(std::locale::classic());
            file << "{\"schema\":1,\"tick_seconds\":" << std::setprecision(17)
                << osg::Timer::instance()->getSecondsPerTick()
                << ",\"superluminal\":"
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
                << "true"
#else
                << "false"
#endif
                << "}\n";
        }

        //! Write under mutex; tick uses OSG's native clock (QPC on Windows) for ETW correlation.
        void write(const char* phase, std::uint64_t span, const std::string& data)
        {
            file << "{\"seq\":" << ++nextEvent << ",\"tick\":" << osg::Timer::instance()->tick()
                << ",\"thread\":\"" << std::this_thread::get_id()
                << "\",\"phase\":\"" << phase << "\",\"span\":" << span << ',' << data << "}\n";
        }
    };

    //! Lazily initialize the sink without any OpenGL queries or background threads.
    TraceState& traceState()
    {
        static TraceState state;
        return state;
    }
}

bool osgEarth::detail::resourceTraceEnabled() noexcept
{
    // Avoid consulting mutable stream state outside its lock.
    static const bool enabled = []() noexcept {
        try { return traceState().file.is_open(); }
        catch (...) { return false; }
    }();
    return enabled;
}

osgEarth::detail::ResourceTraceScope::ResourceTraceScope(
    const char* event, const GLObject* object, int context, bool newLifetime, std::int64_t frame) noexcept
{
    if ((!object && context < 0) || !resourceTraceEnabled()) return;
    try
    {
        auto& state = traceState();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.file.good()) return;
        std::ostringstream data;
        data.imbue(std::locale::classic());
        const auto span = ++state.nextSpan;
        data << "\"trace_span\":" << span << ",\"event\":" << quote(event)
            << ",\"context\":" << context << ",\"frame\":" << frame;
        if (object)
        {
            auto& id = state.lifetimes[object];
            if (id == 0 || newLifetime) id = ++state.nextResource;
            data << ",\"resource\":" << id << ",\"object\":\"" << object
                << "\",\"namespace\":" << object->ns() << ",\"name\":" << object->name()
                << ",\"creator_context\":" << (object->ext() ? int(object->ext()->contextID) : -1)
                << ",\"bytes\":" << object->size() << ",\"recycles\":" << object->recycles()
                << ",\"label\":" << quote(object->label());
            if (const auto* texture = dynamic_cast<const GLTexture*>(object))
            {
                const auto& profile = texture->profile();
                data << ",\"target\":" << profile._target << ",\"format\":" << profile._internalFormat
                    << ",\"width\":" << profile._width << ",\"height\":" << profile._height
                    << ",\"depth\":" << profile._depth << ",\"mip_levels\":" << profile._numMipmapLevels;
            }
            else if (const auto* buffer = dynamic_cast<const GLBuffer*>(object))
            {
                data << ",\"target\":" << buffer->target();
            }
        }
        _data = data.str();
        _span = span;
        state.write("B", _span, _data);
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
        _scope.emplace(event, _data.c_str());
#endif
    }
    catch (...) { _span = 0; }
}

osgEarth::detail::ResourceTraceScope::~ResourceTraceScope() noexcept
{
    if (!_span) return;
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
    _scope.reset();
#endif
    try
    {
        auto& state = traceState();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.write("E", _span, _data);
    }
    catch (...) { }
}
