/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/Layer>
#include <osgEarth/Cache>
#include <osgEarth/Registry>
#include <osgEarth/SceneGraphCallback>
#include <osgEarth/TileKey>
#include <osgEarth/TerrainEngineNode>
#include <osgEarth/NetworkMonitor>
#include <osg/StateSet>
#include <osgDB/Registry>

using namespace osgEarth;

#define LC "[" << className() << "] " << getName() << "\" "

//.................................................................

Config
Layer::Options::getConfig() const
{
    Config conf = ConfigOptions::getConfig();
    conf.set("name", name());
    //conf.set("enabled", enabled());
    conf.set("open", openAutomatically());
    conf.set("enabled", openAutomatically()); // back compat
    conf.set("cacheid", cacheId());
    if (cachePolicy().isSet() && !cachePolicy()->empty())
        conf.set("cache_policy", cachePolicy());
    conf.set("shader_define", shaderDefine());
    conf.set("attribution", attribution());
    conf.set("terrain", terrainPatch());
    conf.set("proxy", _proxySettings );
    conf.set("read_options", osgOptionString());
    conf.set("l2_cache_size", l2CacheSize());

    conf.remove("shader");
    for(auto& shader : shaders())
    {
        conf.add("shader", shader.getConfig());
    }

    return conf;
}

void
Layer::Options::fromConfig(const Config& conf)
{
    conf.get("name", name());
    conf.get("open", openAutomatically()); // back compat
    conf.get("enabled", openAutomatically());
    conf.get("cache_id", cacheId()); // compat
    conf.get("cacheid", cacheId());
    conf.get("attribution", attribution());
    conf.get("cache_policy", cachePolicy());
    conf.get("l2_cache_size", l2CacheSize());

    // legacy support:
    if (!cachePolicy().isSet())
    {
        if ( conf.value<bool>( "cache_only", false ) == true )
            _cachePolicy.mutable_value().usage() = CachePolicy::USAGE_CACHE_ONLY;
        if ( conf.value<bool>( "cache_enabled", true ) == false )
            _cachePolicy.mutable_value().usage() = CachePolicy::USAGE_NO_CACHE;
        if (conf.value<bool>("caching", true) == false)
            _cachePolicy.mutable_value().usage() = CachePolicy::USAGE_NO_CACHE;
    }
    conf.get("shader_define", shaderDefine());

    const ConfigSet& shadersConf = conf.children("shader");
    for (auto& shaderConf : shadersConf)
        shaders().push_back(ShaderOptions(shaderConf));

    conf.get("terrain", terrainPatch());
    conf.get("patch", terrainPatch());
    conf.get("proxy", _proxySettings );
    conf.get("read_options", osgOptionString());
    conf.get("osg_options", osgOptionString()); // back compat
}

//.................................................................

void
Layer::TraversalCallback::traverse(osg::Node* node, osg::NodeVisitor* nv) const
{
    node->accept(*nv);
}

//.................................................................

Layer::Layer() :
    _options(&_optionsConcrete),
    _layerName(osg::Object::_name) // for the debugger
{
    init();
}

Layer::Layer(Layer::Options* optionsPtr, const Layer::Options* optionsPtr0) :
    _options(optionsPtr ? optionsPtr : &_optionsConcrete),
    _options0(optionsPtr0 ? optionsPtr0 : &_optionsConcrete0),
    _layerName(osg::Object::_name) // for the debugger
{
    // init() will be called by base class
}

Layer::Layer(const Layer& rhs, const osg::CopyOp& op) :
    osg::Object(rhs, op),
    _layerName(osg::Object::_name)
{
    //nop
}

Layer::~Layer()
{
    //nop
}

void
Layer::dirty()
{
    bumpRevision();
}

void
Layer::bumpRevision()
{
    ++_revision;
}

void
Layer::setReadOptions(const osgDB::Options* readOptions)
{
    // We are storing _cacheSettings both in the Read Options AND
    // as a class member. This is probably not strictly necessary
    // but we will keep the ref in the Layer just to be on the safe
    // side - gw

    _readOptions = Registry::cloneOrCreateOptions(readOptions);

    // store the referrer for relative-path resolution
    URIContext(options().referrer()).store(_readOptions.get());

    //Store the proxy settings in the options structure.
    if (options().proxySettings().isSet())
    {
        options().proxySettings()->apply(_readOptions.get());
    }

    if (options().osgOptionString().isSet())
    {
        _readOptions->setOptionString(
            options().osgOptionString().get() + " " +
            _readOptions->getOptionString());
    }
}

const osgDB::Options*
Layer::getReadOptions() const
{
    return _readOptions.get();
}

void
Layer::setCacheID(const std::string& value)
{
    _runtimeCacheId = "";
    setOptionThatRequiresReopen(options().cacheId(), value);
}

std::string
Layer::getCacheID() const
{
    // create the unique cache ID for the cache bin.
    if (_runtimeCacheId.empty() == false)
    {
        return _runtimeCacheId;
    }
    else if (options().cacheId().isSet() && !options().cacheId()->empty())
    {
        // user expliticy set a cacheId in the terrain layer options.
        // this appears to be a NOP; review for removal -gw
        return options().cacheId().get();
    }
    else
    {
        // system will generate a cacheId from the layer configuration.
        Config hashConf = options().getConfig();

        // remove non-data properties.
        // TODO: move these a virtual function called
        // getNonDataProperties() or something.
        hashConf.remove("accept_draping");
        hashConf.remove("altitude");
        hashConf.remove("async");
        hashConf.remove("attenuation_range");
        hashConf.remove("attribution");
        hashConf.remove("blend");
        hashConf.remove("cacheid");
        hashConf.remove("cache_id");
        hashConf.remove("cache_enabled");
        hashConf.remove("cache_only");
        hashConf.remove("cache_policy");
        hashConf.remove("caching");
        hashConf.remove("color_filters");
        hashConf.remove("enabled");
        hashConf.remove("fid_attribute");
        hashConf.remove("geo_interpolation");
        hashConf.remove("l2_cache_size");
        hashConf.remove("max_data_level");
        hashConf.remove("max_filter");
        hashConf.remove("max_level");
        hashConf.remove("max_range");
        hashConf.remove("min_filter");
        hashConf.remove("min_level");
        hashConf.remove("min_range");
        hashConf.remove("name");
        hashConf.remove("open_write");
        hashConf.remove("proxy");
        hashConf.remove("rewind_polygons");
        hashConf.remove("shader");
        hashConf.remove("shaders");
        hashConf.remove("shader_define");
        hashConf.remove("shared");
        hashConf.remove("shared_sampler");
        hashConf.remove("shared_matrix");
        hashConf.remove("terrain");
        hashConf.remove("texture_compression");
        hashConf.remove("visible");

        auto hash = osgEarth::hashString(hashConf.toJSON());
        std::stringstream buf;
        const char hyphen = '-';
        if (getName().empty() == false)
            buf << toLegalFileName(getName(), false, &hyphen) << hyphen;
        buf << std::hex << std::setw(8) << std::setfill('0') << hash;
        return buf.str();
    }
}

Config
Layer::getConfig() const
{
    Config conf = options().getConfig();
    conf.key() = getConfigKey();
    return conf;
}

bool
Layer::getOpenAutomatically() const
{
    return (options().openAutomatically() == true);
}

void
Layer::setOpenAutomatically(bool value)
{
    if (options().openAutomatically() != value)
    {
        options().openAutomatically() = value;
    }
}

bool
Layer::getEnabled() const
{
    return getOpenAutomatically();
}

void
Layer::setEnabled(bool value)
{
    setOpenAutomatically(value);
}

const Status&
Layer::setStatus(const Status& status) const
{
    _status = status;
    return _status;
}

const Status&
Layer::setStatus(const Status::Code& code, const std::string& message) const
{
    return setStatus(Status(code, message));
}

void
Layer::setCachePolicy(const CachePolicy& value)
{
    options().cachePolicy() = value;

    if (_cacheSettings.valid())
    {
        _cacheSettings->integrateCachePolicy(options().cachePolicy());
    }
}

const CachePolicy&
Layer::getCachePolicy() const
{
    return options().cachePolicy().get();
}

void
Layer::init()
{
    _uid = osgEarth::createUID();
    _renderType = RENDERTYPE_NONE;
    _status.set(Status::ResourceUnavailable, getOpenAutomatically() ? "Layer closed" : "Layer disabled");

    // For detecting scene graph changes at runtime
    _sceneGraphCallbacks = new SceneGraphCallbacks(this);

    // Copy the layer options name into the Object name.
    // This happens here AND in open.
    if (osg::Object::getName().empty())
    {
        osg::Object::setName(options().name().get());
    }

    if (osg::Object::getName().empty())
    {
        osg::Object::setName("[" + std::string(className()) + "]");
    }
}

Status
Layer::open()
{
    // Cannot open a layer that's already open OR is disabled.
    if (isOpen())
    {
        return getStatus();
    }

    NetworkMonitor::ScopedRequestLayer layerRequest(getName());

    // be optimistic :)
    _status.set(Status::NoError);

    // Copy the layer options name into the Object name.
    if (options().name().isSet())
    {
        osg::Object::setName(options().name().get());
    }

    // Install any shader #defines
    if (options().shaderDefine().isSet() && !options().shaderDefine()->empty())
    {
        OE_DEBUG << LC << "Setting shader define " << options().shaderDefine().get() << "\n";
        getOrCreateStateSet()->setDefine(options().shaderDefine().get());
    }

    setStatus(openImplementation());
    if (isOpen())
    {
        onOpen.fire(this);

        // deprecated
        OE_CALL_DEPRECATED(fireCallback(&LayerCallback::onOpen));
    }

    return getStatus();
}

Status
Layer::open(const osgDB::Options* readOptions)
{
    setReadOptions(readOptions);
    return open();
}

Status
Layer::openImplementation()
{
    // Create some local cache settings for this layer.
    // There might be a CacheSettings object in the readoptions that
    // came from the map. If so, copy it.
    CacheSettings* oldSettings = CacheSettings::get(_readOptions.get());
    _cacheSettings = oldSettings ? new CacheSettings(*oldSettings) : new CacheSettings();

    // If the layer hints are set, integrate that cache policy next.
    _cacheSettings->integrateCachePolicy(layerHints().cachePolicy());

    // bring in the new policy for this layer if there is one:
    _cacheSettings->integrateCachePolicy(options().cachePolicy());

    // if caching is a go, install a bin.
    if (_cacheSettings->isCacheEnabled())
    {
        _runtimeCacheId = getCacheID();

        // make our cacheing bin!
        CacheBin* bin = _cacheSettings->getCache()->addBin(_runtimeCacheId);
        if (bin)
        {
            OE_DEBUG << LC << "Cache bin is [" << _runtimeCacheId << "]" << std::endl;
            _cacheSettings->setCacheBin(bin);
        }
        else
        {
            // failed to create the bin, so fall back on no cache mode.
            OE_WARN << LC << "Failed to open a cache bin [" << _runtimeCacheId << "], disabling caching" << std::endl;
            _cacheSettings->cachePolicy() = CachePolicy::NO_CACHE;
        }
    }

    // Store it for further propagation!
    _cacheSettings->store(_readOptions.get());

    return Status::OK();
}

Status
Layer::closeImplementation()
{
    return Status::NoError;
}

Status
Layer::close()
{    
    if (isOpen())
    {
        Threading::ScopedWriteLock lock(_inuse_mutex);
        closeImplementation();
        _status.set(Status::ResourceUnavailable, "Layer closed");
        _runtimeCacheId = "";

        onClose.fire(this);

        // @deprecated - remove after 3.7.3
        OE_CALL_DEPRECATED(fireCallback(&LayerCallback::onClose));
    }
    return getStatus();
}

bool
Layer::isOpen() const
{
    return getStatus().isOK();
}

const Status&
Layer::getStatus() const
{
    return _status;
}

void
Layer::invoke_prepareForRendering(TerrainEngine* engine)
{
    prepareForRendering(engine);
}

void
Layer::prepareForRendering(TerrainEngine* engine)
{
    // Install an earth-file shader if necessary (once)
    for (const auto& shaderOptions : options().shaders())
    {
        LayerShader* shader = new LayerShader(shaderOptions);
        shader->install(this, engine->getResources());
        _shaders.emplace_back(shader);
    }
}

void
Layer::setName(const std::string& name)
{
    osg::Object::setName(name);
    options().name() = name;
}

const char*
Layer::getTypeName() const
{
    return typeid(*this).name();
}

#define LAYER_OPTIONS_TAG "osgEarth.LayerOptions"

template<class T>
struct Holder : public osg::Object {
    META_Object(osgEarth, Holder<T>);
    Holder() { }
    Holder(const Holder<T>& rhs, const osg::CopyOp& op) : osg::Object(rhs, op), value(rhs.value) { }
    Holder(const std::string name, const T& t) : value(t) {
        setName(name);
    }
    T value;
};

osg::ref_ptr<Layer>
Layer::create(const ConfigOptions& options)
{
    std::string name = options.getConfig().key();

    if (name.empty())
    {
        name = options.getConfig().value("driver");
    }

    if ( name.empty() )
    {
        // fail silently
        OE_DEBUG << "[Layer] ILLEGAL- Layer::create requires a valid driver name" << std::endl;
        return nullptr;
    }

    // convey the configuration options:
    osg::ref_ptr<osgDB::Options> dbopt = Registry::instance()->cloneOrCreateOptions();
    dbopt->getOrCreateUserDataContainer()->addUserObject(new Holder<ConfigOptions>(LAYER_OPTIONS_TAG, options));

    osg::ref_ptr<Layer> result;

    name = "osgearth_layer_" + name;

    // use this instead of osgDB::readObjectFile b/c the latter prints a warning msg.
    auto rw = osgDB::Registry::instance()->getReaderWriterForExtension(name);
    if (rw)
    {
        auto rr = rw->readObject("." + name, dbopt.get());
        if (!rr.validObject() || rr.error())
        {
            // quietly fail so we don't get tons of msgs.
            return nullptr;
        }

        result = dynamic_cast<Layer*>(rr.getObject());
        if (result.valid())
        {
            if (result->getName().empty())
                result->setName(name);
        }
    }

    return result;
}

namespace {
    ConfigOptions s_default_config_options;
}
const ConfigOptions&
Layer::getConfigOptions(const osgDB::Options* options)
{
    //const void* data = options ? options->getPluginData(LAYER_OPTIONS_TAG) : nullptr;
    //return data ? *static_cast<const ConfigOptions*>(data) : s_default_config_options;
    if (options) {
        auto* udc = options->getUserDataContainer();
        if (udc) {
            auto* obj = udc->getUserObject(LAYER_OPTIONS_TAG);
            if (obj) {
                return static_cast<const Holder<ConfigOptions>*>(obj)->value;
            }
        }
    }
    return s_default_config_options;
}

SceneGraphCallbacks*
Layer::getSceneGraphCallbacks() const
{
    return _sceneGraphCallbacks.get();
}

void
Layer::addCallback(LayerCallback* cb)
{
    _callbacks.push_back( cb );
}

void
Layer::removeCallback(LayerCallback* cb)
{
    CallbackVector::iterator i = std::find( _callbacks.begin(), _callbacks.end(), cb );
    if ( i != _callbacks.end() )
        _callbacks.erase( i );
}

void
Layer::apply(osg::Node* node, osg::NodeVisitor* nv) const
{
    if (_traversalCallback.valid())
    {
        _traversalCallback->operator()(node, nv);
    }
    else
    {
        node->accept(*nv);
    }
}

void
Layer::setCullCallback(TraversalCallback* cb)
{
    _traversalCallback = cb;
}

const Layer::TraversalCallback*
Layer::getCullCallback() const
{
    return _traversalCallback.get();
}

const GeoExtent&
Layer::getExtent() const
{
    static GeoExtent s_invalid = GeoExtent::INVALID;
    return s_invalid;
}

DateTimeExtent
Layer::getDateTimeExtent() const
{
    return DateTimeExtent();
}

osg::StateSet*
Layer::getOrCreateStateSet()
{
    if (!_stateSet.valid())
    {
        _stateSet = new osg::StateSet();
        _stateSet->setName("Layer");
    }
    return _stateSet.get();
}

osg::StateSet*
Layer::getStateSet() const
{
    return _stateSet.get();
}

// deprecated, use onOpen or onClose
void
Layer::fireCallback(LayerCallback::MethodPtr method)
{
    for (CallbackVector::iterator i = _callbacks.begin(); i != _callbacks.end(); ++i)
    {
        LayerCallback* cb = dynamic_cast<LayerCallback*>(i->get());
        if (cb) (cb->*method)(this);
    }
}

std::string
Layer::getAttribution() const
{
    // Get the attribution from the layer if it's set.
    return options().attribution().get();
}

void
Layer::setAttribution(const std::string& attribution)
{
    options().attribution() = attribution;
}

void
Layer::resizeGLObjectBuffers(unsigned maxSize)
{
    osg::Object::resizeGLObjectBuffers(maxSize);
    if (getNode())
        getNode()->resizeGLObjectBuffers(maxSize);
    if (getStateSet())
        getStateSet()->resizeGLObjectBuffers(maxSize);
}

void
Layer::releaseGLObjects(osg::State* state) const
{
    osg::Object::releaseGLObjects(state);
    if (getNode())
        getNode()->releaseGLObjects(state);
    if (getStateSet())
        getStateSet()->releaseGLObjects(state);
}

void
Layer::modifyTileBoundingBox(const TileKey& key, osg::BoundingBox& box) const
{
    //NOP
}

const Layer::Hints&
Layer::getHints() const
{
    return _hints;
}

Layer::Hints&
Layer::layerHints()
{
    return _hints;
}

const std::string&
Layer::getOsgOptionString() const
{
    return options().osgOptionString().get();
}

void
Layer::setUserProperty(
    const std::string& key,
    const std::string& value)
{
    options()._internal().set(key, value);
}
