/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <osgEarth/LogarithmicDepthBuffer>
#include <osgEarth/Shaders>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>

#define LC "[LogarithmicDepthBuffer] "

using namespace osgEarth;
using namespace osgEarth::Util;


LogarithmicDepthBuffer::LogarithmicDepthBuffer() :
_useFragDepth(false)
{
    _supported = Registry::capabilities().supportsGLSL();
    if ( !_supported )
    {
        OE_WARN << LC << "Not supported on this platform (no GLSL)" << std::endl;
    }
}

void
LogarithmicDepthBuffer::setUseFragDepth(bool value)
{
    _useFragDepth = value;
}

void
LogarithmicDepthBuffer::install(osg::Camera* camera)
{
    if ( camera && _supported )
    {
        // install the shader component:
        osg::StateSet* stateset = camera->getOrCreateStateSet();
        
        VirtualProgram* vp = VirtualProgram::getOrCreate( stateset );
        vp->setName("Log Depth Buffer");
        Shaders pkg;

        if ( _useFragDepth )
        {
            pkg.load( vp, pkg.LogDepthBuffer );
        }
        else
        {
            pkg.load( vp, pkg.LogDepthBuffer_VertOnly );
        }

        // Lets depth-aware shaders (such as Chonk's occlusion culling) match the stored depth.
        stateset->setDefine("OE_LOG_DEPTH_BUFFER", _useFragDepth ? "2" : "1");
    }
}

void
LogarithmicDepthBuffer::uninstall(osg::Camera* camera)
{
    if ( camera && _supported )
    {
        osg::StateSet* stateset = camera->getStateSet();
        if ( stateset )
        {
            VirtualProgram* vp = VirtualProgram::get( stateset );
            if ( vp )
            {
                Shaders pkg;
                pkg.unload( vp, pkg.LogDepthBuffer );
                pkg.unload( vp, pkg.LogDepthBuffer_VertOnly );
            }
            stateset->removeDefine("OE_LOG_DEPTH_BUFFER");
        }
    }
}
