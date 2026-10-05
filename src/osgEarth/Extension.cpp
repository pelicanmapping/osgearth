/* osgEarth
 * Copyright 2008-2013 Pelican Mapping
 * MIT License
 */
#include <osgEarth/Extension>
#include <osgEarth/Registry>
#include <cctype>
#include <map>
#include <mutex>

using namespace osgEarth;

#define LC "[Extension] "

#define EXTENSION_OPTIONS_TAG "__osgEarth::ExtensionOptions"


Extension::Extension()
{
    //nop
    _defaultOptions = Config("extension");
}

const ConfigOptions&
Extension::getConfigOptions() const
{
    return _defaultOptions;
}

namespace
{
    struct ExtensionFactories
    {
        std::mutex mutex;
        std::map<std::string, Extension::Factory> factories;
    };

    //! Initializes the shared registry safely, including during nodekit loading.
    ExtensionFactories& extensionFactories()
    {
        static ExtensionFactories registry;
        return registry;
    }

    //! Copies a factory under the lock so callers can load modules or construct extensions without holding it.
    Extension::Factory findExtensionFactory(const std::string& name)
    {
        auto& registry = extensionFactories();
        std::lock_guard<std::mutex> lock(registry.mutex);
        auto i = registry.factories.find(toLower(name));
        return i == registry.factories.end() ? nullptr : i->second;
    }
}

bool
Extension::registerFactory(const std::string& name, Factory factory)
{
    if (name.empty() || !factory) return false;
    auto& registry = extensionFactories();
    std::lock_guard<std::mutex> lock(registry.mutex);
    return registry.factories.emplace(toLower(name), factory).second;
}

Extension*
Extension::create(const std::string& name, const ConfigOptions& options)
{
    if ( name.empty() )
    {
        OE_WARN << LC << "ILLEGAL- Extension::create requires a plugin name" << std::endl;
        return 0L;
    }

    auto factory = findExtensionFactory(name);
    const auto separator = name.find(':');
    if (!factory && separator != std::string::npos && separator > 0)
    {
        // A namespace identifies an optional nodekit, not a dependency of the core library.
        std::string nodekit = name.substr(0, separator);
        nodekit[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(nodekit[0])));
        auto* registry = osgDB::Registry::instance();
        registry->loadLibrary(registry->createLibraryNameForNodeKit("osgEarth" + nodekit));
        factory = findExtensionFactory(name);
    }
    if (factory)
    {
        auto* extension = factory(options);
        if (!extension) return nullptr;
        extension->_defaultOptions = options;
        if (extension->getName().empty()) extension->setName(name);
        return extension;
    }

    // convey the configuration options:
    osg::ref_ptr<osgDB::Options> dbopt = Registry::instance()->cloneOrCreateOptions();
    dbopt->setPluginData( EXTENSION_OPTIONS_TAG, (void*)&options );

    std::string pluginExtension = std::string( "osgearth_" ) + name;

    // use this instead of osgDB::readObjectFile b/c the latter prints a warning msg.
    auto rw = osgDB::Registry::instance()->getReaderWriterForExtension(pluginExtension);
    if (!rw)
    {
        return nullptr;
    }

    auto rr = rw->readObject("." + pluginExtension, dbopt.get());
    if ( !rr.validObject() || rr.error() )
    {
        // quietly fail so we don't get tons of msgs.
        return nullptr;
    }

    Extension* extension = dynamic_cast<Extension*>( rr.getObject() );
    if ( extension == nullptr )
    {
        OE_WARN << LC << "Plugin \"" << name << "\" is not an Extension" << std::endl;
        return 0L;
    }

    // for automatic serialization, in the event that the subclass does not
    // implement getConfigOptions.
    extension->_defaultOptions = options;

    if (extension->getName().empty())
        extension->setName(name);

    rr.takeObject();
    return extension;
}


const ConfigOptions&
Extension::getConfigOptions(const osgDB::Options* options)
{
    static ConfigOptions s_default;
    const void* data = options->getPluginData(EXTENSION_OPTIONS_TAG);
    return data ? *static_cast<const ConfigOptions*>(data) : s_default;
}
