#include "input/Input.h"
#include "imgui.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

static int checks = 0;
static void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

void RunRoutingTests();
int main(int argc, char** argv) {
    RunRoutingTests();
    const std::filesystem::path replay = argc > 1 ? std::filesystem::path(argv[1]) :
        std::filesystem::path(__FILE__).parent_path() / "input_api_replay.json";
    Input input;
    Check(input.GetCursorMode() == CursorMode::Free && input.IsCursorVisibleRequested(), "free and visible by default");
    input.SetCursorVisible(false);
    Check(input.GetCursorMode() == CursorMode::Free, "hiding never requests locking");
    input.SetCursorMode(CursorMode::Locked);
    Check(!input.IsCursorVisibleRequested(), "locking preserves visibility request");
    RECT bounds{0, 0, 100, 100};
    input.ApplyCursorMode(bounds);
    Check(input.GetEffectiveCursorMode() == CursorMode::Free, "no active host means no OS lock");
    input.ResetGameInput();
    Check(!input.HasGameInputFocus() && input.GetCursorMode() == CursorMode::Free, "reset releases focus and cursor request");
    Check(input.StartReplay(replay.wstring()), "load public replay API");
    auto frame = [&](bool focused, bool inside) {
        input.Update(1.0f/60.0f);
        input.RouteGameInput(focused, inside, {24.0f, 37.0f});
    };
    frame(true, true); // idle activation
    Check(input.GetPointerPosition().x == 24.0f && input.IsPointerInsideGame(), "game coordinates and bounds are available");
    frame(true, true); // W, space, A, click
    Check(input.IsMouseTrigger(0) && input.IsKeyTrigger(DIK_W), "fresh input reaches game in this frame");
    Check(input.GetActionAxis("MoveVertical") > 0 && input.IsActionTriggered("Jump"), "actions use routed keys and gamepad");
    input.SetUiQueryMode(true);
    input.ConsumeKey(DIK_W);
    input.ConsumeKey(DIK_SPACE);
    input.ConsumeGamepadButtons(XINPUT_GAMEPAD_A);
    input.ConsumeMouseButton(0);
    input.ConsumeMouseWheel();
    Check(input.IsMouseTrigger(0) && input.IsActionTriggered("Jump"), "UI retains its own consumed inputs");
    input.SetUiQueryMode(false);
    Check(!input.IsMousePress(0) && !input.IsActionPressed("Jump") && input.GetActionAxis("MoveVertical") == 0, "UI input cannot leak via direct queries or actions");
    Check(input.GetMouseWheel() == 0, "UI consumes wheel input");
    frame(true, false); // held outside
    Check(input.HasGamePointerDrag() && !input.IsMousePress(0), "consumed drag stays owned outside");
    input.SetUiQueryMode(true);
    Check(input.IsMousePress(0), "UI can continue the drag outside");
    input.SetUiQueryMode(false);
    Check(input.GetMouseWheel() == 0, "outside wheel does not scroll game while dragging");
    frame(true, false); // release outside
    input.SetUiQueryMode(true);
    Check(input.IsMouseRelease(0), "UI receives outside release");
    input.SetUiQueryMode(false);
    Check(!input.IsMouseRelease(0) && !input.IsActionReleased("Jump"), "consumed release stays consumed");
    frame(false, false); // W+A+axis held while inactive
    frame(true, true); // still held on regain
    Check(!input.IsKeyPress(DIK_W) && !input.IsGamepadButtonPress(XINPUT_GAMEPAD_A) && input.GetGamepadLeftStickX() == 0, "refocus suppresses held devices");
    frame(true, true); // neutral
    frame(true, true); // fresh W+A
    Check(input.IsKeyTrigger(DIK_W) && input.IsGamepadButtonTrigger(XINPUT_GAMEPAD_A), "fresh presses after neutral work");
    input.SetCursorMode(CursorMode::Locked);
    input.OnWindowFocusLost();
    Check(!input.HasGameInputFocus(), "host focus loss cancels input even without a simulation frame");
    Check(input.GetCursorMode() == CursorMode::Locked && input.GetEffectiveCursorMode() == CursorMode::Free, "focus loss preserves request but releases effective lock");

    // Exercise the real ImGui two-phase window lifecycle without a GPU backend.
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800,600};
    io.DeltaTime = 1.0f/60.0f;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (int i = 0; i < 3; ++i) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0,0});
        ImGui::SetNextWindowSize({500,400});
        ImGui::Begin("Game",nullptr,ImGuiWindowFlags_NoNavInputs);
        const ImVec2 before = ImGui::GetCursorScreenPos();
        ImGui::End();
        ImGui::Begin("Game",nullptr,ImGuiWindowFlags_NoNavInputs);
        const ImVec2 after = ImGui::GetCursorScreenPos();
        Check(before.x == after.x && before.y == after.y, "input layout and rendered image share the same origin");
        ImGui::Dummy({400,300});
        ImGui::End();
        ImGui::Render();
    }
    ImGui::DestroyContext();
    std::cout << checks << " public API and frame-layout checks passed\n";
}
