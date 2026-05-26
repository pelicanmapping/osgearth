/* osgEarth
* Copyright 2008-2012 Pelican Mapping
* MIT License
*/
#include <osgEarth/ContourMap>
#include <osgEarth/Shaders>
#include <osgEarth/Registry>
#include <osgEarth/TerrainEngineNode>
#include <osgEarth/VerticalDatum>
#include <osgDB/WriteFile>

#define LC "[ContourMap] "

using namespace osgEarth;

//........................................................................

Config
ContourMapLayer::Options::getMetadata()
{
    return Config::readJSON(R"(
        { "name" : "ContourMap",
          "properties" : [
          ]
        }
    )");
}

Config
ContourMapLayer::Options::getConfig() const
{
    Config conf = VisibleLayer::Options::getConfig();
    Config stopsConf("stops");
    for (auto& stop : stops())
    {
        Config s("stop");
        s.set("elevation", stop.elevation);
        s.set("color", stop.color);
        stopsConf.add(s);
    }
    if (!stopsConf.empty())
        conf.add(stopsConf);
    conf.set("vdatum", vdatum());
    return conf;
}

void
ContourMapLayer::Options::fromConfig(const Config& conf)
{
    stops().clear();
    const ConfigSet& stopsConf = conf.child("stops").children();
    for (auto& stop : stopsConf)
    {
        Stop s;
        if (stop.get("elevation", s.elevation) && stop.get("color", s.color))
            stops().emplace_back(s);
    }
    conf.get("vdatum", vdatum());
}

//........................................................................

REGISTER_OSGEARTH_LAYER(contourmap, ContourMapLayer);

void
ContourMapLayer::setTransferFunction(osg::TransferFunction1D* xfer)
{
    _xfer = xfer;
    dirty();
}

void
ContourMapLayer::dirty()
{
    OE_SOFT_ASSERT_AND_RETURN(_xfer.valid() && _xferMin.valid() && _xferRange.valid(), void());

    _xferTexture->setImage(_xfer->getImage());
    _xferMin->set(_xfer->getMinimum());
    _xferRange->set(_xfer->getMaximum() - _xfer->getMinimum());
}

void
ContourMapLayer::init()
{
    super::init();

    setRenderType(RENDERTYPE_TERRAIN_SURFACE);

    osg::StateSet* stateset = getOrCreateStateSet();

    // uniforms we'll need:
    _xferMin = new osg::Uniform(osg::Uniform::FLOAT, "oe_contour_min");
    stateset->addUniform(_xferMin.get());

    _xferRange = new osg::Uniform(osg::Uniform::FLOAT, "oe_contour_range");
    stateset->addUniform(_xferRange.get());

    _xferSampler = new osg::Uniform(osg::Uniform::SAMPLER_2D, "oe_contour_xfer");
    stateset->addUniform(_xferSampler.get());

    // Create a texture from the transfer function's image.
    _xferTexture = new TextureType();
    _xferTexture->setResizeNonPowerOfTwoHint(false);
    _xferTexture->setUseHardwareMipMapGeneration(false);
    _xferTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
    _xferTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    _xferTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    _xferTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    // defer installing the texture until we get a unit

    // shaders:
    VirtualProgram* vp = VirtualProgram::getOrCreate(stateset);
    vp->setName(typeid(*this).name());
    Shaders pkg;
    pkg.load(vp, pkg.ContourMap);

    // build a transfer function.
    osg::TransferFunction1D* xfer = new osg::TransferFunction1D();
    xfer->allocate(1024);

    if (options().stops().empty() == false)
    {
        for (auto& stop : options().stops())
        {
            xfer->setColor(stop.elevation, stop.color, false);
        }
    }
    else
    {
        xfer->setColor(-7500.0f, Color("#000016"));
        xfer->setColor(-2500.0f, Color("#00007f"));
        xfer->setColor(-625.0f, Color("#0000ff"));
        xfer->setColor(-1.0f, Color("#007fff"));
        xfer->setColor(0.0f, Color("#c2b280"));
        xfer->setColor(50.0f, Color("#d6d63f"));
        xfer->setColor(250.0f, Color("#1f9e00"));
        xfer->setColor(1000.0f, Color("#30632c"));
        xfer->setColor(1750.0f, Color("#a37714"));
        xfer->setColor(2250.0f, Color("#6c2f2f"));
        xfer->setColor(2600.0f, Color("#7f7f7f"));
        xfer->setColor(3000.0f, Color("#ffffff"));
        xfer->setColor(5500.0f, Color("#83eeff"));
        xfer->setColor(8000.0f, Color("#ff1d7f"));
    }

    xfer->updateImage();
    this->setTransferFunction(xfer);
}

void
ContourMapLayer::prepareForRendering(TerrainEngine* engine)
{
    super::prepareForRendering(engine);

    if (!engine->getResources()->reserveTextureImageUnitForLayer(_reservationColorRamp, this, "ContourMap"))
    {
        setStatus(Status::ResourceUnavailable, "No texture image units available");
        return;
    }

    osg::StateSet* stateset = getOrCreateStateSet();
    stateset->setTextureAttribute(_reservationColorRamp.unit(), _xferTexture.get());
    _xferSampler->set(_reservationColorRamp.unit());

    if (!options().vdatum()->empty())
    {
        auto* vdatum = VerticalDatum::get(options().vdatum().get());
        if (vdatum)
        {
            if (!engine->getResources()->reserveTextureImageUnitForLayer(_reservationGeoid, this, "ContourMap"))
            {
                setStatus(Status::ResourceUnavailable, "No texture image units available");
                return;
            }

            auto* geoid = vdatum->getGeoid();
            OE_SOFT_ASSERT_AND_RETURN(geoid, void());

            auto* hf = geoid->getHeightField();
            OE_SOFT_ASSERT_AND_RETURN(hf, void());

            auto image = new osg::Image();
            image->allocateImage(hf->getNumColumns(), hf->getNumRows(), 1, GL_RED, GL_FLOAT);
            memcpy(image->data(), hf->getFloatArray()->getDataPointer(), hf->getFloatArray()->getTotalDataSize());

            auto tex = new osg::Texture2D(image);
            tex->setInternalFormat(GL_R32F);
            tex->setResizeNonPowerOfTwoHint(false);
            tex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            tex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            tex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            tex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);

            stateset->setTextureAttribute(_reservationGeoid.unit(), tex);
            stateset->addUniform(new osg::Uniform("oe_contour_geoid", _reservationGeoid.unit()));
            stateset->setDefine("OE_USE_GEOID");

            osgDB::writeImageFile(*image, "geoid.tif");
        }
    }
}

Status
ContourMapLayer::closeImplementation()
{
    _reservationColorRamp.release();
    _reservationGeoid.release();

    return super::closeImplementation();
}
