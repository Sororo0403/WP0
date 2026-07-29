#include "EditorScene.h"

#include "graphics/DirectXCommon.h"
#include "imgui.h"
#include "input/Input.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr ImGuiWindowFlags kGamePanelFlags = ImGuiWindowFlags_NoCollapse;
}

void EditorScene::DrawGamePanelWindow() {
    if (!showGamePanel_) {
        return;
    }
    const bool visible = BeginGamePanelWindow();
    if (visible) {
        DrawGamePanelContent(ctx_ != nullptr ? ctx_->systems.input : nullptr);
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

void EditorScene::DrawGamePanelContent(Input* input) {
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (gameInputCaptured_ && !focused) {
        ReleaseGameInputCapture();
        status_ = "Released Game input because the Game View lost focus.";
    }
    UpdateGamePanelRequestedSize();
    if (!IsGamePanelRenderReady()) {
        ReleaseGameInputCapture();
        ImGui::TextDisabled("Game View RenderSurface is not ready.");
        return;
    }
    bool gameUiHovered = false;
    const bool hasGameCamera = RenderGamePanelFrame(gameUiHovered);
    ImVec2 imageMin{};
    ImVec2 imageMax{};
    DrawGamePanelImage(imageMin, imageMax);
    HandleGamePanelInteraction(imageMin, imageMax, hasGameCamera, gameUiHovered, focused, input);
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

bool EditorScene::RenderGamePanelFrame(bool& gameUiHovered) {
    const bool hasGameCamera = UpdateGameViewCamera();
    if (hasGameCamera) {
        BuildRenderScene();
    } else {
        renderScene_.BeginFrame();
        ReleaseGameInputCapture();
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
    gameUiHovered =
        DrawGameUi(gameViewSurface_.GetWidth(), gameViewSurface_.GetHeight(), hasGameCamera);
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

void EditorScene::HandleGamePanelInteraction(
    const ImVec2& imageMin, const ImVec2& imageMax, const bool hasGameCamera,
    const bool gameUiHovered, const bool gameViewFocused, Input* input) {
    const bool imageHovered = ImGui::IsItemHovered();
    if (playModeState_ == PlayModeState::Edit) {
        HandleGameUiEditing(imageMin, imageMax);
    }
    TryCaptureGameInput(hasGameCamera, gameUiHovered, imageHovered);
    CenterCapturedGameCursor(imageMin, imageMax);
    UpdateGameInputQuery(input, gameViewFocused, imageHovered);
    DrawGamePanelOverlay(imageMin, hasGameCamera);
}

void EditorScene::TryCaptureGameInput(const bool hasGameCamera,
                                      const bool gameUiHovered,
                                      const bool gameImageHovered) {
    if (!hasGameCamera || playModeState_ != PlayModeState::Playing ||
        !gameImageHovered || gameUiHovered ||
        !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || gameInputCaptured_) {
        return;
    }
    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        gameInputCursorRestoreX_ = cursor.x;
        gameInputCursorRestoreY_ = cursor.y;
    }
    focusedButton_ = {};
    pressedButton_ = {};
    activeSlider_ = {};
    openDropdown_ = {};
    activeInputField_ = {};
    gameInputCaptured_ = true;
    status_ = "Game input captured. Press Escape to release.";
}

void EditorScene::CenterCapturedGameCursor(const ImVec2& imageMin,
                                           const ImVec2& imageMax) {
    if (!gameInputCaptured_) {
        return;
    }
    const int centerX =
        static_cast<int>(std::lround((imageMin.x + imageMax.x) * 0.5f));
    const int centerY =
        static_cast<int>(std::lround((imageMin.y + imageMax.y) * 0.5f));
    SetCursorPos(centerX, centerY);
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
}

void EditorScene::UpdateGameInputQuery(Input* input, const bool gameViewFocused,
                                       const bool gameImageHovered) {
    if (input == nullptr || playModeState_ != PlayModeState::Playing) {
        return;
    }
    input->SetQueryEnabled(gameViewFocused,
                           gameViewFocused && (gameImageHovered || gameInputCaptured_),
                           gameViewFocused);
}

void EditorScene::DrawGamePanelOverlay(const ImVec2& imageMin,
                                       const bool hasGameCamera) const {
    if (!hasGameCamera && !playerMode_) {
        DrawGamePanelHint(imageMin, "No Primary Camera - displaying Runtime UI only",
                          IM_COL32(255, 196, 90, 240));
        return;
    }
    if (playModeState_ != PlayModeState::Playing) {
        return;
    }
    const char* hint = gameInputCaptured_ ? "Input captured - Esc to release"
                                          : "Click Game View to capture input";
    DrawGamePanelHint(imageMin, hint, IM_COL32(220, 225, 235, 230));
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
