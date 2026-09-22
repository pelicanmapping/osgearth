/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <osgEarth/Lighting>
#include <osgEarth/StringUtils>
#include <osgEarth/GLUtils>

#include <osgUtil/CullVisitor>
#include <osgDB/ObjectWrapper>
#include <osg/Light>
#include <osg/Material>
#include <algorithm>

using namespace osgEarth;
using namespace osgEarth::Util;

#define LC "[Lighting] "

// prefix to use for uniforms.
#define UPREFIX "osg_"


//............................................................................

void
Lighting::set(osg::StateSet* stateSet, osg::StateAttribute::OverrideValue value)
{
    GLUtils::setLighting(stateSet, value);
}

void
Lighting::remove(osg::StateSet* stateSet)
{
    GLUtils::remove(stateSet, GL_LIGHTING);
}

void
Lighting::installDefaultMaterial(osg::StateSet* stateSet)
{
    osg::Material* m = new osgEarth::MaterialGL3();
    m->setDiffuse(m->FRONT, osg::Vec4(1,1,1,1));
    m->setAmbient(m->FRONT, osg::Vec4(1,1,1,1));
    stateSet->setAttributeAndModes(m, 1);
    osgEarth::MaterialCallback().operator()(m, 0L);
}

//............................................................................

GenerateGL3LightingUniforms::GenerateGL3LightingUniforms() :
    osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
{
    setNodeMaskOverride(~0);
}

void
GenerateGL3LightingUniforms::apply(osg::Node& node)
{
    osg::StateSet* stateset = node.getStateSet();
    if (stateset)
    {
        if (_statesets.find(stateset) == _statesets.end())
        {
            const osg::StateSet::RefAttributePair* rap = stateset->getAttributePair(osg::StateAttribute::MATERIAL);
            if (rap)
            {
                osg::Material* material = dynamic_cast<osg::Material*>(rap->first.get());
                if (material)
                {
                    osg::Material* mat = material;

    #if !defined(OSG_GL_FIXED_FUNCTION_AVAILABLE)
                    // If there's no FFP, we need to replace the Material with a GL3 Material to prevent
                    // error messages on the console.
                    if (dynamic_cast<MaterialGL3*>(material) == 0L)
                    {
                        mat = new MaterialGL3(*material);
                        stateset->setAttributeAndModes(mat, rap->second);
                    }
    #endif

                    // Install the MaterialCallback so uniforms are updated.
                    if (!mat->getUpdateCallback())
                    {
                        if (stateset->getDataVariance() == osg::Object::DYNAMIC)
                        {
                            mat->setUpdateCallback(new MaterialCallback());
                        }
                        else
                        {
                            MaterialCallback mc;
                            mc.operator()(mat, NULL);
                        }
                    }
                }

                // mark this stateset as visited.
                _statesets.insert(stateset);
            }
        }
    }
    traverse(node);
}

void
GenerateGL3LightingUniforms::apply(osg::LightSource& lightSource)
{
    if (lightSource.getLight())
    {
        if (!alreadyInstalled<LightSourceGL3UniformGenerator>(lightSource.getCullCallback()))
        {
            lightSource.addCullCallback(new LightSourceGL3UniformGenerator());
        }

        // If there's no FFP, we need to replace the Light with a LightGL3 to prevent
        // error messages on the console.
        if (dynamic_cast<LightGL3*>(lightSource.getLight()) == 0L)
        {
            lightSource.setLight(new LightGL3(*lightSource.getLight()));
        }
    }

    apply(static_cast<osg::Node&>(lightSource));
}

//............................................................................

LightSourceGL3UniformGenerator::LightSourceGL3UniformGenerator() :
    _statesetsMutex()
{
    //nop
}

bool
LightSourceGL3UniformGenerator::run(osg::Object* obj, osg::Object* data)
{
    osg::LightSource* lightSource = dynamic_cast<osg::LightSource*>(obj);
    osgUtil::CullVisitor* cv = dynamic_cast<osgUtil::CullVisitor*>(data);

    if (cv && lightSource && lightSource->getLight())
    {
        osg::Light* light = lightSource->getLight();

        // replace the index with the light number:
        //std::string prefix = Stringify() << UPREFIX << "LightSource[" << light->getLightNum() << "].";
        std::string prefix;
        if (light->getLightNum() < 10)
        {
            prefix = UPREFIX "LightSource[#].";
            prefix[prefix.length() - 3] = (char)('0' + light->getLightNum());
        }
        else
        {
            prefix = UPREFIX "LightSource[##].";
            int lightNumTens = light->getLightNum()/10;
            prefix[prefix.length() - 4] = (char)('0'+lightNumTens);
            prefix[prefix.length() - 3] = (char)('0' +(light->getLightNum()-(10*lightNumTens)));
        }

        // Lights are positional state so their location in the scene graph is only important
        // in terms of model transformation, and not in terms of what gets lit.
        // Place these uniforms at the root stateset so they affect the entire graph:
        osg::StateSet* ss = cv->getCurrentRenderStage()->getStateSet();
        if (ss == 0L)
        {
            cv->getCurrentRenderStage()->setStateSet(ss = new osg::StateSet());

            std::lock_guard<std::mutex> lock(_statesetsMutex);
            _statesets.push_back(ss);
        }

        ss->getOrCreateUniform(prefix + "ambient", osg::Uniform::FLOAT_VEC4)->set(light->getAmbient());
        ss->getOrCreateUniform(prefix + "diffuse", osg::Uniform::FLOAT_VEC4)->set(light->getDiffuse());
        ss->getOrCreateUniform(prefix + "specular", osg::Uniform::FLOAT_VEC4)->set(light->getSpecular());

        // add the positional elements:
        const osg::Matrix& mvm = *cv->getModelViewMatrix();
        ss->getOrCreateUniform(prefix + "position", osg::Uniform::FLOAT_VEC4)->set(light->getPosition() * mvm);
        osg::Vec3 directionLocal = osg::Matrix::transform3x3(light->getDirection(), mvm);
        directionLocal.normalize();
        ss->getOrCreateUniform(prefix + "spotDirection", osg::Uniform::FLOAT_VEC3)->set(directionLocal);

        ss->getOrCreateUniform(prefix + "spotExponent", osg::Uniform::FLOAT)->set(light->getSpotExponent());
        ss->getOrCreateUniform(prefix + "spotCutoff", osg::Uniform::FLOAT)->set(light->getSpotCutoff());
        ss->getOrCreateUniform(prefix + "spotCosCutoff", osg::Uniform::FLOAT)->set(cosf(light->getSpotCutoff()));
        ss->getOrCreateUniform(prefix + "constantAttenuation", osg::Uniform::FLOAT)->set(light->getConstantAttenuation());
        ss->getOrCreateUniform(prefix + "linearAttenuation", osg::Uniform::FLOAT)->set(light->getLinearAttenuation());
        ss->getOrCreateUniform(prefix + "quadraticAttenuation", osg::Uniform::FLOAT)->set(light->getQuadraticAttenuation());

        LightGL3* lightGL3 = dynamic_cast<LightGL3*>(light);
        bool enabled = lightGL3 ? lightGL3->getEnabled() : true;
        ss->getOrCreateUniform(prefix + "enabled", osg::Uniform::BOOL)->set(enabled);

        osg::Uniform* fsu = ss->getOrCreateUniform("oe_lighting_framestamp", osg::Uniform::UNSIGNED_INT);
        unsigned fs;
        fsu->get(fs);

        osg::StateSet::DefinePair* numLights = ss->getDefinePair("OE_NUM_LIGHTS");

        // Size the shader array by its highest occupied index, not by the number of visited sources.
        // Sparse indices and repeated visits to one light must not truncate or inflate the array.
        int capacity = std::max(1, light->getLightNum() + 1);
        if (fs == cv->getFrameStamp()->getFrameNumber() && numLights)
            capacity = std::max(capacity, ::atoi(numLights->first.c_str()));
        ss->setDefine("OE_NUM_LIGHTS", Stringify() << capacity, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        fsu->set(cv->getFrameStamp()->getFrameNumber());
    }
    return traverse(obj, data);
}

void
LightSourceGL3UniformGenerator::resizeGLBufferObjects(unsigned maxSize)
{
    std::lock_guard<std::mutex> lock(_statesetsMutex);
    for(unsigned i=0; i<_statesets.size(); ++i)
        _statesets[i]->resizeGLObjectBuffers(maxSize);
}

void
LightSourceGL3UniformGenerator::releaseGLObjects(osg::State* state) const
{
    std::lock_guard<std::mutex> lock(_statesetsMutex);
    for(unsigned i=0; i<_statesets.size(); ++i)
        _statesets[i]->releaseGLObjects(state);
    _statesets.clear();
}

//............................................................................
void MaterialCallback::operator() (osg::StateAttribute* attr, osg::NodeVisitor* nv)
{
    static const std::string AMBIENT = UPREFIX "FrontMaterial.ambient";
    static const std::string DIFFUSE = UPREFIX "FrontMaterial.diffuse";
    static const std::string SPECULAR = UPREFIX "FrontMaterial.specular";
    static const std::string EMISSION = UPREFIX "FrontMaterial.emission";
    static const std::string SHININESS = UPREFIX "FrontMaterial.shininess";

    osg::Material* material = static_cast<osg::Material*>(attr);
    for (unsigned int i = 0; i < attr->getNumParents(); i++)
    {
        osg::StateSet* stateSet = attr->getParent(i);

        stateSet->getOrCreateUniform(AMBIENT, osg::Uniform::FLOAT_VEC4)->set(material->getAmbient(osg::Material::FRONT));
        stateSet->getOrCreateUniform(DIFFUSE, osg::Uniform::FLOAT_VEC4)->set(material->getDiffuse(osg::Material::FRONT));
        stateSet->getOrCreateUniform(SPECULAR, osg::Uniform::FLOAT_VEC4)->set(material->getSpecular(osg::Material::FRONT));
        stateSet->getOrCreateUniform(EMISSION, osg::Uniform::FLOAT_VEC4)->set(material->getEmission(osg::Material::FRONT));
        stateSet->getOrCreateUniform(SHININESS, osg::Uniform::FLOAT)->set(material->getShininess(osg::Material::FRONT));

        //TODO: back-face materials
    }
}

//............................................................................

void
LightGL3::apply(osg::State& state) const
{
#ifdef OSG_GL_FIXED_FUNCTION_AVAILABLE
    osg::Light::apply(state);
#endif
}

// Serializer for LightGL3.
// The odd namespace name is here because this macro cannot be used more than once
// in the same namespace in the same cpp file.
namespace osgEarth { namespace Serializers { namespace LightGL3
{
    REGISTER_OBJECT_WRAPPER(
        LightGL3,
        new osgEarth::LightGL3,
        osgEarth::LightGL3,
        "osg::Object osg::StateAttribute osg::Light osgEarth::LightGL3")
    {
        ADD_BOOL_SERIALIZER(Enabled, true);
    }
} } }

//............................................................................

void
MaterialGL3::apply(osg::State& state) const
{
#ifdef OSG_GL_FIXED_FUNCTION_AVAILABLE
    osg::Material::apply(state);
#else
    state.Color(_diffuseFront.r(), _diffuseFront.g(), _diffuseFront.b(), _diffuseFront.a());
#endif
}

// Serializer for MaterialGL3.
// The odd namespace name is here because this macro cannot be used more than once
// in the same namespace in the same cpp file.
namespace osgEarth { namespace Serializers { namespace MaterialGL3
{
    REGISTER_OBJECT_WRAPPER(
        MaterialGL3,
        new osgEarth::MaterialGL3,
        osgEarth::MaterialGL3,
        "osg::Object osg::StateAttribute osg::Material osgEarth::MaterialGL3") { }
} } }
