/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/FeatureImageLayer>
#include <osgEarth/Session>
#include <osgEarth/StyleSheet>
#include <osgEarth/Registry>
#include <osgEarth/Progress>
#include <osgEarth/LandCover>
#include <osgEarth/FeatureStyleSorter>
#include <osgEarth/BuildConfig>
#include <osgEarth/Shaders>
#include <osgEarth/VirtualProgram>
#include <osgEarth/StringUtils>

#ifdef OSGEARTH_HAVE_SLUGHORN
#include "FeatureImageLayerSlug.h"
#endif

using namespace osgEarth;

#define LC "[FeatureImageLayer] " << getName() << ": "


REGISTER_OSGEARTH_LAYER(featureimage, FeatureImageLayer);
REGISTER_OSGEARTH_LAYER(feature_image, FeatureImageLayer);

//........................................................................

Config
FeatureImageLayer::Options::getConfig() const
{
    Config conf = ImageLayer::Options::getConfig();
    featureSource().set(conf, "features");
    styleSheet().set(conf, "styles");
    conf.set("buffer_width", bufferWidth());
    conf.set("background_color", backgroundColor());
    conf.set("rendering", renderingTechnique());

    if (filters().empty() == false)
    {
        Config temp;
        for (unsigned i = 0; i < filters().size(); ++i)
            temp.add(filters()[i].getConfig());
        conf.set("filters", temp);
    }

    return conf;
}

void
FeatureImageLayer::Options::fromConfig(const Config& conf)
{
    featureSource().get(conf, "features");
    styleSheet().get(conf, "styles");
    conf.get("buffer_width", bufferWidth());
    conf.get("background_color", backgroundColor());
    conf.get("rendering", renderingTechnique());

    const Config& filtersConf = conf.child("filters");
    for (ConfigSet::const_iterator i = filtersConf.children().begin(); i != filtersConf.children().end(); ++i)
        filters().push_back(ConfigOptions(*i));
}

//........................................................................

FeatureImageLayer::Isolate::Isolate(const Isolate& rhs) :
    _session(rhs._session),
    _filterChain(rhs._filterChain)
{
}

void
FeatureImageLayer::init()
{
    ImageLayer::init();

    setUseCreateTexture(usesSlugRendering());

    if (usesSlugRendering())
    {
        // Slug payloads are tile-local atlases rather than sampled colors. Parent
        // imagery morphing would require blending two independently packed atlases.
        setMorphImagery(false);
    }

    // Do not set the profile here. We will try to deduce the profile
    // once we can open the underlying feature source.
}

Status
FeatureImageLayer::openImplementation()
{
    Status parent = ImageLayer::openImplementation();
    if (parent.isError())
        return parent;

    if (!ciEquals(options().renderingTechnique().get(), "raster") &&
        !ciEquals(options().renderingTechnique().get(), "slug"))
    {
        return Status(Status::ConfigurationError,
            "rendering must be either 'raster' or 'slug'");
    }

#ifndef OSGEARTH_HAVE_SLUGHORN
    if (usesSlugRendering())
    {
        return Status(Status::ConfigurationError,
            "Slug rendering is not available in this build");
    }
#endif

    // assert a feature source:
    Status fsStatus = options().featureSource().open(getReadOptions());
    if (fsStatus.isError())
        return fsStatus;

    Status ssStatus = options().styleSheet().open(getReadOptions());
    if (ssStatus.isError())
        return ssStatus;

    establishProfile();

    _global._filterChain = FeatureFilterChain::create(
        options().filters(), 
        getReadOptions());

    return Status::NoError;
}

void
FeatureImageLayer::establishProfile()
{
    if (getProfile() == nullptr && getFeatureSource() != nullptr)
    {
        const FeatureProfile* fp = getFeatureSource()->getFeatureProfile();

        if (fp && fp->getTilingProfile())
        {
            setProfile(fp->getTilingProfile());
        }
        else if (fp && fp->getSRS())
        {
            setProfile(Profile::create(fp->getSRS()));
        }
        else
        {
            setProfile(Profile::create(Profile::GLOBAL_GEODETIC));
        }
    }
}

void
FeatureImageLayer::addedToMap(const Map* map)
{
    ImageLayer::addedToMap(map);

    options().featureSource().addedToMap(map);
    options().styleSheet().addedToMap(map);

    if (getFeatureSource())
    {
        establishProfile();

        _global._session = new Session(
            map, 
            getStyleSheet(), 
            getFeatureSource(), 
            getReadOptions());

        updateSession();
    }
}

void
FeatureImageLayer::removedFromMap(const Map* map)
{
    options().featureSource().removedFromMap(map);
    options().styleSheet().removedFromMap(map);

    ImageLayer::removedFromMap(map);
}

void
FeatureImageLayer::setFeatureSource(FeatureSource* fs)
{
    if (getFeatureSource() != fs)
    {
        options().featureSource().setLayer(fs);
        //_data._featureProfile = 0L;

        if (fs)
        {
            if (fs->getStatus().isError())
            {
                setStatus(fs->getStatus());
            }
            else
            {
                // with a new feature source, we need to re-establish
                // the data extents and open a new session.
                updateSession();
            }
        }
    }
}

void
FeatureImageLayer::setStyleSheet(StyleSheet* value)
{
    if (getStyleSheet() != value)
    {
        options().styleSheet().setLayer(value);
        if (_global._session.valid())
        {
            _global._session->setStyles(getStyleSheet());
        }
    }
}

void
FeatureImageLayer::setRenderingTechnique(const std::string& value)
{
    options().renderingTechnique() = value;
    setUseCreateTexture(usesSlugRendering());
    if (usesSlugRendering())
    {
        setMorphImagery(false);
    }
}

const std::string&
FeatureImageLayer::getRenderingTechnique() const
{
    return options().renderingTechnique().get();
}

bool
FeatureImageLayer::usesSlugRendering() const
{
    return ciEquals(options().renderingTechnique().get(), "slug");
}

void
FeatureImageLayer::prepareForRendering(TerrainEngine* engine)
{
    ImageLayer::prepareForRendering(engine);

#ifdef OSGEARTH_HAVE_SLUGHORN
    if (usesSlugRendering())
    {
        VirtualProgram* vp = VirtualProgram::getOrCreate(getOrCreateStateSet());
        vp->setName(className());
        Shaders shaders;
        shaders.load(vp, shaders.FeatureImageLayerSlug, getReadOptions());
    }
#endif
}

void
FeatureImageLayer::updateSession()
{
    if (_global._session.valid() && getFeatureSource())
    {
        const FeatureProfile* fp = getFeatureSource()->getFeatureProfile();

        DataExtentList dataExtents;

        if (fp)
        {
            // recalculate the data extents based on the feature source.
            if (fp->getTilingProfile() != NULL)
            {
                // Use specified profile's GeoExtent
                unsigned maxLevel = fp->getMaxLevel();
                if (options().maxDataLevel().isSet())
                    maxLevel = osg::maximum(maxLevel, options().maxDataLevel().get());

                dataExtents.push_back(DataExtent(fp->getTilingProfile()->getExtent(), fp->getFirstLevel(), maxLevel));
            }
            else if (fp->getExtent().isValid() == true)
            {
                // Use FeatureProfile's GeoExtent
                dataExtents.push_back(DataExtent(fp->getExtent()));
            }
        }

        setDataExtents(dataExtents);

        _global._session->setFeatureSource(getFeatureSource());
        _global._session->setStyles(getStyleSheet());
    }
}

GeoImage
FeatureImageLayer::createImageImplementation(const TileKey& key, ProgressCallback* progress) const
{
    if (getStatus().isError())
    {
        return GeoImage::INVALID;
    }

    // take local refs to prevent threading issues.
    Isolate local(_global);

    if (!local._session.valid())
    {
        setStatus(Status::AssertionFailure, "session is NULL - call support");
        return GeoImage::INVALID;
    }

    if (!local._session->getFeatureSource())
    {
        setStatus(Status::ServiceUnavailable, "No feature source");
        return GeoImage::INVALID;
    }

    const FeatureProfile* featureProfile = local._session->getFeatureSource()->getFeatureProfile();
    if (featureProfile == nullptr)
    {
        setStatus(Status::ConfigurationError, "Feature profile is missing");
        return GeoImage::INVALID;
    }

    const SpatialReference* featureSRS = featureProfile->getSRS();
    if (!featureSRS)
    {
        setStatus(Status::ConfigurationError, "Feature profile has no SRS");
        return GeoImage::INVALID;
    }


    FeatureRasterizer* rasterizer = nullptr;

    osg::ref_ptr<osg::Image> image;
    if (local._session->styles())
    {
        for (auto& style : local._session->styles()->getStyles())
        {
            if (style.second.getSymbol<CoverageSymbol>())
            {
                image = LandCover::createImage(
                    getTileSize(),
                    getTileSize());

                rasterizer = new FeatureRasterizer(image.get(), key.getExtent());
                ImageUtils::PixelWriter writer(image.get());
                writer.assign(osg::Vec4(NO_DATA_VALUE, NO_DATA_VALUE, NO_DATA_VALUE, NO_DATA_VALUE));
                break;
            }
        }
    }

    if (!rasterizer)
    {
        rasterizer = new FeatureRasterizer(getTileSize(), getTileSize(), key.getExtent(), options().backgroundColor().get());
    }

    FilterContext context(local._session.get(), key.getExtent());

    auto renderer = [&](const Style& style, FeatureList& features, ProgressCallback* progress) -> void
    {
        rasterizer->render(features, style, context);
    };

    FeatureStyleSorter sorter;

    // a buffer will pull in data from nearby tiles to mitigate edge artifacts

    sorter.sort(
        key,
        options().bufferWidth().value(),
        local._session.get(),
        local._filterChain,
        nullptr,
        renderer,
        progress);

    GeoImage result = rasterizer->finalize();

#if 0
    // If the rasterizer didn't render anything at all return an invalid image
    // This was originally added to avoid caching images where the server had an error and returned no features
    // and also to just save on disk space for transparent images.
    // However, this is causing areas that are actually empty to fall back on parent tiles
    // which is incorrect.  For example, using the osm highways multilevel 
    // vector tile dataset will try to render roads at level 14, find no roads (b/c there aren't any in a tile) and then
    // will try level 13, 12, etc until it finally gets to a tile that contains "some" road data.
    if (ImageUtils::isEmptyImage(result.getImage()))
    {
        result = GeoImage::INVALID;
    }
#endif
    delete rasterizer;

    return result;
}

TextureWindow
FeatureImageLayer::createTexture(const TileKey& key, ProgressCallback* progress) const
{
#ifndef OSGEARTH_HAVE_SLUGHORN
    return {};
#else
    if (!usesSlugRendering() || getStatus().isError())
        return {};

    Isolate local(_global);
    if (!local._session.valid() || !local._session->getFeatureSource())
        return {};

    const FeatureProfile* featureProfile =
        local._session->getFeatureSource()->getFeatureProfile();
    if (!featureProfile || !featureProfile->getSRS())
        return {};

    FilterContext context(local._session.get(), key.getExtent());

    using SlugBatch = std::pair<Style, FeatureList>;
    std::vector<SlugBatch> batches;

    auto renderer = [&](const Style& style, FeatureList& features,
                        ProgressCallback*) -> void
    {
        batches.emplace_back(style, features);
    };

    FeatureStyleSorter sorter;
    sorter.sort(
        key,
        options().bufferWidth().value(),
        local._session.get(),
        local._filterChain,
        nullptr,
        renderer,
        progress);

    // Match the atlas row width to the output tile. The osgEarth adapter omits
    // curves from overfull band lists instead of allowing them to wrap into
    // unrelated data or growing the texture to a driver-limit width.
    Util::FeatureImageLayerSlug slugBuilder(
        getTileSize(),
        key.getExtent(),
        options().backgroundColor().get(),
        getTileSize());
    for (const auto& batch : batches)
        slugBuilder.render(batch.second, batch.first, context);

    return slugBuilder.finalize();
#endif
}
