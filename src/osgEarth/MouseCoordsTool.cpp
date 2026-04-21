/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include <osgEarth/MouseCoordsTool>
#include <osgEarth/MapNode>
#include <osgEarth/TerrainEngineNode>

using namespace osgEarth;
using namespace osgEarth::Contrib;

//-----------------------------------------------------------------------

MouseCoordsTool::MouseCoordsTool( MapNode* mapNode, LabelControl* label, Formatter* formatter ) :
_mapNode( mapNode )
{
    if ( label )
    {
        addCallback( new MouseCoordsLabelCallback(label, formatter) );
    }
}

void
MouseCoordsTool::addCallback( MouseCoordsTool::Callback* cb )
{
    _callbacks.push_back( cb );
}

bool
MouseCoordsTool::handle( const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa )
{
    if (!_mapNode || !_mapNode->getTerrainEngine())
        return false;

    if (_mapNodePath.empty())
    {
        _mapNodePath.push_back(_mapNode->getTerrainEngine()->getNode());
    }

    if (ea.getEventType() == ea.MOVE || ea.getEventType() == ea.DRAG)
    {
        osg::Vec3d world;
        if ( _mapNode->getTerrain()->getWorldCoordsUnderMouse(aa.asView(), ea.getX(), ea.getY(), world) )
        {
            GeoPoint map;
            map.fromWorld( _mapNode->getMapSRS(), world );

            for( Callbacks::iterator i = _callbacks.begin(); i != _callbacks.end(); ++i )
                i->get()->set( map, aa.asView(), _mapNode );
        }
        else
        {
            for( Callbacks::iterator i = _callbacks.begin(); i != _callbacks.end(); ++i )
                i->get()->reset( aa.asView(), _mapNode );
        }
    }

    return false;
}

//-----------------------------------------------------------------------

MouseCoordsLabelCallback::MouseCoordsLabelCallback( LabelControl* label, Formatter* formatter ) :
_label    ( label ),
_formatter( formatter )
{
    //nop
}

void
MouseCoordsLabelCallback::set( const GeoPoint& mapCoords, osg::View* view, MapNode* mapNode )
{
    if ( _label.valid() )
    {
        osg::Vec3d eye, center, up;
        view->getCamera()->getViewMatrixAsLookAt(eye, center, up);
        osg::Vec3d world;
        mapCoords.toWorld(world);
        double range = (eye-world).length();

        if ( _formatter )
        {
            _label->setText( Stringify()
                <<  _formatter->format( mapCoords )
                << ", " << mapCoords.z() 
                << "; RNG:" << range
                << "  |  "
                << mapCoords.getSRS()->getName() );
        }
        else
        {
            _label->setText( Stringify()
                << std::fixed
                << mapCoords.x()
                << ", " << mapCoords.y()
                << ", " << mapCoords.z()
                << "; RNG:" << range
                << "  |  "
                << mapCoords.getSRS()->getName() );
        }
    }
}

void
MouseCoordsLabelCallback::reset( osg::View* view, MapNode* mapNode )
{
    if ( _label.valid() )
    {
        _label->setText( "" );
        _label->setText(Stringify() << "No data  |  " << mapNode->getMapSRS()->getName() );
    }
}

//-----------------------------------------------------------------------
