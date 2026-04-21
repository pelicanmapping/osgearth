/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/TiledFeatureModelLayer>
#include <osgEarth/Registry>
#include <osgEarth/FeatureStyleSorter>
#include <osgEarth/CropFilter>

using namespace osgEarth;

#define LC "[TiledFeatureModelLayer] \"" << getName() << "\": "

#define OE_TEST OE_NULL

REGISTER_OSGEARTH_LAYER(TiledFeatureModel, TiledFeatureModelLayer);

//...........................................................................

TiledFeatureModelLayer::Options::Options() :
    TiledModelLayer::Options(),
    FeatureModelOptions(),
    GeometryCompilerOptions()
{
    fromConfig(_conf);
}

TiledFeatureModelLayer::Options::Options(const ConfigOptions& options) :
    TiledModelLayer::Options(options),
    FeatureModelOptions(options),
    GeometryCompilerOptions(options)
{
    fromConfig(_conf);
}

void TiledFeatureModelLayer::Options::fromConfig(const Config& conf)
{
    features().get(conf, "features");
    conf.get("crop_features", cropFeaturesToTile());
}

Config
TiledFeatureModelLayer::Options::getConfig() const
{
    Config conf = TiledModelLayer::Options::getConfig();

    Config fmConf = FeatureModelOptions::getConfig();
    conf.merge(fmConf);

    Config gcConf = GeometryCompilerOptions::getConfig();
    conf.merge(gcConf);

    features().set(conf, "features");

    conf.set("crop_features", cropFeaturesToTile());

    return conf;
}

void TiledFeatureModelLayer::Options::mergeConfig(const Config& conf)
{
    TiledModelLayer::Options::mergeConfig(conf);
    fromConfig(conf);
}

//...........................................................................

OE_LAYER_PROPERTY_IMPL(TiledFeatureModelLayer, bool, AlphaBlending, alphaBlending);
OE_LAYER_PROPERTY_IMPL(TiledFeatureModelLayer, bool, EnableLighting, enableLighting);

TiledFeatureModelLayer::~TiledFeatureModelLayer()
{
    //NOP
}

void
TiledFeatureModelLayer::init()
{
    TiledModelLayer::init();
}

Config
TiledFeatureModelLayer::getConfig() const
{
    Config conf = TiledModelLayer::getConfig();
    return conf;
}

void
TiledFeatureModelLayer::setFeatureSource(FeatureSource* source)
{
    if (getFeatureSource() != source)
    {
        options().features().setLayer(source);

        if (source && source->getStatus().isError())
        {
            setStatus(source->getStatus());
            return;
        }

        dirty();
    }
}

FeatureSource*
TiledFeatureModelLayer::getFeatureSource() const
{
    return options().features().getLayer();
}

const FeatureProfile*
TiledFeatureModelLayer::getFeatureProfile() const
{
    FeatureSource* fs = getFeatureSource();
    return fs ? fs->getFeatureProfile() : nullptr;
}

void
TiledFeatureModelLayer::setStyleSheet(StyleSheet* value)
{
    if (getStyleSheet() != value)
    {
        options().styleSheet().setLayer(value);
        dirty();
    }
}

StyleSheet*
TiledFeatureModelLayer::getStyleSheet() const
{
    return options().styleSheet().getLayer();
}

Status
TiledFeatureModelLayer::openImplementation()
{
    Status parent = TiledModelLayer::openImplementation();
    if (parent.isError())
        return parent;

    Status fsStatus = options().features().open(getReadOptions());
    if (fsStatus.isError())
        return fsStatus;

    Status ssStatus = options().styleSheet().open(getReadOptions());
    if (ssStatus.isError())
        return ssStatus;

    return Status::NoError;
}

Status
TiledFeatureModelLayer::closeImplementation()
{
    super::closeImplementation();
    options().features().close();
    options().styleSheet().close();
    return getStatus();
}

const GeoExtent&
TiledFeatureModelLayer::getExtent() const
{
    static GeoExtent s_invalid;

    auto* fp = getFeatureProfile();
    return fp ? fp->getExtent() : s_invalid;
}


void
TiledFeatureModelLayer::addedToMap(const Map* map)
{
    OE_TEST << LC << "addedToMap" << std::endl;
    
    options().features().addedToMap(map);
    options().styleSheet().addedToMap(map);

    auto fs = getFeatureSource();
    if (!fs)
    {
        setStatus(Status::ResourceUnavailable, "Missing REQUIRED feature source");
        return;
    }
    else if (fs->getStatus().isError())
    {
        setStatus(fs->getStatus());
        return;
    }

    auto ss = getStyleSheet();
    if (!ss)
    {
        setStatus(Status::ResourceUnavailable, "Missing REQUIRED stylesheet");
        return;
    }
    else if (ss->getStatus().isError())
    {
        setStatus(ss->getStatus());
        return;
    }

    // Our session object for feature rendering.
    _session = new Session(map, ss, fs, getReadOptions());
    _session->setResourceCache(new ResourceCache());

    if (options().featureIndexing()->enabled() == true)
    {
        FeatureSourceIndexOptions indexOptions;
        indexOptions.enabled() = true;
        indexOptions.embedFeatures() = true;

        _featureIndex = new FeatureSourceIndex(fs, Registry::objectIndex(), indexOptions);
    }

    _filters = FeatureFilterChain::create(options().filters(), getReadOptions());

    // invoke the superclass, where the SimplePager will get created.
    TiledModelLayer::addedToMap(map);
}

void
TiledFeatureModelLayer::removedFromMap(const Map* map)
{
    super::removedFromMap(map);

    _filters.clear();
    _session = nullptr;
    _featureIndex = nullptr;

    options().features().removedFromMap(map);
    options().styleSheet().removedFromMap(map);
}

osg::ref_ptr<osg::Node>
TiledFeatureModelLayer::createTileImplementation(const TileKey& key, ProgressCallback* progress) const
{
    if (!isOpen() || (progress && progress->isCanceled()))
        return nullptr;

    // Get features for this key
    // set up for feature indexing if appropriate:
    osg::ref_ptr<FeatureSourceIndexNode> index = 0L;
    if (_featureIndex.valid())
    {
        index = new FeatureSourceIndexNode(_featureIndex.get());
    }

    GeomFeatureNodeFactory factory(options());

    if (progress && progress->isCanceled())
        return nullptr;

    OE_SOFT_ASSERT_AND_RETURN(getFeatureProfile(), {});
    
    FilterContext context(_session.get(), getFeatureProfile(), key.getExtent(), index);
    Query query(key);


    osg::ref_ptr<osg::Group> group = new osg::Group();

    auto compile = [&](const Style& style, FeatureList& features, ProgressCallback* progress)
        {
#if 1
            // TODO: migrate this up to the feature source at some point? -gw
            auto cropMethod = options().cropFeaturesToTile() == true ?
                CropFilter::METHOD_CROP_TO_EXTENT : CropFilter::METHOD_CENTROID;

            CropFilter crop(cropMethod);
            context = crop.push(features, context);
#endif

            osg::ref_ptr<osg::Node> node;
            FeatureListCursor cursor(features);
            if (factory.createOrUpdateNode(&cursor, style, context, node, query))
            {
                auto styleGroup = new StyleGroup(style);
                styleGroup->addChild(node);
                group->addChild(styleGroup);

                const RenderSymbol* render = style.get<RenderSymbol>();
                if (render)
                    render->applyTo(styleGroup);
            }
        };

    FeatureStyleSorter().sort(key, {}, _session.get(), _filters, nullptr, compile, progress);

    if (group->getNumChildren() == 0 || group->getBound().valid() == false)
    {
        return {};
    }

    if (index.valid())
    {
        index->addChild(group);
        group = index;
    }

    return group;
}

const Profile*
TiledFeatureModelLayer::getProfile() const
{
    static const Profile* s_fallback = Profile::create(Profile::GLOBAL_GEODETIC);

    if (options().profile().isSet())
    {
        auto* profile = Profile::create(options().profile().value());
        if (profile && profile->isOK())
            return profile;
    }

    auto* fs = getFeatureSource();
    OE_SOFT_ASSERT_AND_RETURN(fs, nullptr);

    OE_SOFT_ASSERT_AND_RETURN(getFeatureProfile(), nullptr);

    // first try the tiling profile if there is one.
    auto profile = getFeatureProfile()->getTilingProfile();

    // otherwise, this is an untiled source (like a local shapefile) so will
    // try to construct a profile from its extent
    if (!profile && getFeatureProfile()->getExtent().isValid())
    {
        profile = Profile::create(getFeatureProfile()->getExtent());
    }

    // failing all that, fall back on a default.
    return profile ? profile : s_fallback;
}

unsigned
TiledFeatureModelLayer::getMinLevel() const
{
    if (options().minLevel().isSet())
    {
        return options().minLevel().value();
    }
    else
    {
        OE_SOFT_ASSERT_AND_RETURN(getFeatureProfile(), 0);
        return getFeatureProfile()->getFirstLevel();
    }
}

unsigned
TiledFeatureModelLayer::getMaxLevel() const
{
    if (options().maxLevel().isSet())
    {
        return options().maxLevel().value();
    }
    else
    {
        OE_SOFT_ASSERT_AND_RETURN(getFeatureProfile(), 0);

        if (getFeatureProfile()->isTiled())
        {
            return getFeatureProfile()->getMaxLevel();
        }
        else
        {
            // in the case of an un-tiled feature source (like a local shapefile
            // or geojson file), min level should == the max level so there is 
            // only one level of detail.
            return getMinLevel();
        }
    }
}
