/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/Chonk>
#include <osgEarth/GeoTransform>
#include <osgEarth/NodeUtils>
#include <osgEarth/Registry>
#include <osgEarth/ThreeDTilesLayer>
#include <osg/BlendFunc>

#define LC "[ThreeDTilesLayer] " << getName() << " : "

using namespace osgEarth;
using namespace osgEarth::Util;
using namespace osgEarth::Contrib;
using namespace osgEarth::Contrib::ThreeDTiles;
using namespace osgEarth::Threading;

namespace
{
    //! Converts compatible model branches while retaining instance semantics, unsupported state, and asset reloads.
    //! A local origin keeps RTC/ECEF translations out of float placements; the tile transform stays above this node.
    //! Paging jobs own their conversion graphs; the returned graph retains the shared factory for later updates.
    osg::ref_ptr<osg::Node> convertContent(osg::Node* node, const std::shared_ptr<ChonkFactory>& factory,
        int renderBin, int twoSidedRenderBin)
    {
        if (!node)
            return nullptr;

        const osg::BoundingSphere bound = node->getBound();
        if (!bound.valid())
            return node;

        const osg::Vec3d origin(bound.center());
        osg::ref_ptr<osg::MatrixTransform> local = new osg::MatrixTransform(osg::Matrixd::translate(-origin));
        local->addChild(node);

        auto converted = ChonkFactory::convertScene(local.get(), factory, renderBin, twoSidedRenderBin);

        osg::ref_ptr<osg::MatrixTransform> placed = new osg::MatrixTransform(osg::Matrixd::translate(origin));
        placed->setName(node->getName());
        placed->setNodeMask(node->getNodeMask());
        placed->addChild(converted);
        return placed;
    }
}

//------------------------------------------------------------------------

Config
ThreeDTilesLayer::Options::getConfig() const
{
    Config conf = VisibleLayer::Options::getConfig();
    conf.set("url", _url);
    conf.set("max_sse", _maximumScreenSpaceError);
    conf.set("nvgl", nvgl());
    return conf;
}

void
ThreeDTilesLayer::Options::fromConfig( const Config& conf )
{
    _maximumScreenSpaceError.init(15.0f);
    conf.get("url", _url);
    conf.get("max_sse", _maximumScreenSpaceError);
    conf.get("nvgl", nvgl());
}

//........................................................................

REGISTER_OSGEARTH_LAYER(threedtiles, ThreeDTilesLayer);

OE_LAYER_PROPERTY_IMPL(ThreeDTilesLayer, URI, URL, url);

ThreeDTilesLayer::~ThreeDTilesLayer()
{
    //nop
}

void
ThreeDTilesLayer::init()
{
    VisibleLayer::init();

    // Make sure the glTF plugin is loaded
    std::string libname = osgDB::Registry::instance()->createLibraryNameForExtension("gltf");
    osgDB::Registry::instance()->loadLibrary(libname);
}

Status
ThreeDTilesLayer::openImplementation()
{
    Status parentStatus = VisibleLayer::openImplementation();
    if (parentStatus.isError())
        return parentStatus;

    osg::ref_ptr< osgDB::Options > readOptions = osgEarth::Registry::instance()->cloneOrCreateOptions(this->getReadOptions());

    ReadResult rr = _options->url()->readString(readOptions.get());
    if (rr.failed())
    {
        return Status(Status::ResourceUnavailable, Stringify() << "Error loading tileset: " << rr.errorDetail());
    }

    Tileset* tileset = Tileset::create(rr.getString(), _options->url()->full());
    if (!tileset)
    {
        return Status(Status::GeneralError, "Bad tileset");
    }

    osg::ref_ptr<TextureArena> textures;
    ThreeDTilesetNode::ContentProcessor processContent;
    if (*options().nvgl() && GLUtils::useNVGL())
    {
        textures = new TextureArena();
        textures->setAutoRelease(true);
        auto factory = std::make_shared<ChonkFactory>(textures.get());

        // Reserve both raster-policy bins from the same UID space as TiledModelLayer's bins.
        // Fresh IDs also keep retained graphs from earlier opens isolated from this arena.
        const int renderBin = 100 + static_cast<int>(osgEarth::createUID());
        const int twoSidedRenderBin = 100 + static_cast<int>(osgEarth::createUID());
        processContent = [factory, renderBin, twoSidedRenderBin](osg::Node* node)
        {
            return convertContent(node, factory, renderBin, twoSidedRenderBin);
        };
    }

    _tilesetNode = new ThreeDTilesetNode(tileset, "", getSceneGraphCallbacks(), readOptions.get(), processContent);
    if (textures.valid())
    {
        _tilesetNode->getOrCreateStateSet()->setAttribute(textures, osg::StateAttribute::ON);
        _tilesetNode->getOrCreateStateSet()->setAttributeAndModes(
            new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA),
            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

        // Retain this tileset's arena independently of the layer, including across close/reopen.
        _tilesetNode->addUpdateCallback(new LambdaCallback<>([textures](osg::NodeVisitor& nv)
        {
            textures->update(nv);
            return true;
        }));
    }
    _tilesetNode->setMaximumScreenSpaceError(*options().maximumScreenSpaceError());
    _tilesetNode->setOwnerName(getName());

    return STATUS_OK;
}

float
ThreeDTilesLayer::getMaximumScreenSpaceError() const
{
    return *options().maximumScreenSpaceError();
}

void
ThreeDTilesLayer::setMaximumScreenSpaceError(float maximumScreenSpaceError)
{
    options().maximumScreenSpaceError() = maximumScreenSpaceError;
    if (_tilesetNode)
    {
        _tilesetNode->setMaximumScreenSpaceError(maximumScreenSpaceError);
    }
}

osg::Node*
ThreeDTilesLayer::getNode() const
{
    return _tilesetNode.get();
}
