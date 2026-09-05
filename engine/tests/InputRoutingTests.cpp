#include "input/InputRouting.h"

#include <array>
#include <cstdlib>
#include <iostream>

static int checks = 0;
static void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void RunRoutingTests() {
    InputRouting::State s;
    std::array<bool, 256> keys{};
    std::array<bool, 4> mouse{};
    std::array<bool, 16> pad{};
    std::array<float, 6> axes{};
    auto frame = [&](bool focused, bool inside, bool locked = false) {
        s.keys.Sample(keys);
        s.mouse.Sample(mouse);
        s.pad.Sample(pad);
        s.Route(focused, inside, locked, axes);
    };

    frame(false, false);
    mouse[0] = true;
    frame(true, true);
    Check(s.mouse.Pressed(0) && s.MouseAllowed(0), "first click focuses and reaches the game");
    Check(s.HasDrag(), "drag starts inside game");
    frame(true, false);
    Check(s.mouse.Held(0) && s.MouseAllowed(0), "drag remains owned outside the viewport");
    mouse[0] = false;
    frame(true, false);
    Check(s.mouse.Released(0) && s.MouseAllowed(0), "button-up is delivered outside the viewport");
    Check(!s.HasDrag(), "drag ownership ends on release");
    mouse[0] = true;
    frame(true, false);
    Check(!s.MouseAllowed(0), "outside click is not a game click");
    frame(true, true);
    Check(!s.mouse.Held(0) && !s.MouseAllowed(0), "dragging an outside press into game cannot activate it");
    mouse[0] = false;
    frame(true, true);
    Check(!s.mouse.Released(0), "outside press cannot create a release in game");

    mouse[0] = true;
    frame(true, true);
    s.mouse.Consume(0);
    Check(s.mouse.Pressed(0, true), "UI sees the click it consumes");
    Check(!s.mouse.Pressed(0), "gameplay cannot also fire on a UI click");
    frame(true, false);
    Check(s.mouse.Held(0, true) && !s.mouse.Held(0), "consumption persists through dragging");
    mouse[0] = false;
    frame(true, true);
    Check(s.mouse.Released(0, true) && !s.mouse.Released(0), "UI release cannot leak to gameplay");
    mouse[0] = true;
    frame(true, true);
    Check(s.mouse.Pressed(0), "a later independent click is available again");

    keys[17] = true;
    pad[0] = true;
    axes[0] = 1.0f;
    frame(false, false);
    Check(!s.HasDrag() && !s.MouseAllowed(0), "focus loss cancels drag ownership");
    frame(true, true);
    Check(!s.keys.Held(17) && !s.keys.Held(17, true), "held keyboard input is blocked for UI and game on refocus");
    Check(!s.pad.Held(0) && s.axisBlocked[0], "held gamepad input and axes are blocked on refocus");
    Check(s.axisBlocked[1], "a deflected stick blocks both axes until neutral");
    Check(!s.mouse.Held(0), "held mouse input cannot restart a cancelled drag");
    frame(true, true);
    Check(!s.keys.Held(17), "held key remains blocked after the focus frame");
    keys[17] = false;
    pad[0] = false;
    mouse[0] = false;
    axes[0] = 0.0f;
    frame(true, true);
    Check(!s.keys.Released(17), "cancelled key does not produce a gameplay release");
    Check(!s.axisBlocked[0], "neutral stick permits subsequent movement");
    keys[17] = true;
    pad[0] = true;
    frame(true, true, true);
    Check(s.keys.Pressed(17) && s.pad.Pressed(0), "fresh input works even with cursor locked");
    s.keys.Consume(17);
    s.pad.Consume(0);
    Check(s.keys.Pressed(17, true) && s.pad.Pressed(0, true), "cursor lock does not disable UI navigation");
    Check(!s.keys.Pressed(17) && !s.pad.Pressed(0), "UI navigation is not also gameplay");
    frame(true, true);
    Check(!s.keys.Held(17), "closing UI while a key is held does not start gameplay movement");
    Check(!s.keys.Held(256) && !s.mouse.Pressed(4), "invalid input indices are harmless");
    std::cout << checks << " routing checks passed\n";
}
