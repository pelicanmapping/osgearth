/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "ShaderFactory"
#include "ShaderLoader"
#include "ShaderUtils"
#include "Capabilities"
#include "GLUtils"
#include "Threading"

#include <sstream>

#define LC "[ShaderFactory] "

using namespace osgEarth;
using namespace osgEarth::ShaderComp;
using namespace osgEarth::Util;

namespace
{
    std::mutex s_glslMutex;
    std::string s_glslHeader;

    #if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE)
        bool s_GLES_SHADERS = true;
    #else
        bool s_GLES_SHADERS = false;
    #endif

    // global toggle for varying packing
    bool s_packingEnabled = true;
}

#define INDENT "    "

ShaderFactory::ShaderFactory()
{
    const char* s = ::getenv("OSGEARTH_DISABLE_VARYING_PACKING");
    if (s)
        s_packingEnabled = false;
}

std::string
ShaderFactory::getGLSLHeader()
{
    if (s_glslHeader.empty())
    {
        std::lock_guard<std::mutex> lock(s_glslMutex);
        if (s_glslHeader.empty())
        {
            int version = Capabilities::get().getGLSLVersionInt();

            std::ostringstream buf;
            buf << "#version " << version;

#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE)
            buf << "\nprecision highp float";
#else
            if (GLUtils::useNVGL())
            {
                buf << "\n#extension GL_NV_gpu_shader5 : enable";
            }
            else if (version >= 130)
            {
                if (Capabilities::get().supportsInt64())
                {
                    buf << "\n#extension GL_ARB_gpu_shader_int64 : enable";
                }

                //buf << "\n#extension GL_ARB_gpu_shader4 : enable";
            }
#endif

            s_glslHeader = buf.str();
        }
    }
    return s_glslHeader;
}

void
ShaderFactory::clearProcessorCallbacks()
{
    ShaderPreProcessor::_pre_callbacks.clear();
    ShaderPreProcessor::_post_callbacks.clear();
}

UID
ShaderFactory::addPreProcessorCallback(
    osg::Referenced* host,
    std::function<void(std::string&, osg::Referenced*)> cb)
{  
    UID uid = osgEarth::createUID();
    ShaderPreProcessor::_pre_callbacks[uid] = { host, cb };
    return uid;
}

void
ShaderFactory::removePreProcessorCallback(UID uid)
{
    ShaderPreProcessor::_pre_callbacks.erase(uid);
}

UID
ShaderFactory::addPostProcessorCallback(
    osg::Referenced* host,
    std::function<void(osg::Shader*, osg::Referenced*)> cb)
{
    UID uid = osgEarth::createUID();
    ShaderPreProcessor::_post_callbacks[uid] = { host, cb };
    return uid;
}

void
ShaderFactory::removePostProcessorCallback(UID uid)
{
    ShaderPreProcessor::_post_callbacks.erase(uid);
}


#define SPACE_MODEL 0
#define SPACE_VIEW  1
#define SPACE_CLIP  2

namespace
{
    enum class ComponentType : uint8_t { Float, Int, UInt, Double, Bool, Unknown = 99 };

    struct Variable
    {
        std::string interp;      // interpolation qualifer (flat, etc.)
        std::string type;        // float, vec4, etc.
        ComponentType componentType; // float, int, uint etc
        std::string name;        // name without any array specifiers, etc.
        std::string prec;        // precision qualifier if any
        std::string declaration; // name including array specifiers (for decl)
        int         arraySize;   // 0 if not an array; else array size.

        // Assigned by packer:
        int location = -1;
        std::string packedName;        // e.g. "packed6.xy" OR "oe_tile_key" for direct
    };

    typedef std::vector<Variable> Variables;

    struct PackedSlot
    {
        ComponentType  scalar;
        std::string interp;
        std::string prec;
        int         location;

        bool operator==(const PackedSlot& o) const {
            return scalar == o.scalar && location == o.location && interp == o.interp && prec == o.prec;
        }
    };

    class VaryingPacker
    {
    public:
        // Unique packed slots that must be declared
        std::vector<PackedSlot> packedSlots;

        void pack(std::vector<Variable>& vars)
        {
            packedSlots.clear();

            // get the pack width of a variable, we only return valid value
            // for single scalars and vec2 types as they are all we pack for now.
            auto packWidth = [&](const Variable& v) -> int {
                if (!s_packingEnabled) return -1;
                if (v.arraySize > 0) return -1;
                const std::string & t = v.type;
                if (t == "float" || t == "int" || t == "uint") return 1;
                if (t == "vec2" || t == "ivec2" || t == "uvec2") return 2;
                return -1;
            };

            // how many full locations (vec4s) does a variables type consume
            auto locationsPerElement = [&](const Variable& v) -> int {
                const std::string & t = v.type;
                if (t == "mat4" || t == "dmat4") return 4;
                if (t == "mat3" || t == "dmat3") return 3;
                return 1;
            };

            // the swizzle string for a packed var e.g. if we were to pack
            // a vec2 at component 1 of the packed vec4 it's swizzle would
            // be .yz
            auto swizzle = [&](int comp, int width) -> std::string {
                static const char* lanes = "xyzw";
                std::string s = ".";
                for (int i = 0; i < width; ++i) s.push_back(lanes[comp + i]);
                return s;
            };

            // group indices by (ComponentType, interp, prec)
            struct GroupKey {
                ComponentType scalar { ComponentType::Unknown };
                std::string interp;
                std::string prec;

                bool operator==(const GroupKey& o) const {
                    return scalar == o.scalar && interp == o.interp && prec == o.prec;
                }

                bool operator<(GroupKey const& o) const {
                    if (scalar != o.scalar) return static_cast<int>(scalar) < static_cast<int>(o.scalar);
                    if (interp != o.interp) return interp < o.interp;
                    return prec < o.prec;
                }
            };

            // add the vars to the groups
            std::map<GroupKey, std::vector<int>> groups;
            for (int i = 0; i < (int)vars.size(); ++i) {
                groups[{vars[i].componentType, vars[i].interp, vars[i].prec}].push_back(i);
             }

             // pack each group independently; global location increment across groups
             int nextLocation = 0;

             for (auto const& kv : groups) {
                 GroupKey const& key = kv.first;
                 std::vector<int> const& idxs = kv.second;

                 int packLocation = -1;
                 int packComponent = 0;

                 auto resetPacking = [&]() { packLocation = -1; packComponent = 0; };

                 // Ensure we record slot declaration once
                 auto addPackedSlot = [&](int loc) {
                     PackedSlot s;
                     s.scalar = key.scalar;
                     s.interp = key.interp;
                     s.prec = key.prec;
                     s.location = loc;

                     // unique insert
                     for (size_t i = 0; i < packedSlots.size(); ++i)
                     if (packedSlots[i] == s) return;
                     packedSlots.push_back(s);
                 };

                 for (size_t ii = 0; ii < idxs.size(); ++ii)
                 {
                     Variable& v = vars[idxs[ii]];

                     int width = packWidth(v);
                     if (width > 0)
                     {
                        // Pack into a packed slot, like old float/vec2 packer
                        if (packLocation < 0 || packComponent + width > 4)
                        {
                           packLocation = nextLocation++;
                           packComponent = 0;
                           addPackedSlot(packLocation);
                        }

                        v.location = packLocation;
                        v.packedName = "packed" + std::to_string(packLocation) + swizzle(packComponent, width);

                        packComponent += width;
                     }
                     else
                     {
                         // Non-packable: matrices, vec3+, arrays, etc.
                         resetPacking();

                         v.location = nextLocation;
                         v.packedName = v.name; // your rule: just keep the original name

                         int locs = locationsPerElement(v) * std::max(1, v.arraySize);
                         nextLocation += locs;
                     }
                 }
             }

             // Sort packed slots for pretty emission
             std::sort(packedSlots.begin(), packedSlots.end(),
                 [&](const PackedSlot& a, const PackedSlot& b) {
                     if (a.scalar != b.scalar) return a.scalar < b.scalar;
                     if (a.interp != b.interp) return a.interp < b.interp;
                     if (a.prec != b.prec)  return a.prec < b.prec;
                     return a.location < b.location;
             });
        }

        std::string emitInterfaceBlock(const std::vector<Variable>& vars,
                                       const char* blockName,
                                       const char* indent = "    ") const
        {
            auto slotTypeName = [&](ComponentType k) -> const char* {
                switch (k) {
                    case ComponentType::Float:  return "vec4";
                    case ComponentType::Int:    return "ivec4";
                    case ComponentType::UInt:   return "uvec4";
                    case ComponentType::Bool:   return "bvec4";
                    case ComponentType::Double: return "dvec4";
                    default:                 return "vec4";
                }
            };

            std::stringstream buf;
            buf << blockName << " {\n";

            // 1) Declare packed slots first
            for (size_t i = 0; i < packedSlots.size(); ++i) {
                const PackedSlot& s = packedSlots[i];
                buf << indent;
                if (!s.interp.empty()) buf << s.interp << " ";
                if (!s.prec.empty())   buf << s.prec << " ";
                buf << slotTypeName(s.scalar) << " packed" << s.location << ";\n";
            }

            // 2) Declare non-packed originals normally
            for (size_t i = 0; i < vars.size(); ++i) {
                const Variable& v = vars[i];

                // If packedName starts with "packed", we skip declaring the original
                // because the slot declaration covers it.
                if (v.packedName.size() >= 6 && v.packedName.compare(0, 6, "packed") == 0)
                    continue;

                buf << indent;
                if (!v.interp.empty()) buf << v.interp << " ";
                if (!v.prec.empty())   buf << v.prec << " ";
                buf << v.declaration << ";\n";
            }

            buf << "}";
            return buf.str();
        }
    };


	void addExtensionsToBuffer(
        std::ostream& buf, 
        const VirtualProgram::ExtensionsSet& extensions)
	{
	   for (auto& extension : extensions)
       {
           buf << "#extension " << extension << " : enable\n";
       }
    }
}


VirtualProgram::StageMask
ShaderFactory::createMains(
    osg::State& state,
    const VirtualProgram::FunctionLocationMap& functions,
    const VirtualProgram::ShaderMap& in_shaders,
    const VirtualProgram::ExtensionsSet& in_extensions,
    std::vector< osg::ref_ptr<osg::Shader> >& out_shaders) const
{
    // We require attribute aliasing and matrix uniforms.
    OE_SOFT_ASSERT(state.getUseVertexAttributeAliasing(),
        "OpenSceneGraph vertex attribute aliasing must be enabled. Consider installing the GL3RealizeOperation in your viewer.");

    OE_SOFT_ASSERT(state.getUseModelViewAndProjectionUniforms(),
        "OpenSceneGraph matrix uniforms must be enabled. Consider installing the GL3RealizeOperation in your viewer.");

    StageMask stages =
        VirtualProgram::STAGE_VERTEX |
        VirtualProgram::STAGE_FRAGMENT;

    FunctionLocationMap::const_iterator f;

    f = functions.find(VirtualProgram::LOCATION_VERTEX_TRANSFORM_MODEL_TO_VIEW);
    const OrderedFunctionMap* xformModelToView = f != functions.end() ? &f->second : nullptr;

    // collect the "model" stage vertex functions:
    f = functions.find(VirtualProgram::LOCATION_VERTEX_MODEL );
    const OrderedFunctionMap* modelStage = f != functions.end() ? &f->second : 0L;

    // collect the "view" stage vertex functions:
    f = functions.find(VirtualProgram::LOCATION_VERTEX_VIEW );
    const OrderedFunctionMap* viewStage = f != functions.end() ? &f->second : 0L;

    // geometry shader functions:
    f = functions.find(VirtualProgram::LOCATION_TESS_CONTROL );
    const OrderedFunctionMap* tessControlStage = f != functions.end() ? &f->second : 0L;

    // geometry shader functions:
    f = functions.find(VirtualProgram::LOCATION_TESS_EVALUATION );
    const OrderedFunctionMap* tessEvalStage = f != functions.end() ? &f->second : 0L;

    // geometry shader functions:
    f = functions.find(VirtualProgram::LOCATION_GEOMETRY );
    const OrderedFunctionMap* geomStage = f != functions.end() ? &f->second : 0L;

    // collect the "clip" stage functions:
    f = functions.find(VirtualProgram::LOCATION_VERTEX_CLIP );
    const OrderedFunctionMap* clipStage = f != functions.end() ? &f->second : 0L;

    // fragment shader coloring functions:
    f = functions.find(VirtualProgram::LOCATION_FRAGMENT_COLORING );
    const OrderedFunctionMap* coloringStage = f != functions.end() ? &f->second : 0L;

    // fragment shader lighting functions:
    f = functions.find(VirtualProgram::LOCATION_FRAGMENT_LIGHTING );
    const OrderedFunctionMap* lightingStage = f != functions.end() ? &f->second : 0L;

    // fragment shader lighting functions:
    f = functions.find(VirtualProgram::LOCATION_FRAGMENT_OUTPUT );
    const OrderedFunctionMap* outputStage = f != functions.end() ? &f->second : 0L;

    // what do we need to build?
    bool hasGS  = geomStage        && !geomStage->empty();
    bool hasTCS = tessControlStage && !tessControlStage->empty();
    bool hasTES = tessEvalStage    && !tessEvalStage->empty();
    bool hasFS  = true;
    bool hasVS  = true;

    // where to insert the view/clip stage vertex functions:
    bool viewStageInGS  = hasGS;
    bool viewStageInTES = !viewStageInGS && hasTES;
    bool viewStageInVS  = !viewStageInTES && !viewStageInGS;

    bool clipStageInGS  = hasGS;
    bool clipStageInTES = hasTES && !hasGS;
    bool clipStageInVS  = !clipStageInGS && !clipStageInTES;

    // search for pragma varyings and build up our interface block definitions.
    typedef std::set<std::string> VarDefs;
    VarDefs varDefs;

    // built-ins:
    varDefs.insert("vec4 vp_Color");
    varDefs.insert("vec3 vp_Normal");
    varDefs.insert("vec4 vp_Vertex");
    varDefs.insert("vec3 vp_VertexView");

    // parse the vp_varyings (which were injected by the ShaderLoader)
    // We actually only care about the in's. Because any varying that 
    // doesn't have an "in" somewhere in the shader list is not actually
    // being used as a varying, in which case we don't need it in the
    // interface block.
    // NOTE: The above statement is true IFF we don't move any vertex shaders
    // to the TCS, TES, or GS phase. So we need to check that.

    for(VirtualProgram::ShaderMap::const_iterator s = in_shaders.begin(); s != in_shaders.end(); ++s )
    {
        osg::Shader* shader = s->second._shader->getNominalShader();
        if ( shader )
        {
            ShaderLoader::getAllPragmaValues(shader->getShaderSource(), "vp_varying_in", varDefs);

            // Like I said above, if the possibility exists of shunting vertex shaders to
            // a different stage, we need the outs after all.
            if (hasTCS || hasTES || hasGS)
            {
                ShaderLoader::getAllPragmaValues(shader->getShaderSource(), "vp_varying_out", varDefs);
            }
        }
    }

    Variables vars;
    for(VarDefs::iterator i = varDefs.begin(); i != varDefs.end(); ++i) 
    {
        auto tokens = StringTokenizer()
            .delim(" ", false)
            .delim("\t", false)
            .delim("[", true)
            .delim("]", true)
            .standardQuotes()
            .tokenize(*i);

        if ( tokens.size() >= 2 )
        {
            int p=0;
            Variable v;
            if ( tokens[p] == "flat" || tokens[p] == "nonperspective" || tokens[p] == "smooth" )
            {
                v.interp = tokens[p++];
            }

            if ( tokens[p] == "lowp" || tokens[p] == "mediump" || tokens[p] == "highp" )
            {
                v.prec = tokens[p++];
            }

            if ( p+1 < tokens.size() )
            {
                v.type = tokens[p++];
                v.name = tokens[p++];

                // check for array
                if ( p+2 < tokens.size() && tokens[p] == "[" && tokens[p+2] == "]" )
                {
                    v.declaration = Stringify() << v.type << " " << v.name << tokens[p] << tokens[p+1] << tokens[p+2];
                    v.arraySize = as<int>(tokens[p+1], 0);
                }
                else
                {
                    v.declaration = Stringify() << v.type << " " << v.name;
                    v.arraySize = 0;
                }

                const std::string & t = v.type;
                if (t == "float" || t.rfind("vec", 0) == 0 || t.rfind("mat", 0) == 0) v.componentType = ComponentType::Float;
                else if (t == "int" || t.rfind("ivec", 0) == 0) v.componentType = ComponentType::Int;
                else if (t == "uint" || t.rfind("uvec", 0) == 0) v.componentType = ComponentType::UInt;
                else if (t == "double" || t.rfind("dvec", 0) == 0 || t.rfind("dmat", 0) == 0) v.componentType = ComponentType::Double;
                else if (t == "bool" || t.rfind("bvec", 0) == 0) v.componentType = ComponentType::Bool;
                else v.componentType = ComponentType::Float;

            }

            if ( !v.type.empty() && !v.name.empty() && !v.declaration.empty() )
            {
                vars.push_back( v );
            }
        }
    }

    const std::string
        gl_Color                     = "gl_Color",
        gl_Vertex                    = "gl_Vertex",
        gl_Normal                    = "gl_Normal",
        gl_Position                  = "gl_Position",
        gl_ModelViewMatrix           = "gl_ModelViewMatrix",
        gl_ProjectionMatrix          = "gl_ProjectionMatrix",
        gl_ModelViewProjectionMatrix = "gl_ModelViewProjectionMatrix",
        gl_NormalMatrix              = "gl_NormalMatrix",
        gl_FrontColor                = "gl_FrontColor";

    std::string glMatrixUniforms = "";

    // build the vertex data interface block definition:
    VaryingPacker packer;
    packer.pack(vars);

    std::string vertdata = packer.emitInterfaceBlock(vars, "VP_PerVertex");

    // TODO: perhaps optimize later to not include things we don't need in the FS
    std::string fragdata = vertdata;

    // Build the vertex shader.
    if ( hasVS )
    {
        stages |= VirtualProgram::STAGE_VERTEX;

        std::stringstream buf;

        buf << getGLSLHeader() << "\n"
            "#pragma vp_name VP Vertex Shader Main \n";

        addExtensionsToBuffer(buf, in_extensions);

        buf << "\n// Vertex stage globals:\n";
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << i->prec << (i->prec.empty()?"":" ") << i->declaration << "; \n";
        
        buf << "\n// Vertex stage outputs:\n";
        if ( hasGS || hasTCS )
            buf << "out " << vertdata << " vp_out; \n";
        else
            buf << "out " << fragdata << " vp_out; \n";

        // prototype functions:
        if ( modelStage || (viewStage && viewStageInVS) || (clipStage && clipStageInVS) )
        {
            buf << "\n// Function declarations:\n";
        }

        if ( modelStage )
        {
            for( OrderedFunctionMap::const_iterator i = modelStage->begin(); i != modelStage->end(); ++i )
            {
                buf << "void " << i->second._name << "(inout vec4); \n";
            }
        }

        if (viewStageInVS)
        {
            if (xformModelToView)
            {
                for (auto& iter : *xformModelToView)
                    buf << "void " << iter.second._name << "();\n";
            }

            // prototypes for view stage methods:
            if (viewStage)
            {
                for (OrderedFunctionMap::const_iterator i = viewStage->begin(); i != viewStage->end(); ++i)
                {
                    buf << "void " << i->second._name << "(inout vec4); \n";
                }
            }
        }

        if (clipStageInVS)
        {
            if (xformModelToView)
            {
                for (auto& iter : *xformModelToView)
                    buf << "void " << iter.second._name << "();\n";
            }

            // prototypes for clip stage methods:
            if (clipStage != 0L)
            {
                for (OrderedFunctionMap::const_iterator i = clipStage->begin(); i != clipStage->end(); ++i)
                {
                    buf << "void " << i->second._name << "(inout vec4); \n";
                }
            }
        }

        buf <<
            "\nvoid main(void) \n"
            "{ \n"
            INDENT "vp_Vertex = " << gl_Vertex << "; \n"
            INDENT "vp_Normal = " << gl_Normal << "; \n"
            INDENT "vp_Color  = " << gl_Color  << "; \n";

        if ( modelStage )
        {
            for( OrderedFunctionMap::const_iterator i = modelStage->begin(); i != modelStage->end(); ++i )
            {
                buf << INDENT << i->second._name << "(vp_Vertex); \n";
            }
        }

        if ( viewStageInVS )
        {
            if ( viewStage )
            {
                if (xformModelToView)
                {
                    buf <<
                        INDENT << xformModelToView->begin()->second._name << "();\n";
                }
                else
                {
                    buf <<
                        INDENT << "vp_Vertex = " << gl_ModelViewMatrix << " * vp_Vertex; \n"
                        INDENT << "vp_Normal = normalize(" << gl_NormalMatrix << " * vp_Normal); \n";
                }

                buf << INDENT << "vp_VertexView = vp_Vertex.xyz;\n";

                for( OrderedFunctionMap::const_iterator i = viewStage->begin(); i != viewStage->end(); ++i )
                {
                    buf << INDENT << i->second._name << "(vp_Vertex); \n";
                }
            }

            if ( clipStageInVS )
            {
                if ( clipStage )
                {
                    if ( viewStage )
                    {
                        buf << INDENT << "vp_Vertex = " << gl_ProjectionMatrix << " * vp_Vertex; \n";
                    }
                    else
                    {
                        if (xformModelToView)
                        {
                            buf <<
                                INDENT << xformModelToView->begin()->second._name << "();\n"
                                INDENT << "vp_VertexView = vp_Vertex.xyz;\n"
                                INDENT << "vp_Vertex = " << gl_ProjectionMatrix << " * vp_Vertex;\n";
                        }
                        else
                        {
                            buf <<
                                INDENT << "vp_VertexView = (" << gl_ModelViewMatrix << " * vp_Vertex).xyz; \n"
                                INDENT << "vp_Vertex = " << gl_ModelViewProjectionMatrix << " * vp_Vertex; \n"
                                INDENT << "vp_Normal = normalize(" << gl_NormalMatrix << " * vp_Normal); \n";
                        }
                    }

                    for( OrderedFunctionMap::const_iterator i = clipStage->begin(); i != clipStage->end(); ++i )
                    {
                        buf << INDENT << i->second._name << "(vp_Vertex); \n";
                    }
                }
            }
        }

        // if there are no further vertex-processing stages, transform the position into clip coordinates
        // for the fragment shader now:
        if ( !hasGS && !hasTCS )
        {
            if ( clipStage )
                buf << INDENT "gl_Position = vp_Vertex; \n";
            else if ( viewStage )
                buf << INDENT "gl_Position = " << gl_ProjectionMatrix << " * vp_Vertex; \n";
            else // modelstage
                buf << INDENT "gl_Position = " << gl_ModelViewProjectionMatrix << " * vp_Vertex; \n";
        }

        // otherwise, pass it along as-is.
        else
        {
            buf << INDENT "gl_Position = vp_Vertex; \n";
        }


        if ( hasTCS || hasGS || hasFS )
        {
            // Copy stage globals to output block:
            for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
                buf << INDENT << "vp_out." << i->packedName << " = " << i->name << "; \n";
        }

        buf << "} \n";

        osg::Shader* vertexShader = new osg::Shader( osg::Shader::VERTEX, buf.str() );
        vertexShader->setName( "main(vertex)" );
        out_shaders.push_back( vertexShader );
    }


    //.................................................................................

    if ( hasTCS )
    {
        stages |= VirtualProgram::STAGE_TESSCONTROL;
        std::stringstream buf;

        buf << getGLSLHeader() << "\n"
            << "#pragma vp_name VP Tessellation Control Shader (TCS) Main\n";

        addExtensionsToBuffer(buf, in_extensions);

        buf << glMatrixUniforms << "\n";

        if ( hasVS )
        {
              buf << "\n// TCS stage inputs:\n"
                 << "in " << vertdata << " vp_in [gl_MaxPatchVertices]; \n";
        }

        // The TES is mandatory.
        buf << "\n// TCS stage outputs to TES: \n"
            << "out " << vertdata << " vp_out [gl_MaxPatchVertices]; \n";

        // Stage globals.
        buf << "\n// TCS stage globals \n";
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << i->prec << (i->prec.empty()?"":" ") << i->declaration << "; \n";

        // Helper functions:
        // TODO: move this into its own osg::Shader so it can be shared.
        buf << "\nvoid VP_LoadVertex(in int index) \n"
            << "{ \n";
        
        // Copy input block to stage globals:
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << INDENT << i->name << " = vp_in[index]." << i->packedName << "; \n";

        buf << "} \n";
        
        // Function declares
        if ( tessControlStage )
        {
            buf << "\n// Function declarations:\n";
            for( OrderedFunctionMap::const_iterator i = tessControlStage->begin(); i != tessControlStage->end(); ++i )
                buf << "void " << i->second._name << "(); \n";
        }

        // Main
        buf << "\nvoid main(void) \n"
            << "{ \n"
            << INDENT "// copy default outputs: \n";
                
        // Copy in to globals
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << INDENT << i->name << " = vp_in[gl_InvocationID]." << i->packedName << "; \n";

        // Invoke functions
        if ( tessControlStage )
        {
            for( OrderedFunctionMap::const_iterator i = tessControlStage->begin(); i != tessControlStage->end(); ++i )
                buf << INDENT << i->second._name << "(); \n";
        }
                
        // Copy globals to out.
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << INDENT << "vp_out[gl_InvocationID]." << i->packedName << " = " << i->name << "; \n";

        buf << "} \n";
        
        std::string str = buf.str();
        osg::Shader* tcsShader = new osg::Shader(osg::Shader::TESSCONTROL, str);
        tcsShader->setName("VP TCS");
        out_shaders.push_back( tcsShader );
    }


    //.................................................................................


    if ( hasTES )
    {
        stages |= VirtualProgram::STAGE_TESSEVALUATION;

        std::stringstream buf;

        buf << getGLSLHeader() << "\n"
            << "#pragma vp_name VP Tessellation Evaluation (TES) Shader MAIN\n";

        addExtensionsToBuffer(buf, in_extensions);

        buf << glMatrixUniforms << "\n";

        buf << "\n// TES stage inputs (required):\n"
            << "in " << vertdata << " vp_in []; \n";
        
        // Declare stage globals.
        buf << "\n// TES stage globals: \n";
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << i->declaration << "; \n";
        
        buf << "\n// TES stage outputs: \n";
        if ( hasGS )
            buf << "out " << vertdata << " vp_out; \n";
        else
            buf << "out " << fragdata << " vp_out; \n";

        std::set<std::string> types;
        for(Variables::const_iterator i=vars.begin(); i != vars.end(); ++i)
            types.insert(i->type);

        for(std::set<std::string>::const_iterator i = types.begin(); i != types.end(); ++i)
        {
            buf << *i << " VP_Interpolate3(" << *i << "," << *i << "," << *i << ");\n";
        }

#if 0
        buf <<
            "\n// TES user-supplied interpolators: \n"
            "float VP_Interpolate16(float,float,float,float,float,float,float,float,float,float,float,float,float,float,float,float); \n"
            "vec2  VP_Interpolate16(vec2,vec2,vec2,vec2,vec2,vec2,vec2,vec2,vec2,vec2,vec2,vec2,vec2,vec2,vec2,vec2); \n"
            "vec3  VP_Interpolate16(vec3,vec3,vec3,vec3,vec3,vec3,vec3,vec3,vec3,vec3,vec3,vec3,vec3,vec3,vec3,vec3); \n"
            "vec4  VP_Interpolate16(vec4,vec4,vec4,vec4,vec4,vec4,vec4,vec4,vec4,vec4,vec4,vec4,vec4,vec4,vec4,vec4); \n";
#endif

        if ( tessEvalStage || (viewStage && viewStageInTES) || (clipStage && clipStageInTES) )
        {
            buf << "\n// Function declarations:\n";
            if ( tessEvalStage )
            {
                for( OrderedFunctionMap::const_iterator i = tessEvalStage->begin(); i != tessEvalStage->end(); ++i )
                {
                    buf << "void " << i->second._name << "(); \n";
                }
            }

            if (xformModelToView)
            {
                for (auto& iter : *xformModelToView)
                    buf << "void " << iter.second._name << "();\n";
            }

            if (viewStage && viewStageInTES)
            {
                for( OrderedFunctionMap::const_iterator i = viewStage->begin(); i != viewStage->end(); ++i )
                {
                    buf << "void " << i->second._name << "(inout vec4); \n";
                }
            }

            if (clipStage && clipStageInTES) 
            {
                for( OrderedFunctionMap::const_iterator i = clipStage->begin(); i != clipStage->end(); ++i )
                {
                    buf << "void " << i->second._name << "(inout vec4); \n";
                }
            }

            // Helper functions:
            buf << "\nvoid VP_LoadVertex(in int index) \n"
                << "{ \n";
        
            // Copy input block to stage globals:
            for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
                buf << INDENT << i->name << " = vp_in[index]." << i->packedName << "; \n";

            buf << "} \n";

            buf << "\nvoid VP_Interpolate3() \n"
                << "{ \n"
                << "   int flat_i = gl_TessCoord.y > gl_TessCoord.x && gl_TessCoord.y > gl_TessCoord.z ? 1 : (gl_TessCoord.z > gl_TessCoord.x && gl_TessCoord.z > gl_TessCoord.y ? 2 : 0); \n";
            
            for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            {
                if ( i->interp != "flat" )                     
                {
                    if ( i->arraySize == 0 )
                    {
                        buf << INDENT << i->name << " = VP_Interpolate3"
                            << "( vp_in[0]." << i->packedName
                            << ", vp_in[1]." << i->packedName
                            << ", vp_in[2]." << i->packedName << " ); \n";
                    }
                    else
                    {
                        for(int n=0; n<i->arraySize; ++n)
                        {
                            buf << INDENT << i->name << "[" << n << "] = VP_Interpolate3"
                                << "( vp_in[0]." << i->packedName << "[" << n << "]"
                                << ", vp_in[1]." << i->packedName << "[" << n << "]"
                                << ", vp_in[2]." << i->packedName << "[" << n << "] ); \n";
                        }
                    }
                }
                else
                {                    
                    buf << INDENT << i->name << " = vp_in[flat_i]." << i->packedName << "; \n";
                    //buf << INDENT << i->name << " = vp_in[gl_InvocationID]." << i->name << "; \n";
                }
            }
            buf << "} \n";

            buf << "\nvoid VP_EmitVertex() \n"
                << "{ \n";

            int space = SPACE_MODEL;

            if ( viewStage && viewStageInTES )
            {
                if (xformModelToView)
                {
                    buf <<
                        INDENT << xformModelToView->begin()->second._name << "();\n";
                }
                else
                {
                    buf << INDENT << "vp_Vertex = " << gl_ModelViewMatrix << " * vp_Vertex; \n"
                        << INDENT << "vp_Normal = normalize(" << gl_NormalMatrix << " * vp_Normal); \n";
                }

                buf << INDENT << "vp_VertexView = vp_Vertex.xyz;\n";

                space = SPACE_VIEW;

                for( OrderedFunctionMap::const_iterator i = viewStage->begin(); i != viewStage->end(); ++i )
                {
                    buf << INDENT << i->second._name << "(vp_Vertex); \n";
                }
            }

            if ( clipStage && clipStageInTES )
            {
                if (space == SPACE_MODEL)
                {
                    if (xformModelToView)
                    {
                        buf << INDENT << xformModelToView->begin()->second._name << "();\n";
                    }
                    else
                    {
                        buf << INDENT << "vp_Vertex = " << gl_ModelViewMatrix << " * vp_Vertex; \n"
                            << INDENT << "vp_Normal = normalize(" << gl_NormalMatrix << " * vp_Normal); \n";
                    }
                    buf << INDENT << "vp_VertexView = vp_Vertex.xyz; \n";
                    buf << INDENT << "vp_Vertex = " << gl_ProjectionMatrix << " * vp_Vertex;\n";
                }
                else if (space == SPACE_VIEW)
                {
                    buf << INDENT << "vp_Vertex = " << gl_ProjectionMatrix << " * vp_Vertex; \n";
                }

                space = SPACE_CLIP;

                for( OrderedFunctionMap::const_iterator i = clipStage->begin(); i != clipStage->end(); ++i )
                {
                    buf << INDENT << i->second._name << "(vp_Vertex); \n";
                }
            }

            // resolve vertex to its next space, but ONLY if this is the final Vertex Processing stage.
            if ( !hasGS )
            {
                if (space == SPACE_MODEL)
                {
                    if (xformModelToView)
                    {
                        buf << INDENT << xformModelToView->begin()->second._name << "();\n";
                    }
                    else
                    {
                        buf << INDENT << "vp_Vertex = " << gl_ModelViewMatrix << " * vp_Vertex; \n"
                            << INDENT << "vp_Normal = normalize(" << gl_NormalMatrix << " * vp_Normal); \n";
                    }
                    buf << INDENT << "vp_VertexView = vp_Vertex.xyz; \n";
                    buf << INDENT << "vp_Vertex = " << gl_ProjectionMatrix << " * vp_Vertex;\n";
                    //buf << INDENT << "vp_VertexView = (" << gl_ModelViewMatrix << " * vp_Vertex).xyz; \n"
                    //    << INDENT << "vp_Vertex = " << gl_ModelViewProjectionMatrix << " * vp_Vertex; \n"
                    //    << INDENT << "vp_Normal = normalize(" << gl_NormalMatrix << " * vp_Normal); \n";
                }
                else if (space == SPACE_VIEW)
                {
                    buf << INDENT << "vp_Vertex = " << gl_ProjectionMatrix << " * vp_Vertex; \n";
                }
            }
        
            // Copy globals to output block:
            for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
                buf << INDENT << "vp_out." << i->packedName << " = " << i->name << "; \n";

            buf << INDENT << "gl_Position = vp_Vertex; \n"
                << "} \n";
        }

        buf << "\n"
            << "void main(void) \n"
            << "{ \n"
            << INDENT "// copy default outputs: \n";
        
        if ( !tessEvalStage )
        {
            // Copy default input block to output block (auto passthrough on first vert)
            // NOT SURE WE NEED THIS
            for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
                buf << INDENT << "vp_out." << i->packedName << " = vp_in[0]." << i->packedName << "; \n";
        }

        if ( tessEvalStage )
        {
            for( OrderedFunctionMap::const_iterator i = tessEvalStage->begin(); i != tessEvalStage->end(); ++i )
            {
                buf << INDENT << i->second._name << "(); \n";
            }
        }

        buf << "} \n";
        
        std::string str = buf.str();
        osg::Shader* tesShader = new osg::Shader(osg::Shader::TESSEVALUATION, str);
        tesShader->setName("VP TES");
        out_shaders.push_back( tesShader );
    }


    //.................................................................................

    // Build the geometry shader.
    if ( hasGS )
    {
        stages |= VirtualProgram::STAGE_GEOMETRY;

        std::stringstream buf;

        buf << getGLSLHeader() << "\n"
            << "#pragma vp_name VP Geometry Shader Main\n";

        addExtensionsToBuffer(buf, in_extensions);

        //buf << glMatrixUniforms << "\n";

        if ( hasVS || hasTCS || hasTES )
        {
            buf << "\n// Geometry stage inputs:\n"
                << "in " << vertdata << " vp_in []; \n";
        }    

        // Declare stage globals.
        buf << "\n// Geometry stage globals: \n";
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << i->declaration << "; \n";
        
        buf << "\n// Geometry stage outputs: \n"
            << "out " << fragdata << " vp_out; \n";

        if ( geomStage || (viewStage && viewStageInGS) || (clipStage && clipStageInGS) )
        {
            buf << "\n// Injected function declarations:\n";

            if (xformModelToView)
            {
                buf << INDENT << "void " << xformModelToView->begin()->second._name << "();\n";
            }

            if ( geomStage )
            {
                for( OrderedFunctionMap::const_iterator i = geomStage->begin(); i != geomStage->end(); ++i )
                {
                    buf << "void " << i->second._name << "(); \n";
                }
            }       

            if ( viewStage && viewStageInGS )
            {
                for( OrderedFunctionMap::const_iterator i = viewStage->begin(); i != viewStage->end(); ++i )
                {
                    buf << "void " << i->second._name << "(inout vec4); \n";
                }
            }

            if ( clipStage && clipStageInGS )
            {
                for( OrderedFunctionMap::const_iterator i = clipStage->begin(); i != clipStage->end(); ++i )
                {
                    buf << "void " << i->second._name << "(inout vec4); \n";
                }
            }
        }
        
        // Build-in helper functions:
        buf << "\nvoid VP_LoadVertex(in int index) \n"
            << "{ \n";
        
        // Copy input block to stage globals:
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << INDENT << i->name << " = vp_in[index]." << i->packedName << "; \n";

        buf << "} \n";

        buf << "\nvoid VP_EmitModelVertex() \n"
            << "{ \n";
        
        buf << INDENT << "vp_Vertex = gl_Position; \n";
        int space = SPACE_MODEL;
        if ( viewStage && viewStageInGS )
        {
            if (xformModelToView)
            {
                buf << INDENT << xformModelToView->begin()->second._name << "();\n";
            }
            else
            {
                buf << INDENT << "vp_Vertex = " << gl_ModelViewMatrix << " * vp_Vertex; \n"
                    << INDENT << "vp_Normal = normalize(" << gl_NormalMatrix << " * vp_Normal); \n";
            }
            buf << INDENT << "vp_VertexView = vp_Vertex.xyz; \n";

            space = SPACE_VIEW;

            for( OrderedFunctionMap::const_iterator i = viewStage->begin(); i != viewStage->end(); ++i )
            {
                buf << INDENT << i->second._name << "(vp_Vertex); \n";
            }
        }

        if ( clipStage && clipStageInGS )
        {
            if ( space == SPACE_MODEL )
            {
                if (xformModelToView)
                {
                    buf << INDENT << xformModelToView->begin()->second._name << "();\n";
                }
                else
                {
                    buf << INDENT << "vp_Vertex = " << gl_ModelViewMatrix << " * vp_Vertex; \n"
                        << INDENT << "vp_Normal = normalize(" << gl_NormalMatrix << " * vp_Normal); \n";
                }
                buf << INDENT << "vp_VertexView = vp_Vertex.xyz; \n";
                buf << INDENT << "vp_Vertex = " << gl_ProjectionMatrix << " * vp_Vertex;\n";
                //buf << INDENT << "vp_VertexView = (" << gl_ModelViewMatrix << " * vp_Vertex).xyz; \n"
                //    << INDENT << "vp_Vertex = " << gl_ModelViewProjectionMatrix << " * vp_Vertex; \n"
                //    << INDENT << "vp_Normal = normalize(" << gl_NormalMatrix << " * vp_Normal); \n";
            }
            else if ( space == SPACE_VIEW )
            {
                buf << INDENT << "vp_Vertex = " << gl_ProjectionMatrix << " * vp_Vertex; \n";
            }            

            space = SPACE_CLIP;

            for( OrderedFunctionMap::const_iterator i = clipStage->begin(); i != clipStage->end(); ++i )
            {
                buf << INDENT << i->second._name << "(vp_Vertex); \n";
            }
        }

        // resolve vertex to its next space:
        if ( space == SPACE_MODEL )
        {
            if (xformModelToView)
            {
                buf << INDENT << xformModelToView->begin()->second._name << "();\n";
            }
            else
            {
                buf << INDENT << "vp_Vertex = " << gl_ModelViewMatrix << " * vp_Vertex; \n"
                    << INDENT << "vp_Normal = normalize(" << gl_NormalMatrix << " * vp_Normal); \n";
            }
            buf << INDENT << "vp_VertexView = vp_Vertex.xyz; \n";
            buf << INDENT << "vp_Vertex = " << gl_ProjectionMatrix << " * vp_Vertex;\n";
            //buf << INDENT << "vp_VertexView = (" << gl_ModelViewMatrix << " * vp_Vertex).xyz; \n"
            //    << INDENT << "vp_Vertex = " << gl_ModelViewProjectionMatrix << " * vp_Vertex; \n"
            //    << INDENT << "vp_Normal = normalize(" << gl_NormalMatrix << " * vp_Normal); \n";
        }
        else if ( space == SPACE_VIEW )
        {
            buf << INDENT << "vp_Vertex = " << gl_ProjectionMatrix << " * vp_Vertex; \n";
        }
                
        // Copy globals to output block:
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << INDENT << "vp_out." << i->packedName << " = " << i->name << "; \n";

        buf << INDENT << "gl_Position = vp_Vertex; \n";

        buf << INDENT << "EmitVertex(); \n"
            << "} \n";
        

        buf << "\nvoid VP_EmitViewVertex() \n"
            << "{ \n";
        
        buf << INDENT << "vp_Vertex = gl_Position; \n";
        space = SPACE_VIEW;
        if ( viewStage && viewStageInGS )
        {
            for( OrderedFunctionMap::const_iterator i = viewStage->begin(); i != viewStage->end(); ++i )
            {
                buf << INDENT << i->second._name << "(vp_Vertex); \n";
            }
        }

        if ( clipStage && clipStageInGS )
        {
            buf << INDENT << "vp_Vertex = " << gl_ProjectionMatrix << " * vp_Vertex; \n";
            space = SPACE_CLIP;
            for( OrderedFunctionMap::const_iterator i = clipStage->begin(); i != clipStage->end(); ++i )
            {
                buf << INDENT << i->second._name << "(vp_Vertex); \n";
            }
        }

        // resolve vertex to its next space:
        if ( space == SPACE_VIEW )
            buf << INDENT << "vp_Vertex = " << gl_ProjectionMatrix << " * vp_Vertex; \n";
                
        // Copy globals to output block:
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << INDENT << "vp_out." << i->packedName << " = " << i->name << "; \n";

        buf << INDENT << "gl_Position = vp_Vertex; \n";

        buf << INDENT << "EmitVertex(); \n"
            << "} \n";

        buf << "\n"
            << "void main(void) \n"
            << "{ \n"
            << INDENT "// copy default outputs: \n";
        
        // Copy default input block to output block (auto passthrough on first vert)
        // NOT SURE WE NEED THIS
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << INDENT << "vp_out." << i->packedName << " = vp_in[0]." << i->packedName << "; \n";

        if ( geomStage )
        {
            for( OrderedFunctionMap::const_iterator i = geomStage->begin(); i != geomStage->end(); ++i )
            {
                buf << INDENT << i->second._name << "(); \n";
            }
        }

        buf << "} \n";

        std::string str;
        str = buf.str();
        osg::Shader* geomShader = new osg::Shader( osg::Shader::GEOMETRY, str );
        geomShader->setName( "main(geometry)" );
        out_shaders.push_back( geomShader );
    }
    

    //.................................................................................


    // Build the Fragment shader.
    if ( hasFS )
    {
        stages |= VirtualProgram::STAGE_FRAGMENT;

        std::stringstream buf;

        buf << getGLSLHeader() << "\n"
            << "#pragma vp_name VP Fragment Shader Main\n";

        addExtensionsToBuffer(buf, in_extensions);

        // no output stage? Use default output
        if (!outputStage)
        {
            buf << "\n// Fragment output\n"
                << "out vec4 vp_FragColor;\n";
        }

        buf << "\n// Fragment stage inputs:\n";
        buf << "in " << fragdata << " vp_in; \n";
                
        buf <<
            "\n// Fragment stage globals:\n";

        // Declare stage globals.
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << i->prec << (i->prec.empty()?"":" ") << i->declaration << ";\n";

        if ( coloringStage || lightingStage || outputStage )
        {
            buf << "\n// Function declarations:\n";
        }

        if ( coloringStage )
        {
            for( OrderedFunctionMap::const_iterator i = coloringStage->begin(); i != coloringStage->end(); ++i )
            {
                buf << "void " << i->second._name << "(inout vec4 color); \n";
            }
        }

        if ( lightingStage )
        {
            for( OrderedFunctionMap::const_iterator i = lightingStage->begin(); i != lightingStage->end(); ++i )
            {
                buf << "void " << i->second._name << "(inout vec4 color); \n";
            }
        }

        if ( outputStage )
        {
            for( OrderedFunctionMap::const_iterator i = outputStage->begin(); i != outputStage->end(); ++i )
            {
                buf << "void " << i->second._name << "(inout vec4 color); \n";
            }
        }

        buf << 
            "\nvoid main(void) \n"
            "{ \n";
        
        // Copy input block to stage globals:
        for(Variables::const_iterator i = vars.begin(); i != vars.end(); ++i)
            buf << INDENT << i->name << " = vp_in." << i->packedName << "; \n";

        buf << INDENT << "vp_Normal = normalize(vp_Normal); \n";

        constexpr int coloringPass = 0;
        constexpr int lightingPass = 1;

        for(int pass=0; pass<2; ++pass)
        {
            if ( coloringStage && (pass == coloringPass) )
            {
                for( OrderedFunctionMap::const_iterator i = coloringStage->begin(); i != coloringStage->end(); ++i )
                {
                    buf << INDENT << i->second._name << "( vp_Color ); \n";
                }
            }

            if ( lightingStage && (pass == lightingPass) )
            {
                for( OrderedFunctionMap::const_iterator i = lightingStage->begin(); i != lightingStage->end(); ++i )
                {
                    buf << INDENT << i->second._name << "( vp_Color ); \n";
                }
            }
        }

        if ( outputStage )
        {
            for( OrderedFunctionMap::const_iterator i = outputStage->begin(); i != outputStage->end(); ++i )
            {
                buf << INDENT << i->second._name << "( vp_Color ); \n";
            }
        }
        else
        {
            // in the absense of any output functions, generate a default output statement
            // that simply writes to gl_FragColor.
            buf << INDENT << "vp_FragColor = vp_Color;\n";
        }
        buf << "}\n";

        std::string str;
        str = buf.str();
        osg::Shader* shader = new osg::Shader( osg::Shader::FRAGMENT, str );
        shader->setName( "main(fragment)" );
        out_shaders.push_back( shader );
    }

    return stages;
}


osg::Shader*
ShaderFactory::createColorFilterChainFragmentShader(
    const std::string& function,
    const ColorFilterChain& chain ) const
{
    std::stringstream buf;
    buf << getGLSLHeader() << "\n";

    // write out the shader function prototypes:
    for( ColorFilterChain::const_iterator i = chain.begin(); i != chain.end(); ++i )
    {
        ColorFilter* filter = i->get();
        buf << "void " << filter->getEntryPointFunctionName() << "(inout vec4 color);\n";
    }

    // write out the main function:
    buf << "void " << function << "(inout vec4 color) \n"
        << "{ \n";

    // write out the function calls. if there are none, it's a NOP.
    for( ColorFilterChain::const_iterator i = chain.begin(); i != chain.end(); ++i )
    {
        ColorFilter* filter = i->get();
        buf << INDENT << filter->getEntryPointFunctionName() << "(color);\n";
    }
        
    buf << "} \n";

    std::string bufstr;
    bufstr = buf.str();
    
    return new osg::Shader(osg::Shader::FRAGMENT, bufstr);
}

std::string
ShaderFactory::getRangeUniformName() const
{
    return "oe_range_to_bs";
}

osg::Uniform*
ShaderFactory::createRangeUniform() const
{
    return new osg::Uniform(osg::Uniform::FLOAT, getRangeUniformName());
}
