/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/ScriptEngine>
#include <osgEarth/Notify>
#include <osgEarth/Registry>
#include <osgEarth/Feature>
#include <osgDB/ReadFile>
#include <mutex>

using namespace osgEarth;

/****************************************************************/

void
ScriptEngineOptions::fromConfig( const Config& conf )
{
    optional<std::string> val;
    if (conf.get<std::string>( "script_code", val))
    {
        Script cfgScript(val.get());

        if (conf.get<std::string>( "script_language", val ))
          cfgScript.setLanguage(val.get());

        if (conf.get<std::string>( "script_name", val ))
          cfgScript.setName(val.get());
    }
}

void
ScriptEngineOptions::mergeConfig( const Config& conf )
{
    DriverConfigOptions::mergeConfig( conf );
    fromConfig( conf );
}

Config
ScriptEngineOptions::getConfig() const
{
    Config conf = DriverConfigOptions::getConfig();

    if (_script.isSet())
    {
        if (!_script->getCode().empty()) conf.set("script_code", _script->getCode());
        if (!_script->getLanguage().empty()) conf.set("script_language", _script->getLanguage());
        if (!_script->getName().empty()) conf.set("script_name", _script->getName());
    }

    return conf;
}

//------------------------------------------------------------------------

bool
ScriptEngine::run(
    const std::string& code,
    const FeatureList& features,
    std::vector<ScriptResult>& results,
    FilterContext const* context)
{
    for (auto& feature : features)
    {
        results.emplace_back(run(code, feature.get(), context));
    }
    return true;
}

//------------------------------------------------------------------------

#undef  LC
#define LC "[ScriptEngineFactory] "
#define SCRIPT_ENGINE_OPTIONS_TAG "__osgEarth::ScriptEngineOptions"

ScriptEngineFactory*
ScriptEngineFactory::instance()
{
    static std::once_flag s_once;
    static osg::ref_ptr<ScriptEngineFactory> s_singleton;

    std::call_once(s_once, []() {
        s_singleton = new ScriptEngineFactory();
        Registry::instance()->registerSingleton(s_singleton.get());
    });

    return s_singleton;
}

namespace
{
    std::string makeDriverName(const std::string& language, const std::string& engineName)
    {
        if (engineName.empty())
            return language + "_" + Registry::instance()->getScriptEngineDriverName();
        else
            return language + "_" + engineName;
    }
}

ScriptEngine*
ScriptEngineFactory::create(const std::string& language, const std::string& engineName, bool quiet)
{
    ScriptEngineOptions opts;
    opts.setDriver(makeDriverName(language, engineName));
    return create(opts, quiet);
}

ScriptEngine*
ScriptEngineFactory::create(const Script& script, const std::string& engineName, bool quiet)
{
    ScriptEngineOptions opts;
    opts.setDriver(makeDriverName(script.getLanguage(), engineName));
    opts.script() = script;

    return create(opts, quiet);
}

ScriptEngine*
ScriptEngineFactory::createWithProfile(const Script& script, const std::string& profile, const std::string& engineName, bool quiet)
{
    ScriptEngineOptions opts;
    opts.setDriver(makeDriverName(script.getLanguage(), engineName));
    opts.script() = script;

    ScriptEngine* e = create(opts, quiet);
    if (e)
        e->setProfile(profile);
    return e;
}

ScriptEngine*
ScriptEngineFactory::create( const ScriptEngineOptions& options, bool quiet)
{
    osg::ref_ptr<ScriptEngine> scriptEngine;

    if ( !options.getDriver().empty() )
    {
        if ( std::find(instance()->_failedDrivers.begin(), instance()->_failedDrivers.end(), options.getDriver()) == instance()->_failedDrivers.end() )
        {
            std::string driverExt = std::string("osgearth_scriptengine_") + options.getDriver();

            osg::ref_ptr<osgDB::Options> rwopts = Registry::instance()->cloneOrCreateOptions();
            rwopts->setPluginData( SCRIPT_ENGINE_OPTIONS_TAG, (void*)&options );
            auto rw = osgDB::Registry::instance()->getReaderWriterForExtension(driverExt);
            if (rw)
            {
                osg::ref_ptr<osg::Object> object = rw->readObject("." + driverExt, rwopts.get()).getObject();
                scriptEngine = dynamic_cast<ScriptEngine*>(object.release());
            }
            if (!scriptEngine.valid())
            {
                if (!quiet)
                    OE_WARN << "FAIL, unable to load ScriptEngine driver for \"" << options.getDriver() << "\"" << std::endl;

                instance()->_failedDrivers.push_back(options.getDriver());
            }
        }
        else
        {
            //OE_WARN << "Skipping previously failed ScriptEngine driver \"" << options.getDriver() << "\"" << std::endl;
        }
    }
    else
    {
        if (!quiet)
            OE_WARN << LC << "FAIL, illegal null driver specification" << std::endl;
    }

    return scriptEngine.release();
}

//------------------------------------------------------------------------

const ScriptEngineOptions&
ScriptEngineDriver::getScriptEngineOptions( const osgDB::ReaderWriter::Options* options ) const
{
    static ScriptEngineOptions s_default;
    const void* data = options->getPluginData(SCRIPT_ENGINE_OPTIONS_TAG);
    return data ? *static_cast<const ScriptEngineOptions*>(data) : s_default;
}



std::string
osgEarth::evaluateExpression(const std::string& expr, ScriptEngine* engine)
{
    OE_SOFT_ASSERT_AND_RETURN(engine, {});

    // Evaluate the expression using the engine.
    auto result = engine->run(expr);

    if (result.success())
        return result.asString();
    else
        OE_WARN << LC << "Expression evaluation failed: " << result.message() << std::endl;

    return {};
}
