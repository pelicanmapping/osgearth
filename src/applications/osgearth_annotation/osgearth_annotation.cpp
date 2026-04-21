/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgEarth/MapNode>

#include <osgEarth/EarthManipulator>
#include <osgEarth/ExampleResources>

#include <osgEarth/ImageOverlay>
#include <osgEarth/CircleNode>
#include <osgEarth/RectangleNode>
#include <osgEarth/EllipseNode>
#include <osgEarth/PlaceNode>
#include <osgEarth/LabelNode>
#include <osgEarth/LocalGeometryNode>
#include <osgEarth/FeatureNode>
#include <osgEarth/ModelNode>
#include <osgEarth/TrackNode>

#include <osgEarth/ImageOverlayEditor>

#include <osgEarth/GeometryFactory>

#include <osgViewer/Viewer>
#include <osgDB/ReadFile>

using namespace osgEarth;
using namespace osgEarth::Util;
using namespace osgEarth::Contrib;
using namespace osgEarth::Util;

//------------------------------------------------------------------

int
usage( char** argv )
{
    OE_WARN << "Usage: " << argv[0] << " <earthfile>" << std::endl;
    return -1;
}

//------------------------------------------------------------------

int
main(int argc, char** argv)
{
    osgEarth::initialize();

    osg::Group* root = new osg::Group();

    // try to load an earth file.
    osg::ArgumentParser arguments(&argc,argv);

    osgViewer::Viewer viewer(arguments);
    viewer.setCameraManipulator( new EarthManipulator() );

    // load an earth file and parse demo arguments
    auto node = MapNodeHelper().load(arguments, &viewer);
    if ( !node )
        return usage(argv);

    root->addChild( node );

    // find the map node that we loaded.
    MapNode* mapNode = MapNode::get(node);
    if ( !mapNode )
        return usage(argv);

    // Group to hold all our annotation elements.
    osg::Group* annoGroup = new osg::Group();
    MapNode::get(node)->addChild( annoGroup );

    // Make a group for labels
    osg::Group* labelGroup = new osg::Group();
    annoGroup->addChild( labelGroup );

    osg::Group* editGroup = new osg::Group();
    MapNode::get(node)->addChild( editGroup );

    // Style our labels:
    Style labelStyle;
    labelStyle.getOrCreate<TextSymbol>()->alignment() = TextSymbol::ALIGN_CENTER_CENTER;
    labelStyle.getOrCreate<TextSymbol>()->fill().mutable_value().color() = Color::Yellow;

    // A lat/long SRS for specifying points.
    const SpatialReference* geoSRS = mapNode->getMapSRS()->getGeographicSRS();

    //--------------------------------------------------------------------

    // A series of place nodes (an icon with a text label)
    {
        Style pm;
        pm.getOrCreate<IconSymbol>()->url().mutable_value().setLiteral( "../data/placemark32.png" );
        pm.getOrCreate<IconSymbol>()->declutter() = true;
        pm.getOrCreate<TextSymbol>()->halo() = Color("#5f5f5f");

        // bunch of pins:
        labelGroup->addChild( new PlaceNode(GeoPoint(geoSRS, -74.00, 40.71), "New York"      , pm));
        labelGroup->addChild( new PlaceNode(GeoPoint(geoSRS, -77.04, 38.85), "Washington, DC", pm));
        labelGroup->addChild( new PlaceNode(GeoPoint(geoSRS,-118.40, 33.93), "Los Angeles"   , pm));
        labelGroup->addChild( new PlaceNode(GeoPoint(geoSRS, -71.03, 42.37), "Boston"        , pm));
        labelGroup->addChild( new PlaceNode(GeoPoint(geoSRS,-157.93, 21.35), "Honolulu"      , pm));
        labelGroup->addChild( new PlaceNode(GeoPoint(geoSRS, 139.75, 35.68), "Tokyo"         , pm));
        labelGroup->addChild( new PlaceNode(GeoPoint(geoSRS, -90.25, 29.98), "New Orleans"   , pm));
        labelGroup->addChild( new PlaceNode(GeoPoint(geoSRS, -80.28, 25.82), "Miami"         , pm));
        labelGroup->addChild( new PlaceNode(GeoPoint(geoSRS,-117.17, 32.72), "San Diego"     , pm));

        // test with an LOD:
        osg::LOD* lod = new osg::LOD();
        lod->addChild( new PlaceNode(GeoPoint(geoSRS, 14.68, 50.0), "Prague", pm), 0.0, 2e6);
        labelGroup->addChild( lod );

        // absolute altitude:
        labelGroup->addChild( new PlaceNode(GeoPoint(geoSRS, -87.65, 41.90, 1000, ALTMODE_ABSOLUTE), "Chicago", pm));
    }

    //--------------------------------------------------------------------

    // a box that follows lines of latitude (rhumb line interpolation, the default)
    // and flashes on and off using a cull callback.
    {
        struct C : public osg::NodeCallback {
            void operator()(osg::Node* n, osg::NodeVisitor* nv) {
                static int i=0;
                i++;
                if (i % 100 < 50)
                    traverse(n, nv);
            }
        };
        Geometry* geom = new osgEarth::Polygon();
        geom->push_back( osg::Vec3d(0,   40, 0) );
        geom->push_back( osg::Vec3d(-60, 40, 0) );
        geom->push_back( osg::Vec3d(-60, 60, 0) );
        geom->push_back( osg::Vec3d(0,   60, 0) );

        Feature* feature = new Feature(geom, geoSRS);
        feature->geoInterp() = GEOINTERP_RHUMB_LINE;

        Style geomStyle;
        geomStyle.getOrCreate<LineSymbol>()->stroke().mutable_value().color() = Color::Cyan;
        geomStyle.getOrCreate<LineSymbol>()->stroke().mutable_value().width() = Distance(5.0f, Units::PIXELS);
        geomStyle.getOrCreate<LineSymbol>()->tessellationSize() = Distance(75000, Units::METERS);
        geomStyle.getOrCreate<RenderSymbol>()->depthOffset();

        FeatureNode* fnode = new FeatureNode(feature, geomStyle);

        fnode->addCullCallback(new C());

        annoGroup->addChild( fnode );

        LabelNode* label = new LabelNode("Rhumb line polygon", labelStyle);
        label->setPosition(GeoPoint(geoSRS, -30, 50));
        labelGroup->addChild(label);
    }

    //--------------------------------------------------------------------

    // another rhumb box that crosses the antimeridian
    {
        Geometry* geom = new osgEarth::Polygon();
        geom->push_back( -160., -30. );
        geom->push_back(  150., -20. );
        geom->push_back(  160., -45. );
        geom->push_back( -150., -40. );
        Style geomStyle;

        Feature* feature = new Feature(geom, geoSRS);
        feature->geoInterp() = GEOINTERP_RHUMB_LINE;

        geomStyle.getOrCreate<LineSymbol>()->stroke()->color() = Color::Lime;
        geomStyle.getOrCreate<LineSymbol>()->stroke()->width() = Distance(3.0f, Units::PIXELS);
        geomStyle.getOrCreate<LineSymbol>()->tessellationSize() = Distance(75000, Units::METERS);
        geomStyle.getOrCreate<RenderSymbol>()->depthOffset()->range() = Distance(1.0, Units::KILOMETERS);

        FeatureNode* gnode = new FeatureNode(feature, geomStyle);
        annoGroup->addChild( gnode );

        LabelNode* label = new LabelNode("Antimeridian polygon", labelStyle);
        label->setPosition(GeoPoint(geoSRS, -175, -35));
        labelGroup->addChild(label);
    }

    //--------------------------------------------------------------------



    // A path using great-circle interpolation.
    // Keep a pointer to it so we can modify it later on.
    FeatureNode* pathNode = 0;
    {
        Geometry* path = new LineString();
        path->push_back( osg::Vec3d(-74, 40.714, 0) );   // New York
        path->push_back( osg::Vec3d(139.75, 35.68, 0) ); // Tokyo

        Feature* pathFeature = new Feature(path, geoSRS);
        pathFeature->geoInterp() = GEOINTERP_GREAT_CIRCLE;

        Style pathStyle;
        auto* line = pathStyle.getOrCreate<LineSymbol>();
        auto& stroke = line->stroke().mutable_value();
        stroke.color() = Color::White;
        stroke.width() = Distance(1.0f, Units::PIXELS);
        stroke.smooth() = true;
        line->tessellationSize() = Distance(75000, Units::METERS);
        pathStyle.getOrCreate<PointSymbol>()->size() = 8;
        pathStyle.getOrCreate<PointSymbol>()->fill()->color() = Color::Red;
        pathStyle.getOrCreate<PointSymbol>()->smooth() = true;
        pathStyle.getOrCreate<RenderSymbol>()->depthOffset()->range() = Distance(1.0, Units::KILOMETERS);

        //OE_INFO << "Path extent = " << pathFeature->getExtent().toString() << std::endl;

        pathNode = new FeatureNode(pathFeature, pathStyle);
        annoGroup->addChild( pathNode );

        LabelNode* label = new LabelNode("Great circle path", labelStyle);
        label->setPosition(GeoPoint(geoSRS,-170, 61.2));
        labelGroup->addChild(label);
    }

    //--------------------------------------------------------------------

    // Two circle segments around New Orleans.
    {
        Style circleStyle;
        circleStyle.getOrCreate<PolygonSymbol>()->fill().mutable_value().color() = Color(Color::Cyan, 0.5);
        circleStyle.getOrCreate<AltitudeSymbol>()->clamping() = AltitudeSymbol::CLAMP_TO_TERRAIN;
        circleStyle.getOrCreate<AltitudeSymbol>()->technique() = AltitudeSymbol::TECHNIQUE_DRAPE;

        CircleNode* circle = new CircleNode();
        circle->set(
            GeoPoint(geoSRS, -90.25, 29.98, 1000., ALTMODE_RELATIVE),
            Distance(300, Units::KILOMETERS),
            circleStyle,
            Angle(-45.0, Units::DEGREES),
            Angle(45.0, Units::DEGREES),
            true);

        annoGroup->addChild( circle );
    }

	{
		Style circleStyle;
		circleStyle.getOrCreate<PolygonSymbol>()->fill().mutable_value().color() = Color(Color::Red, 0.5);
		circleStyle.getOrCreate<AltitudeSymbol>()->clamping() = AltitudeSymbol::CLAMP_TO_TERRAIN;
		circleStyle.getOrCreate<AltitudeSymbol>()->technique() = AltitudeSymbol::TECHNIQUE_DRAPE;

		CircleNode* circle = new CircleNode();
        circle->set(
			GeoPoint(geoSRS, -90.25, 29.98, 1000., ALTMODE_RELATIVE),
			Distance(300, Units::KILOMETERS),
			circleStyle,
            Angle(45.0, Units::DEGREES),
            Angle(360.0 - 45.0, Units::DEGREES),
            true);

		annoGroup->addChild( circle );
	}

    //--------------------------------------------------------------------

    // An extruded ellipse around Miami.
    {
        Style ellipseStyle;
        ellipseStyle.getOrCreate<PolygonSymbol>()->fill().mutable_value().color() = Color(Color::Orange, 0.75);
        ellipseStyle.getOrCreate<ExtrusionSymbol>()->height() = Distance(250000.0, Units::METERS); // meters MSL
        EllipseNode* ellipse = new EllipseNode();
        ellipse->set(
            GeoPoint(geoSRS, -80.28, 25.82, 0.0, ALTMODE_RELATIVE),
            Distance(250, Units::MILES),
            Distance(100, Units::MILES),
            Angle   (0, Units::DEGREES),
            ellipseStyle,
            Angle(45.0, Units::DEGREES),
            Angle(360.0 - 45.0, Units::DEGREES),
            true);
        annoGroup->addChild( ellipse );
    }
	{
		Style ellipseStyle;
		ellipseStyle.getOrCreate<PolygonSymbol>()->fill().mutable_value().color() = Color(Color::Blue, 0.75);
        ellipseStyle.getOrCreate<ExtrusionSymbol>()->height() = Distance(250000.0, Units::METERS); // meters MSL
		EllipseNode* ellipse = new EllipseNode();
        ellipse->set(
			GeoPoint(geoSRS, -80.28, 25.82, 0.0, ALTMODE_RELATIVE),
			Distance(250, Units::MILES),
			Distance(100, Units::MILES),
			Angle   (0, Units::DEGREES),
			ellipseStyle,
            Angle(-40.0, Units::DEGREES),
            Angle(40.0, Units::DEGREES),
            true);
		annoGroup->addChild( ellipse );
	}

    //--------------------------------------------------------------------

    {
        // A rectangle around San Diego
        Style rectStyle;
        rectStyle.getOrCreate<PolygonSymbol>()->fill().mutable_value().color() = Color(Color::Green, 0.5);
        rectStyle.getOrCreate<AltitudeSymbol>()->clamping() = AltitudeSymbol::CLAMP_TO_TERRAIN;
        rectStyle.getOrCreate<AltitudeSymbol>()->technique() = AltitudeSymbol::TECHNIQUE_DRAPE;
        RectangleNode* rect = new RectangleNode(
            GeoPoint(geoSRS, -117.172, 32.721),
            Distance(300, Units::KILOMETERS ),
            Distance(600, Units::KILOMETERS ),
            rectStyle);
        annoGroup->addChild( rect );
    }

    //--------------------------------------------------------------------

    // An extruded polygon roughly the shape of Utah. Here we demonstrate the
    // FeatureNode, where you create a geographic geometry and use it as an
    // annotation.
    {
        Geometry* utah = new osgEarth::Polygon();
        utah->push_back( -114.052, 37.0   );
        utah->push_back( -109.054, 37.0   );
        utah->push_back( -109.054, 41.0   );
        utah->push_back( -111.040, 41.0   );
        utah->push_back( -111.080, 42.059 );
        utah->push_back( -114.080, 42.024 );

        Style utahStyle;
        utahStyle.getOrCreate<ExtrusionSymbol>()->height() = Distance(250000.0, Units::METERS); // meters MSL
        utahStyle.getOrCreate<PolygonSymbol>()->fill().mutable_value().color() = Color(Color::White, 0.8);

        Feature*     utahFeature = new Feature(utah, geoSRS);
        FeatureNode* featureNode = new FeatureNode(utahFeature, utahStyle);

        annoGroup->addChild( featureNode );
    }

    //--------------------------------------------------------------------

    // an image overlay.
    {
        ImageOverlay* imageOverlay = 0L;
        osg::ref_ptr<osg::Image> image = osgDB::readRefImageFile( "../data/USFLAG.TGA" );
        if (image.valid())
        {
            imageOverlay = new ImageOverlay(mapNode, image.get());
            imageOverlay->setBounds(Bounds(-100.0, 35.0, 0.0, -90.0, 40.0, 0.0));
            annoGroup->addChild( imageOverlay );

            editGroup->addChild( new ImageOverlayEditor(imageOverlay) );
        }
    }

    //--------------------------------------------------------------------

    // a model node with auto scaling.
    {
        Style style;
        style.getOrCreate<ModelSymbol>()->autoScale() = true;
        style.getOrCreate<ModelSymbol>()->url().mutable_value().setLiteral("../data/red_flag.osg.50.scale");
        ModelNode* modelNode = new ModelNode(mapNode, style);
        modelNode->setPosition(GeoPoint(geoSRS, -100, 52));
        annoGroup->addChild(modelNode);
    }

    //--------------------------------------------------------------------

    // a track node
    {

        // A TrackNode
        auto trackImage = osgDB::readRefImageFile("../data/icon.png");
        if (trackImage.valid())
        {
            GeoPoint trackPos(SpatialReference::get("wgs84"), -55, 22, 0, ALTMODE_ABSOLUTE);
            auto nameSymbol = new TextSymbol();
            nameSymbol->pixelOffset() = osg::Vec2s(0, -trackImage->t() / 3);
            nameSymbol->alignment() = TextSymbol::ALIGN_CENTER_BOTTOM;
            nameSymbol->halo() = Color::Black;
            std::string labelFieldName = "name";
            nameSymbol->content()->setLiteral({ "Hello, TrackNode" });
            nameSymbol->size() = 18.0f;
            TrackNodeFieldSchema trackSchema;
            trackSchema["name"] = { nameSymbol, true };
            auto track = new TrackNode(trackPos, trackImage.get(), trackSchema);

            track->setIconRotation(Angle(45.0, Units::DEGREES));
            annoGroup->addChild(track);
        }
    }

    //--------------------------------------------------------------------

    // initialize the viewer:
    viewer.setSceneData( root );
    return viewer.run();
}
