/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "ShaderLoader"
#include "ShaderUtils"
#include "URI"
#include "VirtualProgram"
#include "Capabilities"
#include "ShaderFactory"
#include "GLSLChunker"

#include <osgDB/FileUtils>
#include <utility>

#undef  LC
#define LC "[ShaderLoader] "

using namespace osgEarth;
using namespace osgEarth::Util;


namespace
{
    //TODO: unordered?
    using StringMap = std::map<std::string, std::string>;


    bool parseLocation(
        const std::string& loc,
        optional<VirtualProgram::FunctionLocation>& location)
    {
        bool locationSet = true;

        if (ciEquals(loc, "vertex_transform_model_to_view"))
            location = VirtualProgram::LOCATION_VERTEX_TRANSFORM_MODEL_TO_VIEW;
        else if (ciEquals(loc, "vertex_model") || ciEquals(loc, "model"))
            location = VirtualProgram::LOCATION_VERTEX_MODEL;
        else if (ciEquals(loc, "vertex_view") || ciEquals(loc, "view"))
            location = VirtualProgram::LOCATION_VERTEX_VIEW;
        else if (ciEquals(loc, "vertex_clip") || ciEquals(loc, "clip"))
            location = VirtualProgram::LOCATION_VERTEX_CLIP;
        else if (ciEquals(loc, "tess_control") || ciEquals(loc, "tessellation_control"))
            location = VirtualProgram::LOCATION_TESS_CONTROL;
        else if (ciEquals(loc, "tess_eval") || ciEquals(loc, "tessellation_eval") || ciEquals(loc, "tessellation_evaluation") || ciEquals(loc, "tess_evaluation"))
            location = VirtualProgram::LOCATION_TESS_EVALUATION;
        else if (ciEquals(loc, "vertex_geometry") || ciEquals(loc, "geometry"))
            location = VirtualProgram::LOCATION_GEOMETRY;
        else if (ciEquals(loc, "fragment") || ciEquals(loc, "fragment_coloring") || ciEquals(loc, "coloring"))
            location = VirtualProgram::LOCATION_FRAGMENT_COLORING;
        else if (ciEquals(loc, "fragment_lighting") || ciEquals(loc, "lighting"))
            location = VirtualProgram::LOCATION_FRAGMENT_LIGHTING;
        else if (ciEquals(loc, "fragment_output"))
            location = VirtualProgram::LOCATION_FRAGMENT_OUTPUT;
        else
            locationSet = false;

        return locationSet;
    }


    struct VPFunction
    {
        VPFunction()
        {
            order.setDefault(1.0f);
        }

        std::string entryPoint;
        optional<VirtualProgram::FunctionLocation> location;
        optional<float> order;
    };

    void getVPFunction(const std::string& source, VPFunction& f)
    {
        std::string::size_type pragmaPos = source.find("#pragma vp_function");
        if (pragmaPos != std::string::npos)
        {
            std::string line = ShaderLoader::getPragmaValue(source, "vp_function");
            if (!line.empty())
            {
                auto tokens = StringTokenizer()
                    .delim(" ").delim(",").delim("\t")
                    .keepEmpties(false)
                    .trimTokens(true)
                    .tokenize(line);

                if (tokens.size() > 0)
                    f.entryPoint = tokens[0];
                if (tokens.size() > 1)
                    parseLocation(tokens[1], f.location);
                if (tokens.size() > 2) {
                    if (tokens[2] == "first")
                        f.order = -FLT_MAX;
                    else if (tokens[2] == "last")
                        f.order = FLT_MAX;
                    else
                        f.order = atof(tokens[2].c_str());
                }
            }
        }

        std::string entryPointStr = ShaderLoader::getPragmaValue(source, "vp_entryPoint");
        if (!entryPointStr.empty())
        {
            f.entryPoint = entryPointStr;
        }

        std::string locationStr = ShaderLoader::getPragmaValue(source, "vp_location");
        if (!locationStr.empty())
        {
            parseLocation(locationStr, f.location);
        }

        std::string orderStr = ShaderLoader::getPragmaValue(source, "vp_order");
        if (!orderStr.empty())
        {
            if (ciEquals(orderStr, "FLT_MAX") || ciEquals(orderStr, "last"))
                f.order = FLT_MAX;
            else if (ciEquals(orderStr, "-FLT_MAX") || ciEquals(orderStr, "first"))
                f.order = -FLT_MAX;
            else
                f.order = as<float>(orderStr, 1.0f);
        }
    }

    std::vector<std::string>
    splitAtEndOfLineStartingWith(const std::string& source, const std::string& token)
    {
        std::vector<std::string> result(2);

        std::string::size_type tokenPos = source.find(token);
        if (tokenPos == std::string::npos)
        {
            result[1] = source;
            return result;
        }

        std::string::size_type newlinePos = source.find('\n', tokenPos);
        if (newlinePos == std::string::npos)
        {
            result[0] = source;
            return result;
        }

        result[0] = source.substr(0, newlinePos+1);
        result[1] = source.substr(newlinePos+1);
        return result;
    }

    void insertStageDefine(std::string& source, osg::Shader::Type stage)
    {
        std::string define;
        if (stage == osg::Shader::VERTEX) define = "VP_STAGE_VERTEX";
        else if (stage == osg::Shader::TESSCONTROL) define = "VP_STAGE_TESSCONTROL";
        else if (stage == osg::Shader::TESSEVALUATION) define = "VP_STAGE_TESSEVALUATION";
        else if (stage == osg::Shader::GEOMETRY) define = "VP_STAGE_GEOMERTY";
        else if (stage == osg::Shader::COMPUTE) define = "VP_STAGE_COMPUTE";
        else if (stage == osg::Shader::FRAGMENT) define = "VP_STAGE_FRAGMENT";
        else define = "UNDEFINED";

        std::size_t version_pos = source.find("#version");
        std::size_t extension_pos = source.rfind("#extension");

        std::vector<std::string> parts;

        if (version_pos < extension_pos)
            parts = splitAtEndOfLineStartingWith(source, "#version");
        else if (extension_pos >= 0)
            parts = splitAtEndOfLineStartingWith(source, "#extension");

        if (parts.size() == 2)
        {
            source = parts[0] + "#define " + define + "\n" + parts[1];
        }
        else
        {
            source = "#define " + define + "\n" + source;
        }
    }

    osg::Shader::Type getShaderTypeFromLocation(VirtualProgram::FunctionLocation loc)
    {
        // If a location is set, install in that location only
        if (loc == VirtualProgram::LOCATION_VERTEX_MODEL ||
            loc == VirtualProgram::LOCATION_VERTEX_VIEW ||
            loc == VirtualProgram::LOCATION_VERTEX_CLIP)
        {
            return osg::Shader::VERTEX;
        }
        else if (
            loc == VirtualProgram::LOCATION_FRAGMENT_COLORING ||
            loc == VirtualProgram::LOCATION_FRAGMENT_LIGHTING ||
            loc == VirtualProgram::LOCATION_FRAGMENT_OUTPUT)
        {
            return osg::Shader::FRAGMENT;
        }
        else if (
            loc == VirtualProgram::LOCATION_GEOMETRY)
        {
            return osg::Shader::GEOMETRY;
        }
        else if (
            loc == VirtualProgram::LOCATION_TESS_CONTROL)
        {
            return osg::Shader::TESSCONTROL;
        }
        else if (
            loc == VirtualProgram::LOCATION_TESS_EVALUATION)
        {
            return osg::Shader::TESSEVALUATION;
        }
        else
        {
            return osg::Shader::UNDEFINED;
        }
    }
}


// find the value of a pragma, e.g.:
//   #pragma oe_key value
// returns the string "value" (without the quotes).
std::string
ShaderLoader::getPragmaValue(const std::string& source, const std::string& key)
{
    std::string token("#pragma " + key);
    std::string::size_type statementPos = source.find(token);
    if ( statementPos == std::string::npos )
        return "";

    // no quotes; parse to newline.
    std::string::size_type startPos = source.find_first_not_of(" \t", statementPos+token.length());
    if ( startPos == std::string::npos )
        return ""; // no whitespace after the pragma key

    std::string::size_type newlinePos = source.find('\n', startPos);
    if ( newlinePos == std::string::npos )
        return ""; // new newline found after pragma

    return trim(source.substr(startPos, newlinePos-startPos));
}

bool
ShaderLoader::getPragmaValueAsTokens(
    const std::string& input,
    const std::string& key,
    std::string& line_out,
    std::vector<std::string>& tokens_out)
{
    std::string::size_type statementPos = input.find(key);
    if (statementPos == std::string::npos)
        return 0;

    std::string::size_type startPos = input.find_first_not_of(" \t(", statementPos + key.length());
    if (startPos == std::string::npos)
        return 0;

    std::string::size_type endPos = input.find_first_of(")\n", startPos);
    if (endPos == std::string::npos)
        return 0;

    std::string::size_type nlPos = input.find('\n', startPos);
    if (nlPos == std::string::npos)
        return 0;

    line_out = input.substr(statementPos, nlPos - statementPos);
    std::string statement(input.substr(statementPos, endPos - statementPos));
    std::string value(trim(input.substr(startPos, endPos - startPos)));

    tokens_out = StringTokenizer()
        .delim(" ").delim(",").delim("\t")
        .keepEmpties(false)
        .trimTokens(true)
        .tokenize(value);

    return tokens_out.size();
}

void
ShaderLoader::getAllPragmaValues(const std::string&     source,
                                 const std::string&     key,
                                 std::set<std::string>& output)
{
    std::string token("#pragma " + key);
    std::string::size_type pragmaPos = 0;
    while( pragmaPos != std::string::npos )
    {
        pragmaPos = source.find(token, pragmaPos);
        if ( pragmaPos != std::string::npos )
        {
            std::string::size_type startPos = source.find_first_not_of(" \t", pragmaPos+token.length());
            if ( startPos != std::string::npos )
            {
                std::string::size_type newlinePos = source.find('\n', startPos);
                if ( newlinePos != std::string::npos )
                {
                    const size_t len = newlinePos - startPos;
                    if ( len > 0 )
                    {
                        output.insert( trim(source.substr(startPos, len)) );
                    }
                    pragmaPos = newlinePos;
                }
                else
                {
                    break;
                }
            }
            else
            {
                break;
            }
        }
    }
}

bool
ShaderLoader::load(VirtualProgram* vp, const std::string& source)
{
    ShaderPackage pkg;
    pkg.add("", source);
    return load(vp, "", pkg, nullptr);
}

bool
ShaderLoader::load(osg::Node* node, const std::string& source)
{
    OE_SOFT_ASSERT_AND_RETURN(node, false);
    return load(VirtualProgram::getOrCreate(node->getOrCreateStateSet()), source);
}

std::string
ShaderLoader::load_raw_source(
    const std::string&    filename,
    const ShaderPackage&  package,
    const osgDB::Options* dbOptions)
{
    std::string output;

    ShaderPackage::SourceMap::const_iterator source = package._sources.find(filename);
    if (source != package._sources.end())
        output = source->second;

    if (!filename.empty())
    {
        URIContext context(dbOptions);
        URI uri(filename, context);

        // searches OSG_FILE_PATH
        std::string path = osgDB::findDataFile(uri.full(), dbOptions);
        if (!path.empty())
        {
            std::string externalSource = URI(path, context).getString(dbOptions);
            if (!externalSource.empty())
            {
                OE_DEBUG << LC << "Loaded external shader " << filename << " from " << path << "\n";
                output = externalSource;
            }
        }
    }

    if (output.empty())
    {
        OE_WARN << LC << "No shader source found for \"" << filename << "\"" << std::endl;
    }

    return output;
}

std::string
ShaderLoader::load(
    const std::string& filename,
    const ShaderPackage& package,
    const osgDB::Options* dbOptions)
{
    std::string output = load_raw_source(filename, package, dbOptions);

    // Bring in include files:
    while (true)
    {
        const std::string token("#pragma include");
        std::string::size_type statementPos = output.find(token);
        if (statementPos == std::string::npos)
            break;

        std::string::size_type startPos = output.find_first_not_of(" \t", statementPos + token.length());
        if (startPos == std::string::npos)
            break;

        std::string::size_type endPos = output.find('\n', startPos);
        if (endPos == std::string::npos)
            break;

        std::string statement(output.substr(statementPos, endPos - statementPos));
        std::string fileToInclude(trim(output.substr(startPos, endPos - startPos)));

        // load the source of the included file, and append a newline so we
        // don't break the MULTILINE macro if the last line of the include
        // file is a comment.
        std::string included_source = Stringify()
            << load_raw_source(fileToInclude, package, dbOptions)
            << "\n";

        Strings::replaceIn(output, statement, included_source);
    }    
    
    // Process any user-defined string replacements
    for (auto& r : package._replaces)
    {
        Strings::replaceIn(output, r.first, r.second);
    }

    // Run user-defined pre-processors
    ShaderPreProcessor::runPre(output);

    // Process any "#pragma define" statements
    while (true)
    {
        const std::string token("#pragma vp_define");
        std::string::size_type statementPos = output.find(token);
        if (statementPos == std::string::npos)
            break;

        std::string::size_type startPos = output.find_first_not_of(" \t", statementPos + token.length());
        if (startPos == std::string::npos)
            break;

        std::string::size_type endPos = output.find('\n', startPos);
        if (endPos == std::string::npos)
            break;

        std::string statement(output.substr(statementPos, endPos - statementPos));
        std::string varName(trim(output.substr(startPos, endPos - startPos)));

        ShaderPackage::DefineMap::const_iterator d = package._defines.find(varName);

        bool defineIt =
            d != package._defines.end() &&
            d->second == true;

        std::string newStatement = Stringify()
            << (defineIt ? "#define " : "#undef ")
            << varName;

        Strings::replaceIn(output, statement, newStatement);
    }

    return output;
}

void
ShaderLoader::split(const std::string& multisource,
                    std::vector<std::string>& output)
{
#define SPLIT_DELIM "[break]"
#define SPLIT_DELIM_LEN 7
    std::string::size_type offset = 0, pos = 0;
    while ((pos = multisource.find(SPLIT_DELIM, offset)) != std::string::npos)
    {
        std::string source = multisource.substr(offset, pos-offset);
        output.push_back(std::move(source));
        offset = pos + SPLIT_DELIM_LEN;
    }
    output.push_back(multisource.substr(offset));
}

bool
ShaderLoader::load(
    VirtualProgram*       vp,
    const std::string&    filename,
    const ShaderPackage&  package,
    const osgDB::Options* dbOptions)
{
    if ( !vp )
    {
        OE_WARN << LC << "Illegal: null VirtualProgram\n";
        return false;
    }

    // load the source string:
    std::string multisource = load(filename, package, dbOptions);
    if ( multisource.empty() )
    {
        OE_WARN << LC << "Failed to load shader source from \"" << filename << "\"\n";
        return false;
    }

    // split the multisource string into one or more shader sources:
    std::vector<std::string> sources;
    split(multisource, sources);

    for (unsigned i = 0; i < sources.size(); ++i)
    {
        std::string source = sources[i];

        // Remove the quotation marks from the source since they are illegal in GLSL
        replaceIn( source, "\"", " ");

        // Named?
        if (vp->getName().empty())
        {
            std::string name = getPragmaValue(source, "vp_name");
            vp->setName(name);
        }

        VPFunction f;
        getVPFunction(source, f);

        if (!f.entryPoint.empty())
        {
            if (f.location.isSet() == false)
                f.location = VirtualProgram::LOCATION_FRAGMENT_COLORING;

            insertStageDefine(source, getShaderTypeFromLocation(f.location.get()));

            // set the function!
            vp->setFunction(
                f.entryPoint, 
                source, 
                f.location.get(),
                f.order.get());
        }

        else // no entry point - library shader.
        {
            // install as a simple shader.
            if (f.location.isSet())
            {
                // If a location is set, install in that location only
                osg::Shader::Type type = getShaderTypeFromLocation(f.location.get());

                insertStageDefine(source, type);
                finalize(source);

                osg::Shader* shader = new osg::Shader(type, source);
                shader->setName( filename );
                vp->setShader( filename, shader );
            }

            else
            {
                // If no location was set, install in all stages.
                const osg::Shader::Type types[5] = { 
                    osg::Shader::VERTEX, 
                    osg::Shader::FRAGMENT, 
                    osg::Shader::GEOMETRY, 
                    osg::Shader::TESSCONTROL, 
                    osg::Shader::TESSEVALUATION
                };

                for(int i=0; i<5; ++i)
                {
                    std::string new_source = source;
                    insertStageDefine(new_source, types[i]);
                    finalize(new_source);
                    osg::Shader* shader = new osg::Shader(types[i], new_source);
                    std::string name = Stringify() << filename + "_" + shader->getTypename();
                    shader->setName( name );
                    vp->setShader( name, shader );
                }
            }
        }
    }

    return true;
}

bool
ShaderLoader::unload(
    VirtualProgram* vp,
    const std::string& source)
{
    ShaderPackage pkg;
    pkg.add("", source);
    return unload(vp, "", pkg, nullptr);
}

bool
ShaderLoader::unload(VirtualProgram*       vp,
                     const std::string&    filename,
                     const ShaderPackage&  package,
                     const osgDB::Options* dbOptions)
{
    if ( !vp )
    {
        // fail quietly
        return false;
    }
    
    // load the source string:
    std::string multisource = load(filename, package, dbOptions);
    if ( multisource.empty() )
    {
        OE_WARN << LC << "Failed to load shader source from \"" << filename << "\"\n";
        return false;
    }

    // split the multisource string into one or more shader sources:
    std::vector<std::string> sources;
    split(multisource, sources);

    for (unsigned i = 0; i < sources.size(); ++i)
    {
        const std::string& source = sources[i];

        VPFunction f;
        getVPFunction(source, f);

        if ( !f.entryPoint.empty() )
        {
            vp->removeShader( f.entryPoint );
        }
        else
        {
            vp->removeShader( filename );
        }
    }

    return true;
}

namespace
{
    void forEachLine(const std::string& file, std::function<bool(const std::string&)> func)
    {
        auto lines = StringTokenizer()
            .delim("\n")
            .keepEmpties(true)
            .trimTokens(false)
            .tokenize(file);

        for (auto& line : lines)
            if (func(line))
                break;
    }
}

void
ShaderLoader::configureHeader(
    std::string& in_out_source)
{
    // if there's already a #version directive, leave the entire header as-is.
    // otherwise write a new header.
    bool hasVersion = in_out_source.find("#version") != std::string::npos;

    if (!hasVersion)
    {
        in_out_source =
            ShaderFactory::getGLSLHeader() + "\n" +
            in_out_source;
    }
}

void
ShaderLoader::sort_components(
    std::string& in_out_source)
{
    GLSLChunker glsl;
    GLSLChunker::Chunks input;
    glsl.read(in_out_source, input);

    GLSLChunker::Chunks versions, extensions, pragmas, code;
    code.reserve(input.size());

    for (auto& chunk : input)
    {
        if (chunk.type == chunk.TYPE_DIRECTIVE)
        {
            OE_HARD_ASSERT(chunk.tokens.size() > 0);

            if (chunk.tokens[0] == "#version")
                versions.push_back(std::move(chunk));
            else if (chunk.tokens[0] == "#extension")
                extensions.push_back(std::move(chunk));
            else if (chunk.tokens[0] == "#pragma")
                pragmas.push_back(std::move(chunk));
            else
                code.push_back(std::move(chunk));
        }
        else
        {
            code.push_back(std::move(chunk));
        }
    }

    input.clear();

    for (auto& c : versions)
        input.push_back(std::move(c));
    for (auto& c : extensions)
        input.push_back(std::move(c));
    for (auto& c : pragmas)
        input.push_back(std::move(c));
    for (auto& c : code)
        input.push_back(std::move(c));

    glsl.write(input, in_out_source);
}

void
ShaderLoader::finalize(
    std::string& source)
{
    // Use a simple loop to replace \r with a space to avoid reallocation
    for (auto& ch : source)
    {
        if (ch == '\r')
            ch = ' ';
    }
    configureHeader(source);
}

//...................................................................

void
ShaderPackage::define(const std::string& name,
                      bool               defOrUndef)
{
    _defines[name] = defOrUndef;
}

void
ShaderPackage::replace(const std::string& pattern,
                       const std::string& value)
{
    _replaces[pattern] = value;
}

bool
ShaderPackage::load(VirtualProgram*       vp,
                    const std::string&    filename,
                    const osgDB::Options* dbOptions) const
{
    return ShaderLoader::load(vp, filename, *this, dbOptions);
}

bool
ShaderPackage::unload(VirtualProgram*       vp,
                      const std::string&    filename,
                      const osgDB::Options* dbOptions) const
{
    return ShaderLoader::unload(vp, filename, *this, dbOptions);
}

bool
ShaderPackage::loadAll(VirtualProgram*       vp,
                       const osgDB::Options* dbOptions) const
{
    int oks = 0;
    for(SourceMap::const_iterator i = _sources.begin(); i != _sources.end(); ++i)
    {
        oks += load( vp, i->first ) ? 1 : 0;
    }
    return oks == _sources.size();
}

bool
ShaderPackage::unloadAll(VirtualProgram*       vp,
                          const osgDB::Options* dbOptions) const
{
    int oks = 0;
    for(SourceMap::const_iterator i = _sources.begin(); i != _sources.end(); ++i)
    {
        oks += unload( vp, i->first ) ? 1 : 0;
    }
    return oks == _sources.size();
}

