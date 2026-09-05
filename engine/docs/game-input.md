# Game input and cursor

Game View focus routes keyboard and gamepad input to the game. Pointer buttons
start only inside the game image; an owned drag keeps its release event outside
the image. Clicking another editor panel or losing application focus cancels
input. Held keys/buttons and deflected sticks must return to neutral before
they can start a new operation after refocus.

The default cursor is **Free and visible**. A game opts into other modes:

```cpp
input->SetCursorMode(CursorMode::Locked);  // relative mouse look
input->SetCursorVisible(false);

// When opening a pause menu:
input->SetCursorMode(CursorMode::Free);
input->SetCursorVisible(true);
```

`Confined` allows movement throughout the game image but prevents leaving it.
Visibility never implies locking. `GetCursorMode()` returns the game's request;
`GetEffectiveCursorMode()` returns what the focused host actually applied.
`GetPointerPosition()` is in game image pixels, with origin at the upper left.
It may be outside the image during a drag; check `IsPointerInsideGame()` for
hit-testing. `GetMouseDX/DY()` expose device movement, independent of image scale.

In the editor, **Shift+Esc** releases game focus and cursor confinement until
the Game View is clicked again. Plain Esc belongs to the game. Editor keyboard
commands do not run while game input has focus; the toolbar/menu remain usable
with a free cursor. Pause, Stop, hidden Game View, and focus loss release cursor
confinement. The packaged player has no editor escape shortcut or capture hint.

Runtime UI processes the routed input before behaviors update. It consumes
pointer presses, navigation, submit, cancel, and text-entry keys it handles.
Consumption lasts through button-up, preventing a UI click from also firing a
weapon, or a held text-entry key from starting movement after the field closes.
Pointer lock does not disable keyboard/gamepad UI navigation.

The host calls `RouteGameInput` before UI and simulation, toggles
`SetUiQueryMode` only around UI dispatch, and applies cursor requests after
simulation. Rendering never changes input routing. WinApp only manages cursor
visibility and emergency release on OS focus loss.

Build `engine/tests/InputTests.vcxproj` (x64 Debug or Release), then run
`generated/outputs/x64/<Configuration>/InputTests/InputTests.exe` for the routing,
public API/replay, and two-phase ImGui layout regression tests.
