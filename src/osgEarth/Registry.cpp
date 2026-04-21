/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "Registry"
#include "Capabilities"
#include "ShaderFactory"
#include "ObjectIndex"
#include "HTTPClient"
#include "TerrainEngineNode"
#include "GLUtils"
#include "Chonk"
#include "MemoryUtils"
#include "ScriptEngine"

#include <osg/ArgumentParser>
#include <osgText/Font>
#include <osgDB/Registry>

#include <gdal.h>
#include <cpl_conv.h>
#include <cstdlib>
#include <mutex>

#include <osgEarth/BuildConfig>

#ifdef OSGEARTH_HAVE_AWS_SDK_CORE
#include <aws/core/Aws.h>
#endif

using namespace osgEarth;

#define LC "[Registry] "

// define the threading library singleton
WEEJOBS_INSTANCE;

void osgEarth::initialize()
{
    // Create the registry singleton.
    Registry::instance()->getCapabilities();
    
    // Tell the weetjobs library how to set a thread name
    jobs::set_thread_name_function([](const char* value) {
        osgEarth::setThreadName(value);
    });
}

void osgEarth::initialize(osg::ArgumentParser& args)
{
    osg::DisplaySettings::instance()->readCommandLine(args);
    initialize();
}

osgEarth::UID
osgEarth::createUID()
{
    static std::atomic_int s_uidGen(0);
    return s_uidGen++;
}

// private bytes usage at startup.
std::int64_t osgEarth::g_startupPrivateBytes =
    osgEarth::Util::Memory::getProcessPrivateUsage();

namespace
{
    bool verbose_gdal_errors = false;

    void CPL_STDCALL myCPLErrorHandler(CPLErr errClass, int errNum, const char* msg)
    {
        static std::once_flag flag;
        std::call_once(flag, [&]() {
            verbose_gdal_errors = ::getenv("OSGEARTH_VERBOSE_GDAL_ERRORS") != nullptr;
        });

        if (errClass == CE_Fatal)
        {
            OE_WARN << "GDAL fatal error: " << msg << " (error " << errNum << ")" << std::endl;
        }
        else if (verbose_gdal_errors)
        {
            OE_NOTICE << "GDAL says: " << msg << " (error " << errNum << ")" << std::endl;
        }
        else
        {
            OE_DEBUG << "GDAL says: " << msg << " (error " << errNum << ")" << std::endl;
        }
    }
}

Registry::Registry() :
    _terrainEngineDriver("rex"),
    _cacheDriver("filesystem"),
    _scriptEngineDriver("qjs"),
    _overrideCachePolicyInitialized(false),
    _maxVertsPerDrawable(UINT_MAX),
    _maxImageDimension(INT_MAX)
{
    OE_INFO << "Hello, world." << std::endl;

    // set up GDAL and OGR.
    OGRRegisterAll();
    GDALAllRegister();

#ifdef OSGEARTH_HAVE_AWS_SDK_CORE
    Aws::SDKOptions options;
    Aws::InitAPI(options);
#endif

#ifdef OSG_USE_UTF8_FILENAME
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8","YES");
#else
    // support Chinese character in the file name and attributes in ESRI's shapefile
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8","NO");
#endif
    CPLSetConfigOption("SHAPE_ENCODING","");

#if GDAL_VERSION_MAJOR>=3
    CPLSetConfigOption("OGR_CT_FORCE_TRADITIONAL_GIS_ORDER", "YES");
#endif

    // Redirect GDAL/OGR console errors to our own handler
    CPLSetErrorHandler(myCPLErrorHandler);

    // Set the GDAL shared block cache size. This defaults to 5% of
    // available memory which is too high.
    GDALSetCacheMax(40 * 1024 * 1024);

    // global initialization for CURL (not thread safe)
    HTTPClient::globalInit();

    // GL debugging environment variables
    if (::getenv("OSGEARTH_GL_DEBUG"))
    {
        GLUtils::enableGLDebugging();
    }

    if (::getenv("OSGEARTH_VP_DEBUG"))
    {
        GLUtils::enableGLDebugging();
        VirtualProgram::enableGLDebugging();
    }

    if (::getenv("OSGEARTH_USE_NVGL") || ::getenv("OSGEARTH_USE_GL4"))
    {
        GLUtils::useNVGL(true);
    }

    // warn if GDAL_DATA is not set
    if (::getenv("GDAL_DATA") == NULL)
    {
        OE_INFO << LC << "Note: GDAL_DATA environment variable is not set" << std::endl;
    }

    // shader generator used internally by osgEarth. Can be replaced.
    _shaderGen = new ShaderGenerator();

    // optimizes sharing of state attributes and state sets for
    // performance boost
    _stateSetCache = new StateSetCache();

    // Default unref-after apply policy:
    _unRefImageDataAfterApply = false;

    if (::getenv("OSGEARTH_DISABLE_UNREF_AFTER_APPLY"))
    {
        _unRefImageDataAfterApply = false;
    }

    // activate KMZ support
    osgDB::Registry::instance()->addArchiveExtension( "kmz" );
    osgDB::Registry::instance()->addArchiveExtension( "3tz");
    osgDB::Registry::instance()->addFileExtensionAlias( "3tz", "zip" );
    osgDB::Registry::instance()->addFileExtensionAlias("glb", "gltf");
    osgDB::Registry::instance()->addFileExtensionAlias("b3dm", "gltf");
    osgDB::Registry::instance()->addMimeTypeExtensionMapping( "application/vnd.google-earth.kml+xml", "kml" );
    osgDB::Registry::instance()->addMimeTypeExtensionMapping( "application/vnd.google-earth.kml+xml; charset=utf8", "kml");
    osgDB::Registry::instance()->addMimeTypeExtensionMapping( "application/vnd.google-earth.kmz",     "kmz" );
    osgDB::Registry::instance()->addMimeTypeExtensionMapping( "text/plain",                           "osgb" );
    osgDB::Registry::instance()->addMimeTypeExtensionMapping( "text/xml",                             "osgb" );
    osgDB::Registry::instance()->addMimeTypeExtensionMapping( "application/json",                     "osgb" );
    osgDB::Registry::instance()->addMimeTypeExtensionMapping( "text/json",                            "osgb" );
    osgDB::Registry::instance()->addMimeTypeExtensionMapping( "text/x-json",                          "osgb" );
    osgDB::Registry::instance()->addMimeTypeExtensionMapping( "image/jpg",                            "jpg" );
    osgDB::Registry::instance()->addMimeTypeExtensionMapping( "image/dds",                            "dds" );
    // This is not correct, but some versions of readymap can return tif with one f instead of two.
    osgDB::Registry::instance()->addMimeTypeExtensionMapping( "image/tif",                            "tif" );
    osgDB::Registry::instance()->addMimeTypeExtensionMapping( "image/webp", "webp");

    // pre-load OSG's ZIP plugin so that we can use it in URIs
    std::string zipLib = osgDB::Registry::instance()->createLibraryNameForExtension( "zip" );
    if ( !zipLib.empty() )
        osgDB::Registry::instance()->loadLibrary( zipLib );

    // pre-load KML/KMZ plugin so that we can use it in URIs
    std::string kmzLib = osgDB::Registry::instance()->createLibraryNameForExtension("kml");
    if (!kmzLib.empty())
        osgDB::Registry::instance()->loadLibrary(kmzLib);

    _defaultOptions = new osgDB::Options();

    const char* teStr = ::getenv(OSGEARTH_ENV_TERRAIN_ENGINE_DRIVER);
    if ( teStr )
    {
        _terrainEngineDriver = std::string(teStr);
        _overrideTerrainEngineDriverName = std::string(teStr);
        OE_INFO << LC << "Terrain engine set from environment: " << _terrainEngineDriver << std::endl;
    }

    // load a default font
    const char* envFont = ::getenv("OSGEARTH_DEFAULT_FONT");
    if ( envFont )
    {
        _defaultFont = osgText::readRefFontFile( std::string(envFont) );
        OE_INFO << LC << "Default font set from environment: " << envFont << std::endl;
    }
    if ( !_defaultFont.valid() )
    {
#ifdef _WIN32
        _defaultFont = osgText::readRefFontFile("arial.ttf");
#endif

        if (!_defaultFont.valid())
        {
            _defaultFont = osgText::Font::getDefaultFont();
        }
    }

    const char* maxVerts = getenv("OSGEARTH_MAX_VERTS_PER_DRAWABLE");
    if (maxVerts)
    {
        _maxVertsPerDrawable = std::atoi(maxVerts);

        if (_maxVertsPerDrawable < 1024)
            _maxVertsPerDrawable = 65536;
    }

    // disable work stealing in the jobs system?
    if (getenv("OSGEARTH_ENABLE_WORK_STEALING"))
    {
        jobs::set_allow_work_stealing(true);
    }

    // register the system stock Units.
    //Units::registerAll( this );

    // register the chonk bin with OSG
    osgUtil::RenderBin::addRenderBinPrototype(
        "ChonkBin",
        new ChonkRenderBin());

    const char* maxDim = getenv("OSGEARTH_MAX_TEXTURE_SIZE");
    if (maxDim)
    {
        _maxImageDimension = as<unsigned>(maxDim, UINT_MAX);
        OE_INFO << LC << "Setting max texture size from environment = " << _maxImageDimension << std::endl;
    }

    // set a script engine to use from an env var.
    const char* scriptEngine = ::getenv(OSGEARTH_ENV_SCRIPT_ENGINE);
    if (scriptEngine)
    {
        _scriptEngineDriver = scriptEngine;
        OE_INFO << LC << "Script engine set from environment: " << _scriptEngineDriver << std::endl;
    }
}

Registry::~Registry()
{
    OE_DEBUG << LC << "Registry destructing" << std::endl;


    // A heavy hammer, but at this stage, which is usually application
    // shutdown, various osgEarth objects (e.g., VirtualPrograms) are
    // in the OSG cache and will cause a crash when they are deleted later.
    osgDB::Registry::instance()->clearObjectCache();

    // pop the custom error handler
    //CPLPopErrorHandler();

    // Release any GL objects
    release();

    OE_INFO << "Goodbye." << std::endl;
}

// Destroy the registry explicitly: this is called in an atexit() hook.  See comment in
// Registry::instance(bool reset).
namespace
{
    static bool g_registry_created = false;
    static bool g_registry_destroyed = false;

    static std::once_flag g_registry_once;
    static Registry* g_registry = nullptr;

    void destroyRegistry()
    {
        if (g_registry)
        {
            g_registry->release();
            delete g_registry;
            g_registry = nullptr;
            g_registry_destroyed = true;
        }
    }
}

Registry*
Registry::instance()
{
    std::call_once(g_registry_once, []() 
        {
            g_registry = new Registry();
            g_registry_created = true;
            g_registry_destroyed = false;
            std::atexit(destroyRegistry);
        });

    if (g_registry_destroyed)
    {
        return nullptr;
    }

    if (g_registry_created == true && g_registry == nullptr)
    {
        OE_HARD_ASSERT(false, "Registry::instance() called recursively. Contact support.");
    }

    return g_registry;
}

void
Registry::releaseGLObjects(osg::State* state) const
{
    // Clear out the state set cache
    if (_stateSetCache.valid())
    {
        _stateSetCache->releaseGLObjects(state);
    }

    // Clear out the VirtualProgram shared program repository
    _programRepo.lock();
    _programRepo.releaseGLObjects(state);
    _programRepo.unlock();
}

void
Registry::release()
{
    // shut down running jobs:
    WEEJOBS_NAMESPACE::instance().shutdown();

    // GL resources (all GCs):
    releaseGLObjects(NULL);

    // Clear out the state set cache
    if (_stateSetCache.valid())
    {
        _stateSetCache->clear();
    }

    // SpatialReference cache
    _srsCache.clear();

    // Shared object index
    _objectIndex = nullptr;

    // Dereference all registered singletons
    _singletons.clear();
    _namedSingletons.clear();
}

osg::ref_ptr<SpatialReference>
Registry::getOrCreateSRS(const SpatialReference::Key& key)
{
    std::lock_guard<std::mutex> lock(_srsCache.mutex());
    osg::ref_ptr<SpatialReference>& srs = _srsCache[key];
    if (!srs.valid())
    {
        srs = SpatialReference::createFromKey(key);
    }
    return srs;
}

void
Registry::setDefaultCachePolicy( const CachePolicy& value )
{
    _defaultCachePolicy = value;
}

void
Registry::setOverrideCachePolicy( const CachePolicy& value )
{
    _overrideCachePolicy = value;
}

bool
Registry::resolveCachePolicy(optional<CachePolicy>& cp) const
{
    optional<CachePolicy> new_cp;

    // start with the defaults
    if ( defaultCachePolicy().isSet() )
        new_cp = defaultCachePolicy();

    // merge in any set properties from the caller's CP, since they override
    // the defaults:
    if ( cp.isSet() )
        new_cp.mutable_value().mergeAndOverride( cp );

    // finally, merge in any set props from the OVERRIDE CP, which take
    // priority over everything else.
    if ( overrideCachePolicy().isSet() )
        new_cp.mutable_value().mergeAndOverride( overrideCachePolicy() );

    // return the new composited cache policy.
    cp = new_cp;
    return cp.isSet();
}

const std::string&
Registry::getDefaultCacheDriverName() const
{
    static std::once_flag s_once;

    std::call_once(s_once, [&]() {
        if (!_cacheDriver.isSet())
        {
            // see if the environment specifies a default caching driver.
            const char* value = ::getenv(OSGEARTH_ENV_CACHE_DRIVER);
            if (value)
            {
                _cacheDriver = value;
                OE_INFO << LC << "Cache driver set from environment: " << value << std::endl;
            }
        }
    });

    return _cacheDriver.get();
}

const optional<CachePolicy>&
Registry::defaultCachePolicy() const
{
    return _defaultCachePolicy;
}

const optional<CachePolicy>&
Registry::overrideCachePolicy() const
{
    static std::once_flag s_once;

    std::call_once(s_once, [&]()
        {
            if (!_overrideCachePolicyInitialized)
            {
                // activate no-cache mode from the environment
                if (::getenv(OSGEARTH_ENV_NO_CACHE))
                {
                    _overrideCachePolicy = CachePolicy::NO_CACHE;
                    OE_INFO << LC << "NO-CACHE MODE set from environment" << std::endl;
                }
                else
                {
                    // activate cache-only mode from the environment
                    if (::getenv(OSGEARTH_ENV_CACHE_ONLY))
                    {
                        _overrideCachePolicy.mutable_value().usage() = CachePolicy::USAGE_CACHE_ONLY;
                        OE_INFO << LC << "CACHE-ONLY MODE set from environment" << std::endl;
                    }

                    // cache max age?
                    const char* cacheMaxAge = ::getenv(OSGEARTH_ENV_CACHE_MAX_AGE);
                    if (cacheMaxAge)
                    {
                        TimeSpan maxAge = osgEarth::Strings::as<long>(std::string(cacheMaxAge), INT_MAX);
                        _overrideCachePolicy.mutable_value().maxAge() = maxAge;
                        OE_INFO << LC << "Cache max age set from environment: " << cacheMaxAge << std::endl;
                    }
                }

                _overrideCachePolicyInitialized = true;
            }
        });                    

    return _overrideCachePolicy;
}

osgEarth::Cache*
Registry::getDefaultCache() const
{
    static std::once_flag s_once;

    std::call_once(s_once, [&]()
        {
            if (!_defaultCache.valid())
            {
                std::string driverName = getDefaultCacheDriverName();

                const char* noCache = ::getenv(OSGEARTH_ENV_NO_CACHE);
                if (noCache == nullptr)
                {
                    // see if there's a cache in the envvar; if so, create a cache.
                    // Note: the value of the OSGEARTH_CACHE_PATH is not used here; rather
                    // it's used in the driver(s) itself.
                    const char* cachePath = ::getenv(OSGEARTH_ENV_CACHE_PATH);
                    if (cachePath && !driverName.empty())
                    {
                        CacheOptions cacheOptions;
                        cacheOptions.setDriver(driverName);
                        _defaultCache = CacheFactory::create(cacheOptions);
                        if (_defaultCache.valid() && _defaultCache->getStatus().isError())
                        {
                            OE_WARN << LC << "Cache error: " << _defaultCache->getStatus().toString() << std::endl;
                        }
                    }
                }
            }
        });

    return _defaultCache.get();
}

void
Registry::setDefaultCache(Cache* cache)
{
    _defaultCache = cache;
}

bool
Registry::isBlacklisted(const std::string& filename)
{
    std::lock_guard<std::mutex> sharedLock(_blacklist.mutex());
    return _blacklist.find(filename) != _blacklist.end();
}

void
Registry::blacklist(const std::string& filename)
{
    _blacklist.lock();
    _blacklist.insert(filename);
    _blacklist.unlock();
}

void
Registry::clearBlacklist()
{
    _blacklist.lock();
    _blacklist.clear();
    _blacklist.unlock();
}

unsigned int
Registry::getNumBlacklistedFilenames()
{
    std::lock_guard<std::mutex> sharedLock(_blacklist.mutex());
    return _blacklist.size();
}

bool
Registry::hasCapabilities() const
{
    return _caps.valid();
}

const Capabilities&
Registry::getCapabilities() const
{
    if ( !_caps.valid() )
        const_cast<Registry*>(this)->initCapabilities();

    return *_caps;
}

void
Registry::setCapabilities( Capabilities* caps )
{
    _caps = caps;
}

void
Registry::initCapabilities()
{
    static std::once_flag s_once;
    std::call_once(s_once, [&]() {
        _caps = new Capabilities();
    });
}

ShaderFactory*
Registry::getShaderFactory() const
{
    static std::once_flag s_once;

    std::call_once(s_once, [&]()
        {
            if (!_shaderLib.valid())
            {
                const_cast<Registry*>(this)->_shaderLib = new ShaderFactory();
            }
        });

    return _shaderLib.get();
}

void
Registry::setShaderFactory( ShaderFactory* lib )
{
    if ( lib != 0L && lib != _shaderLib.get() )
        _shaderLib = lib;
}

ShaderGeneratorProxy
Registry::getShaderGenerator() const
{
    return ShaderGeneratorProxy(_shaderGen.get());
}

void
Registry::setShaderGenerator(ShaderGenerator* shaderGen)
{
    if ( shaderGen != 0L && shaderGen != _shaderGen.get() )
        _shaderGen = shaderGen;
}

void
Registry::setURIReadCallback( URIReadCallback* callback )
{
    _uriReadCallback = callback;
}

URIReadCallback*
Registry::getURIReadCallback() const
{
    return _uriReadCallback.get();
}

void
Registry::setDefaultFont( osgText::Font* font )
{
    std::lock_guard<std::mutex> exclusive(_regMutex);
    _defaultFont = font;
}

osgText::Font*
Registry::getDefaultFont()
{
    std::lock_guard<std::mutex> shared(_regMutex);
    return _defaultFont.get();
}

const osgDB::Options*
Registry::getDefaultOptions() const
{
    return _defaultOptions.get();
}

osgDB::Options*
Registry::cloneOrCreateOptions(const osgDB::Options* input)
{
    osgDB::Options* newOptions =
        input ? static_cast<osgDB::Options*>(input->clone(osg::CopyOp::DEEP_COPY_USERDATA)) :
        new osgDB::Options();

    return newOptions;
}

void
Registry::registerUnits(const UnitsType& prototype)
{
    _unitsVector.push_back(prototype);
}

UnitsType
Registry::getUnits(const std::string& name) const
{
    static std::once_flag s_once;
    std::call_once(s_once, []() { Units::registerAll(Registry::instance()); });

    std::lock_guard<std::mutex> lock(_regMutex);
    for (auto& units : _unitsVector)
    {
        if (ci_equals(name, units.getName()) || ci_equals(name, units.getAbbr()))
        {
            return units;
        }
    }
    return { };
}

void
Registry::setDefaultCacheDriverName(const std::string& name)
{
    _cacheDriver = name;
}

void
Registry::setStateSetCache( StateSetCache* cache )
{
    _stateSetCache = cache;
}

StateSetCache*
Registry::getStateSetCache() const
{
    return _stateSetCache.get();
}

ProgramRepo&
Registry::getProgramRepo()
{
    return _programRepo;
}

ObjectIndex*
Registry::getObjectIndex() const
{
    if (!_objectIndex.valid())
    {
        std::lock_guard<std::mutex> lock(_regMutex);
        if (!_objectIndex.valid())
        {
            _objectIndex = new ObjectIndex();
        }
    }
    return _objectIndex.get();
}

void
Registry::startActivity(const std::string& activity)
{
    std::lock_guard<std::mutex> lock(_activityMutex);
    _activities.insert(Activity(activity,std::string()));
}

void
Registry::startActivity(const std::string& activity,
    const std::string& value)
{
    std::lock_guard<std::mutex> lock(_activityMutex);
    _activities.erase(Activity(activity,std::string()));
    _activities.insert(Activity(activity,value));
}

void
Registry::endActivity(const std::string& activity)
{
    std::lock_guard<std::mutex> lock(_activityMutex);
    _activities.erase(Activity(activity,std::string()));
}

void
Registry::getActivities(std::set<std::string>& output)
{
    std::lock_guard<std::mutex> lock(_activityMutex);
    for(std::set<Activity,ActivityLess>::const_iterator i = _activities.begin();
        i != _activities.end();
        ++i)
    {
        if ( ! i->second.empty() )
            output.insert( i->first + ": " + i->second );
        else
            output.insert( i->first );
    }
}

std::string
Registry::getExtensionForMimeType(const std::string& mt)
{
    std::string mt_lower = osgEarth::toLower(mt);

    const osgDB::Registry::MimeTypeExtensionMap& exmap = osgDB::Registry::instance()->getMimeTypeExtensionMap();
    for( osgDB::Registry::MimeTypeExtensionMap::const_iterator i = exmap.begin(); i != exmap.end(); ++i )
    {
        if ( i->first == mt_lower )
        {
            return i->first;
        }
    }
    return std::string();
}

std::string
Registry::getMimeTypeForExtension(const std::string& ext)
{
    std::string ext_lower = osgEarth::toLower(ext);

    const osgDB::Registry::MimeTypeExtensionMap& exmap = osgDB::Registry::instance()->getMimeTypeExtensionMap();
    for( osgDB::Registry::MimeTypeExtensionMap::const_iterator i = exmap.begin(); i != exmap.end(); ++i )
    {
        if ( i->second == ext_lower )
        {
            return i->first;
        }
    }
    return std::string();
}

void
Registry::setTextureImageUnitOffLimits(int unit)
{
    std::lock_guard<std::mutex> exclusive(_regMutex);
    _offLimitsTextureImageUnits.insert(unit);
}

const std::set<int>
Registry::getOffLimitsTextureImageUnits() const
{
    std::lock_guard<std::mutex> exclusive(_regMutex);
    return _offLimitsTextureImageUnits;
}

float
Registry::getDevicePixelRatio(osg::GraphicsContext* gc) const
{
    return _devicePixelRatioFunc ? _devicePixelRatioFunc(gc) : 1.0f;
}

void
Registry::setDevicePixelRatioFunction(std::function<float(osg::GraphicsContext*)>&& func)
{
    _devicePixelRatioFunc = std::move(func);
}

void
Registry::setMaxNumberOfVertsPerDrawable(unsigned value)
{
    _maxVertsPerDrawable = value;
}

unsigned
Registry::getMaxNumberOfVertsPerDrawable() const
{
    return _maxVertsPerDrawable;
}

int
Registry::getMaxTextureSize() const
{
    return _maxImageDimension;
}

void
Registry::setMaxTextureSize(int value)
{
    _maxImageDimension = value;
}

void
Registry::setDateTime(const DateTime& value)
{
    _dateTime = value;
    onDateTimeChanged.fire(_dateTime);
}

DateTime
Registry::getDateTime() const
{
    return _dateTime;
}