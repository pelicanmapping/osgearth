/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#pragma once
#include "LabelDynamicInternal.h"
#include <osg/Camera>
#include <osg/observer_ptr>
#include <bitset>

namespace osgEarthPrestige { namespace Labeling
{
    /** Bounded scene membership and camera-local cull snapshots. No scene pointers enter placement requests.
     * Control operations require stopped traversal; visit/poll support concurrent primary-camera culls.
     * The source's short mutex never protects shaping, layout, or GL work.
     */
    class OSGEARTHPRESTIGE_EXPORT SceneSource
    {
    public:
        //! Allocates bounded registration state; no worker starts until an open layer has registered nodes.
        SceneSource();
        //! Stops the content worker after traversal has stopped.
        ~SceneSource();
        //! Configures a new map attachment; starts content preparation only when nodes are registered.
        void open(const std::string& font, std::uint64_t sourceID);
        //! Stops preparation and clears camera snapshots while retaining node registrations for reopen.
        void close();
        //! Allocates a fresh identity; throws when 1024 live registrations are already present.
        std::uint64_t add();
        //! Retires an identity and its prepared content; ignores an already retired identity.
        void remove(std::uint64_t id);
        //! Marks a visible node and submits only changed content/anchors; returns false on content capacity failure.
        bool visit(osg::Camera* camera, std::uint64_t frame, std::uint64_t id, std::uint64_t revision,
            const DynamicLabel& value, const osg::Vec3d& world);
        //! Returns immutable membership from frame-1, independent of scene child order; reuses unchanged snapshots.
        std::shared_ptr<const SceneVisibility> visibility(osg::Camera* camera, std::uint64_t frame);
        //! Pins the shared content source, or null when closed/no nodes have needed preparation.
        std::shared_ptr<DynamicSource> source() const;
        //! Reports cull changes not yet consumed by a camera router, to wake an on-demand viewer.
        bool needsFrame() const;
    private:
        struct State;
        std::unique_ptr<State> _state;
    };
} }
