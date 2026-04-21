/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/ScriptEngine>
#include <osgDB/ReaderWriter>
#include <osgDB/FileNameUtils>
#include <osgDB/Registry>

#include "QuickJSNGEngine"

#define LC "[QuickJS] "

namespace osgEarth { namespace Drivers { namespace QJS
{
    class QJSScriptEngineDriver : public osgEarth::ScriptEngineDriver
    {
    public:
        QJSScriptEngineDriver()
        {
            supportsExtension(
                "osgearth_scriptengine_javascript_qjs", "osgEarth QuickJS JavaScript Engine" );
        }

        const char* className() const
        {
            return "osgEarth QuickJS JavaScript Engine";
        }

        ReadResult readObject(const std::string& filename, const osgDB::Options* dbOptions) const
        {
          if ( !acceptsExtension(osgDB::getLowerCaseFileExtension(filename)) )
                return ReadResult::FILE_NOT_HANDLED;

          return ReadResult( new QuickJSNGEngine(getScriptEngineOptions(dbOptions)) );
        }
    };

    REGISTER_OSGPLUGIN(osgearth_scriptengine_javascript_qjs, QJSScriptEngineDriver)

} } } // namespace osgEarth::Drivers::QJS
