/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarthPrestige/Scatter>
#include <osgEarth/PagedNode>
#include <osg/StateSet>
#include <array>
#include <mutex>

namespace osgEarthPrestige
{
    using namespace osgEarth;
    //! Shared immutable interval states; lazily allocates at most 2145 states per population, never per instance.
    struct CanopyTransitionStates
    {
        std::mutex mutex;
        std::array<osg::ref_ptr<osg::StateSet>,65*65> states;
        //! Returns cached alpha-weight endpoints; caller supplies integers 0 <= low <= high <= 64.
        osg::StateSet* get(unsigned low, unsigned high);
    };

    //! Computes a continuous screen-error handover, limited by the age of a newly merged child set.
    //! Missing children must be handled by the caller; seconds/overlap are validated ScatterGroup settings.
    OSGEARTHPRESTIGE_EXPORT float canopyBlend(double pixels, double threshold, float overlap,
        double age, float seconds);

    //! Returns parent/child alpha ramps with one always fully covered. A2C masks overlap rather than add;
    //! recursive multiplication therefore preserves forest coverage, including an inherited outer-page fade.
    OSGEARTHPRESTIGE_EXPORT osg::Vec2f canopyAlphaWeights(float childWeight);

    //! Installs crown selection, stretch-independent volume lighting, and alpha-ramped representation transitions.
    OSGEARTHPRESTIGE_EXPORT void installCanopyShader(osg::StateSet*);

    //! Reads global SSE from this cull traversal, adds the shared population adjustment once, and clamps to 1px.
    //! Missing map context uses the default 25px global budget. The uniform changes only during update traversal.
    OSGEARTHPRESTIGE_EXPORT float populationError(osg::NodeVisitor&, const osg::Uniform* adjustment);

    //! Conservative projected bound diameter in primary-camera pixels, including camera LOD scale; -1 if unavailable.
    OSGEARTHPRESTIGE_EXPORT double populationPixelSize(const osg::BoundingSphere&, osg::NodeVisitor&);

    //! Visible coverage for a page diameter and effective error; smooth 0..1 over 0.75..1.25 times the error budget.
    //! An unavailable projection (-1) conservatively retains the page. No density or placement mutation.
    OSGEARTHPRESTIGE_EXPORT float populationVisibility(double pixels, float error);

    //! Enables Chonk alpha ramps for population paging; single-sample depth/shadow passes retain hard cutouts.
    OSGEARTHPRESTIGE_EXPORT void installPopulationFadeShader(osg::StateSet*);

    //! Fades a whole page/subtree using its conservative bound. Attach only at the first enabled representation tier.
    //! Cull-only callback retains shared policy/state, not its node; it never mutates shared per-camera uniforms.
    OSGEARTHPRESTIGE_EXPORT void installPopulationPageFade(osg::Node*, osg::Uniform* adjustment,
        std::shared_ptr<CanopyTransitionStates>);

    //! Configures screen-error refinement and coverage-preserving A2C blending before publication, retaining shared policy.
    //! worldError is the representation's error in meters; distance priorities remain owned by SimplePager.
    OSGEARTHPRESTIGE_EXPORT void configureCanopyTransition(Util::PagedNode2*, double worldError,
        const ScatterGroup&, std::shared_ptr<CanopyTransitionStates>, osg::Uniform* adjustment = nullptr);
}
