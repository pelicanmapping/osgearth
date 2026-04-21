/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osg/Notify>
#include <osgGA/StateSetManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <osgDB/WriteFile>

#include <osgEarth/MapNode>
#include <osgEarth/GDAL>
#include <osgEarth/ExampleResources>
#include <osgEarth/EarthManipulator>

#include <osgEarth/Style>
#include <osgEarth/OGRFeatureSource>
#include <osgEarth/TiledFeatureModelLayer>
#include <osgEarth/FeatureImageLayer>

using namespace osgEarth;
using namespace osgEarth::Util;

int usage( const std::string& app )
{
    OE_NOTICE << "\n" << app << "\n"
        << "  --rasterize           : draw features as rasterized image tiles \n"
        << "  --drape               : draw features as projected texture \n"
        << "  --clamp               : draw features using shader clamping \n"
        << "  --mem                 : load features from memory \n"
        << "  --labels              : add feature labels \n"
        << "  --out <earthfile>     : test writing to an earth file \n"
        << "\n"
        << MapNodeHelper().usage()
        << std::endl;

    return 0;
}

// inline CSS, for use with the --script option
const char* styles_css =
R"(
    p {
        altitude-clamping: terrain-drape;
        render-backface-culling: false;
    }
    p1: p{ fill: #ff3f3f9f; }
    p2: p{ fill: #3fff3f9f; }
    p3: p{ fill: #3f3fff9f; }
    p4: p{ fill: #ff3fff9f; }
    p5: p{ fill: #ffff3f9f; }
)";

// JavaScript style selector, for use with the --script option
const char* script_source =
R"(
    function getStyleClass()
    {
        // Exclude any countries beginning with the letter A: 
        if ( feature.properties.name.charAt(0) === 'A' )
            return null;
                        
        // If it starts with the letter C, return an inline style:
        if ( feature.properties.name.charAt(0) == 'C' )
            return '{ _fill: #ffc838; stroke: #8f8838; extrusion-height: 250000; }';
                        
        // Otherwise, return a named style based on some calculations:
        var pop = parseFloat(feature.properties.pop);
        if      ( pop <= 14045470 )  return "p1";
        else if ( pop <= 43410900 )  return "p2";
        else if ( pop <= 97228750 )  return "p3";
        else if ( pop <= 258833000 ) return "p4";
        else                         return "p5";
    }
)";

//
// NOTE: run this sample from the repo/tests directory.
//
int main(int argc, char** argv)
{
    osgEarth::initialize();

    osg::ArgumentParser arguments(&argc,argv);

    if ( arguments.read("--help") )
        return usage( argv[0] );

    bool useRaster  = arguments.read("--rasterize");
    bool useMem     = arguments.read("--mem");
    bool useLabels  = arguments.read("--labels");
    bool useDraping = arguments.read("--drape");
    bool useClamping = arguments.read("--clamp");
    bool useScript = arguments.read("--script");

    std::string outfile;
    arguments.read("--out", outfile);

    osgViewer::Viewer viewer(arguments);

    // Start by creating the map:
    osg::ref_ptr<Map> map = new Map();

    // Start with a basemap imagery layer; we'll be using the GDAL driver
    // to load a local GeoTIFF file:
    GDALImageLayer* basemap = new GDALImageLayer();
    basemap->setURL("../data/world.tif");
    map->addLayer(basemap);

    // Next we add a layer to provide the feature data.
    OGRFeatureSource* features = new OGRFeatureSource();
    features->setName("vector-data");

    if (useMem)
    {
        // the --mem options tells us to just make an in-memory geometry:
        Ring* line = new Ring();
        line->push_back( osg::Vec3d(-60, 20, 0) );
        line->push_back( osg::Vec3d(-120, 20, 0) );
        line->push_back( osg::Vec3d(-120, 60, 0) );
        line->push_back( osg::Vec3d(-60, 60, 0) );
        features->setGeometry(line);
    }
    else
    {
        features->setURL("../data/world.shp");
    }
    map->addLayer(features);

    // Define a style for the feature data. Since we are going to render the
    // vectors as lines, configure the line symbolizer:
    Style style;

    LineSymbol* ls = style.getOrCreateSymbol<LineSymbol>();
    ls->stroke().mutable_value().color() = Color::Yellow;
    ls->stroke().mutable_value().width() = Distance(2.0f, Units::PIXELS);
    ls->tessellationSize() = Distance(100, Units::KILOMETERS);

    if (useDraping)
    {
        AltitudeSymbol* alt = style.getOrCreate<AltitudeSymbol>();
        alt->clamping() = alt->CLAMP_TO_TERRAIN;
        alt->technique() = alt->TECHNIQUE_DRAPE;
    }

    else if (useClamping)
    {
        AltitudeSymbol* alt = style.getOrCreate<AltitudeSymbol>();
        alt->clamping() = alt->CLAMP_TO_TERRAIN;
        alt->technique() = alt->TECHNIQUE_GPU;

        ls->tessellationSize()  = Distance(100, Units::KILOMETERS);

        RenderSymbol* render = style.getOrCreate<RenderSymbol>();
        render->depthOffset().mutable_value().automatic() = true;
    }

    if (useRaster)
    {
        FeatureImageLayer* layer = new FeatureImageLayer();
        layer->setFeatureSource(features);
        StyleSheet* sheet = new StyleSheet();
        sheet->addStyle(style);
        layer->setStyleSheet(sheet);
        map->addLayer(layer);
    }

    else
    {
        auto* layer = new TiledFeatureModelLayer();
        layer->setFeatureSource(features);

        StyleSheet* styleSheet = new StyleSheet();

        if (useScript)
        {
            styleSheet->addStylesFromCSS(styles_css);
            styleSheet->setScript(new StyleSheet::ScriptDef(script_source));
            styleSheet->addSelector(StyleSelector("default", StringExpression("getStyleClass()")));
        }
        else
        {
            styleSheet->addStyle(style);
        }

        layer->setStyleSheet(styleSheet);
        map->addLayer(layer);
    }

    if ( useLabels && !useRaster )
    {
        // set up symbology for drawing labels. We're pulling the label
        // text from the name attribute, and its draw priority from the
        // population attribute.
        Style labelStyle;

        TextSymbol* text = labelStyle.getOrCreateSymbol<TextSymbol>();
        text->content() = Expression<String>("feature.properties.name");
        text->priority() = Expression<float>("feature.properties.pop");
        text->size() = 16.0f;
        text->alignment() = TextSymbol::ALIGN_CENTER_CENTER;
        text->fill().mutable_value().color() = Color::White;
        text->halo().mutable_value().color() = Color::DarkGray;

        StyleSheet* sheet = new StyleSheet();
        sheet->addStyle(labelStyle);

        // and configure a model layer:
        auto* fml = new TiledFeatureModelLayer();
        fml->setName("Labels");
        fml->setFeatureSource(features);
        fml->setStyleSheet(sheet);
        map->addLayer(fml);
    }

    LayerVector layers;
    map->getLayers(layers);
    for(LayerVector::const_iterator i = layers.begin(); i != layers.end(); ++i)
    {
        Layer* layer = i->get();
        if (layer->getOpenAutomatically() && !layer->isOpen())
        {
            OE_WARN << layer->getName() << " : " << layer->getStatus().toString() << std::endl;
        }
    }

    // That's it, the map is ready; now create a MapNode to render the Map:
    MapNode* mapNode = new MapNode(map.get());

    if (!outfile.empty())
    {
        OE_NOTICE << "Writing to " << outfile << std::endl;
        osgDB::writeNodeFile(*mapNode, outfile);
    }
    else
    {
        viewer.setSceneData( mapNode );
        viewer.setCameraManipulator( new EarthManipulator() );

        // add some stock OSG handlers:
        MapNodeHelper().configureView(&viewer);

        return viewer.run();
    }
}
