/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarthProcedural2/Export>
#include <osg/StateSet>

namespace osgEarth { namespace Procedural2
{
    //! Installs two-sided leaf lighting before scene publication; authored volume normals retain their orientation.
    OSGEARTHPROCEDURAL2_EXPORT void installAssetLighting(osg::StateSet* state);
} }
