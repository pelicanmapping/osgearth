/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#pragma once
#include "LabelRendererInternal.h"

namespace osgEarthPrestige { namespace Labeling
{
    struct DynamicLabel
    {
        osg::Vec3d worldAnchor; // Absolute world coordinates; terrain resolution is a separate preparation step.
        std::string text;
        TextStyle style;
        float priority = 0;
    };

    struct DynamicOptions
    {
        std::uint64_t sourceID = 2; // Must differ from other sources attached to the same placement engine.
        std::size_t labels = 1024, textBytes = 1024 * 1024, preparedBytes = 8 * 1024 * 1024;
    };

    struct DynamicSet
    {
        std::uint64_t revision = 0;
        std::vector<BatchPtr> batches; // Stable 64-slot chunks; edits replace only their containing chunk.
    };

    struct DynamicStats
    {
        std::uint64_t submitted = 0, completed = 0, published = 0, failures = 0, superseded = 0;
        std::size_t labels = 0, textBytes = 0;
        std::string error;
    };

    /** Bounded, asynchronous annotation content, shared by independent camera attachments.
     * Edits coalesce into the latest desired set; font work and retirement run on one owned preparation worker.
     * Stable slots avoid regrouping on insert/erase. Four publication slots apply backpressure when readers pin old sets.
     * No placement/fades live here.
     * This private source does not mutate a FeatureSource or override records belonging to a tiled source.
     */
    class OSGEARTHPRESTIGE_EXPORT DynamicSource
    {
    public:
        //! Starts a font worker. Invalid limits throw; asynchronous font/preparation failures appear in stats().
        explicit DynamicSource(std::string font, const DynamicOptions& options = {});
        //! Stops publication and joins bounded preparation on the control thread, after traversal has stopped.
        ~DynamicSource();
        /** Adds or replaces an ID, preserving its lifetime until erase. Returns its new revision, or zero on capacity/failure.
         * Thread-safe control/update-thread operation; invalid values throw without changing the desired set.
         */
        std::uint64_t set(std::uint64_t id, DynamicLabel value);
        //! Removes an ID asynchronously; returns the submission revision, or zero if absent/closed. Safe on control threads.
        std::uint64_t erase(std::uint64_t id);
        //! Pins a newer complete content set without waiting; each camera supplies its own last adopted revision.
        std::shared_ptr<const DynamicSet> acquire(std::uint64_t after = 0) const;
        //! Reports preparation or a newer publication without waiting; failures settle until another edit arrives.
        bool pendingOrNew(std::uint64_t after) const;
        //! Copies bounded diagnostics on the control thread; completed includes failed attempts, published only successes.
        DynamicStats stats() const;
        DynamicSource(const DynamicSource&) = delete;
        DynamicSource& operator=(const DynamicSource&) = delete;
    private:
        struct State;
        std::unique_ptr<State> _state;
    };
} }
