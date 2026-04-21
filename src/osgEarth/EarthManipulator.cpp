/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/EarthManipulator>
#include <osgEarth/GeoMath>
#include <osgEarth/TerrainEngineNode>
#include <osgViewer/View>

#define LC "[EarthManip] "

using namespace osgEarth;
using namespace osgEarth::Util;

namespace
{
    // a reasonable approximation of cosine interpolation
    double
    smoothStepInterp( double t ) {
        return (t*t)*(3.0-2.0*t);
    }

    // rough approximation of pow(x,y)
    double
    powFast( double x, double y ) {
        return x/(x+y-y*x);
    }

    // accel/decel curve (a < 0 => decel)
    double
    accelerationInterp( double t, double a ) {
        return a == 0.0? t : a > 0.0? powFast( t, a ) : 1.0 - powFast(1.0-t, -a);
    }

    // normalized linear intep
    osg::Vec3d nlerp(const osg::Vec3d& a, const osg::Vec3d& b, double t) {
        double am = a.length(), bm = b.length();
        osg::Vec3d c = a*(1.0-t) + b*t;
        c.normalize();
        c *= (1.0-t)*am + t*bm;
        return c;
    }

    // linear interp
    osg::Vec3d lerp(const osg::Vec3d& a, const osg::Vec3d& b, double t) {
        return a*(1.0-t) + b*t;
    }

    osg::Matrix computeLocalToWorld(osg::Node* node) {
        osg::Matrix m;
        if ( node ) {
            osg::NodePathList nodePaths = node->getParentalNodePaths();
            if ( nodePaths.size() > 0 ) {
                osg::NodePath p;
                unsigned start = 0;
                for(unsigned i=0; i<nodePaths[0].size(); ++i) {
                    if (dynamic_cast<MapNode*>(nodePaths[0][i]) != 0L) {
                        start = i;
                        break;
                    }
                }
                for(unsigned i=start; i<nodePaths[0].size(); ++i)
                    p.push_back(nodePaths[0][i]);

                m = osg::computeLocalToWorld(p);
                //m = osg::computeLocalToWorld( nodePaths[0] );
            }
            else {
                osg::Transform* t = dynamic_cast<osg::Transform*>(node);
                if ( t ) {
                    t->computeLocalToWorldMatrix( m, 0L );
                }
            }
        }
        return m;
    }

    osg::Vec3d computeWorld(osg::Node* node) {
        return node ? osg::Vec3d(0,0,0) * computeLocalToWorld(node) : osg::Vec3d(0,0,0);
    }

    double normalizeAzimRad( double input )
    {
        if(fabs(input) > 2*osg::PI)
            input = fmod(input,2*osg::PI);
        if( input < -osg::PI ) input += osg::PI*2.0;
        if( input > osg::PI ) input -= osg::PI*2.0;
        return input;
    }

    // This replaces OSG's osg::computeLocalToWorld() function with one that
    // passes your own NodeVisitor to the Transform::computeLocalToWorldMatrix()
    // method. (We cannot subclass OSG's visitor because it's private.) This
    // exists for users that have custom Transform subclasses that override
    // Transform::computeLocalToWorldMatrix and need access to the NodeVisitor.
    struct ComputeLocalToWorld : osg::NodeVisitor
    {
        osg::Matrix _matrix;
        osg::NodeVisitor* _nv;
        ComputeLocalToWorld(osg::NodeVisitor* nv) : _nv(nv) { }
        void accumulate(const osg::NodePath& path)
        {
            if (path.empty()) return;
            unsigned j = path.size();
            for (osg::NodePath::const_reverse_iterator i = path.rbegin();
                i != path.rend();
                ++i, --j)
            {
                const osg::Camera* cam = dynamic_cast<const osg::Camera*>(*i);
                if (cam)
                {
                    if( cam->getReferenceFrame() != osg::Transform::RELATIVE_RF || cam->getParents().empty())
                        break;
                }
                else if (dynamic_cast<MapNode*>(*i))
                {
                    break;
                }
            }
            for (; j<path.size(); ++j)
            {
                const_cast<osg::Node*>(path[j])->accept(*this);
            }
        }
        void apply(osg::Transform& transform)
        {
            transform.computeLocalToWorldMatrix(_matrix, _nv);
        }
    };
}


//------------------------------------------------------------------------
namespace
{
    // Callback that notifies the manipulator whenever the terrain changes
    // around its center point.
    struct ManipTerrainCallback : public TerrainCallback
    {
        ManipTerrainCallback(EarthManipulator* manip) : _manip(manip) { }
        void onTileUpdate(const TileKey& key, osg::Node* graph, TerrainCallbackContext& context)
        {
            osg::ref_ptr<EarthManipulator> safe;
            if ( _manip.lock(safe) )
            {
                safe->handleTileUpdate(key, graph, context);
            }
        }
        osg::observer_ptr<EarthManipulator> _manip;
    };
}



EarthManipulator::Action::Action( ActionType type, const ActionOptions& options ) :
_type( type ),
_options( options )
{
    init();
}

EarthManipulator::Action::Action( ActionType type ) :
_type( type )
{
    init();
}

void
EarthManipulator::Action::init()
{
    _dir =
        _type == ACTION_PAN_LEFT  || _type == ACTION_ROTATE_LEFT? DIR_LEFT :
        _type == ACTION_PAN_RIGHT || _type == ACTION_ROTATE_RIGHT? DIR_RIGHT :
        _type == ACTION_PAN_UP    || _type == ACTION_ROTATE_UP   || _type == ACTION_ZOOM_IN ? DIR_UP :
        _type == ACTION_PAN_DOWN  || _type == ACTION_ROTATE_DOWN || _type == ACTION_ZOOM_OUT ? DIR_DOWN :
        DIR_NA;
}

EarthManipulator::Action::Action( const Action& rhs ) :
_type( rhs._type ),
_dir( rhs._dir ),
_options( rhs._options )
{
    //nop
}

bool
EarthManipulator::Action::getBoolOption( int option, bool defaultValue ) const
{
    for(ActionOptions::const_iterator i = _options.begin(); i != _options.end(); i++ ) {
        if ( i->option() == option )
            return i->boolValue();
    }
    return defaultValue;
}

int
EarthManipulator::Action::getIntOption( int option, int defaultValue ) const
{
    for(ActionOptions::const_iterator i = _options.begin(); i != _options.end(); i++ ) {
        if ( i->option() == option )
            return i->intValue();
    }
    return defaultValue;
}

double
EarthManipulator::Action::getDoubleOption( int option, double defaultValue ) const
{
    for(ActionOptions::const_iterator i = _options.begin(); i != _options.end(); i++ ) {
        if ( i->option() == option )
            return i->doubleValue();
    }
    return defaultValue;
}

/****************************************************************************/

EarthManipulator::Action EarthManipulator::NullAction( EarthManipulator::ACTION_NULL );

namespace
{
    static std::string s_actionNames[] = {
        "null",
        "home",
        "goto",
        "pan",
        "pan-left",
        "pan-right",
        "pan-up",
        "pan-down",
        "rotate",
        "rotate-left",
        "rotate-right",
        "rotate-up",
        "rotate-down",
        "zoom",
        "zoom-in",
        "zoom-out",
        "earth-drag"
    };

    static std::string s_actionOptionNames[] = {
        "scale-x",
        "scale-y",
        "continuous",
        "single-axis",
        "goto-range-factor",
        "duration"
    };

    static short s_actionOptionTypes[] = { 1, 1, 0, 0, 1, 1 }; // 0=bool, 1=double
}

//------------------------------------------------------------------------

EarthManipulator::Settings::Settings() :
osg::Referenced                 (),
Revisioned                      (),
_single_axis_rotation           ( false ),
_lock_azim_while_panning        ( true ),
_mouse_sens                     ( 1.0 ),
_touch_sens                     ( 0.005 ),
_keyboard_sens                  ( 1.0 ),
_scroll_sens                    ( 1.0 ),
_min_pitch                      ( -89.9 ),
_max_pitch                      ( -1.0 ),
_max_x_offset                   ( 0.0 ),
_max_y_offset                   ( 0.0 ),
_min_distance                   ( 1.0 ),
_max_distance                   ( DBL_MAX ),
_tether_mode                    ( TETHER_CENTER ),
_arc_viewpoints                 ( true ),
_auto_vp_duration               ( true ),
_min_vp_duration_s              ( 1.0 ),
_max_vp_duration_s              ( 3.0 ),
_orthoTracksPerspective         ( true ),
_terrainAvoidanceEnabled        ( true ),
_terrainAvoidanceMinDistance    ( 1.0 ),
_throwingEnabled                ( false ),
_throwDecayRate                 ( 0.175 ),
_zoomToMouse                    ( true )
{
    //NOP
}

EarthManipulator::Settings::Settings( const EarthManipulator::Settings& rhs ) :
osg::Referenced( rhs ),
Revisioned     ( rhs ),
_bindings( rhs._bindings ),
_single_axis_rotation( rhs._single_axis_rotation ),
_lock_azim_while_panning( rhs._lock_azim_while_panning ),
_mouse_sens( rhs._mouse_sens ),
_touch_sens( rhs._touch_sens),
_keyboard_sens( rhs._keyboard_sens ),
_scroll_sens( rhs._scroll_sens ),
_min_pitch( rhs._min_pitch ),
_max_pitch( rhs._max_pitch ),
_max_x_offset( rhs._max_x_offset ),
_max_y_offset( rhs._max_y_offset ),
_min_distance( rhs._min_distance ),
_max_distance( rhs._max_distance ),
_tether_mode( rhs._tether_mode ),
_arc_viewpoints( rhs._arc_viewpoints ),
_auto_vp_duration( rhs._auto_vp_duration ),
_min_vp_duration_s( rhs._min_vp_duration_s ),
_max_vp_duration_s( rhs._max_vp_duration_s ),
_orthoTracksPerspective( rhs._orthoTracksPerspective ),
_breakTetherActions( rhs._breakTetherActions ),
_terrainAvoidanceEnabled( rhs._terrainAvoidanceEnabled ),
_terrainAvoidanceMinDistance( rhs._terrainAvoidanceMinDistance ),
_throwingEnabled( rhs._throwingEnabled ),
_throwDecayRate( rhs._throwDecayRate ),
_zoomToMouse( rhs._zoomToMouse )
{
    //NOP
}

void
EarthManipulator::Settings::apply(osg::ArgumentParser& args)
{
    bool   boolval;
    double doubleval;

    if ( args.read("--manip-terrain-avoidance", boolval) )
        setTerrainAvoidanceEnabled( boolval );
    if ( args.read("--manip-terrain-avoidance-min-distance", doubleval) )
        setTerrainAvoidanceMinimumDistance( doubleval );
    if ( args.read("--manip-min-distance", doubleval) )
        setMinMaxDistance(doubleval, _max_distance);
    if ( args.read("--manip-max-distance", doubleval) )
        setMinMaxDistance(_min_distance, doubleval);
    if ( args.read("--manip-min-pitch", doubleval) )
        setMinMaxPitch(doubleval, _max_pitch);
    if ( args.read("--manip-max-pitch", doubleval) )
        setMinMaxPitch(_min_pitch, doubleval);
}

#define HASMODKEY( W, V ) (( W & V ) == V )

// expands one input spec into many if necessary, to deal with modifier key combos.
void
EarthManipulator::Settings::expandSpec( const InputSpec& input, InputSpecs& output ) const
{
    int e = input._event_type;
    int i = input._input_mask;
    int m = input._modkey_mask;

    if ( HASMODKEY(m, osgGA::GUIEventAdapter::MODKEY_CTRL) )
    {
        expandSpec( InputSpec( e, i, m & ~osgGA::GUIEventAdapter::MODKEY_LEFT_CTRL ), output );
        expandSpec( InputSpec( e, i, m & ~osgGA::GUIEventAdapter::MODKEY_RIGHT_CTRL ), output );
    }
    else if ( HASMODKEY(m, osgGA::GUIEventAdapter::MODKEY_ALT) )
    {
        expandSpec( InputSpec( e, i, m & ~osgGA::GUIEventAdapter::MODKEY_LEFT_ALT ), output );
        expandSpec( InputSpec( e, i, m & ~osgGA::GUIEventAdapter::MODKEY_RIGHT_ALT ), output );
    }
    else if ( HASMODKEY(m, osgGA::GUIEventAdapter::MODKEY_SHIFT) )
    {
        expandSpec( InputSpec( e, i, m & ~osgGA::GUIEventAdapter::MODKEY_LEFT_SHIFT ), output );
        expandSpec( InputSpec( e, i, m & ~osgGA::GUIEventAdapter::MODKEY_RIGHT_SHIFT ), output );
    }
    else if ( HASMODKEY(m, osgGA::GUIEventAdapter::MODKEY_META) )
    {
        expandSpec( InputSpec( e, i, m & ~osgGA::GUIEventAdapter::MODKEY_LEFT_META ), output );
        expandSpec( InputSpec( e, i, m & ~osgGA::GUIEventAdapter::MODKEY_RIGHT_META ), output );
    }
    else if ( HASMODKEY(m, osgGA::GUIEventAdapter::MODKEY_HYPER) )
    {
        expandSpec( InputSpec( e, i, m & ~osgGA::GUIEventAdapter::MODKEY_LEFT_HYPER ), output );
        expandSpec( InputSpec( e, i, m & ~osgGA::GUIEventAdapter::MODKEY_RIGHT_HYPER ), output );
    }

    //Always add the input so if we are dealing with a windowing system like QT that just sends MODKEY_CTRL it will still work.
    output.push_back( input );
}

void
EarthManipulator::Settings::bind( const InputSpec& spec, const Action& action )
{
    InputSpecs specs;
    expandSpec( spec, specs );
    for( InputSpecs::const_iterator i = specs.begin(); i != specs.end(); i++ )
    {
        _bindings[*i] = action;
    }
}

void
EarthManipulator::Settings::bindMouse(ActionType actionType,
                                      int button_mask, int modkey_mask,
                                      const ActionOptions& options)
{
    bind(
        InputSpec( osgGA::GUIEventAdapter::DRAG, button_mask, modkey_mask ),
        Action( actionType, options ) );
}

void
EarthManipulator::Settings::bindMouseClick(ActionType action,
                                           int button_mask, int modkey_mask,
                                           const ActionOptions& options)
{
    bind(
        InputSpec( EVENT_MOUSE_CLICK, button_mask, modkey_mask ),
        Action( action, options ) );
}

void
EarthManipulator::Settings::bindMouseDoubleClick(ActionType action,
                                                 int button_mask, int modkey_mask,
                                                 const ActionOptions& options)
{
    bind(
        InputSpec( EVENT_MOUSE_DOUBLE_CLICK, button_mask, modkey_mask ),
        Action( action, options ) );
}

void
EarthManipulator::Settings::bindKey(ActionType action,
                                    int key, int modkey_mask,
                                    const ActionOptions& options)
{
    bind(
        InputSpec( osgGA::GUIEventAdapter::KEYDOWN, key, modkey_mask ),
        Action( action, options ) );
}

void
EarthManipulator::Settings::bindScroll(ActionType action, int scrolling_motion,
                                       int modkey_mask, const ActionOptions& options )
{
    bind(
        InputSpec ( osgGA::GUIEventAdapter::SCROLL, scrolling_motion, modkey_mask ),
        Action( action, options ) );
}


void
EarthManipulator::Settings::bindPinch(ActionType action, const ActionOptions& options)
{
    bind(
        InputSpec( EarthManipulator::EVENT_MULTI_PINCH, 0, 0 ),
        Action( action, options ) );
}

void
EarthManipulator::Settings::bindTwist(ActionType action, const ActionOptions& options)
{
    bind(
         InputSpec( EarthManipulator::EVENT_MULTI_TWIST, 0, 0 ),
         Action( action, options ) );
}

void
EarthManipulator::Settings::bindMultiDrag(ActionType action, const ActionOptions& options)
{
    bind(
        InputSpec( EarthManipulator::EVENT_MULTI_DRAG, 0, 0 ),
        Action( action, options ) );
}

void
EarthManipulator::Settings::bindTouchDrag(ActionType action, const ActionOptions& options)
{
    bind(
        InputSpec(EarthManipulator::EVENT_TOUCH_DRAG, 0, 0),
        Action(action, options));
}

const EarthManipulator::Action&
EarthManipulator::Settings::getAction(int event_type, int input_mask, int modkey_mask) const
{
    //Build the input spec but remove the numlock and caps lock from the modkey mask.  On Linux these seem to be passed in as part of the modkeymask
    //if they are on.  So if you bind an action like SCROLL to a modkey mask of 0 or a modkey mask of ctrl it will never match the spec exactly b/c
    //the modkey mask also includes capslock and numlock.
    InputSpec spec( event_type, input_mask, modkey_mask & ~osgGA::GUIEventAdapter::MODKEY_NUM_LOCK & ~osgGA::GUIEventAdapter::MODKEY_CAPS_LOCK);
    ActionBindings::const_iterator i = _bindings.find(spec);
    return i != _bindings.end() ? i->second : NullAction;
}

void
EarthManipulator::Settings::setMinMaxPitch( double min_pitch, double max_pitch )
{
    _min_pitch = osg::clampBetween( min_pitch, -89.9, 89.0 );
    _max_pitch = osg::clampBetween( max_pitch, min_pitch, 89.0 );
    dirty();
}

void
EarthManipulator::Settings::setMaxOffset(double max_x_offset, double max_y_offset)
{
    _max_x_offset = osg::clampAbove(max_x_offset, 0.0);
    _max_y_offset = osg::clampAbove(max_y_offset, 0.0);
    dirty();
}

void
EarthManipulator::Settings::setMinMaxDistance( double min_distance, double max_distance)
{
    _min_distance = min_distance;
    _max_distance = max_distance;
    dirty();
}

void
EarthManipulator::Settings::setArcViewpointTransitions( bool value )
{
    _arc_viewpoints = value;
    dirty();
}

void
EarthManipulator::Settings::setAutoViewpointDurationEnabled( bool value )
{
    _auto_vp_duration = value;
    dirty();
}

void
EarthManipulator::Settings::setAutoViewpointDurationLimits( double minSeconds, double maxSeconds )
{
    _min_vp_duration_s = std::max( minSeconds, 0.0 );
    _max_vp_duration_s = std::max( maxSeconds, _min_vp_duration_s );
    dirty();
}

/************************************************************************/

void
EarthManipulator::ctor_init()
{
    _last_action = ACTION_NULL;
    _last_event = EVENT_MOUSE_DOUBLE_CLICK;
    _time_s_last_event = 0.0;
    _frameCount = 0;
    _findNodeTraversalMask = 0x01;
    _time_s_now = 0.0;
    _centerHeight = 0.0;
    _time_last_frame = 0.0;
    _continuous_dx = 0;
    _continuous_dy = 0;
    _last_continuous_action_time = 0.0;
    _single_axis_x = 0;
    _single_axis_y = 0;
    _setVPAccel = 0;
    _setVPAccel2 = 0;
    _lastTetherMode = TETHER_CENTER;
    _homeViewpointDuration = 0;
    _lastKnownVFOV = 30.0;
    _userWillCallUpdateCamera = false;
}

EarthManipulator::EarthManipulator() :
osgGA::CameraManipulator()
{
    ctor_init();
    reinitialize();
    configureDefaultSettings();
    if (_settings.valid())
        _lastTetherMode = _settings->getTetherMode();
}

EarthManipulator::EarthManipulator(osg::ArgumentParser& args) :
osgGA::CameraManipulator()
{
    ctor_init();
    reinitialize();
    configureDefaultSettings();
    if (_settings.valid())
        _lastTetherMode = _settings->getTetherMode();

    getSettings()->apply( args );
}

EarthManipulator::EarthManipulator( const EarthManipulator& rhs ) :
osgGA::CameraManipulator( rhs ),
_last_action            ( ACTION_NULL ),
_last_event             ( EVENT_MOUSE_DOUBLE_CLICK ),
_time_s_last_event      ( 0.0 ),
_frameCount             ( 0 ),
_settings               ( new Settings(*rhs.getSettings()) ),
_findNodeTraversalMask  ( rhs._findNodeTraversalMask ),
_time_s_now             ( rhs._time_s_now ),
_centerHeight           ( rhs._centerHeight),
_time_last_frame        ( 0.0 ),
_continuous_dx          ( rhs._continuous_dx ),
_continuous_dy          ( rhs._continuous_dy ),
_last_continuous_action_time ( rhs._last_continuous_action_time ),
_single_axis_x          (rhs._single_axis_x),
_single_axis_y          (rhs._single_axis_y)
{
    reinitialize();
}


EarthManipulator::~EarthManipulator()
{
    osg::ref_ptr<MapNode> mapNode = _mapNode;
    if (mapNode.valid() && _terrainCallback && mapNode->getTerrain())
    {
        mapNode->getTerrain()->removeTerrainCallback( _terrainCallback.get() );
    }
}

void
EarthManipulator::configureDefaultSettings()
{
    _settings = new Settings();

    // install default action bindings:
    ActionOptions options;

    _settings->bindKey( ACTION_HOME, osgGA::GUIEventAdapter::KEY_Space );

    options.clear();
    options.add( OPTION_CONTINUOUS, true );
    options.add(OPTION_SCALE_Y, 5.0);

    // zoom as you hold the right button:
    _settings->bindMouse( ACTION_ZOOM, osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON, 0L, options );
    _settings->bindMouse( ACTION_ZOOM, osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON, osgGA::GUIEventAdapter::MODKEY_CTRL, options);

    options.add( OPTION_SCALE_X, 9.0 );
    options.add( OPTION_SCALE_Y, 9.0 );

    _settings->bindMouse( ACTION_PAN, osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON );
    _settings->bindMouse( ACTION_PAN, osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON, osgGA::GUIEventAdapter::MODKEY_CTRL, options);

    // rotate with either the middle button or the left+right buttons:
    _settings->bindMouse( ACTION_ROTATE, osgGA::GUIEventAdapter::MIDDLE_MOUSE_BUTTON );
    _settings->bindMouse( ACTION_ROTATE, osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON | osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON );
    _settings->bindMouse( ACTION_ROTATE, osgGA::GUIEventAdapter::MIDDLE_MOUSE_BUTTON, osgGA::GUIEventAdapter::MODKEY_CTRL, options);
    _settings->bindMouse( ACTION_ROTATE, osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON | osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON, osgGA::GUIEventAdapter::MODKEY_CTRL, options);

    options.add( OPTION_SCALE_X, 4.0 );
    options.add( OPTION_SCALE_Y, 4.0 );

    // zoom with the scroll wheel:
    _settings->bindScroll( ACTION_ZOOM_IN,  osgGA::GUIEventAdapter::SCROLL_DOWN );
    _settings->bindScroll( ACTION_ZOOM_OUT, osgGA::GUIEventAdapter::SCROLL_UP );

    // pan around with arrow keys:
    _settings->bindKey( ACTION_PAN_LEFT,  osgGA::GUIEventAdapter::KEY_Left );
    _settings->bindKey( ACTION_PAN_RIGHT, osgGA::GUIEventAdapter::KEY_Right );
    _settings->bindKey( ACTION_PAN_UP,    osgGA::GUIEventAdapter::KEY_Up );
    _settings->bindKey( ACTION_PAN_DOWN,  osgGA::GUIEventAdapter::KEY_Down );

    // double click the left button to zoom in on a point:
    options.clear();
    options.add( OPTION_GOTO_RANGE_FACTOR, 0.4 );
    _settings->bindMouseDoubleClick( ACTION_GOTO, osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON, 0L, options );

    // double click the right button (or CTRL-left button) to zoom out to a point
    options.clear();
    options.add( OPTION_GOTO_RANGE_FACTOR, 2.5 );
    _settings->bindMouseDoubleClick( ACTION_GOTO, osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON, 0L, options );
    _settings->bindMouseDoubleClick( ACTION_GOTO, osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON, osgGA::GUIEventAdapter::MODKEY_CTRL, options );

    // map multi-touch pinch to a discrete zoom
    options.clear();
    _settings->bindPinch( ACTION_ZOOM, options );

    options.clear();
    _settings->bindTwist( ACTION_ROTATE, options );
    _settings->bindMultiDrag( ACTION_ROTATE, options );
    _settings->bindTouchDrag( ACTION_PAN, options );

    _settings->setLockAzimuthWhilePanning( true );
}

void
EarthManipulator::applySettings( Settings* settings )
{
    if ( settings )
    {
        _settings = settings;
    }
    else
    {
        configureDefaultSettings();
    }

    _task->_type = TASK_NONE;
    flushMouseEventStack();

    // apply new pitch restrictions
    double old_pitch_rad;
    getEulerAngles( _rotation, 0L, &old_pitch_rad );

    double old_pitch_deg = osg::RadiansToDegrees(old_pitch_rad);

    double new_pitch_deg = osg::clampBetween( old_pitch_deg, _settings->getMinPitch(), _settings->getMaxPitch() );

    setDistance(_distance);

    if ( !osg::equivalent(new_pitch_deg, old_pitch_deg) )
    {
        Viewpoint vp = getViewpoint();
        vp.pitch() = Angle(new_pitch_deg, Units::DEGREES);
        setViewpoint( vp );
    }
}

EarthManipulator::Settings*
EarthManipulator::getSettings() const
{
    return _settings.get();
}

void
EarthManipulator::reinitialize()
{
    _distance = 1.0;
    _viewOffset.set(0,0);
    _posOffset.set(0,0,0);
    _thrown = false;
    _dx = 0.0;
    _dy = 0.0;
    _throw_dx = 0.0;
    _throw_dy = 0.0;
    _continuous = false;
    _task = new Task();
    _last_action = ACTION_NULL;
    _srs = 0L;
    _pendingViewpoint.unset();
    _setVP0.unset();
    _setVP1.unset();
    _lastPointOnEarth.set(0.0, 0.0, 0.0);
    _setVPArcHeight = 0.0;
    _pushed = false;
}


bool
EarthManipulator::established()
{
    if ( _srs.valid() && _mapNode.valid() && _node.valid() )
        return true;

    // lock down the observed node:
    osg::ref_ptr<osg::Node> safeNode;
    if ( !_node.lock(safeNode) )
        return false;

    // find a map node or fail:
    _mapNode = osgEarth::MapNode::findMapNode( safeNode.get() );
    if ( !_mapNode.valid() )
        return false;

    // leave if the map's profile is not estabilshed yet
    if (!_mapNode->getMapSRS())
        return false;

    // resetablish the terrain callback on the map node:
    if ( _terrainCallback.valid() && _mapNode->getTerrain() )
    {
        _mapNode->getTerrain()->removeTerrainCallback( _terrainCallback.get() );
    }
    _terrainCallback = new ManipTerrainCallback( this );
    if (_mapNode->getTerrain())
        _mapNode->getTerrain()->addTerrainCallback( _terrainCallback.get() );

    // Cache the SRS.
    _srs = _mapNode->getMapSRS();

    // Set the home viewpoint if necessary.
    if ( !_homeViewpoint.isSet() )
    {
        if ( _pendingViewpoint.isSet() )
        {
            setHomeViewpoint( _pendingViewpoint.get(), _pendingViewpointDuration.as(Units::SECONDS) );
        }

        else if ( _srs->isGeographic() )
        {
            Viewpoint vp;
            vp.focalPoint() = GeoPoint(_srs.get(), -90.0, 0, 0, ALTMODE_ABSOLUTE);
            vp.heading() = Angle(0.0, Units::DEGREES );
            vp.pitch() = Angle(-89.0, Units::DEGREES );
            vp.range() = Distance(_srs->getEllipsoid().getRadiusEquator() * 3.0, Units::METERS );
            vp.positionOffset() = osg::Vec3d(0,0,0);
            setHomeViewpoint( vp );
        }
        else
        {
            Viewpoint vp;
            const Profile* profile = _mapNode->getMap()->getProfile();
            vp.focalPoint() = profile->getExtent().getCentroid();
            vp.range() = Distance(2.0 * std::max(profile->getExtent().width(), profile->getExtent().height()), Units::METERS);
            vp.positionOffset() = osg::Vec3d(0, 0, 0);
            vp.heading() = Angle(0.0, Units::DEGREES);
            vp.pitch() = Angle(-90.0, Units::DEGREES);
            setHomeViewpoint( vp );
        }
    }

    if ( !_pendingViewpoint.isSet() )
    {
        setViewpoint( _homeViewpoint.get(), _homeViewpointDuration );
    }
    else
    {
        setViewpoint( _pendingViewpoint.get(), _pendingViewpointDuration.as(Units::SECONDS) );
    }

    // Clear out any pending viewpoint.
    _pendingViewpoint.unset();

    return true;
}


void
EarthManipulator::handleTileUpdate(const TileKey& key, osg::Node* graph, TerrainCallbackContext& context)
{
    // Only do collision avoidance if it's enabled, we're not tethering and
    // we're not in the middle of setting a viewpoint.
    if (getSettings()->getTerrainAvoidanceEnabled() &&
        !isTethering() &&
        !isSettingViewpoint() )
    {
        const GeoPoint& pt = centerMap();
        if ( key.getExtent().contains(pt.x(), pt.y()) )
        {
            recalculateCenterFromLookVector();
            collisionDetect();
        }
    }
}

bool
EarthManipulator::createLocalCoordFrame( const osg::Vec3d& worldPos, osg::CoordinateFrame& out_frame ) const
{
    if ( _srs.valid() )
    {
        osg::Vec3d mapPos;
        _srs->transformFromWorld( worldPos, mapPos );
        _srs->createLocalToWorld( mapPos, out_frame );
    }
    return _srs.valid();
}


void
EarthManipulator::setCenter( const osg::Vec3d& worldPos )
{
    _center = worldPos;
    createLocalCoordFrame( worldPos, _centerLocalToWorld );
    if ( _srs.valid() )
    {
        _centerMap.fromWorld( _srs.get(), worldPos );
    }

    // cache the "last known" focal point height so we can use it as a
    // backup if necessary.
    _centerHeight = _srs->isGeographic() ? _center.length() : _center.z();
}


void
EarthManipulator::setNode(osg::Node* node)
{
    // you can only set the node if it has not already been set, OR if you are setting
    // it to NULL. (So to change it, you must first set it to NULL.) This is to prevent
    // OSG from overwriting the node after you have already set on manually.
    if ( node == 0L || !_node.valid() )
    {
        _node     = node;
        _mapNode = 0L;
        _srs     = 0L;

        reinitialize();
        established();
    }
}

osg::Node*
EarthManipulator::getNode()
{
    return _node.get();
}

osg::Matrixd
EarthManipulator::getRotation(const osg::Vec3d& point) const
{
    //The look vector will be going directly from the eye point to the point on the earth,
    //so the look vector is simply the up vector at the center point
    osg::CoordinateFrame cf;
    createLocalCoordFrame( point, cf );

    osg::Vec3d lookVector = -getUpVector(cf);

    osg::Vec3d side;

    //Force the side vector to be orthogonal to north
    osg::Vec3d worldUp(0,0,1);

    double dot = osg::absolute(worldUp * lookVector);
    if (osg::equivalent(dot, 1.0))
    {
        //We are looking nearly straight down the up vector, so use the Y vector for world up instead
        worldUp = osg::Vec3d(0, 1, 0);
    }

    side = lookVector ^ worldUp;
    osg::Vec3d up = side ^ lookVector;
    up.normalize();

    //We want a very slight offset
    double offset = 1e-6;

    return osg::Matrixd::lookAt( point - (lookVector * offset), point, up);
}

osg::Quat
EarthManipulator::computeCenterRotation(const osg::Vec3d& point) const
{
    return getRotation(point).getRotate().inverse();
}


Viewpoint
EarthManipulator::getViewpoint() const
{
    Viewpoint vp;

    // Tethering? Use the tether viewpoint.
    if ( isTethering() && _setVP1.isSet() )
    {
        vp = _setVP1.get();

        if (vp.getNode().valid())
        {
            GeoPoint point;
            point.fromWorld(_srs.get(), computeWorld(vp.getNode().get()));
            vp.focalPoint() = point;
        }
        else
        {
            vp.focalPoint().unset();
        }
    }

    // Transitioning? Capture the last calculated intermediate position.
    else if ( isSettingViewpoint() )
    {
        vp.focalPoint().mutable_value().fromWorld( _srs.get(), _center );
    }

    // If we are stationary:
    else
    {
        vp.focalPoint().mutable_value().fromWorld( _srs.get(), _center );
    }

    // Always update the local offsets.
    double localAzim, localPitch;
    getEulerAngles( _rotation, &localAzim, &localPitch );

    vp.heading() = Angle(localAzim, Units::RADIANS).to(Units::DEGREES);
    vp.pitch() = Angle(localPitch, Units::RADIANS).to(Units::DEGREES);
    vp.range() = Distance(_distance, Units::METERS);

    if ( _posOffset.x() != 0.0 || _posOffset.y() != 0.0 || _posOffset.z() != 0.0 )
    {
        vp.positionOffset() = _posOffset;
    }

    return vp;
}

void
EarthManipulator::setViewpoint(const Viewpoint& vp, double duration_seconds)
{
    // If the manip is not set up, save the viewpoint for later.
    if ( !established() )
    {
        _pendingViewpoint = vp;
        _pendingViewpointDuration.set(duration_seconds, Units::SECONDS);
    }

    else
    {
        // Save any existing tether node so we can properly invoke the callback.
        osg::ref_ptr<osg::Node> oldEndNode;
        if ( isTethering() && _tetherCallback.valid() )
        {
            oldEndNode = _setVP1->getNode();
        }

        // starting viewpoint; all fields will be set:
        _setVP0 = getViewpoint();

        // ending viewpoint
        _setVP1 = vp;

        // Reset the tethering offset quat.
        _tetherRotationVP0 = _tetherRotation;
        _tetherRotationVP1 = osg::Quat();

        // Fill in any missing end-point data with defaults matching the current camera setup.
        // Then all fields are guaranteed to contain usable data during transition.
        double defPitch, defAzim;
        getEulerAngles( _rotation, &defAzim, &defPitch );

        if ( !_setVP1->heading().isSet() )
            _setVP1.mutable_value().heading() = Angle(defAzim, Units::RADIANS);

        if ( !_setVP1->pitch().isSet() )
            _setVP1.mutable_value().pitch() = Angle(defPitch, Units::RADIANS);

        if ( !_setVP1->range().isSet() )
            _setVP1.mutable_value().range() = Distance(_distance, Units::METERS);

        if ( !_setVP1->nodeIsSet() && !_setVP1->focalPoint().isSet() )
        {
            osg::ref_ptr<osg::Node> vpNode = _setVP0->getNode();
            if (vpNode.valid())
                _setVP1.mutable_value().setNode(vpNode.get());
            else
                _setVP1.mutable_value().focalPoint() = _setVP0->focalPoint().get();
        }

        _setVPDuration.set( std::max(duration_seconds, 0.0), Units::SECONDS );

        // access the new tether node if it exists:
        osg::ref_ptr<osg::Node> endNode = _setVP1->getNode();

        // Timed transition, we need to calculate some things:
        if ( duration_seconds > 0.0 )
        {
            // Start point is the current manipulator center:
            osg::Vec3d startWorld;
            osg::ref_ptr<osg::Node> startNode = _setVP0->getNode();
            startWorld = startNode.valid() ? computeWorld(startNode.get()) : _center;

            _setVPStartTime.unset();

            // End point is the world coordinates of the target viewpoint:
            osg::Vec3d endWorld;
            if ( endNode.valid() )
                endWorld = computeWorld(endNode.get());
            else
                _setVP1->focalPoint()->transform( _srs.get() ).toWorld(endWorld);

            // calculate an acceleration factor based on the Z differential.
            _setVPArcHeight = 0.0;
            double range0 = _setVP0->range()->as(Units::METERS);
            double range1 = _setVP1->range()->as(Units::METERS);

            double pitch0 = _setVP0->pitch()->as(Units::RADIANS);
            double pitch1 = _setVP1->pitch()->as(Units::RADIANS);

            double h0 = range0 * sin( -pitch0 );
            double h1 = range1 * sin( -pitch1 );
            double dh = (h1 - h0);

            // calculate the total distance the focal point will travel and derive an arc height:
            double de = (endWorld - startWorld).length();

            // maximum height during viewpoint transition
            if ( _settings->getArcViewpointTransitions() )
            {
                _setVPArcHeight = osg::maximum( de - fabs(dh), 0.0 );
            }

            // calculate acceleration coefficients
            if ( _setVPArcHeight > 0.0 )
            {
                // if we're arcing, we need separate coefficients for the up and down stages
                double h_apex = 2.0*(h0+h1) + _setVPArcHeight;
                double dh2_up = fabs(h_apex - h0)/100000.0;
                _setVPAccel = log10( dh2_up );
                double dh2_down = fabs(h_apex - h1)/100000.0;
                _setVPAccel2 = -log10( dh2_down );
            }
            else
            {
                // on arc => simple unidirectional acceleration:
                double dh2 = (h1 - h0)/100000.0;
                _setVPAccel = fabs(dh2) <= 1.0? 0.0 : dh2 > 0.0? log10( dh2 ) : -log10( -dh2 );
                if ( fabs( _setVPAccel ) < 1.0 ) _setVPAccel = 0.0;
            }

            // Adjust the duration if necessary.
            if ( _settings->getAutoViewpointDurationEnabled() )
            {
                double maxDistance = _srs->getEllipsoid().getRadiusEquator();
                double ratio = osg::clampBetween( de/maxDistance, 0.0, 1.0 );
                ratio = accelerationInterp( ratio, -4.5 );
                double minDur, maxDur;
                _settings->getAutoViewpointDurationLimits( minDur, maxDur );
                _setVPDuration.set( minDur + ratio*(maxDur-minDur), Units::SECONDS );
            }
        }

        else
        {
            // Immediate transition? Just do it now.
            _setVPStartTime = Duration(_time_s_now, Units::SECONDS);
            setViewpointFrame( _time_s_now );
        }

        // Fire a tether callback if required.
        if ( _tetherCallback.valid() )
        {
            // starting a tether to a NEW node:
            if ( isTethering() && oldEndNode.get() != endNode.get() )
                (*_tetherCallback)( endNode.get() );

            // breaking a tether:
            else if ( !isTethering() && oldEndNode.valid() )
                (*_tetherCallback)( 0L );
        }
    }

    // reset other global state flags.
    _thrown      = false;
    _task->_type = TASK_NONE;
}

// returns "t" [0..1], the interpolation coefficient.
double
EarthManipulator::setViewpointFrame(double time_s)
{
    if ( !_setVPStartTime.isSet() )
    {
        _setVPStartTime = Duration(time_s, Units::SECONDS);
        return 0.0;
    }
    else
    {
        // Start point is the current manipulator center:
        osg::Vec3d startWorld;
        osg::ref_ptr<osg::Node> startNode = _setVP0->getNode();
        if (startNode.valid())
            startWorld = computeWorld(startNode.get());
        else
            _setVP0->focalPoint()->transform( _srs.get() ).toWorld(startWorld);

        // End point is the world coordinates of the target viewpoint:
        osg::Vec3d endWorld;
        osg::ref_ptr<osg::Node> endNode = _setVP1->getNode();
        if (endNode.valid())
            endWorld = computeWorld(endNode.get());
        else
            _setVP1->focalPoint()->transform( _srs.get() ).toWorld(endWorld);

        // Remaining time is the full duration minus the time since initiation:
        double elapsed = time_s - _setVPStartTime->as(Units::SECONDS);
        double duration = _setVPDuration.as(Units::SECONDS);
        double t = std::min(1.0, duration > 0.0 ? elapsed/duration : 1.0);
        double tp = t;

        if ( _setVPArcHeight > 0.0 )
        {
            if ( tp <= 0.5 )
            {
                double t2 = 2.0*tp;
                tp = 0.5*t2;
            }
            else
            {
                double t2 = 2.0*(tp-0.5);
                tp = 0.5+(0.5*t2);
            }

            // the more smoothsteps you do, the more pronounced the fade-in/out effect
            tp = smoothStepInterp( tp );
        }
        else if ( t > 0.0 )
        {
            tp = smoothStepInterp( tp );
        }

        osg::Vec3d newCenter =
            _srs->isGeographic() ? nlerp(startWorld, endWorld, tp) : lerp(startWorld, endWorld, tp);

        // Calculate the delta-heading, and make sure we are going in the shortest direction:
        Angle d_azim = _setVP1->heading().get() - _setVP0->heading().get();
        if ( d_azim.as(Units::RADIANS) > osg::PI )
            d_azim = d_azim - Angle(2.0*osg::PI, Units::RADIANS);
        else if ( d_azim.as(Units::RADIANS) < -osg::PI )
            d_azim = d_azim + Angle(2.0*osg::PI, Units::RADIANS);
        double newAzim = _setVP0->heading()->as(Units::RADIANS) + tp*d_azim.as(Units::RADIANS);

        // Calculate the new pitch:
        Angle d_pitch = _setVP1->pitch().get() - _setVP0->pitch().get();
        double newPitch = _setVP0->pitch()->as(Units::RADIANS) + tp*d_pitch.as(Units::RADIANS);

        // Calculate the new range:
        Distance d_range = _setVP1->range().get() - _setVP0->range().get();
        double newRange =
            _setVP0->range()->as(Units::METERS) +
            d_range.as(Units::METERS)*tp + sin(osg::PI*tp)*_setVPArcHeight;

        // Calculate the offsets
        osg::Vec3d offset0 = _setVP0->positionOffset().getOrUse(osg::Vec3d(0,0,0));
        osg::Vec3d offset1 = _setVP1->positionOffset().getOrUse(osg::Vec3d(0,0,0));
        osg::Vec3d newOffset = offset0 + (offset1-offset0)*tp;

        // Activate.
        setLookAt( newCenter, newAzim, newPitch, newRange, newOffset );

        // interpolate tether rotation:
        _tetherRotation.slerp(tp, _tetherRotationVP0, _tetherRotationVP1);

        // At t=1 the transition is complete.
        if ( t >= 1.0 )
        {
            _setVP0.unset();

            // If this was a transition into a tether, keep the endpoint around so we can
            // continue tracking it.
            if ( !isTethering() )
            {
                _setVP1.unset();
            }
        }

        return tp;
    }
}

void
EarthManipulator::setLookAt(const osg::Vec3d& center,
                            double            azim,
                            double            pitch,
                            double            range,
                            const osg::Vec3d& posOffset)
{
    setCenter( center );
    setDistance( range );

    _previousUp = getUpVector( _centerLocalToWorld );
    _centerRotation = computeCenterRotation( center );

    _posOffset = posOffset;

    azim = normalizeAzimRad( azim );

    pitch = osg::clampBetween(
        pitch,
        osg::DegreesToRadians(_settings->getMinPitch()),
        osg::DegreesToRadians(_settings->getMaxPitch()) );

    _rotation = getQuaternion(azim, pitch);
}

void
EarthManipulator::resetLookAt()
{
    double pitch;
    getEulerAngles( _rotation, 0L, &pitch );

    double maxPitch = osg::DegreesToRadians(-10.0);
    if ( pitch > maxPitch )
        rotate( 0.0, -(pitch-maxPitch) );

    osg::Vec3d eye = getWorldMatrix().getTrans();

    // calculate the center point in front of the eye. The reference frame here
    // is the view plane of the camera.
    osg::Matrix m( _rotation * _centerRotation );
    recalculateCenter( m );

    double newDistance = (eye-_center).length();
    setDistance( newDistance );

    _posOffset.set(0,0,0);
    _viewOffset.set(0,0);

    _tetherRotation = osg::Quat();
    _tetherRotationVP0 = osg::Quat();
    _tetherRotationVP1 = osg::Quat();
}

bool
EarthManipulator::isSettingViewpoint() const
{
    return _setVP0.isSet() && _setVP1.isSet();
}

void
EarthManipulator::clearViewpoint()
{
    bool breakingTether = isTethering();

    // Cancel any ongoing transition or tethering:
    _setVP0.unset();
    _setVP1.unset();

    if (breakingTether)
    {
        collapseTetherRotationIntoRotation();
    }

    // Restore the matrix values in a neutral state.
    recalculateCenterFromLookVector();

    // Fire the callback to indicate a tethering break.
    if ( _tetherCallback.valid() && breakingTether )
        (*_tetherCallback)( 0L );
}

bool
EarthManipulator::isTethering() const
{
    // True if setViewpoint() was called and the viewpoint has a node.
    return _setVP1.isSet() && _setVP1->nodeIsSet();
}

void EarthManipulator::collisionDetect()
{
    if (!getSettings()->getTerrainAvoidanceEnabled() ||
        !_srs.valid() )
    {
        return;
    }
    // The camera has changed, so make sure we aren't under the ground.

    osg::Vec3d eye = getWorldMatrix().getTrans();
    osg::CoordinateFrame eyeCoordFrame;
    createLocalCoordFrame(eye, eyeCoordFrame);
    osg::Vec3d eyeUp = getUpVector(eyeCoordFrame);

    // Try to intersect the terrain with a vector going straight up and down.
    double r = std::min(_srs->getEllipsoid().getRadiusEquator(), _srs->getEllipsoid().getRadiusPolar());
    osg::Vec3d ip, normal;

    if (intersect(eye + eyeUp * r, eye - eyeUp * r, ip, normal))
    {
        double eps = _settings->getTerrainAvoidanceMinimumDistance();
        // Now determine if the point is above the ground or not
        osg::Vec3d v0 = eyeUp;
        v0.normalize();
        osg::Vec3d v1 = eye - (ip + eyeUp * eps);
        v1.normalize();

        // save rotation so we can restore it later - the setraw method
        // may alter it and we don't want that.
        osg::Quat rotation = _rotation;

        //osg::Vec3d adjVector = normal;
        osg::Vec3d adjVector = eyeUp;
        if (v0 * v1 <= 0)
        {
            setByLookAtRaw(ip + adjVector * eps, _center, eyeUp);
            _rotation = rotation;
        }
    }
}


bool
EarthManipulator::intersect(const osg::Vec3d& start, const osg::Vec3d& end, osg::Vec3d& intersection, osg::Vec3d& normal) const
{
    osg::ref_ptr<MapNode> mapNode;
    if ( _mapNode.lock(mapNode) && mapNode->getTerrainEngine() )
    {
		osg::ref_ptr<osgUtil::LineSegmentIntersector> lsi = NULL;

		lsi = new osgUtil::LineSegmentIntersector(start,end);

        osgUtil::IntersectionVisitor iv(lsi.get());
        iv.setTraversalMask(_intersectTraversalMask);

        mapNode->getTerrainEngine()->getNode()->accept(iv);

        if (lsi->containsIntersections())
        {
            intersection = lsi->getIntersections().begin()->getWorldIntersectPoint();
            normal = lsi->getIntersections().begin()->getWorldIntersectNormal();
            return true;
        }
    }
    return false;
}

bool
EarthManipulator::intersectLookVector(osg::Vec3d& out_eye,
                                      osg::Vec3d& out_target,
                                      osg::Vec3d& out_up ) const
{
    bool success = false;

    osg::ref_ptr<MapNode> mapNode;
    if ( _mapNode.lock(mapNode) && mapNode->getTerrainEngine() )
    {
        double R = _centerHeight;

        getWorldInverseMatrix().getLookAt(out_eye, out_target, out_up, 1.0);
        osg::Vec3d look = out_target-out_eye;

		osg::ref_ptr<osgUtil::LineSegmentIntersector> lsi =
		    new osgUtil::LineSegmentIntersector(out_eye, out_eye+look*1e8);

        lsi->setIntersectionLimit(lsi->LIMIT_NEAREST);

        osgUtil::IntersectionVisitor iv(lsi.get());
        iv.setTraversalMask(_intersectTraversalMask);

        mapNode->getTerrainEngine()->getNode()->accept(iv);

        if (lsi->containsIntersections())
        {
            out_target = lsi->getIntersections().begin()->getWorldIntersectPoint();
            if ( !_srs->isGeographic() || GeoMath::isPointVisible(out_eye, out_target, R) )
            {
                success = true;
            }
        }

        if ( !success )
        {
            // backup plan: intersect spheroid (if geocentric) or base plane (if projected)
            if ( _srs->isGeographic() )
            {
                osg::Vec3d i0, i1;
                unsigned hits = GeoMath::interesectLineWithSphere(out_eye, out_eye+look*1e8, R, i0, i1);
                if ( hits > 0 )
                {
                    // Need to check not only for intersection, but also visibility
                    // over the horizon.

                    // one hit? look vec is tangent to sphere, or camera is underground
                    if ( hits == 1 && GeoMath::isPointVisible(out_eye, i0, R) )
                    {
                        out_target = i0;
                        success = true;
                    }

                    // two hits? look vec intersects sphere (in two places)
                    else if ( hits == 2 )
                    {
                        // select the closest hit
                        out_target = (out_eye-i0).length2() < (out_eye-i1).length2() ? i0 : i1;
                        if ( GeoMath::isPointVisible(out_eye, out_target, R) )
                        {
                            success = true;
                        }
                    }
                }
            }

            else // !_srs->isGeographic()
            {
                osg::Vec3d i0;
                osg::Plane zup(0, 0, 1, 0);
                unsigned hits = GeoMath::intersectLineWithPlane(out_eye, out_eye+look, zup, i0);
                if (hits > 0)
                {
                    out_target = i0;
                    success = true;
                }
            }
        }
    }

    return success;
}

void
EarthManipulator::home(double unused)
{
    handleAction( ACTION_HOME, 0, 0, 0 );
}

void
EarthManipulator::home(const osgGA::GUIEventAdapter& ,osgGA::GUIActionAdapter& us)
{
    home( 0.0 );
    us.requestRedraw();
}

void
EarthManipulator::computeHomePosition(const osg::Camera *camera, bool useBoundingBox)
{
    if( getNode() )
    {
        const osg::BoundingSphere& boundingSphere = getNode()->getBound();

        osg::Vec3d eye =
            boundingSphere._center +
            osg::Vec3( 0.0, -3.5f * boundingSphere._radius, boundingSphere._radius * 0.0001 );

        setHomePosition(
            eye,
            boundingSphere._center,
            osg::Vec3d( 0, 0, 1 ),
            _autoComputeHomePosition );
    }
}

void
EarthManipulator::init(const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter& )
{
    flushMouseEventStack();
}


void
EarthManipulator::getUsage(osg::ApplicationUsage& usage) const
{
}

void
EarthManipulator::resetMouse( osgGA::GUIActionAdapter& aa, bool flushEventStack )
{
    if (flushEventStack)
      flushMouseEventStack();

    aa.requestContinuousUpdate( false );
    _thrown = false;
    _continuous = false;
    _single_axis_x = 1.0;
    _single_axis_y = 1.0;
    _lastPointOnEarth.set(0.0, 0.0, 0.0);
}


// Camera updates get called AFTER the scene gets its update traversal. So, if you have
// tethering enabled (or some other feature that tracks scene graph nodes), this will
// update the camera after the scene graph. This is important in order to maintain
// frame coherency and prevent "jitter".
//
// The reason we install/uninstall instead of just leaving it there is so we can
// support OSG's "ON_DEMAND" frame scheme, which disables itself is there are any
// update callbacks in the scene graph.
void
EarthManipulator::updateProjection(osg::Camera* eventCamera)
{
    // check to see if we need to install a new camera callback:
    if ( eventCamera )
    {
        // update the projection matrix if necessary
        osg::Viewport* vp = eventCamera->getViewport();
        if ( vp )
        {
            const osg::Matrixd& proj = eventCamera->getProjectionMatrix();
            bool isOrtho = osg::equivalent(proj(3,3), 1.0);

            // For a perspective camera, remember the last known VFOV. We will need it if we
            // detect a switch to orthographic.
            if ( !isOrtho )
            {
                double vfov, ar, zn, zf;
                if (eventCamera->getProjectionMatrixAsPerspective(vfov, ar, zn, zf))
                {
                    _lastKnownVFOV = vfov;
                }
            }

            // For an orthographic camera, convert the distance and remembered VFOV
            // into proper x/y extents to simulate "zoom".
            else if ( _settings->getOrthoTracksPerspective() )
            {
                // need to update the ortho projection matrix to reflect the camera distance.
                double ar = vp->width()/vp->height();
                double y = _distance * tan(0.5*osg::DegreesToRadians(_lastKnownVFOV));
                double x = y * ar;

                double ignore, N, F;
                ProjectionMatrix::getOrtho(proj, ignore, ignore, ignore, ignore, N, F);
                //proj.getOrtho(ignore, ignore, ignore, ignore, N, F);
                osg::Matrix orthoProj;
                ProjectionMatrix::setOrtho(orthoProj, -x, +x, -y, +y, N, F,
                    ProjectionMatrix::getType(proj));

                eventCamera->setProjectionMatrix(orthoProj);

                //OE_DEBUG << "ORTHO: "
                //    << "ar = " << ar << ", width=" << vp->width() << ", height=" << vp->height()
                //    << ", dist = " << _distance << ", vfov=" << _lastKnownVFOV
                //    << ", X = " << x << ", Y = " << y
                //    << std::endl;
            }
        }
    }
}


bool
EarthManipulator::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
{
    bool handled = false;

    // first order of business: make sure the CSN is established.
    if ( !established() )
        return false;

    osg::View* view = aa.asView();

    _time_s_now = ea.getTime();
    //_time_s_now = view->getFrameStamp()->getReferenceTime();

    if ( ea.getEventType() == osgGA::GUIEventAdapter::FRAME )
    {
        if ( _node.valid() )
        {
            // Update the mapnode reference frame. This is the transformation
            // between the Camera and the MapNode. Since all computations are done
            // in map-world space (e.g., ECEF) we need to then introduce this transform
            // at the very end (in getInverseMatrix()) to apply the final view matrix.
            {
                osg::ref_ptr<MapNode> mapNode;
                if (_mapNode.lock(mapNode))
                {
                    _mapNodeFrame = osg::computeLocalToWorld(mapNode->getParentalNodePaths()[0]);
                    _mapNodeFrameInverse.invert(_mapNodeFrame);
                }
            }

            if ( _pendingViewpoint.isSet() )
            {
                setViewpoint( _pendingViewpoint.get(), _pendingViewpointDuration.as(Units::SECONDS) );
                _pendingViewpoint.unset();
                aa.requestRedraw();
            }

            else if ( isSettingViewpoint() && !isTethering() )
            {
                if ( _frameCount < 2 )
                    _setVPStartTime = Duration(_time_s_now, Units::SECONDS);
            }

            if (_thrown)
            {
                double decayFactor = 1.0 - _settings->getThrowDecayRate();

                _throw_dx = std::abs(_throw_dx) > std::abs(_dx * 0.01) ? _throw_dx * decayFactor : 0.0;
                _throw_dy = std::abs(_throw_dy) > std::abs(_dy * 0.01) ? _throw_dy * decayFactor : 0.0;

                if (_throw_dx == 0.0 && _throw_dy == 0.0)
                    _thrown = false;
                else
                    handleMovementAction(_last_action._type, _throw_dx, _throw_dy, aa.asView());
            }

            aa.requestContinuousUpdate( isSettingViewpoint() || _thrown || _pushed );

            if ( _continuous )
            {
                handleContinuousAction( _last_action, aa.asView() );
                aa.requestRedraw();
            }
            else
            {
                _continuous_dx = 0.0;
                _continuous_dy = 0.0;
            }

            if ( _task.valid() && _task->_type != TASK_NONE )
            {
                bool stillRunning = serviceTask(aa.asView());
                if ( stillRunning )
                {
                    aa.requestContinuousUpdate( true );
                }
                else
                {
                    // turn off the continuous, but we still need one last redraw
                    // to process the final state.
                    aa.requestContinuousUpdate( false );
                    aa.requestRedraw();
                }
            }
        }

        _frameCount++;

        return false;
    }


    // the camera manipulator runs last after any other event handlers. So bail out
    // if the incoming event has already been handled by another handler.
    if ( ea.getHandled() )
    {
        return false;
    }

    // form the current Action based on the event type:
    Action action = ACTION_NULL;

    // if tethering is active, check to see whether the incoming mouse event
    // will break the tether.
    if (isTethering() && ea.getEventType() != ea.FRAME && !ea.isMultiTouchEvent())
    {
        const ActionTypeVector& atv = _settings->getBreakTetherActions();
        if ( atv.size() > 0 )
        {
            EventType eventType = (EventType)ea.getEventType();
            int buttonMask = ea.getButtonMask();
            int modKeyMask = ea.getModKeyMask();

            if (eventType == (EventType)osgGA::GUIEventAdapter::RELEASE && isMouseClick(&ea))
            {
                eventType = EVENT_MOUSE_CLICK;
                buttonMask = _mouse_down_event->getButtonMask();
                modKeyMask = _mouse_down_event->getModKeyMask();
            }

            const Action& action = _settings->getAction( eventType, buttonMask, modKeyMask );
            if ( std::find(atv.begin(), atv.end(), action._type) != atv.end() )
            {
                clearViewpoint();
            }
        }
    }


    if ( ea.isMultiTouchEvent() )
    {
        // not a mouse event; clear the mouse queue.
        resetMouse( aa, false );

        // queue up a touch event set and figure out the current state:
        addTouchEvents(ea);
        TouchEvents te;
        if ( parseTouchEvents(te) )
        {
            for( TouchEvents::iterator i = te.begin(); i != te.end(); ++i )
            {
                action = _settings->getAction(i->_eventType, i->_mbmask, 0);

                // Deal with tether breaking for touch
                if (isTethering())
                {
                    const ActionTypeVector& atv = _settings->getBreakTetherActions();
                    if ( std::find(atv.begin(), atv.end(), action._type) != atv.end() )
                    {
                        clearViewpoint();
                    }
                }

                if (action._type != ACTION_NULL)
                {
                    _last_event = i->_eventType;

                    // here we adjust for action scale, global sensitivy
                    double dx = i->_dx, dy = i->_dy;
                    dx *= _settings->getMouseSensitivity();
                    dy *= _settings->getMouseSensitivity();
                    applyOptionsToDeltas( action, dx, dy );

                    if (dx == 0.0 && dy == 0.0) continue;

                    _dx = dx;
                    _dy = dy;

                    if (action._type == ACTION_GOTO)
                        handlePointAction(action, ea.getX(), ea.getY(), view);
                    else
                        handleMovementAction(action._type, dx, dy, view);

                    aa.requestRedraw();
                }
            }

            handled = true;
        }
        else
        {
            // The only multitouch event we want passed on if not handled is a release
            handled = ea.getEventType() != osgGA::GUIEventAdapter::RELEASE;

            // if a new push occurs we want to reset the dx/dy values to stop/prevent throwing
            if (ea.getEventType() == osgGA::GUIEventAdapter::PUSH)
                _dx = _dy = 0.0;
        }
    }

    if ( !handled )
    {
        // not a touch event; clear the touch queue.
        //_touchPointQueue.clear();

        switch( ea.getEventType() )
        {
            case osgGA::GUIEventAdapter::PUSH:
                _pushed = true;
                resetMouse( aa );
                addMouseEvent( ea );
                _mouse_down_event = &ea;
                aa.requestRedraw();
                handled = true;
                break;

            case osgGA::GUIEventAdapter::RELEASE:
                _pushed = false;
                if ( _continuous )
                {
                    // bail out of continuous mode if necessary:
                    _continuous = false;
                    aa.requestContinuousUpdate( false );
                }
                else
                {
                    action = _last_action;

                    _throw_dx = fabs(_dx) > 0.01 ? _dx : 0.0;
                    _throw_dy = fabs(_dy) > 0.01 ? _dy : 0.0;

                    if (_settings->getThrowingEnabled() && ( _time_s_now - _time_s_last_event < 0.05 ) && (_throw_dx != 0.0 || _throw_dy != 0.0))
                    {
                        _thrown = true;
                        aa.requestRedraw();
                        aa.requestContinuousUpdate( true );
                    }
                    else if ( isMouseClick( &ea ) )
                    {
                        addMouseEvent( ea );
                        if ( _mouse_down_event )
                        {
                            action = _settings->getAction( EVENT_MOUSE_CLICK, _mouse_down_event->getButtonMask(), _mouse_down_event->getModKeyMask() );
                            if ( handlePointAction( action, ea.getX(), ea.getY(), aa.asView() ))
                                aa.requestRedraw();
                        }
                        resetMouse( aa );
                    }
                    else
                    {
                        resetMouse( aa );
                        addMouseEvent( ea );
                    }
                }
                handled = true;
                break;

            case osgGA::GUIEventAdapter::DOUBLECLICK:
                // bail out of continuous mode if necessary:
                _continuous = false;
                _pushed = false;
                addMouseEvent( ea );
                if (_mouse_down_event)
                {
                    action = _settings->getAction( ea.getEventType(), _mouse_down_event->getButtonMask(), _mouse_down_event->getModKeyMask() );
                    if ( handlePointAction( action, ea.getX(), ea.getY(), aa.asView() ) )
                        aa.requestRedraw();
                    resetMouse( aa );
                    handled = true;
                }
                break;

            case osgGA::GUIEventAdapter::MOVE: // MOVE not currently bindable
                //NOP
                break;

            case osgGA::GUIEventAdapter::DRAG:
                {
                    if (_pushed)
                    {
                        action = _settings->getAction( ea.getEventType(), ea.getButtonMask(), ea.getModKeyMask() );
                        addMouseEvent( ea );
                        bool wasContinuous = _continuous;
                        _continuous = action.getBoolOption(OPTION_CONTINUOUS, false);
                        if ( handleMouseAction( action, aa.asView() ) )
                            aa.requestRedraw();

                        if ( _continuous && !wasContinuous )
                            _last_continuous_action_time = _time_s_now;

                        aa.requestContinuousUpdate(_continuous);
                        _thrown = false;
                        handled = true;
                    }
                }
                break;

            case osgGA::GUIEventAdapter::KEYDOWN:
                if ( ea.getKey() < osgGA::GUIEventAdapter::KEY_Shift_L )
                {
                    resetMouse( aa );
                    action = _settings->getAction( ea.getEventType(), ea.getKey(), ea.getModKeyMask() );
                    if ( handleKeyboardAction( action ) )
                        aa.requestRedraw();
                    handled = true;
                }
                break;

            case osgGA::GUIEventAdapter::KEYUP:
                resetMouse( aa );
                _task->_type = TASK_NONE;
                handled = true;
                break;

            case osgGA::GUIEventAdapter::SCROLL:
                resetMouse( aa );
                addMouseEvent( ea );
                action = _settings->getAction( ea.getEventType(), ea.getScrollingMotion(), ea.getModKeyMask() );
                if ( handleScrollAction( action, action.getDoubleOption(OPTION_DURATION, 0.2) ) )
                    aa.requestRedraw();
                handled = true;
                break;
            default: break;
        }
    }

    // if a new task was started, request continuous updates.
    if ( _task.valid() && _task->_type != TASK_NONE )
    {
        aa.requestContinuousUpdate( true );
    }

    if ( handled && action._type != ACTION_NULL )
    {
        _last_action = action;
        _time_s_last_event = _time_s_now;
    }

    return handled;
}

namespace
{
    /// Helper class for generating NodePathList.
    class CollectAllParentPaths : public osg::NodeVisitor
    {
    public:
        CollectAllParentPaths(const osg::Node* haltTraversalAtNode=0) :
            osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_PARENTS),
            _haltTraversalAtNode(haltTraversalAtNode)
        {
            // This is the same as the osg::CollectParentPaths visitor (which is not exported)
            // except for this node mask override to ensure that it will even traverse nodes hidden via  node mask.
            setNodeMaskOverride(~0);
        }

        virtual void apply(osg::Node& node)
        {
            if (node.getNumParents()==0 || &node==_haltTraversalAtNode)
            {
                _nodePaths.push_back(getNodePath());
            }
            else
            {
                traverse(node);
            }
       }

        const osg::Node*     _haltTraversalAtNode;
        osg::NodePath        _nodePath;
        osg::NodePathList    _nodePaths;
    };

    osg::NodePathList getAllParentalNodePaths(osg::Node* node, osg::Node* haltTraversalAtNode = 0)
    {
        CollectAllParentPaths cpp(haltTraversalAtNode);
        node->accept(cpp);
        return cpp._nodePaths;
    }
}

void
EarthManipulator::updateTether(double t)
{
    // Initial transition is complete, so update the camera for tether.
    osg::ref_ptr<osg::Node> node = _setVP1->getNode();
    if (node.valid())
    {
        // We use our getAllParentalNodePaths function instead of tether_node->getParentalNodePaths() so that we can
        // ensure even nodes hidden via a node mask are traversed.
        // Establish the reference frame for the target node. If this fails, bail out.
        osg::Matrix L2W;
        osg::NodePathList nodePaths = getAllParentalNodePaths(node.get());
        if ( nodePaths.empty() )
            return;

        osg::ref_ptr<osg::NodeVisitor> nv;
        _updateCameraNodeVisitor.lock(nv);
        ComputeLocalToWorld computeL2W(nv.get());
        computeL2W.accumulate(nodePaths[0]);
        L2W = computeL2W._matrix;

        if ( !L2W.valid() )
            return;

        // If we just called setViewpointFrame, no need to calculate the center again.
        if ( !isSettingViewpoint() )
        {
            setCenter( osg::Vec3d(0,0,0) * L2W );
            _centerRotation = computeCenterRotation(_center);
            _previousUp = getUpVector( _centerLocalToWorld );
        };

        if (_settings->getTetherMode() == TETHER_CENTER)
        {
            if ( _lastTetherMode == TETHER_CENTER_AND_ROTATION )
            {
                // level out the camera so we don't leave the camera is weird state.
                osg::Matrixd localToFrame(L2W*osg::Matrixd::inverse( _centerLocalToWorld ));
                double azim = atan2(-localToFrame(0,1),localToFrame(0,0));
                _tetherRotation.makeRotate(-azim, 0.0, 0.0, 1.0);
            }
        }
        else
        {
            // remove any scaling introduced by the model
            double sx = 1.0/sqrt(L2W(0,0)*L2W(0,0) + L2W(1,0)*L2W(1,0) + L2W(2,0)*L2W(2,0));
            double sy = 1.0/sqrt(L2W(0,1)*L2W(0,1) + L2W(1,1)*L2W(1,1) + L2W(2,1)*L2W(2,1));
            double sz = 1.0/sqrt(L2W(0,2)*L2W(0,2) + L2W(1,2)*L2W(1,2) + L2W(2,2)*L2W(2,2));
            L2W = L2W*osg::Matrixd::scale(sx,sy,sz);

            if (_settings->getTetherMode() == TETHER_CENTER_AND_HEADING)
            {
                // Back out the tetheree's rotation, then discard all but the heading component:
                osg::Matrixd localToFrame(L2W*osg::Matrixd::inverse( _centerLocalToWorld ));
                double azim = atan2(-localToFrame(0,1),localToFrame(0,0));

                osg::Quat finalTetherRotation;
                finalTetherRotation.makeRotate(-azim, 0.0, 0.0, 1.0);
                _tetherRotation.slerp(t, _tetherRotationVP0, finalTetherRotation);
            }

            // Track all rotations
            else if (_settings->getTetherMode() == TETHER_CENTER_AND_ROTATION)
            {
                osg::Quat finalTetherRotation;
                finalTetherRotation = L2W.getRotate() * _centerRotation.inverse();
                _tetherRotation.slerp(t, _tetherRotationVP0, finalTetherRotation);
            }
        }

        _lastTetherMode = _settings->getTetherMode();
    }
}

bool
EarthManipulator::serviceTask(osg::View* view)
{
    if ( _task.valid() && _task->_type != TASK_NONE )
    {
        double dt = _time_s_now - _task->_time_last_service;
        if ( dt > 0.0 )
        {
            // cap the DT so we don't exceed the expected delta.
            dt = osg::clampBelow( dt, _task->_duration_s );

            switch( _task->_type )
            {
                case TASK_PAN:
                    pan( dt * _task->_dx, dt * _task->_dy );
                    break;
                case TASK_ROTATE:
                    rotate( dt * _task->_dx, dt * _task->_dy );
                    break;
                case TASK_ZOOM:
                    zoom( dt * _task->_dx, dt * _task->_dy, view );
                    break;
                default: break;
            }

            _task->_duration_s -= dt;
            _task->_time_last_service = _time_s_now;

            if ( _task->_duration_s <= 0.0 )
            {
                _task->_type = TASK_NONE;
            }
        }
    }

    // returns true if the task is still running.
    return _task.valid() && _task->_type != TASK_NONE;
}

bool
EarthManipulator::isMouseClick( const osgGA::GUIEventAdapter* mouse_up_event ) const
{
    if ( mouse_up_event == NULL || _mouse_down_event == NULL ) return false;

    static const float velocity = 0.1f;

    float dx = mouse_up_event->getXnormalized() - _mouse_down_event->getXnormalized();
    float dy = mouse_up_event->getYnormalized() - _mouse_down_event->getYnormalized();
    float len = sqrtf( dx*dx + dy*dy );
    float dt = mouse_up_event->getTime( ) - _mouse_down_event->getTime();

    return len < dt * velocity;
}

void
EarthManipulator::flushMouseEventStack()
{
    _ga_t1 = NULL;
    _ga_t0 = NULL;
    //_touchPointQueue.clear();
}


void
EarthManipulator::addMouseEvent(const osgGA::GUIEventAdapter& ea)
{
    _ga_t1 = _ga_t0;
    _ga_t0 = &ea;
    //_touchPointQueue.clear();
}

void
EarthManipulator::addTouchEvents(const osgGA::GUIEventAdapter& ea)
{
    _ga_t1 = _ga_t0;
    _ga_t0 = &ea;

    // first, push the old event to the back of the queue.
    while ( _touchPointQueue.size() > 1 )
        _touchPointQueue.pop_front();

    // queue any new events.
    if ( ea.isMultiTouchEvent() )
    {
        osgGA::GUIEventAdapter::TouchData* data = ea.getTouchData();

        _touchPointQueue.push_back(MultiTouchPoint());
        MultiTouchPoint& ev = _touchPointQueue.back();

        for( unsigned i=0; i<data->getNumTouchPoints(); ++i )
        {
            osgGA::GUIEventAdapter::TouchData::TouchPoint tp = data->get(i);
            ev.push_back(tp);
        }
    }
}

bool
EarthManipulator::parseTouchEvents( TouchEvents& output )
{
    // Must have two touch points to continue
    if (_touchPointQueue.size() != 2)
        return false;

    const MultiTouchPoint& p0 = _touchPointQueue[0];  // old touch
    const MultiTouchPoint& p1 = _touchPointQueue[1];  // new touch
    // Each multi-touch point must have at least one touch
    if (p0.empty() || p1.empty())
        return false;

    const double sens = this->getSettings()->getTouchSensitivity();

    // Check for two-finger gestures
    if (p0.size() >= 2 && p1.size() >= 2)
    {
        // If the IDs on the two touches do not match, then ignore the event
        if (p0[0].id != p1[0].id || p0[1].id != p1[1].id)
            return false;

        // If any of the 4 touches (old/new, t0/t1) is an ended event, then ignore the touch.
        // We want to accept moved and stationary fingers; beginning fingers are OK too.
        if (p0[0].phase == osgGA::GUIEventAdapter::TOUCH_ENDED ||
            p0[1].phase == osgGA::GUIEventAdapter::TOUCH_ENDED ||
            p1[0].phase == osgGA::GUIEventAdapter::TOUCH_ENDED ||
            p1[1].phase == osgGA::GUIEventAdapter::TOUCH_ENDED)
            return false;

        // gather information about what happened:
        float dx[2], dy[2];
        for (int i = 0; i < 2; ++i)
        {
            dx[i] = p1[i].x - p0[i].x;
            dy[i] = p1[i].y - p0[i].y;
        }
        osg::Vec2f vec0 = osg::Vec2f(p0[1].x, p0[1].y) - osg::Vec2f(p0[0].x, p0[0].y);
        osg::Vec2f vec1 = osg::Vec2f(p1[1].x, p1[1].y) - osg::Vec2f(p1[0].x, p1[0].y);
        float deltaDistance = vec1.length() - vec0.length();

        float angle[2];
        angle[0] = atan2(p0[0].y - p0[1].y, p0[0].x - p0[1].x);
        angle[1] = atan2(p1[0].y - p1[1].y, p1[0].x - p1[1].x);
        float da = angle[1] - angle[0];


        // Threshold in pixels for determining if a two finger drag happened.
        float dragThres = 1.0f;

        // now see if that corresponds to any touch events:
        if (osg::equivalent(vec0.x(), vec1.x(), dragThres) &&
            osg::equivalent(vec0.y(), vec1.y(), dragThres))
        {
            // two-finger drag.
            output.push_back(TouchEvent());
            TouchEvent& ev = output.back();
            ev._eventType = EVENT_MULTI_DRAG;
            ev._dx = 0.5 * (dx[0] + dx[1]) * sens;
            ev._dy = 0.5 * (dy[0] + dy[1]) * sens;
        }
        else
        {
            // otherwise it's a pinch and/or a zoom.  You can do them together.
            if (fabs(deltaDistance) > (1.0 * 0.0005 / sens))
            {
                // distance between the fingers changed: a pinch.
                output.push_back(TouchEvent());
                TouchEvent& ev = output.back();
                ev._eventType = EVENT_MULTI_PINCH;
                ev._dx = 0.0, ev._dy = deltaDistance * -sens;
            }

            if (fabs(da) > (0.01 * 0.0005 / sens))
            {
                // angle between vectors changed: a twist.
                output.push_back(TouchEvent());
                TouchEvent& ev = output.back();
                ev._eventType = EVENT_MULTI_TWIST;
                ev._dx = da;
                //ev._dy = 0.5 * (dy[0]+dy[1]) * _touch_sens;
                ev._dy = 0.0;
            }
        }
        return output.size() > 0;
    }

    // At this point there is potential for single-touch action

    // Check for double-tap
    if (p1[0].tapCount == 2)
    {
        output.push_back(TouchEvent());
        TouchEvent& ev = output.back();
        ev._eventType = EVENT_MOUSE_DOUBLE_CLICK;
        ev._mbmask = osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON;
        ev._dx = 0.0;
        ev._dy = 0.0;
        return true;
    }

    // If the IDs on the old and new touches do not match, then ignore the event
    if (p0[0].id != p1[0].id)
        return false;

    // Neither the new nor old touch should be an ended event; ignore if so
    if ((p0[0].phase != osgGA::GUIEventAdapter::TOUCH_ENDED &&
        p1[0].phase != osgGA::GUIEventAdapter::TOUCH_ENDED))
    {
        output.push_back(TouchEvent());
        TouchEvent& ev = output.back();
        ev._eventType = EVENT_TOUCH_DRAG;
        ev._dx = (p1[0].x - p0[0].x) * sens;
        ev._dy = (p1[0].y - p0[0].y) * sens;
        return true;
    }

    return false;
}

void
EarthManipulator::setByMatrix(const osg::Matrixd& matrix)
{
    if (!established())
        return;

    osg::Vec3d lookVector(- matrix(2,0),-matrix(2,1),-matrix(2,2));
    osg::Vec3d eye(matrix(3,0),matrix(3,1),matrix(3,2));

    _centerRotation = computeCenterRotation(_center);

    osg::ref_ptr<osg::Node> safeNode = _node.get();

    if ( !safeNode.valid() )
    {
        setCenter( eye + lookVector );
        setDistance( lookVector.length() );
        _rotation = matrix.getRotate().inverse() * _centerRotation.inverse();
        return;
    }

    // need to reintersect with the terrain
    const osg::BoundingSphere& bs = safeNode->getBound();
    float distance = (eye-bs.center()).length() + safeNode->getBound().radius();
    osg::Vec3d start_segment = eye;
    osg::Vec3d end_segment = eye + lookVector*distance;

    osg::Vec3d ip, normal;
    bool hitFound = false;
    if (intersect(start_segment, end_segment, ip, normal))
    {
        setCenter( ip );
        _centerRotation = computeCenterRotation(_center);
        setDistance( (eye-ip).length());

        osg::Matrixd rotation_matrix = osg::Matrixd::translate(0.0,0.0,-_distance)*
                                       matrix*
                                       osg::Matrixd::translate(-_center);
        _rotation = rotation_matrix.getRotate() * _centerRotation.inverse();
        hitFound = true;
    }

    if (!hitFound)
    {
        osg::CoordinateFrame eyeCoordFrame;
        createLocalCoordFrame( eye, eyeCoordFrame );

        osg::Vec3d eyeUp = getUpVector(eyeCoordFrame);

        if (intersect(eye + eyeUp*distance, eye - eyeUp*distance, ip, normal))
        {
            setCenter( ip );
            _centerRotation = computeCenterRotation(_center);
            setDistance((eye-ip).length());
            _rotation.set(0,0,0,1);
            hitFound = true;
        }
    }

    //osg::CoordinateFrame coordinateFrame;
    //createLocalCoordFrame( _center, coordinateFrame );
    _previousUp = getUpVector(_centerLocalToWorld);

    recalculateRoll();
    //recalculateLocalPitchAndAzimuth();

    collisionDetect();
}

osg::Matrixd
EarthManipulator::getMatrix() const
{
    return getWorldMatrix() * _mapNodeFrame;
}

osg::Matrixd
EarthManipulator::getWorldMatrix() const
{
    return osg::Matrixd::translate(_viewOffset.x(), _viewOffset.y(), _distance) *
           osg::Matrixd::rotate   (_rotation) *
           osg::Matrixd::rotate   (_tetherRotation) *
           osg::Matrixd::translate(_posOffset) *
           osg::Matrixd::rotate   (_centerRotation) *
           osg::Matrixd::translate(_center);
}

osg::Matrixd
EarthManipulator::getInverseMatrix() const
{
    return _mapNodeFrameInverse * getWorldInverseMatrix();
}

osg::Matrixd
EarthManipulator::getWorldInverseMatrix() const
{
    return osg::Matrixd::translate(-_center)*
           osg::Matrixd::rotate   (_centerRotation.inverse()) *
           osg::Matrixd::translate(-_posOffset) *
           osg::Matrixd::rotate   (_tetherRotation.inverse()) *
           osg::Matrixd::rotate   (_rotation.inverse()) *
           osg::Matrixd::translate(-_viewOffset.x(), -_viewOffset.y(), -_distance);
}

void
EarthManipulator::updateCamera(osg::Camera& camera)
{
    // time exactly now
    double now = osg::Timer::instance()->time_s();

    // interpolation through a setViewpoint, if applicable
    double t = 1.0;

    // Update a viewpoint transition:
    if (isSettingViewpoint())
    {
        t = setViewpointFrame(now);
    }

    // Update a camera tethered to a node:
    if (isTethering())
    {
        updateTether(t);
    }

    // Update an ortho/perspective projection tracking:
    updateProjection(&camera);


    osgGA::CameraManipulator::updateCamera(camera);

    // Invoke the callback once the camera's been udpated for the ensuing render:
    if (_updateCameraCallback.valid())
    {
        _updateCameraCallback->onUpdateCamera(&camera);
    }
}

void
EarthManipulator::setUpdateCameraNodeVisitor(osg::NodeVisitor* nv)
{
    _updateCameraNodeVisitor = nv;
}

void
EarthManipulator::setByLookAt(const osg::Vec3d& eye,const osg::Vec3d& center,const osg::Vec3d& up)
{
    osg::ref_ptr<osg::Node> safeNode;
    if ( _node.lock(safeNode) )
    {
        // compute rotation matrix
        osg::Vec3d lv(center-eye);
        setDistance( lv.length() );
        setCenter( center );

        bool hitFound = false;

        double distance = lv.length();
        double maxDistance = distance+2*(eye-safeNode->getBound().center()).length();
        osg::Vec3d farPosition = eye+lv*(maxDistance/distance);
        osg::Vec3d endPoint = center;
        for(int i=0;
            !hitFound && i<2;
            ++i, endPoint = farPosition)
        {
            // compute the intersection with the scene.

            osg::Vec3d ip, normal;
            if (intersect(eye, endPoint, ip, normal))
            {
                setCenter( ip );
                setDistance( (ip-eye).length() );
                hitFound = true;
            }
        }
    }

    // note LookAt = inv(CF)*inv(RM)*inv(T) which is equivalent to:
    // inv(R) = CF*LookAt.

    osg::Matrixd rotation_matrix = osg::Matrixd::lookAt(eye,center,up);

    _centerRotation = computeCenterRotation(_center);
    _rotation = rotation_matrix.getRotate().inverse() * _centerRotation.inverse();

    _previousUp = getUpVector(_centerLocalToWorld);

    _posOffset.set(0,0,0);
    _viewOffset.set(0,0);

    recalculateRoll();
}

void
EarthManipulator::setByLookAtRaw(const osg::Vec3d& eye,const osg::Vec3d& center,const osg::Vec3d& up)
{
    // compute rotation matrix
    osg::Vec3d lv(center-eye);
    setDistance( lv.length() );
    setCenter( center );
    _posOffset.set(0,0,0);
    _viewOffset.set(0,0);

    osg::Matrixd rotation_matrix = osg::Matrixd::lookAt(eye,center,up);
    _centerRotation = computeCenterRotation(_center); // getRotation(_center).getRotate().inverse();
    _rotation = rotation_matrix.getRotate().inverse() * _centerRotation.inverse();
    _previousUp = getUpVector(_centerLocalToWorld);

    recalculateRoll();
}


bool
EarthManipulator::recalculateCenterFromLookVector()
{
    // just re-applying the lookat parameters will calculate a new coordinate
    // frame based on a look-at intersection.
    osg::Vec3d eye, target, up;
    bool intersected = intersectLookVector(eye, target, up);
    if (intersected)
    {
        setByLookAtRaw(eye, target, up);
    }

    return intersected;
}


void
EarthManipulator::recalculateCenter( const osg::CoordinateFrame& frame )
{
    osg::ref_ptr<osg::Node> node;
    if ( _node.lock(node) )
    {
        // need to reintersect with the terrain
        double ilen = node->getBound().radius()*0.25f;

        osg::Vec3d up = getUpVector(frame);

        osg::Vec3d ip1;
        osg::Vec3d ip2;
        osg::Vec3d normal;
        bool hit_ip1 = intersect(_center - up * ilen * 0.1, _center + up * ilen, ip1, normal);
        bool hit_ip2 = intersect(_center + up * ilen * 0.1, _center - up * ilen, ip2, normal);
        if (hit_ip1)
        {
            if (hit_ip2)
            {
                setCenter( (_center-ip1).length2() < (_center-ip2).length2() ? ip1 : ip2 );
            }
            else
            {
                setCenter( ip1 );
            }
        }
        else if (hit_ip2)
        {
            setCenter( ip2 );
        }
    }
}


void
EarthManipulator::pan( double dx, double dy )
{
    if ( !isTethering() )
    {
        // to pan, we need a focus point on the terrain:
        if ( !recalculateCenterFromLookVector() )
            return;

        double scale = -0.3f*_distance;

        osg::Matrixd rotation_matrix;
        rotation_matrix.makeRotate( _rotation * _centerRotation  );

        // compute look vector.
        osg::Vec3d lookVector = -getUpVector(rotation_matrix);
        osg::Vec3d sideVector = getSideVector(rotation_matrix);
        osg::Vec3d upVector = getFrontVector(rotation_matrix);

        osg::Vec3d localUp = _previousUp;

        osg::Vec3d forwardVector =localUp^sideVector;
        sideVector = forwardVector^localUp;

        forwardVector.normalize();
        sideVector.normalize();

        osg::Vec3d dv = forwardVector * (dy*scale) + sideVector * (dx*scale);

        // save the previous CF so we can do azimuth locking:
        osg::CoordinateFrame oldCenterLocalToWorld = _centerLocalToWorld;

        // move the center point
        double len = _center.length();
        osg::Vec3d newCenter = _center + dv;

        if ( _srs->isGeographic() )
        {
            // in geocentric, ensure that it doesn't change length.
            newCenter.normalize();
            newCenter *= len;
        }
        setCenter( newCenter );

        if ( _settings->getLockAzimuthWhilePanning() )
        {
            // in azimuth-lock mode, _centerRotation maintains a consistent north vector
            _centerRotation = computeCenterRotation( _center );
        }

        else
        {
            // otherwise, we need to rotate _centerRotation manually.
            osg::Vec3d new_localUp = getUpVector( _centerLocalToWorld );

            osg::Quat pan_rotation;
            pan_rotation.makeRotate( localUp, new_localUp );

            if ( !pan_rotation.zeroRotation() )
            {
                _centerRotation = _centerRotation * pan_rotation;
                _previousUp = new_localUp;
            }
        }
    }
    else
    {
        double scale = _distance;

        // Panning in tether mode changes the focal view offsets.
        _viewOffset.x() -= dx * scale;
        _viewOffset.y() -= dy * scale;

        //Clamp values within range
        _viewOffset.x() = osg::clampBetween( _viewOffset.x(), -_settings->getMaxXOffset(), _settings->getMaxXOffset() );
        _viewOffset.y() = osg::clampBetween( _viewOffset.y(), -_settings->getMaxYOffset(), _settings->getMaxYOffset() );
    }

    collisionDetect();
}

void
EarthManipulator::rotate( double dx, double dy )
{
    // clamp the local pitch delta; never allow the pitch to hit -90.
    double minp = osg::DegreesToRadians( osg::clampAbove(_settings->getMinPitch(), -89.9) );
    double maxp = osg::DegreesToRadians( osg::clampBelow(_settings->getMaxPitch(),  89.9) );

    // clamp pitch range:
    double oldPitch;
    getEulerAngles( _rotation, 0L, &oldPitch );

    if ( dy + oldPitch > maxp || dy + oldPitch < minp )
        dy = 0;

    osg::Matrix rotation_matrix;
    rotation_matrix.makeRotate(_rotation);

    osg::Vec3d lookVector = -getUpVector(rotation_matrix);
    osg::Vec3d sideVector = getSideVector(rotation_matrix);
    osg::Vec3d upVector = getFrontVector(rotation_matrix);

    osg::Vec3d localUp(0.0f,0.0f,1.0f);

    osg::Vec3d forwardVector = localUp^sideVector;
    sideVector = forwardVector^localUp;

    forwardVector.normalize();
    sideVector.normalize();

    osg::Quat rotate_elevation;
    rotate_elevation.makeRotate(dy,sideVector);

    osg::Quat rotate_azim;
    rotate_azim.makeRotate(-dx,localUp);

    _rotation = _rotation * rotate_elevation * rotate_azim;
    collisionDetect();
}

void
EarthManipulator::zoom( double dx, double dy, osg::View* in_view )
{
    if (isTethering())
    {
        double scale = 1.0f + dy;
        setDistance( _distance * scale );
        collisionDetect();
        return;
    }

    // When _ga_t0 == nullptr, that means the action came from a non-mouse event
    // (like a keypress) in which case we'll bypass zoom-to-mouse.
    if (_settings->getZoomToMouse() == false || in_view == NULL || _ga_t0 == nullptr)
    {
        recalculateCenterFromLookVector();
        double scale = 1.0f + dy;
        setDistance( _distance * scale );
        collisionDetect();
        return;
    }

    // Zoom to mouseish

    osgViewer::View* view = dynamic_cast<osgViewer::View*>(in_view);
    if ( !view )
        return;

    if (_ga_t0 == NULL)
        return;

    float x = _ga_t0->getX(), y = _ga_t0->getY();
    float local_x, local_y;

    const osg::Camera* camera = view->getCameraContainingPosition(x, y, local_x, local_y);
    if (!camera)
        camera = view->getCamera();

    if ( !camera )
        return;

    // reset the "remembered start location" if we're just starting a continuous zoom
    static osg::Vec3d zero(0,0,0);
    if (_last_action._type != ACTION_ZOOM)
        _lastPointOnEarth = zero;

    osg::Vec3d target;

    bool onEarth = true;
    if (_lastPointOnEarth != zero)
    {
        // Use the start location (for continuous zoom)
        target = _lastPointOnEarth;
    }
    else
    {
        // Zoom just started; calculate a start location
        onEarth = screenToWorld(x, y, view, target);
    }

    if (onEarth)
    {
        _lastPointOnEarth = target;

        if (_srs.valid() && _srs->isGeographic())
        {
            // globe

            // Calcuate a rotation that we'll use to interpolate from our center point to the target
            osg::Quat rotCenterToTarget;
            rotCenterToTarget.makeRotate(_center, target);

            // Factor by which to scale the distance:
            double scale = 1.0f + dy;
            double newDistance = _distance*scale;
            double delta = _distance - newDistance;
            double ratio = delta/_distance;

            // xform target point into the current focal point's local frame,
            // and adjust the zoom ratio to account for the difference in
            // target distance based on the earth's curvature...approximately!
            osg::Vec3d targetInLocalFrame = _centerRotation.conj()*target;
            double crRatio = _center.length() / targetInLocalFrame.z();
            ratio *= crRatio;

            // Interpolate a new focal point:
            osg::Quat rot;
            rot.slerp(ratio, osg::Quat(), rotCenterToTarget);
            setCenter(rot*_center);

            // recompute the local frame:
            _centerRotation = computeCenterRotation(_center);

            // and set the new zoomed distance.
            setDistance(newDistance);

            collisionDetect();
        }
        else
        {
            // projected map. This will a simple linear interpolation
            // of the eyepoint along the path between the eye and the target.
            recalculateCenterFromLookVector();

            osg::Vec3d eye, at, up;
            getWorldInverseMatrix().getLookAt(eye, at, up);

            osg::Vec3d eyeToTargetVec = target-eye;
            eyeToTargetVec.normalize();

            double scale = 1.0f + dy;
            double newDistance = _distance*scale;
            double delta = _distance - newDistance;
            double ratio = delta/_distance;

            osg::Vec3d newEye = eye + eyeToTargetVec*delta;

            setByLookAt(newEye, newEye+(at-eye), up);
        }
    }

    else
    {
        // if the user's mouse isn't over the earth, just zoom in to the center of the screen
        recalculateCenterFromLookVector();
        double scale = 1.0f + dy;
        setDistance(_distance * scale);
        collisionDetect();
    }
}


bool
EarthManipulator::screenToWorld(float x, float y, osg::View* theView, osg::Vec3d& out_coords) const
{
    osgViewer::View* view = dynamic_cast<osgViewer::View*>( theView );
    if ( !view || !view->getCamera() )
        return false;

    osg::ref_ptr<MapNode> mapNode;
    if ( !_mapNode.lock(mapNode) || !mapNode->getTerrain() )
        return false;

    return mapNode->getTerrain()->getWorldCoordsUnderMouse(view, x, y, out_coords);
}


void
EarthManipulator::setDistance( double distance )
{
    _distance = osg::clampBetween( distance, _settings->getMinDistance(), _settings->getMaxDistance() );
}

void
EarthManipulator::setInitialVFOV(double vfov)
{
    _lastKnownVFOV = vfov;
}

void
EarthManipulator::dumpActionInfo( const EarthManipulator::Action& action, osg::NotifySeverity level ) const
{
    osgEarth::notify(level) << "action: " << s_actionNames[action._type] << "; options: ";
    for( ActionOptions::const_iterator i = action._options.begin(); i != action._options.end(); ++i )
    {
        const ActionOption& option = *i;
        std::string val;
        if (s_actionOptionTypes[option.option()] == 0)
            val = option.boolValue() ? "true" : "false";
        else
            val = std::to_string(option.doubleValue());

        osgEarth::notify(level)
            << s_actionOptionNames[option.option()] << "=" << val << ", ";
    }
    osgEarth::notify(level) << std::endl;
}

void
EarthManipulator::handleMovementAction( const ActionType& type, double dx, double dy, osg::View* view )
{
    switch( type )
    {
    case ACTION_PAN:
        pan( dx, dy );
        break;

    case ACTION_ROTATE:
        // in "single axis" mode, zero out one of the deltas.
        if ( _continuous && _settings->getSingleAxisRotation() )
        {
            if ( ::fabs(dx) > ::fabs(dy) )
                dy = 0.0;
            else
                dx = 0.0;
        }
        rotate( dx, dy );
        break;

    case ACTION_ZOOM:
        zoom( dx, dy, view );
        break;

    case ACTION_EARTH_DRAG:
        if (_thrown)
          pan(dx*0.5, dy*0.5);
        else
          drag( dx, dy, view );
        break;
    default:break;
    }
}

bool
EarthManipulator::handlePointAction( const Action& action, float mx, float my, osg::View* view )
{
    //Exit early if the action is null
    if (action._type == ACTION_NULL)
        return true;

    osg::Vec3d point;
    if ( screenToWorld( mx, my, view, point ))
    {
        switch( action._type )
        {
            case ACTION_GOTO:
            {
                Viewpoint here = getViewpoint();
                here.focalPoint().mutable_value().fromWorld(_srs.get(), point);

                //osg::Vec3d pointVP;
                //here.getSRS()->transformFromWorld(point, pointVP);

                //OE_NOTICE << "X=" << pointVP.x() << ", Y=" << pointVP.y() << std::endl;

//                here.setFocalPoint( pointVP );

                double duration_s = action.getDoubleOption(OPTION_DURATION, 1.0);
                double range_factor = action.getDoubleOption(OPTION_GOTO_RANGE_FACTOR, 1.0);

                here.range() = here.range().get() * range_factor;
                //here.setRange( here.getRange() * range_factor );

                setViewpoint( here, duration_s );
            }
            break;
            default:
            break;
        }
    }
    return true;
}

void
EarthManipulator::handleContinuousAction( const Action& action, osg::View* view )
{
    double t_factor = (_time_s_now - _last_continuous_action_time) * 60.0;
    _last_continuous_action_time = _time_s_now;
    handleMovementAction( action._type, _continuous_dx * t_factor, _continuous_dy * t_factor, view );
}

void
EarthManipulator::applyOptionsToDeltas( const Action& action, double& dx, double& dy )
{
    dx *= action.getDoubleOption( OPTION_SCALE_X, 1.0 );
    dy *= action.getDoubleOption( OPTION_SCALE_Y, 1.0 );

    if ( action.getBoolOption( OPTION_SINGLE_AXIS, false ) == true )
    {
        if ( osg::absolute(dx) > osg::absolute(dy) )
            dy = 0.0;
        else
            dx = 0.0;
    }
}

bool
EarthManipulator::handleMouseAction( const Action& action, osg::View* view )
{
    // return if less then two events have been added.
    if (_ga_t0.get()==NULL || _ga_t1.get()==NULL) return false;

    //if ( osgEarth::getNotifyLevel() > osg::INFO )
    //    dumpActionInfo( action, osg::DEBUG_INFO );

    double dx = _ga_t0->getXnormalized()-_ga_t1->getXnormalized();
    double dy = _ga_t0->getYnormalized()-_ga_t1->getYnormalized();

    // return if there is no movement.
    if (dx==0 && dy==0) return false;

    // here we adjust for action scale, global sensitivy
    dx *= _settings->getMouseSensitivity();
    dy *= _settings->getMouseSensitivity();

    applyOptionsToDeltas( action, dx, dy );

    // in "continuous" mode, we accumulate the deltas each frame - thus
    // the deltas act more like speeds.
    if ( _continuous )
    {
        _continuous_dx += dx * 0.01;
        _continuous_dy += dy * 0.01;
    }
    else
    {

        _dx = dx;
        _dy = dy;
        handleMovementAction( action._type, dx, dy, view );
    }

    return true;
}

bool
EarthManipulator::handleMouseClickAction( const Action& action )
{
    return false;
}

bool
EarthManipulator::handleKeyboardAction( const Action& action, double duration )
{
    double dx = 0, dy = 0;

    switch( action._dir )
    {
    case DIR_LEFT:  dx =  1; break;
    case DIR_RIGHT: dx = -1; break;
    case DIR_UP:    dy = -1; break;
    case DIR_DOWN:  dy =  1; break;
    default: break;
    }

    dx *= _settings->getKeyboardSensitivity();
    dy *= _settings->getKeyboardSensitivity();

    applyOptionsToDeltas( action, dx, dy );

    return handleAction( action, dx, dy, duration );
}

bool
EarthManipulator::handleScrollAction( const Action& action, double duration )
{
    const double scrollFactor = 1.5;

    double dx = 0, dy = 0;

    switch( action._dir )
    {
    case DIR_LEFT:  dx =  1; break;
    case DIR_RIGHT: dx = -1; break;
    case DIR_UP:    dy = -1; break;
    case DIR_DOWN:  dy =  1; break;
    default: break;
    }

    dx *= scrollFactor * _settings->getScrollSensitivity();
    dy *= scrollFactor * _settings->getScrollSensitivity();

    applyOptionsToDeltas( action, dx, dy );

    return handleAction( action, dx, dy, duration );
}

bool
EarthManipulator::handleAction( const Action& action, double dx, double dy, double duration )
{
    bool handled = true;

    //if ( osgEarth::getNotifyLevel() > osg::INFO )
    //    dumpActionInfo( action, osg::DEBUG_INFO );

    //OE_NOTICE << "action=" << action << ", dx=" << dx << ", dy=" << dy << std::endl;

    switch( action._type )
    {
    case ACTION_HOME:
        if ( _homeViewpoint.isSet() )
        {
            setViewpoint( _homeViewpoint.value(), _homeViewpointDuration );
        }
        break;


    case ACTION_PAN:
    case ACTION_PAN_LEFT:
    case ACTION_PAN_RIGHT:
    case ACTION_PAN_UP:
    case ACTION_PAN_DOWN:
        _task->set( TASK_PAN, dx, dy, duration, _time_s_now );
        break;

    case ACTION_ROTATE:
    case ACTION_ROTATE_LEFT:
    case ACTION_ROTATE_RIGHT:
    case ACTION_ROTATE_UP:
    case ACTION_ROTATE_DOWN:
        _task->set( TASK_ROTATE, dx, dy, duration, _time_s_now );
        break;

    case ACTION_ZOOM:
    case ACTION_ZOOM_IN:
    case ACTION_ZOOM_OUT:
        _task->set( TASK_ZOOM, dx, dy, duration, _time_s_now );
        break;

    default:
        handled = false;
    }

    return handled;
}

void
EarthManipulator::recalculateRoll()
{
    osg::Matrixd rotation_matrix;
    rotation_matrix.makeRotate(_centerRotation);

    osg::Vec3d lookVector = -getUpVector(rotation_matrix);
    osg::Vec3d upVector = getFrontVector(rotation_matrix);

    osg::Vec3d localUp = getUpVector(_centerLocalToWorld);

    osg::Vec3d sideVector = lookVector ^ localUp;

    if (sideVector.length()<0.1)
    {
        sideVector = upVector^localUp;
        sideVector.normalize();

    }

    osg::Vec3d newUpVector = sideVector^lookVector;
    newUpVector.normalize();

    osg::Quat rotate_roll;
    rotate_roll.makeRotate(upVector,newUpVector);

    if (!rotate_roll.zeroRotation())
    {
        _centerRotation = _centerRotation * rotate_roll;
    }
}

void
EarthManipulator::getCompositeEulerAngles( double* out_azim, double* out_pitch ) const
{
    osg::Matrix m = getWorldMatrix() * osg::Matrixd::inverse(_centerLocalToWorld);
    osg::Vec3d look = -getUpVector( m );
    osg::Vec3d up   =  getFrontVector( m );

    look.normalize();
    up.normalize();

    if ( out_azim )
    {
        if ( look.z() < -0.9 )
            *out_azim = atan2( up.x(), up.y() );
        else if ( look.z() > 0.9 )
            *out_azim = atan2( -up.x(), -up.y() );
        else
            *out_azim = atan2( look.x(), look.y() );

        *out_azim = normalizeAzimRad( *out_azim );
    }

    if ( out_pitch )
    {
        *out_pitch = asin( look.z() );
    }
}


// Extracts azim and pitch from a quaternion that does not contain any roll.
void
EarthManipulator::getEulerAngles(const osg::Quat& q, double* out_azim, double* out_pitch) const
{
    osg::Matrix m( q );

    osg::Vec3d look = -getUpVector( m );
    osg::Vec3d up   =  getFrontVector( m );

    look.normalize();
    up.normalize();

    if ( out_azim )
    {
        if ( look.z() < -0.9 )
            *out_azim = atan2( up.x(), up.y() );
        else if ( look.z() > 0.9 )
            *out_azim = atan2( -up.x(), -up.y() );
        else
            *out_azim = atan2( look.x(), look.y() );

        *out_azim = normalizeAzimRad( *out_azim );
    }

    if ( out_pitch )
    {
        *out_pitch = asin( look.z() );
    }
}

osg::Quat
EarthManipulator::getQuaternion(double azim, double pitch) const
{
    osg::Quat azim_q (  azim,            osg::Vec3d(0,0,1) );
    osg::Quat pitch_q( -pitch-osg::PI_2, osg::Vec3d(1,0,0) );
    osg::Matrix newRot = osg::Matrixd( azim_q * pitch_q );
    return osg::Matrixd::inverse(newRot).getRotate();
}

void
EarthManipulator::collapseTetherRotationIntoRotation()
{
    // fetch the composite rotation angles (_rotation and _tetherRotation):

    double azim, pitch;
    getCompositeEulerAngles(&azim, &pitch);

    pitch = osg::clampBetween(
        pitch,
        osg::DegreesToRadians(_settings->getMinPitch()),
        osg::DegreesToRadians(_settings->getMaxPitch()) );

    _rotation = getQuaternion(azim, pitch);

    _tetherRotation = osg::Quat();
    _tetherRotationOffset.unset();
}


void
EarthManipulator::setHomeViewpoint( const Viewpoint& vp, double duration_s )
{
    _homeViewpoint = vp;
    _homeViewpointDuration = duration_s;
}


namespace // Utility functions for drag()
{
    // Find the point on a line, specified by p1 and v, closest to another
    // point.
    osg::Vec3d closestPtOnLine(const osg::Vec3d& p1, const osg::Vec3d& v,
                               const osg::Vec3d& p)
    {
        double u = (p - p1) * v / v.length2();
        return p1 + v * u;
    }

    // Intersection of line and plane
    bool findIntersectionWithPlane(const osg::Vec3d& normal, const osg::Vec3d& pt,
                                   const osg::Vec3d& p1, const osg::Vec3d& v,
                                   osg::Vec3d& result)
    {
        double denom = normal * v;
        if (osg::equivalent(0, denom))
            return false;
        double u = normal * (pt - p1) / denom;
        result = p1 + v * u;
        return true;
    }

    // Circle of intersection of two spheres. The circle is in the plane
    // normal to the line between the centers.
    bool sphereInterection(const osg::Vec3d& p0, double r0,
                           const osg::Vec3d& p1, double r1,
                           osg::Vec3d& resultCenter, double& r)
    {
        using namespace osg;
        Vec3d ptvec = (p1 - p0);
        double d = ptvec.normalize();
        if (d > r0 + r1)
            return false;               // spheres are too far apart
        else if (d < fabs(r0 - r1))
            return false;               // One sphere is contained in the other
        else if (equivalent(0, d) && equivalent(r0, r1))
        {
            resultCenter = p0;
            r = r0;
            return true;              // circles are coincident.
        }
        // distance from p0 to the line through the interection points
        double a = (r0 * r0 - r1 * r1 + d * d) / (2 * d);
        // distance from bisection of that line to the intersections
        resultCenter = p0 + ptvec * a;
        r = sqrt(r0 * r0 - a * a);
        return true;
    }

    // Find a point on the sphere (center, radius) through which the tangent
    // through pt passes. The point lies in the plane defined by
    //the line pt->center and ray.
    osg::Vec3d calcTangentPoint(const osg::Vec3d& pt, const osg::Vec3d& center,
                                double radius, const osg::Vec3d& ray)
    {
        using namespace osg;
        // new sphere with center at midpoint between pt and input sphere
        Vec3d center2 = (pt + center) / 2.0;
        double radius2 = (pt - center2).length();
        Vec3d resCtr;
        double resRad;
        // Use Thales' theorem, which states that a triangle inscribed in
        // a circle, with two points on a diameter of the circle and the
        // third on the circle, is a right triangle. Since one endpoint is
        // the center of the original sphere (the earth) and the other is
        // pt, we can get our tangent from that.
        bool valid = sphereInterection(center, radius, center2, radius2, resCtr,
                                       resRad);
        if (!valid)
            return Vec3d(0.0, 0.0, 0.0);
        // Get the tangent point that lies in the plane of the ray and the
        // center line. The sequence of cross products gives us the point
        // that is closest to the ray, rather than the one on the other
        // side of the sphere.
        Vec3d toCenter = center - pt;
        toCenter.normalize();
        Vec3d normal = ray ^ toCenter;
        normal.normalize();
        Vec3d radial = toCenter ^ normal;
        radial = radial * resRad;
        Vec3d result = resCtr + radial;
        return result;

    }
    // Calculate a pointer click in eye coordinates
    osg::Vec3d getWindowPoint(osgViewer::View* view, float x, float y)
    {
        float local_x, local_y;
        const osg::Camera* camera
            = view->getCameraContainingPosition(x, y, local_x, local_y);
        if (!camera)
            camera = view->getCamera();
        osg::Matrix winMat;
        if (camera->getViewport())
            winMat = camera->getViewport()->computeWindowMatrix();
        osg::Matrix projMat = camera->getProjectionMatrix();
        // ray from eye through pointer in camera coordinate system goes
        // from origin through transformed pointer coordinates
        osg::Matrix win2camera = projMat * winMat;
        win2camera.invert(win2camera);
        osg::Vec4d winpt4 = osg::Vec4d(x, y, 0.0, 1.0) * win2camera;
        winpt4 = winpt4 / winpt4.w();
        return osg::Vec3d(winpt4.x(), winpt4.y(), winpt4.z());
    }

    // Decompose  _center and _centerRotation into a longitude rotation
    // and a latitude rotation + translation in the longitudinal plane.

    void decomposeCenter(const osg::Vec3d& center, const osg::Quat& centerRotation,
                         osg::Matrix& Me, osg::Matrix& Mlon)
    {
        Mlon.makeIdentity();
        osg::Matrix Mtotal(centerRotation);
        Mtotal.setTrans(center);
        // Use the X axis to determine longitude rotation. Due to the
        // OpenGL camera rotation, this axis will be the Y axis of the
        // longitude matrix.
        Mlon(1, 0) = Mtotal(0, 0);  Mlon(1, 1) = Mtotal(0, 1);
        // X axis is rotated 90 degrees, obviously
        Mlon(0, 0) = Mlon(1, 1);  Mlon(0, 1) = -Mlon(1, 0);
        osg::Matrix MlonInv = osg::Matrixd::inverse(Mlon);
        Me = Mtotal * MlonInv;
    }

    osg::Matrixd rotateAroundPoint(const osg::Vec3d& pt, double theta,
                                   const osg::Vec3d& axis)
    {
        return (osg::Matrixd::translate(pt)
                * osg::Matrixd::rotate(theta, axis)
                * osg::Matrixd::translate(pt * -1.0));
    }
}

// Theory of operation for the manipulator drag motion
//
// The mouse drag is transformed to a vector on the surface of the
// earth i.e., in the surface plane at the start of the drag. This is
// treated as a displacement along the arc of a great circle. The
// earth will be rotated by the equivalent rotation around the axis of
// the circle. However, the manipulator controls the camera, not the
// earth, so the camera's placement matrix (inverse view matrix)
// should be rotated by the inverse of the calculated
// rotation. EarthManipulator represents the placement matrix as the
// concatenation of 4 transformations: distance from focal point,
// local heading and pitch, rotation to frame of focal point, focal
// point. To change the camera placement we rotate the frame rotation
// (_centerRotation) and focal point (_center).
//
// When the start or end drag click is not on the earth, we choose the
// nearest tangent point on the earth to the ray from the eye and
// proceed.

void
EarthManipulator::drag(double dx, double dy, osg::View* theView)
{
    osgViewer::View* view = dynamic_cast<osgViewer::View*>(theView);
    if ( !view )
        return;

    const osg::Vec3d zero(0.0, 0.0, 0.0);
    if (_last_action._type != ACTION_EARTH_DRAG)
        _lastPointOnEarth = zero;

    double radiusEquator = _srs.valid() ? _srs->getEllipsoid().getRadiusEquator() : 6378137.0;

    float x = _ga_t0->getX(), y = _ga_t0->getY();
    float local_x, local_y;

    const osg::Camera* camera = view->getCameraContainingPosition(x, y, local_x, local_y);
    if (!camera)
        camera = view->getCamera();

    if ( !camera )
        return;

    osg::Matrix viewMat = camera->getViewMatrix();
    osg::Matrix viewMatInv = camera->getInverseViewMatrix();
    if (!_ga_t1.valid())
        return;

    osg::Vec3d worldStartDrag;
    // drag start in camera coordinate system.
    bool onEarth = screenToWorld(_ga_t1->getX(), _ga_t1->getY(), view, worldStartDrag);
    if (onEarth)
    {
        if (_lastPointOnEarth == zero)
            _lastPointOnEarth = worldStartDrag;
        else
            worldStartDrag = _lastPointOnEarth;
    }
    else if (_srs->isGeographic())
    {
        if (_lastPointOnEarth != zero)
        {
            worldStartDrag =_lastPointOnEarth;
        }
        else if (_srs.valid())
        {
            const osg::Vec3d startWinPt = getWindowPoint(view, _ga_t1->getX(), _ga_t1->getY());
            const osg::Vec3d startDrag = calcTangentPoint(
                zero, zero * viewMat, radiusEquator,
                startWinPt);
            worldStartDrag = startDrag * viewMatInv;
        }
    }
    else
        return;
    // ray from eye through pointer in camera coordinate system goes
    // from origin through transformed pointer coordinates
    const osg::Vec3d winpt = getWindowPoint(view, x, y);
    // Find new point to which startDrag has been moved
    osg::Vec3d worldEndDrag;
    osg::Quat worldRot;
    bool endOnEarth = screenToWorld(x, y, view, worldEndDrag);
    if (endOnEarth)
    {
        // OE_WARN << "end drag: " << worldEndDrag << "\n";
    }
    else
    {
        osg::Vec3d earthOrigin = zero * viewMat;
        const osg::Vec3d endDrag = calcTangentPoint(zero, earthOrigin, radiusEquator, winpt);
        worldEndDrag = endDrag * viewMatInv;
    }

    if (_srs->isGeographic())
    {
        worldRot.makeRotate(worldStartDrag, worldEndDrag);
        // Move the camera by the inverse rotation
        osg::Quat cameraRot = worldRot.conj();
        // Derive manipulator parameters from the camera matrix. We
        // can't use _center, _centerRotation, and _rotation directly
        // from the manipulator because they may have been updated
        // already this frame while the camera view matrix,
        // used to do the terrain intersection, has not. This happens
        // when several mouse movement events arrive in a frame. there
        // will be bad stuttering artifacts if we use the updated
        // manipulator parameters.
        osg::Matrixd Mmanip = osg::Matrixd::translate(-_viewOffset.x(), -_viewOffset.y(), -_distance) * viewMatInv;
        osg::Vec3d center = Mmanip.getTrans();
        osg::Quat centerRotation = computeCenterRotation(center);
        osg::Matrixd Mrotation = (Mmanip * osg::Matrixd::translate(center * -1)
                             * osg::Matrixd::rotate(centerRotation.inverse()));
        osg::Matrixd Me = osg::Matrixd::rotate(centerRotation)
            * osg::Matrixd::translate(center) * osg::Matrixd::rotate(cameraRot);
        // In order for the Viewpoint settings to make sense, the
        // inverse camera matrix must not have a roll component, which
        // implies that its x axis remains parallel to the
        // z = 0 plane. The strategy for doing that is different if
        // the azimuth is locked.
        // Additionally, the part of the camera rotation defined by
        // _centerRotation must be oriented with the local frame of
        // _center on the ellipsoid. For the purposes of the drag
        // motion this is nearly identical to the frame obtained by
        // the trackball motion, so we just fix it up at the end.
        if (_settings->getLockAzimuthWhilePanning())
        {
            // The camera needs to be rotated that _centerRotation
            // is a rotation only around the global Z axis and the
            // camera frame X axis. We don't change _rotation, so that
            // azimuth and pitch will stay constant, but the drag must
            // still be correct i.e.,  the point dragged must remain
            // under the cursor. Therefore the rotation must be around the
            // point that was dragged, worldEndDrag.
            //
            // Rotate Me so that its x axis is parallel to the z=0
            // plane.
            // Find cone with worldEndDrag->center axis and x
            // axis of coordinate frame as generator of the conical
            // surface.
            osg::Vec3d coneAxis = worldEndDrag * -1;
            coneAxis.normalize();
            osg::Vec3d xAxis(Me(0, 0), Me(0, 1), Me(0, 2));
            // Center of disk: project xAxis onto coneAxis
            double diskDist = xAxis * coneAxis;
            osg::Vec3d P1 = coneAxis * diskDist;
            // Basis of disk equation:
            // p = P1 + R * r * cos(theta) + S * r * sin(theta)
            osg::Vec3d R = xAxis - P1;
            osg::Vec3d S = R ^ coneAxis;
            double r = R.normalize();
            S.normalize();
            // Solve for angle that rotates xAxis into z = 0 plane.
            // soln to 0 = P1.z + r cos(theta) R.z + r sin(theta) S.z
            double temp1 = r * (osg::square(S.z()) + osg::square(R.z()));
            if (osg::equivalent(temp1, 0.0))
                return;
            double radical = r * temp1 - osg::square(P1.z());
            if (radical < 0)
                return;
            double temp2 = R.z() * sqrt(radical) / temp1;
            double temp3 = S.z() * P1.z() / temp1;
            double sin1 = temp2 + temp3;
            double sin2 = temp2 - temp3;
            double theta1 = DBL_MAX;
            double theta2 = DBL_MAX;
            osg::Matrixd cm1, cm2;
            if (fabs(sin1) <= 1.0)
            {
                theta1 = -asin(sin1);
                osg::Matrixd m = rotateAroundPoint(worldEndDrag, -theta1, coneAxis);
                cm1 = Me * m;
            }
            if (fabs(sin2) <= 1.0)
            {
                theta2 = asin(sin2);
                osg::Matrix m = rotateAroundPoint(worldEndDrag, -theta2, coneAxis);
                cm2 = Me * m;
            }
            if (theta1 == DBL_MAX && theta2 == DBL_MAX)
                return;
            osg::Matrixd* CameraMat = 0;
            if (theta1 != DBL_MAX && cm1(1, 2) >= 0.0)
                CameraMat = &cm1;
            else if (theta2 != DBL_MAX && cm2(1, 2) >= 0.0)
                CameraMat = &cm2;
            else
                return;

            setCenter( CameraMat->getTrans() );
        }
        else
        {
            // The camera matrix must be rotated around the local Z axis so
            // that the X axis is parallel to the global z = 0
            // plane. Then, _rotation is rotated by the inverse
            // rotation to preserve the total transformation.
            double theta = atan2(-Me(0, 2), Me(1, 2));
            double s = sin(theta), c = cos(theta);
            if (c * Me(1, 2) - s * Me(0, 2) < 0.0)
            {
                s = -s;
                c = -c;
            }
            osg::Matrixd m(c, s, 0, 0,
                      -s, c, 0, 0,
                      0, 0, 1, 0,
                      0, 0, 0, 1);
            osg::Matrixd CameraMat = m * Me;
            setCenter( CameraMat.getTrans() );
            // It's not necessary to include the translation
            // component, but it's useful for debugging.
            osg::Matrixd headMat
                = (osg::Matrixd::translate(_viewOffset.x(), _viewOffset.y(), _distance)
                * Mrotation);
            headMat = headMat * osg::Matrixd::inverse(m);
            _rotation = headMat.getRotate();
            //recalculateLocalPitchAndAzimuth();
        }
        _centerRotation = computeCenterRotation(_center);

        _previousUp = getUpVector(_centerLocalToWorld);
    }
    else
    {
        // This is obviously not correct.
        setCenter( _center + (worldStartDrag - worldEndDrag) );
    }
}

