/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "KML_GroundOverlay"
#include "KML_Geometry"
#include <osgEarth/ImageOverlay>

using namespace osgEarth_kml;
using namespace osgEarth;

void
KML_GroundOverlay::scan( xml_node<>* node, KMLContext& cx )
{
    KML_Overlay::scan( node, cx );
}

void
KML_GroundOverlay::build( xml_node<>* node, KMLContext& cx )
{
    // the URL of the overlay image
	xml_node<>* icon = node->first_node("icon", 0, false);
	std::string href;
	if (icon)
	{
		href = getValue(icon, "href");
	}

    if ( href.empty() ) {
        OE_WARN << LC << "GroundOverlay missing required Icon element" << std::endl;
        return;
    }

    ImageOverlay* im = 0L;

    // the extent of the overlay image
	xml_node<>* llb = node->first_node("latlonbox", 0, false);
    xml_node<>* llab = node->first_node("latlonaltbox", 0, false);
	xml_node<>* llq = node->first_node("gx:latlonquad", 0, false);
    if ( llb)
    {
        double north = as<double>(getValue(llb, "north"), 0.0);
        double south = as<double>(getValue(llb, "south"), 0.0);
        double east  = as<double>(getValue(llb, "east"), 0.0);;
        double west  = as<double>(getValue(llb, "west"), 0.0);;
        Angular rotation( -as<double>(getValue(llb, "rotation"), 0.0), Units::DEGREES );

        osg::ref_ptr<osg::Image> image = URI(href, cx._referrer).readImage(cx._dbOptions.get()).getImage();
		if ( !image.valid() )
        {
            OE_WARN << LC << "GroundOverlay failed to read image from " << href << std::endl;
            return;
        }

        //im = new ImageOverlay( cx._mapNode, image.get() );
        im = new ImageOverlay(nullptr, image.get());
        im->setBoundsAndRotation( Bounds(west, south, 0, east, north, 0), rotation );
        cx._groupStack.top()->addChild( im );
    }
    else if (llab)
    {
        double north = as<double>(getValue(llab, "north"), 0.0);
        double south = as<double>(getValue(llab, "south"), 0.0);
        double east = as<double>(getValue(llab, "east"), 0.0);;
        double west = as<double>(getValue(llab, "west"), 0.0);;
        Angular rotation(-as<double>(getValue(llab, "rotation"), 0.0), Units::DEGREES);

        osg::ref_ptr<osg::Image> image = URI(href, cx._referrer).readImage(cx._dbOptions.get()).getImage();
        if (!image.valid())
        {
            OE_WARN << LC << "GroundOverlay failed to read image from " << href << std::endl;
            return;
        }

        im = new ImageOverlay(nullptr, image.get());
        im->setBoundsAndRotation(Bounds(west, south, 0, east, north, 0), rotation);
        cx._groupStack.top()->addChild(im);
    }

    else if ( llq )
    {
        KML_Geometry g;
        Style style;
        g.buildChild( llq, cx, style );
        if ( g._geom.valid() && g._geom->size() >= 4 )
        {
            osg::ref_ptr<osg::Image> image = URI(href, cx._referrer).readImage(cx._dbOptions.get()).getImage();
		    if ( !image.valid() )
            {
                OE_WARN << LC << "GroundOverlay failed to read image from " << href << std::endl;
                return;
            }

            const Geometry& p = *(g._geom.get());
            im = new ImageOverlay(nullptr, image.get() );
            im->setCorners( 
                osg::Vec2d( p[0].x(), p[0].y() ),
                osg::Vec2d( p[1].x(), p[1].y() ),
                osg::Vec2d( p[3].x(), p[3].y() ),
                osg::Vec2d( p[2].x(), p[2].y() ) );
            cx._groupStack.top()->addChild( im );
        }
    }

    else {
        OE_WARN << LC << "GroundOverlay missing required LatLonBox/gx:LatLonQuad element" << std::endl;
        return;
    }


    // superclass build always called last
    KML_Overlay::build( node, cx, im );
}
