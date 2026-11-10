/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "LabelEngineInternal.h"
#include <osgEarth/Threading>
#include <osg/Vec4d>
#include <osgUtil/CullVisitor>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace osgEarthPrestige::Labeling;

bool LabelKey::operator==(const LabelKey& rhs) const
{
    return source == rhs.source && feature == rhs.feature && role == rhs.role && instance == rhs.instance;
}

bool ViewKey::operator==(const ViewKey& rhs) const
{
    return slot == rhs.slot && generation == rhs.generation;
}

Batch::Batch(std::vector<Record> records, std::shared_ptr<const BatchPayload> payload) :
    _records(std::move(records)), _payload(std::move(payload)), _payloadBytes(_payload ? _payload->bytes() : 0)
{
    for (const auto& record : _records)
    {
        for (unsigned i = 0; i < 3; ++i)
            if (!std::isfinite(record.worldAnchor[i]))
                throw std::invalid_argument("Label anchor must be finite");
        for (unsigned i = 0; i < 2; ++i)
            if (!std::isfinite(record.lowerLeft[i]) || !std::isfinite(record.upperRight[i]) ||
                record.lowerLeft[i] > record.upperRight[i])
                throw std::invalid_argument("Label footprint must be finite and ordered");
        if (!std::isfinite(record.calloutGap) || record.calloutGap < 0 || record.calloutGap > 64 ||
            (record.path && record.calloutGap > 0))
            throw std::invalid_argument("Invalid point-label callout gap");
        if (record.path)
        {
            const auto& path = *record.path;
            if (path.world.size() < 2 || path.world.size() > 256 || path.glyphs.empty() || path.glyphs.size() > 256 ||
                !std::isfinite(path.width) || path.width <= 0)
                throw std::invalid_argument("Invalid label path size");
            for (const auto& point : path.world)
                for (unsigned i = 0; i < 3; ++i)
                    if (!std::isfinite(point[i])) throw std::invalid_argument("Non-finite path vertex");
            for (const auto& glyph : path.glyphs)
            {
                if (!std::isfinite(glyph.distance)) throw std::invalid_argument("Non-finite path glyph offset");
                for (unsigned i = 0; i < 2; ++i)
                    if (!std::isfinite(glyph.lowerLeft[i]) || !std::isfinite(glyph.upperRight[i]) ||
                        glyph.lowerLeft[i] > glyph.upperRight[i]) throw std::invalid_argument("Invalid path glyph bounds");
            }
            _pathBytes += sizeof(LabelPath) + path.world.capacity() * sizeof(osg::Vec3d) +
                path.glyphs.capacity() * sizeof(PathGlyph);
        }
        if (!std::isfinite(record.priority))
            throw std::invalid_argument("Label priority must be finite");
    }
}

bool CameraSnapshot::valid() const
{
    for (unsigned i = 0; i < 16; ++i)
        if (!std::isfinite(worldToView.ptr()[i]) || !std::isfinite(projection.ptr()[i]))
            return false;
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(width) && std::isfinite(height) &&
        std::isfinite(x + width) && std::isfinite(y + height) && std::isfinite(devicePixelRatio) &&
        std::isfinite(time) && width > 0.0 && height > 0.0 && devicePixelRatio > 0.0;
}

bool osgEarthPrestige::Labeling::captureCamera(
    const osgUtil::CullVisitor& visitor, double devicePixelRatio, CameraSnapshot& output)
{
    const auto* viewport = visitor.getViewport();
    const auto* stamp = visitor.getFrameStamp();
    const auto* modelView = visitor.getModelViewMatrix();
    const auto* projection = visitor.getProjectionMatrix();
    if (!viewport || !stamp || !modelView || !projection)
        return false;
    CameraSnapshot snapshot;
    snapshot.worldToView = *modelView;
    snapshot.projection = *projection;
    snapshot.x = viewport->x();
    snapshot.y = viewport->y();
    snapshot.width = viewport->width();
    snapshot.height = viewport->height();
    snapshot.devicePixelRatio = devicePixelRatio;
    snapshot.frame = stamp->getFrameNumber();
    snapshot.time = stamp->getReferenceTime();
    if (!snapshot.valid())
        return false;
    output = snapshot;
    return true;
}

bool Request::valid(const Limits& limits) const
{
    if (sequence == 0 || configuration == 0 || !camera.valid() || batches.size() > limits.batches ||
        !std::isfinite(declutterPadding) || (declutterPadding < 0 && declutterPadding != -1) ||
        !std::isfinite(adoptedTime) || (animate && displayedSequence >= sequence))
        return false;
    std::size_t bytes = sizeof(Request) + (sceneVisibility ? sizeof(SceneVisibility) : 0);
    if (bytes > limits.inputBytes || batches.capacity() > (limits.inputBytes - bytes) / sizeof(BatchPtr))
        return false;
    bytes += batches.capacity() * sizeof(BatchPtr);
    std::size_t records = 0;
    for (const auto& batch : batches)
    {
        // Count shared batches conservatively per snapshot, without walking any feature records in cull.
        if (!batch || batch->bytes() > limits.inputBytes - bytes ||
            batch->records().size() > limits.records - records)
            return false;
        bytes += batch->bytes();
        records += batch->records().size();
    }
    return true;
}

std::vector<Placement> osgEarthPrestige::Labeling::project(const Request& request, const std::atomic<bool>& canceled)
{
    std::vector<Placement> output;
    std::size_t count = 0;
    for (const auto& batch : request.batches)
        count += batch->records().size();
    output.reserve(count);
    const auto matrix = request.camera.worldToView * request.camera.projection;
    const auto eye = osg::Matrixd::inverse(request.camera.worldToView).getTrans();
    for (std::size_t b = 0; b < request.batches.size(); ++b)
    {
        const auto& records = request.batches[b]->records();
        for (std::size_t r = 0; r < records.size(); ++r)
        {
            if ((r & 255) == 0 && canceled.load())
                return {};
            const auto& record = records[r];
            if (request.sceneVisibility && record.key.source == request.sceneVisibility->source &&
                request.sceneVisibility->ids[record.key.feature % 1024] != record.key.feature) continue;
            if (record.path)
            {
                Placement placement;
                placement.batch = b; placement.record = r;
                if (placePath(record, request.camera, placement)) output.push_back(std::move(placement));
                continue;
            }
            if (request.camera.geocentric)
            {
                const auto& anchor = record.worldAnchor;
                const osg::Vec3d normal(anchor.x(), anchor.y(), anchor.z() * 1.006739496742);
                if (normal * (eye - anchor) <= 0.0)
                    continue;
            }
            const osg::Vec4d clip = osg::Vec4d(record.worldAnchor, 1.0) * matrix;
            if (!std::isfinite(clip.x()) || !std::isfinite(clip.y()) || !std::isfinite(clip.z()) ||
                !std::isfinite(clip.w()) || clip.w() <= 0.0 || std::abs(clip.x()) > clip.w() ||
                std::abs(clip.y()) > clip.w())
                continue;
            // These are screen overlays: like the glyph shader, depth-range changes do not affect eligibility.
            // Horizon/behind-camera tests still apply; terrain occlusion and explicit distance policy are separate.
            Placement placement;
            placement.batch = b;
            placement.record = r;
            placement.anchorPixels.set(
                request.camera.x + (clip.x() / clip.w() + 1.0) * 0.5 * request.camera.width,
                request.camera.y + (clip.y() / clip.w() + 1.0) * 0.5 * request.camera.height);
            placement.lowerLeft = placement.anchorPixels + record.lowerLeft * request.camera.devicePixelRatio;
            placement.upperRight = placement.anchorPixels + record.upperRight * request.camera.devicePixelRatio;
            if (std::isfinite(placement.lowerLeft.x()) && std::isfinite(placement.lowerLeft.y()) &&
                std::isfinite(placement.upperRight.x()) && std::isfinite(placement.upperRight.y()))
                output.emplace_back(placement);
        }
    }
    return output;
}

View::View(ViewKey key, const Limits& limits) : _key(key), _limits(limits) { }

std::uint64_t View::invalidate()
{
    return ++_configuration;
}

bool View::submit(std::unique_ptr<const Request>& request)
{
    if (!request || !(request->view == _key) || !request->valid(_limits))
        return false;
    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
    if (!lock || _closed.load() || request->configuration != _configuration.load() ||
        request->sequence <= _lastSequence)
        return false;
    ResultPtr displayed;
    if (request->animate && request->displayedSequence != 0)
    {
        for (const auto& result : _results)
            if (result && result->input->sequence == request->displayedSequence)
                displayed = result;
        if (!displayed) return false;
    }
    if (_pending)
    {
        auto free = std::find_if(_retired.begin(), _retired.end(), [](const auto& item) { return !item; });
        if (free == _retired.end())
            return false;
        _retiredDisplays[std::distance(_retired.begin(), free)] = std::move(_pendingDisplay);
        *free = std::move(_pending);
    }
    _lastSequence = request->sequence;
    _pending = std::move(request);
    _pendingDisplay = std::move(displayed);
    return true;
}

ResultPtr View::acquire(std::uint64_t afterSequence) const
{
    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
    if (!lock || _closed.load() || _published < 0)
        return {};
    const auto& result = _results[_published];
    if (result->input->configuration != _configuration.load() || result->input->sequence <= afterSequence)
        return {};
    return result;
}

bool View::pendingOrNew(std::uint64_t afterSequence) const
{
    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
    if (!lock) return true;
    if (_closed.load()) return false;
    return bool(_pending) || _solving || (_published >= 0 &&
        _results[_published]->input->configuration == _configuration.load() &&
        _results[_published]->input->sequence > afterSequence);
}

void View::reclaim()
{
    // Locals intentionally outlive the lock: releasing a batch can recursively destroy substantial resources.
    std::array<std::unique_ptr<const Request>, 3> requests;
    std::array<ResultPtr, 4> results;
    std::array<ResultPtr, 3> displayed;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (std::size_t i = 0; i < _retired.size(); ++i)
        {
            requests[i] = std::move(_retired[i]);
            displayed[i] = std::move(_retiredDisplays[i]);
        }
        if (_closed.load())
        {
            requests.back() = std::move(_pending);
            displayed.back() = std::move(_pendingDisplay);
            _published = -1;
        }
        else if (_published >= 0 && _results[_published]->input->configuration != _configuration.load())
            _published = -1;
        for (std::size_t i = 0; i < _results.size(); ++i)
            if (static_cast<int>(i) != _published && _results[i] && _results[i].use_count() == 1)
                results[i] = std::move(_results[i]);
    }
}

bool View::solve(const Solver& solver, const PrepareResult& prepare)
{
    std::unique_ptr<const Request> request;
    ResultPtr previous;
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_closed.load() || !_pending)
            return false;
        for (std::size_t i = 0; i < _results.size(); ++i)
            if (!_results[i])
            {
                slot = static_cast<int>(i);
                break;
            }
        // Slow draws may pin every result slot. Keep only the latest pending input until capacity returns.
        if (slot < 0)
            return false;
        request = std::move(_pending);
        _solving = true;
        if (request->animate)
            previous = std::move(_pendingDisplay);
        else if (_published >= 0)
            previous = _results[_published];
    }
    if (request->configuration != _configuration.load())
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _solving = false;
        return true;
    }
    try
    {
        auto result = std::make_shared<Result>();
        result->input = std::move(request);
        const auto start = std::chrono::steady_clock::now();
        auto options = _limits.declutter;
        if (result->input->declutterPadding >= 0) options.padding = result->input->declutterPadding;
        result->placements = solver ? solver(*result->input, _closed) :
            declutter(*result->input, _closed, previous.get(), options, &result->layout);
        stabilize(*result, previous.get(), _limits);
        result->layout.milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (prepare && !_closed.load() && result->input->configuration == _configuration.load())
            prepare(*result);
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_closed.load() && result->input->configuration == _configuration.load())
        {
            _results[slot] = std::move(result);
            _published = slot;
        }
    }
    catch (...)
    {
        ++_failures;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _solving = false;
    }
    return true;
}

struct Engine::State
{
    struct Slot
    {
        std::uint64_t generation = 0;
        std::shared_ptr<View> view;
    };
    const Limits limits;
    const Solver solver;
    const PrepareResult prepare;
    std::vector<Slot> slots;
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> stopped { false };
    std::thread worker;

    //! Allocates registry capacity once, before starting the worker.
    State(const Limits& value, Solver function, PrepareResult preparation) :
        limits(value), solver(std::move(function)), prepare(std::move(preparation)), slots(value.views) { }

    //! Services one bounded solve per view per pass; idle polling also detects released draw-frame pins.
    void run()
    {
        osgEarth::setThreadName("oe.labels");
        std::vector<std::shared_ptr<View>> views(slots.size());
        while (!stopped.load())
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (std::size_t i = 0; i < slots.size(); ++i)
                    views[i] = slots[i].view;
            }
            bool worked = false;
            for (auto& view : views)
            {
                if (!view)
                    continue;
                view->reclaim();
                if (!stopped.load())
                    worked = view->solve(solver, prepare) || worked;
                // Retired registrations remain bounded and keep frame resources worker-owned until unpinned.
                if (view->_closed.load())
                {
                    bool empty;
                    {
                        std::lock_guard<std::mutex> lock(view->_mutex);
                        empty = !view->_pending;
                        for (const auto& request : view->_retired)
                            empty = empty && !request;
                        for (const auto& result : view->_results)
                            empty = empty && !result;
                    }
                    if (empty)
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        auto& slot = slots[view->_key.slot];
                        if (slot.view == view)
                            slot.view.reset();
                    }
                }
                view.reset();
            }
            if (!worked)
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(8), [&]() { return stopped.load(); });
            }
        }
        // Terminal teardown has no live traversal; release all unpinned resources on this worker as well.
        for (auto& slot : slots)
            if (slot.view)
                slot.view->reclaim();
    }
};

Engine::Engine(const Limits& limits, Solver solver, PrepareResult prepare)
{
    if (limits.views == 0 || limits.views > std::numeric_limits<std::uint32_t>::max() ||
        limits.batches == 0 || limits.records == 0 || limits.inputBytes < sizeof(Request) ||
        limits.declutter.placements == 0 || limits.declutter.placements > 16384 ||
        limits.declutter.collisionTests == 0 || !std::isfinite(limits.declutter.padding) || limits.declutter.padding < 0)
        throw std::invalid_argument("Label engine requires finite, nonzero limits");
    _state = std::make_unique<State>(limits, std::move(solver), std::move(prepare));
    _state->worker = std::thread([this]() { _state->run(); });
}

Engine::~Engine()
{
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        for (auto& slot : _state->slots)
            if (slot.view)
                slot.view->_closed.store(true);
        _state->stopped.store(true);
    }
    _state->wake.notify_all();
    _state->worker.join();
}

std::shared_ptr<View> Engine::registerView()
{
    std::lock_guard<std::mutex> lock(_state->mutex);
    if (_state->stopped.load())
        return {};
    for (std::size_t i = 0; i < _state->slots.size(); ++i)
    {
        auto& slot = _state->slots[i];
        if (!slot.view)
        {
            slot.view = std::shared_ptr<View>(new View({ static_cast<std::uint32_t>(i), ++slot.generation }, _state->limits));
            return slot.view;
        }
    }
    return {};
}

void Engine::removeView(const std::shared_ptr<View>& view)
{
    if (!view)
        return;
    std::lock_guard<std::mutex> lock(_state->mutex);
    if (view->key().slot < _state->slots.size() && _state->slots[view->key().slot].view == view)
        view->_closed.store(true);
}
