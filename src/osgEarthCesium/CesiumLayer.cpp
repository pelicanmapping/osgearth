#include "CesiumLayer"

/* osgEarth - Geospatial SDK for OpenSceneGraph
 * Copyright 2020 Pelican Mapping
 * MIT License
 */
#include "CesiumLayer"
#include "Settings"

#include <osgEarth/Registry>

#define LC "[CesiumNative3DTilesLayer] " << getName() << " : "

using namespace osgEarth;
using namespace osgEarth::Cesium;

//------------------------------------------------------------------------

Config
CesiumNative3DTilesLayer::Options::getConfig() const
{
    Config conf = VisibleLayer::Options::getConfig();
    conf.set("server", _server);
    conf.set("url", _url);
    conf.set("asset_id", _assetId);
    conf.set("token", _token);
    conf.set("raster_overlay", _rasterOverlay);
    conf.set("max_sse", _maximumScreenSpaceError);
    conf.set("forbid_holes", _forbidHoles);

    return conf;
}

void
CesiumNative3DTilesLayer::Options::fromConfig(const Config& conf)
{
    _server.init("https://api.cesium.com/");
    _maximumScreenSpaceError.setDefault(16.0f);
    _forbidHoles.setDefault(false);
    conf.get("server", _server);
    conf.get("url", _url);
    conf.get("asset_id", _assetId);
    conf.get("token", _token);
    conf.get("raster_overlay", _rasterOverlay);
    conf.get("max_sse", _maximumScreenSpaceError);
    conf.get("forbid_holes", _forbidHoles);
}

//........................................................................

REGISTER_OSGEARTH_LAYER(cesiumnative3dtiles, CesiumNative3DTilesLayer);

OE_LAYER_PROPERTY_IMPL(CesiumNative3DTilesLayer, URI, URL, url);
OE_LAYER_PROPERTY_IMPL(CesiumNative3DTilesLayer, std::string, Token, token);

CesiumNative3DTilesLayer::~CesiumNative3DTilesLayer()
{
    //nop
}

void
CesiumNative3DTilesLayer::init()
{
    VisibleLayer::init();
}

Status
CesiumNative3DTilesLayer::openImplementation()
{
    Status parentStatus = VisibleLayer::openImplementation();
    if (parentStatus.isError())
        return parentStatus;

    std::string token = options().token().get();
    if (token.empty())
    {
        token = getCesiumIonKey();

    }

    if (_options->url().isSet())
    {
        std::vector<int> overlays;
        if (_options->rasterOverlay().isSet())
        {
            overlays.push_back(*_options->rasterOverlay());
        }
        _tilesetNode = new CesiumTilesetNode(_options->url()->full(), _options->server()->full(), token,
            *_options->maximumScreenSpaceError(), overlays, getReadOptions());
    }
    else if (_options->assetId().isSet())
    {
        std::vector<int> overlays;
        if (_options->rasterOverlay().isSet())
        {
            overlays.push_back(*_options->rasterOverlay());
        }
        _tilesetNode = new CesiumTilesetNode(*_options->assetId(), _options->server()->full(), token,
            *_options->maximumScreenSpaceError(), overlays, getReadOptions());
    }

    if (!_tilesetNode.valid())
    {
        return Status(Status::GeneralError, "Failed to load asset from url or asset id");
    }

    _tilesetNode->setForbidHoles(getForbidHoles());

    return STATUS_OK;
}

unsigned int CesiumNative3DTilesLayer::getAssetId() const
{
    return *options().assetId();
}

void CesiumNative3DTilesLayer::setAssetId(unsigned int assetID)
{
    options().assetId() = assetID;
}

int CesiumNative3DTilesLayer::getRasterOverlay() const
{
    return *options().rasterOverlay();
}

void CesiumNative3DTilesLayer::setRasterOverlay(int rasterOverlay)
{
    options().rasterOverlay() = rasterOverlay;
}

osg::Node*
CesiumNative3DTilesLayer::getNode() const
{
    return _tilesetNode.get();
}

float
CesiumNative3DTilesLayer::getMaximumScreenSpaceError() const
{
    return *options().maximumScreenSpaceError();
}

void
CesiumNative3DTilesLayer::setMaximumScreenSpaceError(float maximumScreenSpaceError)
{
    options().maximumScreenSpaceError() = maximumScreenSpaceError;
    if (_tilesetNode)
    {
        _tilesetNode->setMaximumScreenSpaceError(maximumScreenSpaceError);
    }
}

bool
CesiumNative3DTilesLayer::getForbidHoles() const
{
    return *options().forbidHoles();
}

void
CesiumNative3DTilesLayer::setForbidHoles(bool forbidHoles)
{

    options().forbidHoles() = forbidHoles;
    if (_tilesetNode)
    {
        _tilesetNode->setForbidHoles(forbidHoles);
    }
}
