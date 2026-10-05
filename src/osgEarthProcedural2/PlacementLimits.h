/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarthProcedural2/Scatter>
#include <algorithm>
#include <unordered_set>

namespace osgEarth { namespace Procedural2
{
    //! Synchronous request-local work budget, also forwarding cancellation to the caller.
    //! Own it with ref_ptr; the caller must outlive this scope. Nested generation shares this progress object.
    class PlacementWorkProgress : public ProgressCallback
    {
        ProgressCallback* _parent;
        std::uint64_t _remaining;
    public:
        //! Starts a page-wide budget independent of its accepted-placement caps.
        PlacementWorkProgress(ProgressCallback* parent, unsigned limit) :
            ProgressCallback(parent), _parent(parent), _remaining(limit) { }
        //! Publishes successful truncation diagnostics to the original caller without treating them as errors.
        ~PlacementWorkProgress() override
        { if (_parent && !message().empty()) _parent->message() = message(); }
        //! Charges generated/examined candidates before expensive loops; exhaustion fails atomically upstream.
        bool consume(std::uint64_t count)
        { if (count > _remaining) return false; _remaining -= count; return true; }
    };

    //! Charges a shared page budget when present; standalone generators still enforce their local work bounds.
    inline Status consumePlacementWork(std::uint64_t count, ProgressCallback* progress)
    {
        auto budget = dynamic_cast<PlacementWorkProgress*>(progress);
        return budget && !budget->consume(count) ?
            Status(Status::ConfigurationError, "Placement request exceeds cumulative work limit") : Status::NoError;
    }

    //! Request-local bounded selection. Independent hashing avoids traversal-order and coordinate-strip bias.
    //! T supplies a stable id and may carry extra representation metadata. Duplicate IDs count only once.
    template<typename T> class PlacementSelection
    {
        struct Entry
        {
            std::uint64_t rank;
            T value;
        };
        unsigned _limit;
        bool _capped = false;
        std::vector<Entry> _entries;
        std::unordered_set<std::uint64_t> _ids;
        //! Orders stable ranks, resolving hash ties by ID; the heap root is the worst retained candidate.
        static bool less(const Entry& a, const Entry& b)
        { return a.rank != b.rank ? a.rank < b.rank : a.value.id < b.value.id; }
        //! Separate stream from position, density, and tier retention; fixed across platforms and requests.
        static std::uint64_t rank(std::uint64_t id)
        {
            auto x = id ^ UINT64_C(0x510e527fade682d1);
            x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
            x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
            return x ^ (x >> 31);
        }
    public:
        //! Retains at most limit accepted records; limit must be positive (validated by ScatterGroup).
        explicit PlacementSelection(unsigned limit) : _limit(limit) { }
        //! Considers an already filtered placement with O(limit) storage; never stops scanning the region.
        void add(const T& value)
        {
            if (_ids.count(value.id)) return;
            Entry entry{rank(value.id), value};
            if (_entries.size() < _limit)
            { _entries.push_back(entry); _ids.insert(value.id); return; }
            if (!_capped) { std::make_heap(_entries.begin(), _entries.end(), less); _capped = true; }
            if (!less(entry, _entries.front())) return;
            _ids.erase(_entries.front().value.id);
            std::pop_heap(_entries.begin(), _entries.end(), less);
            _entries.back() = entry; _ids.insert(value.id);
            std::push_heap(_entries.begin(), _entries.end(), less);
        }
        //! Publishes a valid capped result; preserves input order when uncapped. Cancellation publishes nothing.
        Status finish(std::vector<T>& output, ProgressCallback* progress, const std::string& scope)
        {
            output.clear();
            if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Placement canceled");
            if (_capped)
            {
                std::sort(_entries.begin(), _entries.end(), less);
                if (progress) progress->message() = "Placement cap reached (" + scope + "): " + std::to_string(_limit);
            }
            output.reserve(_entries.size());
            for (const auto& entry : _entries) output.push_back(entry.value);
            return Status::NoError;
        }
    };

    //! Applies request-only tier retention before accepted caps, using the same rank as the public thinning helper.
    inline bool retainPlacement(const ScatterPlacement& placement, float fraction)
    {
        if (fraction >= 1.0f) return true;
        if (fraction <= 0.0f) return false;
        auto x = (placement.id ^ UINT64_C(0xa54ff53a5f1d36f1)) + UINT64_C(0x9e3779b97f4a7c15);
        x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
        x ^= x >> 31;
        return double(x >> 11) / 9007199254740992.0 < fraction;
    }

    //! Caps a complete filtered cell from an application strategy; selection is independent of input order.
    inline Status limitPlacements(std::vector<ScatterPlacement>& values, unsigned limit, float retention,
        ProgressCallback* progress, const std::string& scope)
    {
        PlacementSelection<ScatterPlacement> selection(limit);
        for (const auto& value : values)
        {
            if (progress && progress->isCanceled())
            { values.clear(); return Status(Status::ResourceUnavailable, "Placement canceled"); }
            if (retainPlacement(value, retention)) selection.add(value);
        }
        return selection.finish(values, progress, scope);
    }
} }
