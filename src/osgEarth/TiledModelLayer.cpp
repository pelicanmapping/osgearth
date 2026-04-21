/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "TiledModelLayer"
#include "SimplePager"
#include "NodeUtils"
#include "Chonk"
#include "Registry"
#include "ShaderGenerator"
#include "Style"
#include "NetworkMonitor"
#include <osg/BlendFunc>

#define LC "[" << className() << "] \"" << getName() << "\": "

using namespace osgEarth;

void TiledModelLayer::Options::fromConfig(const Config& conf)
{
    conf.get("additive", additive());
    conf.get("min_level", minLevel());
    conf.get("max_level", maxLevel());
    conf.get("nvgl", nvgl());
    conf.get("profile", profile());
    conf.get("lod_method", "screen_space", lodMethod(), LODMethod::SCREEN_SPACE);
    conf.get("lod_method", "camera_distance", lodMethod(), LODMethod::CAMERA_DISTANCE);
    conf.get("range_factor", rangeFactor());
    conf.get("min_pixels", minPixels());
}

Config
TiledModelLayer::Options::getConfig() const
{
    Config conf = VisibleLayer::Options::getConfig();
    conf.set("additive", additive());
    conf.set("min_level", minLevel());
    conf.set("max_level", maxLevel());
    conf.set("nvgl", nvgl());
    conf.set("profile", profile());
    conf.set("lod_method", "screen_space", lodMethod(), LODMethod::SCREEN_SPACE);
    conf.set("lod_method", "camera_distance", lodMethod(), LODMethod::CAMERA_DISTANCE);
    conf.set("range_factor", rangeFactor());
    conf.set("min_pixels", minPixels());
    return conf;
}

void TiledModelLayer::setAdditive(bool value)
{
    options().additive() = value;
}

bool TiledModelLayer::getAdditive() const
{
    return options().additive().get();
}

void TiledModelLayer::setRangeFactor(float value)
{
    options().rangeFactor() = value;
}

float TiledModelLayer::getRangeFactor() const
{
    return options().rangeFactor().get();
}

void TiledModelLayer::setLODMethod(LODMethod method)
{
    options().lodMethod() = method;
}

LODMethod TiledModelLayer::getLODMethod() const
{
    return options().lodMethod().get();
}

void TiledModelLayer::setMinPixels(float value)
{
    options().minPixels() = value;
}

float TiledModelLayer::getMinPixels() const
{
    return options().minPixels().get();
}

void TiledModelLayer::setMinLevel(unsigned value)
{
    options().minLevel() = value;
}

unsigned TiledModelLayer::getMinLevel() const
{
    return options().minLevel().get();
}

void TiledModelLayer::setMaxLevel(unsigned value)
{
    options().maxLevel() = value;
}

unsigned TiledModelLayer::getMaxLevel() const
{
    return options().maxLevel().get();
}

osg::ref_ptr<osg::Node>
TiledModelLayer::createTile(const TileKey& key, ProgressCallback* progress) const
{
    ScopedReadLock lock(inUseMutex());

    if (getStatus().isError())
        return {};

    if (!getProfile())
    {
        setStatus(Status::ResourceUnavailable, "No profile");
        return {};
    }

    NetworkMonitor::ScopedRequestLayer layerRequest(getName());

    osg::ref_ptr<osg::Node> result;

    // check the L2 cache
    auto record = _localcache.get(key);
    if (record.has_value())
    {
        OE_DEBUG << "L2 hit(" << key.str() << ")" << std::endl;
        return record.value();
    }

    // only create one at a time per key
    ScopedGate<TileKey> sentry(_gate, key);

    //OE_INFO << "createTile(" << key.str() << ")" << std::endl;

    if (key.getProfile()->isHorizEquivalentTo(getProfile()))
    {
        result = createTileImplementation(key, progress);
    }
    else
    {
        osg::ref_ptr<osg::Group> group = new osg::Group();

        std::vector<TileKey> i_keys;
        getProfile()->getIntersectingTiles(key, i_keys);

        if (i_keys.empty())
            return {};

        std::set<TileKey> unique_keys;

        for (auto unique_key : i_keys)
        {
            if (unique_key.getLOD() > getMaxLevel())
                unique_key = unique_key.createAncestorKey(getMaxLevel());
            if (unique_key.valid())
                unique_keys.insert(unique_key);
        }

        for (auto& unique_key : unique_keys)
        {
            osg::ref_ptr<osg::Node> part = createTileImplementation(unique_key, progress);
            if (part.valid())
            {
                group->addChild(part);
            }
        }

        result = group;
    }

    if (result.valid())
    {
        if (_textures.valid()) // nvgl
        {
            // for each StyleGroup that isn't under another StyleGroup:
            forEachUnnestedNodeOfType<StyleGroup>(result, [&](StyleGroup* styleGroup)
                {
                    osg::ref_ptr<osg::Node> output;
                    osg::Group* xformGroup = nullptr;

                    // for each MT that's not under another MT:
                    forEachUnnestedNodeOfType<osg::MatrixTransform>(styleGroup, [&](auto* xform)
                        {
                            osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable();

                            for (unsigned i = 0; i < xform->getNumChildren(); ++i)
                            {
                                drawable->add(xform->getChild(i), _chonkFactory);
                            }
                            xform->removeChildren(0, xform->getNumChildren());
                            xform->addChild(drawable);

                            if (!xformGroup)
                                xformGroup = new osg::Group();

                            xformGroup->addChild(xform);
                        });

                    if (xformGroup != nullptr)
                    {
                        output = xformGroup;
                    }
                    else
                    {
                        osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable();
                        drawable->add(styleGroup, _chonkFactory);
                        output = drawable;
                    }

                    if (output.valid())
                    {
                        styleGroup->removeChildren(0, styleGroup->getNumChildren());
                        styleGroup->addChild(output);
                    }

                    // Note: don't use "auto" here, gcc does not like it -gw
                    RenderSymbol* render = styleGroup->style.get<RenderSymbol>();
                    if (render)
                        render->applyTo(styleGroup);
                });
        }
        else
        {
            osgEarth::Registry::shaderGenerator().run(result.get(), _statesetCache);
        }
    }

    if (result.valid())
    {
        _localcache.insert(key, result);
    }

    return result;
}

// The Node representing this layer.
osg::Node* TiledModelLayer::getNode() const
{
    return _root.get();
}

// called by the map when this layer is added
void
TiledModelLayer::addedToMap(const Map* map)
{
    super::addedToMap(map);

    if (*options().nvgl() == true && GLUtils::useNVGL() && !_textures.valid())
    {
        _textures = new TextureArena();
        getOrCreateStateSet()->setAttribute(_textures, 1);

        // auto release requires that we install this update callback!
        _textures->setAutoRelease(true);

        _chonkFactory.textures = _textures;

        getNode()->addUpdateCallback(new LambdaCallback<>([this](osg::NodeVisitor& nv)
            {
                _textures->update(nv);
                return true;
            }));

        getOrCreateStateSet()->setAttributeAndModes(
            new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA),
            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
    }

    _map = map;

    _graphDirty = true;

    // re-create the graph if necessary.
    create();

    // tell the user about the selected profiles
    double width, height;
    getProfile()->getTileDimensions(getMinLevel(), width, height);
    auto* srs = getProfile()->getSRS();
    auto tileSize = 1.4142 * srs->transformDistance(Distance(height, srs->getUnits()), Units::METERS);
    OE_INFO << LC << "min_level = " << getMinLevel() << ", effective range = " << (int)(tileSize * getRangeFactor()) << "m; profile = " << getProfile()->getSRS()->getName() << std::endl;
}

// called by the map when this layer is removed
void
TiledModelLayer::removedFromMap(const Map* map)
{
    super::removedFromMap(map);

    if (_root.valid())
    {
        osg::ref_ptr<SimplePager> node = findTopMostNodeOfType<SimplePager>(_root.get());
        if (node.valid())
            node->setDone();

        _root->removeChildren(0, _root->getNumChildren());
    }
}

void TiledModelLayer::dirty()
{
    super::dirty();

    _graphDirty = true;

    // create the scene graph
    create();
}

// post-ctor initialization
void TiledModelLayer::init()
{
    super::init();

    // Create the container group

    _root = new osg::Group();

    // Assign the layer's state set to the root node:
    _root->setStateSet(this->getOrCreateStateSet());

    // Graph needs rebuilding
    _graphDirty = true;

    // Depth sorting by default
    getOrCreateStateSet()->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
}

Status TiledModelLayer::closeImplementation()
{
    _localcache.clear();
    _root->removeChildren(0, _root->getNumChildren());
    return Status::NoError;
}

void TiledModelLayer::create()
{
    if (_map.valid() && _graphDirty)
    {
        _root->removeChildren(0, _root->getNumChildren());

        OE_SOFT_ASSERT_AND_RETURN(getProfile(), void());

        auto pager = new SimplePager(_map.get(), getProfile());

        pager->setCreateNodeFunction([layer_weak{ osg::observer_ptr<TiledModelLayer>(this) }](const TileKey& key, ProgressCallback* progress)
            {
                osg::ref_ptr<TiledModelLayer> layer;
                if (!layer_weak.lock(layer))
                    return osg::ref_ptr<osg::Node>();

                auto output = layer->createTile(key, progress);

                return output;
            });

        pager->setAdditive(this->getAdditive());
        pager->setMinLevel(this->getMinLevel());
        pager->setMaxLevel(this->getMaxLevel());

        if (options().lodMethod() == LODMethod::SCREEN_SPACE)
            pager->setMinPixels(this->getMinPixels());
        else
            pager->setRangeFactor(this->getRangeFactor());

        pager->build();

        _root->addChild(pager);
        _graphDirty = false;
    }
}   

osg::ref_ptr<const Map>
TiledModelLayer::getMap() const
{
    return _map.get();
}
