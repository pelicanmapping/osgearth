/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/NavigationManipulators>
#include <osgEarth/Terrain>
#include <osgViewer/View>
#include <algorithm>
#include <cmath>

using namespace osgEarth;

namespace
{
    /** Combine shoulder/keyboard speed modifiers without changing the configured speed. */
    double speedScale(const GamepadState& input)
    {
        return (input.slow ? 0.25 : 1.0) * (input.fast ? 4.0 : 1.0);
    }

    /** Bound a movement vector so diagonal travel does not exceed the requested speed. */
    osg::Vec3d unitLimited(osg::Vec3d value)
    {
        if (value.length2() > 1.0)
            value.normalize();
        return value;
    }

    /** Recognize movement keys while leaving application shortcuts available. */
    bool navigationKey(int key)
    {
        using E = osgGA::GUIEventAdapter;
        return key == 'w' || key == 'a' || key == 's' || key == 'd' || key == 'q' || key == 'e' ||
            key == E::KEY_Left || key == E::KEY_Right || key == E::KEY_Up || key == E::KEY_Down ||
            key == E::KEY_Shift_L || key == E::KEY_Shift_R || key == E::KEY_Control_L || key == E::KEY_Control_R;
    }
}

NavigationManipulator::NavigationManipulator(double speed, double eyeHeight) :
    _speed(speed), _eyeHeight(eyeHeight)
{
    setAutoComputeHomePosition(false);
}

void NavigationManipulator::setNode(osg::Node* node)
{
    _node = node;
    _map = node ? MapNode::get(node) : nullptr;
    _srs = nullptr;
    updateFrame();
    stop();
}

bool NavigationManipulator::updateFrame()
{
    osg::ref_ptr<MapNode> map;
    if (!_map.lock(map) || !map->getMapSRS())
        return false;
    _srs = map->getMapSRS();
    const auto paths = map->getParentalNodePaths();
    _mapToWorld = paths.empty() ? osg::Matrixd::identity() : osg::computeLocalToWorld(paths.front());
    return _worldToMap.invert(_mapToWorld);
}

bool NavigationManipulator::axes(osg::Vec3d& east, osg::Vec3d& north, osg::Vec3d& up) const
{
    GeoPoint point;
    osg::Matrixd frame;
    if (!_srs || !point.fromWorld(_srs, _eye) || !point.createLocalToWorld(frame))
        return false;
    east.set(frame(0, 0), frame(0, 1), frame(0, 2));
    north.set(frame(1, 0), frame(1, 1), frame(1, 2));
    up.set(frame(2, 0), frame(2, 1), frame(2, 2));
    east.normalize();
    north.normalize();
    up.normalize();
    return true;
}

bool NavigationManipulator::directions(osg::Vec3d& forward, osg::Vec3d& right, osg::Vec3d& up) const
{
    osg::Vec3d east, north;
    if (!axes(east, north, up))
        return false;
    forward = north * std::cos(_heading) + east * std::sin(_heading);
    right = east * std::cos(_heading) - north * std::sin(_heading);
    return true;
}

void NavigationManipulator::setByMatrix(const osg::Matrixd& matrix)
{
    for (unsigned i = 0; i < 16; ++i)
        if (!std::isfinite(matrix.ptr()[i])) return;
    updateFrame();
    const osg::Matrixd local = matrix * _worldToMap;
    _eye = local.getTrans();
    osg::Vec3d direction = osg::Matrixd::transform3x3(osg::Vec3d(0, 0, -1), local);
    direction.normalize();
    osg::Vec3d east, north, up;
    if (axes(east, north, up))
    {
        _heading = std::atan2(direction * east, direction * north);
        _pitch = std::asin(std::clamp(direction * up, -1.0, 1.0));
        _pitch = std::clamp(_pitch, -osg::DegreesToRadians(89.0), osg::DegreesToRadians(89.0));
    }
    _lookYaw = 0.0;
    _hasGround = false;
    stop();
}

void NavigationManipulator::setByInverseMatrix(const osg::Matrixd& matrix)
{
    osg::Matrixd inverse;
    if (inverse.invert(matrix)) setByMatrix(inverse);
}

osg::Matrixd NavigationManipulator::getMatrix() const
{
    osg::Vec3d east(1, 0, 0), north(0, 1, 0), up(0, 0, 1);
    axes(east, north, up);
    const double heading = _heading + _lookYaw;
    const osg::Vec3d forward = (north * std::cos(heading) + east * std::sin(heading)) * std::cos(_pitch) +
        up * std::sin(_pitch);
    // Build orientation at the origin, avoiding loss of precision when subtracting Earth-scale positions.
    osg::Matrixd result = osg::Matrixd::inverse(osg::Matrixd::lookAt(osg::Vec3d(), forward, up));
    result.setTrans(_eye);
    return result * _mapToWorld;
}

osg::Matrixd NavigationManipulator::getInverseMatrix() const
{
    return osg::Matrixd::inverse(getMatrix());
}

void NavigationManipulator::setSpeed(double value)
{
    if (std::isfinite(value) && value > 0.0) _speed = std::clamp(value, 0.01, 100000.0);
}

void NavigationManipulator::setLookSpeed(double value)
{
    if (std::isfinite(value) && value > 0.0) _lookSpeed = std::clamp(value, 0.01, 10.0);
}

void NavigationManipulator::setEyeHeight(double value)
{
    if (std::isfinite(value) && value > 0.0) _eyeHeight = std::clamp(value, 0.1, 100.0);
}

void NavigationManipulator::applyLookInput(const osg::Vec2d& input, double dt)
{
    look(std::clamp(input.x(), -1.0, 1.0) * getLookSpeed() * dt,
        std::clamp(input.y(), -1.0, 1.0) * getLookSpeed() * dt);
}

void NavigationManipulator::look(double yaw, double pitch)
{
    _heading = std::remainder(_heading + yaw, 2.0 * osg::PI);
    _pitch = std::clamp(_pitch + pitch, -osg::DegreesToRadians(89.0), osg::DegreesToRadians(89.0));
}

void NavigationManipulator::resetMotion()
{
}

void NavigationManipulator::stop()
{
    _gamepad = {};
    _keys.clear();
    _dragging = false;
    _lastTime = -1.0;
    resetMotion();
}

void NavigationManipulator::init(const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter&)
{
    stop();
}

void NavigationManipulator::saveHome()
{
    _home = getMatrix();
}

void NavigationManipulator::home(double)
{
    setByMatrix(_home);
    // Apply ground constraints before the camera is rendered, avoiding a one-frame jump back to map altitude.
    step(GamepadState(), 0.0);
}

void NavigationManipulator::home(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
{
    home(ea.getTime());
    aa.requestRedraw();
}

osg::Vec3d NavigationManipulator::displaced(const osg::Vec3d& meters) const
{
    osg::Vec3d delta = meters;
    if (_srs && !_srs->isGeographic())
    {
        const double scale = Units::METERS.convertTo(_srs->getUnits(), 1.0);
        delta.x() *= scale;
        delta.y() *= scale;
    }
    return _eye + delta;
}

bool NavigationManipulator::groundHeight(const GeoPoint& point, double& height) const
{
    osg::ref_ptr<MapNode> map;
    return _map.lock(map) && map->getTerrain() &&
        map->getTerrain()->getHeight(point.getSRS(), point.x(), point.y(), &height);
}

bool NavigationManipulator::constrain(osg::Vec3d& candidate, bool onGround)
{
    GeoPoint point;
    double height;
    if (!_srs || !point.fromWorld(_srs, candidate) || !groundHeight(point, height) || !std::isfinite(height))
    {
        _hasGround = false;
        return false;
    }
    if (onGround || point.z() < height + getEyeHeight())
    {
        point.z() = height + getEyeHeight();
        if (!point.toWorld(candidate))
        {
            _hasGround = false;
            return false;
        }
    }
    _hasGround = true;
    return true;
}

bool NavigationManipulator::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
{
    using E = osgGA::GUIEventAdapter;
    int key = ea.getKey();
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    // Releases must clear held input even when the UI has taken keyboard/mouse capture.
    if (ea.getEventType() == E::KEYUP)
    {
        _keys.erase(key);
        return !ea.getHandled() && navigationKey(key);
    }
    if (ea.getEventType() == E::RELEASE)
        _dragging = false;
    if (ea.getEventType() == E::FRAME)
    {
        auto* view = dynamic_cast<osgViewer::View*>(aa.asView());
        if (!updateFrame() || (view && view->getCamera()->getGraphicsContext() && !GamepadHandler::isViewFocused(view)))
        {
            stop();
            return false;
        }
        const double now = ea.getTime();
        const double dt = _lastTime < 0.0 ? 0.0 : std::clamp(now - _lastTime, 0.0, 0.1);
        _lastTime = now;
        GamepadState input = _gamepad;
        _gamepad.home = false;
        if (input.home)
        {
            home(ea, aa);
            return false;
        }
        input.move.x() += static_cast<int>(_keys.count('d')) - static_cast<int>(_keys.count('a'));
        input.move.y() += static_cast<int>(_keys.count('w')) - static_cast<int>(_keys.count('s'));
        input.look.x() += static_cast<int>(_keys.count(E::KEY_Right)) - static_cast<int>(_keys.count(E::KEY_Left));
        input.look.y() += static_cast<int>(_keys.count(E::KEY_Up)) - static_cast<int>(_keys.count(E::KEY_Down));
        input.rightTrigger = std::max(input.rightTrigger, _keys.count('e') ? 1.0 : 0.0);
        input.leftTrigger = std::max(input.leftTrigger, _keys.count('q') ? 1.0 : 0.0);
        input.fast = input.fast || _keys.count(E::KEY_Shift_L) || _keys.count(E::KEY_Shift_R);
        input.slow = input.slow || _keys.count(E::KEY_Control_L) || _keys.count(E::KEY_Control_R);
        // Small steps limit terrain sampling gaps and keep vehicle integration stable at low frame rates.
        const int count = std::max(1, static_cast<int>(std::ceil(dt / 0.02)));
        for (int i = 0; i < count; ++i)
        {
            const double elapsed = dt / count;
            applyLookInput(input.look, elapsed);
            step(input, elapsed);
        }
        aa.requestContinuousUpdate(true);
        aa.requestRedraw();
        return false;
    }
    if (ea.getHandled())
    {
        _dragging = false;
        return false;
    }
    if (ea.getEventType() == E::KEYDOWN)
    {
        if (key == E::KEY_Home)
        {
            home(ea, aa);
            return true;
        }
        if (navigationKey(key))
        {
            _keys.insert(key);
            aa.requestContinuousUpdate(true);
            return true;
        }
    }
    if (ea.getEventType() == E::PUSH && ea.getButton() == E::RIGHT_MOUSE_BUTTON)
    {
        _dragging = true;
        _mouseX = ea.getXnormalized();
        _mouseY = ea.getYnormalized();
        return true;
    }
    if (ea.getEventType() == E::DRAG && _dragging)
    {
        look((ea.getXnormalized() - _mouseX) * 2.0, (ea.getYnormalized() - _mouseY) * 2.0);
        _mouseX = ea.getXnormalized();
        _mouseY = ea.getYnormalized();
        aa.requestRedraw();
        return true;
    }
    return false;
}

FlyManipulator::FlyManipulator() : NavigationManipulator(250.0, 2.0) { }

void FlyManipulator::step(const GamepadState& input, double dt)
{
    osg::Vec3d forward, right, up;
    if (!directions(forward, right, up)) return;
    const osg::Vec3d looking = forward * std::cos(_pitch) + up * std::sin(_pitch);
    // Cubic lateral response suppresses incidental sideways input while retaining full strafe speed.
    const double lateral = std::clamp(input.move.x(), -1.0, 1.0);
    const double strafe = lateral * lateral * lateral;
    const osg::Vec3d motion = unitLimited(looking * input.move.y() + right * strafe +
        up * (input.rightTrigger - input.leftTrigger));
    osg::Vec3d candidate = displaced(motion * (getSpeed() * speedScale(input) * dt));
    constrain(candidate, false);
    _eye = candidate;
}

WalkManipulator::WalkManipulator() : NavigationManipulator(3.0, 1.7) { }

void WalkManipulator::step(const GamepadState& input, double dt)
{
    osg::Vec3d forward, right, up;
    if (!directions(forward, right, up)) return;
    const osg::Vec3d motion = unitLimited(forward * input.move.y() + right * input.move.x());
    osg::Vec3d candidate = displaced(motion * (getSpeed() * speedScale(input) * dt));
    if (constrain(candidate, true)) _eye = candidate;
}

DriveManipulator::DriveManipulator() : NavigationManipulator(25.0, 1.5)
{
    setLookSpeed(osg::DegreesToRadians(60.0));
}

void DriveManipulator::setAcceleration(double value)
{
    if (std::isfinite(value) && value > 0.0) _acceleration = std::clamp(value, 0.1, 30.0);
}

void DriveManipulator::resetMotion()
{
    _lookYaw = 0.0;
    _lookYawRate = 0.0;
    _velocity = 0.0;
    _displayVelocity = 0.0;
}

void DriveManipulator::applyLookInput(const osg::Vec2d& input, double dt)
{
    const double axis = std::clamp(input.x(), -1.0, 1.0);
    if (axis != 0.0)
    {
        // Integrate a filtered angular rate exactly, so stick changes ease in consistently at any frame rate.
        constexpr double response = 0.12;
        const double targetRate = axis * getLookSpeed();
        const double decay = std::exp(-dt / response);
        _lookYaw += targetRate * dt + (_lookYawRate - targetRate) * response * (1.0 - decay);
        _lookYawRate = targetRate + (_lookYawRate - targetRate) * decay;
    }
    else if (!isMouseLookActive())
    {
        // An exact critically damped spring returns to the vehicle heading without a position discontinuity.
        constexpr double spring = 6.0;
        const double decay = std::exp(-spring * dt);
        const double slope = _lookYawRate + spring * _lookYaw;
        _lookYaw = (_lookYaw + slope * dt) * decay;
        _lookYawRate = (_lookYawRate - spring * slope * dt) * decay;
        if (std::abs(_lookYaw) < osg::DegreesToRadians(0.05) &&
            std::abs(_lookYawRate) < osg::DegreesToRadians(0.5))
        {
            _lookYaw = 0.0;
            _lookYawRate = 0.0;
        }
    }
    else
        _lookYawRate = 0.0;

    const double limit = osg::DegreesToRadians(90.0);
    _lookYaw = std::clamp(_lookYaw, -limit, limit);
    if ((_lookYaw >= limit && _lookYawRate > 0.0) || (_lookYaw <= -limit && _lookYawRate < 0.0))
        _lookYawRate = 0.0;
    look(0.0, std::clamp(input.y(), -1.0, 1.0) * getLookSpeed() * dt);
}

void DriveManipulator::look(double yaw, double pitch)
{
    if (yaw != 0.0) _lookYawRate = 0.0;
    _lookYaw = std::clamp(_lookYaw + yaw, -osg::DegreesToRadians(90.0), osg::DegreesToRadians(90.0));
    _pitch = std::clamp(_pitch + pitch, -osg::DegreesToRadians(85.0), osg::DegreesToRadians(85.0));
}

void DriveManipulator::step(const GamepadState& input, double dt)
{
    const double throttle = std::clamp(input.rightTrigger + std::max(0.0, input.move.y()), 0.0, 1.0);
    const double brake = std::clamp(input.leftTrigger + std::max(0.0, -input.move.y()), 0.0, 1.0);
    const double pedal = throttle - brake;
    if (pedal * _velocity < 0.0)
    {
        const double amount = 12.0 * std::abs(pedal) * dt;
        _velocity = std::copysign(std::max(0.0, std::abs(_velocity) - amount), _velocity);
    }
    else if (pedal != 0.0)
        _velocity += pedal * getAcceleration() * dt;
    else
        _velocity *= std::exp(-0.5 * dt);
    const double limit = getSpeed() * speedScale(input);
    _velocity = std::clamp(_velocity, -0.3 * limit, limit);
    const double steering = std::clamp(input.move.x(), -1.0, 1.0) * osg::DegreesToRadians(30.0);
    // A 2.7 m wheelbase with capped lateral acceleration gives controllable turns at road speeds.
    const double maxYawRate = 6.0 / std::max(1.0, std::abs(_velocity));
    const double yawRate = std::clamp(_velocity / 2.7 * std::tan(steering), -maxYawRate, maxYawRate);
    const double oldHeading = _heading;
    _heading = std::remainder(_heading + yawRate * dt, 2.0 * osg::PI);
    osg::Vec3d forward, right, up;
    if (!directions(forward, right, up)) return;
    osg::Vec3d candidate = displaced(forward * (_velocity * dt));
    if (constrain(candidate, true))
        _eye = candidate;
    else
    {
        _heading = oldHeading;
        _velocity = 0.0;
    }
    _displayVelocity = _velocity;
}

NavigationModeHandler::NavigationModeHandler(Util::EarthManipulator* mapManipulator)
{
    _manipulators[MAP] = mapManipulator;
    _manipulators[FLY] = new FlyManipulator();
    _manipulators[WALK] = new WalkManipulator();
    _manipulators[DRIVE] = new DriveManipulator();
}

osgGA::CameraManipulator* NavigationModeHandler::getManipulator(Mode mode) const
{
    const int index = static_cast<int>(mode);
    return index >= MAP && index <= DRIVE ? _manipulators[index].get() : nullptr;
}

bool NavigationModeHandler::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
{
    if (ea.getEventType() != osgGA::GUIEventAdapter::FRAME) return false;
    auto* view = dynamic_cast<osgViewer::View*>(aa.asView());
    const int requested = _requested.exchange(-1);
    if (!view || requested < MAP || requested > DRIVE) return false;
    auto* previous = view->getCameraManipulator();
    auto* next = _manipulators[requested].get();
    if (!previous || !next || next == previous) return false;
    const osg::Matrixd pose = previous->getMatrix();
    if (auto* navigation = dynamic_cast<NavigationManipulator*>(previous)) navigation->stop();
    view->setCameraManipulator(next, false);
    if (auto* earth = dynamic_cast<Util::EarthManipulator*>(next))
    {
        osg::ref_ptr<osgGA::GUIEventAdapter> release = new osgGA::GUIEventAdapter();
        release->setEventType(osgGA::GUIEventAdapter::KEYUP);
        release->setTime(ea.getTime());
        earth->handle(*release, aa);
        earth->clearViewpoint();
    }
    next->setByMatrix(pose);
    if (auto* navigation = dynamic_cast<NavigationManipulator*>(next))
    {
        if (requested == WALK || requested == DRIVE) navigation->levelView();
        navigation->saveHome();
    }
    _current = requested;
    aa.requestContinuousUpdate(true);
    aa.requestRedraw();
    return false;
}
