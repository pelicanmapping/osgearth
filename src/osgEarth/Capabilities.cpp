/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "Capabilities"
#include "SpatialReference"
#include "Registry"
#include "Notify"

#include <osg/FragmentProgram>
#include <osg/GL2Extensions>
#include <osg/GLExtensions>
#include <osg/Version>
#include <osgViewer/Version>

#include <gdal.h>
#include <sstream>
#include <mutex>

using namespace osgEarth;

#define LC "[Capabilities] "

#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK           0x9126
#endif
#ifndef GL_CONTEXT_CORE_PROFILE_BIT
#define GL_CONTEXT_CORE_PROFILE_BIT       0x00000001
#endif

// ---------------------------------------------------------------------------
// A custom graphics context that we will use to query for OpenGL
// extension and hardware support. (Adapted from osgconv in OpenSceneGraph)

namespace
{
    struct MyGraphicsContext
    {
        MyGraphicsContext()
        {
            // If the number of graphics context is > 0 or < 32 (the default, unitialized value of osg::DisplaySettings::instance()->getMaxNumberOfGraphicsContexts()) then warn users
            // to call osgEarth::initialize before realizing any graphics windows to avoid issues with the maxNumberOfGraphicsContexts being different than the
            // actual number of registered GraphicsContexts in osg.  This can cause issues with unrefAfterApply due to faulty logic in osg::Texture::areAllTextureObjectsLoaded
            // iterating over all of the max number of graphics contexts and checking whether textures are loaded for that context, even if the context isn't still in use.
            // Realizing windows before the capabilities context can also cause odd issues with textures dissapearing in some cases.  It's much safer just to call
            // osgEarth::initialize() at the start of your application to avoid these issues altogether.
            if (osg::DisplaySettings::instance()->getMaxNumberOfGraphicsContexts() > 0 && osg::DisplaySettings::instance()->getMaxNumberOfGraphicsContexts() < 32)
            {
                OE_WARN << "WARNING:  Call osgEarth::initialize() before realizing any graphics windows.  There are currently " << osg::DisplaySettings::instance()->getMaxNumberOfGraphicsContexts() << " graphics contexts." << std::endl;
            }

    	    osg::GraphicsContext::ScreenIdentifier si;
	        si.readDISPLAY();
	        si.setUndefinedScreenDetailsToDefaultScreen();

            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits(osg::DisplaySettings::instance());
    	    traits->hostName = si.hostName;
	        traits->displayNum = si.displayNum;
	        traits->screenNum = si.screenNum;
            traits->x = 0;
            traits->y = 0;
            traits->width = 1;
            traits->height = 1;
            traits->windowDecoration = false;
            traits->doubleBuffer = false;
            traits->sharedContext = 0;
            traits->pbuffer = false;

            // Note: these only work when OSG_GL3_FEATURES is defined (since GL < 3 just gives you the highest version context it can)
            traits->glContextVersion = osg::DisplaySettings::instance()->getGLContextVersion();
            traits->glContextProfileMask = osg::DisplaySettings::instance()->getGLContextProfileMask();

            // Intel graphics adapters dont' support pbuffers, and some of their drivers crash when
            // you try to create them. So by default we will only use the unmapped/pbuffer method
            // upon special request.
            if ( getenv( "OSGEARTH_USE_PBUFFER_TEST") )
            {
                traits->pbuffer = true;
                OE_INFO << LC << "Activating pbuffer test for graphics capabilities" << std::endl;
                _gc = osg::GraphicsContext::createGraphicsContext(traits.get());
                if ( !_gc.valid() )
                    OE_WARN << LC << "Failed to create pbuffer" << std::endl;
            }

            if (!_gc.valid())
            {
                // fall back on a mapped window
                traits->pbuffer = false;
                _gc = osg::GraphicsContext::createGraphicsContext(traits.get());
            }

            if (_gc.valid())
            {
                _gc->realize();
                _gc->makeCurrent();

                if ( traits->pbuffer == false )
                {
                    OE_DEBUG << LC << "Realized graphics window for OpenGL operations." << std::endl;
                }
                else
                {
                    OE_DEBUG << LC << "Realized pbuffer for OpenGL operations." << std::endl;
                }
            }
            else
            {
                OE_WARN << LC << "Failed to realize graphic window - using GL 3.3 default capabilities" << std::endl;
            }
        }

        bool valid() const { return _gc.valid() && _gc->isRealized(); }

        osg::ref_ptr<osg::GraphicsContext> _gc;
    };
}

// ---------------------------------------------------------------------------

#define SAYBOOL(X) (X?"yes":"no")

const Capabilities&
Capabilities::get()
{
    auto registry = osgEarth::Registry::instance();
    OE_HARD_ASSERT(registry != nullptr);
    return registry->capabilities();
}

// Constructor - default are typical settings that we will use
// in the even
Capabilities::Capabilities() :
    _maxGPUTextureUnits(32),
    _maxTextureSize(16384),
    _maxFastTextureSize(4096),
    _supportsGLSL(true),
    _GLSLversion(3.3f),
    _supportsDepthPackedStencilBuffer(true),
    _supportsDrawInstanced(true),
    // Attribute instancing is unsafe until a live context confirms both the
    // advertised feature and osg's glVertexAttribDivisor entry point.
    _supportsInstancedArrays(false),
    _supportsNonPowerOfTwoTextures(true),
    _numProcessors(4),
    _supportsFragDepthWrite(true),
    _supportsS3TC(true),
    _supportsPVRTC(false),
    _supportsARBTC(false),
    _supportsETC(false),
    _supportsRGTC(false),
    _isGLES(false),
    _maxTextureBufferSize(INT_MAX),
    _isCoreProfile(false),
    _supportsVertexArrayObjects(true),
    _supportsInt64(false),
    _supportsNVGL(false),
    _vendor("Unknown"),
    _renderer("Unknown"),
    _version("3.30")
{
    // little hack to force the osgViewer library to link so we can create a graphics context
    osgViewerGetVersion();

    // check the environment in order to disable ATI workarounds
    bool enableATIworkarounds = true;
    if ( ::getenv( "OSGEARTH_DISABLE_ATI_WORKAROUNDS" ) != 0L )
        enableATIworkarounds = false;

    // logical CPUs (cores)
    _numProcessors = std::thread::hardware_concurrency();
    if (_numProcessors <= 0)
        _numProcessors = 4;

    // GLES compile?
#if (defined(OSG_GLES1_AVAILABLE) || defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE))
    _isGLES = true;
#else
    _isGLES = false;
#endif

    bool isAndroid = false;
#ifdef __ANDROID__
    isAndroid = true;
#endif

    // Simulate headless for testing
    bool isHeadless = (::getenv("OSGEARTH_HEADLESS") != nullptr);

    // create a graphics context so we can query OpenGL support:
    std::shared_ptr<MyGraphicsContext> myGC;
    osg::GraphicsContext* gc = nullptr;
    unsigned int id = 0;

    if (isHeadless)
    {
        OE_INFO << LC << "Simulating headless mode" << std::endl;
    }
    else if (isAndroid)
    {
        OE_DEBUG << LC << "Using defaults for Android" << std::endl;
    }
    else
    {
        myGC = std::make_shared<MyGraphicsContext>();
    }

    if (myGC && myGC->valid())
    {
        gc = myGC->_gc.get();
        id = gc->getState()->getContextID();
    }

    if (gc != nullptr)
    {
        const osg::GL2Extensions* GL2 = osg::GL2Extensions::Get( id, true );

        OE_INFO << LC << "osgEarth Version:  " << osgEarthGetVersion() << std::endl;

#ifdef GDAL_RELEASE_NAME
        OE_INFO << LC << "GDAL Version:      " << GDAL_RELEASE_NAME << std::endl;
#endif

        OE_INFO << LC << "OSG Version:       " << osgGetVersion() << std::endl;

#if OSG_GL3_FEATURES
        OE_INFO << LC << "OSG GL3 Features:  yes" << std::endl;
#else
        OE_INFO << LC << "OSG GL3 Features:  no" << std::endl;
#endif
#ifdef OSG_GL_FIXED_FUNCTION_AVAILABLE
        OE_INFO << LC << "OSG FFP Available: yes" << std::endl;
#else
        OE_INFO << LC << "OSG FFP Available: no" << std::endl;
#endif

        OE_INFO << LC << "CPU Cores:         " << _numProcessors << std::endl;

        _supportsGLSL = GL2->isGlslSupported;
        _GLSLversion = GL2->glslLanguageVersion;

        _vendor = std::string(reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
        OE_INFO << LC << "GL_VENDOR:         " << _vendor << std::endl;

        _renderer = std::string( reinterpret_cast<const char*>(glGetString(GL_RENDERER)) );
        OE_INFO << LC << "GL_RENDERER:       " << _renderer << std::endl;

        _version = std::string( reinterpret_cast<const char*>(glGetString(GL_VERSION)) );
        OE_INFO << LC << "GL_VERSION:        " << _version << std::endl;

        // Detect core profile by investigating GL_CONTEXT_PROFILE_MASK
        if ( GL2->glVersion < 3.2f )
        {
            _isCoreProfile = false;
        }
        else
        {
            GLint profileMask = 0;
            glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profileMask);
            _isCoreProfile = ((profileMask & GL_CONTEXT_CORE_PROFILE_BIT) != 0);
        }
        OE_INFO << LC << "GL CORE Profile:   " << SAYBOOL(_isCoreProfile) << std::endl;

        // this extension implies the availability of
        // GL_NV_vertex_buffer_unified_memory (bindless buffers)
        _supportsNVGL =
            GL2->glVersion >= 4.4f &&
            osg::isGLExtensionSupported(id, "GL_NV_vertex_buffer_unified_memory") &&
            osg::isGLExtensionSupported(id, "GL_NV_shader_buffer_load") &&
            osg::isGLExtensionSupported(id, "GL_NV_bindless_multi_draw_indirect");

        glGetIntegerv( GL_MAX_TEXTURE_IMAGE_UNITS_ARB, &_maxGPUTextureUnits );
        OE_DEBUG << LC << "Max GPU texture units = " << _maxGPUTextureUnits << std::endl;

        GLint mvoc;
        glGetIntegerv(GL_MAX_VERTEX_VARYING_COMPONENTS_EXT, &mvoc);
        OE_DEBUG << LC << "Max varyings = " << mvoc << std::endl;

        glGetIntegerv( GL_MAX_TEXTURE_SIZE, &_maxTextureSize );
#if !defined(OSG_GLES1_AVAILABLE) && !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE)
        // Use the texture-proxy method to determine the maximum texture size
        for( int s = _maxTextureSize; s > 2; s >>= 1 )
        {
            glTexImage2D( GL_PROXY_TEXTURE_2D, 0, GL_RGBA8, s, s, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0L );
            GLint width = 0;
            glGetTexLevelParameteriv( GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width );
            if ( width == s )
            {
                _maxTextureSize = s;
                break;
            }
        }
#endif
        OE_DEBUG << LC << "Max texture size = " << _maxTextureSize << std::endl;

        if ( _supportsGLSL )
        {
            OE_DEBUG << LC << "GLSL Version = " << getGLSLVersionInt() << std::endl;
        }

        _supportsDepthPackedStencilBuffer = osg::isGLExtensionSupported( id, "GL_EXT_packed_depth_stencil" ) ||
                                            osg::isGLExtensionSupported( id, "GL_OES_packed_depth_stencil" );

        _supportsDrawInstanced =
            _supportsGLSL &&
            osg::isGLExtensionOrVersionSupported( id, "GL_EXT_draw_instanced", 3.1f );
        OE_DEBUG << LC << "draw instanced = " << SAYBOOL(_supportsDrawInstanced) << std::endl;

#if defined(OSG_GLES3_AVAILABLE)
        const float instancedArraysCoreVersion = 3.0f;
#else
        const float instancedArraysCoreVersion = 3.3f;
#endif
        const osg::GLExtensions* GL = osg::GLExtensions::Get(id, true);
        const bool instancedArraysAdvertised =
            osg::isGLExtensionOrVersionSupported(
                id,
                "GL_ARB_instanced_arrays",
                instancedArraysCoreVersion) ||
            osg::isGLExtensionSupported(id, "GL_EXT_instanced_arrays") ||
            osg::isGLExtensionSupported(id, "GL_ANGLE_instanced_arrays");
        _supportsInstancedArrays =
            _supportsGLSL &&
            instancedArraysAdvertised &&
            GL != nullptr &&
            GL->glVertexAttribDivisor != nullptr;
        OE_DEBUG << LC << "instanced arrays = "
                 << SAYBOOL(_supportsInstancedArrays) << std::endl;

#if !defined(OSG_GLES3_AVAILABLE)
        _supportsNonPowerOfTwoTextures =
            osg::isGLExtensionSupported( id, "GL_ARB_texture_non_power_of_two" );
#else
        _supportsNonPowerOfTwoTextures = true;
#endif
        //OE_INFO << LC << "  NPOT textures = " << SAYBOOL(_supportsNonPowerOfTwoTextures) << std::endl;

        // Writing to gl_FragDepth is not supported under GLES, is supported under gles3
#if (defined(OSG_GLES1_AVAILABLE) || defined(OSG_GLES2_AVAILABLE))
        _supportsFragDepthWrite = false;
#else
        _supportsFragDepthWrite = true;
#endif

        glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &_maxTextureBufferSize);

        // NVIDIA:
        bool isNVIDIA = _vendor.find("NVIDIA") == 0;

        // ATI workarounds:
        bool isATI = _vendor.find("ATI ") == 0;

        _maxFastTextureSize = _maxTextureSize;

        //OE_INFO << LC << "  Max Fast Texture Size = " << _maxFastTextureSize << std::endl;

        // tetxure compression
        std::stringstream buf;
        buf << "Compression = ";
        _supportsARBTC = osg::isGLExtensionSupported( id, "GL_ARB_texture_compression" );
        if (_supportsARBTC) buf << "ARB ";

        _supportsS3TC = osg::isGLExtensionSupported( id, "GL_EXT_texture_compression_s3tc" );
        if ( _supportsS3TC ) buf << "S3 ";

        _supportsPVRTC = osg::isGLExtensionSupported( id, "GL_IMG_texture_compression_pvrtc" );
        if ( _supportsPVRTC ) buf << "PVR ";

        _supportsETC = osg::isGLExtensionSupported( id, "GL_OES_compressed_ETC1_RGB8_texture" );
        if ( _supportsETC ) buf << "ETC1 ";

        _supportsRGTC = osg::isGLExtensionSupported( id, "GL_EXT_texture_compression_rgtc" );
        if ( _supportsRGTC ) buf << "RG";

        OE_DEBUG << LC << buf.str() << std::endl;

        _supportsVertexArrayObjects = osg::isGLExtensionOrVersionSupported(id, "GL_ARB_vertex_array_object", 3.0);

        _supportsInt64 = osg::isGLExtensionSupported(id, "GL_ARB_gpu_shader_int64");
    }
    else
    {
        OE_INFO << LC << "No graphics context - falling back on GL 3.3 defaults" << std::endl;
    }
}

bool
Capabilities::supportsTextureCompression(const osg::Texture::InternalFormatMode& mode) const
{
    switch( mode )
    {
    case osg::Texture::USE_ARB_COMPRESSION:
        return _supportsARBTC;
        break;

    case osg::Texture::USE_S3TC_DXT1a_COMPRESSION:
    case osg::Texture::USE_S3TC_DXT1c_COMPRESSION:
    case osg::Texture::USE_S3TC_DXT1_COMPRESSION:
    case osg::Texture::USE_S3TC_DXT3_COMPRESSION:
    case osg::Texture::USE_S3TC_DXT5_COMPRESSION:
        return _supportsS3TC;
        break;

    case osg::Texture::USE_PVRTC_2BPP_COMPRESSION:
    case osg::Texture::USE_PVRTC_4BPP_COMPRESSION:
        return _supportsPVRTC;
        break;

    case osg::Texture::USE_ETC_COMPRESSION:
        return _supportsETC;
        break;

    case osg::Texture::USE_RGTC1_COMPRESSION:
    case osg::Texture::USE_RGTC2_COMPRESSION:
        return _supportsRGTC;
        break;

    default:
        return false;
    }

    return false;
}
