/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "KMLReader"
#include "KML_Root"
#include "KML_Geometry"
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/XmlUtils>
#include <osgEarth/VirtualProgram>
#include <osgEarth/ScreenSpaceLayout>
#include <stack>
#include <iterator>

using namespace osgEarth_kml;
using namespace osgEarth;

#undef LC
#define LC "[KMLReader] "

KMLReader::KMLReader(MapNode* mapNode, const KMLOptions* options) :
    _mapNode(mapNode),
    _options(options)
{
    //nop
}

osg::Node*
KMLReader::read( std::istream& in, const osgDB::Options* dbOptions )
{
    // pull the URI context out of the DB options:
    URIContext context(dbOptions);

	// Load the XML
    osg::Timer_t start = osg::Timer::instance()->tick();
	std::stringstream buffer;
    buffer << in.rdbuf();
    std::string xmlStr;
    xmlStr = buffer.str();
	xml_document<> doc;
	doc.parse<0>(&xmlStr[0]);

	osg::Node* node = read(doc, dbOptions);

    osg::Timer_t end = osg::Timer::instance()->tick();
	OE_INFO << LC << "Loaded KML in " << osg::Timer::instance()->delta_s(start, end) << std::endl;
	node->setName( context.referrer() );

	return node;
}

osg::Node*
KMLReader::read( xml_document<>& doc, const osgDB::Options* dbOptions )
{
    osg::Group* root = new osg::Group();
    root->ref();

    URIContext context(dbOptions);

	root->setName( context.referrer() );

    KMLContext cx;
    cx._sheet     = new StyleSheet();
    cx._options   = _options;
    //cx._srs      = SpatialReference::create( "wgs84", "egm96" );
    // Use the geographic srs of the map so that clamping will occur against the correct vertical datum.
    cx._srs = _mapNode ? _mapNode->getMapSRS()->getGeographicSRS() : SpatialReference::create("wgs84");
    cx._referrer = context.referrer();
    cx._groupStack.push( root );


    // clone the dbOptions, and install a resource cache if there isn't one already:
    URIResultCache defaultUriCache;
    if ( !URIResultCache::from(dbOptions) )
    {
        osgDB::Options* newOptions = Registry::instance()->cloneOrCreateOptions(dbOptions);
        defaultUriCache.apply( newOptions );
        cx._dbOptions = newOptions;
    }
    else
    {
        cx._dbOptions = dbOptions;
    }

    // initialize the KML options with the defaults if necessary:
    KMLOptions blankOptions;
    if ( cx._options == 0L )
        cx._options = &blankOptions;

    //if ( cx._options->iconAndLabelGroup().valid() && cx._options->declutter() == true )
    //{
    //    Decluttering::setEnabled( cx._options->iconAndLabelGroup()->getOrCreateStateSet(), true );
    //}

    //const Config* top = conf.hasChild("kml" ) ? conf.child_ptr("kml") : &conf;
	xml_node<> *top = doc.first_node("kml", 0, false);

    if ( top)
    {
        KML_Root kmlRoot;

        //osg::Timer_t start = osg::Timer::instance()->tick();
        kmlRoot.scan ( top, cx );    // first pass
        //osg::Timer_t end = osg::Timer::instance()->tick();
        //OE_DEBUG << LC << "  Scan1 took " << osg::Timer::instance()->delta_s(start, end) << std::endl;

        //start = osg::Timer::instance()->tick();
        kmlRoot.scan2( top, cx );   // second pass
        //end = osg::Timer::instance()->tick();
        //OE_DEBUG << LC << "  Scan2 took " << osg::Timer::instance()->delta_s(start, end) << std::endl;

        //start = osg::Timer::instance()->tick();
        kmlRoot.build( top, cx );   // third pass.
        //end = osg::Timer::instance()->tick();
        //OE_DEBUG << LC << "  build took " << osg::Timer::instance()->delta_s(start, end) << std::endl;
    }

    //URIResultCache* cacheUsed = URIResultCache::from(cx._dbOptions.get());
    //CacheStats stats = cacheUsed->getStats();
    //OE_DEBUG << LC << "  URI Cache: " << stats._queries << " reads, " << (stats._hitRatio*100.0) << "% hits" << std::endl;

    // Make sure the KML gets rendered after the terrain.
    root->getOrCreateStateSet()->setRenderBinDetails(2, "RenderBin");

    return root;
}
