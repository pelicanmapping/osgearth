/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarthProcedural2/Export>
#include <osg/StateSet>

namespace osgEarth { namespace Procedural2
{
    //! Installs two-sided leaf lighting and view-corrected baked crown normals before scene publication.
    //! Existing authored volume normals retain their orientation; only baked impostors fade their edge-on cards.
    OSGEARTHPROCEDURAL2_EXPORT void installAssetLighting(osg::StateSet* state);
} }
