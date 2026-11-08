/* osgEarth - Geospatial SDK for OpenSceneGraph
* Copyright 2008-2012 Pelican Mapping
* MIT License
*/
#include "CesiumTilesetNode"
#include "Context"
#include "CesiumIon"
#include "Settings"

#include <osgEarth/Notify>
#include <osgUtil/CullVisitor>
#include <Cesium3DTilesSelection/BoundingVolume.h>

using namespace osgEarth::Cesium;

CesiumTilesetNode::CesiumTilesetNode(unsigned int assetID, const std::string& server, const std::string& token,
    float maximumScreenSpaceError, std::vector<int> overlays) :
    CesiumTilesetNode(assetID, server, token, maximumScreenSpaceError, std::move(overlays), nullptr)
{
}

CesiumTilesetNode::CesiumTilesetNode(unsigned int assetID, const std::string& server, const std::string& token,
    float maximumScreenSpaceError, std::vector<int> overlays, const osgDB::Options* readOptions)
{ 
    Context* context = CesiumIon::instance().getContext(server);

    Cesium3DTilesSelection::TilesetExternals externals{
        readOptions ? std::make_shared<AssetAccessor>(readOptions) : context->assetAccessor,
        context->prepareRenderResources, context->asyncSystem, context->creditSystem, context->logger, nullptr
    };

    Cesium3DTilesSelection::TilesetOptions options;    
    options.maximumScreenSpaceError = maximumScreenSpaceError;
    options.contentOptions.generateMissingNormalsSmooth = true;
    Cesium3DTilesSelection::Tileset* tileset = new Cesium3DTilesSelection::Tileset(externals, assetID, token, options, server);

    for (auto overlay: overlays)
    {
        CesiumRasterOverlays::RasterOverlayOptions rasterOptions;
        const auto ionRasterOverlay = new CesiumRasterOverlays::IonRasterOverlay("", overlay, token, rasterOptions);
        tileset->getOverlays().add(ionRasterOverlay);
    }    
    _tileset = tileset;

    setCullingActive(false);    
}

CesiumTilesetNode::CesiumTilesetNode(const std::string& url, const std::string& server, const std::string& token,
    float maximumScreenSpaceError, std::vector<int> overlays) :
    CesiumTilesetNode(url, server, token, maximumScreenSpaceError, std::move(overlays), nullptr)
{
}

CesiumTilesetNode::CesiumTilesetNode(const std::string& url, const std::string& server, const std::string& token,
    float maximumScreenSpaceError, std::vector<int> overlays, const osgDB::Options* readOptions)
{
    Context* context = CesiumIon::instance().getContext(server);

    Cesium3DTilesSelection::TilesetExternals externals{
        readOptions ? std::make_shared<AssetAccessor>(readOptions) : context->assetAccessor,
        context->prepareRenderResources, context->asyncSystem, context->creditSystem, context->logger, nullptr
    };

    Cesium3DTilesSelection::TilesetOptions options;
    options.maximumScreenSpaceError = maximumScreenSpaceError;
    options.contentOptions.generateMissingNormalsSmooth = true;
    Cesium3DTilesSelection::Tileset* tileset = new Cesium3DTilesSelection::Tileset(externals, url, options);
    for (auto overlay : overlays)
    {
        CesiumRasterOverlays::RasterOverlayOptions rasterOptions;
        const auto ionRasterOverlay = new CesiumRasterOverlays::IonRasterOverlay("", overlay, token, rasterOptions, server);
        tileset->getOverlays().add(ionRasterOverlay);
    }
    _tileset = tileset;
    setCullingActive(false);
}


CesiumTilesetNode::~CesiumTilesetNode()
{
    Cesium3DTilesSelection::Tileset* tileset = (Cesium3DTilesSelection::Tileset*)_tileset;
    delete tileset;
}

float CesiumTilesetNode::getMaximumScreenSpaceError() const
{
    Cesium3DTilesSelection::Tileset* tileset = (Cesium3DTilesSelection::Tileset*)_tileset;
    return tileset->getOptions().maximumScreenSpaceError;
}

void CesiumTilesetNode::setMaximumScreenSpaceError(float maximumScreenSpaceError)
{
    Cesium3DTilesSelection::Tileset* tileset = (Cesium3DTilesSelection::Tileset*)_tileset;
    tileset->getOptions().maximumScreenSpaceError = maximumScreenSpaceError;
}

bool CesiumTilesetNode::getForbidHoles() const
{
    Cesium3DTilesSelection::Tileset* tileset = (Cesium3DTilesSelection::Tileset*)_tileset;
    return tileset->getOptions().forbidHoles;
}

void CesiumTilesetNode::setForbidHoles(bool forbidHoles)
{
    Cesium3DTilesSelection::Tileset* tileset = (Cesium3DTilesSelection::Tileset*)_tileset;
    tileset->getOptions().forbidHoles = forbidHoles;
}

void
CesiumTilesetNode::traverse(osg::NodeVisitor& nv)
{
    if (nv.getVisitorType() == nv.CULL_VISITOR)
    {
        const osgUtil::CullVisitor* cv = nv.asCullVisitor();
        osg::Vec3d osgEye, osgCenter, osgUp;
        cv->getModelViewMatrix()->getLookAt(osgEye, osgCenter, osgUp);
        osg::Vec3d osgDir = osgCenter - osgEye;
        osgDir.normalize();

        glm::dvec3 pos(osgEye.x(), osgEye.y(), osgEye.z());
        glm::dvec3 dir(osgDir.x(), osgDir.y(), osgDir.z());
        glm::dvec3 up(osgUp.x(), osgUp.y(), osgUp.z());
        glm::dvec2 viewportSize(cv->getViewport()->width(), cv->getViewport()->height());

        double vfov, ar, znear, zfar;
        cv->getProjectionMatrix()->getPerspective(vfov, ar, znear, zfar);
        vfov = osg::DegreesToRadians(vfov);
        double hfov = 2 * atan(tan(vfov / 2) * (ar));

        // TODO:  Multiple views
        std::vector<Cesium3DTilesSelection::ViewState> viewStates;
        Cesium3DTilesSelection::ViewState viewState = Cesium3DTilesSelection::ViewState::create(pos, dir, up, viewportSize, hfov, vfov);
        viewStates.push_back(viewState);
        Cesium3DTilesSelection::Tileset* tileset = (Cesium3DTilesSelection::Tileset*)_tileset;
        auto updates = tileset->updateView(viewStates);

        removeChildren(0, getNumChildren());
        for (auto tile : updates.tilesToRenderThisFrame)
        {
            if (tile->getContent().isRenderContent())
            {
                MainThreadResult* result = reinterpret_cast<MainThreadResult*>(tile->getContent().getRenderContent()->getRenderResources());
                if (result && result->node.valid()) {
                    addChild(result->node.get());
                }
            }
        }
    }
    else if (nv.getVisitorType() == nv.UPDATE_VISITOR)
    {        
        osg::Group::traverse(nv);
    }

    osg::Group::traverse(nv);
}

osg::BoundingSphere CesiumTilesetNode::computeBound() const
{
    if (_tileset)
    {
        Cesium3DTilesSelection::Tileset* tileset = (Cesium3DTilesSelection::Tileset*)_tileset;
        auto rootTile = tileset->getRootTile();
        if (rootTile)
        {
            auto bbox = Cesium3DTilesSelection::getOrientedBoundingBoxFromBoundingVolume(rootTile->getBoundingVolume());
            auto& center = bbox.getCenter();
            auto& lengths = bbox.getLengths();
            float radius = std::max(lengths.x, std::max(lengths.y, lengths.z));
            return osg::BoundingSphere(osg::Vec3(center.x, center.y, center.z), radius);
        }
    }
    return osg::BoundingSphere();
}