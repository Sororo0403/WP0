#pragma once
#include "imgui.h"
#include "input/Input.h"

namespace EditorSceneGameUiInput {
inline int ScanCode(ImGuiKey key) {
    switch (key) {
    case ImGuiKey_LeftArrow: return DIK_LEFT;
    case ImGuiKey_RightArrow: return DIK_RIGHT;
    case ImGuiKey_UpArrow: return DIK_UP;
    case ImGuiKey_DownArrow: return DIK_DOWN;
    case ImGuiKey_Enter: return DIK_RETURN;
    case ImGuiKey_Space: return DIK_SPACE;
    case ImGuiKey_Tab: return DIK_TAB;
    case ImGuiKey_Escape: return DIK_ESCAPE;
    case ImGuiKey_Backspace: return DIK_BACK;
    default: return -1;
    }
}
inline bool KeyDown(const Input* input, ImGuiKey key) {
    return input != nullptr && input->IsKeyPress(ScanCode(key));
}
inline bool KeyPressed(const Input* input, ImGuiKey key, bool repeat) {
    return input != nullptr && (input->IsKeyTrigger(ScanCode(key)) ||
        (repeat && input->IsKeyPress(ScanCode(key)) && ImGui::IsKeyPressed(key, true)));
}
} // namespace EditorSceneGameUiInput
