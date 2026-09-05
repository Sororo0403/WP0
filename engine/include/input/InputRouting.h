#pragma once

#include <array>
#include <cstddef>

namespace InputRouting {
template <size_t Count> struct Buttons {
    std::array<bool, Count> now{}, previous{}, blocked{}, consumed{};
    std::array<bool, Count> blockedThisFrame{}, consumedThisFrame{};

    void Sample(const std::array<bool, Count>& value) {
        previous = now;
        now = value;
        blockedThisFrame = blocked;
        consumedThisFrame = consumed;
        for (size_t i = 0; i < Count; ++i) {
            blocked[i] = blocked[i] && now[i];
            consumed[i] = consumed[i] && now[i];
        }
    }
    void Block(size_t i) {
        blocked[i] = now[i];
        blockedThisFrame[i] = true;
    }
    void BlockHeld() {
        for (size_t i = 0; i < Count; ++i) {
            if (now[i] || previous[i]) Block(i);
        }
    }
    void Consume(size_t i) {
        consumed[i] = now[i];
        consumedThisFrame[i] = true;
    }
    bool Allowed(size_t i, bool ui) const {
        return i < Count && !blockedThisFrame[i] && (ui || !consumedThisFrame[i]);
    }
    bool Held(size_t i, bool ui = false) const {
        return Allowed(i, ui) && now[i];
    }
    bool Pressed(size_t i, bool ui = false) const {
        return Allowed(i, ui) && now[i] && !previous[i];
    }
    bool Released(size_t i, bool ui = false) const {
        return Allowed(i, ui) && !now[i] && previous[i];
    }
};

struct State {
    Buttons<256> keys;
    Buttons<4> mouse;
    Buttons<16> pad;
    bool focused = true;
    bool inside = true;
    bool routed = false;
    std::array<bool, 4> mouseOwned{}, mouseAllowed{};
    std::array<bool, 6> axisBlocked{};

    void Route(bool focus, bool pointerInside, bool locked,
               const std::array<float, 6>& axes) {
        routed = true;
        const bool gained = focus && !focused;
        focused = focus;
        inside = pointerInside;
        if (!focus || gained) {
            keys.BlockHeld();
            pad.BlockHeld();
            for (size_t i = 0; i < axes.size(); ++i) {
                axisBlocked[i] = axes[i] != 0.0f;
            }
        }
        // A stick is one control: both axes must return to neutral together.
        for (size_t i = 0; i < 4; i += 2) {
            const bool blocked = axisBlocked[i] || axisBlocked[i + 1];
            const bool neutral = axes[i] == 0.0f && axes[i + 1] == 0.0f;
            axisBlocked[i] = axisBlocked[i + 1] = blocked && !neutral;
        }
        for (size_t i = 4; i < axes.size(); ++i) {
            if (axes[i] == 0.0f) axisBlocked[i] = false;
        }
        for (size_t i = 0; i < mouseOwned.size(); ++i) {
            if (!focus) {
                mouseOwned[i] = false;
                if (mouse.now[i] || mouse.previous[i]) mouse.Block(i);
            } else if (mouse.now[i] && !mouse.previous[i]) {
                mouseOwned[i] = pointerInside || locked;
                if (!mouseOwned[i]) mouse.Block(i);
            }
            mouseAllowed[i] = focus && mouseOwned[i];
            if (!mouse.now[i]) mouseOwned[i] = false;
        }
    }
    bool HasDrag() const {
        for (bool owned : mouseOwned) if (owned) return true;
        return false;
    }
    bool MouseAllowed(size_t i) const {
        return i < mouseAllowed.size() && (!routed || mouseAllowed[i]);
    }
};
} // namespace InputRouting
