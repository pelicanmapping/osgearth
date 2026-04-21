/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

/**
 * This app demonstrates the use of TerrainEngine::createTile(), which lets
 * you create the geometry for an arbitrary terrain tile that you can use for
 * external purposes.
 */

#include <osgGA/StateSetManipulator>
#include <osgGA/GUIEventHandler>
#include <osgViewer/CompositeViewer>
#include <osgViewer/ViewerEventHandlers>
#include <osgUtil/LineSegmentIntersector>
#include <osgEarth/MapNode>
#include <osgEarth/TerrainEngineNode>
#include <osgEarth/StringUtils>
#include <osgEarth/Terrain>
#include <osgEarth/GeoTransform>
#include <osgEarth/EarthManipulator>
#include <osgEarth/Controls>
#include <osgEarth/ExampleResources>
#include <osgEarth/GLUtils>
#include <osgEarth/VirtualProgram>
#include <osg/TriangleFunctor>
#include <osg/ShapeDrawable>
#include <osg/Depth>
#include <osgGA/TrackballManipulator>
#include <iomanip>
#include <sstream>

using namespace osgEarth;
using namespace osgEarth::Util;

static MapNode* s_mapNode = nullptr;
static osgViewer::View* s_tile_view = nullptr;
static osg::Group* s_root = nullptr;
static bool s_extractTriangles = false;

TileKey makeTileKey(const std::string& str, const Profile* profile)
{
    std::istringstream stream(str);
    unsigned lod = 0, x = 0, y = 0;
    stream >> lod;
    if (stream.fail() || stream.peek() != '/')
        return TileKey();
    stream.ignore(1);
    stream >> x;
    if (stream.fail() || stream.peek() != '/')
        return TileKey();
    stream.ignore(1);
    stream >> y;
    if (stream.fail())
        return TileKey();
    return TileKey(lod, x, y, profile);
}

osg::Node* installMultiPassRendering(osg::Node* node)
{
    osg::Group* geode = new osg::Group();
    geode->getOrCreateStateSet()->setAttributeAndModes(new osg::Depth(osg::Depth::LESS, 0, 1, true));
    geode->addChild(node);
    geode->setCullingActive(false);

    osg::Group* wireframeGroup = new osg::Group();
    wireframeGroup->getOrCreateStateSet()->setAttributeAndModes(new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::LINE), 1);
    wireframeGroup->getOrCreateStateSet()->setAttributeAndModes(new osg::Depth(osg::Depth::LEQUAL, 0, 1, true));
    wireframeGroup->getOrCreateStateSet()->setDefine("WIREFRAME");
    wireframeGroup->addChild(node);
    geode->addChild(wireframeGroup);

    osg::Group* pointframeGroup = new osg::Group();
    pointframeGroup->getOrCreateStateSet()->setAttributeAndModes(new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::POINT), 1);
    pointframeGroup->getOrCreateStateSet()->setMode(GL_PROGRAM_POINT_SIZE, 1);
    pointframeGroup->getOrCreateStateSet()->setAttributeAndModes(new osg::Depth(osg::Depth::LEQUAL, 0, 1, true));
    pointframeGroup->getOrCreateStateSet()->setDefine("POINTFRAME");
    pointframeGroup->addChild(node);
    geode->addChild(pointframeGroup);

    geode->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    geode->getOrCreateStateSet()->setRenderBinDetails(99, "RenderBin");

    const char* tri_vs = R"(
            #version 330
            #define VERTEX_VISIBLE       1 // draw it
            #define VERTEX_BOUNDARY      2 // vertex lies on a skirt boundary
            #define VERTEX_HAS_ELEVATION 4 // not subject to elevation texture
            #define VERTEX_SKIRT         8 // it's a skirt vertex (bitmask)
            #define VERTEX_CONSTRAINT   16 // part of a non-morphable constraint
            #pragma import_defines(WIREFRAME)
            #pragma import_defines(POINTFRAME)
            vec4 vp_Color;
            void colorize_vs(inout vec4 vertex)
            {
                vp_Color = vec4(0.2,0.2,0.2,1.0);

              #ifdef POINTFRAME
                gl_PointSize = 12.0;
                int m = int(gl_MultiTexCoord0.z);
                if ((m & VERTEX_CONSTRAINT) != 0)
                    vp_Color.r = 1.0;
                if ((m & VERTEX_BOUNDARY) != 0)
                    vp_Color.g = 1.0;
                if (m <= 1)
                    vp_Color.a = 0.0;
              #endif

              #ifdef WIREFRAME
                vp_Color = vec4(0.75);
              #endif
            }
        )";

    VirtualProgram::getOrCreate(geode->getOrCreateStateSet())->setFunction(
        "colorize_vs", tri_vs, VirtualProgram::LOCATION_VERTEX_VIEW);

    return geode;
}

struct CollectTriangles
{
    CollectTriangles()
    {
        verts = new osg::Vec3Array();
    }

    inline void operator () (const osg::Vec3& v1,const osg::Vec3& v2,const osg::Vec3& v3)
    {
        verts->push_back(v1);
        verts->push_back(v2);
        verts->push_back(v3);
    }

    osg::ref_ptr< osg::Vec3Array > verts;
};

struct CollectTrianglesVisitor : public osg::NodeVisitor
{
    CollectTrianglesVisitor():
        osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
    {
        _vertices = new osg::Vec3dArray();
    }

    void apply(osg::Transform& transform)
    {
        osg::Matrix matrix;
        if (!_matrixStack.empty()) matrix = _matrixStack.back();
        transform.computeLocalToWorldMatrix(matrix,this);
        pushMatrix(matrix);
        traverse(transform);
        popMatrix();
    }

    void apply(osg::Drawable& drawable) override
    {
        osg::TriangleFunctor<CollectTriangles> triangleCollector;
        drawable.accept(triangleCollector);
        for (unsigned int j = 0; j < triangleCollector.verts->size(); j++)
        {
            static osg::Matrix identity;
            osg::Matrix& matrix = _matrixStack.empty() ? identity : _matrixStack.back();
            osg::Vec3d v = (*triangleCollector.verts)[j];
            _vertices->push_back(v * matrix);
        }
    }

    osg::Node* buildNode()
    {
        osg::Geometry* geom = new osg::Geometry;
        osg::Vec3Array* verts = new osg::Vec3Array;
        geom->setVertexArray(verts);

        bool first = true;
        osg::Vec3d anchor;

        for (unsigned int i = 0; i < _vertices->size(); i++)
        {
            if (first)
            {
                anchor = (*_vertices)[i];
                first = false;
            }
            verts->push_back((*_vertices)[i] - anchor);
        }
        geom->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, verts->size()));

        osg::MatrixTransform* mt = new osg::MatrixTransform;
        mt->setMatrix(osg::Matrixd::translate(anchor));
        mt->addChild(installMultiPassRendering(geom));

        return mt;
    }

    inline void pushMatrix(osg::Matrix& matrix) { _matrixStack.push_back(matrix); }

    inline void popMatrix() { _matrixStack.pop_back(); }

    typedef std::vector<osg::Matrix> MatrixStack;
    osg::ref_ptr<osg::Vec3dArray>  _vertices;
    MatrixStack _matrixStack;
};



osg::Node* createBS(const osg::BoundingSphere& bounds)
{
    osg::MatrixTransform* mt = new osg::MatrixTransform;
    mt->setMatrix(osg::Matrixd::translate(bounds.center()));
    osg::ShapeDrawable* sd = new osg::ShapeDrawable(new osg::Sphere(osg::Vec3f(0, 0, 0), bounds.radius()));
    sd->setColor(osg::Vec4(1, 1, 0, 0.3));
    mt->addChild(sd);
    return mt;
}

osg::Node* createBBOX(const osg::BoundingBox& bbox)
{
    osg::MatrixTransform* mt = new osg::MatrixTransform;
    mt->setMatrix(osg::Matrixd::translate(bbox.center()));
    osg::ShapeDrawable* sd = new osg::ShapeDrawable(new osg::Sphere(osg::Vec3f(0, 0, 0), bbox.radius()));
    sd->setColor(osg::Vec4(1, 1, 0, 0.3));
    mt->addChild(sd);
    return mt;
}


// An event handler that will create a tile that can be used for intersections
struct CreateTileHandler : public osgGA::GUIEventHandler
{
    CreateTileHandler()
        : _tileLOD(15), _refLOD(0), _tileFlags(TerrainEngineNode::CREATE_TILE_INCLUDE_ALL)
    {
    }


    CreateTileHandler(osg::ArgumentParser& arguments)
        : CreateTileHandler()
    {
        // new tile LOD for interactive use
        arguments.read("--tilelod", _tileLOD);

        // specific key
        arguments.read("--tilekey", _keyString);

        // LOD at which to create tile geometry
        arguments.read("--reflod", _refLOD);

        // some optinos
        bool tilesWithMasks = false;
        bool tilesWithoutMasks = false;

        tilesWithMasks = arguments.read("--with-masks");
        tilesWithoutMasks = arguments.read("--without-masks");

        // whether to simulate triangle extraction (like a collision mesh would)
        s_extractTriangles = arguments.read("--extract");

        if (tilesWithMasks)
        {
            _tileFlags = TerrainEngineNode::CREATE_TILE_INCLUDE_TILES_WITH_MASKS;
        }
        if (tilesWithoutMasks)
        {
            _tileFlags = TerrainEngineNode::CREATE_TILE_INCLUDE_TILES_WITHOUT_MASKS;
        }
    }
    
    osg::Node* makeCustomTile(const TileKey& key)
    {
        Map* map = s_mapNode->getMap();

        // make a copy:
        TerrainOptions myOptions(const_cast<const MapNode*>(s_mapNode)->options());

        // disable skirts:
        myOptions.heightFieldSkirtRatio() = 0.0f;

        // disable tessellation:
        myOptions.gpuTessellation() = false;

        TerrainTileModelFactory factory(myOptions);

        osg::ref_ptr<TerrainTileModel> model =
            factory.createStandaloneTileModel(map, key, _manifest, {}, nullptr);

        osg::ref_ptr<osg::Node> node =
            s_mapNode->getTerrainEngine()->createStandaloneTile(model.get(), _tileFlags, _refLOD, key);

        if (node.valid())
        {
            if (s_extractTriangles)
            {
                OE_NOTICE << "Extracted tile " << key.str() << " (refLOD=" << _refLOD << ")" << std::endl;
                // Extract the triangles from the node that was created
                // and do our own rendering.
                // Simulates what you would do when passing in the triangles to a physics engine.
                CollectTrianglesVisitor v;
                node->accept(v);
                return v.buildNode();
            }
            else
            {
                OE_NOTICE << "Created tile " << key.str() << " (refLOD=" << _refLOD << ")" << std::endl;
                return installMultiPassRendering(node.get());
            }
        }
        else
        {
            return nullptr;
        }
    }

    void install(osg::Node* node)
    {
        if (node)
        {
            osg::Group* g = s_tile_view->getSceneData()->asGroup();
            g->removeChildren(0, g->getNumChildren());
            g->addChild(node);
            s_tile_view->getCameraManipulator()->home(0.0);
        }
    }

    void update( float x, float y, osgViewer::View* view )
    {
        // look under the mouse:
        osg::Vec3d world;
        osgUtil::LineSegmentIntersector::Intersections hits;
        TileKey key;
        GeoPoint mapPoint;

        if ( view->computeIntersections(x, y, hits) )
        {
            world = hits.begin()->getWorldIntersectPoint();

            // convert to map coords:
            mapPoint.fromWorld( s_mapNode->getMapSRS(), world );

            // Depending on the level of detail key you request, you will get a mesh that should line up exactly with the highest resolution mesh that the terrain engine will draw.
            // At level 15 that is a 257x257 heightfield.  If you select a higher lod, the mesh will be less dense.
            key = s_mapNode->getMap()->getProfile()->createTileKey(mapPoint.x(), mapPoint.y(), _tileLOD);
        }
        osg::ref_ptr<osg::Node> node;

        if (key.valid() && (node = makeCustomTile(key)))
        {
            install(node.get());
        }
        else
        {
            OE_WARN << "Failed to create tile for " << key.str() << std::endl;
        }
    }

    void update(const std::string& tileKeyString)
    {
        TileKey key = makeTileKey(tileKeyString, s_mapNode->getMap()->getProfile());
        osg::ref_ptr<osg::Node> node;
        if (key.valid() && (node = makeCustomTile(key)))
        {
            install(node.get());
            return;
        }
        OE_WARN << "Failed to create tile for " << key.str() << std::endl;
    }
    
    bool handle( const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa )
    {
        if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME && !_keyString.empty())
        {
            update(_keyString);
            _keyString.clear();
        }
        else if (ea.getEventType() == osgGA::GUIEventAdapter::PUSH
                 && ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON
                 && (ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_SHIFT))
        {
            osgViewer::View* view = static_cast<osgViewer::View*>(aa.asView());
            update( ea.getX(), ea.getY(), view );
        }
        return false;
    }

    CreateTileManifest _manifest; // Empty should be fine
    std::string _keyString;
    unsigned _tileLOD;
    unsigned _refLOD;
    TerrainEngineNode::CreateTileFlags _tileFlags;
};


int main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc,argv);
    osg::DisplaySettings::instance()->setNumMultiSamples(4u);

    osgEarth::initialize();
    
    osg::ref_ptr<CreateTileHandler> createTileHandler(new CreateTileHandler(arguments));

    osgViewer::CompositeViewer viewer(arguments);

    MapNodeHelper helper;
    auto earth = helper.load(arguments, &viewer);
    s_mapNode = MapNode::findMapNode(earth);

    osgViewer::View* main_view = new osgViewer::View();
    main_view->setUpViewInWindow(20, 20, 1600, 800);
    main_view->getCamera()->setViewport(0, 0, 800, 800);
    main_view->getCamera()->setProjectionMatrixAsPerspective(30, 1.0, 1.0, 10.0);
    main_view->setCameraManipulator(new osgEarth::Util::EarthManipulator());
    main_view->addEventHandler(createTileHandler.get());
    main_view->setSceneData(earth);
    viewer.addView(main_view);

    s_tile_view = new osgViewer::View();
    s_tile_view->getCamera()->setViewport(800, 0, 800, 800);
    s_tile_view->getCamera()->setProjectionMatrixAsPerspective(30, 1.0, 1.0, 10.0);
    s_tile_view->getCamera()->setGraphicsContext(main_view->getCamera()->getGraphicsContext());
    s_tile_view->getCamera()->setClearColor(Color::Black);
    s_tile_view->getCamera()->getOrCreateStateSet()->setMode(GL_BLEND, 1);
    s_tile_view->setCameraManipulator(new osgGA::TrackballManipulator());
    GLUtils::setLineWidth(s_tile_view->getCamera()->getOrCreateStateSet(), 2.0f, osg::StateAttribute::OVERRIDE);
    GLUtils::setLineSmooth(s_tile_view->getCamera()->getOrCreateStateSet(), osg::StateAttribute::OVERRIDE);
    GLUtils::setPointSmooth(s_tile_view->getCamera()->getOrCreateStateSet(), osg::StateAttribute::OVERRIDE);
    viewer.addView(s_tile_view);

    osg::Group* root2 = new osg::Group();
    s_tile_view->setSceneData(root2);

    helper.configureView(main_view);
    helper.configureView(s_tile_view);

    OE_NOTICE << "Shift-click to create a tile!" << std::endl;

    return viewer.run();
}
