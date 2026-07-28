#include "AssetImportPlanner.h"
#include "EditorScene.h"
#include "PlayerPackageBuilder.h"
#include "PlayerProjectValidator.h"
#include "ProjectDescriptor.h"
#include "RuntimeSceneLoader.h"
#include "ScriptAsset.h"
#include "ScriptBuildService.h"
#include "core/AssetManager.h"
#include "core/MathUtils.h"
#include "core/WinApp.h"
#include "font/TextRenderer.h"
#include "graphics/DirectXCommon.h"
#include "graphics/LightingScene.h"
#include "graphics/RenderScene.h"
#include "graphics/ShaderPaths.h"
#include "graphics/SrvManager.h"
#include "imgui.h"
#include "imgui/ImguiManager.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "input/Input.h"
#include "internal/EditorSceneViewportUtils.h"
#include "model/MeshRenderer.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "sound/ISoundService.h"
#include "sprite/SpriteRenderer.h"
#include "texture/TextureManager.h"
#include "world/WorldCollision.h"
#include "world/WorldSerializer.h"

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>

#ifdef DrawText
#undef DrawText
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace EditorSceneViewportUtils;

namespace {
constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoCollapse;
} // namespace

void EditorScene::DrawPanels() {
    sceneViewSurface_.ReleaseCompletedFrameResources();
    gameViewSurface_.ReleaseCompletedFrameResources();
    cameraPreviewSurface_.ReleaseCompletedFrameResources();
    assetPreviewSurface_.ReleaseCompletedFrameResources();
    projectPanelMinX_ = 0.0f;
    projectPanelMinY_ = 0.0f;
    projectPanelMaxX_ = 0.0f;
    projectPanelMaxY_ = 0.0f;
    Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (input != nullptr) {
        input->SetQueryEnabled(false, false, false);
    }
    if (gameInputCaptured_ && (playModeState_ != PlayModeState::Playing || !showGamePanel_)) {
        ReleaseGameInputCapture();
    }
    DrawHierarchyAndProjectPanels();
    DrawScenePanelWindow();
    DrawGamePanelWindow();
    DrawConsoleAndInspectorPanels();
}

void EditorScene::DrawHierarchyAndProjectPanels() {
    if (showHierarchyPanel_) {
        if (ImGui::Begin("Hierarchy", &showHierarchyPanel_, kPanelFlags)) {
            DrawHierarchyPanel();
        }
        ImGui::End();
    }

    if (showProjectPanel_) {
        if (ImGui::Begin("Project", &showProjectPanel_, kPanelFlags)) {
            ImGui::BeginDisabled(IsInPlayMode());
            DrawProjectPanel();
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
}

void EditorScene::DrawScenePanelWindow() {
    if (showScenePanel_) {
        if (ImGui::Begin("Scene", &showScenePanel_, kPanelFlags)) {
            DrawSceneGizmoToolbar();
            ImGui::Separator();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            requestedSceneWidth_ = (std::max)(1, static_cast<int>(std::lround(available.x)));
            requestedSceneHeight_ = (std::max)(1, static_cast<int>(std::lround(available.y)));
            if (sceneViewSurface_.IsReady() && sceneViewPostProcess_.IsReady() && ctx_ != nullptr &&
                ctx_->rendering.dxCommon != nullptr && ctx_->rendering.model != nullptr) {
                const ImVec2 expectedImageMin = ImGui::GetCursorScreenPos();
                const ImVec2 expectedImageMax = {
                    expectedImageMin.x + static_cast<float>(requestedSceneWidth_),
                    expectedImageMin.y + static_cast<float>(requestedSceneHeight_)};
                ImVec2 expectedPreviewMin{};
                ImVec2 expectedPreviewMax{};
                const WorldEntity* previewEntity = world_.Find(selection_);
                const bool cameraPreviewHovered =
                    previewEntity != nullptr && previewEntity->camera &&
                    cameraPreviewSurface_.IsReady() && cameraPreviewPostProcess_.IsReady() &&
                    TryGetCameraPreviewRect(expectedImageMin, expectedImageMax, expectedPreviewMin,
                                            expectedPreviewMax) &&
                    ImGui::IsMouseHoveringRect(expectedPreviewMin, expectedPreviewMax);
                const bool expectedImageHovered =
                    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                    ImGui::IsMouseHoveringRect(expectedImageMin, expectedImageMax) &&
                    !cameraPreviewHovered;
                HandleSceneCameraControls(expectedImageMin, expectedImageMax, expectedImageHovered);
                BuildRenderScene();
                BuildEditorOverlayScene();
                sceneRenderer_.Render(renderScene_, sceneViewCamera_, sceneViewSurface_,
                                      {0.025f, 0.035f, 0.055f, 1.0f}, &editorOverlayScene_);
                sceneViewSurface_.TransitionDepthToShaderResource();
                sceneViewSurface_.BeginOutputPass({0.0f, 0.0f, 0.0f, 1.0f});
                const PostProcessOutputTarget target{
                    sceneViewSurface_.GetOutputRtvHandle(),
                    static_cast<uint32_t>(sceneViewSurface_.GetWidth()),
                    static_cast<uint32_t>(sceneViewSurface_.GetHeight()),
                    DirectXCommon::kBackBufferFormat,
                };
                sceneViewPostProcess_.DrawToTarget(sceneViewSurface_.GetSceneColorGpuHandle(),
                                                   sceneViewSurface_.GetDepthGpuHandle(), target);
                sceneViewSurface_.EndOutputPass();
                sceneViewSurface_.TransitionDepthToWrite();
                ctx_->rendering.dxCommon->SetBackBufferRenderTarget(false, false);
                const D3D12_GPU_DESCRIPTOR_HANDLE output = sceneViewSurface_.GetOutputGpuHandle();
                ImGui::Image(static_cast<ImTextureID>(output.ptr),
                             ImVec2(static_cast<float>(requestedSceneWidth_),
                                    static_cast<float>(requestedSceneHeight_)));
                const ImVec2 imageMin = ImGui::GetItemRectMin();
                const ImVec2 imageMax = ImGui::GetItemRectMax();
                const bool imageHovered = ImGui::IsItemHovered() && !cameraPreviewHovered;
                if (!IsInPlayMode()) {
                    HandleSceneAssetDrop(imageMin, imageMax);
                    HandleSceneContextMenu(imageMin, imageMax, imageHovered);
                }
                DrawSceneComponentGizmos(imageMin, imageMax);
                DrawSceneSelectionOutline(imageMin, imageMax);
                bool sceneGizmoHovered = false;
                if (!IsInPlayMode()) {
                    if (boxColliderGizmoMode_ != BoxColliderGizmoMode::None) {
                        sceneGizmoHovered = DrawBoxColliderGizmo(imageMin, imageMax);
                    } else if (characterControllerGizmoMode_ !=
                               CharacterControllerGizmoMode::None) {
                        sceneGizmoHovered = DrawCharacterControllerGizmo(imageMin, imageMax);
                    } else {
                        sceneGizmoHovered = DrawSceneTransformGizmo(imageMin, imageMax);
                    }
                }
                if (IsInPlayMode() || !sceneGizmoHovered) {
                    PickSceneEntity(imageMin, imageMax, imageHovered);
                }
                if (imageHovered) {
                    constexpr const char* cameraHint =
                        "RMB Look  |  WASD/QE Move  |  MMB Pan  |  Wheel Dolly  |  F Focus";
                    const ImVec2 hintSize = ImGui::CalcTextSize(cameraHint);
                    const ImVec2 hintMin = {imageMin.x + 8.0f, imageMax.y - hintSize.y - 12.0f};
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled(
                        {hintMin.x - 4.0f, hintMin.y - 2.0f},
                        {hintMin.x + hintSize.x + 4.0f, hintMin.y + hintSize.y + 2.0f},
                        IM_COL32(20, 24, 32, 190), 3.0f);
                    drawList->AddText(hintMin, IM_COL32(220, 225, 235, 230), cameraHint);
                }
                DrawSelectedCameraPreview(imageMin, imageMax);
            } else {
                ImGui::TextDisabled("Scene View RenderSurface is not ready.");
            }
        }
        ImGui::End();
    }
}

void EditorScene::DrawGamePanelWindow() {
    Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (showGamePanel_) {
        if (focusGamePanelRequested_) {
            ImGui::SetNextWindowFocus();
            focusGamePanelRequested_ = false;
        }
        ImGuiWindowFlags gameWindowFlags = kPanelFlags;
        bool* gameWindowOpen = &showGamePanel_;
        if (playerMode_) {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            gameWindowFlags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoBringToFrontOnFocus;
            gameWindowOpen = nullptr;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        }
        const bool gameWindowVisible = ImGui::Begin("Game", gameWindowOpen, gameWindowFlags);
        if (playerMode_) {
            ImGui::PopStyleVar();
        }
        if (gameWindowVisible) {
            const bool gameViewFocused =
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (gameInputCaptured_ && !gameViewFocused) {
                ReleaseGameInputCapture();
                status_ = "Released Game input because the Game View lost focus.";
            }
            const ImVec2 available = ImGui::GetContentRegionAvail();
            requestedGameWidth_ = (std::max)(1, static_cast<int>(std::lround(available.x)));
            requestedGameHeight_ = (std::max)(1, static_cast<int>(std::lround(available.y)));
            if (!gameViewSurface_.IsReady() || !gameViewPostProcess_.IsReady() || ctx_ == nullptr ||
                ctx_->rendering.dxCommon == nullptr || ctx_->rendering.model == nullptr) {
                ReleaseGameInputCapture();
                ImGui::TextDisabled("Game View RenderSurface is not ready.");
            } else {
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
                const bool gameUiHovered = DrawGameUi(gameViewSurface_.GetWidth(),
                                                      gameViewSurface_.GetHeight(), hasGameCamera);
                gameViewSurface_.EndOutputPass();
                gameViewSurface_.TransitionDepthToWrite();
                ctx_->rendering.dxCommon->SetBackBufferRenderTarget(false, false);
                const D3D12_GPU_DESCRIPTOR_HANDLE output = gameViewSurface_.GetOutputGpuHandle();
                ImGui::Image(static_cast<ImTextureID>(output.ptr),
                             ImVec2(static_cast<float>(requestedGameWidth_),
                                    static_cast<float>(requestedGameHeight_)));
                const ImVec2 gameImageMin = ImGui::GetItemRectMin();
                const ImVec2 gameImageMax = ImGui::GetItemRectMax();
                const bool gameImageHovered = ImGui::IsItemHovered();
                if (playModeState_ == PlayModeState::Edit) {
                    HandleGameUiEditing(gameImageMin, gameImageMax);
                }
                if (hasGameCamera && playModeState_ == PlayModeState::Playing && gameImageHovered &&
                    !gameUiHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                    !gameInputCaptured_) {
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
                if (gameInputCaptured_) {
                    const int cursorCenterX =
                        static_cast<int>(std::lround((gameImageMin.x + gameImageMax.x) * 0.5f));
                    const int cursorCenterY =
                        static_cast<int>(std::lround((gameImageMin.y + gameImageMax.y) * 0.5f));
                    SetCursorPos(cursorCenterX, cursorCenterY);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                }
                if (input != nullptr && playModeState_ == PlayModeState::Playing) {
                    input->SetQueryEnabled(gameViewFocused,
                                           gameViewFocused &&
                                               (gameImageHovered || gameInputCaptured_),
                                           gameViewFocused);
                }
                if (!hasGameCamera && !playerMode_) {
                    constexpr const char* kNoCameraHint =
                        "No Primary Camera - displaying Runtime UI only";
                    const ImVec2 hintSize = ImGui::CalcTextSize(kNoCameraHint);
                    const ImVec2 hintMin{gameImageMin.x + 10.0f, gameImageMin.y + 10.0f};
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled(
                        {hintMin.x - 4.0f, hintMin.y - 2.0f},
                        {hintMin.x + hintSize.x + 4.0f, hintMin.y + hintSize.y + 2.0f},
                        IM_COL32(20, 24, 32, 190), 3.0f);
                    drawList->AddText(hintMin, IM_COL32(255, 196, 90, 240), kNoCameraHint);
                } else if (playModeState_ == PlayModeState::Playing) {
                    const char* captureHint = gameInputCaptured_
                                                  ? "Input captured - Esc to release"
                                                  : "Click Game View to capture input";
                    const ImVec2 hintSize = ImGui::CalcTextSize(captureHint);
                    const ImVec2 hintMin{gameImageMin.x + 10.0f, gameImageMin.y + 10.0f};
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled(
                        {hintMin.x - 4.0f, hintMin.y - 2.0f},
                        {hintMin.x + hintSize.x + 4.0f, hintMin.y + hintSize.y + 2.0f},
                        IM_COL32(20, 24, 32, 190), 3.0f);
                    drawList->AddText(hintMin, IM_COL32(220, 225, 235, 230), captureHint);
                }
            }
        }
        ImGui::End();
    }
}

void EditorScene::DrawConsoleAndInspectorPanels() {
    if (showConsolePanel_) {
        if (ImGui::Begin("Console", &showConsolePanel_, kPanelFlags)) {
            DrawConsolePanel();
        }
        ImGui::End();
    }

    if (showInspectorPanel_) {
        if (ImGui::Begin("Inspector", &showInspectorPanel_, kPanelFlags)) {
            ImGui::BeginDisabled(IsInPlayMode());
            DrawInspectorPanel();
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
}
