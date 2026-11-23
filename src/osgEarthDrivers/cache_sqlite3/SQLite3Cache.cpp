/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "SQLite3Cache"
#include <osgEarth/Cache>
#include <osgEarth/CacheStats>
#include <osgEarth/FileUtils>
#include <osgEarth/StringUtils>
#include <osgEarth/Threading>
#include <osgEarth/URI>
#include <osgEarth/Registry>
#include <osgEarth/NetworkMonitor>
#include <osgEarth/Metrics>
#include <osgDB/FileUtils>
#include <osgDB/FileNameUtils>
#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <filesystem>
#include <list>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//#define USE_NETWORK_MONITOR

using namespace osgEarth;
using namespace osgEarth::Drivers;

#undef  LC
#define LC "[SQLite3Cache] "

#define OSG_FORMAT "osgb"

namespace
{

    constexpr unsigned MAX_TRANSACTION_RETRIES = 3u;

    bool isRetryable(int rc)
    {
        const int primary = rc & 0xff;
        return primary == SQLITE_BUSY || primary == SQLITE_LOCKED;
    }

    struct StatementSet
    {
        sqlite3_stmt* selectStmt = nullptr;
        sqlite3_stmt* insertStmt = nullptr;
        sqlite3_stmt* deleteStmt = nullptr;
        sqlite3_stmt* touchStmt = nullptr;
        sqlite3_stmt* existsStmt = nullptr;
        sqlite3_stmt* clearStmt = nullptr;
        sqlite3_stmt* clearAllStmt = nullptr;
        sqlite3_stmt* sizeStmt = nullptr;

        ~StatementSet() { finalize(); }

        void finalize()
        {
            if (selectStmt)   { sqlite3_finalize(selectStmt);   selectStmt = nullptr; }
            if (insertStmt)   { sqlite3_finalize(insertStmt);   insertStmt = nullptr; }
            if (deleteStmt)   { sqlite3_finalize(deleteStmt);   deleteStmt = nullptr; }
            if (touchStmt)    { sqlite3_finalize(touchStmt);    touchStmt = nullptr; }
            if (existsStmt)   { sqlite3_finalize(existsStmt);   existsStmt = nullptr; }
            if (clearStmt)    { sqlite3_finalize(clearStmt);    clearStmt = nullptr; }
            if (clearAllStmt) { sqlite3_finalize(clearAllStmt); clearAllStmt = nullptr; }
            if (sizeStmt)     { sqlite3_finalize(sizeStmt);     sizeStmt = nullptr; }
        }
    };

    class SQLiteConnection
    {
    public:
        SQLiteConnection(sqlite3* db, bool separate) : _db(db), _separate(separate) { }

        ~SQLiteConnection()
        {
            _statements.finalize();
            if (_db)
            {
                const int rc = sqlite3_close(_db);
                if (rc != SQLITE_OK)
                {
                    OE_WARN << LC << "Error closing database connection: "
                        << sqlite3_errstr(rc) << std::endl;
                }
                _db = nullptr;
            }
        }

        SQLiteConnection(const SQLiteConnection&) = delete;
        SQLiteConnection& operator=(const SQLiteConnection&) = delete;

        sqlite3* db() const { return _db; }
        StatementSet& statements() { return _statements; }

        bool prepare(bool writer, std::string& error)
        {
            auto prepareOne = [&](const char* sql, sqlite3_stmt** output) -> bool
            {
                const int rc = sqlite3_prepare_v2(_db, sql, -1, output, nullptr);
                if (rc != SQLITE_OK)
                {
                    error = Stringify() << "Failed to prepare SQLite statement: "
                        << sqlite3_errmsg(_db) << " [" << sql << "]";
                    return false;
                }
                return true;
            };

            if (_separate)
            {
                if (!prepareOne(
                    "SELECT data, metadata, timestamp FROM cache WHERE key=?",
                    &_statements.selectStmt)) return false;

                if (!prepareOne(
                    "SELECT 1 FROM cache WHERE key=?",
                    &_statements.existsStmt)) return false;

                if (!prepareOne(
                    "SELECT SUM(LENGTH(data)) FROM cache",
                    &_statements.sizeStmt)) return false;

                if (writer)
                {
                    if (!prepareOne(
                        "INSERT INTO cache (key, data, metadata, timestamp) "
                        "VALUES (?, ?, ?, CAST(strftime('%s','now') AS INTEGER)) "
                        "ON CONFLICT(key) DO UPDATE SET "
                        "data=excluded.data, metadata=excluded.metadata, timestamp=excluded.timestamp",
                        &_statements.insertStmt)) return false;

                    if (!prepareOne(
                        "DELETE FROM cache WHERE key=?",
                        &_statements.deleteStmt)) return false;

                    if (!prepareOne(
                        "UPDATE cache SET timestamp=CAST(strftime('%s','now') AS INTEGER) WHERE key=?",
                        &_statements.touchStmt)) return false;

                    if (!prepareOne("DELETE FROM cache", &_statements.clearStmt)) return false;
                    if (!prepareOne("DELETE FROM cache", &_statements.clearAllStmt)) return false;
                }
            }
            else
            {
                if (!prepareOne(
                    "SELECT data, metadata, timestamp FROM cache WHERE bin_id=? AND key=?",
                    &_statements.selectStmt)) return false;

                if (!prepareOne(
                    "SELECT 1 FROM cache WHERE bin_id=? AND key=?",
                    &_statements.existsStmt)) return false;

                if (!prepareOne(
                    "SELECT SUM(LENGTH(data)) FROM cache WHERE bin_id=?",
                    &_statements.sizeStmt)) return false;

                if (writer)
                {
                    if (!prepareOne(
                        "INSERT INTO cache (bin_id, key, data, metadata, timestamp) "
                        "VALUES (?, ?, ?, ?, CAST(strftime('%s','now') AS INTEGER)) "
                        "ON CONFLICT(bin_id, key) DO UPDATE SET "
                        "data=excluded.data, metadata=excluded.metadata, timestamp=excluded.timestamp",
                        &_statements.insertStmt)) return false;

                    if (!prepareOne(
                        "DELETE FROM cache WHERE bin_id=? AND key=?",
                        &_statements.deleteStmt)) return false;

                    if (!prepareOne(
                        "UPDATE cache SET timestamp=CAST(strftime('%s','now') AS INTEGER) "
                        "WHERE bin_id=? AND key=?",
                        &_statements.touchStmt)) return false;

                    if (!prepareOne(
                        "DELETE FROM cache WHERE bin_id=?",
                        &_statements.clearStmt)) return false;

                    if (!prepareOne("DELETE FROM cache", &_statements.clearAllStmt)) return false;
                }
            }

            return true;
        }

    private:
        sqlite3* _db = nullptr;
        bool _separate = true;
        StatementSet _statements;
    };

    enum class MutationType
    {
        Put,
        Remove,
        Touch,
        ClearBin,
        ClearAll
    };

    struct Completion
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        bool success = false;
    };

    struct Mutation
    {
        MutationType type = MutationType::Put;
        std::uint64_t generation = 0u;
        unsigned retries = 0u;
        std::string bin;
        std::string key;
        std::string data;
        std::string metaJSON;
        osg::ref_ptr<const osg::Object> object;
        Config meta;
        TimeStamp timestamp = 0;
        std::shared_ptr<Completion> completion;

        std::size_t storageSize() const
        {
            return bin.size() + key.size() + data.size() + metaJSON.size() + 64u;
        }
    };

    struct RecordKey
    {
        std::string bin;
        std::string key;

        bool operator==(const RecordKey& rhs) const
        {
            return bin == rhs.bin && key == rhs.key;
        }
    };

    struct RecordKeyHash
    {
        std::size_t operator()(const RecordKey& value) const
        {
            const std::size_t h1 = std::hash<std::string>()(value.bin);
            const std::size_t h2 = std::hash<std::string>()(value.key);
            return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6u) + (h1 >> 2u));
        }
    };

    struct PendingRecord
    {
        MutationType type = MutationType::Put;
        std::uint64_t generation = 0u;
        osg::ref_ptr<const osg::Object> object;
        Config meta;
        TimeStamp timestamp = 0;
    };

    enum class PendingStatus
    {
        None,
        Present,
        Absent
    };

    enum class OpenResult { Ready, Missing, Error };

    class DatabaseState : public std::enable_shared_from_this<DatabaseState>
    {
    public:
        class ReaderLease
        {
        public:
            ReaderLease() = default;
            ReaderLease(DatabaseState* owner, std::unique_ptr<SQLiteConnection> connection) :
                _owner(owner),
                _connection(std::move(connection)) { }

            ReaderLease(ReaderLease&& rhs) noexcept :
                _owner(rhs._owner),
                _connection(std::move(rhs._connection))
            {
                rhs._owner = nullptr;
            }

            ReaderLease& operator=(ReaderLease&& rhs) noexcept
            {
                if (this != &rhs)
                {
                    release();
                    _owner = rhs._owner;
                    _connection = std::move(rhs._connection);
                    rhs._owner = nullptr;
                }
                return *this;
            }

            ~ReaderLease() { release(); }

            SQLiteConnection* operator->() const { return _connection.get(); }
            explicit operator bool() const { return _connection != nullptr; }

        private:
            ReaderLease(const ReaderLease&) = delete;
            ReaderLease& operator=(const ReaderLease&) = delete;

            void release()
            {
                if (_owner && _connection)
                    _owner->releaseReader(std::move(_connection));
                _owner = nullptr;
            }

            DatabaseState* _owner = nullptr;
            std::unique_ptr<SQLiteConnection> _connection;
        };

        static std::shared_ptr<DatabaseState> create(
            const std::string& path,
            bool separate,
            const SQLite3CacheOptions& options,
            jobs::jobpool* pool,
            const std::shared_ptr<CacheStatistics>& stats,
            std::string& error)
        {
            auto result = std::shared_ptr<DatabaseState>(
                new DatabaseState(path, separate, options, pool, stats));
            if (result->ensureOpen(!separate, &error) == OpenResult::Error)
                return nullptr;
            return result;
        }

        ~DatabaseState()
        {
            drain();
            {
                std::lock_guard<std::mutex> lock(_readerMutex);
                _idleReaders.clear();
                _readerCount = 0u;
            }
            _writer.reset();
        }

        const std::string& path() const { return _path; }

        void setPool(jobs::jobpool* pool)
        {
            drain();
            std::lock_guard<std::mutex> lock(_queueMutex);
            _pool = pool;
            _flushScheduled = false;
        }

        bool enqueuePut(
            const std::string& bin,
            const std::string& key,
            std::string&& data,
            std::string&& metaJSON,
            const Config& meta,
            const osg::Object* object)
        {
            Mutation mutation;
            mutation.type = MutationType::Put;
            mutation.bin = bin;
            mutation.key = key;
            mutation.data = std::move(data);
            mutation.metaJSON = std::move(metaJSON);
            mutation.meta = meta;
            mutation.object = object;
            mutation.timestamp = DateTime().asTimeStamp();

            return submit(std::move(mutation));
        }

        bool remove(const std::string& bin, const std::string& key)
        {
            Mutation mutation;
            mutation.type = MutationType::Remove;
            mutation.bin = bin;
            mutation.key = key;
            return submit(std::move(mutation));
        }

        bool touch(const std::string& bin, const std::string& key)
        {
            Mutation mutation;
            mutation.type = MutationType::Touch;
            mutation.bin = bin;
            mutation.key = key;
            return submit(std::move(mutation));
        }

        bool clearBin(const std::string& bin)
        {
            Mutation mutation;
            mutation.type = MutationType::ClearBin;
            mutation.bin = bin;
            return submit(std::move(mutation));
        }

        bool clearAll()
        {
            Mutation mutation;
            mutation.type = MutationType::ClearAll;
            return submit(std::move(mutation));
        }

        PendingStatus getPending(
            const std::string& bin,
            const std::string& key,
            PendingRecord& output) const
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            auto i = _pendingRecords.find(RecordKey{ bin, key });
            if (i != _pendingRecords.end())
            {
                output = i->second;
                return i->second.type == MutationType::Put ?
                    PendingStatus::Present : PendingStatus::Absent;
            }

            if (_pendingClearAll != 0u || _pendingBinClears.find(bin) != _pendingBinClears.end())
                return PendingStatus::Absent;

            return PendingStatus::None;
        }

        // Only an absent database is a cache miss. Connection failures remain backend errors.
        ReaderLease acquireReader(bool* missing = nullptr)
        {
            const auto result = ensureOpen(false);
            if (missing)
                *missing = result == OpenResult::Missing;
            if (result != OpenResult::Ready)
                return ReaderLease();

            CacheStatsScope waitScope(_stats, CacheStatistics::Metric::ReaderWait);
            std::unique_lock<std::mutex> lock(_readerMutex);
            while (_idleReaders.empty() && _readerCount >= _maxReaders)
                _readerCV.wait(lock);

            if (!_idleReaders.empty())
            {
                auto connection = std::move(_idleReaders.back());
                _idleReaders.pop_back();
                waitScope.success();
                return ReaderLease(this, std::move(connection));
            }

            ++_readerCount;
            lock.unlock();

            std::string error;
            auto connection = openConnection(false, error);
            if (!connection)
            {
                OE_WARN << LC << error << std::endl;
                lock.lock();
                --_readerCount;
                lock.unlock();
                _readerCV.notify_one();
                waitScope.error();
                return ReaderLease();
            }

            waitScope.success();
            return ReaderLease(this, std::move(connection));
        }

        void drain()
        {
            for (;;)
            {
                flushOne();

                {
                    std::unique_lock<std::mutex> lock(_queueMutex);
                    if (!_queue.empty())
                    {
                        _drainCV.wait_for(lock, std::chrono::milliseconds(10));
                        continue;
                    }
                }

                // Queue emptiness does not mean that the last dispatched callback
                // has released this state and its SQLite connection yet.
                _flushGroup->join();

                {
                    std::lock_guard<std::mutex> lock(_queueMutex);
                    if (_queue.empty())
                    {
                        _flushScheduled = false;
                        _drainCV.notify_all();
                        return;
                    }
                }
            }
        }

        bool compact()
        {
            const auto result = ensureOpen(false);
            if (result != OpenResult::Ready)
                return result == OpenResult::Missing;

            drain();
            std::lock_guard<std::mutex> flushLock(_flushMutex);
            char* errMsg = nullptr;
            int rc = sqlite3_exec(_writer->db(), "PRAGMA optimize", nullptr, nullptr, &errMsg);
            if (rc == SQLITE_OK)
                rc = sqlite3_exec(_writer->db(), "VACUUM", nullptr, nullptr, &errMsg);

            if (rc != SQLITE_OK)
            {
                OE_WARN << LC << "Compact failed for \"" << _path << "\": "
                    << (errMsg ? errMsg : sqlite3_errmsg(_writer->db())) << std::endl;
                sqlite3_free(errMsg);
                return false;
            }
            return true;
        }

        unsigned storageSize(const std::string& bin)
        {
            auto reader = acquireReader();
            if (!reader)
                return 0u;

            sqlite3_stmt* stmt = reader->statements().sizeStmt;
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            if (!_separate && sqlite3_bind_text(stmt, 1, bin.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                sqlite3_reset(stmt);
                return 0u;
            }

            const int rc = sqlite3_step(stmt);
            sqlite3_int64 value = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
            sqlite3_reset(stmt);
            if (value <= 0)
                return 0u;
            return value > static_cast<sqlite3_int64>(std::numeric_limits<unsigned>::max()) ?
                std::numeric_limits<unsigned>::max() : static_cast<unsigned>(value);
        }

        std::uintmax_t diskSize() const
        {
            std::uintmax_t result = 0u;
            for (const auto& filename : { _path, _path + "-wal", _path + "-shm" })
                result += Util::getFileSize(filename);
            return result;
        }

    private:
        DatabaseState(
            const std::string& path,
            bool separate,
            const SQLite3CacheOptions& options,
            jobs::jobpool* pool,
            const std::shared_ptr<CacheStatistics>& stats) :
            _path(path),
            _separate(separate),
            _options(options),
            _pool(pool),
            _stats(stats),
            _flushGroup(jobs::jobgroup::create()),
            _maxReaders(std::max(1u, options.readerConnections().get())),
            _maxQueueBytes(static_cast<std::size_t>(options.maxQueueMB().get()) * 1024u * 1024u),
            _maxBatchEntries(std::max(1u, options.batchSize().get())),
            _maxBatchBytes(std::max<std::size_t>(
                1u, static_cast<std::size_t>(options.batchSizeMB().get()) * 1024u * 1024u))
        {
        }

        // Publish a prepared writer once per state. An absent file is rechecked on later calls
        // since another cache instance can create it. Failed opens are retryable.
        OpenResult ensureOpen(bool allowCreate, std::string* errorOut = nullptr)
        {
            if (_open.load(std::memory_order_acquire))
                return OpenResult::Ready;
            std::lock_guard<std::mutex> lock(_openMutex);
            if (_open.load(std::memory_order_relaxed))
                return OpenResult::Ready;

            std::error_code ec;
            const bool exists = std::filesystem::exists(std::filesystem::u8path(_path), ec);
            if (!allowCreate && !exists && !ec)
                return OpenResult::Missing;

            std::string error;
            if (!initialize(allowCreate, !exists && !ec, error))
            {
                if (errorOut)
                    *errorOut = error;
                else
                    OE_WARN << LC << error << std::endl;
                return OpenResult::Error;
            }

            _open.store(true, std::memory_order_release);
            if (_separate)
                OE_INFO << LC << "Opened SQLite cache bin at \"" << _path << "\"" << std::endl;
            return OpenResult::Ready;
        }

        // Called under the open mutex. Only a shared cache or a record write may create the file.
        bool initialize(bool allowCreate, bool newDatabase, std::string& error)
        {
            if (sqlite3_threadsafe() == 0)
            {
                error = "SQLite was built without thread-safety support";
                return false;
            }

            sqlite3* rawDb = nullptr;
            // Each connection is exclusively owned by the serialized writer or by
            // one reader lease, so SQLite's per-connection mutex is redundant.
            const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | (allowCreate ? SQLITE_OPEN_CREATE : 0);
            int rc = sqlite3_open_v2(_path.c_str(), &rawDb, flags, nullptr);
            if (rc != SQLITE_OK)
            {
                error = Stringify() << "Failed to open SQLite database \"" << _path << "\": "
                    << (rawDb ? sqlite3_errmsg(rawDb) : sqlite3_errstr(rc));
                sqlite3_close(rawDb);
                return false;
            }

            sqlite3_extended_result_codes(rawDb, 1);
            sqlite3_busy_timeout(rawDb, static_cast<int>(_options.busyTimeout().get()));

            auto retryPause = [](const std::chrono::steady_clock::time_point& deadline)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return true;
            };

            auto execWithRetry = [&](const char* sql, char** errMsg) -> int
            {
                const auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(_options.busyTimeout().get());
                int result = SQLITE_OK;
                do
                {
                    if (errMsg && *errMsg)
                    {
                        sqlite3_free(*errMsg);
                        *errMsg = nullptr;
                    }
                    result = sqlite3_exec(rawDb, sql, nullptr, nullptr, errMsg);
                }
                while (isRetryable(result) && retryPause(deadline));
                return result;
            };

            if (newDatabase)
            {
                rc = execWithRetry("PRAGMA page_size=8192", nullptr);
                if (rc != SQLITE_OK)
                {
                    error = Stringify() << "Failed to set SQLite page size: " << sqlite3_errmsg(rawDb);
                    sqlite3_close(rawDb);
                    return false;
                }
            }

            bool walEnabled = false;
            const auto journalDeadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(_options.busyTimeout().get());
            do
            {
                sqlite3_stmt* journalStmt = nullptr;
                rc = sqlite3_prepare_v2(
                    rawDb, "PRAGMA journal_mode=WAL", -1, &journalStmt, nullptr);
                if (rc == SQLITE_OK)
                    rc = sqlite3_step(journalStmt);

                const char* journalMode = rc == SQLITE_ROW ?
                    reinterpret_cast<const char*>(sqlite3_column_text(journalStmt, 0)) : nullptr;
                walEnabled = journalMode && osgEarth::ciEquals(journalMode, "wal");
                sqlite3_finalize(journalStmt);
            }
            while (!walEnabled && isRetryable(rc) && retryPause(journalDeadline));

            if (!walEnabled)
            {
                error = Stringify() << "Failed to enable WAL for SQLite database \"" << _path
                    << "\": " << sqlite3_errmsg(rawDb);
                sqlite3_close(rawDb);
                return false;
            }

            if (!configureConnection(rawDb, false, error))
            {
                sqlite3_close(rawDb);
                return false;
            }

            const char* separateSchema =
                "CREATE TABLE IF NOT EXISTS cache ("
                "  key       TEXT NOT NULL PRIMARY KEY,"
                "  data      BLOB NOT NULL,"
                "  metadata  TEXT,"
                "  timestamp INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER))"
                ");"
                "DROP INDEX IF EXISTS idx_cache_timestamp;";

            const char* sharedSchema =
                "CREATE TABLE IF NOT EXISTS cache ("
                "  bin_id    TEXT NOT NULL,"
                "  key       TEXT NOT NULL,"
                "  data      BLOB NOT NULL,"
                "  metadata  TEXT,"
                "  timestamp INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)),"
                "  PRIMARY KEY (bin_id, key)"
                ");"
                "DROP INDEX IF EXISTS idx_cache_timestamp;";

            char* errMsg = nullptr;
            rc = execWithRetry(_separate ? separateSchema : sharedSchema, &errMsg);
            if (rc != SQLITE_OK)
            {
                error = Stringify() << "Failed to create SQLite cache schema: "
                    << (errMsg ? errMsg : sqlite3_errmsg(rawDb));
                sqlite3_free(errMsg);
                sqlite3_close(rawDb);
                return false;
            }

            _writer = std::make_unique<SQLiteConnection>(rawDb, _separate);
            if (!_writer->prepare(true, error))
            {
                _writer.reset();
                return false;
            }

            return true;
        }

        bool configureConnection(sqlite3* db, bool readOnly, std::string& error) const
        {
            const unsigned cacheKB = std::max(1u, _options.cacheSizeMB().get()) * 1024u;
            const std::string pragmas = Stringify()
                << "PRAGMA synchronous=NORMAL;"
                << "PRAGMA cache_size=-" << cacheKB << ";"
                << (readOnly ? "PRAGMA query_only=ON;" : "");

            char* errMsg = nullptr;
            const int rc = sqlite3_exec(db, pragmas.c_str(), nullptr, nullptr, &errMsg);
            if (rc != SQLITE_OK)
            {
                error = Stringify() << "Failed to configure SQLite connection: "
                    << (errMsg ? errMsg : sqlite3_errmsg(db));
                sqlite3_free(errMsg);
                return false;
            }
            return true;
        }

        std::unique_ptr<SQLiteConnection> openConnection(bool writer, std::string& error)
        {
            sqlite3* rawDb = nullptr;
            const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
            const int rc = sqlite3_open_v2(_path.c_str(), &rawDb, flags, nullptr);
            if (rc != SQLITE_OK)
            {
                error = Stringify() << "Failed to open SQLite reader for \"" << _path << "\": "
                    << (rawDb ? sqlite3_errmsg(rawDb) : sqlite3_errstr(rc));
                sqlite3_close(rawDb);
                return nullptr;
            }

            sqlite3_extended_result_codes(rawDb, 1);
            sqlite3_busy_timeout(rawDb, static_cast<int>(_options.busyTimeout().get()));
            if (!configureConnection(rawDb, !writer, error))
            {
                sqlite3_close(rawDb);
                return nullptr;
            }

            auto result = std::make_unique<SQLiteConnection>(rawDb, _separate);
            if (!result->prepare(writer, error))
                return nullptr;
            return result;
        }

        void releaseReader(std::unique_ptr<SQLiteConnection> connection)
        {
            std::lock_guard<std::mutex> lock(_readerMutex);
            _idleReaders.push_back(std::move(connection));
            _readerCV.notify_one();
        }

        // Only a put can populate a missing bin. Maintenance on an empty bin succeeds immediately.
        bool submit(Mutation&& mutation)
        {
            const bool put = mutation.type == MutationType::Put;
            const auto result = ensureOpen(put);
            if (result != OpenResult::Ready)
                return result == OpenResult::Missing;

            std::shared_ptr<Completion> completion;
            bool dispatch = false;
            bool synchronous = false;
            {
                std::unique_lock<std::mutex> lock(_queueMutex);
                const std::size_t mutationSize = mutation.storageSize();
                while (_pool && _maxQueueBytes > 0u && !_queue.empty() &&
                    _queuedBytes + mutationSize > _maxQueueBytes)
                {
                    _spaceCV.wait(lock);
                }

                synchronous = _pool == nullptr;
                if (!put || synchronous)
                {
                    completion = std::make_shared<Completion>();
                    mutation.completion = completion;
                }

                mutation.generation = ++_nextGeneration;
                const RecordKey recordKey{ mutation.bin, mutation.key };

                switch (mutation.type)
                {
                case MutationType::Put:
                    _pendingRecords[recordKey] = PendingRecord{
                        mutation.type, mutation.generation, mutation.object,
                        mutation.meta, mutation.timestamp };
                    break;
                case MutationType::Remove:
                    _pendingRecords[recordKey] = PendingRecord{
                        mutation.type, mutation.generation, nullptr, Config(), 0 };
                    break;
                case MutationType::ClearBin:
                    for (auto i = _pendingRecords.begin(); i != _pendingRecords.end(); )
                    {
                        if (i->first.bin == mutation.bin)
                            i = _pendingRecords.erase(i);
                        else
                            ++i;
                    }
                    _pendingBinClears[mutation.bin] = mutation.generation;
                    break;
                case MutationType::ClearAll:
                    _pendingRecords.clear();
                    _pendingBinClears.clear();
                    _pendingClearAll = mutation.generation;
                    break;
                case MutationType::Touch:
                    break;
                }

                _queuedBytes += mutationSize;
                _queue.push_back(std::move(mutation));
                if (_stats)
                    _stats->updateQueueHighWater(_queuedBytes, _queue.size());
                if (!synchronous && !_flushScheduled)
                {
                    _flushScheduled = true;
                    dispatch = true;
                }
            }

            if (dispatch)
                dispatchFlush();
            else if (synchronous)
                flushOne();

            return waitFor(completion);
        }

        void dispatchFlush()
        {
            auto self = shared_from_this();
            jobs::jobpool* targetPool = nullptr;
            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                targetPool = _pool;
            }

            if (!targetPool)
            {
                flushOne();
                return;
            }

            jobs::dispatch(
                [self]() { self->runScheduledFlush(); },
                jobs::context{ "sqlite_cache_flush", targetPool, {}, _flushGroup });
        }

        void runScheduledFlush()
        {
            flushOne();

            bool again = false;
            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                if (_queue.empty())
                {
                    _flushScheduled = false;
                    _drainCV.notify_all();
                }
                else
                {
                    again = true;
                }
            }

            if (again)
                dispatchFlush();
        }

        bool waitFor(const std::shared_ptr<Completion>& completion)
        {
            if (!completion)
                return true;

            for (;;)
            {
                std::unique_lock<std::mutex> lock(completion->mutex);
                if (completion->done)
                    return completion->success;

                if (completion->cv.wait_for(lock, std::chrono::milliseconds(10),
                    [&]() { return completion->done; }))
                {
                    return completion->success;
                }
                lock.unlock();

                // This also makes synchronous mode and runtime shutdown safe.
                flushOne();
            }
        }

        bool bindMutation(sqlite3_stmt* stmt, const Mutation& mutation)
        {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);

            int rc = SQLITE_OK;
            int index = 1;
            if (!_separate && mutation.type != MutationType::ClearAll)
                rc = sqlite3_bind_text(stmt, index++, mutation.bin.c_str(), -1, SQLITE_TRANSIENT);

            if (rc == SQLITE_OK &&
                (mutation.type == MutationType::Put ||
                 mutation.type == MutationType::Remove ||
                 mutation.type == MutationType::Touch))
            {
                rc = sqlite3_bind_text(stmt, index++, mutation.key.c_str(), -1, SQLITE_TRANSIENT);
            }

            if (rc == SQLITE_OK && mutation.type == MutationType::Put)
            {
                rc = sqlite3_bind_blob64(stmt, index++, mutation.data.data(),
                    static_cast<sqlite3_uint64>(mutation.data.size()), SQLITE_STATIC);
                if (rc == SQLITE_OK)
                    rc = sqlite3_bind_text(stmt, index++, mutation.metaJSON.c_str(), -1, SQLITE_STATIC);
            }

            return rc == SQLITE_OK;
        }

        int executeMutation(const Mutation& mutation)
        {
            StatementSet& stmts = _writer->statements();
            sqlite3_stmt* stmt = nullptr;
            switch (mutation.type)
            {
            case MutationType::Put:      stmt = stmts.insertStmt; break;
            case MutationType::Remove:   stmt = stmts.deleteStmt; break;
            case MutationType::Touch:    stmt = stmts.touchStmt; break;
            case MutationType::ClearBin: stmt = stmts.clearStmt; break;
            case MutationType::ClearAll: stmt = stmts.clearAllStmt; break;
            }

            if (!stmt || !bindMutation(stmt, mutation))
                return SQLITE_MISUSE;

            const int rc = sqlite3_step(stmt);
            sqlite3_reset(stmt);
            return rc;
        }

        void flushOne()
        {
            std::lock_guard<std::mutex> flushLock(_flushMutex);

            std::list<Mutation> batch;
            std::size_t batchBytes = 0u;
            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                while (!_queue.empty() && batch.size() < _maxBatchEntries)
                {
                    const std::size_t nextBytes = _queue.front().storageSize();
                    if (!batch.empty() && batchBytes + nextBytes > _maxBatchBytes)
                        break;

                    batchBytes += nextBytes;
                    _queuedBytes -= std::min(_queuedBytes, nextBytes);
                    batch.splice(batch.end(), _queue, _queue.begin());
                }
                _spaceCV.notify_all();
            }

            if (batch.empty())
                return;

            OE_PROFILING_ZONE_NAMED("OE SQLite3 Cache Flush");
            CacheStatsScope transactionScope(_stats, CacheStatistics::Metric::Transaction);
            CacheStatsScope backendScope(_stats, CacheStatistics::Metric::BackendWrite);
            if (_stats)
                _stats->addTransactionMutations(batch.size());

            int rc = sqlite3_exec(_writer->db(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
            const bool transactionOpen = rc == SQLITE_OK;
            if (transactionOpen)
            {
                for (const auto& mutation : batch)
                {
                    rc = executeMutation(mutation);
                    if (rc != SQLITE_DONE)
                        break;
                }
            }

            if (rc == SQLITE_DONE)
                rc = sqlite3_exec(_writer->db(), "COMMIT", nullptr, nullptr, nullptr);

            const bool success = rc == SQLITE_OK;
            if (!success && transactionOpen && sqlite3_get_autocommit(_writer->db()) == 0)
                sqlite3_exec(_writer->db(), "ROLLBACK", nullptr, nullptr, nullptr);

            if (!success && isRetryable(rc) && batch.front().retries < MAX_TRANSACTION_RETRIES)
            {
                if (_stats)
                    _stats->addBusyRetry();
                transactionScope.error();
                backendScope.error();
                for (auto& mutation : batch)
                    ++mutation.retries;

                std::lock_guard<std::mutex> lock(_queueMutex);
                for (const auto& mutation : batch)
                    _queuedBytes += mutation.storageSize();
                _queue.splice(_queue.begin(), batch);
                _drainCV.notify_all();
                return;
            }

            if (!success)
            {
                transactionScope.error();
                backendScope.error();
                OE_WARN << LC << "SQLite transaction failed for \"" << _path << "\": "
                    << sqlite3_errmsg(_writer->db()) << std::endl;
            }
            else
            {
                transactionScope.success(batchBytes);
                backendScope.success(batchBytes);
            }

            std::vector<std::shared_ptr<Completion>> completions;
            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                for (const auto& mutation : batch)
                {
                    const RecordKey recordKey{ mutation.bin, mutation.key };
                    if (mutation.type == MutationType::Put || mutation.type == MutationType::Remove)
                    {
                        auto i = _pendingRecords.find(recordKey);
                        if (i != _pendingRecords.end() && i->second.generation == mutation.generation)
                            _pendingRecords.erase(i);
                    }
                    else if (mutation.type == MutationType::ClearBin)
                    {
                        auto i = _pendingBinClears.find(mutation.bin);
                        if (i != _pendingBinClears.end() && i->second == mutation.generation)
                            _pendingBinClears.erase(i);
                    }
                    else if (mutation.type == MutationType::ClearAll &&
                        _pendingClearAll == mutation.generation)
                    {
                        _pendingClearAll = 0u;
                    }

                    if (mutation.completion)
                        completions.push_back(mutation.completion);
                }
                _drainCV.notify_all();
            }

            for (const auto& completion : completions)
            {
                {
                    std::lock_guard<std::mutex> lock(completion->mutex);
                    completion->done = true;
                    completion->success = success;
                }
                completion->cv.notify_all();
            }
        }

        std::string _path;
        bool _separate = true;
        SQLite3CacheOptions _options;
        std::mutex _openMutex;
        std::atomic_bool _open{ false };
        std::unique_ptr<SQLiteConnection> _writer;
        std::shared_ptr<CacheStatistics> _stats;

        mutable std::mutex _queueMutex;
        std::condition_variable _spaceCV;
        std::condition_variable _drainCV;
        std::list<Mutation> _queue;
        std::size_t _queuedBytes = 0u;
        std::uint64_t _nextGeneration = 0u;
        std::unordered_map<RecordKey, PendingRecord, RecordKeyHash> _pendingRecords;
        std::unordered_map<std::string, std::uint64_t> _pendingBinClears;
        std::uint64_t _pendingClearAll = 0u;
        jobs::jobpool* _pool = nullptr;
        std::shared_ptr<jobs::jobgroup> _flushGroup;
        bool _flushScheduled = false;
        std::mutex _flushMutex;

        std::mutex _readerMutex;
        std::condition_variable _readerCV;
        std::vector<std::unique_ptr<SQLiteConnection>> _idleReaders;
        unsigned _readerCount = 0u;
        unsigned _maxReaders = 1u;

        std::size_t _maxQueueBytes = 0u;
        std::size_t _maxBatchEntries = 1u;
        std::size_t _maxBatchBytes = 1u;
    };

    class SQLite3CacheBin;

    class SQLite3Cache : public Cache
    {
    public:
        SQLite3Cache() = default;
        SQLite3Cache(const SQLite3Cache&, const osg::CopyOp&) { }
        META_Object(osgEarth, SQLite3Cache);

        explicit SQLite3Cache(const CacheOptions& options);
        ~SQLite3Cache() override;

        CacheBin* addBin(const std::string& binID) override;
        CacheBin* getOrCreateDefaultBin() override;
        void setNumThreads(unsigned num) override;
        bool compact() override;
        bool clear() override;
        off_t getApproximateSize() const override;

    private:
        std::shared_ptr<DatabaseState> createStateForBin(const std::string& binID);
        void registerState(const std::shared_ptr<DatabaseState>& state);
        std::vector<std::shared_ptr<DatabaseState>> states() const;
        std::string binDatabasePath(const std::string& binID) const;

        std::string _rootPath;
        SQLite3CacheOptions _options;
        jobs::jobpool* _pool = nullptr;
        std::shared_ptr<CacheStatistics> _stats;
        std::shared_ptr<DatabaseState> _sharedState;
        mutable std::mutex _binMutex;
        mutable std::mutex _statesMutex;
        std::vector<std::weak_ptr<DatabaseState>> _states;
    };

    class SQLite3CacheBin : public CacheBin
    {
    public:
        SQLite3CacheBin(
            const std::string& binID,
            const std::shared_ptr<DatabaseState>& state,
            const SQLite3CacheOptions& options,
            const std::shared_ptr<CacheStatistics>& stats) :
            CacheBin(binID, options.enableNodeCaching().get()),
            _state(state),
            _options(options),
            _stats(stats)
        {
            initReaderWriter();
        }

        ~SQLite3CacheBin() override
        {
            if (_state)
                _state->drain();
        }

        ReadResult readObject(const std::string& key, const osgDB::Options* dbo) override;
        ReadResult readImage(const std::string& key, const osgDB::Options* dbo) override;
        ReadResult readString(const std::string& key, const osgDB::Options* dbo) override;

        bool write(
            const std::string& key,
            const osg::Object* object,
            const Config& meta,
            const osgDB::Options* dbo) override;

        bool remove(const std::string& key) override
        {
            CacheStatsScope scope(_stats, CacheStatistics::Metric::Remove);
            const bool result = _ok && _state && _state->remove(getID(), key);
            result ? scope.success() : scope.error();
            return result;
        }

        bool touch(const std::string& key) override
        {
            CacheStatsScope scope(_stats, CacheStatistics::Metric::Touch);
            const bool result = _ok && _state && _state->touch(getID(), key);
            result ? scope.success() : scope.error();
            return result;
        }

        RecordStatus getRecordStatus(const std::string& key) override;

        bool clear() override
        {
            CacheStatsScope scope(_stats, CacheStatistics::Metric::Clear);
            const bool result = _ok && _state && _state->clearBin(getID());
            result ? scope.success() : scope.error();
            return result;
        }

        bool compact() override
        {
            return _ok && _state && _state->compact();
        }

        unsigned getStorageSize() override
        {
            return _ok && _state ? _state->storageSize(getID()) : 0u;
        }

    private:
        void initReaderWriter();
        ReadResult read(const std::string& key, const osgDB::Options* dbo, bool image);
        osg::ref_ptr<const osgDB::Options> mergeOptions(const osgDB::Options* input) const;

        std::shared_ptr<DatabaseState> _state;
        SQLite3CacheOptions _options;
        std::shared_ptr<CacheStatistics> _stats;
        osg::ref_ptr<osgDB::ReaderWriter> _rw;
        osg::ref_ptr<osgDB::Options> _rwOptions;
        std::string _compressorName;
        std::atomic_bool _ok{ true };
    };

    SQLite3Cache::SQLite3Cache(const CacheOptions& options) :
        Cache(options),
        _options(options)
    {
        if (!_options.rootPath().isSet())
        {
            const char* cachePath = ::getenv(OSGEARTH_ENV_CACHE_PATH);
            if (cachePath)
                _options.rootPath() = cachePath;
        }

        if (!_options.rootPath().isSet())
        {
            _status.set(Status::ConfigurationError, "No cache path specified");
            return;
        }

        _rootPath = URI(*_options.rootPath(), options.referrer()).full();
        if (!osgDB::makeDirectory(_rootPath))
        {
            _status.set(Status::ResourceUnavailable, Stringify()
                << "Failed to create or access folder \"" << _rootPath << "\"");
            return;
        }

        _stats = CacheStatistics::create(_options, "sqlite3", _rootPath);

        setNumThreads(_options.threads().get());

        if (!_options.separateBins().get())
        {
            std::string error;
            const std::string path = osgDB::concatPaths(_rootPath, "osgearth_cache.db");
            _sharedState = DatabaseState::create(path, false, _options, _pool, _stats, error);
            if (!_sharedState)
            {
                _status.set(Status::ResourceUnavailable, error);
                return;
            }
            registerState(_sharedState);
            OE_INFO << LC << "Opened shared SQLite cache at \"" << path << "\"" << std::endl;
        }
        else
        {
            OE_INFO << LC << "SQLite cache (separate bins) at \"" << _rootPath << "\"" << std::endl;
        }
    }

    SQLite3Cache::~SQLite3Cache()
    {
        for (const auto& state : states())
            state->drain();
    }

    void SQLite3Cache::setNumThreads(unsigned num)
    {
        _pool = nullptr;
        if (num > 0u)
        {
            _pool = jobs::get_pool("oe.sqlite3cache");
            _pool->set_can_steal_work(false);
            _pool->set_concurrency(osg::clampBetween(num, 1u, 8u));
        }

        for (const auto& state : states())
            state->setPool(_pool);
    }

    void SQLite3Cache::registerState(const std::shared_ptr<DatabaseState>& state)
    {
        std::lock_guard<std::mutex> lock(_statesMutex);
        _states.push_back(state);
    }

    std::vector<std::shared_ptr<DatabaseState>> SQLite3Cache::states() const
    {
        std::vector<std::shared_ptr<DatabaseState>> result;
        std::unordered_set<DatabaseState*> seen;
        std::lock_guard<std::mutex> lock(_statesMutex);
        for (const auto& weak : _states)
        {
            if (auto state = weak.lock())
            {
                if (seen.insert(state.get()).second)
                    result.push_back(std::move(state));
            }
        }
        return result;
    }

    std::string SQLite3Cache::binDatabasePath(const std::string& binID) const
    {
        bool safe = !binID.empty() && binID.size() <= 120u && binID != "." && binID != "..";
        for (unsigned char c : binID)
        {
            if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.'))
            {
                safe = false;
                break;
            }
        }

        std::string stem = binID.substr(0u, binID.find('.'));
        std::transform(stem.begin(), stem.end(), stem.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const std::unordered_set<std::string> reservedNames = {
            "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
            "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
            "lpt6", "lpt7", "lpt8", "lpt9", "osgearth_cache"
        };
        if (reservedNames.find(stem) != reservedNames.end())
            safe = false;

        std::string filename;
        if (safe)
        {
            filename = binID;
        }
        else
        {
            filename = Cache::makeCacheKey(binID, "bin");
            std::replace(filename.begin(), filename.end(), '/', '_');
            std::replace(filename.begin(), filename.end(), '\\', '_');
        }
        return osgDB::concatPaths(_rootPath, filename + ".db");
    }

    std::shared_ptr<DatabaseState> SQLite3Cache::createStateForBin(const std::string& binID)
    {
        if (_sharedState)
            return _sharedState;

        std::string error;
        const std::string path = binDatabasePath(binID);
        auto state = DatabaseState::create(path, true, _options, _pool, _stats, error);
        if (!state)
        {
            OE_WARN << LC << error << std::endl;
            return nullptr;
        }
        registerState(state);
        return state;
    }

    CacheBin* SQLite3Cache::addBin(const std::string& name)
    {
        if (getStatus().isError())
            return nullptr;

        std::lock_guard<std::mutex> lock(_binMutex);
        if (CacheBin* existing = _bins.get(name))
            return existing;

        auto state = createStateForBin(name);
        if (!state)
            return nullptr;

        osg::ref_ptr<CacheBin> bin = new SQLite3CacheBin(name, state, _options, _stats);
        return _bins.getOrCreate(name, bin.get());
    }

    CacheBin* SQLite3Cache::getOrCreateDefaultBin()
    {
        if (getStatus().isError())
            return nullptr;

        std::lock_guard<std::mutex> lock(_binMutex);
        if (!_defaultBin.valid())
        {
            auto state = createStateForBin("__default");
            if (state)
                _defaultBin = new SQLite3CacheBin("__default", state, _options, _stats);
        }
        return _defaultBin.get();
    }

    bool SQLite3Cache::compact()
    {
        bool ok = true;
        for (const auto& state : states())
            ok = state->compact() && ok;
        return ok;
    }

    bool SQLite3Cache::clear()
    {
        bool ok = true;
        for (const auto& state : states())
            ok = state->clearAll() && ok;
        return ok;
    }

    off_t SQLite3Cache::getApproximateSize() const
    {
        std::uintmax_t size = 0u;
        for (const auto& state : states())
            size += state->diskSize();

        const auto maximum = static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max());
        return static_cast<off_t>(std::min(size, maximum));
    }

    void SQLite3CacheBin::initReaderWriter()
    {
        _rw = osgDB::Registry::instance()->getReaderWriterForExtension(OSG_FORMAT);
        if (!_rw.valid())
        {
            OE_WARN << LC << "Failed to find ReaderWriter for \"" << OSG_FORMAT << "\"" << std::endl;
            _ok = false;
            return;
        }

        _rwOptions = Registry::instance()->cloneOrCreateOptions();
        const char* compressor = ::getenv(OSGEARTH_ENV_DEFAULT_COMPRESSOR);
        _compressorName = compressor ? compressor : "zlib";
        if (!_compressorName.empty())
            _rwOptions->setPluginStringData("Compressor", _compressorName);
    }

    osg::ref_ptr<const osgDB::Options>
    SQLite3CacheBin::mergeOptions(const osgDB::Options* input) const
    {
        if (!input)
            return _rwOptions.get();

        osg::ref_ptr<osgDB::Options> result = Registry::instance()->cloneOrCreateOptions(input);
        if (!_compressorName.empty())
            result->setPluginStringData("Compressor", _compressorName);
        return result.get();
    }

    ReadResult SQLite3CacheBin::read(
        const std::string& key,
        const osgDB::Options* dbo,
        bool isImage)
    {
        CacheStatsScope readScope(_stats, CacheStatistics::Metric::Read);
        if (!_ok || !_state)
        {
            readScope.miss();
            return ReadResult(ReadResult::RESULT_NOT_FOUND);
        }

        PendingRecord pending;
        const PendingStatus pendingStatus = _state->getPending(getID(), key, pending);
        if (pendingStatus == PendingStatus::Absent)
        {
            readScope.miss();
            return ReadResult(ReadResult::RESULT_NOT_FOUND);
        }

        if (pendingStatus == PendingStatus::Present)
        {
            ReadResult result = isImage ?
                ReadResult(const_cast<osg::Image*>(
                    dynamic_cast<const osg::Image*>(pending.object.get())), pending.meta) :
                ReadResult(const_cast<osg::Object*>(pending.object.get()), pending.meta);
            result.setLastModifiedTime(pending.timestamp);
            readScope.success();
            return result;
        }

        CacheStatsScope backendScope(_stats, CacheStatistics::Metric::BackendRead);
        bool missing;
        auto reader = _state->acquireReader(&missing);
        if (!reader)
        {
            if (missing)
            {
                backendScope.miss();
                readScope.miss();
                return ReadResult(ReadResult::RESULT_NOT_FOUND);
            }
            backendScope.error();
            readScope.error();
            return ReadResult(ReadResult::RESULT_READER_ERROR);
        }

        sqlite3_stmt* stmt = reader->statements().selectStmt;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        int rc = SQLITE_OK;
        if (_options.separateBins().get())
        {
            rc = sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        }
        else
        {
            rc = sqlite3_bind_text(stmt, 1, getID().c_str(), -1, SQLITE_TRANSIENT);
            if (rc == SQLITE_OK)
                rc = sqlite3_bind_text(stmt, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        }

        std::string data;
        std::string metaJSON;
        TimeStamp timestamp = 0;
        if (rc == SQLITE_OK)
            rc = sqlite3_step(stmt);

        if (rc == SQLITE_ROW)
        {
            const void* blob = sqlite3_column_blob(stmt, 0);
            const int blobSize = sqlite3_column_bytes(stmt, 0);
            if (blob && blobSize > 0)
                data.assign(static_cast<const char*>(blob), static_cast<std::size_t>(blobSize));

            const char* metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (metadata)
                metaJSON = metadata;
            timestamp = static_cast<TimeStamp>(sqlite3_column_int64(stmt, 2));
        }
        sqlite3_reset(stmt);

        if (rc == SQLITE_DONE)
        {
            backendScope.miss();
            readScope.miss();
            return ReadResult(ReadResult::RESULT_NOT_FOUND);
        }
        if (rc != SQLITE_ROW)
        {
            OE_WARN << LC << "SQLite read failed for key \"" << key << "\" in bin ["
                << getID() << "]: " << sqlite3_errmsg(reader->db()) << std::endl;
            backendScope.error();
            readScope.error();
            return ReadResult(ReadResult::RESULT_READER_ERROR);
        }

        // The SQLite connection is only needed while copying the row. Return it
        // before the comparatively expensive OSG deserialization so other readers
        // do not wait on the bounded connection pool.
        reader = DatabaseState::ReaderLease();
        backendScope.success(data.size());
        backendScope.finish();

        if (data.empty())
        {
            readScope.miss();
            return ReadResult(ReadResult::RESULT_NOT_FOUND);
        }

        CacheStatsScope deserializeScope(_stats, CacheStatistics::Metric::Deserialize);
        std::istringstream datastream(data);
        osg::ref_ptr<const osgDB::Options> options = mergeOptions(dbo);
        osgDB::ReaderWriter::ReadResult result = isImage ?
            _rw->readImage(datastream, options.get()) :
            _rw->readObject(datastream, options.get());

        if (!result.success())
        {
            OE_WARN << LC << "Failed to deserialize cached object for key \"" << key
                << "\" in bin [" << getID() << "]: " << result.message() << std::endl;
            deserializeScope.error();
            readScope.error();
            return ReadResult(ReadResult::RESULT_READER_ERROR);
        }
        deserializeScope.success(data.size());
        deserializeScope.finish();

        Config meta;
        if (!metaJSON.empty())
            meta.fromJSON(metaJSON);

        ReadResult output(result.getObject(), meta);
        output.setLastModifiedTime(timestamp);
        readScope.success(data.size());
        return output;
    }

    ReadResult SQLite3CacheBin::readImage(const std::string& key, const osgDB::Options* dbo)
    {
#ifdef USE_NETWORK_MONITOR
        auto handle = NetworkMonitor::begin(key, "pending", "Cache");
        auto result = read(key, dbo, true);
        NetworkMonitor::end(handle, result.succeeded() ? "OK" : "failed");
        return result;
#else
        return read(key, dbo, true);
#endif
    }

    ReadResult SQLite3CacheBin::readObject(const std::string& key, const osgDB::Options* dbo)
    {
#ifdef USE_NETWORK_MONITOR
        auto handle = NetworkMonitor::begin(key, "pending", "Cache");
        auto result = read(key, dbo, false);
        NetworkMonitor::end(handle, result.succeeded() ? "OK" : "failed");
        return result;
#else
        return read(key, dbo, false);
#endif
    }

    ReadResult SQLite3CacheBin::readString(const std::string& key, const osgDB::Options* dbo)
    {
        ReadResult result = readObject(key, dbo);
        if (result.succeeded() && !result.get<StringObject>())
            return ReadResult("Empty string");
        return result;
    }

    bool SQLite3CacheBin::write(
        const std::string& key,
        const osg::Object* object,
        const Config& meta,
        const osgDB::Options* writeOptions)
    {
        CacheStatsScope writeScope(_stats, CacheStatistics::Metric::Write);
        if (!_ok || !_state || !object)
        {
            writeScope.error();
            return false;
        }

        const bool isNode = dynamic_cast<const osg::Node*>(object) != nullptr;
        if (isNode && !_options.enableNodeCaching().get())
        {
            writeScope.success();
            return true;
        }

        CacheStatsScope serializeScope(_stats, CacheStatistics::Metric::Serialize);
        osgDB::ReaderWriter::WriteResult result;
        std::stringstream datastream;
        osg::ref_ptr<const osgDB::Options> options = mergeOptions(writeOptions);

        if (dynamic_cast<const osg::Image*>(object))
        {
            result = _rw->writeImage(
                *static_cast<const osg::Image*>(object), datastream, options.get());
        }
        else if (isNode)
        {
            result = _rw->writeNode(
                *static_cast<const osg::Node*>(object), datastream, options.get());
        }
        else
        {
            result = _rw->writeObject(*object, datastream, options.get());
        }

        if (!result.success())
        {
            OE_WARN << LC << "Failed to serialize object for key \"" << key
                << "\" in bin [" << getID() << "]: " << result.message() << std::endl;
            serializeScope.error();
            writeScope.error();
            return false;
        }

        std::string data = datastream.str();
        serializeScope.success(data.size());
        serializeScope.finish();
        const std::size_t size = data.size();
        const bool success = _state->enqueuePut(
            getID(), key, std::move(data), meta.toJSON(), meta, object);
        success ? writeScope.success(size) : writeScope.error();
        return success;
    }

    CacheBin::RecordStatus SQLite3CacheBin::getRecordStatus(const std::string& key)
    {
        if (!_ok || !_state)
            return STATUS_NOT_FOUND;

        PendingRecord pending;
        const PendingStatus pendingStatus = _state->getPending(getID(), key, pending);
        if (pendingStatus == PendingStatus::Present)
            return STATUS_OK;
        if (pendingStatus == PendingStatus::Absent)
            return STATUS_NOT_FOUND;

        auto reader = _state->acquireReader();
        if (!reader)
            return STATUS_NOT_FOUND;

        sqlite3_stmt* stmt = reader->statements().existsStmt;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        int rc = SQLITE_OK;
        if (_options.separateBins().get())
        {
            rc = sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        }
        else
        {
            rc = sqlite3_bind_text(stmt, 1, getID().c_str(), -1, SQLITE_TRANSIENT);
            if (rc == SQLITE_OK)
                rc = sqlite3_bind_text(stmt, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        }
        if (rc == SQLITE_OK)
            rc = sqlite3_step(stmt);
        sqlite3_reset(stmt);

        if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        {
            OE_WARN << LC << "SQLite status query failed for key \"" << key << "\" in bin ["
                << getID() << "]: " << sqlite3_errmsg(reader->db()) << std::endl;
        }
        return rc == SQLITE_ROW ? STATUS_OK : STATUS_NOT_FOUND;
    }
}

class SQLite3CacheDriver : public CacheDriver
{
public:
    SQLite3CacheDriver()
    {
        supportsExtension("osgearth_cache_sqlite3", "SQLite3 cache for osgEarth");
    }

    const char* className() const override
    {
        return "SQLite3 cache for osgEarth";
    }

    ReadResult readObject(const std::string& fileName, const Options* options) const override
    {
        if (!acceptsExtension(osgDB::getLowerCaseFileExtension(fileName)))
            return ReadResult::FILE_NOT_HANDLED;
        return ReadResult(new SQLite3Cache(getCacheOptions(options)));
    }
};

REGISTER_OSGPLUGIN(osgearth_cache_sqlite3, SQLite3CacheDriver)
