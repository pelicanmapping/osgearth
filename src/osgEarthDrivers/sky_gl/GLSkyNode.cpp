/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include "GLSkyNode"
#include <osg/LightSource>

#include <osgEarth/VirtualProgram>
#include <osgEarth/SpatialReference>
#include <osgEarth/GeoData>
#include <osgEarth/Lighting>
#include <osgEarth/PhongLightingEffect>
#include <osgEarth/Ephemeris>

#define LC "[GLSkyNode] "

using namespace osgEarth;
using namespace osgEarth::Util;
using namespace osgEarth::GLSky;

//---------------------------------------------------------------------------

GLSkyNode::GLSkyNode(const GLSkyOptions& options) :
SkyNode ( options ),
_options( options )
{
    construct();
}

void
GLSkyNode::construct()
{
    setCullingActive(false);

    _light = new LightGL3(0);
    _light->setDataVariance(_light->DYNAMIC);
    float ambient = osg::clampBetween(_options.ambient().get(), 0.0f, 1.0f);
    _light->setAmbient(osg::Vec4(ambient, ambient, ambient, 1.0f));
    _light->setDiffuse(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
    _light->setSpecular(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));

    // installs the main uniforms and the shaders that will light the subgraph (terrain).
    osg::StateSet* stateset = this->getOrCreateStateSet();

    _lighting = new PhongLightingEffect();
    _lighting->attach( stateset );

    // install the Sun as a lightsource.
    _lightSource = new osg::LightSource();
    _lightSource->setLight(_light.get());
    _lightSource->setCullingActive(false);
    _lightSource->addCullCallback(new LightSourceGL3UniformGenerator());

    // Note: DO NOT install lightsource as a child. Traverse manually.

    onSetDateTime();
}

GLSkyNode::~GLSkyNode()
{
    if ( _lighting.valid() )
        _lighting->detach();
}

void
GLSkyNode::onSetEphemeris()
{
    // trigger the date/time update.
    onSetDateTime();
}

void
GLSkyNode::onSetReferencePoint()
{
    onSetDateTime();
}

void
GLSkyNode::onSetDateTime()
{
    if ( !getSunLight() )
        return;

    CelestialBody sun = getEphemeris()->getSunPosition(getDateTime());   

    // If the user set a projected-map reference point, assume we are using
    // a projected map and set the sun position acordingly.
    if (getReferencePoint().isValid())
    {
        // pull the ref point:
        GeoPoint refpoint = getReferencePoint();

        // convert to lat/long:
        GeoPoint refLatLong;
        osg::ref_ptr<const SpatialReference> wgs84 = SpatialReference::get("wgs84");
        refpoint.transform(wgs84.get(), refLatLong);

        // Matrix to convert the ECEF sun position to the local tangent plane
        // centered on our reference point:
        osg::Matrixd world2local;
        refLatLong.createWorldToLocal(world2local);

        // convert the sun position:
        osg::Vec3d sunPosLocal = sun.geocentric * world2local;
        sunPosLocal.normalize();

        getSunLight()->setPosition( osg::Vec4(sunPosLocal, 0.0) );
    }

    else if (_options.coordinateSystem() == SkyOptions::COORDSYS_ECEF)
    {
        osg::Vec3d pos = sun.geocentric;
        pos.normalize();
        _light->setPosition(osg::Vec4(pos, 0.0)); // directional light
    }

    else if (_options.coordinateSystem() == SkyOptions::COORDSYS_ECI)
    {
        osg::Vec3d pos = sun.eci;
        pos.normalize();
        _light->setPosition(osg::Vec4(pos, 0.0)); // directional light
    }
}

void
GLSkyNode::attach( osg::View* view, int lightNum )
{
    if ( !view ) return;

    _light->setLightNum( lightNum );
    
    // install the light in the view (so other modules can access it, like shadowing)
    view->setLight(_light.get());

    // Tell the view not to automatically include a light.
    view->setLightingMode( osg::View::NO_LIGHT );

    // initial date/time setup.
    onSetDateTime();
}

void
GLSkyNode::traverse(osg::NodeVisitor& nv)
{
    _lightSource->accept(nv);
    osg::Group::traverse(nv);
}
