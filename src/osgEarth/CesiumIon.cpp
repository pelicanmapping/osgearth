/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "CesiumIon"
#include <osgEarth/Registry>
#include <osgEarth/JsonUtils>
#include <osgEarth/TDTiles>
#include <osgEarth/TMS>
#include <osgEarth/Bing>
#include <osgEarth/Notify>
#include <osgEarth/Locators>

#include <osgUtil/SmoothingVisitor>

using namespace osgEarth;
using namespace osgEarth::Contrib::ThreeDTiles;

#undef LC
#define LC "[CesiumIon] "

//............................................................................

Status
CesiumIonResource::open(const URI& server,
                        const std::string& assetId,
                        const std::string& token,
                        const osgDB::Options* readOptions)
{
    if (assetId.empty())
    {
        return Status::Error(Status::ConfigurationError, "Fail: driver requires a valid \"asset_id\" property");
    }

    if (token.empty())
    {
        return Status::Error(Status::ConfigurationError, "Fail: driver requires a valid \"token\" property");
    }

    std::stringstream buf;
    buf << server.full();
    if (!endsWith(server.full(), "/")) buf << "/";
    buf << "v1/assets/" << assetId << "/endpoint?access_token=" << token;
    URI endpoint(buf.str());
    OE_DEBUG << "Getting endpoint " << endpoint.full() << std::endl;

    ReadResult r = URI(endpoint).readString(readOptions);
    if (r.failed())
        return Status::Error(Status::ConfigurationError, "Failed to get metadata from asset endpoint");

    Json::Value doc;
    Json::Reader reader;
    if (!reader.parse(r.getString(), doc))
    {
        return Status::Error(Status::ConfigurationError, "Failed to parse metadata from asset endpoint");
    }

    _resourceUrl = doc["url"].asString();
    _resourceToken = doc["accessToken"].asString();

    // A resource can be opened more than once, so nothing from a previous asset may survive.
    _acceptHeader.clear();
    _externalType.clear();
    _externalOptions = Json::Value();

    // Configure the accept header. External assets have no Ion token, and must not send an
    // authorization header at all because the third-party host would reject it.
    if (!_resourceToken.empty())
    {
        _acceptHeader = "Bearer " + _resourceToken;
    }

    if (doc.isMember("externalType"))
    {
        _externalType = doc["externalType"].asString();
        if (doc.isMember("options"))
        {
            _externalOptions = doc["options"];

            // External assets (e.g. Google Photorealistic Tiles) keep their location under
            // "options", and any credentials they need are already in that URL.
            if (_externalOptions.isMember("url"))
            {
                _resourceUrl = _externalOptions["url"].asString();
            }
        }
    }

    return STATUS_OK;
}

//........................................................................

Config
CesiumIonImageLayer::Options::getConfig() const
{
    Config conf = ImageLayer::Options::getConfig();
            conf.set("server", _server);
            conf.set("asset_id", _assetId);
            conf.set("token", _token);
    return conf;
}

void
CesiumIonImageLayer::Options::fromConfig(const Config& conf)
{
    _server.init("https://api.cesium.com/");
    conf.get("server", _server);
    conf.get("asset_id", _assetId);
    conf.get("token", _token);
}

Config
CesiumIonImageLayer::Options::getMetadata()
{
    return Config::readJSON(R"( {
        "name" : "CesiumIon Service",
        "properties" : [
        ]
    } )");
}

REGISTER_OSGEARTH_LAYER(cesiumionimage, CesiumIonImageLayer);

OE_LAYER_PROPERTY_IMPL(CesiumIonImageLayer, URI, Server, server);
OE_LAYER_PROPERTY_IMPL(CesiumIonImageLayer, std::string, AssetId, assetId);
OE_LAYER_PROPERTY_IMPL(CesiumIonImageLayer, std::string, Token, token);

void
CesiumIonImageLayer::init()
{
    ImageLayer::init();
}

Status
CesiumIonImageLayer::openImplementation()
{
    Status parent = ImageLayer::openImplementation();
    if (parent.isError())
        return parent;

    const char* key = ::getenv("OSGEARTH_CESIUMION_KEY");
    if (key)
        _key = key;
    else
        _key = options().token().get();

    if (_key.empty())
    {
        return Status(Status::ConfigurationError, "CesiumIon API key is required");
    }

    CesiumIonResource ionResource;
    Status status = ionResource.open(
        options().server().get(),
        options().assetId().get(),
        _key,
        getReadOptions());

    if (status.isOK())
    {
        URIContext uriContext(ionResource._resourceUrl);
        uriContext.addHeader("authorization", ionResource._acceptHeader);
        URI tmsURI = URI("tilemapresource.xml", uriContext);


        if (ionResource._externalType.empty())
        {
            TMSImageLayer* tmsImageLayer = new TMSImageLayer();
            tmsImageLayer->setURL(tmsURI);
            _imageLayer = tmsImageLayer;
        }
        else
        {
            if (ionResource._externalType == "BING")
            {
                BingImageLayer *bingImageLayer = new BingImageLayer();
                bingImageLayer->setAPIKey(ionResource._externalOptions["key"].asString());
                bingImageLayer->setImagerySet(ionResource._externalOptions["mapStyle"].asString());
                _imageLayer = bingImageLayer;
            }
        }

        if (_imageLayer)
        {
            _imageLayer->setName(getName());
            status = _imageLayer->open();
            if (status.isError())
                return status;
            setProfile(_imageLayer->getProfile());
            DataExtentList dataExtents;
            _imageLayer->getDataExtents(dataExtents);
            setDataExtents(dataExtents);
        }
        else
        {
            return Status::Error("Unsupported Cesium Ion image layer");
        }
    }

    return status;
}

Status
CesiumIonImageLayer::closeImplementation()
{
    if (_imageLayer.valid())
    {
        _imageLayer->close();
        _imageLayer = NULL;
    }
    return ImageLayer::closeImplementation();
}

GeoImage
CesiumIonImageLayer::createImageImplementation(const TileKey& key, ProgressCallback* progress) const
{
    if (_imageLayer)
    {
        return _imageLayer->createImage(key, progress);
    }
    return GeoImage(Status("Invalid image layer"));    
}

//........................................................................

Config
CesiumIon3DTilesLayer::Options::getConfig() const
{
    Config conf = ThreeDTilesLayer::Options::getConfig();
    conf.set("server", _server);
    conf.set("asset_id", _assetId);
    conf.set("token", _token);
    return conf;
}

void
CesiumIon3DTilesLayer::Options::fromConfig(const Config& conf)
{
    _server.init("https://api.cesium.com/");
    conf.get("server", _server);
    conf.get("asset_id", _assetId);
    conf.get("token", _token);
}

void
CesiumIon3DTilesLayer::init()
{
    ThreeDTilesLayer::init();
}

Status
CesiumIon3DTilesLayer::openImplementation()
{
    Status parentStatus = VisibleLayer::openImplementation();
    if (parentStatus.isError())
        return parentStatus;

    const char* key = ::getenv("OSGEARTH_CESIUMION_KEY");
    if (key)
        _key = key;
    else
        _key = options().token().get();

    if (_key.empty())
    {
        return Status(Status::ConfigurationError, "CesiumIon API key is required");
    }

    CesiumIonResource ionResource;
    Status status = ionResource.open(
        options().server().get(),
        options().assetId().get(),
        _key,
        getReadOptions());

    if (status.isError())
        return status;

    if (ionResource._resourceUrl.empty())
    {
        return Status(Status::ResourceUnavailable, "Asset endpoint did not return a tileset URL");
    }

    URIContext uriContext;
    if (!ionResource._acceptHeader.empty())
    {
        uriContext.addHeader("authorization", ionResource._acceptHeader);
    }

    return openTileset(URI(ionResource._resourceUrl, uriContext), ionResource._acceptHeader);
}

REGISTER_OSGEARTH_LAYER(cesiumion3dtiles, CesiumIon3DTilesLayer);
OE_LAYER_PROPERTY_IMPL(CesiumIon3DTilesLayer, URI, Server, server);
OE_LAYER_PROPERTY_IMPL(CesiumIon3DTilesLayer, std::string, AssetId, assetId);
OE_LAYER_PROPERTY_IMPL(CesiumIon3DTilesLayer, std::string, Token, token);





//........................................................................

Config
CesiumIonTerrainMeshLayer::Options::getConfig() const
{
    Config conf = TerrainMeshLayer::Options::getConfig();
    conf.set("server", _server);
    conf.set("asset_id", _assetId);
    conf.set("token", _token);
    return conf;
}

void
CesiumIonTerrainMeshLayer::Options::fromConfig(const Config& conf)
{
    _server.init("https://api.cesium.com/");
    conf.get("server", _server);
    conf.get("asset_id", _assetId);
    conf.get("token", _token);
}

Config
CesiumIonTerrainMeshLayer::Options::getMetadata()
{
    return Config::readJSON(R"(
        { "name" : "CesiumIon Service",
            "properties" : [
            ]
        })");    
}

REGISTER_OSGEARTH_LAYER(cesiumionterrainmesh, CesiumIonTerrainMeshLayer);

OE_LAYER_PROPERTY_IMPL(CesiumIonTerrainMeshLayer, URI, Server, server);
OE_LAYER_PROPERTY_IMPL(CesiumIonTerrainMeshLayer, std::string, AssetId, assetId);
OE_LAYER_PROPERTY_IMPL(CesiumIonTerrainMeshLayer, std::string, Token, token);

void
CesiumIonTerrainMeshLayer::init()
{
    TerrainMeshLayer::init();
}

Status
CesiumIonTerrainMeshLayer::openImplementation()
{
    Status parent = TerrainMeshLayer::openImplementation();
    if (parent.isError())
        return parent;

    const char* key = ::getenv("OSGEARTH_CESIUMION_KEY");
    if (key)
        _key = key;
    else
        _key = options().token().get();

    if (_key.empty())
    {
        return Status(Status::ConfigurationError, "CesiumIon API key is required");
    }

    CesiumIonResource ionResource;
    Status status = ionResource.open(
        options().server().get(),
        options().assetId().get(),
        _key,
        getReadOptions());

    if (status.isOK())
    {
        URIContext uriContext;
        uriContext.addHeader("authorization", ionResource._acceptHeader);
        _assetURI = URI(ionResource._resourceUrl, uriContext);
    }

    return status;
}

Status
CesiumIonTerrainMeshLayer::closeImplementation()
{
    return TerrainMeshLayer::closeImplementation();
}


///////

namespace
{
    struct QuantizedMeshHeader
    {
        // The center of the tile in Earth-centered Fixed coordinates.
        double CenterX;
        double CenterY;
        double CenterZ;

        // The minimum and maximum heights in the area covered by this tile.
        // The minimum may be lower and the maximum may be higher than
        // the height of any vertex in this tile in the case that the min/max vertex
        // was removed during mesh simplification, but these are the appropriate
        // values to use for analysis or visualization.
        float MinimumHeight;
        float MaximumHeight;

        // The tile's bounding sphere.  The X,Y,Z coordinates are again expressed
        // in Earth-centered Fixed coordinates, and the radius is in meters.
        double BoundingSphereCenterX;
        double BoundingSphereCenterY;
        double BoundingSphereCenterZ;
        double BoundingSphereRadius;

        // The horizon occlusion point, expressed in the ellipsoid-scaled Earth-centered Fixed frame.
        // If this point is below the horizon, the entire tile is below the horizon.
        // See http://cesiumjs.org/2013/04/25/Horizon-culling/ for more information.
        double HorizonOcclusionPointX;
        double HorizonOcclusionPointY;
        double HorizonOcclusionPointZ;
    };

    struct VertexData
    {
        unsigned int vertexCount;
        std::vector<unsigned short> u;
        std::vector<unsigned short> v;
        std::vector<unsigned short> height;
    };

    int zig_zag_decode(int n)
    {
        return (n >> 1) ^ (-(n & 1));
    }


    TileMesh quantizedMeshToTileMesh(const osgEarth::TileKey& key, std::stringstream& buf)
    {
        // Parse the header
        QuantizedMeshHeader header;
        buf.read(reinterpret_cast<char*>(&header), sizeof(QuantizedMeshHeader));

        VertexData vertexData;
        buf.read(reinterpret_cast<char*>(&vertexData.vertexCount), sizeof(unsigned int));
        vertexData.u.resize(vertexData.vertexCount);
        vertexData.v.resize(vertexData.vertexCount);
        vertexData.height.resize(vertexData.vertexCount);
        buf.read(reinterpret_cast<char*>(vertexData.u.data()), sizeof(unsigned short) * vertexData.vertexCount);
        buf.read(reinterpret_cast<char*>(vertexData.v.data()), sizeof(unsigned short) * vertexData.vertexCount);
        buf.read(reinterpret_cast<char*>(vertexData.height.data()), sizeof(unsigned short) * vertexData.vertexCount);

        unsigned short u = 0;
        unsigned short v = 0;
        unsigned short height = 0;
        for (unsigned int i = 0; i < vertexData.vertexCount; ++i)
        {
            u += zig_zag_decode(vertexData.u[i]);
            v += zig_zag_decode(vertexData.v[i]);
            height += zig_zag_decode(vertexData.height[i]);
            vertexData.u[i] = u;
            vertexData.v[i] = v;
            vertexData.height[i] = height;
        }

        unsigned int triangleCount;
        buf.read(reinterpret_cast<char*>(&triangleCount), sizeof(unsigned int));

        osg::ref_ptr< osg::DrawElements > drawElements;

        if (vertexData.vertexCount > 65536)
        {
            std::vector<unsigned int> indices;
            indices.resize(triangleCount * 3u);
            buf.read(reinterpret_cast<char*>(indices.data()), sizeof(unsigned int) * indices.size());

            unsigned int highest = 0;
            for (unsigned int i = 0; i < indices.size(); ++i)
            {
                unsigned int code = indices[i];
                indices[i] = highest - code;
                if (code == 0) {
                    ++highest;
                }
            }

            drawElements = new osg::DrawElementsUInt(GL_TRIANGLES);
            for (unsigned int i = 0; i < indices.size(); ++i)
            {
                drawElements->addElement(indices[i]);
            }
        }
        else
        {
            std::vector<unsigned short> indices;
            indices.resize(triangleCount * 3u);
            buf.read(reinterpret_cast<char*>(indices.data()), sizeof(unsigned short) * indices.size());

            unsigned short highest = 0;
            for (unsigned int i = 0; i < indices.size(); ++i)
            {
                unsigned short code = indices[i];
                indices[i] = highest - code;
                if (code == 0) {
                    ++highest;
                }
            }

            drawElements = new osg::DrawElementsUShort(GL_TRIANGLES);
            for (unsigned int i = 0; i < indices.size(); ++i)
            {
                drawElements->addElement(indices[i]);
            }
        }

        TileMesh mesh;
        osgEarth::GeoLocator locator(key.getExtent());
        osgEarth::GeoPoint centroid_world = key.getExtent().getCentroid();
        osg::Matrix world2local, local2world;
        centroid_world.createWorldToLocal(world2local);
        local2world.invert(world2local);

        osg::ref_ptr<osg::VertexBufferObject> vbo = new osg::VertexBufferObject();

        osg::ref_ptr< osg::Vec3Array > verts = new osg::Vec3Array(vertexData.vertexCount);
        verts->setVertexBufferObject(vbo.get());
        verts->setBinding(osg::Array::BIND_PER_VERTEX);

        osg::ref_ptr< osg::Vec3Array > texCoords = new osg::Vec3Array(vertexData.vertexCount);
        texCoords->setVertexBufferObject(vbo.get());
        texCoords->setBinding(osg::Array::BIND_PER_VERTEX);

        osg::Vec3d unit;
        osg::Vec3d model;
        osg::Vec3d modelLTP;

        for (unsigned int i = 0; i < vertexData.vertexCount; ++i)
        {
            float tileWith = header.MaximumHeight - header.MinimumHeight;
            float s = (float)vertexData.u[i] / 32767.0f;
            float t = (float)vertexData.v[i] / 32767.0f;
            float r = (float)vertexData.height[i] / 32767.0f;

            float height = header.MinimumHeight + r * (header.MaximumHeight - header.MinimumHeight);
            unit.set(s, t, height);
            locator.unitToWorld(unit, model);
            modelLTP = model * world2local;

            int default_marker = VERTEX_VISIBLE | VERTEX_CONSTRAINT | VERTEX_HAS_ELEVATION;
            (*texCoords)[i].set(osg::Vec3(s, t, (float)default_marker));
            (*verts)[i].set(modelLTP);

            // Request oct encoded normals?
        }

        osg::ref_ptr< osg::Vec3Array > normals;
        if (!normals)
        {
            osg::ref_ptr< osg::Geode > geode = new osg::Geode;
            osg::ref_ptr< osg::Geometry > geometry = new osg::Geometry;
            geometry->setVertexArray(verts);
            geometry->addPrimitiveSet(drawElements);
            geode->addDrawable(geometry);
            osgUtil::SmoothingVisitor sv;
            geode->accept(sv);
            normals = static_cast<osg::Vec3Array*>(geometry->getNormalArray());
            if (normals)
            {
                normals->setVertexBufferObject(vbo.get());
            }
        }


        mesh.verts = verts;
        mesh.uvs = texCoords;
        mesh.normals = normals;
        mesh.indices = drawElements;
        mesh.localToWorld = local2world;

        return mesh;
    }
}
///////


TileMesh CesiumIonTerrainMeshLayer::createTileImplementation(
    const TileKey& key,
    ProgressCallback* progress) const
{
    unsigned x, y;
    key.getTileXY(x, y);
    unsigned int numRows, numCols;
    key.getProfile()->getNumTiles(key.getLevelOfDetail(), numCols, numRows);
    y = numRows - y - 1;

    URI tileURI(Stringify() << _assetURI.full() << key.getLevelOfDetail() << "/" << x << "/" << y << ".terrain", _assetURI.context());

    osgEarth::ReadResult result;
    for (unsigned int i = 0; i < 4; ++i)
    {
        result = tileURI.readString(getReadOptions(), progress);
        if (result.succeeded())
        {
            std::string data = result.getString();
            std::stringstream buf(data);
            auto mesh = quantizedMeshToTileMesh(key, buf);
            applyConstraints(key, mesh);
            return mesh;
        }
        else
        {
            //OE_NOTICE << "Request for " << tileURI.full() << " failed attempt=" << i << std::endl;
        }
    }

    TileMesh invalid;
    return invalid;
}
