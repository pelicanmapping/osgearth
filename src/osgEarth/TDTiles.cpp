/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/Metrics>
#include <osgEarth/TDTiles>
#include <osgEarth/Utils>
#include <osgEarth/Registry>
#include <osgEarth/URI>
#include <osgEarth/NodeUtils>
#include <osgEarth/FileUtils>
#include <osgEarth/NetworkMonitor>
#include <osgEarth/Threading>
#include <osgEarth/GLUtils>
#include <osgEarth/LineDrawable>
#include <osgEarth/Math>
#include <osgEarth/Horizon>
#include <osgEarth/MapNode>
#include <osgEarth/Progress>

#include <algorithm>

#include <osgDB/FileNameUtils>
#include <osgDB/Registry>
#include <osgUtil/IncrementalCompileOperation>

using namespace osgEarth;
using namespace osgEarth::Threading;
using namespace osgEarth::Util;
using namespace osgEarth::Contrib::ThreeDTiles;

#define LC "[3DTiles] "

#define SENTRY_VALUE NULL

// Job pool shared by every tileset's content loads.
#define JOB_POOL_NAME "oe.3dtiles"

// Content loads allowed in flight per tileset unless set otherwise (Cesium's maximumSimultaneousTileLoads).
#define DEFAULT_MAX_SIMULTANEOUS_LOADS 20u

// A queued load whose tile no cull has visited for this long (seconds) is canceled.
#define STALE_REQUEST_AGE 0.25

// The horizon test uses an ellipsoid this much (meters) smaller than the map's, so terrain below the
// ellipsoid (the Dead Sea shore is ~430 m down) can't hide tiles that are actually visible.
#define HORIZON_ELLIPSOID_MARGIN 1000.0

//........................................................................

using ICO = osgUtil::IncrementalCompileOperation;

namespace
{
    // State shared by all tiles of one tileset during one cull traversal.
    struct CullContext
    {
        osgEarth::Contrib::ThreeDTiles::ThreeDTilesetNode* tileset = nullptr;

        // Eye-to-world matrix of the camera being culled.
        osg::Matrixd inverseView;

        // Occlusion by the planet, when the tileset is on a geocentric map.
        bool horizonValid = false;
        osgEarth::Horizon horizon;

        // Content requests made during the traversal, with their priorities (higher first).
        std::vector<std::pair<double, osgEarth::Contrib::ThreeDTiles::ThreeDTileNode*>> requests;
    };

    // The context of the tileset being culled on this thread. A cull traversal runs on a single thread and
    // visits a tileset's tiles from inside its ThreeDTilesetNode::traverse, which installs and removes it.
    thread_local CullContext* s_cullContext = nullptr;

    // Holds one of a tileset's load slots for as long as it exists.
    struct LoadSlot
    {
        explicit LoadSlot(std::shared_ptr<std::atomic_uint> counter) : _counter(std::move(counter)) { ++(*_counter); }
        ~LoadSlot() { --(*_counter); }
        std::shared_ptr<std::atomic_uint> _counter;
    };
}


namespace osgEarth { namespace Contrib { namespace ThreeDTiles
{
    class ThreeDTilesJSONReaderWriter : public osgDB::ReaderWriter
    {
    public:
        ThreeDTilesJSONReaderWriter()
        {
            supportsExtension("3dtiles", "3D-Tiles JSON TileSet");
        }

        virtual const char* className() const { return "3D-Tiles JSON TileSet"; }

        virtual ReadResult readObject(const std::string& location, const osgDB::Options* options) const
        {
            return readNode(location, options);
        }

        virtual ReadResult readNode(const std::string& location, const osgDB::Options* options) const
        {
            std::string ext = osgDB::getFileExtension(location);
            if (!acceptsExtension(ext))
                return ReadResult::FILE_NOT_HANDLED;

            std::string uristring = osgDB::getNameLessExtension(location);

            osgEarth::ReadResult rr = URI(uristring).readString(options);
            if (rr.failed())
                return ReadResult(rr.errorDetail());

            Tileset* tileset = Tileset::create(rr.getString(), uristring);
            if (!tileset)
                return ReadResult("Unable to parse tileset");

            osg::ref_ptr< osgDB::Options > readOptions = osgEarth::Registry::instance()->cloneOrCreateOptions(options);
            osg::ref_ptr<ThreeDTilesetNode> node = new ThreeDTilesetNode(tileset, "", NULL, readOptions.get());
            node->setMaximumScreenSpaceError(15.0f);
            return node.release();
        }
    };
    REGISTER_OSGPLUGIN(3dtiles, ThreeDTilesJSONReaderWriter);
} } }

//........................................................................

void
Asset::fromJSON(const Json::Value& value)
{
    if (value.isMember("version"))
        version() = value.get("version", "").asString();
    if (value.isMember("tilesetVersion"))
        tilesetVersion() = value.get("tilesetVersion", "").asString();
    if (value.isMember("gltfUpAxis"))
        gltfUpAxis() = value.get("gltfUpAxis", "").asString();

}

Json::Value
Asset::getJSON() const
{
    Json::Value value(Json::objectValue);
    if (version().isSet())
        value["version"] = version().get();
    if (tilesetVersion().isSet())
        value["tilesetVersion"] = tilesetVersion().get();
    if (gltfUpAxis().isSet())
        value["gltfUpAxis"] = gltfUpAxis().get();
    return value;
}

//........................................................................

void
BoundingVolume::fromJSON(const Json::Value& value)
{
    if (value.isMember("region"))
    {
        const Json::Value& a = value["region"];
        if (a.isArray() && a.size() == 6)
        {
            Json::Value::const_iterator i = a.begin();
            auto& r = region().mutable_value();
            r.xMin() = (*i++).asDouble();
            r.yMin() = (*i++).asDouble();
            r.xMax() = (*i++).asDouble();
            r.yMax() = (*i++).asDouble();
            r.zMin() = (*i++).asDouble();
            r.zMax() = (*i++).asDouble();
        }
        else OE_WARN << "Invalid region array" << std::endl;
    }

    if (value.isMember("sphere"))
    {
        const Json::Value& a = value["sphere"];
        if (a.isArray() && a.size() == 4)
        {
            Json::Value::const_iterator i = a.begin();
            auto& s = sphere().mutable_value();
            s.center().x() = (*i++).asDouble();
            s.center().y() = (*i++).asDouble();
            s.center().z() = (*i++).asDouble();
            s.radius()     = (*i++).asDouble();

        }
    }
    if (value.isMember("box"))
    {
        const Json::Value& a = value["box"];
        if (a.isArray() && a.size() == 12)
        {
            double values[12];
            unsigned int index = 0;
            for (Json::ValueConstIterator j = a.begin(); j != a.end(); ++j)
            {
                values[index] = (*j).asDouble();
                index++;
            }
            osg::Vec3d center(values[0], values[1], values[2]);
            osg::Vec3d xvec(values[3], values[4], values[5]);
            osg::Vec3d yvec(values[6], values[7], values[8]);
            osg::Vec3d zvec(values[9], values[10], values[11]);

            orientedBox() = osg::Matrixd(
                xvec.x(), xvec.y(), xvec.z(), 0.0,
                yvec.x(), yvec.y(), yvec.z(), 0.0,
                zvec.x(), zvec.y(), zvec.z(), 0.0,
                center.x(), center.y(), center.z(), 1.0);

            // The axis-aligned bounds must take all eight corners: the face centers alone miss most of a
            // tilted box, which made tiles get culled while they were still in view.
            auto& b = box().mutable_value();
            for (int corner = 0; corner < 8; ++corner)
            {
                b.expandBy(center +
                    xvec * ((corner & 1) ? 1.0 : -1.0) +
                    yvec * ((corner & 2) ? 1.0 : -1.0) +
                    zvec * ((corner & 4) ? 1.0 : -1.0));
            }
        }
        else OE_WARN << "Invalid box array" << std::endl;
    }
}

Json::Value
BoundingVolume::getJSON() const
{
    Json::Value value(Json::objectValue);

    if (region().isSet())
    {
        Json::Value a(Json::arrayValue);
        a.append(region()->xMin());
        a.append(region()->yMin());
        a.append(region()->xMax());
        a.append(region()->yMax());
        a.append(region()->zMin());
        a.append(region()->zMax());
        value["region"] = a;
    }
    else if (sphere().isSet())
    {
        Json::Value a(Json::arrayValue);
        a.append(sphere()->center().x());
        a.append(sphere()->center().y());
        a.append(sphere()->center().z());
        a.append(sphere()->radius());
        value["sphere"] = a;
    }
    else if (orientedBox().isSet())
    {
        const osg::Matrixd& m = orientedBox().get();
        Json::Value a(Json::arrayValue);
        for (int row : { 3, 0, 1, 2 })
        {
            for (int col = 0; col < 3; ++col)
                a.append(m(row, col));
        }
        value["box"] = a;
    }
    else if (box().isSet())
    {
        OE_WARN << LC << "box not implemented" << std::endl;
    }
    return value;
}

osg::BoundingSphere
BoundingVolume::asBoundingSphere() const
{
    // Note: this should be epsg:4979 according to the 3D-Tiles spec,
    // but that only exists in very new versions of PROJ. For the
    // purposes of osgEarth there's no difference anyway
    const SpatialReference* srs = SpatialReference::get("epsg:4326");
    if (!srs)
        return osg::BoundingSphere();

    if (region().isSet())
    {
        GeoExtent extent(srs,
            osg::RadiansToDegrees(region()->xMin()),
            osg::RadiansToDegrees(region()->yMin()),
            osg::RadiansToDegrees(region()->xMax()),
            osg::RadiansToDegrees(region()->yMax()));

        osg::BoundingSphered bs = extent.createWorldBoundingSphere(region()->zMin(), region()->zMax());
        return osg::BoundingSphere(bs.center(), bs.radius());
    }

    else if (sphere().isSet())
    {
        return sphere().get();
    }

    else if (orientedBox().isSet())
    {
        // The farthest corner from the center; the corners come in opposite pairs, so four suffice.
        const osg::Matrixd& m = orientedBox().get();
        osg::Vec3d x(m(0, 0), m(0, 1), m(0, 2));
        osg::Vec3d y(m(1, 0), m(1, 1), m(1, 2));
        osg::Vec3d z(m(2, 0), m(2, 1), m(2, 2));
        double radius2 = std::max(
            std::max((x + y + z).length2(), (x + y - z).length2()),
            std::max((x - y + z).length2(), (x - y - z).length2()));
        return osg::BoundingSphere(m.getTrans(), sqrt(radius2));
    }

    else if (box().isSet())
    {
        return osg::BoundingSphere(box()->center(), box()->radius());
    }

    return osg::BoundingSphere();
}

//........................................................................

void
TileContent::fromJSON(const Json::Value& value, LoadContext& lc)
{
    if (value.isMember("boundingVolume"))
        boundingVolume() = BoundingVolume(value.get("boundingVolume", Json::nullValue));
    if (value.isMember("uri"))
        uri() = URI(value.get("uri", "").asString(), lc._uc);
    if (value.isMember("url"))
        uri() = URI(value.get("url", "").asString(), lc._uc);
}

Json::Value
TileContent::getJSON() const
{
    Json::Value value(Json::objectValue);
    if (boundingVolume().isSet())
        value["boundingVolume"] = boundingVolume()->getJSON();
    if (uri().isSet())
        value["uri"] = uri()->base();
    return value;
}

//........................................................................

void
Tile::fromJSON(const Json::Value& value, LoadContext& uc)
{
    if (value.isMember("boundingVolume"))
        boundingVolume() = value["boundingVolume"];
    if (value.isMember("viewerRequestVolume"))
        viewerRequestVolume() = value["viewerRequestVolume"];
    if (value.isMember("geometricError"))
        geometricError() = value.get("geometricError", 0.0).asDouble();
    if (value.isMember("content"))
        content() = TileContent(value["content"], uc);

    if (value.isMember("refine"))
    {
        refine() = osgEarth::ciEquals(value["refine"].asString(), "ADD") ? REFINE_ADD : REFINE_REPLACE;
    }

    if (value.isMember("transform"))
    {
        const Json::Value& digits = value["transform"];
        double c[16];
        if (digits.isArray() && digits.size() == 16)
        {
            unsigned k=0;
            for(Json::Value::const_iterator i = digits.begin(); i != digits.end(); ++i)
                c[k++] = (*i).asDouble();
            transform() = osg::Matrix(c);
        }
    }

    if (value.isMember("children"))
    {
        const Json::Value& a = value["children"];
        if (a.isArray())
        {
            for (Json::Value::const_iterator i = a.begin(); i != a.end(); ++i)
            {
                osg::ref_ptr<Tile> tile = new Tile(*i, uc);
                children().push_back(tile.get());
            }
        }
    }
}

Json::Value
Tile::getJSON() const
{
    Json::Value value(Json::objectValue);

    if (boundingVolume().isSet())
        value["boundingVolume"] = boundingVolume()->getJSON();
    if (viewerRequestVolume().isSet())
        value["viewerRequestVolume"] = viewerRequestVolume()->getJSON();
    if (geometricError().isSet())
        value["geometricError"] = geometricError().get();
    if (refine().isSet())
        value["refine"] = (refine().get() == REFINE_ADD) ? "ADD" : "REPLACE";
    if (content().isSet())
        value["content"] = content()->getJSON();


    if (!children().empty())
    {
        Json::Value collection(Json::arrayValue);
        for(unsigned i=0; i<children().size(); ++i)
        {
            Tile* child = children()[i].get();
            if (child)
            {
                collection.append(child->getJSON());
            }
        }
        value["children"] = collection;
    }

    return value;
}

osg::BoundingSphere
Tile::getBoundingSphere()
{
    osg::BoundingSphere bsphere = boundingVolume()->asBoundingSphere();

    // If the bounding volume is a box or a sphere we need to adjust it by the Tile's transform.
    if (boundingVolume()->box().isSet() || boundingVolume()->sphere().isSet())
    {
        // Taken directly from osg::Transform.
        osg::Matrixd l2w;
        l2w.preMult(transform().get());

        if (!bsphere.valid()) return bsphere;

        osg::BoundingSphere::vec_type xdash = bsphere._center;
        xdash.x() += bsphere._radius;
        xdash = xdash * l2w;

        osg::BoundingSphere::vec_type ydash = bsphere._center;
        ydash.y() += bsphere._radius;
        ydash = ydash * l2w;

        osg::BoundingSphere::vec_type zdash = bsphere._center;
        zdash.z() += bsphere._radius;
        zdash = zdash * l2w;

        bsphere._center = bsphere._center * l2w;

        xdash -= bsphere._center;
        osg::BoundingSphere::value_type sqrlen_xdash = xdash.length2();

        ydash -= bsphere._center;
        osg::BoundingSphere::value_type sqrlen_ydash = ydash.length2();

        zdash -= bsphere._center;
        osg::BoundingSphere::value_type sqrlen_zdash = zdash.length2();

        bsphere._radius = sqrlen_xdash;
        if (bsphere._radius < sqrlen_ydash) bsphere._radius = sqrlen_ydash;
        if (bsphere._radius < sqrlen_zdash) bsphere._radius = sqrlen_zdash;
        bsphere._radius = (osg::BoundingSphere::value_type)sqrt(bsphere._radius);
    }
    return bsphere;
}

//........................................................................

void
Tileset::fromJSON(const Json::Value& value, LoadContext& uc)
{
    if (value.isMember("asset"))
        asset() = Asset(value.get("asset", Json::nullValue));
    if (value.isMember("boundingVolume"))
        boundingVolume() = BoundingVolume(value.get("boundingVolume", Json::nullValue));
    if (value.isMember("geometricError"))
        geometricError() = value.get("geometricError", 0.0).asDouble();
    if (value.isMember("root"))
        root() = new Tile(value["root"], uc);
}

Json::Value
Tileset::getJSON() const
{
    Json::Value value(Json::objectValue);
    if (asset().isSet())
        value["asset"] = asset()->getJSON();
    if (boundingVolume().isSet())
        value["boundingVolume"] = boundingVolume()->getJSON();
    if (geometricError().isSet())
        value["geometricError"] = geometricError().get();
    if (root().valid())
        value["root"] = root()->getJSON();
    return value;
}

Tileset*
Tileset::create(const std::string& json, const URIContext& uc)
{
    Json::Reader reader;
    Json::Value root(Json::objectValue);
    if (!reader.parse(json, root, false))
        return NULL;

    LoadContext lc;
    lc._uc = uc;

    return new Tileset(root, lc);
}

//........................................................................

//! A random opaque color, used to tell tiles' bounding volume visualizations apart.
static osg::Vec4
randomColor()
{
    float r = (float)rand() / (float)RAND_MAX;
    float g = (float)rand() / (float)RAND_MAX;
    float b = (float)rand() / (float)RAND_MAX;
    return osg::Vec4(r, g, b, 1.0f);
}

namespace
{
    struct LoadTilesetOperation
    {
        LoadTilesetOperation(ThreeDTilesetNode* parentTileset, const URI& uri, osgDB::Options* options) :
            _uri(uri),
            _options(options),
            _parentTileset(parentTileset)
        {
            // Get the currently active request layer and reuse it when the operator actually occurs, which will probably be on a different thread.
            _requestLayer = NetworkMonitor::getRequestLayer();
        }

        osg::ref_ptr<osg::Node> loadTileSet(ProgressCallback* progress)
        {
            NetworkMonitor::ScopedRequestLayer layerRequest(_requestLayer);

            if (progress && progress->canceled())
                return nullptr;

            osg::ref_ptr<ThreeDTilesetContentNode> tilesetNode;

            osg::ref_ptr<ThreeDTilesetNode> parentTileset;
            if (_parentTileset.lock(parentTileset))
            {
                // load the tile set:
                ReadResult rr = _uri.readString(_options.get(), progress);

                if (rr.failed())
                {
                    OE_WARN << "Fail to read tileset \"" << _uri.full() << ": " << rr.errorDetail() << std::endl;
                }

                osg::ref_ptr<Tileset> tileset = Tileset::create(rr.getString(), _uri.full());
                if (tileset.valid())
                {
                    if (progress && progress->canceled())
                        return nullptr;

                    tilesetNode = new ThreeDTilesetContentNode(parentTileset.get(), tileset.get(), _options.get());
                }
            }

            return tilesetNode;
        }

        osg::ref_ptr< osgDB::Options > _options;
        osg::observer_ptr<ThreeDTilesetNode> _parentTileset;
        URI _uri;
        std::string _requestLayer;
    };


    using ReadTileData = osg::ref_ptr<osg::Node>;
    using ReadTileResult = Future<ReadTileData>;

    // Runs a tile load in the 3D Tiles job pool and returns its result. The job holds the given load slot only
    // while it reads; GL compilation is handed to the ICO (when there is one), which resolves the future once it
    // is done, so the pool thread doesn't sit waiting for the draw thread. Abandoning the future cancels the job,
    // or the download if it is already running. A null result means the load failed.
    template<typename LOAD>
    ReadTileResult dispatchTileLoad(
        const std::string& name,
        LOAD load,
        osg::observer_ptr<ICO> ico,
        std::shared_ptr<void> slot)
    {
        jobs::context context;
        context.name = name;
        context.pool = jobs::get_pool(JOB_POOL_NAME);

        auto job = [name, load, ico, slot](auto& promise)
        {
            osg::ref_ptr<osg::Node> node;
            try
            {
                osg::ref_ptr<ProgressCallback> progress = new ProgressCallback(&promise);
                node = load(progress.get());
            }
            catch (const std::exception& e)
            {
                // An exception escaping a pool thread would terminate the application.
                OE_WARN << LC << "Failed to load " << name << ": " << e.what() << std::endl;
                node = nullptr;
            }

            if (node.valid() && !promise.canceled())
            {
                GLObjectsCompiler compiler;
                osg::ref_ptr<osgUtil::StateToCompile> state = compiler.collectState(node.get());
                compiler.requestIncrementalCompileWithICO(node, state.get(), ico, promise);
                return;
            }

            promise.resolve(node);
        };

        ReadTileResult result;
        return jobs::dispatch(job, result, context);
    }

    osg::ref_ptr<osg::Node> readTilesetSync(
        ThreeDTilesetNode* parentTileset,
        const URI& uri,
        osgDB::Options* options)
    {
        LoadTilesetOperation operation(parentTileset, uri, options);
        return operation.loadTileSet(nullptr);
    }

    ReadTileResult readTilesetAsync(
        ThreeDTilesetNode* parentTileset,
        const URI& uri,
        osgDB::Options* options,
        osg::observer_ptr<ICO> ico,
        std::shared_ptr<void> slot)
    {
        std::shared_ptr<LoadTilesetOperation> operation = std::make_shared<LoadTilesetOperation>(
            parentTileset, uri, options);

        auto load = [operation](ProgressCallback* progress)
        {
            return operation->loadTileSet(progress);
        };

        return dispatchTileLoad(uri.full(), load, ico, slot);
    }

    // Loads content without compiling it: the root tile of an external tileset loads this way while its
    // tileset is read in a job, and that job compiles the whole tileset node afterwards.
    osg::ref_ptr<osg::Node> readTileContentSync(
        const URI& uri,
        osg::ref_ptr<const osgDB::Options> options,
        const ThreeDTilesetNode::ContentProcessor& processContent)
    {
        osg::ref_ptr<osg::Node> node = uri.getNode(options.get(), nullptr);
        return node.valid() && processContent ? processContent(node.get()) : node;
    }

    // Reads and converts a model on a paging thread before collecting its GL compilation state.
    // Copy the processor into the job so its resources survive the originating tileset.
    ReadTileResult readTileContentAsync(
        const URI& uri,
        osg::ref_ptr<const osgDB::Options> options,
        const std::string& requestLayer,
        const ThreeDTilesetNode::ContentProcessor& processContent,
        osg::observer_ptr<ICO> ico,
        std::shared_ptr<void> slot)
    {
        auto load = [uri, options, requestLayer, processContent](ProgressCallback* progress)
        {
            NetworkMonitor::ScopedRequestLayer layerRequest(requestLayer);
            osg::ref_ptr<osg::Node> node = uri.getNode(options.get(), progress);
            if (progress && progress->canceled())
                return osg::ref_ptr<osg::Node>();
            return node.valid() && processContent ? processContent(node.get()) : node;
        };

        return dispatchTileLoad(uri.full(), load, ico, slot);
    }
}

ThreeDTileNode::ThreeDTileNode(ThreeDTilesetNode* tileset, Tile* tile, bool immediateLoad, osgDB::Options* options) :
    _tileset(tileset),
    _tile(tile),
    _requestedContent(false),
    _immediateLoad(immediateLoad),
    _firstVisit(true),
    _options(options),
    _trackerItrValid(false),
    _lastCulledFrameNumber(0),
    _lastCulledFrameTime(0.0f),
    _refine(REFINE_ADD)
{
    OE_PROFILING_ZONE;
    _hasContent = _tile->content().isSet() && _tile->content()->uri().isSet();
    if (_tile->content().isSet())
    {
        OE_PROFILING_ZONE_TEXT(_tile->content()->uri()->full().c_str());
    }

    // If this tile has content, store a URI Context to that relative-path external file
    // references (textures) will resolve correctly.
    if (_tile->content().isSet())
    {
        _options = Registry::instance()->cloneOrCreateOptions(options);
        URIContext(_tile->content()->uri()->full()).store(_options.get());
    }

    // the transform to localize this tile:
    if (tile->transform().isSet())
    {
        setMatrix(tile->transform().get());
    }

    if (tile->refine().isSet())
    {
        _refine = *tile->refine();
    }

    if (_immediateLoad && _tile->content().isSet())
    {
        URIContext context = _tile->content()->uri()->context();
        if (!_tileset->getAuthorizationHeader().empty())
        {
            context.addHeader("authorization", _tileset->getAuthorizationHeader());
        }
        URI uri(_tile->content()->uri()->base(), context);


        if (osgEarth::Strings::endsWith(_tile->content()->uri()->base(), ".json"))
        {
            _content = readTilesetSync(_tileset, uri, options).get();
        }
        else
        {
            _content = readTileContentSync(uri, _options, _tileset->_processContent);
        }

        if (_content.valid())
        {
            _tileset->runPreMergeOperations(_content.get());
            _tileset->runPostMergeOperations(_content.get());
        }
        OE_PROFILING_ZONE_TEXT("Immediate load");
    }

    if (_tile->children().size() > 0)
    {
        _children = new osg::Group;
        for (unsigned int i = 0; i < _tile->children().size(); ++i)
        {
            ThreeDTileNode* child = new ThreeDTileNode(_tileset, _tile->children()[i].get(), false, _options.get());
            child->setParentTile(this);
            _children->addChild(child);
        }

        if (_children->getNumChildren() == 0)
        {
            _children = 0;
        }
        addChild(_children.get());
    }

    computeBoundingVolume();

    createDebugBounds();
}

void ThreeDTileNode::setParentTile(ThreeDTileNode* parentTile)
{
    _parentTile = parentTile;
    // Inherit the parent's refine policy if this Tile's refine policy isn't set.
    if (!parentTile || _tile->refine().isSet() || _refine == parentTile->getRefinePolicy())
    {
        return;
    }
    _refine = parentTile->getRefinePolicy();

    // The children were built (and inherited) in the constructor, before this tile knew its parent,
    // so pass the changed policy down to them.
    if (_children.valid())
    {
        for (unsigned int i = 0; i < _children->getNumChildren(); i++)
        {
            ThreeDTileNode* childTile = dynamic_cast<ThreeDTileNode*>(_children->getChild(i));
            if (childTile)
            {
                childTile->setParentTile(this);
            }
        }
    }
}

void ThreeDTileNode::computeBoundingVolume()
{
    const BoundingVolume& volume = _tile->boundingVolume().get();

    if (volume.region().isSet())
    {
        const SpatialReference* srs = SpatialReference::get("epsg:4326");
        const osg::BoundingBoxd& region = volume.region().get();

        GeoExtent extent(srs,
            osg::RadiansToDegrees(region.xMin()),
            osg::RadiansToDegrees(region.yMin()),
            osg::RadiansToDegrees(region.xMax()),
            osg::RadiansToDegrees(region.yMax()));

        // For really large bounding regions just use the bounding sphere to avoid transformation issues.
        if (extent.width() >= 5.0 || extent.height() >= 5.0)
        {
            _localBoundingSphere = extent.createWorldBoundingSphere(region.zMin(), region.zMax());
            return;
        }

        GeoPoint centroid = extent.getCentroid();

        osg::Matrixd worldToLocal, localToWorld;
        centroid.createWorldToLocal(worldToLocal);
        centroid.createLocalToWorld(localToWorld);

        // Sample a 3x3 grid at both heights: the ellipsoid bulges up between the corners, so the corners alone
        // would leave out the top of the region around its middle.
        osg::Vec3d world;
        osg::BoundingBoxd bb;
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                double lon = extent.west() + 0.5 * extent.width() * i;
                double lat = extent.south() + 0.5 * extent.height() * j;
                GeoPoint(srs, lon, lat, region.zMin()).toWorld(world);
                bb.expandBy(world * worldToLocal);
                GeoPoint(srs, lon, lat, region.zMax()).toWorld(world);
                bb.expandBy(world * worldToLocal);
            }
        }

        // Bounding region culling and display are supposed to ignore the matrix transform of the tile, but b/c of the nested
        // nature of the ThreeDTileNode class the MatrixTransform is going to be applied already by the time
        // that the bounding box culling code and display occur.  We multiply the localToWorld of the bounding box by the
        // inverse transform matrix to undo any tranformation done by the tile transform when displaying or using the bounding volume for culling.
        _boundingBoxLocalToWorld = getInverseMatrix() * localToWorld;
        _boundingBox = bb;
    }
    else if (volume.orientedBox().isSet())
    {
        // Cull and measure distances against the box itself, in a frame aligned with its axes, rather than an
        // axis-aligned box around it, which can be several times larger for a tilted box.
        const osg::Matrixd& m = volume.orientedBox().get();
        osg::Vec3d halfAxes[3] = {
            osg::Vec3d(m(0, 0), m(0, 1), m(0, 2)),
            osg::Vec3d(m(1, 0), m(1, 1), m(1, 2)),
            osg::Vec3d(m(2, 0), m(2, 1), m(2, 2)) };

        // Build an orthonormal frame from the two longest half-axes (a flat box has a zero-length one).
        int longest = 0;
        for (int i = 1; i < 3; ++i)
        {
            if (halfAxes[i].length2() > halfAxes[longest].length2())
                longest = i;
        }
        int second = (longest + 1) % 3;
        if (halfAxes[(longest + 2) % 3].length2() > halfAxes[second].length2())
            second = (longest + 2) % 3;

        osg::Vec3d frame[3];
        frame[0] = halfAxes[longest];
        double length0 = frame[0].normalize();
        frame[1] = halfAxes[second] - frame[0] * (halfAxes[second] * frame[0]);
        double length1 = frame[1].normalize();

        if (length0 > 0.0 && length1 > 0.0)
        {
            frame[2] = frame[0] ^ frame[1];

            // The extent along each frame axis is the sum of the half-axes' projections onto it: exactly the
            // box when its axes are orthogonal, and still enclosing it if they are slightly skewed.
            double extents[3];
            for (int k = 0; k < 3; ++k)
            {
                extents[k] = 0.0;
                for (int i = 0; i < 3; ++i)
                    extents[k] += std::abs(halfAxes[i] * frame[k]);
            }

            _boundingBoxLocalToWorld.set(
                frame[0].x(), frame[0].y(), frame[0].z(), 0.0,
                frame[1].x(), frame[1].y(), frame[1].z(), 0.0,
                frame[2].x(), frame[2].y(), frame[2].z(), 0.0,
                m(3, 0), m(3, 1), m(3, 2), 1.0);
            _boundingBox.set(-extents[0], -extents[1], -extents[2], extents[0], extents[1], extents[2]);
        }
        else
        {
            // A degenerate box (a line or a point): use its axis-aligned bounds, kept relative to the center.
            _boundingBoxLocalToWorld.makeTranslate(m.getTrans());
            _boundingBox.set(
                volume.box()->_min - m.getTrans(),
                volume.box()->_max - m.getTrans());
        }
    }
    else if (volume.box().isSet())
    {
        _boundingBox = volume.box().get();
    }

    if (_boundingBox.valid())
    {
        _localBoundingSphere.set(_boundingBox.center() * _boundingBoxLocalToWorld, _boundingBox.radius());
    }
    else if (volume.sphere().isSet())
    {
        _localBoundingSphere.set(volume.sphere()->center(), volume.sphere()->radius());
    }
}

osg::ref_ptr<osg::Geode> buildSphere(const double radius,
    const unsigned int rings,
    const unsigned int sectors,
    const osg::Vec4& color)
{
    osg::ref_ptr<osg::Geode>      sphereGeode = new osg::Geode;
    osg::ref_ptr<osg::Geometry>   sphereGeometry = new osg::Geometry;
    osg::ref_ptr<osg::Vec3Array>  sphereVertices = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec3Array>  sphereNormals = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec2Array>  sphereTexCoords = new osg::Vec2Array;
    osg::ref_ptr< osg::Vec4Array > sphereColors = new osg::Vec4Array;

    float const R = 1. / static_cast<float>(rings - 1);
    float const S = 1. / static_cast<float>(sectors - 1);

    sphereGeode->addDrawable(sphereGeometry);

    // Establish texture coordinates, vertex list, and normals
    for (unsigned int r(0); r < rings; ++r) {
        for (unsigned int s(0); s < sectors; ++s) {
            float const y = sin(-osg::PI_2 + osg::PI * r * R);
            float const x = cos(2 * osg::PI * s * S) * sin(osg::PI * r * R);
            float const z = sin(2 * osg::PI * s * S) * sin(osg::PI * r * R);

            //sphereTexCoords->push_back(osg::Vec2(s * R, r * R));

            sphereVertices->push_back(osg::Vec3(x * radius,
                y * radius,
                z * radius))
                ;
            sphereNormals->push_back(osg::Vec3(x, y, z));

        }
    }

    sphereGeometry->setVertexArray(sphereVertices.get());
    //sphereGeometry->setTexCoordArray(0, sphereTexCoords);
    sphereColors->push_back(color);
    sphereGeometry->setColorArray(sphereColors.get(), osg::Array::BIND_OVERALL);

    osg::ref_ptr<osg::DrawElementsUInt> faces = new osg::DrawElementsUInt(osg::PrimitiveSet::LINE_LOOP);
    sphereGeometry->addPrimitiveSet(faces.get());

    // Generate quads for each face.
    for (unsigned int r(0); r < rings - 1; ++r) {
        for (unsigned int s(0); s < sectors - 1; ++s) {
            // Corners of quads should be in CCW order.
            faces->push_back((r + 0) * sectors + (s + 0)); // ll
            faces->push_back((r + 0) * sectors + (s + 1)); // lr
            faces->push_back((r + 1) * sectors + (s + 1)); // ur

            faces->push_back((r + 0) * sectors + (s + 0)); // ll
            faces->push_back((r + 1) * sectors + (s + 1)); // ur
            faces->push_back((r + 1) * sectors + (s + 0)); // ul
        }
    }


    return sphereGeode;
}

void ThreeDTileNode::createDebugBounds()
{
    const osg::Vec4 color = randomColor();

    if (_tile->boundingVolume()->sphere().isSet())
    {
        osg::MatrixTransform* mt = new osg::MatrixTransform;
        mt->setMatrix(osg::Matrixd::translate(_localBoundingSphere.center()));
        mt->addChild(buildSphere(_localBoundingSphere.radius(), 20, 20, color));
        _boundsDebug = mt;
    }
    else if (_boundingBox.valid())
    {
        const int index[24] = {
                0, 1, 1, 2, 2, 3, 3, 0,
                4, 5, 5, 6, 6, 7, 7, 4,
                0, 4, 1, 5, 2, 6, 3, 7
        };

        std::vector< osg::Vec3 > corners;
        LineDrawable* d = new LineDrawable(GL_LINES);
        d->setUseGPU(false);
        corners.push_back(osg::Vec3(_boundingBox.xMin(), _boundingBox.yMin(), _boundingBox.zMin()));
        corners.push_back(osg::Vec3(_boundingBox.xMax(), _boundingBox.yMin(), _boundingBox.zMin()));
        corners.push_back(osg::Vec3(_boundingBox.xMax(), _boundingBox.yMax(), _boundingBox.zMin()));
        corners.push_back(osg::Vec3(_boundingBox.xMin(), _boundingBox.yMax(), _boundingBox.zMin()));
        corners.push_back(osg::Vec3(_boundingBox.xMin(), _boundingBox.yMin(), _boundingBox.zMax()));
        corners.push_back(osg::Vec3(_boundingBox.xMax(), _boundingBox.yMin(), _boundingBox.zMax()));
        corners.push_back(osg::Vec3(_boundingBox.xMax(), _boundingBox.yMax(), _boundingBox.zMax()));
        corners.push_back(osg::Vec3(_boundingBox.xMin(), _boundingBox.yMax(), _boundingBox.zMax()));

        for (int i = 0; i < 24; ++i)
            d->pushVertex(corners[index[i]]);

        d->setColor(color);
        d->finish();

        osg::MatrixTransform* transform = new osg::MatrixTransform;
        transform->setMatrix(_boundingBoxLocalToWorld);
        transform->addChild(d);
        _boundsDebug = transform;
    }
}


osg::BoundingSphere ThreeDTileNode::computeBound() const
{
    return _tile->getBoundingSphere();
}

osg::Node* ThreeDTileNode::getContent()
{
    return _content.get();
}

bool ThreeDTileNode::isContentReady()
{
    resolveContent();
    return _content.valid();
}

void ThreeDTileNode::resolveContent()
{
    // Resolve the future
    if (!_content.valid() && _requestedContent && _contentFuture.available())
    {
        _content = _contentFuture.value();

        if (!_content.valid())
        {
            // The load failed, maybe because the server is throttling requests. Allow another request after a
            // backoff that doubles with each failure (up to 30 s), rather than leaving this tile, and any parent
            // waiting to refine to it, stuck for good.
            _requestedContent = false;
            _contentFuture.abandon();
            double backoff = std::min(30.0, 0.5 * (double)(1u << std::min(_loadFailures, 6u)));
            ++_loadFailures;
            _retryTime = osg::Timer::instance()->time_s() + backoff;
        }
        else
        {
            _loadFailures = 0u;

            // Assign the parent node if we just loaded a tileset
            ThreeDTilesetContentNode* tilesetContentNode = dynamic_cast<ThreeDTilesetContentNode*>(_content.get());
            if (tilesetContentNode)
            {
                ThreeDTileNode* tileNode = tilesetContentNode->getTileNode();
                if (tileNode)
                {
                    tileNode->setParentTile(this);
                }
            }

            _tileset->runPreMergeOperations(_content.get());
            _tileset->runPostMergeOperations(_content.get());

            addChild(_content.get());
        }
    }
}


bool ThreeDTileNode::requestContent(ICO* ico)
{
    if (_content.valid() || _requestedContent || !hasContent())
    {
        return false;
    }

    // After a failed load, wait out the backoff before trying again.
    if (_loadFailures > 0u && osg::Timer::instance()->time_s() < _retryTime)
    {
        return false;
    }

    URIContext context = _tile->content()->uri()->context();
    if (!_tileset->getAuthorizationHeader().empty())
    {
        context.addHeader("authorization", _tileset->getAuthorizationHeader());
    }

    URI uri(_tile->content()->uri()->base(), context);

    std::shared_ptr<void> slot = _tileset->acquireLoadSlot();

    if (osgEarth::Strings::endsWith(osgEarth::removeQueryParams(_tile->content()->uri()->base()), ".json"))
    {
        // "json" extension = external tileset:
        NetworkMonitor::ScopedRequestLayer layerRequest(_tileset->getOwnerName());
        _contentFuture = readTilesetAsync(_tileset, uri, _options.get(), ico, slot);
    }
    else
    {
        // else, actual content:
        _contentFuture = readTileContentAsync(
            uri, _options, _tileset->getOwnerName(), _tileset->_processContent, ico, slot);
    }

    _requestedContent = true;
    return true;
}

void ThreeDTileNode::cancelContentRequest()
{
    if (isContentRequestPending())
    {
        _contentFuture.abandon();
        _requestedContent = false;
    }
}

bool ThreeDTileNode::isContentRequestPending() const
{
    return _requestedContent && !_content.valid() && !_contentFuture.available();
}

bool ThreeDTileNode::prepareForRefine(osgUtil::CullVisitor* cv, double priority)
{
    // Touch the tile even when it's outside the view and won't be traversed: its parent can't refine until it
    // is ready, so it must not expire while the parent needs it.
    updateTracking(cv);

    // An external tileset is drawn through its root tile, so that has to be drawable as well.
    ThreeDTileNode* root = nullptr;
    if (hasContent())
    {
        if (!isContentReady())
        {
            _tileset->requestTileContent(this, priority);
            return false;
        }

        ThreeDTilesetContentNode* tilesetContent = dynamic_cast<ThreeDTilesetContentNode*>(_content.get());
        root = tilesetContent ? tilesetContent->getTileNode() : nullptr;
        if (!root)
        {
            // Ordinary loaded content is drawable as is. This is the usual case, so return before paying for the
            // modelview push below, which only the external root and empty-tile cases measure anything in.
            return true;
        }
    }
    else if (!_children.valid() || _children->getNumChildren() == 0)
    {
        return true;
    }

    // The parent calls this from its own frame; measure this tile and its descendants in theirs.
    osg::ref_ptr<osg::RefMatrix> matrix = new osg::RefMatrix(*cv->getModelViewMatrix());
    computeLocalToWorldMatrix(*matrix, cv);
    cv->pushModelViewMatrix(matrix.get(), getReferenceFrame());

    bool ready = true;
    if (root)
    {
        ready = root->prepareForRefine(cv, priority);
    }
    else
    {
        // A tile without content draws its children instead, but only once its own error calls for it (see
        // traverse); until then it draws nothing, so it isn't ready and the parent keeps drawing. Its children
        // are requested from the same point as any other tile's, so nothing out of range is loaded or kept.
        const double maxSSE = _tileset->getMaximumScreenSpaceError();
        const double error = computeScreenSpaceError(cv);
        ready = error > maxSSE;
        if (error > 0.8 * maxSSE)
        {
            for (unsigned int i = 0; i < _children->getNumChildren(); i++)
            {
                ThreeDTileNode* childTile = dynamic_cast<ThreeDTileNode*>(_children->getChild(i));
                if (childTile && !childTile->prepareForRefine(cv, priority))
                {
                    ready = false;
                }
            }
        }
    }

    cv->popModelViewMatrix();
    return ready;
}

double ThreeDTileNode::getDistanceToTile(osgUtil::CullVisitor* cv)
{
    osg::Matrixd modelView(*cv->getModelViewMatrix());
    double distance;

    if (_boundingBox.valid())
    {
        // Find the eye in the box's frame; the closest point of the box is the eye clamped to it.
        osg::Matrixd viewToBox;
        viewToBox.invert(_boundingBoxLocalToWorld * modelView);
        osg::Vec3d eye = viewToBox.getTrans();
        osg::Vec3d closest(
            osg::clampBetween(eye.x(), _boundingBox.xMin(), _boundingBox.xMax()),
            osg::clampBetween(eye.y(), _boundingBox.yMin(), _boundingBox.yMax()),
            osg::clampBetween(eye.z(), _boundingBox.zMin(), _boundingBox.zMax()));
        distance = (eye - closest).length();
    }
    else
    {
        osg::Matrixd viewToLocal;
        viewToLocal.invert(modelView);
        distance = osg::maximum(
            (viewToLocal.getTrans() - _localBoundingSphere.center()).length() - _localBoundingSphere.radius(),
            0.0);
    }

    return distance * cv->getLODScale();
}

double ThreeDTileNode::computeLoadPriority(const osg::Matrixd& modelView, double error) const
{
    // Like Cesium's foveated priority, favor tiles toward the center of the view: the weight runs from 1
    // straight ahead down to 0.25 directly behind the eye (or 1 when the eye is inside the tile's sphere).
    osg::Vec3d center = _localBoundingSphere.center() * modelView;
    double length = center.length();
    double cosAngle = length > _localBoundingSphere.radius() ? -center.z() / length : 1.0;
    return error * (0.25 + 0.375 * (1.0 + cosAngle));
}

double ThreeDTileNode::computeScreenSpaceError(osgUtil::CullVisitor* cv)
{
    double distance = osg::maximum(getDistanceToTile(cv), 0.0000001);
    const osg::Matrix& proj = cv->getCurrentCamera()->getProjectionMatrix();
    if (ProjectionMatrix::isPerspective(proj))
    {
        double height = cv->getCurrentCamera()->getViewport()->height();
        return (*_tile->geometricError() * height) / (distance * _tileset->getSSEDenominator());
    }
    else // orthographic
    {
        const osg::Viewport* vp = cv->getCurrentCamera()->getViewport();
        double L, R, B, T, N, F;
        ProjectionMatrix::getOrtho(proj, L, R, B, T, N, F);
        double pixelSize = std::max(T-B, R-L) / std::max(vp->width(), vp->height());
        return (*_tile->geometricError()) / pixelSize;
    }
}

bool ThreeDTileNode::unloadContent()
{
    // Don't unload the content of tiles that were loaded immediately.  This shouldn't be called as they aren't tracked, but just in case.
    if (_immediateLoad)
    {
        return false;
    }

    if (_content.valid())
    {
        // Don't call releaseGLObjects() here. This runs in the update traversal, possibly while the draw thread is
        // still rendering, and tile content shares state with the tiles that stay loaded (every glTF tile uses
        // the one PBRTexture program), so releasing it would delete GL objects that are in use. Dropping the node
        // is enough: OSG queues the GL objects it alone owned for deletion on the draw thread.
        removeChild(_content.get());
        _content = nullptr;
    }

    _firstVisit = true;
    _content = 0;
    _requestedContent = false;
    _contentFuture.abandon(); // = Future<osg::ref_ptr<osg::Node>>();
    _loadFailures = 0u;

    return true;
}

void ThreeDTileNode::resizeGLObjectBuffers(unsigned int maxSize)
{
    osg::MatrixTransform::resizeGLObjectBuffers(maxSize);

    if (_content.valid())
    {
        _content->resizeGLObjectBuffers(maxSize);
    }

    if (_children.valid())
    {
        _children->resizeGLObjectBuffers(maxSize);
    }
}

void ThreeDTileNode::releaseGLObjects(osg::State* state) const
{
    if (_content.valid())
    {
        _content->releaseGLObjects(state);
    }

    if (_children.valid())
    {
        _children->releaseGLObjects(state);
    }

    if (_boundsDebug.valid())
    {
        _boundsDebug->releaseGLObjects(state);
    }
}

unsigned int ThreeDTileNode::getLastCulledFrameNumber() const
{
    return _lastCulledFrameNumber;
}

float ThreeDTileNode::getLastCulledFrameTime() const
{
    return _lastCulledFrameTime;
}

void ThreeDTileNode::updateTracking(osgUtil::CullVisitor* cv)
{
    // Update tracking if this node wasn't immediately loaded.  Tiles that were immediately loaded are expected to be tracked by their parent.
    if (hasContent() && !_immediateLoad)
    {
        _tileset->touchTile(this);
        _lastCulledFrameNumber = cv->getFrameStamp()->getFrameNumber();
        _lastCulledFrameTime = cv->getFrameStamp()->getReferenceTime();
    }
}

void ThreeDTileNode::traverse(osg::NodeVisitor& nv)
{
    if (nv.getVisitorType() == nv.CULL_VISITOR)
    {
        osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(&nv);
        osg::Matrixd modelView(*cv->getModelViewMatrix());

        // Skip tiles hidden behind the planet. Besides saving their loads, this stops tiles on the far side of
        // the globe from stretching the computed far plane so far that the near plane clips the ground under
        // the camera.
        CullContext* context = s_cullContext;
        if (context && context->horizonValid && _localBoundingSphere.valid())
        {
            osg::Vec3d worldCenter = _localBoundingSphere.center() * modelView * context->inverseView;
            if (!context->horizon.isVisible(worldCenter, _localBoundingSphere.radius()))
            {
                return;
            }
        }

        if (_boundingBox.valid())
        {
            osg::ref_ptr< osg::RefMatrix > refMatrix = new osg::RefMatrix(*cv->getModelViewMatrix());
            refMatrix->preMult(_boundingBoxLocalToWorld);
            cv->pushModelViewMatrix(refMatrix.get(), getReferenceFrame());
            bool culled = cv->isCulled(_boundingBox);
            cv->popModelViewMatrix();

            if (culled)
            {
                return;
            }
        }

        resolveContent();

        // Compute the SSE
        double error = computeScreenSpaceError(cv);

        updateTracking(cv);

        // Loads for this tile and its children are ranked by how far it is from meeting the error target.
        double priority = computeLoadPriority(modelView, error);

        // A drawn tile's content is normally loaded already, but a tileset's root, or a tile drawn by a parent
        // without content of its own, may still be waiting for it (and this lets unloaded nodes reload).
        if (hasContent() && !_content.valid())
        {
            _tileset->requestTileContent(this, priority);
        }

        // Children are only needed once this tile wants to refine. Requests start a little before the
        // refinement threshold so the children are usually ready by the time it is crossed. Children of a
        // tile below that point are neither requested nor touched, so the tileset's expiry can unload them.
        // A tile without content has nothing of its own to draw, so it refines without waiting for its children,
        // but still only once its error calls for it: empty tiles are how a tileset holds back content until the
        // camera comes within range.
        const double maxSSE = _tileset->getMaximumScreenSpaceError();
        const double requestChildrenSSE = 0.8 * maxSSE;
        const bool empty = !hasContent();
        bool refine = false;

        if (_children.valid() && _children->getNumChildren() > 0 && error > requestChildrenSSE)
        {
            bool areChildrenReady = true;
            for (unsigned int i = 0; i < _children->getNumChildren(); i++)
            {
                ThreeDTileNode* childTile = dynamic_cast<ThreeDTileNode*>(_children->getChild(i));
                if (childTile && !childTile->prepareForRefine(cv, priority))
                {
                    areChildrenReady = false;
                }
            }

            // Keep drawing this tile until every child is ready, so refining never leaves a hole. An additive tile
            // keeps drawing its own content when it refines, so it doesn't wait: each child shows once it's ready,
            // even while a sibling isn't (an empty one out of range may not be for a long time).
            refine = error > maxSSE && (empty || _refine == REFINE_ADD || areChildrenReady);
        }

        if (refine)
        {
            if (_content.valid() && _refine == REFINE_ADD)
            {
                _content->accept(nv);
            }

            if (_tileset->getShowBoundingVolumes() && _boundsDebug.valid())
            {
                _boundsDebug->accept(nv);
            }

            if (_children.valid())
            {
                _children->accept(nv);
            }
        }
        else
        {
            if (_content.valid())
            {
                _content->accept(nv);
            }

            if (_tileset->getShowBoundingVolumes() && _boundsDebug.valid())
            {
                _boundsDebug->accept(nv);
            }
        }
    }
    else if (nv.getTraversalMode() == osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN)
    {
        resolveContent();
        bool areChildrenReady = true;
        if (_children.valid())
        {
            for (unsigned int i = 0; i < _children->getNumChildren(); i++)
            {
                osg::ref_ptr< ThreeDTileNode > childTile = dynamic_cast<ThreeDTileNode*>(_children->getChild(i));
                if (childTile.valid())
                {
                    // Can we traverse the child?
                    if (childTile->hasContent() && !childTile->isContentReady())
                    {
                        areChildrenReady = false;
                    }
                }
            }
        }
        else
        {
            areChildrenReady = false;
        }

        // As in the cull, a tile without content of its own always stands in for its children.
        if ((areChildrenReady || !hasContent()) && _children.valid() && _children->getNumChildren() > 0)
        {
            if (_content.valid() && _refine == REFINE_ADD)
            {
                _content->accept(nv);
            }

            if (_children.valid())
            {
                _children->accept(nv);
            }
        }
        else
        {
            if (_content.valid())
            {
                _content->accept(nv);
            }
        }
    }
    else if (nv.getTraversalMode() == osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
    {
        resolveContent();
        if (_content.valid())
        {
            _content->accept(nv);
        }

        if (_children.valid())
        {
            _children->accept(nv);
        }
    }

}

ThreeDTilesetNode::ThreeDTilesetNode(Tileset* tileset, const std::string& authorizationHeader,
    SceneGraphCallbacks* sceneGraphCallbacks, osgDB::Options* options, const ContentProcessor& processContent) :
    _processContent(processContent),
    _tileset(tileset),
    _options(options),
    _maximumScreenSpaceError(15.0f),
    _maxTiles(50),
    _showBoundingVolumes(false),
    _maxAge(5.0f),
    _maxSimultaneousLoads(0u),
    _loadsInFlight(std::make_shared<std::atomic_uint>(0u)),
    _lastExpiredFrame(0),
    _authorizationHeader(authorizationHeader),
    _sgCallbacks(sceneGraphCallbacks),
	_sseDenominator(1.0)
{
    ADJUST_UPDATE_TRAV_COUNT(this, +1);
    const char* c = ::getenv("OSGEARTH_3DTILES_CACHE_SIZE");
    if (c)
    {
        setMaxTiles((unsigned)atoi(c));
    }

    c = ::getenv("OSGEARTH_3DTILES_MAX_AGE");
    if (c)
    {
        setMaxAge((float)atof(c));
    }

    c = ::getenv("OSGEARTH_3DTILES_MAX_LOADS");
    setMaxSimultaneousLoads(c ? (unsigned)atoi(c) : DEFAULT_MAX_SIMULTANEOUS_LOADS);

    _tracker.push_back(0);
    // Pointer to last element
    _sentryItr = --_tracker.end();

    if (!_options.valid())
    {
        _options = new osgDB::Options;
    }

    // Tile content is glTF, so let the glTF reader compress and mipmap its textures. It keeps color textures sRGB
    // and caches the prepared images, so a texture shared by several tiles is only prepared once.
    std::string optString = _options->getOptionString();
    optString += " gltfPrepareTextures";

    // If the gltfUpAxis property is set to z we don't need to do the y up to z up transformation
    // so we set an option string telling the gltf loader to not apply the transformation for this tileset.
    if (tileset->asset().isSet() && osgEarth::toLower(*tileset->asset()->gltfUpAxis()) == "z")
    {
        optString += " gltfZUp";
    }
    _options->setOptionString(optString);

    addChild(new ThreeDTilesetContentNode(this, tileset, _options.get()));
}

const std::string&
ThreeDTilesetNode::getOwnerName() const
{
    return _ownerName;
}

void
ThreeDTilesetNode::setOwnerName(const std::string& value)
{
    _ownerName = value;
}


unsigned int ThreeDTilesetNode::getMaxTiles() const
{
    return _maxTiles;
}

void ThreeDTilesetNode::setMaxTiles(unsigned int maxTiles)
{
    _maxTiles = maxTiles;
}

float ThreeDTilesetNode::getMaxAge() const
{
    return _maxAge;
}

void ThreeDTilesetNode::setMaxAge(float maxAge)
{
    _maxAge = maxAge;
}

unsigned int ThreeDTilesetNode::getMaxSimultaneousLoads() const
{
    return _maxSimultaneousLoads;
}

void ThreeDTilesetNode::setMaxSimultaneousLoads(unsigned int value)
{
    _maxSimultaneousLoads = std::max(value, 1u);

    // Every load needs a pool thread to itself, or it would wait behind the others regardless of priority.
    jobs::jobpool* pool = jobs::get_pool(JOB_POOL_NAME);
    if (pool->concurrency() < _maxSimultaneousLoads)
    {
        pool->set_concurrency(_maxSimultaneousLoads);
    }
}

std::shared_ptr<void> ThreeDTilesetNode::acquireLoadSlot()
{
    return std::make_shared<LoadSlot>(_loadsInFlight);
}

void ThreeDTilesetNode::requestTileContent(ThreeDTileNode* node, double priority)
{
    CullContext* context = s_cullContext;
    if (context && context->tileset == this)
    {
        context->requests.emplace_back(priority, node);
    }
    else if (node)
    {
        node->requestContent(nullptr);
    }
}

void ThreeDTilesetNode::cancelStaleRequests(double frameTime)
{
    std::lock_guard<std::mutex> lock(_loadingMutex);
    for (auto i = _loading.begin(); i != _loading.end(); )
    {
        osg::ref_ptr<ThreeDTileNode> tile;
        if (!i->lock(tile) || !tile->isContentRequestPending())
        {
            i = _loading.erase(i);
        }
        else if (frameTime - tile->getLastCulledFrameTime() > STALE_REQUEST_AGE)
        {
            // No longer wanted (out of view, or its parent no longer refines), so free the slot.
            tile->cancelContentRequest();
            i = _loading.erase(i);
        }
        else
        {
            ++i;
        }
    }
}

void ThreeDTilesetNode::dispatchRequests(
    std::vector<std::pair<double, ThreeDTileNode*>>& requests, ICO* ico, double frameTime)
{
    cancelStaleRequests(frameTime);

    unsigned int inFlight = *_loadsInFlight;
    if (requests.empty() || inFlight >= _maxSimultaneousLoads)
    {
        return;
    }

    unsigned int slots = _maxSimultaneousLoads - inFlight;
    std::sort(requests.begin(), requests.end(),
        [](const std::pair<double, ThreeDTileNode*>& lhs, const std::pair<double, ThreeDTileNode*>& rhs)
        {
            return lhs.first > rhs.first;
        });

    std::lock_guard<std::mutex> lock(_loadingMutex);
    for (auto& request : requests)
    {
        if (slots == 0u)
        {
            break;
        }

        // A tile can be queued more than once per frame; only the first request starts a load.
        if (request.second->requestContent(ico))
        {
            _loading.emplace_back(request.second);
            --slots;
        }
    }
}

float ThreeDTilesetNode::getMaximumScreenSpaceError() const
{
    return _maximumScreenSpaceError;
}

void ThreeDTilesetNode::setMaximumScreenSpaceError(float maximumScreenSpaceError)
{
    _maximumScreenSpaceError = maximumScreenSpaceError;
}

bool ThreeDTilesetNode::getShowBoundingVolumes() const
{
    return _showBoundingVolumes;
}

void ThreeDTilesetNode::setShowBoundingVolumes(bool showBoundingVolumes)
{
    _showBoundingVolumes = showBoundingVolumes;
}

double ThreeDTilesetNode::getSSEDenominator() const
{
	return _sseDenominator;
}

SceneGraphCallbacks* ThreeDTilesetNode::getSceneGraphCallbacks(SceneGraphCallbacks* callbacks)
{
    return _sgCallbacks.get();
}

void ThreeDTilesetNode::setSceneGraphCallbacks(SceneGraphCallbacks* callbacks)
{
    _sgCallbacks = callbacks;
}

void
ThreeDTilesetNode::runPreMergeOperations(osg::Node* node)
{
    if (_sgCallbacks.valid())
    {
        _sgCallbacks->firePreMergeNode(node);
    }
}

void
ThreeDTilesetNode::runPostMergeOperations(osg::Node* node)
{
    if (_sgCallbacks.valid())
    {
        _sgCallbacks->firePostMergeNode(node);
    }
}

void ThreeDTilesetNode::touchTile(ThreeDTileNode* node)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (node->_trackerItrValid)
    {
        _tracker.erase(node->_trackerItr);
    }

    _tracker.push_back(node);
    node->_trackerItrValid = true;
    node->_trackerItr = --_tracker.end();
}

void ThreeDTilesetNode::expireTiles(const osg::NodeVisitor& nv)
{
    OE_PROFILING_ZONE;

    unsigned int frameNumber = nv.getFrameStamp()->getFrameNumber();
    float frameTime = nv.getFrameStamp()->getReferenceTime();

    osg::Timer_t startTime = osg::Timer::instance()->tick();
    osg::Timer_t endTime;

    std::lock_guard<std::mutex> lock(_mutex);

    // Max time in ms to allocate to erasing tiles
    float maxTime = 2.0f;

    ThreeDTileNode::TileTracker::iterator itr = _tracker.begin();

    unsigned int numErased = 0;
    unsigned int numSkipped = 0;
    while (_tracker.size() > _maxTiles && itr != _sentryItr)
    {
        osg::ref_ptr< ThreeDTileNode > tile = dynamic_cast<ThreeDTileNode*>(itr->get());
        if (tile.valid())
        {
            float age = frameTime - tile->getLastCulledFrameTime();
            bool canUnload = tile->getAutoUnload() && age >= _maxAge;

            if (canUnload && tile->unloadContent())
            {
                tile->_trackerItrValid = false;
                itr = _tracker.erase(itr);
                ++numErased;
            }
            else
            {
                numSkipped++;
                ++itr;
            }
        }

        endTime = osg::Timer::instance()->tick();
        if (osg::Timer::instance()->delta_m(startTime, endTime) > maxTime)
        {
            break;
        }
    }

#if 0
    if (numErased > 0 || numSkipped > 0)
    {
        OE_NOTICE << "Erased " << numErased << " and skipped " << numSkipped << " in " << osg::Timer::instance()->delta_m(startTime, endTime) << "ms" << std::endl;
    }
    OE_NOTICE << "Tiles in memory " << _tracker.size() << " max tiles=" << _maxTiles << std::endl;
#endif

    // Erase the sentry and stick it at the end of the list
    _tracker.erase(_sentryItr);
    _tracker.push_back(0);
    _sentryItr = --_tracker.end();
}

void ThreeDTilesetNode::traverse(osg::NodeVisitor& nv)
{
    if (nv.getVisitorType() == nv.UPDATE_VISITOR)
    {
        // Check to make sure expireTiles is only called once per frame, even if the UpdateVisitor is sent down multiple times.
        // This can happen if the node has multiple parents.
        if (nv.getFrameStamp()->getFrameNumber() > _lastExpiredFrame)
        {
            expireTiles(nv);
            _lastExpiredFrame = nv.getFrameStamp()->getFrameNumber();
        }
    }
	else if (nv.getVisitorType() == nv.CULL_VISITOR)
	{
		osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(&nv);
		const osg::Matrix& proj = cv->getCurrentCamera()->getProjectionMatrix();
		double fovy, ar, zn, zf;
        ProjectionMatrix::getPerspective(proj, fovy, ar, zn, zf);
		_sseDenominator = 2.0 * tan(0.5 * deg2rad(fovy));

        CullContext context;
        context.tileset = this;
        context.inverseView = cv->getCurrentCamera()->getInverseViewMatrix();

        // Horizon culling only applies to a tileset on a geocentric map.
        osg::ref_ptr<MapNode> mapNode;
        if (ObjectStorage::get(&nv, mapNode) && mapNode->isGeocentric())
        {
            const Ellipsoid& ellipsoid = mapNode->getMapSRS()->getEllipsoid();
            context.horizon.setEllipsoid(Ellipsoid(
                ellipsoid.getRadiusEquator() - HORIZON_ELLIPSOID_MARGIN,
                ellipsoid.getRadiusPolar() - HORIZON_ELLIPSOID_MARGIN));
            context.horizon.setEye(context.inverseView.getTrans(), cv->getProjectionMatrix());
            context.horizonValid = true;
        }

        CullContext* outerContext = s_cullContext;
        s_cullContext = &context;
        osg::Group::traverse(nv);
        s_cullContext = outerContext;

        // Start the most important loads this traversal asked for.
        ICO* ico = nullptr;
        osgViewer::View* osgView = dynamic_cast<osgViewer::View*>(cv->getCurrentCamera()->getView());
        if (osgView && osgView->getDatabasePager())
        {
            ico = osgView->getDatabasePager()->getIncrementalCompileOperation();
        }
        dispatchRequests(context.requests, ico, nv.getFrameStamp()->getReferenceTime());
        return;
	}

    osg::Group::traverse(nv);
}

ThreeDTilesetContentNode::ThreeDTilesetContentNode(ThreeDTilesetNode* tilesetNode, Tileset* tileset, osgDB::Options* options) :
    _tilesetNode(tilesetNode),
    _tileset(tileset),
    _options(options),
    _tileNode(0)
{
    // Set up the root tile.
    if (tileset->root().valid())
    {
        _tileNode = new ThreeDTileNode(_tilesetNode, tileset->root().get(), true, _options.get());
        addChild(_tileNode);
    }
}

ThreeDTileNode* ThreeDTilesetContentNode::getTileNode()
{
    return _tileNode;
}