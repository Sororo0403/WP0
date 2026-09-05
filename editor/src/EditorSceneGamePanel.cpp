#include "EditorScene.h"

#include "graphics/DirectXCommon.h"
#include "core/WinApp.h"
#include "imgui.h"
#include "input/Input.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr ImGuiWindowFlags kGamePanelFlags = ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus;
}

void EditorScene::DrawGamePanelWindow() {
    if (!showGamePanel_) {
        return;
    }
    const bool visible = BeginGamePanelWindow();
    if (visible) {
        DrawGamePanelContent();
    }
    ImGui::End();
}

bool EditorScene::BeginGamePanelWindow() {
    if (focusGamePanelRequested_) {
        ImGui::SetNextWindowFocus();
        focusGamePanelRequested_ = false;
    }
    ImGuiWindowFlags flags = kGamePanelFlags;
    bool* open = &showGamePanel_;
    if (playerMode_) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        flags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus;
        open = nullptr;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    }
    const bool visible = ImGui::Begin("Game", open, flags);
    if (playerMode_) {
        ImGui::PopStyleVar();
    }
    return visible;
}

void EditorScene::DrawGamePanelContent() {
    if (!IsGamePanelRenderReady()) {
        ImGui::TextDisabled("Game View RenderSurface is not ready.");
        return;
    }
    const bool hasGameCamera = RenderGamePanelFrame();
    ImVec2 imageMin{};
    ImVec2 imageMax{};
    DrawGamePanelImage(imageMin, imageMax);
    if (playModeState_ == PlayModeState::Edit) HandleGameUiEditing(imageMin, imageMax);
    DrawGamePanelOverlay(imageMin, hasGameCamera);
}

void EditorScene::UpdateGamePanelRequestedSize() {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    requestedGameWidth_ = (std::max)(1, static_cast<int>(std::lround(available.x)));
    requestedGameHeight_ = (std::max)(1, static_cast<int>(std::lround(available.y)));
}

bool EditorScene::IsGamePanelRenderReady() const {
    return gameViewSurface_.IsReady() && gameViewPostProcess_.IsReady() && ctx_ != nullptr &&
           ctx_->rendering.dxCommon != nullptr && ctx_->rendering.model != nullptr;
}

bool EditorScene::RenderGamePanelFrame() {
    const bool hasGameCamera = UpdateGameViewCamera();
    if (hasGameCamera) {
        BuildRenderScene();
    } else {
        renderScene_.BeginFrame();
    }
    sceneRenderer_.Render(renderScene_, gameViewCamera_, gameViewSurface_,
                          {0.025f, 0.035f, 0.055f, 1.0f});
    gameViewSurface_.TransitionDepthToShaderResource();
    gameViewSurface_.BeginOutputPass({0.0f, 0.0f, 0.0f, 1.0f});
    const PostProcessOutputTarget target{
        gameViewSurface_.GetOutputRtvHandle(),
        static_cast<uint32_t>(gameViewSurface_.GetWidth()),
        static_cast<uint32_t>(gameViewSurface_.GetHeight()),
        DirectXCommon::kBackBufferFormat,
    };
    gameViewPostProcess_.DrawToTarget(gameViewSurface_.GetSceneColorGpuHandle(),
                                      gameViewSurface_.GetDepthGpuHandle(), target);
    (void)DrawGameUi(gameViewSurface_.GetWidth(), gameViewSurface_.GetHeight(), hasGameCamera);
    gameViewSurface_.EndOutputPass();
    gameViewSurface_.TransitionDepthToWrite();
    ctx_->rendering.dxCommon->SetBackBufferRenderTarget(false, false);
    return hasGameCamera;
}

void EditorScene::DrawGamePanelImage(ImVec2& imageMin, ImVec2& imageMax) {
    const D3D12_GPU_DESCRIPTOR_HANDLE output = gameViewSurface_.GetOutputGpuHandle();
    ImGui::Image(static_cast<ImTextureID>(output.ptr),
                 ImVec2(static_cast<float>(requestedGameWidth_),
                        static_cast<float>(requestedGameHeight_)));
    imageMin = ImGui::GetItemRectMin();
    imageMax = ImGui::GetItemRectMax();
}

void EditorScene::PrepareGameInputFrame() {
    gameUiHoveredButton_ = {};
    gameUiSubmitHeld_ = false;
    Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (input == nullptr) return;
    input->SetUiQueryMode(false);
    if (gameInputFocused_ && !input->HasGameInputFocus() && !playerMode_) {
        gameInputSuspended_ = true;
    }
    if (!showGamePanel_) {
        ReleaseGameInputFocus();
        return;
    }

    // First Begin establishes this frame's layout and focus; drawing appends to
    // the same window later, without changing any routing state.
    const bool visible = BeginGamePanelWindow();
    UpdateGamePanelRequestedSize();
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    const ImVec2 maximum{minimum.x + requestedGameWidth_, minimum.y + requestedGameHeight_};
    gameInputScreenBounds_ = {static_cast<LONG>(std::floor(minimum.x)),
                             static_cast<LONG>(std::floor(minimum.y)),
                             static_cast<LONG>(std::ceil(maximum.x)),
                             static_cast<LONG>(std::ceil(maximum.y))};
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool hovered = visible && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
                         mouse.x >= minimum.x && mouse.x < maximum.x &&
                         mouse.y >= minimum.y && mouse.y < maximum.y;
    bool clicked = false;
    for (int button = 0; button < 3; ++button) clicked |= ImGui::IsMouseClicked(button);
    if (hovered && clicked) {
        gameInputSuspended_ = false;
        ImGui::SetWindowFocus();
    }
    const WinApp* window = ctx_->systems.winApp;
    const bool foreground = window != nullptr && GetForegroundWindow() == window->GetHwnd() &&
                            !IsIconic(window->GetHwnd());
    const bool focused = playerMode_ || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool active = visible && foreground && focused && !popup &&
                        !gameInputSuspended_ && playModeState_ == PlayModeState::Playing &&
                        !(clicked && !hovered);
    if (!active) {
        // A background game must not reacquire a requested lock by itself.
        if (gameInputFocused_ && !playerMode_) gameInputSuspended_ = true;
        ReleaseGameInputFocus();
    }
    gameInputFocused_ = active;
    gamePointerInside_ = active && hovered;
    input->RouteGameInput(active, gamePointerInside_, {mouse.x - minimum.x, mouse.y - minimum.y});
    if (visible) {
        input->SetUiQueryMode(true);
        ProcessGameUiInput(requestedGameWidth_, requestedGameHeight_, UpdateGameViewCamera());
        input->SetUiQueryMode(false);
    }
    ImGui::End();
}

void EditorScene::ApplyGameCursor() {
    if (ctx_ == nullptr || ctx_->systems.input == nullptr || ctx_->systems.winApp == nullptr) return;
    Input& input = *ctx_->systems.input;
    const bool running = gameInputFocused_ && showGamePanel_ && playModeState_ == PlayModeState::Playing;
    if (!running) input.RouteGameInput(false, false, input.GetPointerPosition());
    const bool show = !running || input.IsCursorVisibleRequested() ||
                      (!gamePointerInside_ && input.GetCursorMode() == CursorMode::Free);
    ctx_->systems.winApp->SetCursorVisible(show);
    if (show) {
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    } else {
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
    input.ApplyCursorMode(gameInputScreenBounds_);
}

void EditorScene::DrawGamePanelOverlay(const ImVec2& imageMin,
                                       const bool hasGameCamera) const {
    if (playerMode_) return;
    if (!hasGameCamera) {
        DrawGamePanelHint(imageMin, "No Primary Camera - displaying Runtime UI only",
                         IM_COL32(255, 196, 90, 240));
        return;
    }
    const Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (playModeState_ == PlayModeState::Playing && input != nullptr &&
        input->GetEffectiveCursorMode() != CursorMode::Free) {
        DrawGamePanelHint(imageMin, "Shift+Esc to return to Editor", IM_COL32(220, 225, 235, 230));
    }
}

void EditorScene::DrawGamePanelHint(const ImVec2& imageMin, const char* text,
                                    const uint32_t textColor) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 position{imageMin.x + 10.0f, imageMin.y + 10.0f};
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        {position.x - 4.0f, position.y - 2.0f},
        {position.x + size.x + 4.0f, position.y + size.y + 2.0f},
        IM_COL32(20, 24, 32, 190), 3.0f);
    drawList->AddText(position, textColor, text);
}
