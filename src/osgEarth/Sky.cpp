/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <osgEarth/Sky>
#include <osgEarth/SkyNode2>
#include <osgEarth/MapNode>
#include <osgEarth/GLUtils>

using namespace osgEarth;

#undef  LC
#define LC "[SkyNode] "


SkyNode::SkyNode()
{
    baseInit(SkyOptions());
}

SkyNode::SkyNode(const SkyOptions& options)
{
    baseInit(options);
}

SkyNode::~SkyNode()
{
    //nop
}

void
SkyNode::baseInit(const SkyOptions& options)
{
    _ephemeris = new Ephemeris();

    setLighting( osg::StateAttribute::ON );

    if ( options.hours().isSet() )
    {
        float hours = osg::clampBetween(options.hours().get(), 0.0f, 24.0f);
        DateTime now = Registry::instance()->getDateTime();
        Registry::instance()->setDateTime(DateTime(now.year(), now.month(), now.day(), (double)hours));
    }

    this->getOrCreateStateSet()->setDefine("OE_NUM_LIGHTS", "1");
}

void
SkyNode::setEphemeris(Ephemeris* ephemeris)
{
    // cannot be null.
    _ephemeris = ephemeris ? ephemeris : new Ephemeris();
    onSetEphemeris();
}

const Ephemeris*
SkyNode::getEphemeris() const
{
    return _ephemeris.get();
}

void
SkyNode::setDateTime(const DateTime& dt)
{
    // just sets the global date time now.
    Registry::instance()->setDateTime(dt);
    //_dateTime = dt;
    //OE_INFO << LC << "Time = " << dt.asRFC1123() << std::endl;
    //onSetDateTime();
}

DateTime
SkyNode::getDateTime() const
{
    return Registry::instance()->getDateTime();
}

void
SkyNode::setReferencePoint(const GeoPoint& value)
{
    _refpoint = value;
    onSetReferencePoint();
}

void
SkyNode::setLighting(osg::StateAttribute::OverrideValue value)
{
    _lightingValue = value;

    if (value & osg::StateAttribute::INHERIT)
        GLUtils::remove(this->getStateSet(), GL_LIGHTING);
    else
        GLUtils::setLighting(this->getOrCreateStateSet(), value);
}

void
SkyNode::setSunVisible(bool value)
{
    _sunVisible = value;
    onSetSunVisible();
}

void
SkyNode::setMoonVisible(bool value)
{
    _moonVisible = value;
    onSetMoonVisible();
}

void
SkyNode::setStarsVisible(bool value)
{
    _starsVisible = value;
    onSetStarsVisible();
}

void
SkyNode::setAtmosphereVisible(bool value)
{
    _atmosphereVisible = value;
    onSetAtmosphereVisible();
}

void
SkyNode::setSimulationTimeTracksDateTime(bool value)
{
    _simTimeTracksDateTime = value;
}

bool
SkyNode::getSimulationTimeTracksDateTime() const
{
    return _simTimeTracksDateTime;
}

void
SkyNode::traverse(osg::NodeVisitor& nv)
{
    // install the date time callback (once this node is in the scene graph)
    if (!_callbackInstalled.exchange(true))
    {
        Registry::instance()->onDateTimeChanged([weak = osg::observer_ptr<SkyNode>(this)](auto dt) {
            osg::ref_ptr<SkyNode> strong;
            if (weak.lock(strong))
                strong->onSetDateTime();
        });
    }

    osg::ref_ptr<const osg::FrameStamp> fs;
    double old_simtime;

    if (_simTimeTracksDateTime &&
        nv.getVisitorType() == nv.UPDATE_VISITOR)
    {
        // simulation time tracks the sky's date/time
        fs = nv.getFrameStamp();
        if (fs.valid())
        {
            old_simtime = fs->getSimulationTime();
            const_cast<osg::FrameStamp*>(fs.get())->setSimulationTime(getDateTime().asTimeStamp());
        }
    }

    osg::Group::traverse(nv);

    if (_simTimeTracksDateTime &&
        fs.valid())
    {
        const_cast<osg::FrameStamp*>(fs.get())->setSimulationTime(old_simtime);
    }
}

//------------------------------------------------------------------------

#define SKY_OPTIONS_TAG "__osgEarth::Util::SkyOptions"

SkyNode*
SkyNode::create(const SkyOptions& options)
{
    std::string driverName = osgEarth::trim(options.getDriver());
    if (driverName == "sky2" || driverName == "2")
        return new SkyNode2(SkyNode2::Options(options));
    if ( driverName.empty() )
        driverName = "simple";

    std::string extensionName = std::string("sky_") + driverName;

    osg::ref_ptr<Extension> extension = Extension::create(extensionName, options);
    if ( !extension.valid() ) {
        OE_WARN << LC << "Failed to load extension for sky driver \"" << driverName << "\"\n";
        return 0L;
    }

    auto* factory = extension->as<Util::SkyNodeFactory>();
    if ( !factory ) {
        OE_WARN << LC << "Internal error; extension \"" << extensionName << "\" does not implement SkyNodeFactory\n";
        return 0L;
    }

    osg::ref_ptr<SkyNode> result = factory->createSkyNode();
    return result.release();
}

SkyNode*
SkyNode::create()
{
    SkyOptions options;
    return create(options);
}

SkyNode*
SkyNode::create(const std::string& driver)
{
    SkyOptions options;
    options.setDriver( driver );
    return create(options);
}


//------------------------------------------------------------------------

const SkyOptions&
Util::SkyDriver::getSkyOptions(const osgDB::Options* options) const
{
    static SkyOptions s_default;
    const void* data = options->getPluginData(SKY_OPTIONS_TAG);
    return data ? *static_cast<const SkyOptions*>(data) : s_default;
}
