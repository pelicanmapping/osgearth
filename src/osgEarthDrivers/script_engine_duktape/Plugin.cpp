/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgDB/ReaderWriter>
#include <osgEarth/ScriptEngine>
#include <osgEarth/Common>
#include <osgDB/FileNameUtils>
#include <osgDB/Registry>

#include "DuktapeEngine"

#define LC "[Duktape] "

namespace osgEarth { namespace Drivers { namespace Duktape
{
    /**
     * Driver plugin entry point - creates a DuktapeEngine instance.
     */
    class DuktapeScriptEngineDriver : public osgEarth::ScriptEngineDriver
    {
    public:
        DuktapeScriptEngineDriver()
        {
            supportsExtension(
                "osgearth_scriptengine_javascript_duktape", "osgEarth Duktape JavaScript Engine" );
        }

        const char* className() const
        {
            return "osgEarth Duktape JavaScript Engine";
        }

        ReadResult readObject(const std::string& filename, const osgDB::Options* dbOptions) const
        {
          if ( !acceptsExtension(osgDB::getLowerCaseFileExtension(filename)) )
                return ReadResult::FILE_NOT_HANDLED;

          return ReadResult( new DuktapeEngine(getScriptEngineOptions(dbOptions)) );
        }
    };

    REGISTER_OSGPLUGIN(osgearth_scriptengine_javascript_duktape, DuktapeScriptEngineDriver)

} } } // namespace osgEarth::Drivers::Duktape
