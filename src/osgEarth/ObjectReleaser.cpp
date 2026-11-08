/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "ObjectReleaser"
#include "Threading"
#include "Metrics"
#include <condition_variable>
#include <deque>
#include <algorithm>
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
#include <Superluminal/PerformanceAPI.h>
#endif

using namespace osgEarth;

struct ObjectReleaser::Queue
{
    std::string label;
    std::size_t pending = 0;
};

struct ObjectReleaser::State
{
    struct Batch
    {
        std::shared_ptr<Queue> queue;
        Objects objects;
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Batch> batches;
    jobs::context job;
    std::size_t pending = 0;
    bool running = false;
    bool stopped = false;
    static thread_local State* worker;
    static thread_local std::vector<Queue*> active;

    //! Destroys one batch without the queue lock, permitting nested paging-owner teardown.
    void dispose(Batch& batch)
    {
        active.push_back(batch.queue.get());
        {
            OE_PROFILING_ZONE_NAMED("Destroy retired objects");
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
            PERFORMANCEAPI_INSTRUMENT_DATA("Destroy retired objects", batch.queue->label.c_str());
#endif
            batch.objects.clear();
        }
        active.pop_back();
        {
            std::lock_guard<std::mutex> lock(mutex);
            --batch.queue->pending;
            --pending;
        }
        changed.notify_all();
    }

    //! Runs the sole dispatcher until empty; only the dispatcher owns the running flag.
    void run()
    {
        State* previous = worker;
        worker = this;
        for (;;)
        {
            Batch batch;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (batches.empty())
                {
                    running = false;
                    break;
                }
                batch = std::move(batches.front());
                batches.pop_front();
            }
            dispose(batch);
        }
        worker = previous;
        changed.notify_all();
    }

    //! Waits for a client (or all clients); worker-side recursion pumps work instead of self-joining.
    void drain(Queue* queue)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (worker != this)
        {
            changed.wait(lock, [&]() { return queue ? queue->pending == 0 : pending == 0 && !running; });
            return;
        }

        const auto executing = queue ? std::count(active.begin(), active.end(), queue) : active.size();
        while ((queue ? queue->pending : pending) > std::size_t(executing))
        {
            Batch batch = std::move(batches.front());
            batches.pop_front();
            lock.unlock();
            dispose(batch);
            lock.lock();
        }
    }
};

thread_local ObjectReleaser::State* ObjectReleaser::State::worker = nullptr;
thread_local std::vector<ObjectReleaser::Queue*> ObjectReleaser::State::active;

ObjectReleaser::ObjectReleaser() : _state(std::make_shared<State>())
{
    _state->job.name = "Destroy retired objects";
    _state->job.pool = jobs::get_pool("oe.dispose", 1);
    _state->job.pool->set_can_steal_work(false);
}

ObjectReleaser::~ObjectReleaser()
{
    shutdown();
}

ObjectReleaser::Client ObjectReleaser::createClient(const std::string& label)
{
    Client client;
    client._state = _state;
    client._queue = std::make_shared<Queue>();
    client._queue->label = label;
    return client;
}

bool ObjectReleaser::Client::available() const
{
    if (!_state) return false;
    std::lock_guard<std::mutex> lock(_state->mutex);
    return !_state->stopped && _queue->pending < 2 && _state->pending < 64;
}

void ObjectReleaser::Client::release(Objects& objects) const
{
    if (objects.empty()) return;
    bool dispatch = false;
    if (_state)
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        if (!_state->stopped)
        {
            _state->batches.emplace_back();
            auto& batch = _state->batches.back();
            batch.queue = _queue;
            batch.objects.swap(objects);
            ++_queue->pending;
            ++_state->pending;
            dispatch = !_state->running;
            _state->running = true;
        }
    }
    if (dispatch)
    {
        // Callable copies retain only service state, never a separate copy of the retired payload.
        auto state = _state;
        jobs::dispatch([state]() { state->run(); }, state->job);
    }
    objects.clear(); // Only nonempty for terminal teardown after shutdown or an inactive handle.
}

void ObjectReleaser::Client::drain() const
{
    if (_state) _state->drain(_queue.get());
}

void ObjectReleaser::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->stopped = true;
    }
    _state->drain(nullptr);
}
