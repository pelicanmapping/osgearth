/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once

// Private, uninstalled prototype. No SDK or live scene-graph objects cross the worker boundary.
#include <osgEarthPrestige/Export>
#include <osg/Matrixd>
#include <osg/Vec2d>
#include <osg/Vec3d>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace osgUtil { class CullVisitor; }

namespace osgEarthPrestige { namespace Labeling
{
    struct OSGEARTHPRESTIGE_EXPORT LabelKey
    {
        std::uint64_t source = 0;
        std::uint64_t feature = 0;
        std::uint32_t role = 0;
        std::int32_t instance = 0;

        //! Compares feature identity, including label role and world-wrap instance; text is not identity.
        bool operator==(const LabelKey& rhs) const;
    };

    struct PathGlyph
    {
        double distance = 0; // Logical-pixel distance from the beginning of the shaped run.
        osg::Vec2d lowerLeft, upperRight; // Ink/halo bounds about this glyph's baseline sample.
    };

    struct LabelPath
    {
        std::vector<osg::Vec3d> world; // One continuous, tile-clipped line; at most 256 vertices.
        std::vector<PathGlyph> glyphs; // HarfBuzz order, including combining glyphs and ligatures.
        double width = 0;
        bool gpu = false; // Resolved GL 4.3 fitting mode with continuously interpolated screen tangents.
    };

    struct PathPlacement
    {
        struct Glyph { osg::Vec3d world, tangent; };
        std::vector<Glyph> glyphs; // Immutable, specific to one camera's solve.
    };

    struct Record
    {
        LabelKey key;
        std::uint64_t revision = 0;
        std::uint64_t continuity = 0; // Nonzero lifetime token preserves fades across edits; zero uses revision.
        osg::Vec3d worldAnchor;
        osg::Vec2d lowerLeft, upperRight; // Logical-pixel footprint relative to the anchor.
        std::uint64_t shapedRun = 0, style = 0;
        float priority = 0.0f;
        float calloutGap = 0.0f; // Zero keeps fixed placement; positive logical pixels enable eight alternatives.
        std::shared_ptr<const LabelPath> path; // Null retains the point-label fast path.
    };

    struct BatchPayload
    {
        //! Releases immutable prepared data when its tile, worker results and draw pins have all retired.
        virtual ~BatchPayload() = default;
        //! Reports owned CPU storage; shared global glyph pages are accounted by the font service separately.
        virtual std::size_t bytes() const = 0;
    };

    class OSGEARTHPRESTIGE_EXPORT Batch
    {
    public:
        //! Takes prepared records off the traversal thread; throws invalid_argument for invalid geometry.
        explicit Batch(std::vector<Record> records, std::shared_ptr<const BatchPayload> payload = {});
        //! Returns immutable records; a shared batch must never be modified after construction.
        const std::vector<Record>& records() const { return _records; }
        //! Reports retained CPU storage, including unused vector capacity, for conservative admission.
        std::size_t bytes() const { return sizeof(Batch) + _records.capacity() * sizeof(Record) + _payloadBytes + _pathBytes; }
        //! Returns optional immutable tile-local rendering resources, without touching the source or font service.
        const std::shared_ptr<const BatchPayload>& payload() const { return _payload; }
    private:
        std::vector<Record> _records;
        std::shared_ptr<const BatchPayload> _payload;
        std::size_t _payloadBytes = 0, _pathBytes = 0;
    };

    using BatchPtr = std::shared_ptr<const Batch>;

    struct OSGEARTHPRESTIGE_EXPORT ViewKey
    {
        std::uint32_t slot = 0;
        std::uint64_t generation = 0;
        //! Compares a registration lifetime, not a camera address or graphics context.
        bool operator==(const ViewKey& rhs) const;
    };

    struct OSGEARTHPRESTIGE_EXPORT CameraSnapshot
    {
        osg::Matrixd worldToView, projection;
        double x = 0.0, y = 0.0, width = 1.0, height = 1.0;
        double devicePixelRatio = 1.0;
        double time = 0.0;
        std::uint64_t frame = 0;
        bool geocentric = false; // WGS84 surface-anchor horizon filtering, matching the renderer.

        //! Checks finite matrices, time, viewport, and positive dimensions/density before admission.
        bool valid() const;
    };

    struct DeclutterOptions
    {
        std::size_t placements = 4096;
        std::size_t collisionTests = 262144;
        double padding = 2.0; // Logical pixels around each footprint; touching boundaries do not collide.
    };

    struct LayoutStats
    {
        std::size_t candidates = 0, accepted = 0, collisionTests = 0;
        bool budgetExhausted = false;
        double milliseconds = 0.0;
    };

    struct Limits
    {
        std::size_t views = 8;
        std::size_t batches = 256;
        std::size_t records = 16384;
        std::size_t inputBytes = 4 * 1024 * 1024;
        DeclutterOptions declutter;
    };

    /** Copies the current world-root cull state; call before entering local transforms, once per explicit view.
     * Never retain visitor/OSG pointers. Returns false for a missing viewport/frame stamp or invalid camera state.
     * Collect coarse batch references during traversal and submit the completed request at the end of this cull.
     */
    OSGEARTHPRESTIGE_EXPORT bool captureCamera(
        const osgUtil::CullVisitor& visitor, double devicePixelRatio, CameraSnapshot& output);

    //! Immutable camera-local scene membership; fresh registration tokens prevent reused slots inheriting visibility.
    struct SceneVisibility
    {
        std::uint64_t source = 0;
        std::array<std::uint64_t, 1024> ids {};
    };

    struct OSGEARTHPRESTIGE_EXPORT Request
    {
        ViewKey view;
        std::uint64_t sequence = 0;
        std::uint64_t configuration = 1;
        std::uint64_t dynamicRevision = 0; // Prepared annotation generation; adoption rejects superseded content.
        CameraSnapshot camera;
        std::vector<BatchPtr> batches;
        std::shared_ptr<const SceneVisibility> sceneVisibility; // Null leaves non-scene clients unchanged.
        double declutterPadding = -1.0; // Negative selects engine default; nonnegative overrides this solve only.
        bool animate = false;
        std::uint64_t displayedSequence = 0;
        double adoptedTime = 0.0; // Camera reference time when this view adopted displayedSequence.

        //! Validates a complete snapshot in O(batch count); rejects oversize inputs without truncating them.
        bool valid(const Limits& limits) const;
    };

    struct Placement
    {
        std::size_t batch = 0, record = 0;
        osg::Vec2d anchorPixels, lowerLeft, upperRight;
        std::shared_ptr<const PathPlacement> path;
        osg::Vec2d offset; // Camera-local logical-pixel displacement from the current projected anchor.
        unsigned alternative = 0; // Zero is centered; 1-8 identify stable compass alternatives.
    };

    struct OSGEARTHPRESTIGE_EXPORT Fade
    {
        double start = 0.0;
        float from = 1.0f, to = 1.0f;
        bool relative = false; // Relative starts use the owning result's actual adoption time.
        float velocity = 0.0f; // Prior curve slope, used to resolve reversals at actual adoption, not request time.
        double sampledTime = 0.0;
        //! Evaluates the 200 ms linear transition without mutating it; time uses the camera reference clock.
        float opacity(double time, double adoptedTime) const;
        //! Returns the absolute completion time, including any entry delay.
        double end(double adoptedTime) const;
        //! Compares immutable animation descriptors for instance-buffer reuse.
        bool operator==(const Fade& rhs) const;
    };

    struct PresentedLabel
    {
        BatchPtr batch;
        std::size_t record = 0;
        Fade fade;
        std::shared_ptr<const PathPlacement> path;
        osg::Vec2d offset; // Retained through outgoing fades and immutable render-packet reuse.
    };

    struct ResultPayload
    {
        //! Releases immutable renderer preparation on the engine worker after frame pins retire.
        virtual ~ResultPayload() = default;
    };

    struct Result
    {
        std::unique_ptr<const Request> input;
        std::vector<Placement> placements;
        std::shared_ptr<const ResultPayload> payload;
        LayoutStats layout;
        std::vector<PresentedLabel> presentation;
        double settledTime = 0.0; // Seconds after adoption until transitions can be compacted by a follow-up solve.
    };

    using ResultPtr = std::shared_ptr<const Result>;
    using Solver = std::function<std::vector<Placement>(const Request&, const std::atomic<bool>&)>;
    using PrepareResult = std::function<void(Result&)>;

    //! Fits a complete shaped run to a projected path on the worker; rejects short, hidden or sharply bent paths.
    //! Produces camera-local immutable glyph anchors/tangents and a conservative collision rectangle.
    OSGEARTHPRESTIGE_EXPORT bool placePath(const Record& record, const CameraSnapshot& camera, Placement& output);

    //! Projects bounded candidates in double precision; cancellation drops the entire unpublished result.
    OSGEARTHPRESTIGE_EXPORT std::vector<Placement> project(const Request& request, const std::atomic<bool>& canceled);

    /** Solves one bounded, deterministic greedy layout; call only on a preparation thread with valid input.
     * Higher priorities win; equal priorities prefer unchanged identities accepted in this view's previous result.
     * A bounded grid handles normal footprints; oversized footprints use an exact scan. Budgets publish a safe
     * prefix, cancellation returns nothing. Results may overlap temporarily when reprojected during camera motion.
     * Previous results from another view/generation/configuration are ignored. Invalid options throw.
     */
    OSGEARTHPRESTIGE_EXPORT std::vector<Placement> declutter(const Request& request,
        const std::atomic<bool>& canceled, const Result* previous = nullptr,
        const DeclutterOptions& options = DeclutterOptions(), LayoutStats* stats = nullptr);

    //! Prepares per-label curves on the worker, using only this camera's acknowledged displayed result.
    //! Exits reserve the view against new entries for at most 200 ms. Storage obeys the supplied limits.
    OSGEARTHPRESTIGE_EXPORT void stabilize(Result& result, const Result* displayed, const Limits& limits);

    //! Compares pose and screen projection, ignoring near/far-only changes from automatic depth adjustment.
    OSGEARTHPRESTIGE_EXPORT bool samePose(const CameraSnapshot& a, const CameraSnapshot& b);

    //! Tests viewport, direction, age and up to 32 accepted anchors; never performs collision work or allocates.
    //! Ordinary motion is allowed for 500 ms; jumps above 35 degrees or 35% of the viewport reject old layouts.
    //! stationaryTime can extend age from the last stationary interval when motion first resumes after idle.
    OSGEARTHPRESTIGE_EXPORT bool compatible(const Result& result, const CameraSnapshot& camera, double stationaryTime = -1.0);

    class OSGEARTHPRESTIGE_EXPORT Engine;

    /** An explicitly registered placement view. Rendering operations use try_lock and never wait for the worker.
     * A caller serializes submissions/adoption for a given view; different views may cull concurrently.
     * Camera ownership stays with the caller. Retain acquired results for the entire cull/draw/pick frame.
     */
    class OSGEARTHPRESTIGE_EXPORT View
    {
    public:
        //! Returns the immutable registration identity; reuse of a registry slot increments its generation.
        ViewKey key() const { return _key; }
        //! Advances hard-invalidation state (resize, projection convention, content reset); never waits.
        std::uint64_t invalidate();
        //! Returns the current hard-invalidation revision for a new snapshot.
        std::uint64_t configuration() const { return _configuration.load(); }
        /** Attempts ownership transfer; on contention, invalid input, or backpressure leaves request untouched.
         * Replaced requests are retired by the worker. Sequence numbers must increase within a registration.
         */
        bool submit(std::unique_ptr<const Request>& request);
        /** Returns a newer compatible publication, or null without waiting. Older camera poses remain usable.
         * The worker retains a reference until all frame pins are released, preventing cull-side reclamation.
         */
        ResultPtr acquire(std::uint64_t afterSequence = 0) const;
        //! Nonblocking frame demand: pending/running work or an unconsumed compatible publication; contention says yes.
        bool pendingOrNew(std::uint64_t afterSequence) const;
        //! Reports solver failures; exceptions drop that request, retain prior output, and do not kill the worker.
        std::uint64_t failures() const { return _failures.load(); }

    private:
        friend class Engine;
        //! Creates an empty bounded mailbox; only Engine registers views.
        View(ViewKey key, const Limits& limits);
        //! Releases obsolete inputs/results on the worker, outside the mailbox lock.
        void reclaim();
        //! Runs at most one request; the sole engine worker visits views in round-robin order.
        bool solve(const Solver& solver, const PrepareResult& prepare);

        const ViewKey _key;
        const Limits _limits;
        std::atomic<std::uint64_t> _configuration { 1 }, _failures { 0 };
        std::atomic<bool> _closed { false };
        mutable std::mutex _mutex;
        std::uint64_t _lastSequence = 0;
        std::unique_ptr<const Request> _pending;
        ResultPtr _pendingDisplay;
        std::array<std::unique_ptr<const Request>, 2> _retired;
        std::array<ResultPtr, 2> _retiredDisplays;
        std::array<ResultPtr, 4> _results;
        int _published = -1;
        bool _solving = false; // Protected by _mutex, including the worker's publication/failure boundary.
    };

    /** Private C++17 scheduling core. One worker services all views, with no per-camera job queue or thread.
     * Registration, removal and destruction are control/update-thread operations, never cull/draw operations.
     * Shutdown joins only on that control thread; custom solvers must bound their work and honor cancellation.
     */
    class OSGEARTHPRESTIGE_EXPORT Engine
    {
    public:
        //! Starts a bounded worker. An empty solver selects per-view decluttering; preparation precedes publication.
        //! Invalid limits throw. Explicit custom solvers (including project) replace collision placement.
        explicit Engine(const Limits& limits = Limits(), Solver solver = {}, PrepareResult prepare = {});
        //! Closes all registrations and joins the worker; stop traversal and release frame pins first.
        ~Engine();
        //! Registers an independent view; returns null while every slot is live or awaiting worker retirement.
        std::shared_ptr<View> registerView();
        //! Closes exactly this generation; late submissions/results are rejected, reclamation stays on the worker.
        void removeView(const std::shared_ptr<View>& view);
        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

    private:
        struct State;
        std::unique_ptr<State> _state;
    };
} }
