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

void EditorScene::DrawBoxColliderInspector(WorldEntity* entity) {
    if (entity->boxCollider) {
        ImGui::SeparatorText("Box Collider");
        if (ImGui::Button("Remove Box Collider")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->boxCollider.reset();
            if (boxColliderGizmoEntity_ == selection_) {
                boxColliderGizmoMode_ = BoxColliderGizmoMode::None;
                boxColliderGizmoEntity_ = {};
            }
            RecordImmediateEdit("Remove BoxCollider", before, selectionBefore);
            status_ = "Removed BoxCollider.";
        } else {
            BoxColliderComponent& collider = *entity->boxCollider;
            auto colliderGizmoButton = [&](const char* label, BoxColliderGizmoMode mode) {
                const bool selected =
                    boxColliderGizmoEntity_ == selection_ && boxColliderGizmoMode_ == mode;
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                }
                if (ImGui::Button(label)) {
                    boxColliderGizmoMode_ = selected ? BoxColliderGizmoMode::None : mode;
                    boxColliderGizmoEntity_ = selected ? EntityId{} : selection_;
                    characterControllerGizmoMode_ = CharacterControllerGizmoMode::None;
                    characterControllerGizmoEntity_ = {};
                }
                if (selected) {
                    ImGui::PopStyleColor();
                }
            };
            colliderGizmoButton("Edit Center", BoxColliderGizmoMode::Center);
            ImGui::SameLine();
            colliderGizmoButton("Edit Size", BoxColliderGizmoMode::Size);
            if (boxColliderGizmoEntity_ == selection_ &&
                boxColliderGizmoMode_ != BoxColliderGizmoMode::None) {
                ImGui::TextDisabled("Editing in Scene View. Click the active button to finish.");
            }
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##BoxCollider", &collider.enabled)) {
                RecordImmediateEdit("Toggle BoxCollider", std::move(before), selectionBefore);
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Is Trigger##BoxCollider", &collider.isTrigger)) {
                RecordImmediateEdit("Toggle BoxCollider Trigger", std::move(before),
                                    selectionBefore);
            }
            if (ImGui::DragFloat3("Center##BoxCollider", &collider.center.x, 0.02f)) {
                RefreshDirty();
                status_ = "Modified BoxCollider.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify BoxCollider Center");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragFloat3("Size##BoxCollider", &collider.size.x, 0.02f, 0.001f, 1000000.0f,
                                  "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
                collider.size.x = (std::max)(0.001f, collider.size.x);
                collider.size.y = (std::max)(0.001f, collider.size.y);
                collider.size.z = (std::max)(0.001f, collider.size.z);
                RefreshDirty();
                status_ = "Modified BoxCollider.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify BoxCollider Size");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
        }
    }
}

void EditorScene::DrawCharacterControllerInspector(WorldEntity* entity) {
    if (entity->characterController) {
        ImGui::SeparatorText("Character Controller");
        const bool requiredByBehavior =
            std::ranges::any_of(entity->scripts, [this](const BehaviorComponent& script) {
                const BehaviorRequirements* requirements =
                    behaviorRegistry_.Requirements(script.type);
                return requirements != nullptr && requirements->characterController;
            });
        ImGui::BeginDisabled(requiredByBehavior);
        const bool removeRequested = ImGui::Button("Remove Character Controller");
        ImGui::EndDisabled();
        if (requiredByBehavior && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Required by the assigned Behavior.");
        }
        if (removeRequested) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->characterController.reset();
            if (characterControllerGizmoEntity_ == selection_) {
                characterControllerGizmoMode_ = CharacterControllerGizmoMode::None;
                characterControllerGizmoEntity_ = {};
            }
            RecordImmediateEdit("Remove CharacterController", before, selectionBefore);
            status_ = "Removed CharacterController.";
        } else {
            CharacterControllerComponent& controller = *entity->characterController;
            auto controllerGizmoButton = [&](const char* label, CharacterControllerGizmoMode mode) {
                const bool selected = characterControllerGizmoEntity_ == selection_ &&
                                      characterControllerGizmoMode_ == mode;
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                }
                if (ImGui::Button(label)) {
                    characterControllerGizmoMode_ =
                        selected ? CharacterControllerGizmoMode::None : mode;
                    characterControllerGizmoEntity_ = selected ? EntityId{} : selection_;
                    boxColliderGizmoMode_ = BoxColliderGizmoMode::None;
                    boxColliderGizmoEntity_ = {};
                }
                if (selected) {
                    ImGui::PopStyleColor();
                }
            };
            controllerGizmoButton("Edit Center##CharacterController",
                                  CharacterControllerGizmoMode::Center);
            ImGui::SameLine();
            controllerGizmoButton("Edit Radius##CharacterController",
                                  CharacterControllerGizmoMode::Radius);
            ImGui::SameLine();
            controllerGizmoButton("Edit Height##CharacterController",
                                  CharacterControllerGizmoMode::Height);
            if (characterControllerGizmoEntity_ == selection_ &&
                characterControllerGizmoMode_ != CharacterControllerGizmoMode::None) {
                ImGui::TextDisabled("Editing in Scene View. Click the active button to finish.");
            }
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##CharacterController", &controller.enabled)) {
                RecordImmediateEdit("Toggle CharacterController", std::move(before),
                                    selectionBefore);
            }
            if (ImGui::DragFloat3("Center##CharacterController", &controller.center.x, 0.02f)) {
                RefreshDirty();
                status_ = "Modified CharacterController.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify CharacterController Center");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            auto drawControllerFloat = [&](const char* label, float& value, float minimum,
                                           float maximum) {
                if (ImGui::DragFloat(label, &value, 0.01f, minimum, maximum, "%.3f",
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified CharacterController.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify CharacterController");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            drawControllerFloat("Radius##CharacterController", controller.radius, 0.001f,
                                1000000.0f);
            controller.height = (std::max)(controller.height, controller.radius * 2.0f);
            controller.skinWidth =
                (std::min)(controller.skinWidth, (std::max)(0.0f, controller.radius - 0.001f));
            drawControllerFloat("Height##CharacterController", controller.height,
                                controller.radius * 2.0f, 1000000.0f);
            controller.stepOffset = (std::min)(controller.stepOffset, controller.height);
            drawControllerFloat("Slope Limit##CharacterController", controller.slopeLimitDegrees,
                                0.0f, 90.0f);
            drawControllerFloat("Step Offset##CharacterController", controller.stepOffset, 0.0f,
                                controller.height);
            drawControllerFloat("Skin Width##CharacterController", controller.skinWidth, 0.0f,
                                (std::max)(0.0f, controller.radius - 0.001f));
            drawControllerFloat("Min Move Distance##CharacterController",
                                controller.minMoveDistance, 0.0f, 1000000.0f);
        }
    }
}
