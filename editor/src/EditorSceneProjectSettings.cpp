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

bool EditorScene::SavePhysicsSettings() {
    std::string error;
    if (!physicsSettingsStore_.Save(physicsSettings_, error)) {
        status_ = "Error: Could not save Physics Settings: " + error;
        return false;
    }
    world_.SetPhysicsSettings(physicsSettings_);
    physicsSettingsDirty_ = false;
    status_ = "Saved Physics Settings.";
    return true;
}

bool EditorScene::SavePlayerSettings() {
    std::string error;
    if (!playerSettingsStore_.Save(playerSettings_, error)) {
        status_ = "Error: Could not save Player Settings: " + error;
        return false;
    }
    playerSettingsDirty_ = false;
    status_ = "Saved Player Settings.";
    return true;
}

void EditorScene::DrawProjectSettingsWindow() {
    if (!showProjectSettings_) {
        return;
    }
    ImGui::SetNextWindowSize({760.0f, 620.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project Settings", &showProjectSettings_)) {
        ImGui::End();
        return;
    }

    DrawProjectGeneralSettings();
    DrawProjectPlayerSettings();
    DrawProjectPhysicsSettings();
    DrawProjectInputSettings();
    ImGui::End();
}

void EditorScene::DrawProjectGeneralSettings() {
    ImGui::SeparatorText("General");
    std::error_code startupSceneError;
    std::filesystem::path startupSceneLabel =
        std::filesystem::relative(startupScenePath_, sceneRoot_, startupSceneError);
    if (startupSceneError) {
        startupSceneLabel = startupScenePath_.filename();
    }
    ImGui::Text("Startup Scene");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", startupSceneLabel.generic_string().c_str());
    std::error_code currentSceneError;
    const bool currentSceneCanStart =
        !scenePath_.empty() && std::filesystem::is_regular_file(scenePath_, currentSceneError) &&
        !currentSceneError && scenePath_ != startupScenePath_;
    ImGui::BeginDisabled(IsInPlayMode() || !currentSceneCanStart);
    if (ImGui::Button("Set Current Scene as Startup")) {
        ProjectDescriptor project;
        std::string error;
        if (ProjectDescriptor::SetStartupScene(projectRoot_, scenePath_, project, error)) {
            startupScenePath_ = project.startupScene;
            std::error_code labelError;
            std::filesystem::path label =
                std::filesystem::relative(startupScenePath_, sceneRoot_, labelError);
            if (labelError) {
                label = startupScenePath_.filename();
            }
            status_ = "Set Startup Scene: " + label.generic_string();
        } else {
            status_ = "Error: Could not set Startup Scene: " + error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("The Player starts from this saved scene.");
}

void EditorScene::DrawProjectPlayerSettings() {
    ImGui::SeparatorText("Player");
    ImGui::TextDisabled("Project file: %s", playerSettingsStore_.Path().generic_string().c_str());
    ImGui::TextWrapped(
        "These settings apply the next time Player Preview or a packaged Player starts.");
    ImGui::BeginDisabled(IsInPlayMode());
    ImGui::BeginDisabled(!playerSettingsDirty_);
    if (ImGui::Button("Save##PlayerSettings")) {
        SavePlayerSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert##PlayerSettings")) {
        PlayerSettings restored{};
        std::string error;
        if (playerSettingsStore_.Load(restored, error)) {
            playerSettings_ = restored;
            playerSettingsDirty_ = false;
            status_ = "Reverted Player Settings.";
        } else {
            status_ = "Error: Could not reload Player Settings: " + error;
        }
    }
    ImGui::EndDisabled();
    if (playerSettingsDirty_) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.75f, 0.25f, 1.0f}, "Unsaved changes");
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputInt("Width", &playerSettings_.width)) {
        playerSettings_.width = std::clamp(playerSettings_.width, 320, 16384);
        playerSettingsDirty_ = true;
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputInt("Height", &playerSettings_.height)) {
        playerSettings_.height = std::clamp(playerSettings_.height, 180, 16384);
        playerSettingsDirty_ = true;
    }
    if (ImGui::Checkbox("Borderless Fullscreen", &playerSettings_.fullscreen)) {
        playerSettingsDirty_ = true;
    }
    ImGui::EndDisabled();
}

void EditorScene::DrawProjectPhysicsSettings() {
    ImGui::SeparatorText("Physics");
    ImGui::TextDisabled("Project file: %s", physicsSettingsStore_.Path().generic_string().c_str());
    ImGui::TextWrapped("Define project Layers and which Layer pairs are allowed to collide. "
                       "The matrix filters Character Controller blocking and Trigger events.");
    ImGui::BeginDisabled(IsInPlayMode());
    ImGui::BeginDisabled(!physicsSettingsDirty_);
    if (ImGui::Button("Save")) {
        SavePhysicsSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        PhysicsSettings restored{};
        std::string error;
        if (physicsSettingsStore_.Load(restored, error)) {
            physicsSettings_ = std::move(restored);
            world_.SetPhysicsSettings(physicsSettings_);
            physicsSettingsDirty_ = false;
            status_ = "Reverted Physics Settings.";
        } else {
            status_ = "Error: Could not reload Physics Settings: " + error;
        }
    }
    ImGui::EndDisabled();
    if (physicsSettingsDirty_) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.75f, 0.25f, 1.0f}, "Unsaved changes");
    }

    ImGui::SeparatorText("Layers");
    ImGui::TextDisabled("Layer 0 is reserved as Default. Empty Layer names are unused.");
    if (ImGui::BeginChild("PhysicsLayers", {0.0f, 220.0f}, ImGuiChildFlags_Borders)) {
        for (size_t index = 0u; index < PhysicsSettings::kLayerCount; ++index) {
            ImGui::PushID(static_cast<int>(index));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%2zu", index);
            ImGui::SameLine();
            std::array<char, 65> buffer{};
            strncpy_s(buffer.data(), buffer.size(), physicsSettings_.layerNames[index].c_str(),
                      _TRUNCATE);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::BeginDisabled(index == 0u);
            if (ImGui::InputText("##LayerName", buffer.data(), buffer.size())) {
                physicsSettings_.layerNames[index] = buffer.data();
                physicsSettingsDirty_ = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    std::vector<size_t> definedLayers;
    for (size_t index = 0u; index < PhysicsSettings::kLayerCount; ++index) {
        if (!physicsSettings_.layerNames[index].empty()) {
            definedLayers.push_back(index);
        }
    }
    ImGui::SeparatorText("Layer Collision Matrix");
    ImGui::TextDisabled("A checked pair will be allowed to collide.");
    std::vector<std::string> columnLabels;
    columnLabels.reserve(definedLayers.size());
    for (size_t layer : definedLayers) {
        columnLabels.push_back(std::to_string(layer));
    }
    constexpr ImGuiTableFlags matrixFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_ScrollX |
                                            ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("PhysicsCollisionMatrix", static_cast<int>(definedLayers.size() + 1u),
                          matrixFlags, {0.0f, 250.0f})) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        for (const std::string& label : columnLabels) {
            ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthFixed, 30.0f);
        }
        ImGui::TableHeadersRow();
        for (size_t row = 0u; row < definedLayers.size(); ++row) {
            const size_t first = definedLayers[row];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%zu: %s", first, physicsSettings_.layerNames[first].c_str());
            for (size_t column = 0u; column < definedLayers.size(); ++column) {
                ImGui::TableSetColumnIndex(static_cast<int>(column + 1u));
                if (column > row) {
                    continue;
                }
                const size_t second = definedLayers[column];
                bool collide = physicsSettings_.LayersCollide(first, second);
                ImGui::PushID(static_cast<int>(first));
                ImGui::PushID(static_cast<int>(second));
                if (ImGui::Checkbox("##Collide", &collide)) {
                    physicsSettings_.SetLayersCollide(first, second, collide);
                    world_.SetPhysicsSettings(physicsSettings_);
                    physicsSettingsDirty_ = true;
                }
                ImGui::PopID();
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
}
