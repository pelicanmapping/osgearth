/**
 * weejobs
 * Copyright 2025 Pelican Mapping
 * https://github.com/pelicanmapping/weejobs
 * MIT License
 */
#pragma once
#include <atomic>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>
#include <string>
#include <algorithm>

// OPTIONAL: Define WEEJOBS_EXPORT if you want to use this library from multiple modules (DLLs)
#ifndef WEEJOBS_EXPORT
#define WEEJOBS_EXPORT
#endif

// OPTIONAL: Customize the namespace by defining WEEJOBS_NAMESPACE before including this file.
#ifndef WEEJOBS_NAMESPACE
#define WEEJOBS_NAMESPACE jobs
#endif

// Version
#define WEEJOBS_VERSION_MAJOR 1
#define WEEJOBS_VERSION_MINOR 0
#define WEEJOBS_VERSION_REV   3
#define WEEJOBS_STR_NX(s) #s
#define WEEJOBS_STR(s) WEEJOBS_STR_NX(s)
#define WEEJOBS_COMPUTE_VERSION(major, minor, patch) ((major) * 10000 + (minor) * 100 + (patch))
#define WEEJOBS_VERSION_NUMBER WEEJOBS_COMPUTE_VERSION(WEEJOBS_VERSION_MAJOR, WEEJOBS_VERSION_MINOR, WEEJOBS_VERSION_REV)
#define WEEJOBS_VERSION_STRING WEEJOBS_STR(WEEJOBS_VERSION_MAJOR) "." WEEJOBS_STR(WEEJOBS_VERSION_MINOR) "." WEEJOBS_STR(WEEJOBS_VERSION_REV)

#if __cplusplus >= 201703L
#define WEEJOBS_NO_DISCARD [[nodiscard]]
#else
#define WEEJOBS_NO_DISCARD
#endif

/**
* weejobs is an API for scheduling a task to run in the background.
* Please read the README.md file for more information.
*/
namespace WEEJOBS_NAMESPACE
{
    /**
    * Interface for something that can be canceled
    */
    class cancelable
    {
    public:
        virtual bool canceled() const { return false; }
    };

    namespace detail
    {
        /**
         * Event with a binary signaled state, for multi-threaded sychronization.
         *
         * The event has two states:
         *  "set" means that a call to wait() will not block;
         *  "unset" means that calls to wait() will block until another thread calls set().
         *
         * The event starts out unset.
         *
         * Typical usage: Thread A creates Thread B to run asynchronous code. Thread A
         * then calls wait(), which blocks Thread A. When Thread B is finished, it calls
         * set(). Thread A then wakes up and continues execution.
         *
         * NOTE: ALL waiting threads will wake up when the Event is cleared.
         */
        struct event
        {
        public:
            //! Construct a new event
            event() : _set(false) { }

            //! DTOR
            ~event() {
                _set = false;
                for (int i = 0; i < 255; ++i)  // workaround buggy broadcast
                    _cond.notify_all();
            }

            //! Block until the event is set, then return true.
            inline bool wait() {
                while (!_set) {
                    std::unique_lock<std::mutex> lock(_m);
                    if (!_set)
                        _cond.wait(lock);
                }
                return _set;
            }

            //! Block until the event is set or the timout expires.
            //! Return true if the event has set, otherwise false.
            template<typename T>
            inline bool wait(T timeout) {
                if (!_set) {
                    std::unique_lock<std::mutex> lock(_m);
                    if (!_set)
                        _cond.wait_for(lock, timeout);
                }
                return _set;
            }

            //! Block until the event is set; then reset it.
            inline bool waitAndReset() {
                std::unique_lock<std::mutex> lock(_m);
                if (!_set)
                    _cond.wait(lock);
                _set = false;
                return true;
            }

            //! Set if true, reset if false.
            inline void operator = (bool value) {
                if (value)
                    set();
                else
                    reset();
            }

            //! Set the event state, causing any waiters to unblock.
            inline void set() {
                if (!_set) {
                    std::unique_lock<std::mutex> lock(_m);
                    if (!_set) {
                        _set = true;
                        _cond.notify_all();
                    }
                }
            }

            //! Reset (unset) the event state; new waiters will block until set() is called.
            inline void reset() {
                std::unique_lock<std::mutex> lock(_m);
                _set = false;
            }

            //! Whether the event state is set (waiters will not block).
            inline bool isSet() const {
                return _set;
            }

            //! Synonymous with isSet()
            inline operator bool() const {
                return isSet();
            }

        protected:
            bool _set;
            std::condition_variable_any _cond;
            std::mutex _m; // do not use Mutex, we never want tracking
        };


        /**
         * Sempahore lets N users aquire it and then notifies when the
         * count goes back down to zero.
         */
        class semaphore
        {
        public:
            //! Acquire, increasing the usage count by one
            inline void acquire()
            {
                std::unique_lock<std::mutex> lock(_m);
                ++_count;
            }

            inline void operator ++ () { acquire(); }

            //! Release, decreasing the usage count by one.
            //! When the count reaches zero, joiners will be notified and
            //! the semaphore will reset to its initial state.
            inline void release()
            {
                std::unique_lock<std::mutex> lock(_m);
                _count = std::max(_count - 1, 0);
                if (_count == 0)
                    _cv.notify_all();
            }

            inline void operator -- () { release(); }

            //! Reset to initialize state; this will cause a join to occur
            //! even if no acquisitions have taken place.
            inline void reset()
            {
                std::unique_lock<std::mutex> lock(_m);
                _count = 0;
                _cv.notify_all();
            }

            //! Current count in the semaphore
            inline std::size_t count() const
            {
                std::unique_lock<std::mutex> lock(_m);
                return _count;
            }

            //! Block until the semaphore count returns to zero.
            //! (It must first have left zero)
            //! Warning: this method will block forever if the count
            //! never reaches zero!
            inline void join()
            {
                std::unique_lock<std::mutex> lock(_m);
                while (_count > 0)
                    _cv.wait(lock);
            }

            //! Block until the semaphore count returns to zero, or
            //! the operation is canceled.
            //! (It must first have left zero)
            inline void join(cancelable* c)
            {
                _cv.wait_for(_m, std::chrono::seconds(1), [this, c]() {
                    return
                        (_count == 0) ||
                        (c && c->canceled());
                    }
                );
                _count = 0;
            }

        private:
            int _count = 0;
            std::condition_variable_any _cv;
            mutable std::mutex _m;
        };

#if __cplusplus >= 201703L || _MSVC_LANG >= 201703L
        template<typename F, typename...Args>
        using result_of_t = typename std::invoke_result<F, Args...>::type;
#else
        template<typename F, typename...Args>
        using result_of_t = typename std::result_of<F(Args...)>::type;
#endif
    }

    /**
    * Include a jobgroup in a context to group together multiple jobs.
    * You can then call jobgroup::join() to wait for the whole group
    * to finish.
    */
    struct jobgroup : public detail::semaphore
    {
        static std::shared_ptr<jobgroup> create()
        {
            return std::make_shared<jobgroup>();
        }
    };

    /**
    * Context object you can pass to dispatch(...) to control aspects of
    * how the background task is run.
    */
    struct context
    {
        std::string name; // readable name of the job
        class jobpool* pool = nullptr; // job pool to run in
        std::function<float()> priority = {}; // priority of the job
        std::shared_ptr<jobgroup> group = nullptr; // join group for this job
        bool can_cancel = true; // if true, the job will cancel if its future goes out of scope
    };

    /**
     * Future holds the future result of an asynchronous operation.
     *
     * Usage:
     *   Producer (usually an asynchronous function call) creates a future<T>
     *   (the promise of a future result) and immediately returns it. The Consumer
     *   then performs other work, and eventually (or immediately) checks available()
     *   for a result or canceled() for cancelation. If availabile() is true,
     *   Consumer calls value() to fetch the valid result.
     *
     *   As long as at least two equivalent Future object (i.e. Futures pointing to the
     *   same internal shared data) exist, the Future is considered valid. Once
     *   that count goes to one, the Future is either available (the value is ready)
     *   or empty (i.e., canceled or abandoned).
     */
    template<typename T>
    class future : public cancelable
    {
    private:
        // internal structure to track references to the result
        // One instance of this is shared among all Future instances
        // created from the copy constructor.
        struct shared_t
        {
            T _obj;
            mutable detail::event _ev;
            std::mutex _continuation_mutex;
            std::function<void()> _continuation;
            std::atomic_bool _continuation_ran = { false };
        };

    public:
        //! Default constructor
        future()
        {
            _shared = std::make_shared<shared_t>();
        }

        //! Default copy constructor
        future(const future& rhs) = default;

        //! True is this Future is unused and not connected to any other Future
        bool empty() const
        {
            return !available() && _shared.use_count() == 1;
        }

        //! True if the promise was resolved and a result if available.
        bool available() const
        {
            return _shared->_ev.isSet();
        }

        //! True if a promise exists, but has not yet been resolved;
        //! Presumably the asynchronous task is still working.
        bool working() const
        {
            return !empty() && !available();
        }

        // cancelable interface
        bool canceled() const override
        {
            return empty();
        }

        //! Deference the result object. Make sure you check available()
        //! to check that the future was actually resolved; otherwise you
        //! will just get the default object.
        const T& value() const
        {
            return _shared->_obj;
        }

        //! Dereference this object to const pointer to the result.
        const T* operator -> () const
        {
            return &_shared->_obj;
        }

        //! Result is available AND equal to the argument.
        bool has_value(const T& arg) const
        {
            return available() && value() == arg;
        }

        //! Same as value(), but if the result is available will reset the
        //! future before returning the result object.
        T release()
        {
            bool avail = available();
            T result = value();
            if (avail)
                reset();
            return result;
        }

        //! Blocks until the result becomes available or the future is abandoned;
        //! then returns the result object.
        const T& join() const
        {
            while (
                !empty() &&
                !_shared->_ev.wait(std::chrono::milliseconds(1)));
            return value();
        }

        //! Blocks until the result becomes available or the future is abandoned
        //! or a cancelation flag is set; then returns the result object. Be sure to
        //! check canceled() after calling join() to see if the return value is valid.
        const T& join(const cancelable* p) const
        {
            while (working() && (p == nullptr || !p->canceled()))
            {
                _shared->_ev.wait(std::chrono::milliseconds(1));
            }
            return value();
        }

        //! Blocks until the result becomes available or the future is abandoned
        //! or a cancelation flag is set; then returns the result object. Be sure to
        //! check canceled() after calling join() to see if the return value is valid.
        const T& join(const cancelable& p) const
        {
            return join(&p);
        }

        //! Release reference to a promise, resetting this future to its default state
        void abandon()
        {
            _shared.reset(new shared_t());
        }

        //! synonym for abandon.
        void reset()
        {
            abandon();
        }

        //! Resolve (fulfill) the promise with the provided result value.
        void resolve(const T& value)
        {
            _shared->_obj = value;
            _shared->_ev.set();
            fire_continuation();
        }

        //! Resolve (fulfill) the promise with an rvalue
        void resolve(T&& value)
        {
            _shared->_obj = std::move(value);
            _shared->_ev.set();
            fire_continuation();
        }

        //! Resolve (fulfill) the promise with a default result
        void resolve()
        {
            _shared->_ev.set();
            fire_continuation();
        }

        //! The number of objects, including this one, that
        //! reference the shared container. If this method
        //! returns 1, that means this is the only object with
        //! access to the data. This method will never return zero.
        unsigned refs() const
        {
            return _shared.use_count();
        }

        //! Add a continuation to this future. The continuation will be dispatched
        //! when this object's result becomes available; that result will be the input
        //! value to the continuation function. The continuation function in turn must
        //! return a value (cannot be void).
        template<typename F, typename R = typename detail::result_of_t<F, const T&, cancelable&>>
        WEEJOBS_NO_DISCARD inline future<R> then_dispatch(F func, const context& con = {});

        //! Add a continuation to this future. Instead of the functor returning a value,
        //! it will instead have the option of resolving the incoming future/promise object.
        //! This is useful for operations that have their own way of running asynchronous code.
        //! Note: for some reason when you use this variant you must specific the template
        //! argument, e.g. result.then<int>(auto i, promise<int> p)
        template<typename R>
        WEEJOBS_NO_DISCARD inline future<R> then_dispatch(std::function<void(const T&, future<R>&)> func, const context& con = {});

        //! Add a continuation to this future. The functor only takes an input value and has no
        //! return value (fire and forget).
        inline void then_dispatch(std::function<void(const T&)> func, const context& con = {});

    private:
        std::shared_ptr<shared_t> _shared;

        void fire_continuation()
        {
            std::lock_guard<std::mutex> lock(_shared->_continuation_mutex);

            if (_shared->_continuation && !_shared->_continuation_ran.exchange(true))
                _shared->_continuation();

            // Zero out the continuation function immediately after running it.
            // This is important because the continuation might hold a reference to a promise
            // that might hamper cancelation.
            _shared->_continuation = nullptr;
        }
    };

    //! in the "promise/future" pattern, we use the same object for both,
    //! but here's an alias for clarity.
    template<class T> using promise = future<T>;

    namespace detail
    {
        struct job
        {
            context ctx;
            std::function<bool()> _delegate;

            bool operator < (const job& rhs) const
            {
                float lp = ctx.priority ? ctx.priority() : -FLT_MAX;
                float rp = rhs.ctx.priority ? rhs.ctx.priority() : -FLT_MAX;
                return lp < rp;
            }
        };

        inline bool steal_job(class jobpool* thief, detail::job& stolen);
    }

    /**
    * A priority-sorted collection of jobs that are running or waiting
    * to run in a thread pool.
    */
    class jobpool
    {
    public:
        /**
        * Metrics of a thread pool.
        */
        struct metrics_t
        {
            std::string name;
            std::atomic_uint concurrency = { 0u };
            std::atomic_uint pending = { 0u };
            std::atomic_uint running = { 0u };
            std::atomic_uint postprocessing = { 0u };
            std::atomic_uint canceled = { 0u };
            std::atomic_uint total = { 0u };

            //! Whether this pool's counts appear in the total metrics counts
            bool visible = true;
        };

    public:
        //! Destroy
        ~jobpool()
        {
            stop_threads();
        }

        //! Name of this job pool
        const std::string& name() const
        {
            return _metrics.name;
        }

        metrics_t* metrics()
        {
            return &_metrics;
        }

        //! Set the concurrency of this job scheduler
        void set_concurrency(unsigned value)
        {
            value = std::max(value, 1u);
            if (_target_concurrency != value)
            {
                _target_concurrency = value;
                start_threads();
            }
        }

        //! Get the target concurrency (thread count) 
        unsigned concurrency() const
        {
            return _target_concurrency;
        }

        //! Whether this job pool is allowed to steal work from other job pools
        //! when it is idle. Default = true.
        void set_can_steal_work(bool value)
        {
            _can_steal_work = value;
        }

        //! Discard all queued jobs
        void cancel_all()
        {
            std::lock_guard<std::mutex> lock(_queue_mutex);
            _queue.clear();
            _metrics.canceled += _metrics.pending;
            _metrics.pending = 0;
        }

        //! Schedule an asynchronous task on this scheduler
        //! Use job::dispatch to run jobs (usually no need to call this directly)
        //! @param delegate Function to execute
        //! @param context Job details
        void _dispatch_delegate(std::function<bool()>& delegate, const context& context)
        {
            if (!_done)
            {
                // If we have a group semaphore, acquire it BEFORE queuing the job
                if (context.group)
                {
                    context.group->acquire();
                }

                if (_target_concurrency > 0)
                {
                    std::lock_guard<std::mutex> lock(_queue_mutex);

                    _queue.emplace_back(detail::job{ context, delegate });

                    _metrics.pending++;
                    _metrics.total++;
                    _block.notify_one();
                }
                else
                {
                    // no threads? run synchronously.
                    delegate();

                    if (context.group)
                    {
                        context.group->release();
                    }
                }
            }
        }

        //! removes the highest priority job from the queue and places it
        //! in output. Returns true if a job was taken, false if the queue
        //! was empty.
        inline bool _take_job(detail::job& output, bool lock)
        {
            if (lock)
            {
                std::lock_guard<std::mutex> lock(_queue_mutex);
                return _take_job(output, false);
            }
            else if (!_done && !_queue.empty())
            {
                auto ptr = _queue.end();
                float highest_priority = -FLT_MAX;
                for (auto iter = _queue.begin(); iter != _queue.end(); ++iter)
                {
                    float priority = iter->ctx.priority != nullptr ?
                        iter->ctx.priority() :
                        0.0f;

                    if (ptr == _queue.end() || priority > highest_priority)
                    {
                        ptr = iter;
                        highest_priority = priority;
                    }
                }

                if (ptr == _queue.end())
                    ptr = _queue.begin();

                output = std::move(*ptr);
                if (_queue.size() > 1)
                    *ptr = std::move(_queue.back());
                _queue.resize(_queue.size() - 1);

                _metrics.pending--;
                return true;
            }
            return false;
        }

        //! Construct a new job pool.
        //! Do not call this directly - call getPool(name) instead.
        jobpool(const std::string& name, unsigned concurrency) :
            _target_concurrency(concurrency)
        {
            _metrics.name = name;
            _metrics.concurrency = 0;
            _queue.reserve(256);
        }

        //! Pulls queued jobs and runs them in whatever thread run() is called from.
        //! Runs in a loop until _done is set.
        inline void run();

        //! Spawn all threads in this scheduler
        inline void start_threads();

        //! Signall all threads to stop
        inline void stop_threads();

        //! Wait for all threads to exit (after calling stop_threads)
        inline void join_threads();

        //! Blocks until all pending work is finished.
        inline void finish_work();

        std::atomic<bool> _can_steal_work = { true };
        std::vector<detail::job> _queue;
        mutable std::mutex _queue_mutex; // protect access to the queue
        std::atomic<unsigned> _target_concurrency; // target number of concurrent threads in the pool
        std::condition_variable_any _block; // thread waiter block
        std::atomic<bool> _done = { false }; // set to true when threads should exit
        std::vector<std::thread> _threads; // threads in the pool
        metrics_t _metrics; // metrics for this pool
    };

    class metrics
    {
    public:
        //! Total number of pending jobs across all schedulers
        int total_pending() const;

        //! Total number of running jobs across all schedulers
        int total_running() const;

        //! Total number of running jobs across all schedulers
        int total_postprocessing() const;

        //! Total number of canceled jobs across all schedulers
        int total_canceled() const;

        //! Total number of active jobs in the system
        int total() const;

        //! Gets a vector of all jobpool metrics structures.
        inline const std::vector<struct jobpool::metrics_t*> all()
        {
            return _pools;
        }

        std::vector<struct jobpool::metrics_t*> _pools;
    };

    /**
    * Runtime singleton object;
    * Declare with WEEJOBS_INSTANCE in one of your .cpp files.
    */
    namespace detail
    {
        struct runtime
        {
            inline runtime();
            inline ~runtime();
            inline void shutdown();

            std::atomic<bool> _alive = { true };
            std::atomic<bool> _stealing_allowed = { false };
            std::mutex _pools_mutex;
            std::vector<jobpool*> _pools;
            metrics _metrics;
            std::function<void(const char*)> _set_thread_name;
        };
    }

    //! Access to the runtime singleton - users need not call this
    extern WEEJOBS_EXPORT detail::runtime& instance();

    //! Returns the job pool with the given name, creating a new one if it doesn't 
    //! already exist. If you don't specify a name, a default pool is used.
    //! @param name Name of the pool to fetch (or create)
    //! @param pool_size Number of threads in the pool (if it's a new pool)
    //! @return Pointer to the job pool
    inline jobpool* get_pool(const std::string& name = {}, unsigned pool_size = 2u)
    {
        std::lock_guard<std::mutex> lock(instance()._pools_mutex);

        for (auto pool : instance()._pools)
        {
            if (pool->name() == name)
                return pool;
        }
        auto new_pool = new jobpool(name, pool_size);
        instance()._pools.push_back(new_pool);
        instance()._metrics._pools.push_back(&new_pool->_metrics);
        new_pool->start_threads();
        return new_pool;
    }

    namespace detail
    {
        // dispatches a function to the appropriate job pool.
        inline void pool_dispatch(std::function<bool()> delegate, const context& context)
        {
            auto pool = context.pool ? context.pool : get_pool({});
            if (pool)
            {
                pool->_dispatch_delegate(delegate, context);

                // if work stealing is enabled, wake up all pools
                if (instance()._stealing_allowed)
                {
                    std::lock_guard<std::mutex> lock(instance()._pools_mutex);

                    for (auto pool : instance()._pools)
                    {
                        pool->_block.notify_all();
                    }
                }
            }
        }
    }

    //! Dispatches a job with no return value. Fire and forget.
    //! @param task Function to run in a thread. Prototype is void(void).
    //! @param context Optional configuration for the asynchronous function call
    inline void dispatch(std::function<void()> task, const context& context = {})
    {
        auto delegate = [task]() mutable -> bool { task(); return true; };
        detail::pool_dispatch(delegate, context);
    }

    //! Dispatches a job and immediately returns a future result.
    //! @param task Function to run in a thread. Prototype is T(cancelable&)
    //! @param context Optional configuration for the asynchronous function call
    //! @return Future result of the async function call
    template<typename F, typename T = typename detail::result_of_t<F, cancelable&>>
    WEEJOBS_NO_DISCARD inline future<T> dispatch(F task, const context& context = {})
    {
        future<T> promise;
        bool can_cancel = context.can_cancel;

        std::function<bool()> delegate = [task, promise, can_cancel]() mutable
            {
                bool good = true;
                if (can_cancel)
                {
                    good = !promise.canceled();
                    if (good)
                        promise.resolve(task(promise));
                }
                else
                {
                    cancelable dummy;
                    promise.resolve(task(dummy));
                }
                return good;
            };

        detail::pool_dispatch(delegate, context);

        return promise;
    }

    //! Dispatches a job and immediately returns a future result.
    //! @param task Function to run in a thread. Prototype is T(cancelable&)
    //! @param promise Optional user-supplied promise object
    //! @param context Optional configuration for the asynchronous function call
    //! @return Future result of the async function call
    template<typename F, typename T = typename detail::result_of_t<F, cancelable&>>
    WEEJOBS_NO_DISCARD inline future<T> dispatch(F task, future<T> promise, const context& context = {})
    {
        bool can_cancel = context.can_cancel;

        std::function<bool()> delegate = [task, promise, can_cancel]() mutable
            {
                bool run = !can_cancel || !promise.canceled();
                if (run)
                {
                    task(promise);
                }
                return run;
            };

        detail::pool_dispatch(delegate, context);

        return promise;
    }

    //! Metrics for all job pool
    inline metrics* get_metrics()
    {
        return &instance()._metrics;
    }

    //! stop all threads, wait for them to exit, and shut down the system
    inline void shutdown()
    {
        instance().shutdown();
    }

    //! Whether the weejobs runtime is still alive (has not been shutdown)
    inline bool alive()
    {
        return instance()._alive;
    }

    //! Install a function that the SDK can use to set job pool thread names
    //! when it spawns them.
    inline void set_thread_name_function(std::function<void(const char*)> f)
    {
        instance()._set_thread_name = f;
    }

    //! Whether to allow jobpools to steal work from other jobpools when they are idle.
    inline void set_allow_work_stealing(bool value)
    {
        instance()._stealing_allowed = value;
    }

    inline detail::runtime::runtime()
    {
        //nop
    }

    inline detail::runtime::~runtime()
    {
        shutdown();
    }

    inline void detail::runtime::shutdown()
    {
        _alive = false;

        //std::cout << "stopping " << _pools.size() << " threads..." << std::endl;
        for (auto& pool : _pools)
            if (pool)
                pool->stop_threads();

        //std::cout << "joining " << _pools.size() << " threads..." << std::endl;
        for (auto& pool : _pools)
            if (pool)
                pool->join_threads();

        for (auto& pool : _pools)
            if (pool)
                delete pool;

        _pools.clear();
    }

    inline void jobpool::run()
    {
        while (!_done)
        {
            detail::job next;
            bool have_next = false;
            {
                if (_can_steal_work && instance()._stealing_allowed)
                {
                    {
                        std::unique_lock<std::mutex> lock(_queue_mutex);

                        // work-stealing enabled: wait until any queue is non-empty
                        _block.wait(lock, [this]() { return (_metrics.pending > 0) || _done; });

                        if (!_done && !_queue.empty())
                        {
                            have_next = _take_job(next, false);
                        }
                    }

                    if (!_done && !have_next)
                    {
                        have_next = detail::steal_job(this, next);
                    }
                }
                else
                {
                    std::unique_lock<std::mutex> lock(_queue_mutex);

                    // wait until just our local queue is non-empty
                    _block.wait(lock, [this] { return !_queue.empty() || _done; });

                    if (!_done && !_queue.empty())
                    {
                        have_next = _take_job(next, false);
                    }
                }
            }

            if (have_next)
            {
                _metrics.running++;

                auto t0 = std::chrono::steady_clock::now();

                bool job_executed = next._delegate();

                auto duration = std::chrono::steady_clock::now() - t0;

                if (job_executed == false)
                {
                    _metrics.canceled++;
                }

                // release the group semaphore if necessary
                if (next.ctx.group != nullptr)
                {
                    next.ctx.group->release();
                }

                _metrics.running--;
            }

            // See if we no longer need this thread because the
            // target concurrency has been reduced
            if (_target_concurrency < _metrics.concurrency)
            {
                _metrics.concurrency--;
                break;
            }
        }
    }

    inline void jobpool::start_threads()
    {
        _done = false;

        // Not enough? Start up more
        while (_metrics.concurrency < _target_concurrency)
        {
            _metrics.concurrency++;

            _threads.push_back(std::thread([this]
                {
                    if (instance()._set_thread_name)
                    {
                        instance()._set_thread_name(_metrics.name.c_str());
                    }
                    run();
                }
            ));
        }
    }

    inline void jobpool::stop_threads()
    {
        _done = true;

        // Clear out the queue
        std::lock_guard<std::mutex> lock(_queue_mutex);

        // reset any group semaphores so that JobGroup.join()
        // will not deadlock.
        for (auto& queuedjob : _queue)
        {
            if (queuedjob.ctx.group != nullptr)
            {
                queuedjob.ctx.group->release();
            }
        }
        _queue.clear();

        // wake up all threads so they can exit
        _block.notify_all();
    }

    //! Wait for all threads to exit (after calling stop_threads)
    inline void jobpool::join_threads()
    {
        // wait for them to exit
        for (unsigned i = 0; i < _threads.size(); ++i)
        {
            if (_threads[i].joinable())
            {
                _threads[i].join();
            }
        }

        _threads.clear();
    }

    //! Blocks until all pending work is finished.
    inline void jobpool::finish_work()
    {
        while (_metrics.pending > 0 || _metrics.running > 0 || _metrics.postprocessing > 0)
        {
            std::this_thread::yield();
        }
    }

    // steal a job from another jobpool's queue (other than "thief").
    inline bool detail::steal_job(jobpool* thief, detail::job& stolen)
    {
        jobpool* pool_with_most_jobs = nullptr;
        {
            std::lock_guard<std::mutex> lock(instance()._pools_mutex);

            std::size_t max_num_jobs = 0u;
            for (auto pool : instance()._pools)
            {
                if (pool != thief)
                {
                    if (pool->_metrics.pending > max_num_jobs)
                    {
                        max_num_jobs = pool->_metrics.pending;
                        pool_with_most_jobs = pool;
                    }
                }
            }
        }

        if (pool_with_most_jobs)
        {
            return pool_with_most_jobs->_take_job(stolen, true);
        }

        return false;
    }

    template<typename T>
    template<typename F, typename R>
    inline future<R> future<T>::then_dispatch(F func, const context& con)
    {
        // The future result of F. 
        // In this case, the continuation task will return a value that the system will use to resolve the promise.
        future<R> continuation_promise;

        // lock the continuation and set it:
        {
            std::lock_guard<std::mutex> lock(_shared->_continuation_mutex);

            if (_shared->_continuation)
            {
                return {}; // only one continuation allowed
            }

            // take a weak ptr to this future's shared data. If this future goes away we'll still
            // have access to its result.
            std::weak_ptr<shared_t> weak_shared = _shared;

            context copy_of_con = con;

            _shared->_continuation = [func, copy_of_con, weak_shared, continuation_promise]() mutable
                {
                    auto shared = weak_shared.lock();

                    // verify the parent's result is actually available (simulate available())
                    if (shared && shared->_ev.isSet())
                    {
                        // copy it and dispatch it as the input to a new job:
                        T copy_of_value = shared->_obj;

                        // Once this wrapper gets created, note that we now have 2 refereces to the continuation_promise.
                        // To prevent this from hampering cancelation, the continuation fuction is set to nullptr
                        // immediately after being called.
                        auto wrapper = [func, copy_of_value, continuation_promise]() mutable
                            {
                                continuation_promise.resolve(func(copy_of_value, continuation_promise));
                            };

                        jobs::dispatch(wrapper, copy_of_con);
                    }
                };
        }

        // maybe the future is already available?
        if (available())
        {
            fire_continuation();
        }

        return continuation_promise;
    }

    template<typename T>
    template<typename R>
    inline future<R> future<T>::then_dispatch(std::function<void(const T&, future<R>&)> func, const context& con)
    {
        // The future we will return to the caller.
        // Note, the user function "func" is responsible for resolving this promise.
        future<R> continuation_promise;

        // lock the continuation and set it:
        {
            std::lock_guard<std::mutex> lock(_shared->_continuation_mutex);

            if (_shared->_continuation)
            {
                return {}; // only one continuation allowed
            }

            // take a weak ptr to this future's shared data. If this future goes away we'll still
            // have access to its result.
            std::weak_ptr<shared_t> weak_shared = _shared;

            // The user task is responsible for resolving the promise.
            // This continuation executes the user function directly instead of dispatching it
            // to the job pool. This is because we expect the user function to use some external
            // asynchronous mechanism to resolve the promise.
            _shared->_continuation = [func, weak_shared, continuation_promise]() mutable
                {
                    auto shared = weak_shared.lock();
                    if (shared)
                    {
                        func(shared->_obj, continuation_promise);
                    }
                };
        }

        if (available())
        {
            fire_continuation();
        }

        return continuation_promise;
    }

    template<typename T>
    inline void future<T>::then_dispatch(std::function<void(const T&)> func, const context& con)
    {
        // lock the continuation and set it:
        {
            std::lock_guard<std::mutex> lock(_shared->_continuation_mutex);

            if (_shared->_continuation)
            {
                return; // only one continuation allowed
            }

            // take a weak ptr to this future's shared data. If this future goes away we'll still
            // have access to its result.
            std::weak_ptr<shared_t> weak_shared = _shared;
            auto copy_of_con = con;

            _shared->_continuation = [func, weak_shared, copy_of_con]() mutable
                {
                    auto shared = weak_shared.lock();
                    if (shared)
                    {
                        auto copy_of_value = shared->_obj;
                        auto fire_and_forget_delegate = [func, copy_of_value]() mutable
                            {
                                func(copy_of_value);
                                return true;
                            };

                        detail::pool_dispatch(fire_and_forget_delegate, copy_of_con);
                    }
                };
        }

        if (available())
        {
            fire_continuation();
        }
    }

    //! Total number of pending jobs across all schedulers
    inline int metrics::total_pending() const
    {
        std::lock_guard<std::mutex> lock(instance()._pools_mutex);
        int count = 0;
        for (auto pool : _pools)
            if (pool->visible)
                count += pool->pending;
        return count;
    }

    //! Total number of running jobs across all schedulers
    inline int metrics::total_running() const
    {
        std::lock_guard<std::mutex> lock(instance()._pools_mutex);
        int count = 0;
        for (auto pool : _pools)
            if (pool->visible)
                count += pool->running;
        return count;
    }

    //! Total number of running jobs across all schedulers
    inline int metrics::total_postprocessing() const
    {
        std::lock_guard<std::mutex> lock(instance()._pools_mutex);
        int count = 0;
        for (auto pool : _pools)
            if (pool->visible)
                count += pool->postprocessing;
        return count;
    }

    //! Total number of canceled jobs across all schedulers
    inline int metrics::total_canceled() const
    {
        std::lock_guard<std::mutex> lock(instance()._pools_mutex);
        int count = 0;
        for (auto pool : _pools)
            if (pool->visible)
                count += pool->canceled;
        return count;
    }

    //! Total number of active jobs in the system
    inline int metrics::total() const
    {
        std::lock_guard<std::mutex> lock(instance()._pools_mutex);
        int count = 0;
        for (auto pool : _pools)
            if (pool->visible)
                count += pool->pending + pool->running + pool->postprocessing;
        return count;
    }

    // Use this macro ONCE in your application in a .cpp file to 
    // instaniate the weejobs runtime singleton.
#define WEEJOBS_INSTANCE \
    namespace WEEJOBS_NAMESPACE { \
        static detail::runtime runtime_singleton_instance; \
        detail::runtime& instance() { return runtime_singleton_instance; } \
    }
}
