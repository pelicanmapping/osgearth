/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/LayerShader>
#include <osgEarth/ShaderLoader>
#include <osgEarth/Shaders>
#include <osgEarth/VirtualProgram>
#include <osgEarth/Color>
#include <osgEarth/Layer>
#include <osg/Texture2D>
#include <osg/Texture2DArray>

#undef  LC
#define LC "[LayerShader] "

using namespace osgEarth;
using namespace osgEarth::Util;

//...................................................................

Config
ShaderOptions::getConfig() const
{
    Config conf = ConfigOptions::getConfig();

    if (!_code.empty())
    {
        conf.setValue(_code);
    }

    conf.remove("sampler");
    for (unsigned i = 0; i < _samplers.size(); ++i) {
        Config c("sampler");
        c.add("name", _samplers[i]._name);
        if (_samplers[i]._uris.size() > 1) {
            Config urlarray("array");
            c.add(urlarray);
            for (std::vector<URI>::const_iterator j = _samplers[i]._uris.begin(); j != _samplers[i]._uris.end(); ++j) {
                urlarray.add(j->getConfig());
            }
        }
        else if (_samplers[i]._uris.size() == 1) {
            c.add("url", _samplers[i]._uris.back().getConfig());
        }
        conf.add(c);
    }

    conf.remove("uniform");
    for (unsigned i = 0; i < _uniforms.size(); ++i) {
        Config c("uniform");
        c.set("name", _uniforms[i]._name);  
        if (_uniforms[i]._floatValue.isSet())
            c.set("value", _uniforms[i]._floatValue);
        else if (_uniforms[i]._vec3Value.isSet())
            c.set("value", Color(_uniforms[i]._vec3Value.get()).toHTML());
        conf.add(c);
    }

    return conf;
}

void
ShaderOptions::fromConfig(const Config& conf)
{
    _code = conf.value();

    _samplers.clear();
    ConfigSet s = conf.children("sampler");
    for (ConfigSet::const_iterator i = s.begin(); i != s.end(); ++i) {
        _samplers.push_back(Sampler());
        _samplers.back()._name = i->value("name");
        const Config* urlarray = i->find("array");
        if (urlarray) {
            ConfigSet uris = urlarray->children("url");
            for (ConfigSet::const_iterator j = uris.begin(); j != uris.end(); ++j) {
                URI uri(j->value(), URIContext(conf.referrer()));
                _samplers.back()._uris.push_back(uri);
            }
        }
        else {
            optional<URI> uri;
            i->get("url", uri);
            if (uri.isSet())
                _samplers.back()._uris.push_back(uri.get());
        }
    }

    s = conf.children("uniform");
    for (ConfigSet::const_iterator i = s.begin(); i != s.end(); ++i)
    {
        _uniforms.push_back(Uniform());
        _uniforms.back()._name = i->value("name");
        std::string value = i->value("value");
        if (!value.empty() && value[0] == '#') 
        {
            Color color(value);
            _uniforms.back()._vec3Value = osg::Vec3f(color.r(), color.g(), color.b());
        }
        else
        {
            i->get("value", _uniforms.back()._floatValue);
        }
    }

    if (conf.hasChild("material"))
    {
        auto& child = conf.child("material");
        _pbrsampler.mutable_value()._name = child.value("name");
        _pbrsampler.mutable_value()._material = PBRMaterial(child);
    }
}

//...................................................................

LayerShader::LayerShader(const ShaderOptions& options) :
_options(options)
{
    //nop
}

LayerShader::~LayerShader()
{
    //nop
}

void
LayerShader::install(Layer* layer, TerrainResources* res)
{
    if (!layer || !res)
        return;

    osg::StateSet* stateset = layer->getOrCreateStateSet();

    VirtualProgram* vp = VirtualProgram::getOrCreate(stateset);
    vp->setName(layer->getName());
    Shaders package;
    package.add("", _options.code());

    ShaderLoader::load(vp, "", package, layer->getReadOptions());
    //package.loadAll(vp, layer->getReadOptions());

    for (int i = 0; i < _options.samplers().size(); ++i)
    {
        const ShaderOptions::Sampler& sampler = _options.samplers()[i];
        if (!sampler._name.empty())
        {
            _reservations.push_back(TextureImageUnitReservation());
            TextureImageUnitReservation& reservation = _reservations.back();

            if (sampler._uris.size() == 1) // Texture2D
            {
                if (res->reserveTextureImageUnitForLayer(reservation, layer, "User shader sampler"))
                {
                    osg::Image* image = sampler._uris[0].getImage(layer->getReadOptions());
                    if (image)
                    {
                        osg::Texture2D* tex = new osg::Texture2D(image);
                        tex->setFilter(tex->MIN_FILTER, tex->NEAREST_MIPMAP_LINEAR);
                        tex->setFilter(tex->MAG_FILTER, tex->LINEAR);
                        tex->setWrap(tex->WRAP_S, tex->REPEAT);
                        tex->setWrap(tex->WRAP_T, tex->REPEAT);
                        tex->setUnRefImageDataAfterApply(true);
                        tex->setMaxAnisotropy(4.0);
                        tex->setResizeNonPowerOfTwoHint(false);

                        stateset->setTextureAttribute(reservation.unit(), tex);
                        stateset->addUniform(new osg::Uniform(sampler._name.c_str(), reservation.unit()));
                    }
                }
                else
                {
                    OE_WARN << LC << "Failed to allocate a texture image unit for this terrain shader sampler!\n";
                }
            }

            else if (sampler._uris.size() > 1) // Texture2DArray
            {
                if (res->reserveTextureImageUnitForLayer(reservation, layer, "User shader sampler array"))
                {
                    int sizeX = 0, sizeY = 0;
                    osg::Texture2DArray* tex = new osg::Texture2DArray();
                    tex->setTextureSize(512, 512, sampler._uris.size());
                    tex->setTextureDepth(sampler._uris.size());

                    for (int j = 0; j < sampler._uris.size(); ++j)
                    {
                        const URI& uri = sampler._uris[j];

                        osg::ref_ptr<osg::Image> image = uri.getImage(layer->getReadOptions());
                        if (image)
                        {
                            OE_DEBUG << LC << "   Added image from \"" << uri.full() << "\"\n";
                            tex->setImage(i, image);
                            tex->setFilter(tex->MIN_FILTER, tex->NEAREST_MIPMAP_LINEAR);
                            tex->setFilter(tex->MAG_FILTER, tex->LINEAR);
                            tex->setWrap(tex->WRAP_S, tex->CLAMP_TO_EDGE);
                            tex->setWrap(tex->WRAP_T, tex->CLAMP_TO_EDGE);
                            tex->setUnRefImageDataAfterApply(true);
                            tex->setResizeNonPowerOfTwoHint(false);

                            stateset->setTextureAttribute(reservation.unit(), tex);
                            stateset->addUniform(new osg::Uniform(sampler._name.c_str(), reservation.unit()));

                            if (sizeX == 0)
                            {
                                sizeX = image->s();
                                sizeY = image->t();
                                tex->setTextureSize(sizeX, sizeY, sampler._uris.size());
                            }
                        }
                    }
                }
                else
                {
                    OE_WARN << LC << "Failed to allocate a texture image unit for this terrain shader sampler!\n";
                }
            }
        }
    }

    for (int i = 0; i < _options.uniforms().size(); ++i)
    {
        const ShaderOptions::Uniform& uniform = _options.uniforms()[i];
        if (!uniform._name.empty())
        {
            if (uniform._floatValue.isSet())
            {
                osg::Uniform* u = new osg::Uniform(uniform._name.c_str(), (float)uniform._floatValue.get());
                stateset->addUniform(u);
            }
            else if (uniform._vec3Value.isSet())
            {
                osg::Uniform* u = new osg::Uniform(uniform._name.c_str(), uniform._vec3Value.get());
                stateset->addUniform(u);
            }
        }
    }

    if (_options.pbrsampler().isSet())
    {
        auto& pbrsampler = _options.pbrsampler().get();

        PBRTexture textures;
        textures.load(pbrsampler._material);

        _reservations.reserve(4);
        // Custom shaders may include PBRMaterial.glsl and decode the raw sampler.
        stateset->addUniform(new osg::Uniform((pbrsampler._name + "_layoutAndFactors").c_str(), textures.layoutAndFactors));

        if (textures.albedo)
        {
            _reservations.emplace_back();
            auto& reservation = _reservations.back();

            if (res->reserveTextureImageUnitForLayer(reservation, layer, "User shader albedo sampler"))
            {
                stateset->setTextureAttribute(reservation.unit(), textures.albedo);
                stateset->addUniform(new osg::Uniform((pbrsampler._name + "_albedo").c_str(), reservation.unit()));
            }
        }

        if (textures.normal)
        {
            _reservations.emplace_back();
            auto& reservation = _reservations.back();

            if (res->reserveTextureImageUnitForLayer(reservation, layer, "User shader normal sampler"))
            {
                stateset->setTextureAttribute(reservation.unit(), textures.normal);
                stateset->addUniform(new osg::Uniform((pbrsampler._name + "_normal").c_str(), reservation.unit()));
            }
        }

        if (textures.pbr)
        {
            _reservations.emplace_back();
            auto& reservation = _reservations.back();

            if (res->reserveTextureImageUnitForLayer(reservation, layer, "User shader pbr sampler"))
            {
                stateset->setTextureAttribute(reservation.unit(), textures.pbr);
                stateset->addUniform(new osg::Uniform((pbrsampler._name + "_pbr").c_str(), reservation.unit()));
            }
        }
        if (textures.occlusion)
        {
            _reservations.emplace_back();
            auto& reservation = _reservations.back();
            if (res->reserveTextureImageUnitForLayer(reservation, layer, "User shader occlusion sampler"))
            {
                stateset->setTextureAttribute(reservation.unit(), textures.occlusion);
                stateset->addUniform(new osg::Uniform((pbrsampler._name + "_occlusion").c_str(), reservation.unit()));
            }
        }
    }
}

//...................................................................
