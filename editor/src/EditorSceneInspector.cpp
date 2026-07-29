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

void EditorScene::DrawInspectorPanel() {
    WorldEntity* entity = world_.Find(selection_);
    if (entity == nullptr) {
        ImGui::TextDisabled("Nothing selected.");
        return;
    }

    std::vector<EntityId> inspectedEntities;
    if (hierarchySelection_.size() > 1u && hierarchySelection_.contains(selection_)) {
        inspectedEntities.reserve(hierarchySelection_.size());
        for (const WorldEntity& candidate : world_.Entities()) {
            if (hierarchySelection_.contains(candidate.id)) {
                inspectedEntities.push_back(candidate.id);
            }
        }
    } else {
        inspectedEntities.push_back(selection_);
    }
    DrawEntityHeaderAndTransformInspector(entity, inspectedEntities);
    DrawAddComponentInspector(entity);
    DrawScriptsInspector(entity);
    DrawBoxColliderInspector(entity);
    DrawCharacterControllerInspector(entity);
    DrawCameraInspector(entity);
    DrawLightInspector(entity);
    DrawAudioSourceInspector(entity);
    DrawAudioListenerInspector(entity);
    DrawAnimatorInspector(entity);
    DrawCanvasInspector(entity);
    DrawCanvasGroupInspector(entity);
    DrawEventSystemInspector(entity);
    DrawTextInspector(entity);
    DrawImageInspector(entity);
    DrawButtonInspector(entity);
    DrawToggleInspector(entity);
    DrawSliderInspector(entity);
    DrawDropdownInspector(entity);
    DrawInputFieldInspector(entity);
    DrawMaterialOverrideInspector(entity);
    DrawMeshRendererInspector(entity);
}

void EditorScene::DrawMeshRendererInspector(WorldEntity* entity) {
    if (!entity->meshRenderer) {
        return;
    }
    ImGui::SeparatorText("Mesh Renderer");
    MeshRendererComponent& renderer = *entity->meshRenderer;
    if (ImGui::Button("Remove Mesh Renderer")) {
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        entity->meshRenderer.reset();
        RecordImmediateEdit("Remove MeshRenderer", before, selectionBefore);
        status_ = "Removed MeshRenderer.";
        return;
    }
    std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (ImGui::Checkbox("Enabled", &renderer.enabled)) {
        RecordImmediateEdit("Toggle MeshRenderer", std::move(before), selectionBefore);
    }
    int source = static_cast<int>(renderer.sourceType);
    before = WorldSerializer::Serialize(world_);
    if (ImGui::Combo("Source", &source, "Primitive\0Model\0")) {
        renderer.sourceType = static_cast<MeshSourceType>(source);
        RecordImmediateEdit("Change Mesh Source", std::move(before), selectionBefore);
    }
    if (renderer.sourceType == MeshSourceType::Primitive) {
        int primitive = static_cast<int>(renderer.primitive);
        before = WorldSerializer::Serialize(world_);
        if (ImGui::Combo("Primitive", &primitive, kPrimitiveNames,
                         static_cast<int>(std::size(kPrimitiveNames)))) {
            renderer.primitive = static_cast<MeshPrimitive>(primitive);
            RecordImmediateEdit("Change Primitive", std::move(before), selectionBefore);
        }
    } else {
        std::array<char, 512> pathBuffer{};
        strncpy_s(pathBuffer.data(), pathBuffer.size(), renderer.modelPath.c_str(), _TRUNCATE);
        if (ImGui::InputText("Model", pathBuffer.data(), pathBuffer.size(),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            AssignModelAsset(selection_, pathBuffer.data());
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kModelAssetDragPayload);
                payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                AssignModelAsset(selection_, static_cast<const char*>(payload->Data));
            }
            ImGui::EndDragDropTarget();
        }
    }
}

void EditorScene::DrawEntityHeaderAndTransformInspector(
    WorldEntity* entity, const std::vector<EntityId>& inspectedEntities) {
    const bool multipleEntities = inspectedEntities.size() > 1u;
    bool displayedActive = entity->active;
    const bool mixedActive = std::ranges::any_of(inspectedEntities, [&](EntityId inspected) {
        const WorldEntity* target = world_.Find(inspected);
        return target != nullptr && target->active != displayedActive;
    });
    if (mixedActive) {
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    }
    if (ImGui::Checkbox("Active", &displayedActive)) {
        SetSelectedEntitiesActive(selection_, displayedActive);
    }
    if (mixedActive) {
        ImGui::PopItemFlag();
    }

    if (multipleEntities) {
        ImGui::Text("%zu Entities Selected", inspectedEntities.size());
        ImGui::TextDisabled("Transform changes preserve relative offsets and scale ratios.");
    } else {
        std::array<char, 256> nameBuffer{};
        strncpy_s(nameBuffer.data(), nameBuffer.size(), entity->name.c_str(), _TRUNCATE);
        const bool nameChanged = ImGui::InputText("Name", nameBuffer.data(), nameBuffer.size());
        if (ImGui::IsItemActivated()) {
            BeginHistoryEdit("Rename Entity");
        }
        if (nameChanged) {
            entity->name = nameBuffer.data();
            if (entity->name.empty()) {
                entity->name = "Entity";
            }
            RefreshDirty();
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            CommitHistoryEdit();
        }
        ImGui::TextDisabled("ID: %s", entity->id.ToString().c_str());
    }
    uint8_t displayedLayer = entity->layer;
    const bool mixedLayers = std::ranges::any_of(inspectedEntities, [&](EntityId inspected) {
        const WorldEntity* target = world_.Find(inspected);
        return target != nullptr && target->layer != displayedLayer;
    });
    std::string layerPreview = mixedLayers ? "Mixed" : physicsSettings_.layerNames[displayedLayer];
    if (layerPreview.empty()) {
        layerPreview = "Layer " + std::to_string(displayedLayer) + " (Undefined)";
    }
    if (ImGui::BeginCombo("Layer", layerPreview.c_str())) {
        for (size_t layer = 0u; layer < PhysicsSettings::kLayerCount; ++layer) {
            if (physicsSettings_.layerNames[layer].empty()) {
                continue;
            }
            const bool selected = !mixedLayers && displayedLayer == layer;
            const std::string label =
                std::to_string(layer) + ": " + physicsSettings_.layerNames[layer];
            if (ImGui::Selectable(label.c_str(), selected)) {
                CommitHistoryEdit();
                const std::string before = WorldSerializer::Serialize(world_);
                const EntityId selectionBefore = selection_;
                for (EntityId inspected : inspectedEntities) {
                    if (WorldEntity* target = world_.Find(inspected)) {
                        target->layer = static_cast<uint8_t>(layer);
                    }
                }
                RecordImmediateEdit(inspectedEntities.size() > 1u ? "Set Entity Layers"
                                                                  : "Set Entity Layer",
                                    before, selectionBefore);
                status_ = inspectedEntities.size() > 1u ? "Changed the selected Entity Layers."
                                                        : "Changed the Entity Layer.";
                displayedLayer = static_cast<uint8_t>(layer);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Separator();
    ImGui::TextUnformatted(multipleEntities ? "Transforms" : "Transform");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##Transform")) {
        CommitHistoryEdit();
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        for (EntityId inspected : inspectedEntities) {
            if (WorldEntity* target = world_.Find(inspected)) {
                target->transform = TransformComponent{};
            }
        }
        RecordImmediateEdit(multipleEntities ? "Reset Transforms" : "Reset Transform", before,
                            selectionBefore);
        status_ = multipleEntities ? "Reset selected Transforms." : "Reset Transform.";
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##Transform")) {
        CommitHistoryEdit();
        transformClipboard_ = entity->transform;
        status_ = "Copied Transform.";
    }
    ImGui::SameLine();
    if (!transformClipboard_) {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton("Paste##Transform")) {
        CommitHistoryEdit();
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        for (EntityId inspected : inspectedEntities) {
            if (WorldEntity* target = world_.Find(inspected)) {
                target->transform = *transformClipboard_;
            }
        }
        RecordImmediateEdit(multipleEntities ? "Paste Transforms" : "Paste Transform", before,
                            selectionBefore);
        status_ = multipleEntities ? "Pasted Transform to selected entities." : "Pasted Transform.";
    }
    if (!transformClipboard_) {
        ImGui::EndDisabled();
    }
    auto drawTransform = [&](const char* label, DirectX::XMFLOAT3 TransformComponent::* member,
                             float speed, bool scale) {
        const DirectX::XMFLOAT3 previous = entity->transform.*member;
        DirectX::XMFLOAT3 edited = previous;
        const bool changed = ImGui::DragFloat3(label, &edited.x, speed);
        if (ImGui::IsItemActivated()) {
            BeginHistoryEdit(
                std::string(multipleEntities ? "Modify Transforms " : "Modify Transform ") + label);
        }
        if (changed) {
            const DirectX::XMFLOAT3 delta{edited.x - previous.x, edited.y - previous.y,
                                          edited.z - previous.z};
            auto applyComponent = [scale](float current, float oldActive, float newActive,
                                          float additiveDelta) {
                constexpr float epsilon = 1.0e-6f;
                if (scale && std::abs(oldActive) > epsilon) {
                    return current * (newActive / oldActive);
                }
                return current + additiveDelta;
            };
            for (EntityId inspected : inspectedEntities) {
                WorldEntity* target = world_.Find(inspected);
                if (target == nullptr) {
                    continue;
                }
                DirectX::XMFLOAT3& value = target->transform.*member;
                if (inspected == selection_) {
                    value = edited;
                } else {
                    value = {applyComponent(value.x, previous.x, edited.x, delta.x),
                             applyComponent(value.y, previous.y, edited.y, delta.y),
                             applyComponent(value.z, previous.z, edited.z, delta.z)};
                }
            }
            RefreshDirty();
            status_ = multipleEntities ? "Modified selected Transforms." : "Modified Transform.";
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            CommitHistoryEdit();
        }
    };
    drawTransform("Position", &TransformComponent::position, 0.05f, false);
    drawTransform("Rotation", &TransformComponent::rotationDegrees, 0.25f, false);
    drawTransform("Scale", &TransformComponent::scale, 0.02f, true);

    if (multipleEntities) {
        ImGui::Separator();
        ImGui::TextDisabled("Component editing is available when one Entity is selected.");
        return;
    }
}

void EditorScene::DrawAddComponentInspector(WorldEntity* entity) {
    ImGui::Separator();
    if (ImGui::Button("Add Component")) {
        ImGui::OpenPopup("AddComponentMenu");
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kScriptAssetDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
            static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            AssignScriptAsset(selection_, static_cast<const char*>(payload->Data));
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopup("AddComponentMenu")) {
        if (!entity->meshRenderer && ImGui::MenuItem("Mesh Renderer")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->meshRenderer = MeshRendererComponent{};
            RecordImmediateEdit("Add MeshRenderer", before, selectionBefore);
            status_ = "Added MeshRenderer.";
        }
        if (!entity->materialOverride && ImGui::MenuItem("Material Override")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->materialOverride = MaterialOverrideComponent{};
            RecordImmediateEdit("Add Material Override", before, selectionBefore);
            status_ = "Added Material Override.";
        }
        if (!entity->camera && ImGui::MenuItem("Camera")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->camera = CameraComponent{};
            RecordImmediateEdit("Add Camera", before, selectionBefore);
            status_ = "Added Camera.";
        }
        if (!entity->light && ImGui::MenuItem("Light")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->light = LightComponent{};
            RecordImmediateEdit("Add Light", before, selectionBefore);
            status_ = "Added Light.";
        }
        if (!entity->audioSource && ImGui::MenuItem("Audio Source")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->audioSource = AudioSourceComponent{};
            RecordImmediateEdit("Add AudioSource", before, selectionBefore);
            status_ = "Added AudioSource.";
        }
        if (!entity->audioListener && ImGui::MenuItem("Audio Listener")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->audioListener = AudioListenerComponent{};
            RecordImmediateEdit("Add AudioListener", before, selectionBefore);
            status_ = "Added AudioListener.";
        }
        if (!entity->animator && ImGui::MenuItem("Animator")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->animator = AnimatorComponent{};
            RecordImmediateEdit("Add Animator", before, selectionBefore);
            status_ = "Added Animator.";
        }
        if (!entity->canvas && ImGui::MenuItem("Canvas")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->canvas = CanvasComponent{};
            RecordImmediateEdit("Add Canvas", before, selectionBefore);
            status_ = "Added Canvas.";
        }
        if (!entity->canvasGroup && ImGui::MenuItem("Canvas Group")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->canvasGroup = CanvasGroupComponent{};
            RecordImmediateEdit("Add CanvasGroup", before, selectionBefore);
            status_ = "Added Canvas Group.";
        }
        if (!entity->eventSystem && ImGui::MenuItem("Event System")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->eventSystem = EventSystemComponent{};
            RecordImmediateEdit("Add EventSystem", before, selectionBefore);
            status_ = "Added Event System.";
        }
        if (!entity->text && ImGui::MenuItem("Text")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->text = TextComponent{};
            RecordImmediateEdit("Add Text", before, selectionBefore);
            status_ = "Added Text.";
        }
        if (!entity->image && ImGui::MenuItem("Image")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->image = ImageComponent{};
            RecordImmediateEdit("Add Image", before, selectionBefore);
            status_ = "Added Image.";
        }
        if (!entity->button && ImGui::MenuItem("Button")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->button = ButtonComponent{};
            RecordImmediateEdit("Add Button", before, selectionBefore);
            status_ = "Added Button.";
        }
        if (!entity->toggle && ImGui::MenuItem("Toggle")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->toggle = ToggleComponent{};
            RecordImmediateEdit("Add Toggle", before, selectionBefore);
            status_ = "Added Toggle.";
        }
        if (!entity->slider && ImGui::MenuItem("Slider")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->slider = SliderComponent{};
            RecordImmediateEdit("Add Slider", before, selectionBefore);
            status_ = "Added Slider.";
        }
        if (!entity->dropdown && ImGui::MenuItem("Dropdown")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->dropdown = DropdownComponent{};
            RecordImmediateEdit("Add Dropdown", before, selectionBefore);
            status_ = "Added Dropdown.";
        }
        if (!entity->inputField && ImGui::MenuItem("Input Field")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->inputField = InputFieldComponent{};
            RecordImmediateEdit("Add InputField", before, selectionBefore);
            status_ = "Added Input Field.";
        }
        if (!entity->boxCollider && ImGui::MenuItem("Box Collider")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->boxCollider = BoxColliderComponent{};
            RecordImmediateEdit("Add BoxCollider", before, selectionBefore);
            status_ = "Added BoxCollider.";
        }
        if (!entity->characterController && ImGui::MenuItem("Character Controller")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->characterController = CharacterControllerComponent{};
            RecordImmediateEdit("Add CharacterController", before, selectionBefore);
            status_ = "Added CharacterController.";
        }
        if (ImGui::MenuItem("Script")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->scripts.emplace_back();
            RecordImmediateEdit("Add Script", before, selectionBefore);
            status_ = "Added an empty Script component.";
        }
        ImGui::EndPopup();
    }
}

void EditorScene::DrawScriptsInspector(WorldEntity* entity) {
    for (size_t scriptIndex = 0; scriptIndex < entity->scripts.size(); ++scriptIndex) {
        if (DrawScriptEntryInspector(entity, scriptIndex)) {
            break;
        }
    }
}

bool EditorScene::DrawScriptEntryInspector(WorldEntity* entity, size_t scriptIndex) {
    ImGui::PushID(static_cast<int>(scriptIndex));
    ImGui::SeparatorText("Script");
    if (ImGui::Button("Remove Script")) {
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        entity->scripts.erase(entity->scripts.begin() + static_cast<std::ptrdiff_t>(scriptIndex));
        RecordImmediateEdit("Remove Script", before, selectionBefore);
        status_ = "Removed Script component.";
        ImGui::PopID();
        return true;
    } else {
        ImGui::SameLine();
        ImGui::BeginDisabled(scriptIndex == 0u);
        const bool moveUp = ImGui::Button("Move Up");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(scriptIndex + 1u >= entity->scripts.size());
        const bool moveDown = ImGui::Button("Move Down");
        ImGui::EndDisabled();
        if (moveUp || moveDown) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            const size_t destination = moveUp ? scriptIndex - 1u : scriptIndex + 1u;
            std::swap(entity->scripts[scriptIndex], entity->scripts[destination]);
            RecordImmediateEdit("Reorder Scripts", before, selectionBefore);
            status_ = "Changed Script execution order.";
            ImGui::PopID();
            return true;
        }
        ImGui::TextDisabled("Execution Order: %zu", scriptIndex + 1u);
        BehaviorComponent& behavior = entity->scripts[scriptIndex];
        const EntityId selectionBefore = selection_;
        std::string before = WorldSerializer::Serialize(world_);
        if (ImGui::Checkbox("Enabled", &behavior.enabled)) {
            RecordImmediateEdit("Toggle Script", std::move(before), selectionBefore);
        }
        const std::string scriptLabel = behavior.scriptAssetPath.empty()
                                            ? "None (drop a Script asset)"
                                            : behavior.scriptAssetPath;
        ImGui::TextUnformatted("Script");
        ImGui::SameLine();
        if (ImGui::Button(scriptLabel.c_str(), {-FLT_MIN, 0.0f})) {
            ImGui::OpenPopup("ScriptAssetPicker");
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kScriptAssetDragPayload);
                payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                AssignScriptAsset(selection_, static_cast<const char*>(payload->Data), scriptIndex);
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopup("ScriptAssetPicker")) {
            if (ImGui::MenuItem("None", nullptr, behavior.scriptAssetPath.empty(),
                                !behavior.scriptAssetPath.empty())) {
                ClearScriptAsset(selection_, scriptIndex);
            }
            ImGui::Separator();
            if (scriptAssets_.empty()) {
                ImGui::TextDisabled("No Script assets found.");
            } else {
                for (const std::filesystem::path& scriptAsset : scriptAssets_) {
                    const std::string assetPath = scriptAsset.generic_string();
                    const std::string assetReference =
                        "asset://" + scriptAsset.lexically_relative("assets").generic_string();
                    const std::string label =
                        scriptAsset.filename().generic_string() + "##" + assetPath;
                    if (ImGui::MenuItem(label.c_str(), nullptr,
                                        behavior.scriptAssetPath == assetReference)) {
                        AssignScriptAsset(selection_, scriptAsset, scriptIndex);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", assetPath.c_str());
                    }
                }
            }
            ImGui::EndPopup();
        }
        ImGui::TextDisabled("Runtime type: %s",
                            behavior.type.empty() ? "None" : behavior.type.c_str());
        if (behavior.type.empty() || behavior.scriptAssetPath.empty()) {
            ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f}, "Assign a C++ Script asset.");
        } else {
            std::string requirementError;
            if (!behaviorRegistry_.ValidateRequirements(behavior.type, *entity,
                                                        &requirementError)) {
                ImGui::TextColored({1.0f, 0.45f, 0.35f, 1.0f}, "%s", requirementError.c_str());
                const BehaviorRequirements* requirements =
                    behaviorRegistry_.Requirements(behavior.type);
                if (requirements != nullptr && ImGui::Button("Add Required Components")) {
                    const std::string requirementBefore = WorldSerializer::Serialize(world_);
                    if (behaviorRegistry_.EnsureRequirements(behavior.type, *entity)) {
                        RecordImmediateEdit("Add Script Requirements", requirementBefore,
                                            selectionBefore);
                        status_ = "Added required components for Script.";
                    } else {
                        status_ = "Script requirements could not be added.";
                    }
                }
            } else {
                const std::string_view registeredSource =
                    behaviorRegistry_.SourceAsset(behavior.type);
                if (registeredSource.empty() || registeredSource != behavior.scriptAssetPath) {
                    ImGui::TextColored(
                        {1.0f, 0.45f, 0.35f, 1.0f},
                        "The Script asset does not match its registered runtime type.");
                }
            }
        }
        DrawScriptPropertiesInspector(entity, behavior, selectionBefore);
        if (behavior.type == "FirstPersonController" && !entity->characterController) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.25f, 1.0f),
                               "Character Controller is required for collision movement.");
        }
    }
    ImGui::PopID();

    return false;
}

void EditorScene::DrawScriptPropertiesInspector(WorldEntity* entity, BehaviorComponent& behavior,
                                                EntityId selectionBefore) {
    const std::vector<ScriptPropertyDefinition>* propertyDefinitions =
        behaviorRegistry_.Properties(behavior.type);
    if (propertyDefinitions != nullptr) {
        for (size_t propertyIndex = 0u; propertyIndex < propertyDefinitions->size();
             ++propertyIndex) {
            const ScriptPropertyDefinition& definition = (*propertyDefinitions)[propertyIndex];
            ImGui::PushID(static_cast<int>(propertyIndex));
            (void)(DrawScalarScriptPropertyInspector(behavior, definition, selectionBefore) ||
                   DrawAssetScriptPropertyInspector(entity, behavior, definition,
                                                    selectionBefore) ||
                   DrawStringScriptPropertyInspector(behavior, definition) ||
                   DrawEntityScriptPropertyInspector(behavior, definition, selectionBefore));
            ImGui::PopID();
        }
    }
}

bool EditorScene::DrawScalarScriptPropertyInspector(BehaviorComponent& behavior,
                                                    const ScriptPropertyDefinition& definition,
                                                    EntityId selectionBefore) {
    if (definition.type != ScriptPropertyType::Float &&
        definition.type != ScriptPropertyType::Boolean &&
        definition.type != ScriptPropertyType::Integer &&
        definition.type != ScriptPropertyType::Vector3) {
        return false;
    }
    auto stored =
        std::ranges::find(behavior.properties, definition.name, &ScriptPropertyValue::name);
    if (definition.type == ScriptPropertyType::Float) {
        float value = stored != behavior.properties.end() && stored->type == definition.type
                          ? stored->floatValue
                          : definition.defaultFloat;
        const float speed =
            (std::max)(0.001f, (definition.maximumFloat - definition.minimumFloat) * 0.005f);
        if (ImGui::DragFloat(definition.name.c_str(), &value, speed, definition.minimumFloat,
                             definition.maximumFloat, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
            if (stored == behavior.properties.end()) {
                behavior.properties.push_back({definition.name, definition.type, value, {}});
            } else if (stored->type != definition.type) {
                *stored = {definition.name, definition.type, value, {}};
            } else {
                stored->floatValue = value;
            }
            RefreshDirty();
            status_ = "Modified Script property.";
        }
        if (ImGui::IsItemActivated()) {
            BeginHistoryEdit("Modify Script Property");
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            CommitHistoryEdit();
        }
    } else if (definition.type == ScriptPropertyType::Boolean) {
        bool value = stored != behavior.properties.end() && stored->type == definition.type
                         ? stored->booleanValue
                         : definition.defaultBoolean;
        if (ImGui::Checkbox(definition.name.c_str(), &value)) {
            const std::string propertyBefore = WorldSerializer::Serialize(world_);
            if (stored == behavior.properties.end()) {
                behavior.properties.push_back({});
                stored = std::prev(behavior.properties.end());
            }
            *stored = {};
            stored->name = definition.name;
            stored->type = definition.type;
            stored->booleanValue = value;
            RecordImmediateEdit("Modify Script Property", propertyBefore, selectionBefore);
            status_ = "Modified Script property.";
        }
    } else if (definition.type == ScriptPropertyType::Integer) {
        int value = stored != behavior.properties.end() && stored->type == definition.type
                        ? stored->integerValue
                        : definition.defaultInteger;
        if (ImGui::DragInt(definition.name.c_str(), &value, 1.0f, definition.minimumInteger,
                           definition.maximumInteger, "%d", ImGuiSliderFlags_AlwaysClamp)) {
            if (stored == behavior.properties.end()) {
                behavior.properties.push_back({});
                stored = std::prev(behavior.properties.end());
            }
            if (stored->type != definition.type) {
                *stored = {};
                stored->name = definition.name;
                stored->type = definition.type;
            }
            stored->integerValue = value;
            RefreshDirty();
            status_ = "Modified Script property.";
        }
        if (ImGui::IsItemActivated()) {
            BeginHistoryEdit("Modify Script Property");
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            CommitHistoryEdit();
        }
    } else if (definition.type == ScriptPropertyType::Vector3) {
        ScriptVector3 value = stored != behavior.properties.end() && stored->type == definition.type
                                  ? stored->vector3Value
                                  : definition.defaultVector3;
        float components[3]{value.x, value.y, value.z};
        if (ImGui::DragFloat3(definition.name.c_str(), components, 0.1f, 0.0f, 0.0f, "%.3f")) {
            if (stored == behavior.properties.end()) {
                behavior.properties.push_back({});
                stored = std::prev(behavior.properties.end());
            }
            if (stored->type != definition.type) {
                *stored = {};
                stored->name = definition.name;
                stored->type = definition.type;
            }
            stored->vector3Value = {components[0], components[1], components[2]};
            RefreshDirty();
            status_ = "Modified Script property.";
        }
        if (ImGui::IsItemActivated()) {
            BeginHistoryEdit("Modify Script Property");
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            CommitHistoryEdit();
        }
    }
    return true;
}

bool EditorScene::DrawStringScriptPropertyInspector(BehaviorComponent& behavior,
                                                    const ScriptPropertyDefinition& definition) {
    if (definition.type != ScriptPropertyType::String) {
        return false;
    }
    auto stored =
        std::ranges::find(behavior.properties, definition.name, &ScriptPropertyValue::name);
    if (definition.type == ScriptPropertyType::String) {
        const std::string value =
            stored != behavior.properties.end() && stored->type == definition.type
                ? stored->stringValue
                : definition.defaultString;
        std::array<char, 1025> buffer{};
        std::memcpy(buffer.data(), value.data(), (std::min)(value.size(), buffer.size() - 1u));
        if (ImGui::InputText(definition.name.c_str(), buffer.data(), buffer.size())) {
            if (stored == behavior.properties.end()) {
                behavior.properties.push_back({});
                stored = std::prev(behavior.properties.end());
            }
            if (stored->type != definition.type) {
                *stored = {};
                stored->name = definition.name;
                stored->type = definition.type;
            }
            stored->stringValue = buffer.data();
            RefreshDirty();
            status_ = "Modified Script property.";
        }
        if (ImGui::IsItemActivated()) {
            BeginHistoryEdit("Modify Script Property");
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            CommitHistoryEdit();
        }
    }
    return true;
}

bool EditorScene::DrawEntityScriptPropertyInspector(BehaviorComponent& behavior,
                                                    const ScriptPropertyDefinition& definition,
                                                    EntityId selectionBefore) {
    if (definition.type != ScriptPropertyType::Entity) {
        return false;
    }
    auto stored =
        std::ranges::find(behavior.properties, definition.name, &ScriptPropertyValue::name);
    if (definition.type == ScriptPropertyType::Entity) {
        const EntityId referenced =
            stored != behavior.properties.end() && stored->type == definition.type
                ? stored->entityValue
                : EntityId{};
        const WorldEntity* referencedEntity = world_.Find(referenced);
        std::string label = referencedEntity != nullptr ? referencedEntity->name
                            : referenced.IsValid()      ? "Missing Entity"
                                                        : "None";
        label += "##EntityProperty";
        ImGui::TextUnformatted(definition.name.c_str());
        ImGui::SameLine();
        if (ImGui::Button(label.c_str(), {-FLT_MIN, 0.0f})) {
            ImGui::OpenPopup("EntityPropertyPicker");
        }
        const auto assignEntityProperty = [&](EntityId value) {
            auto destination =
                std::ranges::find(behavior.properties, definition.name, &ScriptPropertyValue::name);
            if ((destination == behavior.properties.end() && !value.IsValid()) ||
                (destination != behavior.properties.end() && destination->type == definition.type &&
                 destination->entityValue == value)) {
                return;
            }
            const std::string propertyBefore = WorldSerializer::Serialize(world_);
            if (destination == behavior.properties.end()) {
                behavior.properties.push_back({definition.name, definition.type, 0.0f, value});
            } else if (destination->type != definition.type) {
                *destination = {definition.name, definition.type, 0.0f, value};
            } else {
                destination->entityValue = value;
            }
            RecordImmediateEdit("Assign Script Entity Property", propertyBefore, selectionBefore);
            status_ = "Assigned Script Entity property.";
        };
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload);
                payload != nullptr && payload->IsDelivery() &&
                payload->DataSize == sizeof(EntityId)) {
                const EntityId dropped = *static_cast<const EntityId*>(payload->Data);
                if (world_.Contains(dropped)) {
                    assignEntityProperty(dropped);
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopup("EntityPropertyPicker")) {
            if (ImGui::MenuItem("None", nullptr, !referenced.IsValid())) {
                assignEntityProperty({});
            }
            ImGui::Separator();
            for (const WorldEntity& candidate : world_.Entities()) {
                const std::string candidateLabel = candidate.name + "##" + candidate.id.ToString();
                if (ImGui::MenuItem(candidateLabel.c_str(), nullptr, candidate.id == referenced)) {
                    assignEntityProperty(candidate.id);
                }
            }
            ImGui::EndPopup();
        }
    }
    return true;
}
