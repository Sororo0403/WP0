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

#include "internal/EditorSceneHierarchyUtils.h"

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

using namespace EditorSceneHierarchyUtils;

void EditorScene::DrawCanvasInspector(WorldEntity* entity) {
    if (entity->canvas) {
        ImGui::SeparatorText("Canvas");
        if (ImGui::Button("Remove Canvas")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->canvas.reset();
            RecordImmediateEdit("Remove Canvas", before, selectionBefore);
            status_ = "Removed Canvas.";
        } else {
            CanvasComponent& canvas = *entity->canvas;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Canvas", &canvas.enabled)) {
                RecordImmediateEdit("Toggle Canvas", std::move(before), selectionBefore);
            }
            const char* scaleMode = canvas.scaleMode == CanvasScaleMode::ConstantPixelSize
                                        ? "Constant Pixel Size"
                                        : "Scale With Screen Size";
            if (ImGui::BeginCombo("UI Scale Mode##Canvas", scaleMode)) {
                const auto selectScaleMode = [&](CanvasScaleMode value, const char* label) {
                    if (ImGui::Selectable(label, canvas.scaleMode == value)) {
                        const std::string modeBefore = WorldSerializer::Serialize(world_);
                        canvas.scaleMode = value;
                        RecordImmediateEdit("Change Canvas Scale Mode", std::move(modeBefore),
                                            selectionBefore);
                        status_ = "Changed Canvas scale mode.";
                    }
                };
                selectScaleMode(CanvasScaleMode::ConstantPixelSize, "Constant Pixel Size");
                selectScaleMode(CanvasScaleMode::ScaleWithScreenSize, "Scale With Screen Size");
                ImGui::EndCombo();
            }
            if (canvas.scaleMode == CanvasScaleMode::ScaleWithScreenSize) {
                float resolution[2]{
                    canvas.referenceResolution.x,
                    canvas.referenceResolution.y,
                };
                if (ImGui::DragFloat2("Reference Resolution##Canvas", resolution, 1.0f, 1.0f,
                                      16384.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp)) {
                    canvas.referenceResolution = {resolution[0], resolution[1]};
                    RefreshDirty();
                    status_ = "Modified Canvas reference resolution.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Canvas");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
                const char* screenMatchMode =
                    canvas.screenMatchMode == CanvasScreenMatchMode::MatchWidthOrHeight
                        ? "Match Width Or Height"
                    : canvas.screenMatchMode == CanvasScreenMatchMode::Shrink ? "Shrink"
                                                                              : "Expand";
                if (ImGui::BeginCombo("Screen Match Mode##Canvas", screenMatchMode)) {
                    const auto selectScreenMatchMode = [&](CanvasScreenMatchMode value,
                                                           const char* label) {
                        if (ImGui::Selectable(label, canvas.screenMatchMode == value)) {
                            const std::string modeBefore = WorldSerializer::Serialize(world_);
                            canvas.screenMatchMode = value;
                            RecordImmediateEdit("Change Canvas Screen Match Mode",
                                                std::move(modeBefore), selectionBefore);
                            status_ = "Changed Canvas screen match mode.";
                        }
                    };
                    selectScreenMatchMode(CanvasScreenMatchMode::MatchWidthOrHeight,
                                          "Match Width Or Height");
                    selectScreenMatchMode(CanvasScreenMatchMode::Expand, "Expand");
                    selectScreenMatchMode(CanvasScreenMatchMode::Shrink, "Shrink");
                    ImGui::EndCombo();
                }
                if (canvas.screenMatchMode == CanvasScreenMatchMode::MatchWidthOrHeight) {
                    if (ImGui::SliderFloat("Match##Canvas", &canvas.matchWidthOrHeight, 0.0f, 1.0f,
                                           "%.2f")) {
                        RefreshDirty();
                        status_ = "Modified Canvas screen match value.";
                    }
                    if (ImGui::IsItemActivated()) {
                        BeginHistoryEdit("Modify Canvas Screen Match");
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        CommitHistoryEdit();
                    }
                    ImGui::TextDisabled("0 = Width, 1 = Height");
                }
            }
            if (ImGui::DragInt("Sorting Order##Canvas", &canvas.sortingOrder, 1.0f, -1000000,
                               1000000, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Canvas sorting order.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Canvas Sorting Order");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            ImGui::TextDisabled("Higher sorting orders are drawn and selected on top.");
        }
    }
}

void EditorScene::DrawCanvasGroupInspector(WorldEntity* entity) {
    if (entity->canvasGroup) {
        ImGui::SeparatorText("Canvas Group");
        if (ImGui::Button("Remove Canvas Group")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->canvasGroup.reset();
            RecordImmediateEdit("Remove CanvasGroup", before, selectionBefore);
            status_ = "Removed Canvas Group.";
        } else {
            CanvasGroupComponent& group = *entity->canvasGroup;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##CanvasGroup", &group.enabled)) {
                RecordImmediateEdit("Toggle CanvasGroup", std::move(before), selectionBefore);
                status_ = "Toggled Canvas Group.";
            }
            if (ImGui::SliderFloat("Alpha##CanvasGroup", &group.alpha, 0.0f, 1.0f, "%.2f")) {
                RefreshDirty();
                status_ = "Modified Canvas Group alpha.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify CanvasGroup Alpha");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Interactable##CanvasGroup", &group.interactable)) {
                RecordImmediateEdit("Toggle CanvasGroup Interactable", std::move(before),
                                    selectionBefore);
                status_ = "Toggled Canvas Group interaction.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Blocks Raycasts##CanvasGroup", &group.blocksRaycasts)) {
                RecordImmediateEdit("Toggle CanvasGroup Raycasts", std::move(before),
                                    selectionBefore);
                status_ = "Toggled Canvas Group raycast blocking.";
            }
            ImGui::TextDisabled("Settings affect UI on this Entity and its children.");
        }
    }
}

void EditorScene::DrawEventSystemInspector(WorldEntity* entity) {
    if (entity->eventSystem) {
        ImGui::SeparatorText("Event System");
        if (ImGui::Button("Remove Event System")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->eventSystem.reset();
            RecordImmediateEdit("Remove EventSystem", before, selectionBefore);
            status_ = "Removed Event System.";
        } else {
            EventSystemComponent& eventSystem = *entity->eventSystem;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##EventSystem", &eventSystem.enabled)) {
                RecordImmediateEdit("Toggle EventSystem", std::move(before), selectionBefore);
                status_ = "Toggled Event System.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Send Navigation Events##EventSystem",
                                &eventSystem.sendNavigationEvents)) {
                RecordImmediateEdit("Toggle EventSystem Navigation", std::move(before),
                                    selectionBefore);
                status_ = "Toggled Event System navigation events.";
            }

            const WorldEntity* selectedEntity = world_.Find(eventSystem.firstSelected);
            std::string selectedLabel = selectedEntity != nullptr             ? selectedEntity->name
                                        : eventSystem.firstSelected.IsValid() ? "Missing Entity"
                                                                              : "None";
            selectedLabel += "##EventSystemFirstSelected";
            ImGui::TextUnformatted("First Selected");
            ImGui::SameLine();
            if (ImGui::Button(selectedLabel.c_str(), {-FLT_MIN, 0.0f})) {
                ImGui::OpenPopup("EventSystemFirstSelectedPicker");
            }
            const auto assignFirstSelected = [&](EntityId value) {
                if (eventSystem.firstSelected == value) {
                    return;
                }
                const std::string targetBefore = WorldSerializer::Serialize(world_);
                eventSystem.firstSelected = value;
                RecordImmediateEdit("Assign EventSystem First Selected", targetBefore,
                                    selectionBefore);
                status_ = "Assigned Event System first selection.";
            };
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload);
                    payload != nullptr && payload->IsDelivery() &&
                    payload->DataSize == sizeof(EntityId)) {
                    EntityId dropped{};
                    std::memcpy(&dropped, payload->Data, sizeof(dropped));
                    const WorldEntity* droppedEntity = world_.Find(dropped);
                    if (droppedEntity != nullptr &&
                        (droppedEntity->button || droppedEntity->slider)) {
                        assignFirstSelected(dropped);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopup("EventSystemFirstSelectedPicker")) {
                if (ImGui::MenuItem("None", nullptr, !eventSystem.firstSelected.IsValid())) {
                    assignFirstSelected({});
                }
                ImGui::Separator();
                for (const WorldEntity& candidate : world_.Entities()) {
                    if (!candidate.button && !candidate.slider) {
                        continue;
                    }
                    const std::string candidateLabel =
                        candidate.name + "##" + candidate.id.ToString();
                    if (ImGui::MenuItem(candidateLabel.c_str(), nullptr,
                                        candidate.id == eventSystem.firstSelected)) {
                        assignFirstSelected(candidate.id);
                    }
                }
                ImGui::EndPopup();
            }
            const size_t eventSystemCount = static_cast<size_t>(
                std::ranges::count_if(world_.Entities(), [](const WorldEntity& candidate) {
                    return candidate.eventSystem && candidate.eventSystem->enabled;
                }));
            if (eventSystemCount > 1u) {
                ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                                   "Multiple enabled Event Systems exist.");
            }
        }
    }
}
