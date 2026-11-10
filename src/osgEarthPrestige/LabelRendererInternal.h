/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#pragma once
#include "LabelTextInternal.h"
#include <osg/Group>
#include <osg/Geometry>

namespace osgEarthPrestige { namespace Labeling
{
    class TileSource;
    class TileView;
    struct TileSet;
    class DynamicSource;
    class SceneSource;
    struct DynamicSet;
    struct TextStyle
    {
        float pixels = 24.0f;
        osg::Vec4f color { 1, 1, 1, 1 };
        osg::Vec4f halo { 0.02f, 0.04f, 0.08f, 0.9f };
        float haloWidth = 1.0f;
        float haloSoftness = 0.0f; // Fraction [0,1] of the outer halo radius to feather inward.
        float calloutGap = 0.0f; // Opt-in point alternatives; fixed 12px in the earth-file callout slice.
        float rotation = 0.0f; // Radians, around the label anchor.
    };

    struct OSGEARTHPRESTIGE_EXPORT TextBatchResources : BatchPayload
    {
        std::shared_ptr<const TextResources> text;
        std::vector<TextStyle> styles;
        bool incremental = false; // Small independently replaceable chunks; ordinary tiled batches merge by atlas page.
        //! Conservatively counts tile-local run storage; shared atlas images are excluded.
        std::size_t bytes() const override;
    };

    //! Sets a conservative rotated ink/halo footprint matching the renderer; call during batch preparation.
    //! Invalid styles or run extents throw. The resulting record bounds are in logical pixels.
    OSGEARTHPRESTIGE_EXPORT void setTextFootprint(Record& record, const TextRun& run, const TextStyle& style);

    /** Creates worker preparation for an immutable text/style set.
     * Shares glyph arrays per atlas page/packet between separate halo and fill passes.
     * Run and style IDs index these arrays. Caps a result at 131,072 glyph instances, including outgoing labels.
     * Caches up to eight view slots; unchanged chunk selections/fades reuse immutable instance buffers.
     * Ordinary tiled/static labels merge by point/path kind; dynamic chunks prepare independently.
     */
    OSGEARTHPRESTIGE_EXPORT PrepareResult prepareText(
        std::shared_ptr<const TextResources> resources = {}, std::vector<TextStyle> styles = {});

    /** Explicit world-root attachment for one camera. Wrap shared map content in a separate node per view.
     * Cull posts ordinary snapshots at 15 Hz, with urgent refresh on invalidation; font/glyph packing stays off cull.
     * Four reusable frame slots pin current-camera uniforms and immutable result geometry until draw retires.
     * Construct/destroy on the control thread, with traversal stopped during destruction.
     */
    class OSGEARTHPRESTIGE_EXPORT ViewNode : public osg::Group
    {
    public:
        //! Registers a view over immutable batches; engine must use prepareText. Exhausted capacity throws.
        ViewNode(std::shared_ptr<Engine> engine, std::vector<BatchPtr> batches, float density = 1.0f,
            bool geocentric = true, std::shared_ptr<TileSource> tiles = {}, std::shared_ptr<DynamicSource> updates = {},
            std::shared_ptr<SceneSource> scene = {});
        //! Copies world-root cull state and emits frame-pinned render leaves; other traversals visit children.
        void traverse(osg::NodeVisitor& visitor) override;
        //! Updates this view's pixel ratio immediately before its cull; invalid values are ignored.
        //! Caller must hold exclusive cull ownership of this ViewNode, or stop traversal.
        void setDevicePixelRatio(float density);
        //! Requests a new collision margin from the control thread; preserves registration/history/fades.
        //! Invalid values throw. The next cull submits the latest value to the background solver.
        void setDeclutterPadding(double padding);
        //! Returns diagnostic registration identity; placement and fade state remain private to this view.
        ViewKey viewKey() const { return _view->key(); }
        //! Returns the number of newly adopted results; read after stopping cull threads.
        std::uint64_t publications() const { return _publications; }
        //! Returns submitted label draw calls; read after stopping cull threads.
        std::uint64_t submissions() const { return _submissions; }
        //! Returns this camera's latest solver counters; read only after stopping cull threads.
        LayoutStats layoutStats() const { return _displayed ? _displayed->layout : LayoutStats(); }
        //! Thread-safe frame demand for an on-demand viewer's control loop; true while work or fades are pending.
        bool needsFrame() const;
        //! Counts suppressed stale/jumped layouts; read after stopping cull threads.
        std::uint64_t invalidations() const { return _invalidations; }
        //! Returns this camera's displayed annotation generation; read only after stopping cull threads.
        std::uint64_t displayedDynamicRevision() const
        { return _displayed ? _displayed->input->dynamicRevision : 0; }
        //! A view registration cannot be copied into another camera attachment.
        ViewNode(const ViewNode&) = delete;
        ViewNode& operator=(const ViewNode&) = delete;

    protected:
        //! Retires this registration without waiting for layout; application ownership keeps Engine alive.
        ~ViewNode() override;

    private:
        struct Frame
        {
            ResultPtr pin;
            std::vector<osg::ref_ptr<osg::Geometry>> meshes;
        };
        std::shared_ptr<Engine> _engine;
        std::shared_ptr<View> _view;
        std::shared_ptr<TileSource> _tileSource;
        std::shared_ptr<TileView> _tileView;
        std::shared_ptr<const TileSet> _tiles;
        std::shared_ptr<SceneSource> _sceneSource;
        std::shared_ptr<const SceneVisibility> _sceneVisibility;
        std::shared_ptr<DynamicSource> _dynamicSource;
        std::shared_ptr<const DynamicSet> _dynamic;
        CameraSnapshot _tileCamera;
        bool _haveTileCamera = false;
        std::vector<BatchPtr> _baseBatches, _batches;
        std::array<Frame, 4> _frames;
        ResultPtr _displayed;
        CameraSnapshot _last;
        bool _haveCamera = false, _geocentric;
        float _density;
        double _lastSubmit = -1.0, _adoptedTime = 0.0, _stationaryTime = 0.0;
        bool _compacted = true, _inMotion = false, _refresh = false;
        std::atomic<bool> _needsFrame { true }, _layoutRefresh { false };
        std::atomic<double> _declutterPadding { -1.0 };
        std::atomic<std::uint64_t> _dynamicSeen { 0 };
        std::uint64_t _sequence = 0, _seenSequence = 0;
        std::uint64_t _publications = 0, _submissions = 0, _invalidations = 0;
    };
} }
