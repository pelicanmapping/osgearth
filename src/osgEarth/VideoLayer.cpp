/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <osgEarth/VideoLayer>
#include <osg/ImageStream>
#include <osgEarth/Registry>

using namespace osgEarth;

//.......................................................................

Config
VideoLayer::Options::getConfig() const
{
    Config conf = ImageLayer::Options::getConfig();
    conf.set("url", _url);
    return conf;
}

void
VideoLayer::Options::fromConfig( const Config& conf )
{
    conf.get("url", _url );
}

//-------------------------------------------------------------

REGISTER_OSGEARTH_LAYER(video, VideoLayer);

OE_LAYER_PROPERTY_IMPL(VideoLayer, URI, URL, url);

void
VideoLayer::init()
{
    super::init();
    
    // Configure the layer to use createTexture() to return data
    setUseCreateTexture();
}

Status
VideoLayer::openImplementation()
{
    Status parent = super::openImplementation();
    if (parent.isError())
        return parent;

    if (!options().url().isSet())
    {
        return Status(Status::ConfigurationError, "Missing required url");
    }

    osg::ref_ptr< osg::Image > image = options().url()->readImage(getReadOptions()).getImage();
    if (image.valid())
    {
        osg::ImageStream* is = dynamic_cast<osg::ImageStream*>(image.get());
        if (is)
        {
            is->setLoopingMode(osg::ImageStream::LOOPING);
            is->play();
        }

        _texture = new osg::Texture2D(image);
        _texture->setResizeNonPowerOfTwoHint(false);
        _texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture2D::LINEAR);
        _texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture2D::LINEAR);
        _texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        _texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        _texture->setUnRefImageDataAfterApply(false);
    }
    else
    {
        std::stringstream buf;
        buf << "Failed to load " << options().url()->full();
        return Status(Status::ServiceUnavailable, buf.str());
    }

    setProfile(Profile::create(Profile::GLOBAL_GEODETIC));

    return getStatus();
}

TextureWindow
VideoLayer::createTexture(const TileKey& key, ProgressCallback* progress) const
{
    OE_SOFT_ASSERT_AND_RETURN(isOpen(), {});
    OE_SOFT_ASSERT_AND_RETURN(_texture.valid(), {});

    osg::Matrix textureMatrix;
    bool flip = _texture->getImage()->getOrigin() == osg::Image::TOP_LEFT;    
    key.getExtent().createScaleBias(key.getProfile()->getExtent(), textureMatrix);
    if (flip)
    {
        textureMatrix *= osg::Matrixf::scale(1.0, flip ? -1.0 : 1.0, 1.0);
    }  
    return TextureWindow(_texture.get(), textureMatrix);
}