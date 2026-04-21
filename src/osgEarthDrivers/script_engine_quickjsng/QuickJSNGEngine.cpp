/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "QuickJSNGEngine"
#include <osgEarth/StringUtils>

#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
#include <Superluminal/PerformanceAPI.h>
#endif

#undef  LC
#define LC "[JavaScript] "

using namespace osgEarth;
using namespace osgEarth::Drivers::QJS;

//............................................................................

namespace
{
    static JSValue log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
    {
        if (argc < 1)
            return JS_EXCEPTION;

        const char* str = JS_ToCString(ctx, argv[0]);
        if (!str)
            return JS_EXCEPTION;

        OE_NOTICE << "[JS] " << str << std::endl;

        JS_FreeCString(ctx, str);
        return JS_UNDEFINED;
    }

    static void saveFeature()
    {
        // TODO
    }
}

//............................................................................

void
QuickJSNGEngine::Context::initialize(const ScriptEngineOptions& options)
{
    if (_context == nullptr)
    {
        // new heap + context.
        _runtime = JS_NewRuntime();
        _context = JS_NewContext(_runtime);
        _globalObj = JS_GetGlobalObject(_context);

        // Compile and evaluate any static module:
        if (options.script().isSet())
        {
            auto v = JS_Eval(_context,
                options.script()->getCode().c_str(),
                options.script()->getCode().size(),
                "<static>",
                JS_EVAL_TYPE_GLOBAL);

            if (JS_IsException(v))
            {
                JSValue ex = JS_GetException(_context);
                const char* msg = JS_ToCString(_context, ex);
                OE_WARN << LC << "Error evaluating static script module: " << msg << std::endl;
                OE_WARN << LC << "Code:" << std::endl << options.script()->getCode() << std::endl;
                JS_FreeCString(_context, msg);
                JS_FreeValue(_context, ex);
            }
        }

        // Log function:
        JS_SetPropertyStr(_context, _globalObj, "log", JS_NewCFunction(_context, log, "log", 1));
    }
}

QuickJSNGEngine::Context::~Context()
{
    if ( _context )
        JS_FreeContext(_context);

    if (_runtime)
        JS_FreeRuntime(_runtime);
}

bool
QuickJSNGEngine::Context::compile(const std::string& code)
{
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
    PERFORMANCEAPI_INSTRUMENT_FUNCTION();
#endif

    if (_failed)
    {
        return false;
    }

    // precompile the bytecode if it's new
    if (code != _functionSource)
    {
        if (JS_IsFunction(_context, _function))
        {
            JS_FreeValue(_context, _function);
        }

        // check the compiled function cache to see if we already have this script compiled:
        auto iter = _functions.find(code);
        if (iter == _functions.end())
        {
            _function = JS_Eval(_context, code.c_str(), code.size(), "<input>",
                JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);

            // success or failure, cache the result.
            _functions[code] = JS_DupValue(_context, _function);
        }
        else
        {
            _function = JS_DupValue(_context, iter->second);
        }

        // check to see if the script compile succeeded:
        if (JS_IsException(_function))
        {
            JSValue ex = JS_GetException(_context);
            const char* msg = JS_ToCString(_context, ex);
            OE_WARN << LC << "Compile error: " << msg << std::endl << code << std::endl;
            JS_FreeCString(_context, msg);
            JS_FreeValue(_context, ex);
            _failed = true;
            return false;
        }

        else
        {
            _functionSource = code;
            _failed = false;
        }
    }

    return true;
}

void
QuickJSNGEngine::Context::setFeature(const Feature* feature)
{
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
    PERFORMANCEAPI_INSTRUMENT_FUNCTION();
#endif

    if (!feature)
        return;

    if (!JS_IsObject(_featureObj))
    {
        // create the skeleton for our JS "feature" object - only need to do this once.
        // caching the objects and atoms speeds up updates a lot.
        _featureObj = JS_NewObject(_context);

        // atom for "feature.id"
        _atom_id = JS_NewAtom(_context, "id");

        // atom for "feature.geometry.type"
        _atom_type = JS_NewAtom(_context, "type");

        _geometryObj = JS_NewObject(_context);
        JS_SetPropertyStr(_context, _featureObj, "geometry", JS_DupValue(_context, _geometryObj));

        _propertiesObj = JS_NewObject(_context);
        JS_SetPropertyStr(_context, _featureObj, "properties", JS_DupValue(_context, _propertiesObj));

        JS_SetPropertyStr(_context, _globalObj, "feature", JS_DupValue(_context, _featureObj));

        _typeStrings[Geometry::TYPE_POINT] = JS_NewString(_context, Geometry::toString(Geometry::Type::TYPE_POINT).c_str());
        _typeStrings[Geometry::TYPE_POINTSET] = JS_NewString(_context, Geometry::toString(Geometry::Type::TYPE_POINTSET).c_str());
        _typeStrings[Geometry::TYPE_LINESTRING] = JS_NewString(_context, Geometry::toString(Geometry::Type::TYPE_LINESTRING).c_str());
        _typeStrings[Geometry::TYPE_POLYGON] = JS_NewString(_context, Geometry::toString(Geometry::Type::TYPE_POLYGON).c_str());
        _typeStrings[Geometry::TYPE_RING] = JS_NewString(_context, Geometry::toString(Geometry::Type::TYPE_RING).c_str());
        _typeStrings[Geometry::TYPE_MULTI] = JS_NewString(_context, Geometry::toString(Geometry::Type::TYPE_MULTI).c_str());
        _typeStrings[Geometry::TYPE_TRIMESH] = JS_NewString(_context, Geometry::toString(Geometry::Type::TYPE_TRIMESH).c_str());
        _typeStrings[Geometry::TYPE_UNKNOWN] = JS_NewString(_context, Geometry::toString(Geometry::Type::TYPE_UNKNOWN).c_str());
    }

    // destroy previous key set.
    // benchmarking showed that doing this is slightly faster than creating a new properties
    // object each time.
    for (auto& key : _previousKeys)
    {
        JS_DeleteProperty(_context, _propertiesObj, key, 0);
    }
    _previousKeys.clear();

    // set "feature.id"
    JS_SetProperty(_context, _featureObj, _atom_id, JS_NewInt64(_context, feature->getFID()));

    // set "feature.geometry.type"
    JS_SetProperty(_context, _geometryObj, _atom_type, JS_DupValue(_context, _typeStrings[(int)feature->getGeometry()->getComponentType()]));

    // populate "feature.properties"
    auto& attrs = feature->getAttrs();
    for (auto& a : attrs)
    {
        auto type = a.second.getType();
        auto value =
            type == ATTRTYPE_DOUBLE ? JS_NewFloat64(_context, a.second.getDouble()) :
            type == ATTRTYPE_INT ? JS_NewInt32(_context, a.second.getInt()) :
            type == ATTRTYPE_BOOL ? JS_NewBool(_context, a.second.getBool()) :
            type == ATTRTYPE_STRING ? getOrCreateStringValue(a.second.getString()) :
            getOrCreateStringValue(a.second.getAsString());

        auto atom = getPropertyAtom(a.first);
        if (a.first.front() == '.')
            JS_SetProperty(_context, _featureObj, atom, value);
        else
            JS_SetProperty(_context, _propertiesObj, atom, value);

        _previousKeys.emplace_back(atom);
    }
}

//............................................................................

QuickJSNGEngine::QuickJSNGEngine(const ScriptEngineOptions& options) :
    ScriptEngine(options),
    _options(options)
{
    //nop
}

bool
QuickJSNGEngine::run(const std::string& code, const FeatureList& features, std::vector<ScriptResult>& results, FilterContext const* context)
{
    if (code.empty())
    {
        for (auto& f : features)
            results.emplace_back(EMPTY_STRING, false, "Script is empty.");
        return false;
    }

    // cache the Context on a per-thread basis
    Context& c = _contexts.get();
    c.initialize(_options);

    if (!c.compile(code))
    {
        return false;
    }

    for (auto& feature : features)
    {
        if (!feature.valid())
            continue;

        // Load the next feature into the global object:
        c.setFeature(feature.get());

        // run the pre-compiled script:
        auto bytecode = JS_DupValue(c._context, c._function);
        auto r = JS_EvalFunction(c._context, bytecode);

        if (!JS_IsException(r))
        {
            auto str = JS_ToCString(c._context, r);
            results.emplace_back(str, true);
            JS_FreeCString(c._context, str);
        }
        else
        {
            JSValue ex = JS_GetException(c._context);
            const char* msg = JS_ToCString(c._context, ex);
            OE_WARN << LC << "Runtime error: " << msg << std::endl;
            OE_WARN << LC << "Code:\n" << code << std::endl;
            JS_FreeCString(c._context, msg);
            JS_FreeValue(c._context, ex);
            c._failed = true;
            JS_FreeValue(c._context, r); // unref
            return false;
            //break;
        }

        JS_FreeValue(c._context, r); // unref
    }

    return true;
}

namespace
{
}

ScriptResult
QuickJSNGEngine::run(const std::string& code, Feature* feature, FilterContext const* context)
{
    if (code.empty())
        return ScriptResult(EMPTY_STRING, false, "Feature is empty");

    // cache the Context on a per-thread basis
    Context& c = _contexts.get();
    c.initialize(_options);

    if (!c.compile(code))
    {
        return ScriptResult(EMPTY_STRING, false, "Compile error");
    }

    // Load the next feature into the global object:
    if (feature != c._loadedFeature)
    {
        c.setFeature(feature);
        c._loadedFeature = feature;
    }

    // run the pre-compiled script:
    auto bytecode = JS_DupValue(c._context, c._function);
    JSValue r = JS_EvalFunction(c._context, bytecode);

    ScriptResult result;

    if (!JS_IsException(r))
    {
        auto str = JS_ToCString(c._context, r);
        result = ScriptResult(str, true);
        JS_FreeCString(c._context, str);
    }
    else
    {
        JSValue ex = JS_GetException(c._context);
        const char* msg = JS_ToCString(c._context, ex);
        OE_WARN << LC << "Runtime error: " << msg << std::endl;
        result = ScriptResult(EMPTY_STRING, false, msg);
        JS_FreeCString(c._context, msg);
        JS_FreeValue(c._context, ex);
        c._failed = true;
    }

    JS_FreeValue(c._context, r); // unref

    return result;
}
