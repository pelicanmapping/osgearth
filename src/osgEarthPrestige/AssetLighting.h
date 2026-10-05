/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarthPrestige/Export>
#include <osg/StateSet>

namespace osgEarthPrestige
{
    //! Installs two-sided leaf lighting and view-corrected baked crown normals before scene publication.
    //! Existing authored volume normals retain their orientation; only baked impostors fade their edge-on cards.
    OSGEARTHPRESTIGE_EXPORT void installAssetLighting(osg::StateSet* state);
}
