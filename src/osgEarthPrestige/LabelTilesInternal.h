/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#pragma once
#include "LabelRendererInternal.h"
#include <osgEarth/FeatureSource>
#include <osgEarth/TileKey>
#include <chrono>

namespace osgEarthPrestige { namespace Labeling
{
    struct TileStyleRule
    {
        std::vector<std::string> classes;
        unsigned minLevel = 0, maxLevel = 16, style = 0;
        double priority = 0; // Class priority minus the source rank; lower ranks are more important.
        bool line = false; // Fits shaped glyphs to a linear feature instead of a point.
    };

    struct TileOptions
    {
        std::string textAttribute = "name", priorityAttribute = "rank_max";
        std::string fallbackTextAttribute, classAttribute = "class";
        std::vector<TileStyleRule> rules; // Empty retains the bundled Natural Earth rank/zoom policy.
        bool gpuPaths = false; // Resolved mode: the earth layer enables this by default when OpenGL 4.3 is supported.
        bool requireFeatureID = false; // Native tiled feeds must provide nonzero, stable IDs across levels.
        std::uint64_t sourceID = 1;
        unsigned maxLevel = 8, tilesPerView = 24, recordsPerTile = 256, residentTiles = 128;
        std::size_t residentBytes = 32 * 1024 * 1024;
        double tilePixels = 512;
        bool repairLegacyControls = false; // Opt-in repair for the repository's older Natural Earth sample.
    };

    struct TileStats
    {
        std::uint64_t loads = 0, evictions = 0, failures = 0, skipped = 0, truncated = 0;
        std::size_t residentTiles = 0, residentBytes = 0;
        std::uint64_t styleUpdates = 0, restyledTiles = 0;
        std::string error;
    };

    struct TileSet
    {
        std::uint64_t revision = 0;
        std::vector<BatchPtr> batches;
        std::vector<osgEarth::TileKey> coverage;
        bool complete = false;
    };

    //! Selects a bounded geocentric quadtree cut on a preparation thread; no source, font, or GL calls.
    OSGEARTHPRESTIGE_EXPORT std::vector<osgEarth::TileKey> selectLabelTiles(
        const CameraSnapshot& camera, const osgEarth::Profile* profile, const TileOptions& options);

    //! Replaces changed style payloads and bounds off traversal; unchanged batches, shaped runs and atlases are reused.
    //! Requires a validated style table with unchanged indexing; preserves record identity/revision for camera history.
    OSGEARTHPRESTIGE_EXPORT BatchPtr restyleTextBatch(const BatchPtr& batch, const std::vector<TextStyle>& styles);

    class TileSource;
    class OSGEARTHPRESTIGE_EXPORT TileView
    {
    public:
        //! Replaces a value-only camera demand without waiting; retry next cull on contention.
        bool submit(const CameraSnapshot& camera);
        //! Pins a newer immutable coverage set; content reclamation remains on the tile worker.
        std::shared_ptr<const TileSet> acquire(std::uint64_t after) const;
        //! Reports pending/loading work for on-demand frame loops; failures back off rather than spin forever.
        bool busy() const { return !_closed.load() && (_busy.load() || _readyRevision.load() > _consumed.load()); }
        //! Cancels this registration; call from its owning ViewNode after traversal has stopped.
        void close() { _closed.store(true); }
    private:
        friend class TileSource;
        mutable std::mutex _mutex;
        CameraSnapshot _camera;
        bool _pending = false;
        std::atomic<bool> _closed { false }, _busy { false };
        std::atomic<std::uint64_t> _readyRevision { 0 };
        mutable std::atomic<std::uint64_t> _consumed { 0 };
        std::array<std::shared_ptr<const TileSet>, 4> _results;
        int _published = -1;
        std::uint64_t _revision = 0;
        std::vector<osgEarth::TileKey> _wanted; // Tile worker only.
        std::uint64_t _cacheRevision = 0;
        std::chrono::steady_clock::time_point _nextCheck;
    };

    /** Shared bounded tile cache and one source/font preparation worker, separate from the placement worker.
     * Supports geocentric point and path labels from geographic or Web Mercator tile profiles.
     * Source must be open and supply stable feature IDs across queries/levels.
     * This worker owns cursor access; callers must not edit/close the source until the service is destroyed.
     * Source-side allocations/caches are outside this service's budget. Each registration supplies its own camera.
     * Profile/TileKey, geographic bounds, FeatureSource queries and cancellation reuse core osgEarth facilities.
     */
    class OSGEARTHPRESTIGE_EXPORT TileSource
    {
    public:
        //! Starts preparation. Invalid options/source throw; font/query failures are available through stats().
        TileSource(osgEarth::FeatureSource* source, std::string font, std::vector<TextStyle> styles,
            const TileOptions& options = {});
        //! Stops loading and joins on the control thread; never destroy the service in cull/draw.
        ~TileSource();
        //! Registers up to eight live cameras. Returns null if all slots are occupied; control-thread operation.
        std::shared_ptr<TileView> registerView();
        //! Copies bounded diagnostics; control-thread only, not a cull operation.
        TileStats stats() const;
        //! Queues the latest table; appearance-only edits reuse source data, and all edits preserve camera registrations.
        //! Control-thread call; invalid values throw, a stopped/failed worker returns false. Table size must stay fixed.
        //! Optional rules replace level bounds only; changed eligibility lazily requeries affected tile levels.
        bool setStyles(std::vector<TextStyle> styles, std::vector<TileStyleRule> rules = {});
        TileSource(const TileSource&) = delete;
        TileSource& operator=(const TileSource&) = delete;
    private:
        struct State;
        std::unique_ptr<State> _state;
    };
} }
