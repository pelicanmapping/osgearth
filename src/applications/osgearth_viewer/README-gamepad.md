# Windows gamepad prototype

Build with the repository's `configure.bat` and `build.bat`. From the repository root,
in a Command Prompt:

```bat
call osgearth_shell.bat
cd tests
osgearth_viewer simple.earth --gamepad
```

The shared `osgEarth::GamepadHandler` uses the view's existing `EarthManipulator`.
Its Windows backend links against the Windows SDK's `xinput9_1_0` library.
For Fly, Walk, and Drive modes, use `osgearth_imgui` and its Tools > Camera panel;
see [navigation controls](../osgearth_imgui/README-navigation.md).

| Control | Action |
| --- | --- |
| Left stick | Pan |
| Right stick | Heading and pitch |
| Left / right trigger | Zoom out / in |
| Left / right shoulder | Quarter speed / four times speed |
| A | Home viewpoint |

Stick movement has radial dead zones and a quadratic response for fine control.
Movement uses elapsed time, with a 100 ms maximum step after stalls. Trigger zoom
uses an exponential range factor. Both sticks and triggers can operate together.

Only the foreground viewer window accepts input. Release both sticks, both
triggers, and A after connecting a controller, returning to the window, or pressing
Home. This prevents held controls from unexpectedly moving the camera.

The first connected XInput controller is selected. Disconnected slots are scanned
once a second; disconnecting stops gamepad motion. Gamepad navigation cancels an
existing viewpoint transition or tether and stops prior mouse/keyboard motion.
Zoom uses the focal point, independent of the mouse cursor.

Prototype limits: earth files only, one controller, fixed bindings and sensitivity,
continuous rendering for polling, no UI navigation or rumble. The handler is intended
for the native OSG Windows viewer; embedded windows and multiple views are outside
this prototype. Mouse and keyboard still work; active gamepad navigation takes priority.
