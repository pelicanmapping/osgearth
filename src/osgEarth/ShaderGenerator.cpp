
/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include <osgEarth/ShaderGenerator>
#include <osgEarth/Capabilities>
#include <osgEarth/ImageUtils>
#include <osgEarth/Registry>
#include <osgEarth/URI>
#include <osgEarth/Shaders>
#include <osgEarth/GLUtils>
#include <osgEarth/Lighting>
#include <osgEarth/PBRMaterial>
#include <array>
#include "ShaderLoader"

#include <osg/PagedLOD>
#include <osg/ProxyNode>
#include <osg/Texture1D>
#include <osg/Texture3D>
#include <osg/TextureRectangle>
#include <osg/Texture2DArray>
#include <osg/TextureCubeMap>
#include <osg/TexGen>
#include <osg/TexMat>
#include <osg/ClipNode>
#include <osg/PointSprite>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgText/Text>
#include <osgSim/LightPointNode>
#include <osgUtil/SmoothingVisitor>

#include <osg/ValueObject>

#define LC "[ShaderGenerator] "

#define SHADERGEN_PL_EXTENSION "osgearth_shadergen"

#define SHADERGEN_HINT_IGNORE "osgEarth.ShaderGenerator.ignore"

// Set this to detect whether a Geode's drawables all have the same VP
// profile, and if so, promote that VP to the Geode's state set.
#define PROMOTE_EQUIVALENT_DRAWABLE_VP_TO_GEODE 1

using namespace osgEarth;
using namespace osgEarth::Util;

//------------------------------------------------------------------------

// compatibility string for GLES:

#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE)
#   define GLSL_PRECISION "precision mediump float;"
#   define MEDIUMP        "mediump "
#   define LOWP           "lowp "
#   define HIGHP          "highp "
#else
#   define GLSL_PRECISION
#   define MEDIUMP        ""
#   define LOWP           ""
#   define HIGHP          ""
#endif

// shader names
#define TEX_COORD      "oe_sg_texcoord"
#define TEX_COORD_TEXT "oe_sg_texcoord_text"
#define SAMPLER        "oe_sg_sampler"
#define SAMPLER_TEXT   "oe_sg_sampler_text"
#define ATTRIB         "oe_sg_attrib"
#define TEXENV_COLOR   "oe_sg_texenvcolor"
#define TEX_MATRIX     "oe_sg_texmat"

#define VERTEX_MODEL_FUNCTION "oe_sg_vert_model"
#define VERTEX_VIEW_FUNCTION  "oe_sg_vert_view"
#define FRAGMENT_FUNCTION     "oe_sg_frag"

// other stuff
#define INDENT "    "

//------------------------------------------------------------------------

namespace
{
    struct OSGEarthShaderGenPseudoLoader : public osgDB::ReaderWriter
    {
        OSGEarthShaderGenPseudoLoader()
        {
            this->supportsExtension( SHADERGEN_PL_EXTENSION, "ShaderGen pseudoloader" );
        }

        const char* className() const
        {
            return "OSGEarth ShaderGen pseudoloader";
        }

        bool acceptsExtension(const std::string& extension) const
        {
            return osgDB::equalCaseInsensitive( extension, SHADERGEN_PL_EXTENSION );
        }

        ReadResult readObject(const std::string& filename, const osgDB::Options* options) const
        {
            return readNode( filename, options );
        }

        ReadResult readNode(const std::string& filename, const osgDB::Options* options) const
        {
            if ( !acceptsExtension(osgDB::getFileExtension(filename)) )
                return ReadResult::FILE_NOT_HANDLED;

            std::string stripped = osgDB::getNameLessExtension(filename);

            //OE_DEBUG << LC << "Loading " << stripped << " from PLOD/Proxy and generating shaders." << std::endl;

            osgEarth::ReadResult result = URI(stripped).readNode(options);
            if ( result.succeeded() && result.getNode() != 0L )
            {
                osg::ref_ptr<osg::Node> node = result.releaseNode();

                osgEarth::Registry::shaderGenerator().run(
                    node.get(),
                    osgDB::getSimpleFileName(stripped),
                    Registry::stateSetCache() );

                return ReadResult( node.release() );
            }

            else
            {
                OE_WARN << LC << "Error loading \"" << stripped << "\": " << result.errorDetail() << "\n";
                return ReadResult::ERROR_IN_READING_FILE;
            }
        }
    };

    REGISTER_OSGPLUGIN(SHADERGEN_PL_EXTENSION, OSGEarthShaderGenPseudoLoader)
}

//------------------------------------------------------------------------

namespace
{
    struct ActiveAttributeCollector : public osg::StateAttribute::ModeUsage
    {
        ActiveAttributeCollector(osg::StateSet* stateset, const osg::StateAttribute* sa, unsigned unit=0) :
            _stateset(stateset),
            _sa      (const_cast<osg::StateAttribute*>(sa)),
            _unit    (unit) {}

        virtual ~ActiveAttributeCollector() {}

        virtual void usesMode(osg::StateAttribute::GLMode mode)
        {
            if (_stateset->getMode(mode) & osg::StateAttribute::ON)
            {
                if ( _sa->isTextureAttribute() )
                    _stateset->setTextureAttribute(_unit, _sa, osg::StateAttribute::ON);
                else
                    _stateset->setAttribute(_sa, osg::StateAttribute::ON);
            }
        }

        virtual void usesTextureMode(osg::StateAttribute::GLMode mode)
        {
            if (_stateset->getTextureMode(_unit, mode) & osg::StateAttribute::ON)
            {
                if ( _sa->isTextureAttribute() )
                    _stateset->setTextureAttribute(_unit, _sa, osg::StateAttribute::ON);
                else
                    _stateset->setAttribute(_sa, osg::StateAttribute::ON);
            }
        }

        osg::StateSet*       _stateset;
        osg::StateAttribute* _sa;
        unsigned             _unit;
    };
}

//------------------------------------------------------------------------

namespace
{
    /**
     * The OSG State extended with mode/attribute accessors.
     */
    class StateEx : public osg::State
    {
    public:
        StateEx() : State() {}

        // Captures the ACTIVE state into a state set. i.e., only state attributes
        // set to ON.
        osg::StateSet* capture() const
        {
            osg::StateSet* stateset = new osg::StateSet();

            // add ON modes to the new stateset:
            for(ModeMap::const_iterator i=_modeMap.begin();
                i!=_modeMap.end();
                ++i)
            {
                // note GLMode = mitr->first
                const ModeStack& ms = i->second;
                if (!ms.valueVec.empty())
                {
                    stateset->setMode(i->first,ms.valueVec.back());
                }
            }

            // add ON texture modes to the new stateset:
            for(unsigned unit=0; unit<_textureModeMapList.size(); ++unit)
            {
                const ModeMap& modeMap = _textureModeMapList[unit];
                for(ModeMap::const_iterator i = modeMap.begin(); i != modeMap.end(); ++i)
                {
                    const ModeStack& ms = i->second;
                    if (!ms.valueVec.empty())
                    {
                        stateset->setTextureMode(unit, i->first, ms.valueVec.back());
                    }
                }
            }

            for(AttributeMap::const_iterator i=_attributeMap.begin();
                i!=_attributeMap.end();
                ++i)
            {
                const AttributeStack& as = i->second;
                if (!as.attributeVec.empty())
                {
                    const osg::State::AttributePair& pair = as.attributeVec.back();
                    osg::StateAttribute* sa = const_cast<osg::StateAttribute*>(pair.first);
                    ActiveAttributeCollector collector(stateset, sa);
                    bool modeless = isModeless(sa) || !sa->getModeUsage(collector);
                    if (modeless)
                    {
                        // if getModeUsage returns false, there are no modes associated with
                        // this attr, so just add it (it can't be forcably disabled)
                        stateset->setAttribute(sa, osg::StateAttribute::ON);
                    }
                }
            }

            for(unsigned unit=0; unit<_textureAttributeMapList.size(); ++unit)
            {
                const AttributeMap& attrMap = _textureAttributeMapList[unit];
                for(AttributeMap::const_iterator i = attrMap.begin(); i != attrMap.end(); ++i)
                {
                    const AttributeStack& as = i->second;
                    if (!as.attributeVec.empty())
                    {
                        const osg::State::AttributePair& pair = as.attributeVec.back();
                        osg::StateAttribute* sa = const_cast<osg::StateAttribute*>(pair.first);
                        ActiveAttributeCollector collector(stateset, sa, unit);
						bool modeless = isModeless(sa) || !sa->getModeUsage(collector);
                        // Shader generation removes fixed-function texture modes.
                        // Retain its existing sampler inputs when regenerating a
                        // program, while still honoring an explicit OFF mode.
                        auto* texture = dynamic_cast<osg::Texture*>(sa);
                        if (texture && stateset->getTextureMode(unit, texture->getTextureTarget()) == osg::StateAttribute::INHERIT)
                        {
                            auto sampler = _uniformMap.find(Stringify() << SAMPLER << unit);
                            int binding = -1;
                            if (sampler != _uniformMap.end() && !sampler->second.uniformVec.empty() &&
                                sampler->second.uniformVec.back().first->get(binding) && binding == static_cast<int>(unit))
                                modeless = true;
                        }
						if (modeless)
						{
                            // if getModeUsage returns false, there are no modes associated with
                            // this attr, so just add it (it can't be forcably disabled)
                            stateset->setTextureAttribute(unit, sa, osg::StateAttribute::ON);
                        }
                    }
                }
            }

            return stateset;
        }

        // some attrs dont' properly report mode usage until OSG 3.3.1.
        // ref: https://github.com/openscenegraph/osg/commit/22af59482ac4f727eeed5b97476a3a47d7fe8a69
        bool isModeless(osg::StateAttribute* sa) const
        {

#if !defined(OSG_GL_FIXED_FUNCTION_AVAILABLE)
            // No modes in non-ffp
            return true;
#else
            return false;
#endif
        }
    };

    // if the node has a stateset, clone it and replace it with the clone.
    // otherwise, just create a new stateset on the node.
    osg::StateSet* cloneOrCreateStateSet(osg::Node* node)
    {
        if ( node->getStateSet() )
        {
            node->setStateSet( osg::clone(node->getStateSet(), osg::CopyOp::SHALLOW_COPY) );
            return node->getStateSet();
        }
        else
        {
            return node->getOrCreateStateSet();
        }
    }
}

//...........................................................................

ShaderGenerator::GenBuffers::GenBuffers() :
    _stateSet(0L)
{
    //nop
}

//...........................................................................

ShaderGenerator::ShaderGenerator()
{
    // find everything regardless of node masking
    setTraversalMode( TRAVERSE_ALL_CHILDREN );
    setNodeMaskOverride( ~0 );
    _state = new StateEx();
}

ShaderGenerator::ShaderGenerator(const ShaderGenerator& rhs, const osg::CopyOp& copy) :
    osg::NodeVisitor(rhs, copy),
    _active(rhs._active),
    _duplicateSharedSubgraphs(rhs._duplicateSharedSubgraphs),
    _removeUnsupportedFFPAttributes(rhs._removeUnsupportedFFPAttributes)
{
    _state = new StateEx();
}

void
ShaderGenerator::setIgnoreHint(osg::Object* object, bool ignore)
{
    if (object)
    {
        object->setUserValue( SHADERGEN_HINT_IGNORE, ignore );
    }
}

bool
ShaderGenerator::ignore(const osg::Object* object)
{
    bool value;
    return object && object->getUserValue(SHADERGEN_HINT_IGNORE, value) && value;
}

void
ShaderGenerator::setDuplicateSharedSubgraphs(bool value)
{
    _duplicateSharedSubgraphs = value;
}

void
ShaderGenerator::addAcceptCallback(AcceptCallback* cb)
{
    _acceptCallbacks.push_back( cb );
}

bool
ShaderGenerator::accept(const osg::StateAttribute* sa) const
{
    if ( sa == 0L )
        return false;

    if ( ignore(sa) )
        return false;

    for(AcceptCallbackVector::const_iterator i = _acceptCallbacks.begin(); i != _acceptCallbacks.end(); ++i )
    {
        if ( !i->get()->accept(sa) )
            return false;
    }
    return true;
}

void
ShaderGenerator::run(osg::Node*         graph,
                     const std::string& vpName,
                     StateSetCache*     cache)
{
    if ( graph )
    {
        // generate shaders:
        graph->accept( *this );

        // perform GL state sharing
        optimizeStateSharing( graph, cache );

        // generate uniforms and uniform callbacks for lighting and material elements.
        GenerateGL3LightingUniforms generateUniforms;
        graph->accept(generateUniforms);

        osg::StateSet* stateset = cloneOrCreateStateSet(graph);

        // install a blank VP at the top as the default.
        VirtualProgram* vp = VirtualProgram::get(stateset);
        if ( !vp )
        {
            vp = VirtualProgram::getOrCreate(stateset);
            vp->setInheritShaders( true );
            vp->setName( vpName );
        }
    }
}

osg::ref_ptr<osg::StateSet>
ShaderGenerator::run(osg::StateSet* ss)
{
    if (!ss)
        return NULL;

    _state->pushStateSet(ss);
    osg::ref_ptr<osg::StateSet> replacement;
    processGeometry(nullptr, ss, replacement);
    _state->popStateSet();
    return replacement;
}

void
ShaderGenerator::optimizeStateSharing(osg::Node* node, StateSetCache* cache)
{
    if ( node && cache )
        cache->optimize(node);
}

void
ShaderGenerator::duplicateSharedNode(osg::Node& node)
{
    if ( node.getNumParents() > 1 )
    {
        for(int i=1; i<(int)node.getNumParents(); ++i)
        {
            osg::Group* parent = node.getParent(i);
            osg::Node* replicant = osg::clone(
                &node,
                osg::CopyOp::DEEP_COPY_NODES | osg::CopyOp::DEEP_COPY_DRAWABLES | osg::CopyOp::DEEP_COPY_ARRAYS);
            parent->replaceChild(&node, replicant);
        }
    }
}

void
ShaderGenerator::apply(osg::Node& node)
{
    if (!_active)
        return;

    if (ignore(&node))
        return;

    if (_duplicateSharedSubgraphs)
        duplicateSharedNode(node);

    applyNonCoreNodeIfNecessary(node);

    osg::ref_ptr<osg::StateSet> stateset = node.getStateSet();
    if (stateset.valid())
    {
        disableUnsupportedAttributes(stateset.get());
        _state->pushStateSet(stateset.get());
    }

    traverse(node);

    if (stateset.valid())
    {
        _state->popStateSet();
    }
}

void
ShaderGenerator::apply(osg::Group& group)
{
    apply(static_cast<osg::Node&>(group));
}

void
ShaderGenerator::apply(osg::Geode& node)
{
    if ( !_active )
        return;

    if ( ignore(&node) )
        return;

    if ( _duplicateSharedSubgraphs )
        duplicateSharedNode(node);

    osg::ref_ptr<osg::StateSet> stateset = node.getStateSet();
    if ( stateset.valid() )
    {
        disableUnsupportedAttributes(stateset.get());
        _state->pushStateSet( stateset.get() );
    }

    unsigned numDrawables = node.getNumDrawables();
    bool traverseDrawables = true;

#ifdef PROMOTE_EQUIVALENT_DRAWABLE_VP_TO_GEODE
    // This block checks whether all the geode's drawables are equivalent,
    // i.e., they are the same type (geometry or text) and none of them
    // have their own state sets. IF that's the case, we can create a
    // single shader program for the entire geode. This is an optimization.
    if ( stateset.valid() )
    {
        unsigned d;
        unsigned numInheritingText = 0, numInheritingGeometry = 0;
        for( d = 0; d < numDrawables; ++d )
        {
            osg::Drawable* drawable = node.getDrawable(d);
            if ( drawable->getStateSet() == 0L )
            {
                if ( drawable->asGeometry() )
                    numInheritingGeometry++;
                else if ( dynamic_cast<osgText::Text*>(drawable) )
                    numInheritingText++;
            }
        }

        if (numInheritingGeometry == numDrawables )
        {
            osg::ref_ptr<osg::StateSet> replacement;
            if ( processGeometry(nullptr, stateset.get(), replacement) )
            {
                node.setStateSet(replacement.get() );
                traverseDrawables = false;
            }
        }
        else if (numInheritingText == numDrawables )
        {
            osg::ref_ptr<osg::StateSet> replacement;
            if ( processText(stateset.get(), replacement) )
            {
                node.setStateSet(replacement.get() );
                traverseDrawables = false;
            }
        }
    }
#endif

    // Drawables have state sets, so let's traverse them.
    if ( traverseDrawables )
    {
        for( unsigned d = 0; d < node.getNumDrawables(); ++d )
        {
            apply( node.getDrawable(d) );
        }
    }

    if ( stateset.valid() )
    {
        _state->popStateSet();
    }
}

void
ShaderGenerator::apply(osg::Drawable& node)
{
    if ( !_active )
        return;

    if ( ignore(&node) )
        return;

    if ( _duplicateSharedSubgraphs )
        duplicateSharedNode(node);

    apply( &node );
}

void
ShaderGenerator::apply( osg::Drawable* drawable )
{
    if ( drawable )
    {
        if (_drawablesVisited.find(drawable) != _drawablesVisited.end())
            return;
        else
            _drawablesVisited.insert(drawable);

        osg::ref_ptr<osg::StateSet> ss = drawable->getStateSet();
        if ( ss.valid() )
        {
            disableUnsupportedAttributes(ss.get());
            _state->pushStateSet(ss.get());
        }

        osg::ref_ptr<osg::StateSet> replacement;
        if ( dynamic_cast<osgText::Text*>(drawable) != 0L )
        {
            if ( processText(ss.get(), replacement) )
            {
                drawable->setStateSet( replacement.get() );
            }
        }
        else
        {
            osg::Geometry* geom = drawable->asGeometry();
            if ( geom )
            {
                geom->setUseVertexBufferObjects(true);
                geom->setUseDisplayList(false);
                
                // generate normals if none exist
                if (geom->getNormalArray() == nullptr)
                {
                    osgUtil::SmoothingVisitor normal_gen;
                    geom->accept(normal_gen);
                }
            }

            if ( processGeometry(geom, ss.get(), replacement) )
            {
                drawable->setStateSet(replacement.get());
            }
        }

        if ( ss.valid() )
        {
            _state->popStateSet();
        }
    }
}


void
ShaderGenerator::apply(osg::PagedLOD& node)
{
    if ( !_active )
        return;

    if ( ignore(&node) )
        return;

    for( unsigned i=0; i<node.getNumFileNames(); ++i )
    {
        static std::mutex s_mutex;
        std::lock_guard<std::mutex> lock(s_mutex);

        const std::string& filename = node.getFileName( i );
        if (!filename.empty() &&
            osgDB::getLowerCaseFileExtension(filename).compare(SHADERGEN_PL_EXTENSION) != 0 )
        {
            node.setFileName( i, Stringify() << filename << "." << SHADERGEN_PL_EXTENSION );
        }
    }

    apply( static_cast<osg::LOD&>(node) );
}


void
ShaderGenerator::apply(osg::ProxyNode& node)
{
    if ( !_active )
        return;

    if ( ignore(&node) )
        return;

    if ( node.getLoadingExternalReferenceMode() != osg::ProxyNode::LOAD_IMMEDIATELY )
    {
        // rewrite the filenames to include the shadergen pseudo-loader extension so
        // that dynamically loaded children will have the same shadergen applied.
        for( unsigned i=0; i<node.getNumFileNames(); ++i )
        {
            const std::string& filename = node.getFileName( i );
            if (!filename.empty() &&
                osgDB::getLowerCaseFileExtension(filename).compare(SHADERGEN_PL_EXTENSION) != 0 )
            {
                node.setFileName( i, Stringify() << filename << "." << SHADERGEN_PL_EXTENSION );
            }
        }
    }

    apply( static_cast<osg::Group&>(node) );
}


void
ShaderGenerator::apply(osg::ClipNode& node)
{
    // no longer needed
    apply( static_cast<osg::Group&>(node) );
}

void
ShaderGenerator::applyNonCoreNodeIfNecessary(osg::Node& node)
{
    if ( dynamic_cast<osgSim::LightPointNode*>(&node) )
    {
        apply( static_cast<osgSim::LightPointNode&>(node) );
    }
}

void
ShaderGenerator::apply(osgSim::LightPointNode& node)
{
    if ( node.getPointSprite() )
    {
        osg::ref_ptr<osg::StateSet> stateset;

        // if the node has state, clone it so we can add our temp attribute.
        stateset = node.getStateSet() ?
            osg::clone(node.getStateSet(), osg::CopyOp::SHALLOW_COPY) :
            new osg::StateSet();

        // add a temporary point sprite so the generator will make sprite code.
        osg::ref_ptr<osg::PointSprite> sprite = new osg::PointSprite();
        stateset->setTextureAttributeAndModes(0, sprite.get());

        disableUnsupportedAttributes(stateset.get());

        _state->pushStateSet( stateset.get() );

        osg::ref_ptr<osg::StateSet> replacement;
        if ( processGeometry(nullptr, stateset.get(), replacement) )
        {
            // remove the temporary sprite.
            replacement->removeTextureAttribute(0, sprite.get());
            node.setStateSet(replacement.get() );
        }

        _state->popStateSet();
    }
}

bool
ShaderGenerator::processText(const osg::StateSet* ss, osg::ref_ptr<osg::StateSet>& replacement)
{
    // do nothing if there's no GLSL support
    if ( !_active )
        return false;

    // Capture the active current state:
    osg::ref_ptr<osg::StateSet> current = static_cast<StateEx*>(_state.get())->capture();

    // New state set. We never modify existing statesets.
    replacement = ss ? osg::clone(ss, osg::CopyOp::SHALLOW_COPY) : new osg::StateSet();

    // new VP:
    osg::ref_ptr<VirtualProgram> vp = VirtualProgram::cloneOrCreate(replacement.get());

    // give the VP a name if it needs one.
    if ( vp->getName().empty() )
    {
        vp->setName( _name );
    }

    Shaders shaders;
    shaders.load(vp.get(), shaders.Text);
    #if defined(OSG_GL3_AVAILABLE) && !defined(OSG_GL2_AVAILABLE) && !defined(OSG_GL1_AVAILABLE)
        replacement->setDefine("OSGTEXT_GLYPH_ALPHA_FORMAT_IS_RED");
    #endif

    return replacement.valid();
}


bool
ShaderGenerator::processGeometry(
    const osg::Geometry* geom,
    const osg::StateSet* original,
    osg::ref_ptr<osg::StateSet>& replacement)
{
    // do nothing if there's no GLSL support
    if ( !_active )
        return false;

    // capture the active current state:
    osg::ref_ptr<osg::StateSet> current = static_cast<StateEx*>(_state.get())->capture();

    // check for a real osg::Program in the whole state stack. If it exists, bail out
    // so that OSG can use the program already in the graph. We never override a
    // full Program.
    osg::StateAttribute* program = current->getAttribute(osg::StateAttribute::PROGRAM);
    if ( dynamic_cast<osg::Program*>(program) != 0L )
        return false;

    // Copy or create a new stateset (that we may or may not use depending on
    // what we find). Never modify an existing stateset!
    osg::ref_ptr<osg::StateSet> newStateSet =
        original ? osg::clone(original, osg::CopyOp::SHALLOW_COPY) :
        new osg::StateSet();

    // likewise, create a VP that we might populate.
    osg::ref_ptr<VirtualProgram> vp = VirtualProgram::cloneOrCreate(original, newStateSet.get());

    // we'll set this to true if the new stateset goes into effect and
    // needs to be returned.
    bool needNewStateSet = false;
    bool needVertexFunction = false;
    bool needFragmentFunction = false;

    // give the VP a name if it needs one.
    if ( vp->getName().empty() )
    {
        vp->setName( _name );
    }

    // Check whether the lighting state has changed and install a mode uniform.
    if ( original && original->getMode(GL_LIGHTING) != osg::StateAttribute::INHERIT )
    {
        needNewStateSet = true;
        osg::StateAttribute::GLModeValue value = current->getMode(GL_LIGHTING);
        GLUtils::setLighting(newStateSet.get(), value);
    }

    // start generating the shader source.
    GenBuffers buf;
    buf._stateSet = newStateSet.get();

    // Keep PBRTexture descriptors in place for other consumers (e.g. Chonk).
    // Ordinary rendering binds their component textures on available units.
    const int maxUnits = Registry::capabilities().getMaxGPUTextureUnits();
    std::map<int, std::array<int, 3>> materialUnits;
    std::set<int> componentUnits;
    for (int unit = 0; unit < maxUnits; ++unit)
    {
        auto* material = dynamic_cast<PBRTexture*>(current->getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
        if (!material || !accept(material)) continue;
        std::array<int, 3> units = {{ -1, -1, -1 }};
        unsigned component = 0;
        for (auto* texture : { material->albedo.get(), material->normal.get(), material->pbr.get() })
        {
            const std::string name = Stringify() << "oe_sg_pbr_" << unit << "_" << component;
            if (texture)
            {
                // Reuse bindings on repeated runs, including inherited state.
                const auto& uniforms = _state->getUniformMap();
                auto previous = uniforms.find(name);
                int binding = -1;
                if (previous != uniforms.end() && !previous->second.uniformVec.empty())
                    previous->second.uniformVec.back().first->get(binding);
                auto* bound = binding >= 0 && binding < maxUnits ?
                    current->getTextureAttribute(binding, osg::StateAttribute::TEXTURE) : nullptr;
                if (binding < 0 || binding >= maxUnits || componentUnits.count(binding) ||
                    (bound && bound != texture))
                {
                    binding = maxUnits - 1;
                    while (binding >= 0 && (componentUnits.count(binding) ||
                        current->getTextureAttribute(binding, osg::StateAttribute::TEXTURE))) --binding;
                }
                if (binding < 0)
                {
                    OE_WARN << LC << "Insufficient texture units for PBR material" << std::endl;
                    return false;
                }
                units[component] = binding;
                componentUnits.insert(binding);
                newStateSet->setTextureAttributeAndModes(binding, texture, osg::StateAttribute::ON);
                newStateSet->getOrCreateUniform(name, osg::Uniform::SAMPLER_2D)->set(binding);
            }
            ++component;
        }
        materialUnits[unit] = units;
    }

    // if the geometry doesn't have normals, we need to set a default value.
    if (geom && geom->getNormalArray() == nullptr)
    {
        buf._modelHead << INDENT << "vec3 vp_Normal;\n";
        buf._modelBody << INDENT << "vp_Normal = vec3(0,0,1);\n";
        needNewStateSet = true;
    }

    // if the stateset changes any texture attributes, we need a new virtual program:
    if (current->getTextureAttributeList().size() > 0)
    {
        bool wroteTexelDecl = false;
        bool wrotePBRDecl = false;

        // Loop over all possible texture image units.
        int maxUnit = Registry::capabilities().getMaxGPUTextureUnits();

        for( int unit = 0; unit < maxUnit; ++unit )
        {
            if (componentUnits.count(unit)) continue;
            if ( !wroteTexelDecl )
            {
                buf._fragBody << INDENT << MEDIUMP "vec4 texel; \n";
                wroteTexelDecl = true;
            }

            osg::Texture* tex = dynamic_cast<osg::Texture*>( current->getTextureAttribute(unit, osg::StateAttribute::TEXTURE) );

            osg::TexGen* texgen = dynamic_cast<osg::TexGen*>(current->getTextureAttribute(unit, osg::StateAttribute::TEXGEN));
            osg::TexEnv* texenv = dynamic_cast<osg::TexEnv*>(current->getTextureAttribute(unit, osg::StateAttribute::TEXENV));
            osg::TexMat* texmat = dynamic_cast<osg::TexMat*>(current->getTextureAttribute(unit, osg::StateAttribute::TEXMAT));
            osg::PointSprite* sprite = dynamic_cast<osg::PointSprite*>(current->getTextureAttribute(unit, osg::StateAttribute::POINTSPRITE));

            auto material = materialUnits.find(unit);
            if (material != materialUnits.end())
            {
                needNewStateSet = true;
                if (!wrotePBRDecl)
                {
                    wrotePBRDecl = true;
                    buf._viewHead << "out vec3 oe_sg_pbr_position;\n";
                    buf._viewBody << "oe_sg_pbr_position = vertex_view.xyz / vertex_view.w;\n";
                    buf._fragHead << R"(
#pragma import_defines(OE_IS_SHADOW_CAMERA, OE_IS_DEPTH_CAMERA)
in vec3 oe_sg_pbr_position;
in vec3 vp_Normal;
struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;
vec3 oe_sg_pbr_normal(vec3 sample_normal, vec2 uv)
{
    vec3 N = normalize(vp_Normal);
    vec3 dp1 = dFdx(oe_sg_pbr_position), dp2 = dFdy(oe_sg_pbr_position);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 p2 = cross(dp2, N), p1 = cross(N, dp1);
    vec3 T = p2 * duv1.x + p1 * duv2.x;
    vec3 B = p2 * duv1.y + p1 * duv2.y;
    float det = max(dot(T,T), dot(B,B));
    float inv = det > 0.0 ? inversesqrt(det) : 0.0;
    vec3 n = mat3(T * inv, B * inv, N) * (sample_normal * 2.0 - 1.0);
    float len = length(n);
    return len > 0.0 ? n / len : N;
}
)";
                }
                buf._modelHead << "out vec4 " TEX_COORD << unit << ";\n";
                buf._viewHead << "out vec4 " TEX_COORD << unit << ";\n";
                buf._fragHead << "in vec4 " TEX_COORD << unit << ";\n";
                apply(texgen, unit, buf);
                apply(texmat, unit, buf);
                if (sprite) apply(sprite, unit, buf);
                const auto& units = material->second;
                for (unsigned c = 0; c < units.size(); ++c)
                    if (units[c] >= 0)
                        buf._fragHead << "uniform sampler2D oe_sg_pbr_" << unit << "_" << c << ";\n";
                if (units[0] >= 0)
                {
                    buf._fragBody << "texel = texture(oe_sg_pbr_" << unit << "_0, " TEX_COORD << unit << ".xy);\n";
                    apply(texenv, unit, buf);
                }
                buf._fragBody << "#if !defined(OE_IS_SHADOW_CAMERA) && !defined(OE_IS_DEPTH_CAMERA)\n";
                if (units[1] >= 0)
                    buf._fragBody << "vp_Normal = oe_sg_pbr_normal(texture(oe_sg_pbr_" << unit
                        << "_1, " TEX_COORD << unit << ".xy).xyz, " TEX_COORD << unit << ".xy);\n";
                if (units[2] >= 0)
                    buf._fragBody << "texel = texture(oe_sg_pbr_" << unit << "_2, " TEX_COORD << unit << ".xy);\n"
                        << "oe_pbr.displacement = texel.r;\n"
                        << "oe_pbr.roughness *= texel.g;\n"
                        << "oe_pbr.ao *= texel.b;\n"
                        << "oe_pbr.metal = clamp(oe_pbr.metal + texel.a, 0.0, 1.0);\n";
                buf._fragBody << "#endif\n";
            }

            if (accept(tex) && !ImageUtils::isFloatingPointInternalFormat(tex->getInternalFormat()))
            {
                if ( apply(tex, texgen, texenv, texmat, sprite, unit, buf) == true )
                {
                    needNewStateSet = true;
                }
            }

#ifndef OSG_GL_FIXED_FUNCTION_AVAILABLE
            if (_removeUnsupportedFFPAttributes)
            {
                if (texgen)
                    newStateSet->removeTextureAttribute(unit, texgen);
                if (texenv)
                    newStateSet->removeTextureAttribute(unit, texenv);
                if (texmat)
                    newStateSet->removeTextureAttribute(unit, texmat);
            }
#endif
        }
    }

    bool linearColor = false;
    if (geom && geom->getUserValue(SHADERGEN_HINT_LINEAR_COLOR, linearColor) && linearColor)
    {
        needNewStateSet = true;
        buf._fragHead << "// Encode linear color after texture modulation.\n";
        buf._fragBody << R"(
    vec3 c = clamp(color.rgb, 0.0, 1.0);
    color.rgb = mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
        12.92 * c, lessThanEqual(c, vec3(0.0031308)));
)";
    }

    // Process the state attributes.
    osg::StateSet::AttributeList& attrs = current->getAttributeList();
    if ( apply(attrs, buf) )
    {
        needNewStateSet = true;
    }

    if ( needNewStateSet )
    {
        std::string modelHeadSource = buf._modelHead.str();
        std::string modelBodySource = buf._modelBody.str();

        if (!modelHeadSource.empty() && !modelBodySource.empty())
        {
            std::string modelSource = Stringify()
                << modelHeadSource
                << "void " VERTEX_MODEL_FUNCTION "(inout vec4 vertex_model)\n{\n"
                << modelBodySource
                << "}\n";

            vp->setFunction(VERTEX_MODEL_FUNCTION, modelSource, VirtualProgram::LOCATION_VERTEX_MODEL, 0.5f);
        }

        std::string viewHeadSource;
        viewHeadSource = buf._viewHead.str();

        std::string viewBodySource;
        viewBodySource = buf._viewBody.str();

        if ( !viewHeadSource.empty() && !viewBodySource.empty() )
        {
            std::string viewSource = Stringify()
                << viewHeadSource
                << "void " VERTEX_VIEW_FUNCTION "(inout vec4 vertex_view)\n{\n"
                << viewBodySource
                << "}\n";

            vp->setFunction(VERTEX_VIEW_FUNCTION, viewSource, VirtualProgram::LOCATION_VERTEX_VIEW, 0.5f);
        }


        std::string fragHeadSource;
        fragHeadSource = buf._fragHead.str();

        std::string fragBodySource;
        fragBodySource = buf._fragBody.str();

        if ( !fragHeadSource.empty() && !fragBodySource.empty() )
        {
            std::string fragSource = Stringify()
                << fragHeadSource
                << "void " FRAGMENT_FUNCTION "(inout vec4 color)\n{\n"
                << fragBodySource
                << "}\n";

            vp->setFunction(FRAGMENT_FUNCTION, fragSource, VirtualProgram::LOCATION_FRAGMENT_COLORING, 0.5f);
        }
    }

    if ( needNewStateSet )
    {
        replacement = newStateSet.get();
    }
    return replacement.valid();
}


bool
ShaderGenerator::apply(osg::Texture*     tex,
                       osg::TexGen*      texgen,
                       osg::TexEnv*      texenv,
                       osg::TexMat*      texmat,
                       osg::PointSprite* sprite,
                       int               unit,
                       GenBuffers&       buf)
{
   bool ok = true;

   buf._modelHead << "out " MEDIUMP "vec4 " TEX_COORD << unit << ";\n";
   buf._viewHead << "out " MEDIUMP "vec4 " TEX_COORD << unit << ";\n";
   buf._fragHead << "in " MEDIUMP "vec4 " TEX_COORD << unit << ";\n";

   apply( texgen, unit, buf );
   apply( texmat, unit, buf );

   if ( sprite )
   {
      apply(sprite, unit, buf);
   }
   else if ( dynamic_cast<osg::Texture1D*>(tex) )
   {
      apply(static_cast<osg::Texture1D*>(tex), unit, buf);
   }
   else if ( dynamic_cast<osg::Texture2D*>(tex) )
   {
      apply(static_cast<osg::Texture2D*>(tex), unit, buf);
   }
   else if ( dynamic_cast<osg::Texture3D*>(tex) )
   {
      apply(static_cast<osg::Texture3D*>(tex), unit, buf);
   }
   else if ( dynamic_cast<osg::TextureRectangle*>(tex) )
   {
      apply(static_cast<osg::TextureRectangle*>(tex), unit, buf);
   }
   else if ( dynamic_cast<osg::Texture2DArray*>(tex) )
   {
      apply(static_cast<osg::Texture2DArray*>(tex), unit, buf);
   }
   else if ( dynamic_cast<osg::TextureCubeMap*>(tex) )
   {
       apply(static_cast<osg::TextureCubeMap*>(tex), unit, buf);
   }
   else
   {
      OE_WARN << LC << "Unsupported texture type: " << tex->className() << std::endl;
      ok = false;
   }

   if ( ok )
   {
      apply( texenv, unit, buf );
   }

   return ok;
}


bool
ShaderGenerator::apply(osg::TexEnv* texenv, int unit, GenBuffers& buf)
{
    // see if we have a texenv; if so get its blending mode.
    osg::TexEnv::Mode blendingMode = osg::TexEnv::MODULATE;

    if ( accept(texenv) )
    {
        blendingMode = texenv->getMode();

        if ( blendingMode == osg::TexEnv::BLEND )
        {
            std::string texEnvColorUniform = Stringify() << TEXENV_COLOR << unit;
            buf._stateSet
                ->getOrCreateUniform(texEnvColorUniform, osg::Uniform::FLOAT_VEC4)
                ->set( texenv->getColor() );
        }
    }

    // See http://www.opengl.org/sdk/docs/man/xhtml/glTexEnv.xml
    switch( blendingMode )
    {
    case osg::TexEnv::REPLACE:
        buf._fragBody
            << INDENT "color = texel; \n";
        break;
    case osg::TexEnv::MODULATE:
        buf._fragBody
            << INDENT "color = color * texel; \n";
        break;
    case osg::TexEnv::DECAL:
        buf._fragBody
            << INDENT "color.rgb = color.rgb * (1.0 - texel.a) + (texel.rgb * texel.a); \n";
        break;
    case osg::TexEnv::BLEND:
        buf._fragHead
            << "uniform " MEDIUMP "vec4 " TEXENV_COLOR << unit << "\n;";
        buf._fragBody
            << INDENT "color.rgb = color.rgb * (1.0 - texel.rgb) + (" << TEXENV_COLOR << unit << ".rgb * texel.rgb); \n"
            << INDENT "color.a   = color.a * texel.a; \n";
        break;
    case osg::TexEnv::ADD:
    default:
        buf._fragBody
            << INDENT "color.rgb = color.rgb + texel.rgb; \n"
            << INDENT "color.a   = color.a * texel.a; \n";
    }

    return true;
}

bool
ShaderGenerator::apply(osg::TexGen* texgen, int unit, GenBuffers& buf)
{
    bool genDefault = false;

    // by default, do not use texture coordinate generation:
    if ( !accept(texgen) )
    {
        genDefault = true;
    }

    else
    {
        // Handle different TexGen modes.
        // From the GLSL Orange Book.
        switch( texgen->getMode() )
        {
        case osg::TexGen::OBJECT_LINEAR:
            buf._modelBody
                << INDENT "{\n"
                << INDENT TEX_COORD << unit << " = "
                <<      "gl_Vertex.x*gl_ObjectPlaneS[" <<unit<< "] + "
                <<      "gl_Vertex.y*gl_ObjectPlaneT[" <<unit<< "] + "
                <<      "gl_Vertex.z*gl_ObjectPlaneR[" <<unit<< "] + "
                <<      "gl_Vertex.w*gl_ObjectPlaneQ[" <<unit<< "]; \n"
                << INDENT "}\n";
            break;

        case osg::TexGen::EYE_LINEAR:
            buf._viewBody
                << INDENT "{\n"
                << INDENT TEX_COORD << unit << " = "
                <<      "vertex_view.x*gl_EyePlaneS[" <<unit<< "] + "
                <<      "vertex_view.y*gl_EyePlaneT[" <<unit<< "] + "
                <<      "vertex_view.z*gl_EyePlaneR[" <<unit<< "] + "
                <<      "vertex_view.w*gl_EyePlaneQ[" <<unit<< "]; \n"
                << INDENT "}\n";
            break;

        case osg::TexGen::SPHERE_MAP:
            buf._viewHead
                << "vec3 vp_Normal; // stage global\n";
            buf._viewBody
                << INDENT "{\n" // scope it in case there are > 1
                << INDENT "vec3 view_vec = normalize(vertex_view.xyz/vertex_view.w); \n"
                << INDENT "vec3 r = reflect(view_vec, vp_Normal);\n"
                << INDENT "r.z += 1.0; \n"
                << INDENT "float m = 2.0 * sqrt(dot(r,r)); \n"
                << INDENT TEX_COORD << unit << " = vec4(r.x/m + 0.5, r.y/m + 0.5, 0.0, 1.0); \n"
                << INDENT "}\n";
            break;

        case osg::TexGen::REFLECTION_MAP:
            buf._viewHead
                << "vec3 vp_Normal; // stage global\n";
            buf._viewBody
                << INDENT "{\n"
                << INDENT "vec3 view_vec = normalize(vertex_view.xyz/vertex_view.w);\n"
                << INDENT TEX_COORD << unit << " = vec4(reflect(view_vec, vp_Normal), 1.0); \n"
                << INDENT "}\n";
            break;

        case osg::TexGen::NORMAL_MAP:
            buf._viewHead
                << "vec3 vp_Normal; //stage global\n";
            buf._viewBody
                << INDENT "{\n"
                << INDENT TEX_COORD << unit << " = vec4(vp_Normal, 1.0); \n"
                << INDENT "}\n";
            break;

        default: // fall back on non-gen setup.
            genDefault = true;
            break;
        }
    }

    if ( genDefault )
    {
        // GLSL only supports built-in "gl_MultiTexCoord{0..7}"
        if ( unit <= 7 )
        {
            buf._modelBody
                << INDENT << TEX_COORD << unit << " = gl_MultiTexCoord" << unit << ";\n";
        }
        else
        {
            OE_INFO << LC
                << "Texture coordinate on unit (" << unit << ") "
                << "requires a custom vertex attribute (osg_MultiTexCoord" << unit << ")."
                << std::endl;

            buf._modelBody
                << INDENT << TEX_COORD << unit << " = osg_MultiTexCoord" << unit << ";\n";
        }
    }

    return true;
}

bool
ShaderGenerator::apply(osg::TexMat* texmat, int unit, GenBuffers& buf)
{
    if ( accept(texmat) )
    {
        std::string texMatUniform = Stringify() << TEX_MATRIX << unit;

        buf._viewHead << "uniform mat4 " << texMatUniform << ";\n";
        buf._viewBody << INDENT << TEX_COORD << unit << " = " << texMatUniform << " * " << TEX_COORD<<unit << ";\n";

        buf._stateSet
            ->getOrCreateUniform(texMatUniform, osg::Uniform::FLOAT_MAT4)
            ->set( texmat->getMatrix() );
    }

    return true;
}

bool
ShaderGenerator::apply(osg::Texture1D* tex, int unit, GenBuffers& buf)
{
    buf._fragHead << "uniform sampler1D " SAMPLER << unit << ";\n";
    buf._fragBody << INDENT "texel = texture(" SAMPLER << unit << ", " TEX_COORD << unit << ".x);\n";
    buf._stateSet->getOrCreateUniform( Stringify() << SAMPLER << unit, osg::Uniform::SAMPLER_1D )->set( unit );

    return true;
}

bool
ShaderGenerator::apply(osg::Texture2D* tex, int unit, GenBuffers& buf)
{
    buf._fragHead << "uniform sampler2D " SAMPLER << unit << ";\n";
    buf._fragBody << INDENT "texel = texture(" SAMPLER << unit << ", " TEX_COORD << unit << ".xy);\n";
    buf._stateSet->getOrCreateUniform( Stringify() << SAMPLER << unit, osg::Uniform::SAMPLER_2D )->set( unit );
    buf._stateSet->removeTextureMode(unit, GL_TEXTURE_2D);

    return true;
}

bool
ShaderGenerator::apply(osg::Texture3D* tex, int unit, GenBuffers& buf)
{
    buf._fragHead << "uniform sampler3D " SAMPLER << unit << ";\n";
    buf._fragBody << INDENT "texel = texture(" SAMPLER << unit << ", " TEX_COORD << unit << ".xyz);\n";
    buf._stateSet->getOrCreateUniform( Stringify() << SAMPLER << unit, osg::Uniform::SAMPLER_3D )->set( unit );

    return true;
}

bool
ShaderGenerator::apply(osg::TextureRectangle* tex, int unit, GenBuffers& buf)
{
    buf._fragHead << "uniform sampler2DRect " SAMPLER << unit << ";\n";
    buf._fragBody << INDENT "texel = texture(" SAMPLER << unit << ", " TEX_COORD << unit << ".xy);\n";
    buf._stateSet->getOrCreateUniform( Stringify() << SAMPLER << unit, osg::Uniform::SAMPLER_2D )->set( unit );

    return true;
}

bool
ShaderGenerator::apply(osg::Texture2DArray* tex, int unit, GenBuffers& buf)
{
    buf._fragHead << "uniform sampler2DArray " SAMPLER << unit << ";\n";
    buf._stateSet->getOrCreateUniform( Stringify() << SAMPLER << unit, osg::Uniform::SAMPLER_2D_ARRAY )->set( unit );
    buf._fragBody << INDENT "texel = texture(" SAMPLER << unit << ", " TEX_COORD << unit << ".xyz);\n";

    return true;
}

bool
ShaderGenerator::apply(osg::TextureCubeMap* tex, int unit, GenBuffers& buf)
{
    std::string sampler = Stringify() << SAMPLER << unit;
    buf._fragHead << "uniform samplerCube " << sampler << ";\n";
    buf._fragBody << INDENT "texel = texture(" << sampler << ", " TEX_COORD << unit << ".xyz);\n";
    buf._stateSet->getOrCreateUniform( sampler, osg::Uniform::SAMPLER_CUBE )->set( unit );

    return true;
}

bool
ShaderGenerator::apply(osg::PointSprite* tex, int unit, GenBuffers& buf)
{
    std::string sampler = Stringify() << SAMPLER << unit;
    buf._fragHead << "uniform sampler2D " << sampler << ";\n";
    buf._fragBody << INDENT << "texel = texture(" << sampler << ", gl_PointCoord);\n";
    buf._stateSet->getOrCreateUniform( sampler, osg::Uniform::SAMPLER_2D )->set( unit );

    return true;
}

bool
ShaderGenerator::apply(osg::StateSet::AttributeList& attrs, GenBuffers& buf)
{
    bool addedSomething = false;

    for(osg::StateSet::AttributeList::iterator i = attrs.begin(); i != attrs.end(); ++i)
    {
        osg::StateAttribute* attr = i->second.first.get();
        if ( apply(attr, buf) )
        {
            addedSomething = true;
        }
    }

    return addedSomething;
}

bool
ShaderGenerator::apply(osg::StateAttribute* attr, GenBuffers& buf)
{
    bool addedSomething = false;

#if defined(OSG_GLES3_AVAILABLE)
    osg::Point* asPoint = dynamic_cast<osg::Point*>(attr);
    if(asPoint != NULL)
    {
        buf._viewHead << "uniform float oe_sg_pointSize;\n";
        buf._viewBody << INDENT << "gl_PointSize = oe_sg_pointSize;\n";
        buf._stateSet->getOrCreateUniform( "oe_sg_pointSize", osg::Uniform::FLOAT )->set( asPoint->getSize() );
        addedSomething = true;
    }
#endif
    
    return addedSomething;
}

void
ShaderGenerator::disableUnsupportedAttributes(osg::StateSet* stateset)
{
    if (!stateset)
        return;

    stateset->removeAttribute(osg::StateAttribute::SHADEMODEL);
}
