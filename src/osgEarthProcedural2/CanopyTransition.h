/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarthProcedural2/Scatter>
#include <osgEarth/PagedNode>
#include <osg/StateSet>
#include <array>
#include <mutex>

namespace osgEarth { namespace Procedural2
{
    //! Shared immutable interval states; lazily allocates at most 2145 states per population, never per instance.
    struct CanopyTransitionStates
    {
        std::mutex mutex;
        std::array<osg::ref_ptr<osg::StateSet>,65*65> states;
        //! Returns a cached screen-door interval; caller supplies integer endpoints 0 <= low <= high <= 64.
        osg::StateSet* get(unsigned low, unsigned high);
    };

    //! Computes a continuous screen-error handover, limited by the age of a newly merged child set.
    //! Missing children must be handled by the caller; seconds/overlap are validated ScatterGroup settings.
    OSGEARTHPROCEDURAL2_EXPORT float canopyBlend(double pixels, double threshold, float overlap,
        double age, float seconds);

    //! Splits an inherited integer interval into complementary parent/child coverage, supporting nested fades.
    OSGEARTHPROCEDURAL2_EXPORT unsigned canopySplit(unsigned low, unsigned high, float childWeight);

    //! Installs crown selection, stretch-independent volume lighting, and complementary color/depth/shadow dithering.
    OSGEARTHPROCEDURAL2_EXPORT void installCanopyShader(osg::StateSet*);

    //! Reads global SSE from this cull traversal, adds the shared population adjustment once, and clamps to 1px.
    //! Missing map context uses the default 25px global budget. The uniform changes only during update traversal.
    OSGEARTHPROCEDURAL2_EXPORT float populationError(osg::NodeVisitor&, const osg::Uniform* adjustment);

    //! Conservative projected bound diameter in primary-camera pixels, including camera LOD scale; -1 if unavailable.
    OSGEARTHPROCEDURAL2_EXPORT double populationPixelSize(const osg::BoundingSphere&, osg::NodeVisitor&);

    //! Visible coverage for a page diameter and effective error; smooth 0..1 over 0.75..1.25 times the error budget.
    //! An unavailable projection (-1) conservatively retains the page. No density or placement mutation.
    OSGEARTHPROCEDURAL2_EXPORT float populationVisibility(double pixels, float error);

    //! Installs complementary coverage discard for population paging in color, depth and shadow passes.
    OSGEARTHPROCEDURAL2_EXPORT void installPopulationFadeShader(osg::StateSet*);

    //! Fades a whole page/subtree using its conservative bound. Attach only at the first enabled representation tier.
    //! Cull-only callback retains shared policy/state, not its node; it never mutates shared per-camera uniforms.
    OSGEARTHPROCEDURAL2_EXPORT void installPopulationPageFade(osg::Node*, osg::Uniform* adjustment,
        std::shared_ptr<CanopyTransitionStates>);

    //! Configures screen-error refinement and complementary blending before publication, retaining shared policy.
    //! worldError is the representation's error in meters; distance priorities remain owned by SimplePager.
    OSGEARTHPROCEDURAL2_EXPORT void configureCanopyTransition(Util::PagedNode2*, double worldError,
        const ScatterGroup&, std::shared_ptr<CanopyTransitionStates>, osg::Uniform* adjustment = nullptr);
} }
