/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/ShaderUtils>
#include <osgEarth/ShaderFactory>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/CullingUtils>
#include <osgEarth/GLSLChunker>

using namespace osgEarth;
using namespace osgEarth::Util;

//------------------------------------------------------------------------

namespace 
{
#if !defined(OSG_GL_FIXED_FUNCTION_AVAILABLE)
    static bool s_NO_FFP = true;
#else
    static bool s_NO_FFP = false;
#endif


    typedef std::list<const osg::StateSet*> StateSetStack;
    
    static const osg::Material*
    getFrontMaterial(const StateSetStack& statesetStack)
    {
        const osg::Material* base_material = NULL;
        osg::StateAttribute::GLModeValue base_val = osg::StateAttribute::ON;
        
        for(StateSetStack::const_iterator itr = statesetStack.begin();
            itr != statesetStack.end();
            ++itr)
        {
            
            osg::StateAttribute::GLModeValue val = (*itr)->getMode(GL_DIFFUSE);//?
            
            //if ( (val & osg::StateAttribute::INHERIT) == 0 )
            {
            //    if ((val & osg::StateAttribute::PROTECTED)!=0 ||
             //       (base_val & osg::StateAttribute::OVERRIDE)==0)
                {
                    base_val = val;
                    const osg::StateAttribute* materialAtt = (*itr)->getAttribute(osg::StateAttribute::MATERIAL);
                    if(materialAtt){
                        const osg::Material* asMaterial = dynamic_cast<const osg::Material*>(materialAtt);
                        if(val){
                            base_material = asMaterial;
                        }
                    }
                }
            }
            
        }
        return base_material;
    }
}

#undef LC
#define LC "[ShaderUtils] "

namespace
{
    // Code borrowed from osg::State.cpp
    bool replace(std::string& str, const std::string& original_phrase, const std::string& new_phrase)
    {
        bool replacedStr = false;
        std::string::size_type pos = 0;
        while((pos=str.find(original_phrase, pos))!=std::string::npos)
        {
            std::string::size_type endOfPhrasePos = pos+original_phrase.size();
            if (endOfPhrasePos<str.size())
            {
                char c = str[endOfPhrasePos];
                if ((c>='0' && c<='9') ||
                    (c>='a' && c<='z') ||
                    (c>='A' && c<='Z') ||
                    (c==']'))
                {
                    pos = endOfPhrasePos;
                    continue;
                }
            }

            replacedStr = true;
            str.replace(pos, original_phrase.size(), new_phrase);
        }
        return replacedStr;
    }

    bool replaceAndInsertDeclaration(std::string& source, std::string::size_type declPos, const std::string& originalStr, const std::string& newStr, const std::string& declarationPrefix, const std::string& declarationSuffix ="")
    {
        bool yes = replace(source, originalStr, newStr);
        if ( yes )
        {
            source.insert(declPos, declarationPrefix + newStr + declarationSuffix + std::string(";\n"));
        }
        return yes;
    }

    bool replaceAndInsertLiteral(std::string& source, std::string::size_type declPos, const std::string& originalStr, const std::string& newStr, const std::string& lit)
    {
        bool yes = replace(source, originalStr, newStr);
        if ( yes )
        {
            source.insert(declPos, lit);
        }
        return yes;
    }

    int replaceVarying(GLSLChunker::Chunks& chunks, int index, const StringVector& tokens, int offset, const std::string& prefix, bool isInput)
    {
        std::stringstream buf;
        if (isInput)
            buf << "#pragma vp_varying_in";
        else
            buf << "#pragma vp_varying_out";

        if ( !prefix.empty() )
            buf << " " << prefix;

        for(int i=offset; i<tokens.size(); ++i)
        {
            if ( !tokens[i].empty() )
            {
                int len = tokens[i].length();
                if ( tokens[i][len-1] == ';' )
                    buf << " " << tokens[i].substr(0, len-1); // strip semicolon
                else
                    buf << " " << tokens[i];
            }
        }
        
        chunks[index].text = buf.str();
        chunks[index].type = GLSLChunker::Chunk::TYPE_DIRECTIVE;

        std::stringstream buf2;
        for(int i=offset; i<tokens.size(); ++i)
            buf2 << (i==offset?"":" ") << tokens[i];

        GLSLChunker::Chunk newChunk;
        newChunk.type = GLSLChunker::Chunk::TYPE_STATEMENT;
        newChunk.text = buf2.str();
        chunks.insert( chunks.begin()+index, newChunk );

        return index+1;
    }

    bool replaceVaryings(osg::Shader::Type type, GLSLChunker::Chunks& chunks)
    {
        bool madeChanges = false;

        for(int i=0; i<chunks.size(); ++i)
        {
            if ( chunks[i].type == GLSLChunker::Chunk::TYPE_STATEMENT )
            {
                std::string replacement;
                const std::vector<std::string>& tokens = chunks[i].tokens;

                // Note:
                // "in"s are ignored for vertex shaders, since their ins are not varyings but
                // rather input attributes.
                // BUT, if there is a GS in the mix, and vertex shaders get moved into the GS,
                // this will fail. Figure this out someday. -gw

                if      ( tokens.size() > 1 && tokens[0] == "out" && type != osg::Shader::FRAGMENT )
                    i = replaceVarying(chunks, i, tokens, 1, "", false), madeChanges = true;
                else if ( tokens.size() > 1 && tokens[0] == "in" && type != osg::Shader::VERTEX )
                    i = replaceVarying(chunks, i, tokens, 1, "", true), madeChanges = true;
                else if ( tokens.size() > 2 && tokens[0] == "varying" && tokens[1] == "out" && type != osg::Shader::FRAGMENT )
                    i = replaceVarying(chunks, i, tokens, 2, "", false), madeChanges = true;
                else if ( tokens.size() > 2 && tokens[0] == "flat" && tokens[1] == "out" && type != osg::Shader::FRAGMENT )
                    i = replaceVarying(chunks, i, tokens, 2, "flat", false), madeChanges = true;
                else if ( tokens.size() > 2 && tokens[0] == "flat" && tokens[1] == "in" && type != osg::Shader::VERTEX )
                    i = replaceVarying(chunks, i, tokens, 2, "flat", true), madeChanges = true;
                else if ( tokens.size() > 1 && tokens[0] == "varying" )
                    i = replaceVarying(chunks, i, tokens, 1, "", true), madeChanges = true;
            }
        }

        return madeChanges;
    }

    void applySupportForNoFFPImpl(GLSLChunker::Chunks& chunks)
    {
#if !defined(OSG_GL_FIXED_FUNCTION_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE) //osg state convertVertexShaderSourceToOsgBuiltIns inserts these and the double declaration is causing an error in gles

        // for geometry and tessellation shaders, replace the built-ins with 
        // osg uniform aliases.
        const char* lines[4] = {
            "uniform mat4 osg_ModelViewMatrix;",
            "uniform mat4 osg_ProjectionMatrix;",
            "uniform mat4 osg_ModelViewProjectionMatrix;",
            "uniform mat3 osg_NormalMatrix;"
        };
    
        GLSLChunker chunker;

        for (GLSLChunker::Chunks::iterator chunk = chunks.begin(); chunk != chunks.end(); ++chunk)
        {
            if (chunk->type != GLSLChunker::Chunk::TYPE_DIRECTIVE ||
                (chunk->tokens.size()>0 && chunk->tokens[0].compare(0, 3, "#if")==0))
            {
                for (unsigned line = 0; line < 4; ++line) {
                    chunk = chunks.insert(chunk, chunker.chunkLine(lines[line]));
                    ++chunk;
                }
                break;
            }
        }

        chunker.replace(chunks, "gl_ModelViewMatrix", "osg_ModelViewMatrix");
        chunker.replace(chunks, "gl_ProjectionMatrix", "osg_ProjectionMatrix");
        chunker.replace(chunks, "gl_ModelViewProjectionMatrix", "osg_ModelViewProjectionMatrix");
        chunker.replace(chunks, "gl_NormalMatrix", "osg_NormalMatrix");
    
#endif // !defined(OSG_GL_FIXED_FUNCTION_AVAILABLE)
    }
}

#if 0
void
ShaderPreProcessor::applySupportForNoFFP(osg::Shader* shader)
{
    if (!shader)
        return;
            
#if !defined(OSG_GL_FIXED_FUNCTION_AVAILABLE)

    GLSLChunker chunker;
    GLSLChunker::Chunks chunks;
    chunker.read(shader->getShaderSource(), chunks);

    applySupportForNoFFPImpl(chunks);

    std::string output;
    chunker.write(chunks, output);
    shader->setShaderSource(output);

#endif // !defined(OSG_GL_FIXED_FUNCTION_AVAILABLE)
}
#endif

std::unordered_map<UID, ShaderPreProcessor::PreCallbackInfo> ShaderPreProcessor::_pre_callbacks;
std::unordered_map<UID, ShaderPreProcessor::PostCallbackInfo> ShaderPreProcessor::_post_callbacks;

void
ShaderPreProcessor::runPre(std::string& source)
{
    for (auto& callback : _pre_callbacks)
    {
        osg::ref_ptr<osg::Referenced> host_safe;
        callback.second.host.lock(host_safe);
        if (host_safe.valid())
            callback.second.function(source, host_safe.get());
    }
}

void
ShaderPreProcessor::runPost(osg::Shader* shader)
{
    if ( shader )
    {
        bool dirty = false;

        // Run post-callbacks
        for (auto& callback : _post_callbacks)
        {
            osg::ref_ptr<osg::Referenced> host_safe;
            callback.second.host.lock(host_safe);
            if (host_safe.valid())
                callback.second.function(shader, host_safe.get());
        }

        std::string source = shader->getShaderSource();

        // First replace any quotes with spaces. Quotes are illegal.
        if ( source.find('\"') != std::string::npos )
        {
            osgEarth::replaceIn(source, "\"", " ");
            dirty = true;
        }

        // Chunk the shader.
        GLSLChunker chunker;
        GLSLChunker::Chunks chunks;
        chunker.read( source, chunks );
        

        if (shader->getType() != osg::Shader::FRAGMENT)
        {
            //applySupportForNoFFPImpl(chunks);
        }

        // Replace varyings with directives that the ShaderFactory can interpret
        // when creating interface blocks.
        replaceVaryings( shader->getType(), chunks );
        chunker.write( chunks, source );
        shader->setShaderSource( source );

        //OE_WARN << source << std::endl << std::endl;
    }
}

//------------------------------------------------------------------------

ArrayUniform::ArrayUniform( const std::string& name, osg::Uniform::Type type, osg::StateSet* stateSet, unsigned size )
{
    attach( name, type, stateSet, size );
}

void
ArrayUniform::attach( const std::string& name, osg::Uniform::Type type, osg::StateSet* stateSet, unsigned size )
{
    _uniform    = stateSet->getUniform( name );
    _uniformAlt = stateSet->getUniform( name + "[0]" );

    if ( !isValid() )
    {
        _uniform    = new osg::Uniform( type, name, size );
        _uniformAlt = new osg::Uniform( type, name + "[0]", size );
        stateSet->addUniform( _uniform.get() );
        stateSet->addUniform( _uniformAlt.get() );
    }

    _stateSet = stateSet;
}

void 
ArrayUniform::setElement( unsigned index, int value )
{
    if ( isValid() )
    {
        ensureCapacity( index+1 );
        _uniform->setElement( index, value );
        _uniformAlt->setElement( index, value );
    }
}

void 
ArrayUniform::setElement( unsigned index, unsigned value )
{
    if ( isValid() )
    {
        ensureCapacity( index+1 );
        _uniform->setElement( index, value );
        _uniformAlt->setElement( index, value );
    }
}

void 
ArrayUniform::setElement( unsigned index, bool value )
{
    if ( isValid() )
    {
        ensureCapacity( index+1 );
        _uniform->setElement( index, value );
        _uniformAlt->setElement( index, value );
    }
}

void 
ArrayUniform::setElement( unsigned index, float value )
{
    if ( isValid() )
    {
        ensureCapacity( index+1 );
        _uniform->setElement( index, value );
        _uniformAlt->setElement( index, value );
    }
}

void
ArrayUniform::setElement( unsigned index, const osg::Matrixf& value )
{
    if ( isValid() )
    {
        ensureCapacity( index+1 );
        _uniform->setElement( index, value );
        _uniformAlt->setElement( index, value );
    }
}

void
ArrayUniform::setElement( unsigned index, const osg::Vec3f& value )
{
    if ( isValid() )
    {
        ensureCapacity( index+1 );
        _uniform->setElement( index, value );
        _uniformAlt->setElement( index, value );
    }
}

void
ArrayUniform::setElement(unsigned index, const osg::Vec4f& value)
{
    if (isValid())
    {
        ensureCapacity(index + 1);
        _uniform->setElement(index, value);
        _uniformAlt->setElement(index, value);
    }
}

bool 
ArrayUniform::getElement( unsigned index, int& out_value ) const
{
    return isValid() ? _uniform->getElement( index, out_value ) : false;
}

bool 
ArrayUniform::getElement( unsigned index, unsigned& out_value ) const
{
    return isValid() ? _uniform->getElement( index, out_value ) : false;
}

bool 
ArrayUniform::getElement( unsigned index, bool& out_value ) const
{
    return isValid() ? _uniform->getElement( index, out_value ) : false;
}

bool 
ArrayUniform::getElement( unsigned index, float& out_value ) const
{
    return isValid() ? _uniform->getElement( index, out_value ) : false;
}

bool 
ArrayUniform::getElement( unsigned index, osg::Matrixf& out_value ) const
{
    return isValid() ? _uniform->getElement( index, out_value ) : false;
}

bool 
ArrayUniform::getElement( unsigned index, osg::Vec3f& out_value ) const
{
    return isValid() ? _uniform->getElement( index, out_value ) : false;
}

bool
ArrayUniform::getElement(unsigned index, osg::Vec4f& out_value) const
{
    return isValid() ? _uniform->getElement(index, out_value) : false;
}

namespace
{
    template<typename T>
    void copyElements(osg::Uniform* src, ArrayUniform* dest) {
        for (unsigned i = 0; i < src->getNumElements(); ++i) {
            T temp;
            src->getElement(i, temp);
            dest->setElement(i, temp);
        }
    };
}

void
ArrayUniform::ensureCapacity( unsigned newSize )
{
    if ( isValid() && _uniform->getNumElements() < newSize )
    {
        osg::ref_ptr<osg::StateSet> stateSet_safe = _stateSet.get();
        if ( stateSet_safe.valid() )
        {
            osg::ref_ptr<osg::Uniform> oldUniform = _uniform.get();
            osg::ref_ptr<osg::Uniform> oldUniformAlt = _uniformAlt.get();

            stateSet_safe->removeUniform( _uniform->getName() );
            stateSet_safe->removeUniform( _uniformAlt->getName() );

            _uniform    = new osg::Uniform( _uniform->getType(), _uniform->getName(), newSize );
            _uniformAlt = new osg::Uniform( _uniform->getType(), _uniform->getName() + "[0]", newSize );

            switch (oldUniform->getType())
            {
            case osg::Uniform::FLOAT:
                copyElements<float>(oldUniform.get(), this); break;
            case osg::Uniform::INT:
                copyElements<int>(oldUniform.get(), this); break;
            case osg::Uniform::UNSIGNED_INT:
                copyElements<unsigned>(oldUniform.get(), this); break;
            case osg::Uniform::FLOAT_VEC3:
                copyElements<osg::Vec3f>(oldUniform.get(), this); break;
            case osg::Uniform::FLOAT_VEC4:
                copyElements<osg::Vec4f>(oldUniform.get(), this); break;
            case osg::Uniform::FLOAT_MAT4:
                copyElements<osg::Matrixf>(oldUniform.get(), this); break;
            case osg::Uniform::BOOL:
                copyElements<bool>(oldUniform.get(), this); break;
            };

            stateSet_safe->addUniform( _uniform.get() );
            stateSet_safe->addUniform( _uniformAlt.get() );

            stateSet_safe.release(); // don't want to unref delete
        }
    }
}

void
ArrayUniform::detach()
{
    if ( isValid() )
    {
        osg::ref_ptr<osg::StateSet> stateSet_safe = _stateSet.get();
        if ( stateSet_safe.valid() )
        {
            stateSet_safe->removeUniform( _uniform->getName() );
            stateSet_safe->removeUniform( _uniformAlt->getName() );

            _uniform = 0L;
            _uniformAlt = 0L;
            _stateSet = 0L;

            stateSet_safe.release(); // don't want to unref delete
        }
    }
}

//------------------------------------------------------------------------

void
DiscardAlphaFragments::install(osg::StateSet* ss, float minAlpha) const
{
    if ( ss && minAlpha < 1.0f && Registry::capabilities().supportsGLSL() )
    {
        VirtualProgram* vp = VirtualProgram::getOrCreate(ss);
        if ( vp )
        {
            vp->setName("Discard Alpha");

            std::string code = Stringify()
                << "void oe_discardalpha_frag(inout vec4 color) { \n"
                << "    if ( color.a < " << std::fixed << std::setprecision(4) << minAlpha << ") discard;\n"
                << "} \n";

            vp->setFunction(
                "oe_discardalpha_frag",
                code,
                VirtualProgram::LOCATION_FRAGMENT_COLORING,
                0L, 0.95f);
        }
    }
}
 
void
DiscardAlphaFragments::uninstall(osg::StateSet* ss) const
{
    if ( ss )
    {
        VirtualProgram* vp = VirtualProgram::get(ss);
        if ( vp )
        {
            vp->removeShader("oe_discardalpha_frag");
        }
    }
}


namespace
{
    const char* fs =
        "void oe_default_fs(inout vec4 color) { } \n";
}

void
ShaderUtils::installDefaultShader(osg::StateSet* ss)
{
    VirtualProgram* vp = VirtualProgram::getOrCreate(ss);
    vp->setFunction("oe_default_fs", fs, VirtualProgram::LOCATION_FRAGMENT_COLORING, 0.0);
}


ShaderInfoLog::ShaderInfoLog(
    const osg::Program* program,
    const std::string& log) :
    _program(program),
    _log(log)
{

}

namespace
{
    void parse_glsl_message(
        const std::string& input,
        const std::vector<std::string>& source,
        //        const std::string& source,
        unsigned& out_lineno,
        bool& out_isError,
        std::string& out_message)
    {
        // example:
        // 9602(22) : error C1121: index: function declaration in non global scope not allowed
        // filename(lineno) : type code : message

        // parse the message:
        int filename;
        int lineno;
        char type[16];
        char code[16];
        char text[1024];

        sscanf(input.c_str(), "%d(%d) : %s %s : %s",
            &filename,
            &lineno,
            type,
            code,
            text);

        // if the filename is not 0, find the corresponding #line directive
        // and count from there.
        if (filename != 0)
        {
            bool done = false;
            for (unsigned n = 0; n < source.size() && !done; ++n)
            {
                std::string line(Strings::trim(source[n]));
                if (Strings::startsWith(line, "#line"))
                {
                    int sub_lineno, sub_filename;
                    sscanf(line.c_str(), "#line %d %d", &sub_lineno, &sub_filename);
                    if (sub_filename == filename)
                    {
                        lineno = n - sub_lineno + lineno;
                        done = true;
                    }
                }
            }
        }

        out_lineno = lineno;
        out_isError = (std::string(type) == "error");
        out_message = text;
    }
}

void
ShaderInfoLog::dumpErrors(
    osg::State& state) const
{
    bool done = false;
    for (auto i = 0u; i < _program->getNumShaders() && !done; ++i)
    {
        auto shader = _program->getShader(i);
        auto pshader = shader->getPCS(state);
        std::string log;
        pshader->getInfoLog(log);

        // split into lines:
        auto errors = StringTokenizer()
            .delim("\n")
            .keepEmpties(false)
            .tokenize(log);

        // split into lines:
        auto lines = StringTokenizer()
            .delim("\n")
            .keepEmpties(false)
            .trimTokens(false)
            .tokenize(shader->getShaderSource());

        // keep track of same lines (in order)
        std::stringstream buf;
        for (int e = 0; e < errors.size() && !done; ++e)
        {
            unsigned lineno;
            bool isError;
            std::string text;

            parse_glsl_message(errors[e], lines, lineno, isError, text);
            if (isError)
            {
                int start = 0; // std::max(0, n - 3);
                int end = lines.size(); // std::min((int)lines.size(), n + 7);
                for (int k = start; k < end; ++k)
                {
                    std::string star = (k == lineno) ? "**> " : ":   ";
                    buf << k << star << lines[k] << std::endl;
                }

                // just print the first error.
                done = true;
            }
        }
        std::string msg = buf.str();
        if (!msg.empty())
        {
            OE_WARN << "SHADER " << shader->getName() << " infolog errors: " << std::endl
                << msg << std::endl;
        }
    }
}
