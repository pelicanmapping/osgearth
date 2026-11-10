/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#include "LabelDynamicInternal.h"
#include <osgEarth/Threading>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <stdexcept>
#include <thread>

using namespace osgEarthPrestige::Labeling;

struct DynamicSource::State
{
    struct Entry
    {
        DynamicLabel value;
        std::uint64_t id = 0, revision = 0, lifetime = 0;
        std::size_t slot = 0;
    };
    std::string font;
    DynamicOptions options;
    mutable std::mutex mutex;
    std::condition_variable wake;
    static constexpr std::size_t chunkSize = 64, chunkCount = 16;
    using Entries = std::vector<std::shared_ptr<const Entry>>;
    std::map<std::uint64_t, std::shared_ptr<const Entry>> desired;
    std::array<std::shared_ptr<const Entry>, chunkSize * chunkCount> slots;
    std::array<std::uint64_t, chunkCount> versions {};
    // Worker-owned cache: only changed chunks are copied/prepared, even when a generation is superseded.
    std::array<std::uint64_t, chunkCount> preparedVersions {};
    std::array<BatchPtr, chunkCount> preparedChunks;
    std::array<std::shared_ptr<const DynamicSet>, 4> results;
    int published = -1;
    DynamicStats counters;
    bool stopped = false, failed = false;
    std::thread worker;

    //! Captures immutable configuration before the preparation worker starts.
    State(std::string f, DynamicOptions op) : font(std::move(f)), options(op) { }

    //! Prepares at most 64 records on the worker; previous chunks and their glyph resources remain immutable.
    BatchPtr prepareChunk(const Entries& entries, TextService& fonts)
    {
        if (entries.empty()) return {};
        std::vector<std::string> strings;
        strings.reserve(entries.size());
        for (const auto& entry : entries) strings.push_back(entry->value.text);
        auto resources = std::make_shared<TextBatchResources>();
        resources->incremental = true;
        resources->text = fonts.prepare(strings);
        std::vector<Record> records;
        records.reserve(entries.size());
        resources->styles.reserve(entries.size());
        for (const auto& entry : entries)
        {
            const auto& value = entry->value;
            Record record;
            record.key = { options.sourceID, entry->id, 0, 0 };
            record.revision = entry->revision;
            record.continuity = entry->lifetime;
            record.worldAnchor = value.worldAnchor;
            record.priority = value.priority;
            record.shapedRun = record.style = records.size();
            setTextFootprint(record, *resources->text->runs[records.size()], value.style);
            records.push_back(record);
            resources->styles.push_back(value.style);
        }
        return std::make_shared<const Batch>(std::move(records), std::move(resources));
    }

    //! Assembles one atomic generation from cached chunks; failure retains the last complete publication.
    std::shared_ptr<DynamicSet> prepare(const std::array<Entries, chunkCount>& entries,
        const std::array<std::uint64_t, chunkCount>& capturedVersions, std::uint64_t revision, TextService& fonts)
    {
        auto result = std::make_shared<DynamicSet>();
        result->revision = revision;
        result->batches.reserve(chunkCount);
        std::size_t bytes = 0;
        auto chunks = preparedChunks;
        for (std::size_t i = 0; i < chunkCount; ++i)
        {
            if (capturedVersions[i] != preparedVersions[i])
            {
                chunks[i] = prepareChunk(entries[i], fonts);
            }
            if (chunks[i])
            {
                bytes += chunks[i]->bytes();
                result->batches.push_back(chunks[i]);
            }
        }
        if (bytes > options.preparedBytes)
            throw std::runtime_error("Dynamic label prepared-byte budget exceeded");
        preparedChunks = std::move(chunks);
        preparedVersions = capturedVersions;
        return result;
    }

    //! Reclaims unpinned publications and prepares only the latest desired generation; stale work never publishes.
    void run()
    {
        osgEarth::setThreadName("oe.labelupdates");
        try
        {
            TextService fonts(font);
            for (;;)
            {
                std::array<std::shared_ptr<const DynamicSet>, 4> retired;
                std::array<Entries, chunkCount> entries;
                std::array<std::uint64_t, chunkCount> capturedVersions;
                std::uint64_t revision = 0;
                int slot = -1;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    if (stopped) break;
                    for (std::size_t i = 0; i < results.size(); ++i)
                    {
                        if (static_cast<int>(i) != published && results[i] && results[i].use_count() == 1)
                            retired[i] = std::move(results[i]);
                        if (!results[i]) slot = static_cast<int>(i);
                    }
                    if (slot >= 0 && counters.submitted > counters.completed)
                    {
                        revision = counters.submitted;
                        capturedVersions = versions;
                        for (std::size_t i = 0; i < chunkCount; ++i)
                        {
                            if (capturedVersions[i] == preparedVersions[i]) continue;
                            entries[i].reserve(chunkSize);
                            for (std::size_t j = i * chunkSize; j < (i + 1) * chunkSize; ++j)
                                if (slots[j]) entries[i].push_back(slots[j]);
                        }
                    }
                    else
                        wake.wait_for(lock, std::chrono::milliseconds(8));
                }
                if (!revision) continue;
                std::shared_ptr<DynamicSet> result;
                std::string error;
                try { result = prepare(entries, capturedVersions, revision, fonts); }
                catch (const std::exception& e) { error = e.what(); }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopped) break;
                    if (revision != counters.submitted) { ++counters.superseded; continue; }
                    counters.completed = revision;
                    if (result)
                    {
                        results[slot] = std::move(result);
                        published = slot;
                        counters.published = revision;
                        counters.error.clear();
                    }
                    else { ++counters.failures; counters.error = error; }
                }
            }
        }
        catch (const std::exception& e)
        {
            std::lock_guard<std::mutex> lock(mutex);
            failed = true;
            counters.completed = counters.submitted;
            ++counters.failures;
            counters.error = e.what();
        }
        // Destruction is a control-thread operation after traversal; reclaim remaining service ownership here.
        for (auto& result : results) result.reset();
        for (auto& chunk : preparedChunks) chunk.reset();
    }
};

DynamicSource::DynamicSource(std::string font, const DynamicOptions& options)
{
    if (options.labels == 0 || options.labels > 1024 || options.textBytes == 0 ||
        options.textBytes > 4 * 1024 * 1024 || options.preparedBytes == 0 || options.preparedBytes > 32 * 1024 * 1024)
        throw std::invalid_argument("Invalid dynamic label limits");
    _state = std::make_unique<State>(std::move(font), options);
    _state->worker = std::thread([this]() { _state->run(); });
}

DynamicSource::~DynamicSource()
{
    { std::lock_guard<std::mutex> lock(_state->mutex); _state->stopped = true; }
    _state->wake.notify_all();
    _state->worker.join();
}

std::uint64_t DynamicSource::set(std::uint64_t id, DynamicLabel value)
{
    Record validation;
    setTextFootprint(validation, TextRun(), value.style);
    if (value.text.empty() || value.text.size() > 4096 || !std::isfinite(value.priority))
        throw std::invalid_argument("Invalid dynamic label text/priority");
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!std::isfinite(value.worldAnchor[axis])) throw std::invalid_argument("Invalid dynamic label anchor");
    for (unsigned component = 0; component < 4; ++component)
        if (!std::isfinite(value.style.color[component]) || !std::isfinite(value.style.halo[component]))
            throw std::invalid_argument("Invalid dynamic label color");
    std::lock_guard<std::mutex> lock(_state->mutex);
    if (_state->stopped || _state->failed) return 0;
    auto found = _state->desired.find(id);
    const auto oldBytes = found == _state->desired.end() ? 0 : found->second->value.text.size();
    if ((found == _state->desired.end() && _state->desired.size() == _state->options.labels) ||
        value.text.size() > _state->options.textBytes - (_state->counters.textBytes - oldBytes)) return 0;
    auto entry = std::make_shared<State::Entry>();
    entry->id = id;
    entry->value = std::move(value);
    entry->revision = ++_state->counters.submitted;
    if (found != _state->desired.end())
    {
        entry->slot = found->second->slot;
        entry->lifetime = found->second->lifetime;
    }
    else
    {
        entry->slot = std::find(_state->slots.begin(), _state->slots.end(), nullptr) - _state->slots.begin();
        entry->lifetime = entry->revision;
    }
    _state->desired[id] = entry;
    _state->slots[entry->slot] = entry;
    _state->versions[entry->slot / State::chunkSize] = entry->revision;
    _state->counters.textBytes += entry->value.text.size();
    _state->counters.textBytes -= oldBytes;
    _state->counters.labels = _state->desired.size();
    _state->wake.notify_one();
    return entry->revision;
}

std::uint64_t DynamicSource::erase(std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(_state->mutex);
    if (_state->stopped || _state->failed) return 0;
    auto found = _state->desired.find(id);
    if (found == _state->desired.end()) return 0;
    _state->counters.textBytes -= found->second->value.text.size();
    const auto slot = found->second->slot;
    _state->slots[slot].reset();
    _state->desired.erase(found);
    _state->counters.labels = _state->desired.size();
    const auto revision = ++_state->counters.submitted;
    _state->versions[slot / State::chunkSize] = revision;
    _state->wake.notify_one();
    return revision;
}

std::shared_ptr<const DynamicSet> DynamicSource::acquire(std::uint64_t after) const
{
    std::unique_lock<std::mutex> lock(_state->mutex, std::try_to_lock);
    if (!lock || _state->stopped || _state->failed || _state->published < 0 || _state->counters.published <= after)
        return {};
    return _state->results[_state->published];
}

bool DynamicSource::pendingOrNew(std::uint64_t after) const
{
    std::unique_lock<std::mutex> lock(_state->mutex, std::try_to_lock);
    if (!lock) return true;
    return !_state->stopped && !_state->failed &&
        (_state->counters.submitted > _state->counters.completed || _state->counters.published > after);
}

DynamicStats DynamicSource::stats() const
{
    std::lock_guard<std::mutex> lock(_state->mutex);
    return _state->counters;
}
