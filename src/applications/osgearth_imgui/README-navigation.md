# Camera navigation modes

Build with `configure.bat` and `build.bat`. From a Command Prompt in the repository root:

```bat
call osgearth_shell.bat
cd tests
osgearth_imgui simple.earth --gamepad
```

Open **Tools > Camera** and select **Map**, **Fly**, **Walk**, or **Drive**.
The Gamepad checkbox can enable Windows XInput without a command-line flag.
Keyboard and mouse controls also work without a gamepad, including on other platforms.

| Mode | Left stick | Right stick | LT / RT |
| --- | --- | --- | --- |
| Map | Pan focal point | Orbit focal point | Zoom out / in |
| Fly | Forward/back and strafe | Look around | Descend / ascend |
| Walk | Forward/back and strafe | Look around | Unused |
| Drive | Steer; forward/back also operates pedals | Hold left/right to glance; release to face forward | Brake then reverse / throttle |

LB slows movement to one quarter speed; RB increases it to four times speed.
A returns to the map home viewpoint, or to the camera position saved when entering
an eye-centered mode. Release sticks, triggers and A after switching modes,
reconnecting, returning to the window, or pressing Home. Controller input only acts
in the foreground Windows viewer. Disconnects and loss of focus stop vehicle motion.

For Fly and Walk, use **WASD** to move and strafe. **E/Q** ascend/descend in Fly.
For Drive, **W/S** operate throttle/brake/reverse and **A/D** steer. Use the arrow
keys or right-mouse drag to look; **Shift/Ctrl** are fast/slow modifiers and **Home**
returns to the entry pose. The Camera panel exposes movement speed, look speed,
eye height/flight clearance, and driving acceleration. Settings survive mode changes
within the running application.

Each mode is a dedicated `osgGA::CameraManipulator`: `EarthManipulator`,
`FlyManipulator`, `WalkManipulator`, or `DriveManipulator`. Include
`osgEarth/NavigationManipulators` to use the new classes. `GamepadHandler` supplies
normalized `GamepadState` samples; the dedicated manipulators own their navigation
behavior and keyboard/mouse handling. `NavigationModeHandler` retains the four
controllers and applies GUI requests during event traversal, transferring the
camera pose and clearing previous input/velocity. The application uses continuous
frames for polling and terrain following.

Fly automatically scales speed with height above loaded terrain. The Camera panel's
**Speed at 1 km** is the reference speed: with the default 2,500 m/s, flight speed is
100 m/s near ground, 250 m/s at 100 m, 2,500 m/s at 1 km, and 25,000 m/s at 10 km.
The scale ranges from 0.04x to 400x, with a final 100,000 m/s cap including boost.
Acceleration eases over 0.75 seconds; deceleration responds over 0.2 seconds.
If terrain is not loaded, map-datum altitude is used until terrain becomes available.
**Full-stick speed** shows the scaled speed including fast/slow modifiers; partial stick
deflection still reduces motion. Disable **Altitude speed** for constant-speed flight.
Look sensitivity, Walk, and Drive are unaffected.

Fly applies an additional cubic response to lateral input to reduce sideways drift
when pushing forward, retaining full strafe speed at full sideways input.
Fly is eye-centered free flight with a local-up horizon and clearance from loaded
terrain. Walk and Drive settle onto terrain beneath the current camera, preserving
heading and starting with a level view. If destination terrain is not loaded,
they hold position; they resample as terrain arrives. Heights use the map's vertical
datum and position calculations use double precision, including across the dateline.

Drive uses a gently filtered horizontal look rate, limited to 90 degrees either side of the vehicle.
Small stick deflections turn the view slowly; larger deflections turn it faster. Releasing horizontal
look smoothly returns the view to the vehicle heading with a damped spring, including while steering.
The default Look speed is 60 degrees/second and is adjustable in the Camera panel.
Left/right arrows and right-mouse dragging also recenter smoothly on release. Vertical pitch retains
its existing behavior. Walk and Fly are unchanged.

Drive is an arcade in-cab model with acceleration, braking/reverse, coasting and a
2.7 m wheelbase steering model. Free look does not steer the vehicle. This prototype
follows the loaded terrain surface; it does not simulate suspension, tire grip,
gravity, or collisions with buildings/other scene objects. Switching from a very
high map view to Walk/Drive moves down to the loaded surface beneath the camera.
