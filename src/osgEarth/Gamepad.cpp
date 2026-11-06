/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <osgEarth/Gamepad>
#include <osgEarth/NavigationManipulators>
#include <osgViewer/View>
#include <algorithm>
#include <cmath>
#ifdef _WIN32
#include <osgViewer/api/Win32/GraphicsWindowWin32>
#include <xinput.h>
#endif

using namespace osgEarth;

#ifdef _WIN32
namespace
{
    /** Normalize asymmetric signed endpoints to [-1,1]. */
    double axis(SHORT value)
    {
        return value / (value < 0 ? 32768.0 : 32767.0);
    }

    /** Apply a radial dead zone and quadratic response while preserving diagonal direction. */
    osg::Vec2d stick(SHORT x, SHORT y, double deadZone)
    {
        osg::Vec2d value(axis(x), axis(y));
        const double length = value.length();
        if (length <= deadZone) return osg::Vec2d();
        const double magnitude = (std::min(length, 1.0) - deadZone) / (1.0 - deadZone);
        return value * (magnitude * magnitude / length);
    }

    /** Remove the trigger dead zone and rescale remaining travel to [0,1]. */
    double trigger(BYTE value)
    {
        return std::max(0.0, (value - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) /
            (255.0 - XINPUT_GAMEPAD_TRIGGER_THRESHOLD));
    }
}
#endif

GamepadHandler::GamepadHandler(bool enabled) : _enabled(enabled) { }

bool GamepadHandler::isViewFocused(osgViewer::View* view)
{
    if (!view) return false;
#ifdef _WIN32
    auto* window = dynamic_cast<osgViewer::GraphicsWindowWin32*>(view->getCamera()->getGraphicsContext());
    const HWND foreground = GetForegroundWindow();
    return foreground && window && window->getHWND() && GetAncestor(window->getHWND(), GA_ROOT) == foreground;
#else
    return true;
#endif
}

bool GamepadHandler::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
{
    if (ea.getEventType() != osgGA::GUIEventAdapter::FRAME)
    {
        if (ea.getEventType() == osgGA::GUIEventAdapter::PUSH ||
            ea.getEventType() == osgGA::GUIEventAdapter::DRAG ||
            ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN ||
            ea.getEventType() == osgGA::GUIEventAdapter::SCROLL)
            _moving = false;
        return false;
    }
    auto* view = dynamic_cast<osgViewer::View*>(aa.asView());
    auto* manip = view ? view->getCameraManipulator() : nullptr;
    auto* navigation = dynamic_cast<NavigationManipulator*>(manip);
    auto* earth = dynamic_cast<Util::EarthManipulator*>(manip);
    if (_active.get() != manip)
    {
        osg::ref_ptr<osgGA::CameraManipulator> previous;
        if (_active.lock(previous))
            if (auto* old = dynamic_cast<NavigationManipulator*>(previous.get())) old->stop();
        _active = manip;
        _armed = false;
        _moving = false;
        _buttons = 0;
        _lastTime = -1.0;
    }
    const double now = ea.getTime();
    const double dt = _lastTime < 0.0 ? 0.0 : std::clamp(now - _lastTime, 0.0, 0.1);
    if (now < _lastTime) _nextScan = now;
    _lastTime = now;
    const bool enabled = getEnabled();
    const bool focused = isViewFocused(view);
    if (!enabled || !focused || (!navigation && !earth) || !manip->getNode())
    {
        if (navigation)
        {
            navigation->setGamepadState({});
            if (!focused || (_wasEnabled && !enabled)) navigation->stop();
        }
        _armed = false;
        _moving = false;
        _buttons = 0;
        _wasEnabled = enabled;
        return false;
    }
    if (!_wasEnabled) _nextScan = now;
    _wasEnabled = true;

#ifdef _WIN32
    XINPUT_STATE state{};
    int controller = _controller.load();
    if (controller >= 0)
    {
        if (XInputGetState(controller, &state) != ERROR_SUCCESS)
        {
            _controller = -1;
            _nextScan = now + 1.0;
            _armed = false;
            _moving = false;
            _buttons = 0;
            if (navigation) navigation->stop();
            return false;
        }
    }
    else
    {
        if (navigation) navigation->setGamepadState({});
        // Empty slots can be costly to poll; scan at most once a second.
        if (now < _nextScan) return false;
        _nextScan = now + 1.0;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
        {
            if (XInputGetState(i, &state) == ERROR_SUCCESS)
            {
                controller = static_cast<int>(i);
                _controller = controller;
                break;
            }
        }
        if (controller < 0) return false;
    }
    const auto& pad = state.Gamepad;
    GamepadState input;
    input.move = stick(pad.sThumbLX, pad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE / 32767.0);
    input.look = stick(pad.sThumbRX, pad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE / 32767.0);
    input.leftTrigger = trigger(pad.bLeftTrigger);
    input.rightTrigger = trigger(pad.bRightTrigger);
    input.slow = (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    input.fast = (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    const bool home = (pad.wButtons & XINPUT_GAMEPAD_A) != 0;
    input.home = home && !(_buttons & XINPUT_GAMEPAD_A);
    _buttons = pad.wButtons;
    if (!_armed)
    {
        if (navigation) navigation->setGamepadState({});
        _armed = input.move.length2() == 0.0 && input.look.length2() == 0.0 &&
            input.leftTrigger == 0.0 && input.rightTrigger == 0.0 && !home;
        return false;
    }
    if (navigation)
        navigation->setGamepadState(input);
    else if (earth)
    {
        const double zoom = input.rightTrigger - input.leftTrigger;
        const bool moving = input.move.length2() > 0.0 || input.look.length2() > 0.0 || zoom != 0.0;
        if (input.home || (moving && !_moving))
        {
            // KEYUP clears mouse inertia and the resident keyboard task through the public interface.
            osg::ref_ptr<osgGA::GUIEventAdapter> release = new osgGA::GUIEventAdapter();
            release->setEventType(osgGA::GUIEventAdapter::KEYUP);
            release->setTime(now);
            earth->handle(*release, aa);
            if (earth->isSettingViewpoint() || earth->isTethering()) earth->clearViewpoint();
        }
        _moving = moving;
        if (input.home) earth->home(ea, aa);
        else if (moving && dt > 0.0)
        {
            const double step = dt * (input.slow ? 0.25 : 1.0) * (input.fast ? 4.0 : 1.0);
            if (input.move.length2() > 0.0) earth->pan(-input.move.x() * step, -input.move.y() * step);
            if (input.look.length2() > 0.0) earth->rotate(input.look.x() * step, input.look.y() * step);
            // Focal-point exponential zoom, independent of cursor position and frame rate.
            if (zoom != 0.0) earth->zoom(0.0, std::expm1(-zoom * step), nullptr);
            aa.requestRedraw();
        }
    }
    if (input.home)
    {
        _armed = false;
        _moving = false;
    }
#endif
    return false;
}
