/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgEarth/catch.hpp>
#include <osgEarth/GeoData>
#include <osgEarth/Registry>
#include <osgEarth/MemCache>
#include <osgEarth/Containers>  // For osgEarth::LRUCache
#include <osgEarth/FileUtils>
#include <osgDB/ConvertUTF>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

using namespace osgEarth;

namespace osgEarth { namespace Tests
{
    extern std::string executablePath;
} }

namespace
{
    constexpr const char* SQLITE_PROCESS_TEST_ROOT = "OSGEARTH_SQLITE_PROCESS_TEST_ROOT";
    constexpr unsigned PROCESS_PRODUCERS = 2u;
    constexpr unsigned PROCESS_RECORDS_PER_PRODUCER = 150u;

    class ScopedProcessTestEnvironment
    {
    public:
        explicit ScopedProcessTestEnvironment(const std::string& path)
        {
#ifdef _WIN32
            const std::wstring value = osgDB::convertUTF8toUTF16(path);
            _valid = SetEnvironmentVariableW(
                L"OSGEARTH_SQLITE_PROCESS_TEST_ROOT", value.c_str()) != FALSE;
#else
            _valid = ::setenv(SQLITE_PROCESS_TEST_ROOT, path.c_str(), 1) == 0;
#endif
        }

        ~ScopedProcessTestEnvironment()
        {
#ifdef _WIN32
            SetEnvironmentVariableW(L"OSGEARTH_SQLITE_PROCESS_TEST_ROOT", nullptr);
#else
            ::unsetenv(SQLITE_PROCESS_TEST_ROOT);
#endif
        }

        bool valid() const { return _valid; }

    private:
        bool _valid = false;
    };

    std::string processTestRoot()
    {
#ifdef _WIN32
        const wchar_t* value = ::_wgetenv(L"OSGEARTH_SQLITE_PROCESS_TEST_ROOT");
        return value ? osgDB::convertUTF16toUTF8(value) : std::string();
#else
        const char* value = ::getenv(SQLITE_PROCESS_TEST_ROOT);
        return value ? value : std::string();
#endif
    }

    std::string childPath(const std::string& parent, const std::string& child)
    {
        return osgDB::concatPaths(parent, child);
    }

    unsigned countDatabaseFiles(const std::string& path)
    {
        unsigned result = 0u;
        const osgDB::DirectoryContents contents = osgDB::getDirectoryContents(path);
        for (osgDB::DirectoryContents::const_iterator i = contents.begin(); i != contents.end(); ++i)
        {
            if (osgDB::getFileExtension(*i) == "db")
                ++result;
        }
        return result;
    }

    class TestProcess
    {
    public:
        ~TestProcess()
        {
            if (_running)
                wait();
        }

        TestProcess(const TestProcess&) = delete;
        TestProcess& operator=(const TestProcess&) = delete;
        TestProcess() = default;

        bool start(const std::string& executable, const std::string& testFilter)
        {
#ifdef _WIN32
            const std::wstring wideExecutable = osgDB::convertUTF8toUTF16(executable);
            const std::wstring wideFilter = osgDB::convertUTF8toUTF16(testFilter);
            std::wstring commandLine = L"\"" + wideExecutable + L"\" \"" +
                wideFilter + L"\" --reporter compact";
            std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
            mutableCommand.push_back(L'\0');

            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(
                wideExecutable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0,
                nullptr, nullptr, &startup, &process))
            {
                return false;
            }

            CloseHandle(process.hThread);
            _process = process.hProcess;
            _running = true;
            return true;
#else
            _pid = ::fork();
            if (_pid == 0)
            {
                ::execl(executable.c_str(), executable.c_str(),
                    testFilter.c_str(), "--reporter", "compact",
                    static_cast<char*>(nullptr));
                ::_exit(127);
            }
            _running = _pid > 0;
            return _running;
#endif
        }

        int wait()
        {
            if (!_running)
                return _exitCode;

#ifdef _WIN32
            WaitForSingleObject(_process, INFINITE);
            DWORD exitCode = 1u;
            GetExitCodeProcess(_process, &exitCode);
            CloseHandle(_process);
            _process = nullptr;
            _exitCode = static_cast<int>(exitCode);
#else
            int status = 0;
            if (::waitpid(_pid, &status, 0) < 0)
                _exitCode = 1;
            else if (WIFEXITED(status))
                _exitCode = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                _exitCode = 128 + WTERMSIG(status);
            else
                _exitCode = 1;
            _pid = -1;
#endif
            _running = false;
            return _exitCode;
        }

    private:
        bool _running = false;
        int _exitCode = 1;
#ifdef _WIN32
        HANDLE _process = nullptr;
#else
        pid_t _pid = -1;
#endif
    };

    bool createMarker(const std::string& path)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "ready";
        return output.good();
    }

    bool waitForProcessWriters(const std::string& root)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (osgDB::fileExists(childPath(root, "writer-a.ready")) &&
                osgDB::fileExists(childPath(root, "writer-b.ready")))
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    struct TemporaryCachePath
    {
        TemporaryCachePath()
        {
            static std::atomic_uint s_counter{ 0u };
            const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
            path = childPath(Util::getTempPath(),
                ("osgearth_sqlite_cache_test_" + std::to_string(ticks) + "_" +
                 std::to_string(s_counter.fetch_add(1u))));
        }

        ~TemporaryCachePath()
        {
            Util::removeDirectory(path);
        }

        std::string path;
    };

    osg::ref_ptr<Cache> createSQLiteCache(
        const std::string& path,
        unsigned threads,
        bool separateBins)
    {
        Config config;
        config.set("path", path);
        config.set("threads", threads);
        config.set("separate_bins", separateBins);
        config.set("reader_connections", 4u);
        config.set("batch_size", 32u);
        CacheOptions options(config);
        options.setDriver("sqlite3");
        return CacheFactory::create(options);
    }

    osg::ref_ptr<Cache> createFileSystemCache(
        const std::string& path,
        unsigned threads,
        bool stats = false,
        const std::string& statsPath = std::string())
    {
        Config config;
        config.set("path", path);
        config.set("threads", threads);
        config.set("stats", stats);
        config.set("stats_interval", 0u);
        if (!statsPath.empty())
            config.set("stats_path", statsPath);
        CacheOptions options(config);
        options.setDriver("filesystem");
        return CacheFactory::create(options);
    }

    osg::ref_ptr<Cache> createInstrumentedCache(
        const std::string& driver,
        const std::string& path,
        const std::string& statsPath)
    {
        Config config;
        config.set("path", path);
        config.set("threads", 0u);
        config.set("separate_bins", false);
        config.set("stats", true);
        config.set("stats_path", statsPath);
        config.set("stats_interval", 0u);
        CacheOptions options(config);
        options.setDriver(driver);
        return CacheFactory::create(options);
    }

    bool writeString(CacheBin* bin, const std::string& key, const std::string& value)
    {
        osg::ref_ptr<StringObject> object = new StringObject(value);
        return bin->write(key, object.get(), nullptr);
    }

    bool readStringEquals(CacheBin* bin, const std::string& key, const std::string& expected)
    {
        ReadResult result = bin->readString(key, nullptr);
        return result.succeeded() && result.getString() == expected;
    }

    void requireCachePathCanBeRemoved(const std::string& path)
    {
        REQUIRE(Util::removeDirectory(path));
        REQUIRE_FALSE(osgDB::fileExists(path));
    }

    void runSQLiteProcessWriter(const std::string& workerName)
    {
        const std::string root = processTestRoot();
        REQUIRE_FALSE(root.empty());

        osg::ref_ptr<Cache> cache = createSQLiteCache(childPath(root, "cache"), 2u, false);
        REQUIRE(cache.valid());
        REQUIRE_FALSE(cache->getStatus().isError());
        osg::ref_ptr<CacheBin> bin = cache->addBin("process-shared");
        REQUIRE(bin.valid());

        REQUIRE(createMarker(childPath(root, workerName + ".ready")));
        const std::string startMarker = childPath(root, "start");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!osgDB::fileExists(startMarker) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(osgDB::fileExists(startMarker));

        std::atomic_bool succeeded{ true };
        std::vector<std::thread> producers;
        for (unsigned producer = 0u; producer < PROCESS_PRODUCERS; ++producer)
        {
            producers.emplace_back([&, producer]()
            {
                for (unsigned record = 0u; record < PROCESS_RECORDS_PER_PRODUCER; ++record)
                {
                    const std::string key = workerName + "-" + std::to_string(producer) +
                        "-" + std::to_string(record);
                    if (!writeString(bin.get(), key, key))
                        succeeded = false;
                }
            });
        }
        for (auto& producer : producers)
            producer.join();
        REQUIRE(succeeded.load());

        bin = nullptr;
        cache = nullptr; // drain and commit before the process reports success
    }
}

TEST_CASE("SQLite3 process writer A", "[.sqlite3-process-writer-a]")
{
    runSQLiteProcessWriter("writer-a");
}

TEST_CASE("SQLite3 process writer B", "[.sqlite3-process-writer-b]")
{
    runSQLiteProcessWriter("writer-b");
}

TEST_CASE("Cache")
{
    // Get the cache
    osg::ref_ptr<Cache> cache = new MemCache();
    REQUIRE(cache.valid());

    // Open a bin:
    osg::ref_ptr<CacheBin> bin = cache->addBin("test_bin");
    REQUIRE(bin.valid());

    SECTION("String")
    {
        std::string key("string_key");
        std::string value("What is the sound of one hand clapping?");
        osg::ref_ptr<StringObject> s = new StringObject(value);

        // Write a string to the cache
        REQUIRE(bin->write(key, s.get(), 0L));

        // Read the string from the cache
        ReadResult r = bin->readString(key, 0L);
        REQUIRE(r.succeeded());
        REQUIRE(r.getString() == value);

        // Remove the string
        REQUIRE(bin->remove(key));

        // Verify it was removed
        ReadResult r2 = bin->readString(key, 0L);
        REQUIRE(r2.failed());
    }

    SECTION("Image")
    {
        std::string key("image_key");
        osg::ref_ptr<osg::Image> image = ImageUtils::createOnePixelImage(osg::Vec4(1, 0, 0, 1));

        // Write an image to the cache
        REQUIRE(bin->write(key, image.get(), 0L));

        // Read it back
        ReadResult r = bin->readImage(key, 0L);
        REQUIRE(r.succeeded());
        REQUIRE(ImageUtils::areEquivalent(r.getImage(), image.get()));

        // Remove it
        REQUIRE(bin->remove(key));

        // Confirm removal
        ReadResult r2 = bin->readImage(key, 0L);
        REQUIRE(r2.failed());
    }
}

TEST_CASE("SQLite3 cache concurrency and lifetime", "[cache][sqlite3]")
{
    SECTION("zero writer threads is synchronous and closes all SQLite resources")
    {
        TemporaryCachePath root;
        osg::ref_ptr<Cache> cache = createSQLiteCache(root.path, 0u, false);
        REQUIRE(cache.valid());
        REQUIRE_FALSE(cache->getStatus().isError());

        osg::ref_ptr<CacheBin> bin = cache->addBin("sync");
        REQUIRE(bin.valid());
        REQUIRE(writeString(bin.get(), "answer", "forty-two"));
        REQUIRE(readStringEquals(bin.get(), "answer", "forty-two"));

        bin = nullptr;
        cache = nullptr;

        cache = createSQLiteCache(root.path, 0u, false);
        REQUIRE(cache.valid());
        bin = cache->addBin("sync");
        REQUIRE(readStringEquals(bin.get(), "answer", "forty-two"));
        REQUIRE(bin->remove("answer"));
        REQUIRE(bin->readString("answer", nullptr).failed());

        bin = nullptr;
        cache = nullptr;
        requireCachePathCanBeRemoved(root.path);
    }

    SECTION("asynchronous mutations remain ordered and immediately visible")
    {
        TemporaryCachePath root;
        osg::ref_ptr<Cache> cache = createSQLiteCache(root.path, 4u, false);
        REQUIRE(cache.valid());
        osg::ref_ptr<CacheBin> bin = cache->addBin("ordered");
        REQUIRE(bin.valid());

        for (unsigned i = 0u; i < 100u; ++i)
            REQUIRE(writeString(bin.get(), "same-key", std::to_string(i)));

        REQUIRE(readStringEquals(bin.get(), "same-key", "99"));
        REQUIRE(bin->remove("same-key"));
        REQUIRE(bin->readString("same-key", nullptr).failed());

        REQUIRE(writeString(bin.get(), "after-remove", "present"));
        REQUIRE(bin->clear());
        REQUIRE(bin->readString("after-remove", nullptr).failed());

        bin = nullptr;
        cache = nullptr;

        cache = createSQLiteCache(root.path, 2u, false);
        bin = cache->addBin("ordered");
        REQUIRE(bin->readString("same-key", nullptr).failed());
        REQUIRE(bin->readString("after-remove", nullptr).failed());
        bin = nullptr;
        cache = nullptr;
        requireCachePathCanBeRemoved(root.path);
    }

    SECTION("many threads share one database without losing writes")
    {
        TemporaryCachePath root;
        osg::ref_ptr<Cache> cache = createSQLiteCache(root.path, 4u, false);
        REQUIRE(cache.valid());
        osg::ref_ptr<CacheBin> bins[] = {
            cache->addBin("concurrent-a"),
            cache->addBin("concurrent-b")
        };
        REQUIRE(bins[0].valid());
        REQUIRE(bins[1].valid());

        std::atomic_bool succeeded{ true };
        std::vector<std::thread> workers;
        for (unsigned thread = 0u; thread < 8u; ++thread)
        {
            workers.emplace_back([&, thread]()
            {
                CacheBin* bin = bins[thread % 2u].get();
                for (unsigned item = 0u; item < 50u; ++item)
                {
                    const std::string key =
                        "thread-" + std::to_string(thread) + "-" + std::to_string(item);
                    if (!writeString(bin, key, key) || !readStringEquals(bin, key, key))
                        succeeded = false;
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        REQUIRE(succeeded.load());

        bins[0] = nullptr;
        bins[1] = nullptr;
        cache = nullptr;

        cache = createSQLiteCache(root.path, 4u, false);
        bins[0] = cache->addBin("concurrent-a");
        bins[1] = cache->addBin("concurrent-b");
        for (unsigned thread = 0u; thread < 8u; ++thread)
        {
            CacheBin* bin = bins[thread % 2u].get();
            for (unsigned item = 0u; item < 50u; ++item)
            {
                const std::string key =
                    "thread-" + std::to_string(thread) + "-" + std::to_string(item);
                REQUIRE(readStringEquals(bin, key, key));
            }
        }

        bins[0] = nullptr;
        bins[1] = nullptr;
        cache = nullptr;
        requireCachePathCanBeRemoved(root.path);
    }

    SECTION("independent cache instances coordinate through WAL locking")
    {
        TemporaryCachePath root;
        osg::ref_ptr<Cache> caches[] = {
            createSQLiteCache(root.path, 2u, false),
            createSQLiteCache(root.path, 2u, false)
        };
        REQUIRE(caches[0].valid());
        REQUIRE(caches[1].valid());
        osg::ref_ptr<CacheBin> bins[] = {
            caches[0]->addBin("multiprocess"),
            caches[1]->addBin("multiprocess")
        };

        std::atomic_bool succeeded{ true };
        std::thread first([&]()
        {
            for (unsigned i = 0u; i < 100u; ++i)
            {
                const std::string key = "first-" + std::to_string(i);
                if (!writeString(bins[0].get(), key, key))
                    succeeded = false;
            }
        });
        std::thread second([&]()
        {
            for (unsigned i = 0u; i < 100u; ++i)
            {
                const std::string key = "second-" + std::to_string(i);
                if (!writeString(bins[1].get(), key, key))
                    succeeded = false;
            }
        });
        first.join();
        second.join();
        REQUIRE(succeeded.load());

        bins[0] = nullptr;
        bins[1] = nullptr;
        caches[0] = nullptr;
        caches[1] = nullptr;

        osg::ref_ptr<Cache> verifier = createSQLiteCache(root.path, 0u, false);
        osg::ref_ptr<CacheBin> bin = verifier->addBin("multiprocess");
        for (unsigned i = 0u; i < 100u; ++i)
        {
            REQUIRE(readStringEquals(bin.get(), "first-" + std::to_string(i),
                "first-" + std::to_string(i)));
            REQUIRE(readStringEquals(bin.get(), "second-" + std::to_string(i),
                "second-" + std::to_string(i)));
        }

        bin = nullptr;
        verifier = nullptr;
        requireCachePathCanBeRemoved(root.path);
    }

    SECTION("separate operating-system processes write one WAL database")
    {
        TemporaryCachePath root;
        REQUIRE(Util::makeDirectory(root.path));

        ScopedProcessTestEnvironment environment(root.path);
        REQUIRE(environment.valid());
        REQUIRE_FALSE(osgEarth::Tests::executablePath.empty());

        TestProcess first;
        TestProcess second;
        const bool firstStarted = first.start(
            osgEarth::Tests::executablePath, "[.sqlite3-process-writer-a]");
        const bool secondStarted = second.start(
            osgEarth::Tests::executablePath, "[.sqlite3-process-writer-b]");

        bool writersReady = false;
        if (firstStarted && secondStarted)
            writersReady = waitForProcessWriters(root.path);

        const bool startCreated = createMarker(childPath(root.path, "start"));
        const int firstExitCode = firstStarted ? first.wait() : 1;
        const int secondExitCode = secondStarted ? second.wait() : 1;

        INFO("first child exit code: " << firstExitCode);
        INFO("second child exit code: " << secondExitCode);
        REQUIRE(firstStarted);
        REQUIRE(secondStarted);
        REQUIRE(writersReady);
        REQUIRE(startCreated);
        REQUIRE(firstExitCode == 0);
        REQUIRE(secondExitCode == 0);

        osg::ref_ptr<Cache> verifier = createSQLiteCache(
            childPath(root.path, "cache"), 0u, false);
        REQUIRE(verifier.valid());
        osg::ref_ptr<CacheBin> bin = verifier->addBin("process-shared");
        REQUIRE(bin.valid());
        for (const std::string worker : { "writer-a", "writer-b" })
        {
            for (unsigned producer = 0u; producer < PROCESS_PRODUCERS; ++producer)
            {
                for (unsigned record = 0u; record < PROCESS_RECORDS_PER_PRODUCER; ++record)
                {
                    const std::string key = worker + "-" + std::to_string(producer) +
                        "-" + std::to_string(record);
                    REQUIRE(readStringEquals(bin.get(), key, key));
                }
            }
        }

        bin = nullptr;
        verifier = nullptr;
        requireCachePathCanBeRemoved(root.path);
    }

    SECTION("a bin safely outlives its cache")
    {
        TemporaryCachePath root;
        osg::ref_ptr<Cache> cache = createSQLiteCache(root.path, 2u, false);
        osg::ref_ptr<CacheBin> bin = cache->addBin("survivor");
        REQUIRE(bin.valid());

        cache = nullptr;
        REQUIRE(writeString(bin.get(), "still-alive", "yes"));
        REQUIRE(readStringEquals(bin.get(), "still-alive", "yes"));
        REQUIRE(bin->remove("still-alive"));

        bin = nullptr;
        requireCachePathCanBeRemoved(root.path);
    }

    SECTION("shared and separate database layouts isolate and persist multiple bins")
    {
        for (const bool separateBins : { false, true })
        {
            TemporaryCachePath root;
            osg::ref_ptr<Cache> cache = createSQLiteCache(root.path, 2u, separateBins);
            REQUIRE(cache.valid());
            REQUIRE_FALSE(cache->getStatus().isError());

            osg::ref_ptr<CacheBin> first = cache->addBin("layer-a");
            osg::ref_ptr<CacheBin> second = cache->addBin("layer-b");
            REQUIRE(first.valid());
            REQUIRE(second.valid());

            REQUIRE(writeString(first.get(), "same-key", "from-a"));
            REQUIRE(writeString(second.get(), "same-key", "from-b"));
            for (unsigned i = 0u; i < 50u; ++i)
            {
                REQUIRE(writeString(first.get(), "a-" + std::to_string(i), "a"));
                REQUIRE(writeString(second.get(), "b-" + std::to_string(i), "b"));
            }

            first = nullptr;
            second = nullptr;
            cache = nullptr;

            cache = createSQLiteCache(root.path, 0u, separateBins);
            REQUIRE(cache.valid());
            first = cache->addBin("layer-a");
            second = cache->addBin("layer-b");
            REQUIRE(readStringEquals(first.get(), "same-key", "from-a"));
            REQUIRE(readStringEquals(second.get(), "same-key", "from-b"));
            for (unsigned i = 0u; i < 50u; ++i)
            {
                REQUIRE(readStringEquals(first.get(), "a-" + std::to_string(i), "a"));
                REQUIRE(readStringEquals(second.get(), "b-" + std::to_string(i), "b"));
            }

            first = nullptr;
            second = nullptr;
            cache = nullptr;

            const unsigned databaseCount = countDatabaseFiles(root.path);
            INFO("separate_bins=" << separateBins);
            REQUIRE(databaseCount == (separateBins ? 2u : 1u));
            requireCachePathCanBeRemoved(root.path);
        }
    }

    SECTION("unsafe bin identifiers cannot escape the cache directory")
    {
        TemporaryCachePath parent;
        const std::string root = childPath(parent.path, "cache");
        const std::string escaped = childPath(parent.path, "escaped.db");
        osg::ref_ptr<Cache> cache = createSQLiteCache(root, 0u, true);
        REQUIRE(cache.valid());
        osg::ref_ptr<CacheBin> bin = cache->addBin("../escaped");
        REQUIRE(bin.valid());
        REQUIRE(writeString(bin.get(), "key", "value"));
        bin = nullptr;

        bin = cache->addBin("CON");
        REQUIRE(bin.valid());
        REQUIRE(writeString(bin.get(), "key", "value"));
        bin = nullptr;
        cache = nullptr;

        REQUIRE_FALSE(osgDB::fileExists(escaped));
        const unsigned databaseCount = countDatabaseFiles(root);
        REQUIRE(databaseCount == 2u);
        requireCachePathCanBeRemoved(parent.path);
    }
}

TEST_CASE("Filesystem cache concurrency and lifetime", "[cache][filesystem]")
{
    SECTION("zero writer threads is synchronous and destruction is safe")
    {
        TemporaryCachePath root;
        osg::ref_ptr<Cache> cache = createFileSystemCache(root.path, 0u);
        REQUIRE(cache.valid());
        REQUIRE_FALSE(cache->getStatus().isError());
        osg::ref_ptr<CacheBin> bin = cache->getOrCreateDefaultBin();
        REQUIRE(bin.valid());
        REQUIRE(writeString(bin.get(), "sync", "value"));
        REQUIRE(readStringEquals(bin.get(), "sync", "value"));
        bin = nullptr;
        cache = nullptr;

        cache = createFileSystemCache(root.path, 0u);
        REQUIRE(cache.valid());
        bin = cache->getOrCreateDefaultBin();
        REQUIRE(readStringEquals(bin.get(), "sync", "value"));
        bin = nullptr;
        cache = nullptr;
        requireCachePathCanBeRemoved(root.path);
    }

    SECTION("latest queued overwrite wins and is durable")
    {
        TemporaryCachePath root;
        osg::ref_ptr<Cache> cache = createFileSystemCache(root.path, 4u);
        REQUIRE(cache.valid());
        osg::ref_ptr<CacheBin> bin = cache->getOrCreateDefaultBin();
        REQUIRE(bin.valid());

        for (unsigned i = 0u; i < 200u; ++i)
            REQUIRE(writeString(bin.get(), "hot-key", std::to_string(i)));
        REQUIRE(readStringEquals(bin.get(), "hot-key", "199"));

        bin = nullptr;
        cache = nullptr;
        cache = createFileSystemCache(root.path, 0u);
        bin = cache->getOrCreateDefaultBin();
        REQUIRE(readStringEquals(bin.get(), "hot-key", "199"));
        bin = nullptr;
        cache = nullptr;
        requireCachePathCanBeRemoved(root.path);
    }

    SECTION("a bin safely outlives its cache")
    {
        TemporaryCachePath root;
        osg::ref_ptr<Cache> cache = createFileSystemCache(root.path, 2u);
        osg::ref_ptr<CacheBin> bin = cache->getOrCreateDefaultBin();
        REQUIRE(bin.valid());

        cache = nullptr;
        REQUIRE(writeString(bin.get(), "survivor", "yes"));
        REQUIRE(readStringEquals(bin.get(), "survivor", "yes"));
        bin = nullptr;

        cache = createFileSystemCache(root.path, 0u);
        bin = cache->getOrCreateDefaultBin();
        REQUIRE(readStringEquals(bin.get(), "survivor", "yes"));
        bin = nullptr;
        cache = nullptr;
        requireCachePathCanBeRemoved(root.path);
    }
}

TEST_CASE("Cache runtime statistics emit comparable JSON", "[cache][stats]")
{
    auto verify = [](const std::string& driver)
    {
        TemporaryCachePath root;
        const std::string statsPath = childPath(
            childPath(root.path, "reports"), driver + ".jsonl");
        osg::ref_ptr<Cache> cache = createInstrumentedCache(
            driver, childPath(root.path, "cache"), statsPath);
        REQUIRE(cache.valid());
        REQUIRE_FALSE(cache->getStatus().isError());
        osg::ref_ptr<CacheBin> bin = cache->getOrCreateDefaultBin();
        REQUIRE(bin.valid());
        REQUIRE(writeString(bin.get(), "hit", "value"));
        REQUIRE(readStringEquals(bin.get(), "hit", "value"));
        REQUIRE(bin->readString("miss", nullptr).failed());
        bin = nullptr;
        cache = nullptr;

        REQUIRE(osgDB::fileExists(statsPath));
        std::ifstream input(statsPath);
        std::string json((std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        INFO(json);
        REQUIRE(json.find("\"schema\":1") != std::string::npos);
        REQUIRE(json.find("\"final\":true") != std::string::npos);
        REQUIRE(json.find("\"driver\":\"" + driver + "\"") != std::string::npos);
        REQUIRE(json.find("\"read\":{\"count\":2") != std::string::npos);
        REQUIRE(json.find("\"write\":{\"count\":1") != std::string::npos);
        REQUIRE(json.find("\"success\":1,\"miss\":1,\"error\":0") != std::string::npos);
        input.close();
        requireCachePathCanBeRemoved(root.path);
    };

    SECTION("filesystem") { verify("filesystem"); }
    SECTION("sqlite3") { verify("sqlite3"); }
}

TEST_CASE("LRUCache")
{
    SECTION("LRUCache_BasicEviction")
    {
        // LRUCache with capacity 3
        osgEarth::LRUCache<int, std::string> cache(3u);

        cache.insert(1, "one");
        cache.insert(2, "two");
        cache.insert(3, "three");

        // Verify retrieval
        REQUIRE(cache.get(1) == "one");
        REQUIRE(cache.get(2) == "two");
        REQUIRE(cache.get(3) == "three");

        // Insert a fourth, expecting key 1 to be evicted
        cache.insert(4, "four");

        REQUIRE_FALSE(cache.touch(1));
        REQUIRE(cache.touch(2));
        REQUIRE(cache.touch(3));
        REQUIRE(cache.touch(4));
        REQUIRE(cache.get(2) == "two");
        REQUIRE(cache.get(3) == "three");
        REQUIRE(cache.get(4) == "four");
    }

    SECTION("LRUCache_UsageRefresh")
    {
        osgEarth::LRUCache<int, std::string> cache(3u);

        cache.insert(1, "one");
        cache.insert(2, "two");
        cache.insert(3, "three");

        // Access keys 1 and 2, marking them as recently used
        REQUIRE(cache.get(1) == "one");
        REQUIRE(cache.get(2) == "two");

        // Insert new item expecting the least recently used (key 3) to be evicted
        cache.insert(4, "four");

        REQUIRE_FALSE(cache.touch(3));
        REQUIRE(cache.touch(1));
        REQUIRE(cache.touch(2));
        REQUIRE(cache.touch(4));
        REQUIRE(cache.get(1) == "one");
        REQUIRE(cache.get(2) == "two");
        REQUIRE(cache.get(4) == "four");
    }

    SECTION("LRUCache_get_or_insert")
    {
        osgEarth::LRUCache<int, std::string> cache(2u);

        // Insert a value using get_or_insert for a missing key
        auto v1 = cache.get_or_insert(1, [](osgEarth_std::optional<std::string>& out) { out = std::string("one"); });
        REQUIRE(v1.has_value());
        REQUIRE(v1.value() == "one");
        REQUIRE(cache.get(1).has_value());
        REQUIRE(cache.get(1).value() == "one");

        // get_or_insert for an existing key should not call the functor, should return the cached value
        auto v2 = cache.get_or_insert(1, [](osgEarth_std::optional<std::string>& out) { out = std::string("should_not_be_used"); });
        REQUIRE(v2.has_value());
        REQUIRE(v2.value() == "one");

        // Insert another value
        auto v3 = cache.get_or_insert(2, [](osgEarth_std::optional<std::string>& out) { out = std::string("two"); });
        REQUIRE(v3.has_value());
        REQUIRE(v3.value() == "two");
        REQUIRE(cache.get(2).has_value());
        REQUIRE(cache.get(2).value() == "two");

        // Insert a third value, which should evict the least recently used (key 1)
        auto v4 = cache.get_or_insert(3, [](osgEarth_std::optional<std::string>& out) { out = std::string("three"); });
        REQUIRE(v4.has_value());
        REQUIRE(v4.value() == "three");
        REQUIRE(cache.get(3).has_value());
        REQUIRE(cache.get(3).value() == "three");
        REQUIRE_FALSE(cache.touch(1)); // key 1 should be evicted

        // get_or_insert for an evicted key should call the functor again
        auto v5 = cache.get_or_insert(1, [](osgEarth_std::optional<std::string>& out) { out = std::string("one-again"); });
        REQUIRE(v5.has_value());
        REQUIRE(v5.value() == "one-again");
        REQUIRE(cache.get(1).has_value());
        REQUIRE(cache.get(1).value() == "one-again");

        // Test that if the functor does not set the value, nothing is inserted
        auto v6 = cache.get_or_insert(4, [](osgEarth_std::optional<std::string>&) { /* do not set */ });
        REQUIRE_FALSE(v6.has_value());
        REQUIRE_FALSE(cache.touch(4));
    }

}
