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

void EditorScene::DrawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    DrawFileMenu();
    DrawBuildMenu();
    DrawEditMenu();
    DrawViewMenu();
    ImGui::Separator();
    DrawRuntimeControls();
    DrawEditorTitle();
    ImGui::EndMainMenuBar();
}

void EditorScene::DrawFileMenu() {
    if (ImGui::BeginMenu("File")) {
        const bool editing = !IsInPlayMode();
        if (ImGui::MenuItem("New Scene", "Ctrl+N", false, editing)) {
            RequestSceneAction(PendingSceneAction::NewScene);
        }
        if (ImGui::MenuItem("Open Scene...", "Ctrl+O", false, editing)) {
            RequestSceneAction(PendingSceneAction::OpenScene);
        }
        if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, editing)) {
            SaveScene();
        }
        if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, editing)) {
            SaveSceneAs();
        }
        if (ImGui::BeginMenu("Recent Scenes", editing && !recentScenePaths_.empty())) {
            for (const std::filesystem::path& path : recentScenePaths_) {
                std::error_code error;
                std::filesystem::path label = std::filesystem::relative(path, sceneRoot_, error);
                if (error) {
                    label = path.filename();
                }
                const std::string text = label.generic_string();
                if (ImGui::MenuItem(text.c_str())) {
                    RequestSceneAction(PendingSceneAction::OpenScene, path);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reload Scene", nullptr, false, editing)) {
            RequestSceneAction(PendingSceneAction::ReloadScene);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            RequestSceneAction(PendingSceneAction::Exit);
        }
        ImGui::EndMenu();
    }
}

void EditorScene::DrawBuildMenu() {
    if (ImGui::BeginMenu("Build")) {
        if (ImGui::MenuItem("Build Project", nullptr, false, !IsInPlayMode())) {
            BuildPlayerPackage();
        }
        if (ImGui::MenuItem("Build And Run", "F9", false, !IsInPlayMode())) {
            BuildAndRunPlayerPackage();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Run Project", "F8", false, !IsInPlayMode())) {
            LaunchPlayerPreview();
        }
        ImGui::EndMenu();
    }
}

void EditorScene::DrawEditMenu() {
    if (ImGui::BeginMenu("Edit")) {
        const bool editing = !IsInPlayMode();
        const bool canUndo = editing && !undoHistory_.empty();
        const bool canRedo = editing && !redoHistory_.empty();
        const bool canDuplicate = editing && world_.Contains(selection_);
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
            Undo();
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) {
            Redo();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A", false, !world_.Empty())) {
            SelectAllHierarchyEntities();
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, canDuplicate)) {
            CopySelection();
        }
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, canDuplicate)) {
            CutSelection();
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, editing && !entityClipboard_.empty())) {
            PasteEntityClipboard();
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, canDuplicate)) {
            DuplicateSelection();
        }
        if (ImGui::MenuItem("Delete", "Delete", false, canDuplicate)) {
            DeleteSelection();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Project Settings...")) {
            showProjectSettings_ = true;
        }
        ImGui::EndMenu();
    }
}

void EditorScene::DrawViewMenu() {
    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Hierarchy", nullptr, &showHierarchyPanel_);
            ImGui::MenuItem("Project", nullptr, &showProjectPanel_);
            ImGui::MenuItem("Scene", nullptr, &showScenePanel_);
            ImGui::MenuItem("Game", nullptr, &showGamePanel_);
            ImGui::MenuItem("Console", nullptr, &showConsolePanel_);
            ImGui::MenuItem("Inspector", nullptr, &showInspectorPanel_);
            ImGui::Separator();
            if (ImGui::MenuItem("Show All Panels")) {
                showHierarchyPanel_ = true;
                showProjectPanel_ = true;
                showScenePanel_ = true;
                showGamePanel_ = true;
                showConsolePanel_ = true;
                showInspectorPanel_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reset Panel Layout")) {
            showHierarchyPanel_ = true;
            showProjectPanel_ = true;
            showScenePanel_ = true;
            showGamePanel_ = true;
            showConsolePanel_ = true;
            showInspectorPanel_ = true;
            resetDockLayoutRequested_ = true;
        }
        ImGui::EndMenu();
    }
}

void EditorScene::DrawRuntimeControls() {
    const bool playing = playModeState_ == PlayModeState::Playing;
    const bool paused = playModeState_ == PlayModeState::Paused;
    if (paused) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.18f, 0.48f, 0.24f, 1.0f});
    }
    ImGui::BeginDisabled(playing);
    if (ImGui::Button(paused ? "Resume" : "Play")) {
        if (playModeState_ == PlayModeState::Edit) {
            EnterPlayMode();
        } else if (paused) {
            TogglePlayPause();
        }
    }
    ImGui::EndDisabled();
    if (paused) {
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (paused) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.58f, 0.40f, 0.12f, 1.0f});
    }
    if (ImGui::Button("Pause")) {
        TogglePlayPause();
    }
    if (paused) {
        ImGui::PopStyleColor();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!paused);
    if (ImGui::Button("Step")) {
        StepRuntimeWorld();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Advance the paused Runtime World by one 1/60-second update (F7).");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!IsInPlayMode());
    if (ImGui::Button("Stop")) {
        StopPlayMode();
    }
    ImGui::EndDisabled();
}

void EditorScene::DrawEditorTitle() {
    std::string editorLabel = "LikeEngine Editor - ";
    editorLabel += scenePath_.empty() ? "Untitled" : scenePath_.filename().string();
    if (dirty_) {
        editorLabel += " *";
    }
    const bool titlePlaying = playModeState_ == PlayModeState::Playing;
    const bool titlePaused = playModeState_ == PlayModeState::Paused;
    if (titlePlaying) {
        editorLabel += "  [PLAYING]";
    } else if (titlePaused) {
        editorLabel += "  [PAUSED]";
    }
    if (IsInPlayMode()) {
        char runtimeStatus[64]{};
        sprintf_s(runtimeStatus, "  Frame %llu | %.2fs",
                  static_cast<unsigned long long>(runtimeFrameCount_), runtimeElapsedSeconds_);
        editorLabel += runtimeStatus;
    }
    ImGui::TextUnformatted(editorLabel.c_str());
}
