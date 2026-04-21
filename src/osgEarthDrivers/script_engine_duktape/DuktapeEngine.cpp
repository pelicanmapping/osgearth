/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "DuktapeEngine"
#include "JSGeometry"
#include <osgEarth/StringUtils>
#include <osgEarth/Metrics>

#undef  LC
#define LC "[JavaScript] "

using namespace osgEarth;
using namespace osgEarth::Drivers::Duktape;

//............................................................................

#define FEATURE_PTR_KEY "\xff""feature_ptr"

namespace
{
    // generic logging function.
    static duk_ret_t log( duk_context *ctx )
    {
        duk_idx_t i, n;

        std::string msg;
        for( i = 0, n = duk_get_top( ctx ); i < n; i++ ) {
            if( i > 0 ) {
                msg += " ";
            }
            msg += duk_safe_to_string( ctx, i );
        }
        OE_WARN << LC << msg << std::endl;
        return 0;
    }

    static duk_ret_t oe_duk_save_feature(duk_context* ctx)
    {
        Feature* feature = nullptr;
        
        // pushed "this" object (object the function was called on):
        duk_push_this(ctx); // [feature]

        // get the hidden native pointer property:
        duk_get_prop_string(ctx, -1, FEATURE_PTR_KEY); // [feature, value|undefined]
        feature = reinterpret_cast<Feature*>(duk_get_pointer(ctx, -1));

        duk_pop(ctx); // [feature]

        if (!feature)
        {
            duk_pop(ctx); // []
            return duk_error(ctx, DUK_ERR_TYPE_ERROR, "feature.save(): missing native Feature pointer");
        }

        if ( duk_get_prop_string(ctx, -1, "properties") && duk_is_object(ctx, -1) ) // [feature, properties|undefined]
        {
            duk_enum(ctx, -1, 0); // [feature, properties, enum]

            while (duk_next(ctx, -1, 1/*get_value=true*/)) // [feature, properties, enum, key, value]
            {
                std::string key( duk_get_string(ctx, -2) );

                if (duk_is_string(ctx, -1))
                {
                    feature->set( key, std::string(duk_get_string(ctx, -1)) );
                }
                else if (duk_is_number(ctx, -1))
                {
                    feature->set( key, (double)duk_get_number(ctx, -1) );
                }
                else if (duk_is_boolean(ctx, -1))
                {
                    feature->set( key, duk_get_boolean(ctx, -1) != 0 );
                }
                else if( duk_is_null_or_undefined( ctx, -1 ) )
                {
                    feature->setNull( key );
                }
                duk_pop_2(ctx); // [feature, properties, enum]
            }

            duk_pop_2(ctx); // [feature]
        }
        else
        {  
            duk_pop(ctx); // [feature]
        }

#if 0
        // save the geometry, if set:
        if ( duk_get_prop_string(ctx, -1, "geometry") )
        {
            if (duk_is_object(ctx, -1))
            {
                // [ptr, global, feature, geometry]
                std::string json( duk_json_encode(ctx, -1) ); // [ptr, global, feature, json]
                Geometry* newGeom = GeometryUtils::geometryFromGeoJSON(json);
                if ( newGeom )
                {
                    feature->setGeometry( newGeom );
                }
                duk_pop(ctx); // [ptr, global, feature]
            }
            else
            {
                feature->setGeometry(nullptr);
            }
        }
        else
        {
            // [ptr, global, feature, undefined]
        }
#endif

        duk_pop(ctx); // [] (as we found it)
        return 0;     // no return values.
    }
}

//............................................................................

namespace
{

    // Create a "feature" object in the global namespace.
    void setFeature(duk_context* ctx, Feature const* feature, bool exposeGeometryAPI)
    {
        if (!feature)
            return;

        OE_PROFILING_ZONE;

        duk_push_global_object(ctx); // [global]

        // Complete profile: properties, geometry, and API bindings.
        if (exposeGeometryAPI)
        {
            std::string geojson = feature->getGeoJSON();
            duk_push_string(ctx, geojson.c_str());               // [global, json]
            duk_json_decode(ctx, -1);                            // [global, feature]
            duk_push_pointer(ctx, (void*)feature);               // [global, feature, ptr]
            duk_put_prop_string(ctx, -2, "__ptr");               // [global, feature]
            duk_put_prop_string(ctx, -2, "feature");             // [global]

            GeometryAPI::bindToFeature(ctx);
        }

        // Minimal profile: ID and properties only. MUCH faster!
        else
        {
            duk_idx_t feature_i = duk_push_object(ctx);     // [global] [feature]
            {
                duk_push_number(ctx, feature->getFID());    // [global] [feature] [id]
                duk_put_prop_string(ctx, feature_i, "id");  // [global] [feature]

                auto& attrs = feature->getAttrs();

                // Any attribute whose name starts with "." will be added directly to feature.
                for (auto& a : attrs)
                {
                    if (a.first[0] == '.')
                    {
                        auto type = a.second.getType();

                        if (type == ATTRTYPE_DOUBLE)
                        {
                            duk_push_number(ctx, a.second.getDouble()); // [global] [feature] [name]
                        }
                        else if (type == ATTRTYPE_INT)
                        {
                            duk_push_number(ctx, (double)a.second.getInt()); // [global] [feature] [name]
                        }
                        else if (type == ATTRTYPE_BOOL)
                        {
                            duk_push_boolean(ctx, a.second.getBool() ? 1 : 0); // [global] [feature] [name]
                        }
                        else // ATTRTYPE_STRING or others
                        {
                            duk_push_string(ctx, a.second.getAsString().c_str()); // [global] [feature] [name]
                        }

                        duk_put_prop_string(ctx, feature_i, a.first.substr(1).c_str());   // [global] [feature] [properties]
                    }
                }

                duk_idx_t props_i = duk_push_object(ctx);   // [global] [feature] [properties]
                {
                    for(auto& a : attrs)
                    {
                        if (a.first[0] != '.')
                        {
                            auto type = a.second.getType();

                            if (type == ATTRTYPE_DOUBLE)
                            {
                                duk_push_number(ctx, a.second.getDouble());         // [global] [feature] [name]
                            }
                            else if (type == ATTRTYPE_INT)
                            {
                                duk_push_number(ctx, (double)a.second.getInt());    // [global] [feature] [name]
                            }
                            else if (type == ATTRTYPE_BOOL)
                            {
                                duk_push_boolean(ctx, a.second.getBool() ? 1 : 0);  // [global] [feature] [name]
                            }
                            else // ATTRTYPE_STRING or others
                            {
                                duk_push_string(ctx, a.second.getAsString().c_str()); // [global] [feature] [name]
                            }

                            duk_put_prop_string(ctx, props_i, a.first.c_str());     // [global] [feature] [properties]
                        }
                    }
                }
                duk_put_prop_string(ctx, feature_i, "properties"); // [global] [feature]

                duk_idx_t geometry_i = duk_push_object(ctx);  // [global] [feature] [geometry]
                {
                    duk_push_string(ctx, Geometry::toString(feature->getGeometry()->getComponentType()).c_str()); // [global] [feature] [geometry] [type]
                    duk_put_prop_string(ctx, geometry_i, "type"); // [global] [feature] [geometry]
                }
                duk_put_prop_string(ctx, feature_i, "geometry"); // [global] [feature]
            }

            duk_push_pointer(ctx, (void*)feature); // [global] [feature] [ptr]
            duk_put_prop_string(ctx, -2, FEATURE_PTR_KEY); // [global] [feature]

            duk_push_c_function(ctx, oe_duk_save_feature, 0/*numargs*/); // [global] [feature] [function]
            duk_put_prop_string(ctx, -2, "save"); // [global] [feature]

            duk_put_prop_string(ctx, -2, "feature"); // [global]
        }

        // add the save() function
        //duk_eval_string_noresult(ctx,
        //    "feature.save = function() { oe_duk_save_feature(this.__ptr); }");

        duk_pop(ctx);
    }

}

//............................................................................

namespace
{
    std::string prepareScript(const std::string& input)
    {
        // There is an apparent bug(?) in duktape with respect to newlines.
        // Where this would return true:
        // 
        //    return feature.properties.landuse === 'forest';
        // 
        // ..this returns false:
        // 
        //    return
        //        feature.properties.landuse === 'forest';
        //
        // The workaround here is to remove newlines (that are not inside quotes);
        // we also have to remove inline comments along the way.

        // split the script into lines
        auto lines = StringTokenizer()
            .delim("\n")
            .keepEmpties(false)
            .tokenize(input);

        StringTokenizer stripInlineComment;
        stripInlineComment
            .delim("//")
            .standardQuotes()
            .ignoreDanglingQuotes();

        for (auto& line : lines)
        {
            auto tokens = stripInlineComment(line);
            if (tokens.size() > 0)
                line = tokens[0];
            else
                line = {};
        }

        // reassemble the script into a single line string
        std::string output;
        output.reserve(input.size());
        for (auto& line : lines)
            output.append(line).append(" ");

        return output;
    }
}

void
DuktapeEngine::Context::initialize(const ScriptEngineOptions& options, bool exposeGeometryAPI)
{
    if ( _ctx == nullptr)
    {
        // new heap + context.
        _ctx = duk_create_heap_default();

        // if there is a static script, evaluate it first. This will register
        // any functions or objects with the EcmaScript global object.
        if ( options.script().isSet() )
        {
            auto temp = prepareScript(options.script()->getCode());

            bool ok = (duk_peval_string(_ctx, temp.c_str()) == 0); // [ "result" ]
            if ( !ok )
            {
                const char* err = duk_safe_to_string(_ctx, -1);
                OE_WARN << LC << err << std::endl;
                OE_WARN << LC << "Code:" << std::endl << options.script()->getCode() << std::endl;
            }
            duk_pop(_ctx); // []
        }

        duk_push_global_object( _ctx );

        // Add global log function.
        duk_push_c_function( _ctx, log, DUK_VARARGS ); // [global, function]
        duk_put_prop_string( _ctx, -2, "log" );        // [global]

        if (exposeGeometryAPI)
        {
            GeometryAPI::install(_ctx);
        }

        duk_pop(_ctx); // []
    }
}

DuktapeEngine::Context::~Context()
{
    if ( _ctx )
    {
        duk_destroy_heap(_ctx);
        _ctx = nullptr;
    }
}

//............................................................................

DuktapeEngine::DuktapeEngine(const ScriptEngineOptions& options) :
    ScriptEngine(options),
    _options(options)
{
    //nop
}

DuktapeEngine::~DuktapeEngine()
{
    //nop
}

bool
DuktapeEngine::compile(Context& c, const std::string& code, ScriptResult& result)
{
    duk_context* ctx = c._ctx;

    if (code == c._bytecodeSource)
    {
        if (c._failed)
        {
            // this code caused a previous compile error, so bail out.
            // []
            return false;
        }
        else
        {
            // function is already compiled and atop the stack, good to go.
            // [function]
            return true;
        }
    }

    if (code.empty())
    {
        return false;
    }

    if (!c._bytecodeSource.empty())
    {
        // [function]
        // remove the old compiled function from atop the stack.
        duk_pop(ctx); // []
    }

    c._bytecodeSource = code;

    if (duk_pcompile_string(ctx, 0, code.c_str()) == 0)
    {
        // compile succeeded, function is atop the stack and ready to use.
        // [function]
        c._failed = false;
        return true;
    }

    else
    {
        // [error]
        std::string resultString = duk_safe_to_string(ctx, -1);
        OE_WARN << LC << "Compile error: " << resultString << std::endl;
        c._failed = true;
        duk_pop(ctx); // []
        result = ScriptResult("", false, resultString); // return error.
        return false;
    }
}

bool
DuktapeEngine::run(const std::string& code, const FeatureList& features, std::vector<ScriptResult>& results, FilterContext const* context)
{
    if (code.empty())
    {
        for (auto& f : features)
            results.emplace_back(EMPTY_STRING, false, "Script is empty.");
        return false;
    }

    OE_PROFILING_ZONE;

    const bool exposeGeometryAPI = false;

    // cache the Context on a per-thread basis
    Context& c = _contexts.get();
    c.initialize(_options, exposeGeometryAPI);
    duk_context* ctx = c._ctx;

    std::string resultString;
    ScriptResult result;

    if (!compile(c, code, result)) // [function | null]
    {
        // []
        for (auto& f : features)
            results.push_back(result);
        return false;
    }

    // [function]
    for (auto& feature : features)
    {
        // Load the next feature into the global object:
        setFeature(c._ctx, feature.get(), exposeGeometryAPI);

        // Duplicate the function on the top since it gets consumed by pcall and
        // we will presumably be calling it multiple times
        duk_dup_top(ctx); // [function function]

        // Run the script:
        duk_int_t rc = duk_pcall(ctx, 0); // [function result]
        resultString = duk_safe_to_string(ctx, -1);
        duk_pop(ctx); // [function]

        if (rc != DUK_EXEC_SUCCESS)
        {
            OE_WARN << LC << "Runtime error: " << resultString << std::endl;
            c._failed = true;
            duk_pop(ctx); // []
            results.emplace_back(EMPTY_STRING, false, resultString); // error
            return false;
        }
        else
        {
            results.emplace_back(resultString, true);
        }
    }

    // [function]
    // Leave the function atop the context stack, awaiting the next call.

    return true;
}

ScriptResult
DuktapeEngine::run(const std::string& code, Feature* feature, FilterContext const* context)
{
    if (code.empty())
        return ScriptResult(EMPTY_STRING, false, "Script is empty");

    OE_PROFILING_ZONE;

    const bool exposeGeometryAPI = false;

    // cache the Context on a per-thread basis
    Context& c = _contexts.get();
    c.initialize( _options, exposeGeometryAPI);
    duk_context* ctx = c._ctx;

    // compile the function:
    ScriptResult result;
    if (!compile(c, code, result))
    {
        // []
        return result;
    }

    // load the feature into the global namespace:
    // [function]
	if ( feature != c._feature.get() )
    {
		setFeature(ctx, feature, exposeGeometryAPI);
        c._feature = feature;
	}

    // Duplicate the function on the top since it gets consumed by pcall and
    // we will presumably be calling it multiple times
    duk_dup_top(ctx); // [function function]

    std::string resultString;

    duk_int_t rc = duk_pcall(ctx, 0); // [function result]
    resultString = duk_safe_to_string(ctx, -1);
    duk_pop(ctx); // [function]

    if (rc != DUK_EXEC_SUCCESS)
    {
        OE_WARN << LC << "Runtime error: " << resultString << std::endl;
        c._failed = true;
        duk_pop(ctx); // []
        return ScriptResult(EMPTY_STRING, false, resultString); // error
    }

    return ScriptResult(resultString, true);
}
