#include "EditorScene.h"

#include "AssetImportPlanner.h"
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
#include "imgui/ImguiManager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "input/Input.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "model/MeshRenderer.h"
#include "sound/ISoundService.h"
#include "sprite/SpriteRenderer.h"
#include "texture/TextureManager.h"
#include "world/WorldSerializer.h"
#include "world/WorldCollision.h"

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

#include "internal/EditorSceneHierarchyUtils.h"

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
    const bool multipleEntities = inspectedEntities.size() > 1u;

    bool displayedActive = entity->active;
    const bool mixedActive = std::ranges::any_of(
        inspectedEntities, [&](EntityId inspected) {
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
    const bool mixedLayers = std::ranges::any_of(
        inspectedEntities, [&](EntityId inspected) {
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
            const std::string label = std::to_string(layer) + ": " +
                                      physicsSettings_.layerNames[layer];
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
                status_ = inspectedEntities.size() > 1u
                              ? "Changed the selected Entity Layers."
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
        RecordImmediateEdit(multipleEntities ? "Reset Transforms" : "Reset Transform",
                            before, selectionBefore);
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
        RecordImmediateEdit(multipleEntities ? "Paste Transforms" : "Paste Transform",
                            before, selectionBefore);
        status_ = multipleEntities ? "Pasted Transform to selected entities."
                                   : "Pasted Transform.";
    }
    if (!transformClipboard_) {
        ImGui::EndDisabled();
    }
    auto drawTransform = [&](const char* label,
                             DirectX::XMFLOAT3 TransformComponent::*member,
                             float speed, bool scale) {
        const DirectX::XMFLOAT3 previous = entity->transform.*member;
        DirectX::XMFLOAT3 edited = previous;
        const bool changed = ImGui::DragFloat3(label, &edited.x, speed);
        if (ImGui::IsItemActivated()) {
            BeginHistoryEdit(std::string(multipleEntities ? "Modify Transforms "
                                                          : "Modify Transform ") +
                             label);
        }
        if (changed) {
            const DirectX::XMFLOAT3 delta{edited.x - previous.x, edited.y - previous.y,
                                          edited.z - previous.z};
            auto applyComponent = [scale](float current, float oldActive,
                                          float newActive, float additiveDelta) {
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
            status_ = multipleEntities ? "Modified selected Transforms."
                                       : "Modified Transform.";
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
        if (!entity->canvasGroup &&
            ImGui::MenuItem("Canvas Group")) {
            const std::string before =
                WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->canvasGroup = CanvasGroupComponent{};
            RecordImmediateEdit("Add CanvasGroup", before,
                                selectionBefore);
            status_ = "Added Canvas Group.";
        }
        if (!entity->eventSystem &&
            ImGui::MenuItem("Event System")) {
            const std::string before =
                WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->eventSystem = EventSystemComponent{};
            RecordImmediateEdit("Add EventSystem", before,
                                selectionBefore);
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
            const std::string before =
                WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->slider = SliderComponent{};
            RecordImmediateEdit("Add Slider", before,
                                selectionBefore);
            status_ = "Added Slider.";
        }
        if (!entity->dropdown && ImGui::MenuItem("Dropdown")) {
            const std::string before =
                WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->dropdown = DropdownComponent{};
            RecordImmediateEdit("Add Dropdown", before,
                                selectionBefore);
            status_ = "Added Dropdown.";
        }
        if (!entity->inputField &&
            ImGui::MenuItem("Input Field")) {
            const std::string before =
                WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->inputField = InputFieldComponent{};
            RecordImmediateEdit("Add InputField", before,
                                selectionBefore);
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

    for (size_t scriptIndex = 0; scriptIndex < entity->scripts.size(); ++scriptIndex) {
        ImGui::PushID(static_cast<int>(scriptIndex));
        ImGui::SeparatorText("Script");
        if (ImGui::Button("Remove Script")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->scripts.erase(entity->scripts.begin() +
                                  static_cast<std::ptrdiff_t>(scriptIndex));
            RecordImmediateEdit("Remove Script", before, selectionBefore);
            status_ = "Removed Script component.";
            ImGui::PopID();
            break;
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
                break;
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
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kScriptAssetDragPayload);
                    payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                    static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                    AssignScriptAsset(selection_, static_cast<const char*>(payload->Data),
                                      scriptIndex);
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
                            "asset://" +
                            scriptAsset.lexically_relative("assets").generic_string();
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
                ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                                   "Assign a C++ Script asset.");
            } else {
                std::string requirementError;
                if (!behaviorRegistry_.ValidateRequirements(
                        behavior.type, *entity, &requirementError)) {
                    ImGui::TextColored({1.0f, 0.45f, 0.35f, 1.0f}, "%s",
                                       requirementError.c_str());
                    const BehaviorRequirements* requirements =
                        behaviorRegistry_.Requirements(behavior.type);
                    if (requirements != nullptr &&
                        ImGui::Button("Add Required Components")) {
                        const std::string requirementBefore =
                            WorldSerializer::Serialize(world_);
                        if (behaviorRegistry_.EnsureRequirements(
                                behavior.type, *entity)) {
                            RecordImmediateEdit("Add Script Requirements",
                                                requirementBefore,
                                                selectionBefore);
                            status_ = "Added required components for Script.";
                        } else {
                            status_ = "Script requirements could not be added.";
                        }
                    }
                } else {
                    const std::string_view registeredSource =
                        behaviorRegistry_.SourceAsset(behavior.type);
                    if (registeredSource.empty() ||
                        registeredSource != behavior.scriptAssetPath) {
                        ImGui::TextColored(
                            {1.0f, 0.45f, 0.35f, 1.0f},
                            "The Script asset does not match its registered runtime type.");
                    }
                }
            }
            const std::vector<ScriptPropertyDefinition>* propertyDefinitions =
                behaviorRegistry_.Properties(behavior.type);
            if (propertyDefinitions != nullptr) {
                for (size_t propertyIndex = 0u;
                     propertyIndex < propertyDefinitions->size(); ++propertyIndex) {
                    const ScriptPropertyDefinition& definition =
                        (*propertyDefinitions)[propertyIndex];
                    ImGui::PushID(static_cast<int>(propertyIndex));
                    auto stored = std::ranges::find(behavior.properties, definition.name,
                                                    &ScriptPropertyValue::name);
                    if (definition.type == ScriptPropertyType::Float) {
                        float value = stored != behavior.properties.end() &&
                                              stored->type == definition.type
                                          ? stored->floatValue
                                          : definition.defaultFloat;
                        const float speed = (std::max)(
                            0.001f,
                            (definition.maximumFloat - definition.minimumFloat) * 0.005f);
                        if (ImGui::DragFloat(definition.name.c_str(), &value, speed,
                                             definition.minimumFloat,
                                             definition.maximumFloat, "%.3f",
                                             ImGuiSliderFlags_AlwaysClamp)) {
                            if (stored == behavior.properties.end()) {
                                behavior.properties.push_back(
                                    {definition.name, definition.type, value, {}});
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
                        bool value = stored != behavior.properties.end() &&
                                             stored->type == definition.type
                                         ? stored->booleanValue
                                         : definition.defaultBoolean;
                        if (ImGui::Checkbox(definition.name.c_str(), &value)) {
                            const std::string propertyBefore =
                                WorldSerializer::Serialize(world_);
                            if (stored == behavior.properties.end()) {
                                behavior.properties.push_back({});
                                stored = std::prev(behavior.properties.end());
                            }
                            *stored = {};
                            stored->name = definition.name;
                            stored->type = definition.type;
                            stored->booleanValue = value;
                            RecordImmediateEdit("Modify Script Property", propertyBefore,
                                                selectionBefore);
                            status_ = "Modified Script property.";
                        }
                    } else if (definition.type == ScriptPropertyType::Integer) {
                        int value = stored != behavior.properties.end() &&
                                            stored->type == definition.type
                                        ? stored->integerValue
                                        : definition.defaultInteger;
                        if (ImGui::DragInt(definition.name.c_str(), &value, 1.0f,
                                           definition.minimumInteger,
                                           definition.maximumInteger, "%d",
                                           ImGuiSliderFlags_AlwaysClamp)) {
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
                        ScriptVector3 value = stored != behavior.properties.end() &&
                                                      stored->type == definition.type
                                                  ? stored->vector3Value
                                                  : definition.defaultVector3;
                        float components[3]{value.x, value.y, value.z};
                        if (ImGui::DragFloat3(definition.name.c_str(), components, 0.1f,
                                              0.0f, 0.0f, "%.3f")) {
                            if (stored == behavior.properties.end()) {
                                behavior.properties.push_back({});
                                stored = std::prev(behavior.properties.end());
                            }
                            if (stored->type != definition.type) {
                                *stored = {};
                                stored->name = definition.name;
                                stored->type = definition.type;
                            }
                            stored->vector3Value =
                                {components[0], components[1], components[2]};
                            RefreshDirty();
                            status_ = "Modified Script property.";
                        }
                        if (ImGui::IsItemActivated()) {
                            BeginHistoryEdit("Modify Script Property");
                        }
                        if (ImGui::IsItemDeactivatedAfterEdit()) {
                            CommitHistoryEdit();
                        }
                    } else if (definition.type == ScriptPropertyType::AnimationClip) {
                        const std::string value =
                            stored != behavior.properties.end() &&
                                    stored->type == definition.type
                                ? stored->stringValue
                                : definition.defaultString;
                        const ModelHandle modelHandle = entity->meshRenderer
                                                            ? ResolveModel(*entity->meshRenderer)
                                                            : ModelHandle{};
                        const Model* animationModel =
                            modelHandle.IsValid() && ctx_ != nullptr && ctx_->rendering.model
                                ? ctx_->rendering.model->GetModel(modelHandle)
                                : nullptr;
                        const std::string preview = value.empty() ? "None" : value;
                        if (ImGui::BeginCombo(definition.name.c_str(), preview.c_str())) {
                            const auto assignClip = [&](const std::string& clip) {
                                const std::string propertyBefore =
                                    WorldSerializer::Serialize(world_);
                                if (stored == behavior.properties.end()) {
                                    behavior.properties.push_back({});
                                    stored = std::prev(behavior.properties.end());
                                }
                                *stored = {};
                                stored->name = definition.name;
                                stored->type = definition.type;
                                stored->stringValue = clip;
                                RecordImmediateEdit("Modify Script Property", propertyBefore,
                                                    selectionBefore);
                                status_ = "Modified Script Animation Clip property.";
                            };
                            if (ImGui::Selectable("None", value.empty())) {
                                assignClip({});
                            }
                            if (animationModel != nullptr) {
                                std::vector<std::string> clips;
                                clips.reserve(animationModel->animations.size());
                                for (const auto& [name, clip] : animationModel->animations) {
                                    (void)clip;
                                    clips.push_back(name);
                                }
                                std::ranges::sort(clips);
                                for (const std::string& clip : clips) {
                                    if (ImGui::Selectable(clip.c_str(), value == clip)) {
                                        assignClip(clip);
                                    }
                                }
                            }
                            ImGui::EndCombo();
                        }
                        if (animationModel == nullptr || animationModel->animations.empty()) {
                            ImGui::TextDisabled(
                                "Assign an animated Model to choose an Animation Clip.");
                        } else if (!value.empty() &&
                                   !animationModel->animations.contains(value)) {
                            ImGui::TextDisabled("The selected Animation Clip was not found.");
                        }
                    } else if (definition.type == ScriptPropertyType::InputAction) {
                        const std::string value =
                            stored != behavior.properties.end() &&
                                    stored->type == definition.type
                                ? stored->stringValue
                                : definition.defaultString;
                        Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
                        const std::string resolvedName =
                            input != nullptr ? input->GetActionName(value)
                                             : std::string{};
                        const std::string preview =
                            value.empty() ? "None"
                                          : resolvedName.empty() ? value
                                                                 : resolvedName;
                        const auto acceptsAction =
                            [&definition](const InputActionBinding& binding) {
                                switch (definition.inputActionKind) {
                                case ScriptInputActionKind::Any:
                                    return true;
                                case ScriptInputActionKind::Button:
                                    return binding.type == InputActionType::Button;
                                case ScriptInputActionKind::Axis:
                                    return binding.type == InputActionType::Axis;
                                }
                                return false;
                            };
                        ImGui::BeginDisabled(input == nullptr);
                        if (ImGui::BeginCombo(definition.name.c_str(), preview.c_str())) {
                            const auto assignAction = [&](const std::string& action) {
                                const std::string propertyBefore =
                                    WorldSerializer::Serialize(world_);
                                if (stored == behavior.properties.end()) {
                                    behavior.properties.push_back({});
                                    stored = std::prev(behavior.properties.end());
                                }
                                *stored = {};
                                stored->name = definition.name;
                                stored->type = definition.type;
                                stored->stringValue = action;
                                RecordImmediateEdit("Modify Script Input Action Property",
                                                    propertyBefore, selectionBefore);
                                status_ = "Modified Script Input Action property.";
                            };
                            if (ImGui::Selectable("None", value.empty())) {
                                assignAction({});
                            }
                            for (const std::string& action : input->GetActionNames()) {
                                const InputActionBinding* binding =
                                    input->GetActionBinding(action);
                                if (binding == nullptr || !acceptsAction(*binding)) {
                                    continue;
                                }
                                if (ImGui::Selectable(action.c_str(),
                                                      resolvedName == action)) {
                                    assignAction(input->GetActionId(action));
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::EndDisabled();
                        if (input == nullptr) {
                            ImGui::TextDisabled("Input service is unavailable.");
                        } else if (!value.empty()) {
                            const InputActionBinding* selected =
                                input->GetActionBinding(value);
                            if (selected == nullptr) {
                                ImGui::TextDisabled(
                                    "The selected Input Action was not found.");
                            } else if (!acceptsAction(*selected)) {
                                ImGui::TextDisabled(
                                    "The selected Input Action has the wrong type.");
                            }
                        }
                    } else if (definition.type == ScriptPropertyType::Scene) {
                        const std::string value =
                            stored != behavior.properties.end() &&
                                    stored->type == definition.type
                                ? stored->stringValue
                                : definition.defaultString;
                        const std::string preview = value.empty() ? "None" : value;
                        if (ImGui::BeginCombo(definition.name.c_str(), preview.c_str())) {
                            const auto assignScene = [&](const std::string& scene) {
                                const std::string propertyBefore =
                                    WorldSerializer::Serialize(world_);
                                if (stored == behavior.properties.end()) {
                                    behavior.properties.push_back({});
                                    stored = std::prev(behavior.properties.end());
                                }
                                *stored = {};
                                stored->name = definition.name;
                                stored->type = definition.type;
                                stored->stringValue = scene;
                                RecordImmediateEdit("Modify Script Scene Property",
                                                    propertyBefore, selectionBefore);
                                status_ = "Modified Script Scene property.";
                            };
                            if (ImGui::Selectable("None", value.empty())) {
                                assignScene({});
                            }
                            for (const std::filesystem::path& scene : sceneAssets_) {
                                const std::string reference = scene.generic_string();
                                if (ImGui::Selectable(reference.c_str(),
                                                      value == reference)) {
                                    assignScene(reference);
                                }
                            }
                            ImGui::EndCombo();
                        }
                        if (!value.empty() &&
                            std::ranges::none_of(
                                sceneAssets_, [&value](const std::filesystem::path& scene) {
                                    return scene.generic_string() == value;
                                })) {
                            ImGui::TextDisabled("The selected Scene was not found.");
                        }
                    } else if (definition.type == ScriptPropertyType::String) {
                        const std::string value =
                            stored != behavior.properties.end() &&
                                    stored->type == definition.type
                                ? stored->stringValue
                                : definition.defaultString;
                        std::array<char, 1025> buffer{};
                        std::memcpy(buffer.data(), value.data(),
                                    (std::min)(value.size(), buffer.size() - 1u));
                        if (ImGui::InputText(definition.name.c_str(), buffer.data(),
                                             buffer.size())) {
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
                    } else if (definition.type == ScriptPropertyType::Entity) {
                        const EntityId referenced =
                            stored != behavior.properties.end() &&
                                    stored->type == definition.type
                                ? stored->entityValue
                                : EntityId{};
                        const WorldEntity* referencedEntity = world_.Find(referenced);
                        std::string label = referencedEntity != nullptr
                                                ? referencedEntity->name
                                                : referenced.IsValid() ? "Missing Entity"
                                                                       : "None";
                        label += "##EntityProperty";
                        ImGui::TextUnformatted(definition.name.c_str());
                        ImGui::SameLine();
                        if (ImGui::Button(label.c_str(), {-FLT_MIN, 0.0f})) {
                            ImGui::OpenPopup("EntityPropertyPicker");
                        }
                        const auto assignEntityProperty = [&](EntityId value) {
                            auto destination = std::ranges::find(
                                behavior.properties, definition.name,
                                &ScriptPropertyValue::name);
                            if ((destination == behavior.properties.end() &&
                                 !value.IsValid()) ||
                                (destination != behavior.properties.end() &&
                                 destination->type == definition.type &&
                                 destination->entityValue == value)) {
                                return;
                            }
                            const std::string propertyBefore =
                                WorldSerializer::Serialize(world_);
                            if (destination == behavior.properties.end()) {
                                behavior.properties.push_back(
                                    {definition.name, definition.type, 0.0f, value});
                            } else if (destination->type != definition.type) {
                                *destination =
                                    {definition.name, definition.type, 0.0f, value};
                            } else {
                                destination->entityValue = value;
                            }
                            RecordImmediateEdit("Assign Script Entity Property",
                                                propertyBefore, selectionBefore);
                            status_ = "Assigned Script Entity property.";
                        };
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(kEntityDragPayload);
                                payload != nullptr && payload->IsDelivery() &&
                                payload->DataSize == sizeof(EntityId)) {
                                const EntityId dropped =
                                    *static_cast<const EntityId*>(payload->Data);
                                if (world_.Contains(dropped)) {
                                    assignEntityProperty(dropped);
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                        if (ImGui::BeginPopup("EntityPropertyPicker")) {
                            if (ImGui::MenuItem("None", nullptr,
                                                !referenced.IsValid())) {
                                assignEntityProperty({});
                            }
                            ImGui::Separator();
                            for (const WorldEntity& candidate : world_.Entities()) {
                                const std::string candidateLabel =
                                    candidate.name + "##" + candidate.id.ToString();
                                if (ImGui::MenuItem(candidateLabel.c_str(), nullptr,
                                                    candidate.id == referenced)) {
                                    assignEntityProperty(candidate.id);
                                }
                            }
                            ImGui::EndPopup();
                        }
                    }
                    ImGui::PopID();
                }
            }
            if (behavior.type == "FirstPersonController" && !entity->characterController) {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.25f, 1.0f),
                                   "Character Controller is required for collision movement.");
            }
        }
        ImGui::PopID();
    }
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
                const bool selected = boxColliderGizmoEntity_ == selection_ &&
                                      boxColliderGizmoMode_ == mode;
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
                RecordImmediateEdit("Toggle BoxCollider", std::move(before),
                                    selectionBefore);
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
            if (ImGui::DragFloat3("Size##BoxCollider", &collider.size.x, 0.02f, 0.001f,
                                  1000000.0f, "%.3f",
                                  ImGuiSliderFlags_AlwaysClamp)) {
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

    if (entity->characterController) {
        ImGui::SeparatorText("Character Controller");
        const bool requiredByBehavior = std::ranges::any_of(
            entity->scripts, [this](const BehaviorComponent& script) {
                const BehaviorRequirements* requirements =
                    behaviorRegistry_.Requirements(script.type);
                return requirements != nullptr && requirements->characterController;
            });
        ImGui::BeginDisabled(requiredByBehavior);
        const bool removeRequested = ImGui::Button("Remove Character Controller");
        ImGui::EndDisabled();
        if (requiredByBehavior &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
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
            auto controllerGizmoButton = [&](const char* label,
                                             CharacterControllerGizmoMode mode) {
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
            if (ImGui::DragFloat3("Center##CharacterController", &controller.center.x,
                                  0.02f)) {
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
            drawControllerFloat("Slope Limit##CharacterController",
                                controller.slopeLimitDegrees, 0.0f, 90.0f);
            drawControllerFloat("Step Offset##CharacterController", controller.stepOffset,
                                0.0f, controller.height);
            drawControllerFloat("Skin Width##CharacterController", controller.skinWidth,
                                0.0f, (std::max)(0.0f, controller.radius - 0.001f));
            drawControllerFloat("Min Move Distance##CharacterController",
                                controller.minMoveDistance, 0.0f, 1000000.0f);
        }
    }

    if (entity->camera) {
        ImGui::SeparatorText("Camera");
        if (ImGui::Button("Remove Camera")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->camera.reset();
            RecordImmediateEdit("Remove Camera", before, selectionBefore);
            status_ = "Removed Camera.";
        } else {
            CameraComponent& camera = *entity->camera;
            if (ImGui::Button("Align to Scene View")) {
                AlignSelectedCameraToSceneView();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Move and rotate this Camera to match the Scene View.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Move View to Camera")) {
                AlignSceneViewToSelectedCamera();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Move the Scene View to this Camera's position and rotation.");
            }
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Camera", &camera.enabled)) {
                RecordImmediateEdit("Toggle Camera", std::move(before), selectionBefore);
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Primary##Camera", &camera.primary)) {
                if (camera.primary) {
                    world_.SetPrimaryCamera(entity->id);
                }
                RecordImmediateEdit("Change Primary Camera", std::move(before), selectionBefore);
            }
            int projection = static_cast<int>(camera.projection);
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("Projection##Camera", &projection,
                             "Perspective\0Orthographic\0")) {
                camera.projection = static_cast<CameraProjection>(projection);
                RecordImmediateEdit("Change Camera Projection", std::move(before),
                                    selectionBefore);
            }
            auto drawCameraFloat = [&](const char* label, float& value, float speed,
                                       float minimum, float maximum, const char* format) {
                if (ImGui::DragFloat(label, &value, speed, minimum, maximum, format,
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Camera.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Camera");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            if (camera.projection == CameraProjection::Perspective) {
                drawCameraFloat("Field of View", camera.fieldOfViewDegrees, 0.25f, 1.0f,
                                179.0f, "%.1f deg");
            } else {
                drawCameraFloat("Orthographic Height", camera.orthographicHeight, 0.05f,
                                0.001f, 1000000.0f, "%.3f");
            }
            drawCameraFloat("Near Clip", camera.nearClip, 0.005f, 0.001f,
                            (std::max)(0.001f, camera.farClip - 0.001f), "%.3f");
            drawCameraFloat("Far Clip", camera.farClip, 0.5f,
                            camera.nearClip + 0.001f, 1000000000.0f, "%.1f");
        }
    }

    if (entity->light) {
        ImGui::SeparatorText("Light");
        if (ImGui::Button("Remove Light")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->light.reset();
            RecordImmediateEdit("Remove Light", before, selectionBefore);
            status_ = "Removed Light.";
        } else {
            LightComponent& light = *entity->light;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Light", &light.enabled)) {
                RecordImmediateEdit("Toggle Light", std::move(before), selectionBefore);
            }
            int type = static_cast<int>(light.type);
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("Type##Light", &type, "Directional\0Point\0Spot\0")) {
                light.type = static_cast<LightType>(type);
                RecordImmediateEdit("Change Light Type", std::move(before), selectionBefore);
            }
            if (ImGui::ColorEdit3("Color##Light", &light.color.x,
                                  ImGuiColorEditFlags_Float)) {
                RefreshDirty();
                status_ = "Modified Light.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Light Color");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            auto drawLightFloat = [&](const char* label, float& value, float speed,
                                      float minimum, float maximum, const char* format) {
                if (ImGui::DragFloat(label, &value, speed, minimum, maximum, format,
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Light.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Light");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            drawLightFloat("Intensity##Light", light.intensity, 0.02f, 0.0f, 1000000.0f,
                           "%.2f");
            if (light.type != LightType::Directional) {
                drawLightFloat("Range##Light", light.range, 0.05f, 0.001f, 1000000.0f,
                               "%.2f");
            }
            if (light.type == LightType::Spot) {
                drawLightFloat("Inner Angle##Light", light.innerAngleDegrees, 0.25f, 0.0f,
                               (std::max)(0.0f, light.outerAngleDegrees - 0.1f), "%.1f deg");
                drawLightFloat("Outer Angle##Light", light.outerAngleDegrees, 0.25f,
                               light.innerAngleDegrees + 0.1f, 179.0f, "%.1f deg");
            }
        }
    }

    if (entity->audioSource) {
        ImGui::SeparatorText("Audio Source");
        if (ImGui::Button("Remove Audio Source")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->audioSource.reset();
            RecordImmediateEdit("Remove AudioSource", before, selectionBefore);
            status_ = "Removed AudioSource.";
        } else {
            AudioSourceComponent& source = *entity->audioSource;
            if (IsInPlayMode()) {
                ImGui::TextDisabled("Runtime: %s",
                                    source.runtimePlaying ? "Playing" : "Stopped");
            } else {
                ImGui::TextDisabled(
                    "Script API: PlayAudioSource / PlayAudioSourceOneShot / StopAudioSource");
            }
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##AudioSource", &source.enabled)) {
                RecordImmediateEdit("Toggle AudioSource", std::move(before), selectionBefore);
            }

            const std::string clipLabel = source.clipPath.empty()
                                              ? "None (drop an Audio asset)"
                                              : source.clipPath;
            ImGui::TextUnformatted("Clip");
            ImGui::SameLine();
            if (ImGui::Button(clipLabel.c_str(), {-FLT_MIN, 0.0f})) {
                ImGui::OpenPopup("AudioAssetPicker");
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kAudioAssetDragPayload);
                    payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                    static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                    AssignAudioAsset(selection_, static_cast<const char*>(payload->Data));
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopup("AudioAssetPicker")) {
                if (ImGui::Selectable("None", source.clipPath.empty())) {
                    before = WorldSerializer::Serialize(world_);
                    source.clipPath.clear();
                    RecordImmediateEdit("Clear Audio Clip", std::move(before),
                                        selectionBefore);
                }
                for (const std::filesystem::path& audioAsset : audioAssets_) {
                    const std::string label = audioAsset.generic_string();
                    if (ImGui::Selectable(label.c_str(), source.clipPath == label)) {
                        AssignAudioAsset(selection_, audioAsset);
                    }
                }
                ImGui::EndPopup();
            }

            auto drawAudioCheckbox = [&](const char* label, bool& value,
                                         const char* historyLabel) {
                before = WorldSerializer::Serialize(world_);
                if (ImGui::Checkbox(label, &value)) {
                    RecordImmediateEdit(historyLabel, std::move(before), selectionBefore);
                }
            };
            drawAudioCheckbox("Play On Awake##AudioSource", source.playOnAwake,
                              "Toggle AudioSource Play On Awake");
            drawAudioCheckbox("Loop##AudioSource", source.loop, "Toggle AudioSource Loop");
            drawAudioCheckbox("Spatial##AudioSource", source.spatial,
                              "Toggle AudioSource Spatial");

            auto drawAudioFloat = [&](const char* label, float& value, float speed,
                                      float minimum, float maximum) {
                if (ImGui::DragFloat(label, &value, speed, minimum, maximum, "%.2f",
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified AudioSource.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify AudioSource");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            drawAudioFloat("Volume##AudioSource", source.volume, 0.01f, 0.0f, 1.0f);
            drawAudioFloat("Pitch##AudioSource", source.pitch, 0.01f,
                           AudioSourceComponent::kMinPitch, AudioSourceComponent::kMaxPitch);
            if (source.spatial) {
                drawAudioFloat("Min Distance##AudioSource", source.minDistance, 0.05f,
                               0.0f, (std::max)(0.0f, source.maxDistance - 0.01f));
                drawAudioFloat("Max Distance##AudioSource", source.maxDistance, 0.1f,
                               source.minDistance + 0.01f, 1000000.0f);
            }
        }
    }

    if (entity->audioListener) {
        ImGui::SeparatorText("Audio Listener");
        if (ImGui::Button("Remove Audio Listener")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->audioListener.reset();
            RecordImmediateEdit("Remove AudioListener", before, selectionBefore);
            status_ = "Removed AudioListener.";
        } else {
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##AudioListener", &entity->audioListener->enabled)) {
                RecordImmediateEdit("Toggle AudioListener", std::move(before), selectionBefore);
            }
            ImGui::TextDisabled("Receives 3D audio at this Entity's Transform.");
            if (!entity->camera) {
                ImGui::TextDisabled("A Camera component is not required.");
            }
        }
    }

    if (entity->animator) {
        ImGui::SeparatorText("Animator");
        if (ImGui::Button("Remove Animator")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            if (editAnimatorPreviewEntity_ == entity->id) {
                EndEditAnimatorPreview();
            }
            entity->animator.reset();
            RecordImmediateEdit("Remove Animator", before, selectionBefore);
            status_ = "Removed Animator.";
        } else {
            AnimatorComponent& animator = *entity->animator;
            if (IsInPlayMode()) {
                ImGui::TextDisabled("Runtime: %s%s",
                                    animator.runtimePlaying ? "Playing" : "Stopped",
                                    animator.runtimeFinished ? " (Finished)" : "");
                if (!animator.runtimeClip.empty()) {
                    ImGui::TextDisabled("Runtime Clip: %s", animator.runtimeClip.c_str());
                }
                char animationProgress[64]{};
                std::snprintf(animationProgress, std::size(animationProgress), "%.2f / %.2f s",
                              animator.runtimeTime, animator.runtimeDuration);
                ImGui::ProgressBar(animator.runtimeNormalizedTime, {-FLT_MIN, 0.0f},
                                   animationProgress);
                if (animator.runtimeTransitioning) {
                    ImGui::ProgressBar(animator.runtimeTransitionProgress, {-FLT_MIN, 0.0f},
                                       "Cross Fade");
                }
            } else {
                ImGui::TextDisabled(
                    "Script API: PlayAnimation / CrossFadeAnimation / StopAnimation");
            }
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Animator", &animator.enabled)) {
                RecordImmediateEdit("Toggle Animator", std::move(before), selectionBefore);
            }

            const ModelHandle handle = entity->meshRenderer
                                           ? ResolveModel(*entity->meshRenderer)
                                           : ModelHandle{};
            const Model* model = handle.IsValid() && ctx_ != nullptr && ctx_->rendering.model
                                     ? ctx_->rendering.model->GetModel(handle)
                                     : nullptr;
            const char* clipLabel = animator.clip.empty() ? "First Clip" : animator.clip.c_str();
            if (ImGui::BeginCombo("Clip##Animator", clipLabel)) {
                if (ImGui::Selectable("First Clip", animator.clip.empty())) {
                    before = WorldSerializer::Serialize(world_);
                    animator.clip.clear();
                    RecordImmediateEdit("Change Animator Clip", std::move(before),
                                        selectionBefore);
                    if (editAnimatorPreviewEntity_ == entity->id) {
                        BeginEditAnimatorPreview(entity->id);
                    }
                }
                if (model != nullptr) {
                    std::vector<std::string> clips;
                    clips.reserve(model->animations.size());
                    for (const auto& [name, clip] : model->animations) {
                        (void)clip;
                        clips.push_back(name);
                    }
                    std::ranges::sort(clips);
                    for (const std::string& clip : clips) {
                        if (ImGui::Selectable(clip.c_str(), animator.clip == clip)) {
                            before = WorldSerializer::Serialize(world_);
                            animator.clip = clip;
                            RecordImmediateEdit("Change Animator Clip", std::move(before),
                                                selectionBefore);
                            if (editAnimatorPreviewEntity_ == entity->id) {
                                BeginEditAnimatorPreview(entity->id);
                            }
                        }
                    }
                }
                ImGui::EndCombo();
            }
            if (model == nullptr || model->animations.empty()) {
                ImGui::TextDisabled("Assign an animated Model to Mesh Renderer.");
            }
            if (!IsInPlayMode() && model != nullptr && !model->animations.empty()) {
                const bool previewing = editAnimatorPreviewEntity_ == entity->id &&
                                        editAnimatorPreviewModel_.IsValid();
                Model* previewModel =
                    previewing && ctx_ != nullptr && ctx_->rendering.model != nullptr
                        ? ctx_->rendering.model->GetModel(editAnimatorPreviewModel_)
                        : nullptr;
                const bool previewPlaying = previewModel != nullptr && previewModel->isPlaying;
                if (ImGui::Button(previewPlaying ? "Pause##AnimatorPreview"
                                                 : "Play##AnimatorPreview")) {
                    if (previewModel != nullptr && !previewPlaying &&
                        previewModel->animationFinished) {
                        status_ = BeginEditAnimatorPreview(entity->id)
                                      ? "Restarted Animator preview."
                                      : "Animator preview could not be started.";
                    } else if (previewModel != nullptr && !previewPlaying) {
                        previewModel->isPlaying = true;
                        previewModel->animationFinished = false;
                        status_ = "Resumed Animator preview.";
                    } else if (previewPlaying) {
                        previewModel->isPlaying = false;
                        status_ = "Paused Animator preview.";
                    } else if (BeginEditAnimatorPreview(entity->id)) {
                        status_ = "Started Animator preview.";
                    } else {
                        status_ = "Animator preview could not be started.";
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Restart##AnimatorPreview")) {
                    status_ = BeginEditAnimatorPreview(entity->id)
                                  ? "Restarted Animator preview."
                                  : "Animator preview could not be started.";
                }
                if (previewModel != nullptr) {
                    ImGui::SameLine();
                    if (ImGui::Button("Stop##AnimatorPreview")) {
                        EndEditAnimatorPreview();
                        status_ = "Stopped Animator preview.";
                    }
                    const auto clip = previewModel->animations.find(
                        previewModel->currentAnimation);
                    const float duration =
                        clip != previewModel->animations.end()
                            ? (std::max)(0.0f, clip->second.duration)
                            : 0.0f;
                    float time = std::clamp(previewModel->animationTime, 0.0f, duration);
                    if (ImGui::SliderFloat("Time##AnimatorPreview", &time, 0.0f, duration,
                                           "%.2f s")) {
                        previewModel->animationTime = time;
                        previewModel->isPlaying = false;
                        previewModel->animationFinished =
                            duration > 0.0f && time >= duration;
                        ctx_->rendering.model->UpdateAnimation(editAnimatorPreviewModel_, 0.0f);
                        status_ = "Scrubbed Animator preview.";
                    }
                }
            }
            auto drawAnimatorCheckbox = [&](const char* label, bool& value,
                                            const char* historyLabel) {
                before = WorldSerializer::Serialize(world_);
                if (ImGui::Checkbox(label, &value)) {
                    RecordImmediateEdit(historyLabel, std::move(before), selectionBefore);
                }
            };
            drawAnimatorCheckbox("Play On Awake##Animator", animator.playOnAwake,
                                 "Toggle Animator Play On Awake");
            drawAnimatorCheckbox("Loop##Animator", animator.loop, "Toggle Animator Loop");
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Lock Root Position##Animator",
                                &animator.lockRootPosition)) {
                RecordImmediateEdit("Toggle Animator Root Position Lock",
                                    std::move(before), selectionBefore);
                status_ = animator.lockRootPosition
                              ? "Locked Animator root position."
                              : "Unlocked Animator root position.";
                if (editAnimatorPreviewEntity_ == entity->id &&
                    editAnimatorPreviewModel_.IsValid() && ctx_ != nullptr &&
                    ctx_->rendering.model != nullptr) {
                    if (Model* previewModel =
                            ctx_->rendering.model->GetModel(editAnimatorPreviewModel_);
                        previewModel != nullptr) {
                        previewModel->lockRootAnimationPosition =
                            animator.lockRootPosition;
                        ctx_->rendering.model->UpdateAnimation(
                            editAnimatorPreviewModel_, 0.0f);
                    }
                }
            }
            ImGui::TextDisabled(
                "Keeps animation root translation aligned with the Entity.");
            if (editAnimatorPreviewEntity_ == entity->id &&
                editAnimatorPreviewModel_.IsValid() && ctx_ != nullptr &&
                ctx_->rendering.model != nullptr) {
                if (Model* previewModel =
                        ctx_->rendering.model->GetModel(editAnimatorPreviewModel_);
                    previewModel != nullptr) {
                    previewModel->isLoop = animator.loop;
                }
            }
            if (ImGui::DragFloat("Speed##Animator", &animator.speed, 0.01f, 0.0f, 100.0f,
                                 "%.2fx", ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Animator.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Animator Speed");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
        }
    }

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
                RecordImmediateEdit("Toggle Canvas", std::move(before),
                                    selectionBefore);
            }
            const char* scaleMode =
                canvas.scaleMode == CanvasScaleMode::ConstantPixelSize
                    ? "Constant Pixel Size"
                    : "Scale With Screen Size";
            if (ImGui::BeginCombo("UI Scale Mode##Canvas", scaleMode)) {
                const auto selectScaleMode =
                    [&](CanvasScaleMode value, const char* label) {
                        if (ImGui::Selectable(label,
                                              canvas.scaleMode == value)) {
                            const std::string modeBefore =
                                WorldSerializer::Serialize(world_);
                            canvas.scaleMode = value;
                            RecordImmediateEdit(
                                "Change Canvas Scale Mode",
                                std::move(modeBefore), selectionBefore);
                            status_ = "Changed Canvas scale mode.";
                        }
                    };
                selectScaleMode(CanvasScaleMode::ConstantPixelSize,
                                "Constant Pixel Size");
                selectScaleMode(CanvasScaleMode::ScaleWithScreenSize,
                                "Scale With Screen Size");
                ImGui::EndCombo();
            }
            if (canvas.scaleMode ==
                CanvasScaleMode::ScaleWithScreenSize) {
                float resolution[2]{
                    canvas.referenceResolution.x,
                    canvas.referenceResolution.y,
                };
                if (ImGui::DragFloat2(
                        "Reference Resolution##Canvas", resolution, 1.0f,
                        1.0f, 16384.0f, "%.0f",
                        ImGuiSliderFlags_AlwaysClamp)) {
                    canvas.referenceResolution = {resolution[0],
                                                  resolution[1]};
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
                    canvas.screenMatchMode ==
                            CanvasScreenMatchMode::MatchWidthOrHeight
                        ? "Match Width Or Height"
                        : canvas.screenMatchMode ==
                                  CanvasScreenMatchMode::Shrink
                              ? "Shrink"
                              : "Expand";
                if (ImGui::BeginCombo("Screen Match Mode##Canvas",
                                      screenMatchMode)) {
                    const auto selectScreenMatchMode =
                        [&](CanvasScreenMatchMode value,
                            const char* label) {
                            if (ImGui::Selectable(
                                    label,
                                    canvas.screenMatchMode == value)) {
                                const std::string modeBefore =
                                    WorldSerializer::Serialize(world_);
                                canvas.screenMatchMode = value;
                                RecordImmediateEdit(
                                    "Change Canvas Screen Match Mode",
                                    std::move(modeBefore),
                                    selectionBefore);
                                status_ =
                                    "Changed Canvas screen match mode.";
                            }
                        };
                    selectScreenMatchMode(
                        CanvasScreenMatchMode::MatchWidthOrHeight,
                        "Match Width Or Height");
                    selectScreenMatchMode(
                        CanvasScreenMatchMode::Expand, "Expand");
                    selectScreenMatchMode(
                        CanvasScreenMatchMode::Shrink, "Shrink");
                    ImGui::EndCombo();
                }
                if (canvas.screenMatchMode ==
                    CanvasScreenMatchMode::MatchWidthOrHeight) {
                    if (ImGui::SliderFloat(
                            "Match##Canvas",
                            &canvas.matchWidthOrHeight, 0.0f, 1.0f,
                            "%.2f")) {
                        RefreshDirty();
                        status_ =
                            "Modified Canvas screen match value.";
                    }
                    if (ImGui::IsItemActivated()) {
                        BeginHistoryEdit(
                            "Modify Canvas Screen Match");
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        CommitHistoryEdit();
                    }
                    ImGui::TextDisabled("0 = Width, 1 = Height");
                }
            }
            if (ImGui::DragInt("Sorting Order##Canvas",
                               &canvas.sortingOrder, 1.0f, -1000000, 1000000,
                               "%d", ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Canvas sorting order.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Canvas Sorting Order");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            ImGui::TextDisabled(
                "Higher sorting orders are drawn and selected on top.");
        }
    }

    if (entity->canvasGroup) {
        ImGui::SeparatorText("Canvas Group");
        if (ImGui::Button("Remove Canvas Group")) {
            const std::string before =
                WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->canvasGroup.reset();
            RecordImmediateEdit("Remove CanvasGroup", before,
                                selectionBefore);
            status_ = "Removed Canvas Group.";
        } else {
            CanvasGroupComponent& group = *entity->canvasGroup;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##CanvasGroup",
                                &group.enabled)) {
                RecordImmediateEdit("Toggle CanvasGroup",
                                    std::move(before),
                                    selectionBefore);
                status_ = "Toggled Canvas Group.";
            }
            if (ImGui::SliderFloat("Alpha##CanvasGroup",
                                   &group.alpha, 0.0f, 1.0f, "%.2f")) {
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
            if (ImGui::Checkbox("Interactable##CanvasGroup",
                                &group.interactable)) {
                RecordImmediateEdit(
                    "Toggle CanvasGroup Interactable",
                    std::move(before), selectionBefore);
                status_ =
                    "Toggled Canvas Group interaction.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Blocks Raycasts##CanvasGroup",
                                &group.blocksRaycasts)) {
                RecordImmediateEdit(
                    "Toggle CanvasGroup Raycasts",
                    std::move(before), selectionBefore);
                status_ =
                    "Toggled Canvas Group raycast blocking.";
            }
            ImGui::TextDisabled(
                "Settings affect UI on this Entity and its children.");
        }
    }

    if (entity->eventSystem) {
        ImGui::SeparatorText("Event System");
        if (ImGui::Button("Remove Event System")) {
            const std::string before =
                WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->eventSystem.reset();
            RecordImmediateEdit("Remove EventSystem", before,
                                selectionBefore);
            status_ = "Removed Event System.";
        } else {
            EventSystemComponent& eventSystem =
                *entity->eventSystem;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##EventSystem",
                                &eventSystem.enabled)) {
                RecordImmediateEdit("Toggle EventSystem",
                                    std::move(before),
                                    selectionBefore);
                status_ = "Toggled Event System.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox(
                    "Send Navigation Events##EventSystem",
                    &eventSystem.sendNavigationEvents)) {
                RecordImmediateEdit(
                    "Toggle EventSystem Navigation",
                    std::move(before), selectionBefore);
                status_ =
                    "Toggled Event System navigation events.";
            }

            const WorldEntity* selectedEntity =
                world_.Find(eventSystem.firstSelected);
            std::string selectedLabel =
                selectedEntity != nullptr
                    ? selectedEntity->name
                    : eventSystem.firstSelected.IsValid()
                          ? "Missing Entity"
                          : "None";
            selectedLabel += "##EventSystemFirstSelected";
            ImGui::TextUnformatted("First Selected");
            ImGui::SameLine();
            if (ImGui::Button(selectedLabel.c_str(),
                              {-FLT_MIN, 0.0f})) {
                ImGui::OpenPopup("EventSystemFirstSelectedPicker");
            }
            const auto assignFirstSelected = [&](EntityId value) {
                if (eventSystem.firstSelected == value) {
                    return;
                }
                const std::string targetBefore =
                    WorldSerializer::Serialize(world_);
                eventSystem.firstSelected = value;
                RecordImmediateEdit(
                    "Assign EventSystem First Selected",
                    targetBefore, selectionBefore);
                status_ = "Assigned Event System first selection.";
            };
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(
                            kEntityDragPayload);
                    payload != nullptr && payload->IsDelivery() &&
                    payload->DataSize == sizeof(EntityId)) {
                    EntityId dropped{};
                    std::memcpy(&dropped, payload->Data,
                                sizeof(dropped));
                    const WorldEntity* droppedEntity =
                        world_.Find(dropped);
                    if (droppedEntity != nullptr &&
                        (droppedEntity->button ||
                         droppedEntity->slider)) {
                        assignFirstSelected(dropped);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopup(
                    "EventSystemFirstSelectedPicker")) {
                if (ImGui::MenuItem(
                        "None", nullptr,
                        !eventSystem.firstSelected.IsValid())) {
                    assignFirstSelected({});
                }
                ImGui::Separator();
                for (const WorldEntity& candidate :
                     world_.Entities()) {
                    if (!candidate.button && !candidate.slider) {
                        continue;
                    }
                    const std::string candidateLabel =
                        candidate.name + "##" +
                        candidate.id.ToString();
                    if (ImGui::MenuItem(
                            candidateLabel.c_str(), nullptr,
                            candidate.id ==
                                eventSystem.firstSelected)) {
                        assignFirstSelected(candidate.id);
                    }
                }
                ImGui::EndPopup();
            }
            const size_t eventSystemCount =
                static_cast<size_t>(std::ranges::count_if(
                    world_.Entities(),
                    [](const WorldEntity& candidate) {
                        return candidate.eventSystem &&
                               candidate.eventSystem->enabled;
                    }));
            if (eventSystemCount > 1u) {
                ImGui::TextColored(
                    {1.0f, 0.72f, 0.25f, 1.0f},
                    "Multiple enabled Event Systems exist.");
            }
        }
    }

    if (entity->text) {
        ImGui::SeparatorText("Text");
        if (ImGui::Button("Remove Text")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->text.reset();
            RecordImmediateEdit("Remove Text", before, selectionBefore);
            status_ = "Removed Text.";
        } else {
            TextComponent& text = *entity->text;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Text", &text.enabled)) {
                RecordImmediateEdit("Toggle Text", std::move(before),
                                    selectionBefore);
            }
            const UiAnchorChoice& textAnchor =
                GetUiAnchorChoice(text.anchor);
            if (ImGui::BeginCombo("Anchor##Text", textAnchor.label)) {
                for (const UiAnchorChoice& choice : kUiAnchorChoices) {
                    if (ImGui::Selectable(choice.label,
                                          text.anchor == choice.value)) {
                        const std::string anchorBefore =
                            WorldSerializer::Serialize(world_);
                        text.anchor = choice.value;
                        RecordImmediateEdit("Modify Text Anchor",
                                            anchorBefore, selectionBefore);
                        status_ = "Modified Text anchor.";
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled(
                "Position is an offset from the selected anchor.");

            std::array<char, 4097> textBuffer{};
            std::memcpy(textBuffer.data(), text.text.data(),
                        (std::min)(text.text.size(), textBuffer.size() - 1u));
            if (ImGui::InputTextMultiline("Content##Text", textBuffer.data(),
                                          textBuffer.size(), {-FLT_MIN, 80.0f})) {
                text.text = textBuffer.data();
                RefreshDirty();
                status_ = "Modified Text content.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Text");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragFloat2("Position##Text", &text.position.x, 1.0f,
                                  -1000000.0f, 1000000.0f, "%.1f",
                                  ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Text position.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Text");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            const std::string fontLabel =
                text.fontPath.empty() ? "Default" : text.fontPath;
            if (ImGui::Button((fontLabel + "##TextFont").c_str(),
                              {-FLT_MIN, 0.0f})) {
                ImGui::OpenPopup("TextFontPicker");
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kFontAssetDragPayload);
                    payload != nullptr && payload->IsDelivery() &&
                    payload->DataSize > 1 &&
                    static_cast<const char*>(
                        payload->Data)[payload->DataSize - 1] == '\0') {
                    AssignTextFont(selection_,
                                   static_cast<const char*>(payload->Data));
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopup("TextFontPicker")) {
                if (ImGui::MenuItem("Default", nullptr,
                                    text.fontPath.empty())) {
                    const std::string clearBefore =
                        WorldSerializer::Serialize(world_);
                    text.fontPath.clear();
                    RecordImmediateEdit("Clear Text Font", clearBefore,
                                        selectionBefore);
                    status_ = "Reset Text to the default font.";
                }
                ImGui::Separator();
                for (const std::filesystem::path& fontAsset : fontAssets_) {
                    const std::string reference =
                        "asset://" +
                        fontAsset.lexically_relative("assets").generic_string();
                    const std::string label =
                        fontAsset.filename().generic_string() + "##TextFont" +
                        fontAsset.generic_string();
                    if (ImGui::MenuItem(label.c_str(), nullptr,
                                        text.fontPath == reference)) {
                        AssignTextFont(selection_, fontAsset);
                    }
                }
                ImGui::EndPopup();
            }
            if (!text.fontPath.empty()) {
                const std::optional<std::filesystem::path> resolvedFont =
                    ResolveProjectAssetPath(text.fontPath);
                if (!resolvedFont ||
                    !AssetImport::IsFontFile(*resolvedFont)) {
                    ImGui::TextColored(
                        {1.0f, 0.4f, 0.3f, 1.0f},
                        "The assigned font asset is missing or invalid.");
                } else if (const auto loadedFont =
                               loadedFonts_.find(text.fontPath);
                           loadedFont != loadedFonts_.end() &&
                           !loadedFont->second.IsValid()) {
                    ImGui::TextColored(
                        {1.0f, 0.72f, 0.25f, 1.0f},
                        "The font could not be loaded; using the default.");
                }
            }
            if (ImGui::DragFloat("Font Size##Text", &text.fontSize, 0.5f,
                                 1.0f, 512.0f, "%.1f",
                                 ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Text font size.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Text");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragFloat("Line Spacing##Text",
                                 &text.lineSpacing, 0.5f, 0.0f, 512.0f,
                                 "%.1f",
                                 ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Text line spacing.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Text Line Spacing");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragFloat("Wrap Width##Text",
                                 &text.wrapWidth, 1.0f, 0.0f, 16384.0f,
                                 "%.1f",
                                 ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Text wrap width.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Text Wrap Width");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            ImGui::TextDisabled("0 disables automatic wrapping.");
            if (ImGui::ColorEdit4("Color##Text", &text.color.x)) {
                RefreshDirty();
                status_ = "Modified Text color.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Text");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }

            const char* alignment =
                text.alignment == TextAlignment::Left
                    ? "Left"
                    : text.alignment == TextAlignment::Center ? "Center" : "Right";
            if (ImGui::BeginCombo("Alignment##Text", alignment)) {
                const auto selectAlignment = [&](TextAlignment value,
                                                 const char* label) {
                    if (ImGui::Selectable(label, text.alignment == value)) {
                        const std::string alignmentBefore =
                            WorldSerializer::Serialize(world_);
                        text.alignment = value;
                        RecordImmediateEdit("Modify Text Alignment",
                                            alignmentBefore, selectionBefore);
                        status_ = "Modified Text alignment.";
                    }
                };
                selectAlignment(TextAlignment::Left, "Left");
                selectAlignment(TextAlignment::Center, "Center");
                selectAlignment(TextAlignment::Right, "Right");
                ImGui::EndCombo();
            }

            const WorldEntity* ancestor = entity;
            while (ancestor != nullptr && !ancestor->canvas) {
                ancestor = ancestor->parent.IsValid()
                               ? world_.Find(ancestor->parent)
                               : nullptr;
            }
            if (ancestor == nullptr) {
                ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                                   "Text requires a Canvas on this Entity or an ancestor.");
            }
        }
    }

    if (entity->image) {
        ImGui::SeparatorText("Image");
        if (ImGui::Button("Remove Image")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            const std::string previousPath = entity->image->texturePath;
            entity->image.reset();
            loadedTextures_.erase(previousPath);
            RecordImmediateEdit("Remove Image", before, selectionBefore);
            status_ = "Removed Image.";
        } else {
            ImageComponent& image = *entity->image;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Image", &image.enabled)) {
                RecordImmediateEdit("Toggle Image", std::move(before),
                                    selectionBefore);
            }
            const char* imageType =
                image.type == ImageType::Filled ? "Filled" : "Simple";
            if (ImGui::BeginCombo("Type##Image", imageType)) {
                if (ImGui::Selectable("Simple",
                                      image.type == ImageType::Simple)) {
                    const std::string typeBefore =
                        WorldSerializer::Serialize(world_);
                    image.type = ImageType::Simple;
                    RecordImmediateEdit("Modify Image Type", typeBefore,
                                        selectionBefore);
                    status_ = "Modified Image type.";
                }
                if (ImGui::Selectable("Filled",
                                      image.type == ImageType::Filled)) {
                    const std::string typeBefore =
                        WorldSerializer::Serialize(world_);
                    image.type = ImageType::Filled;
                    RecordImmediateEdit("Modify Image Type", typeBefore,
                                        selectionBefore);
                    status_ = "Modified Image type.";
                }
                ImGui::EndCombo();
            }
            const std::string preserveAspectBefore =
                WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Preserve Aspect##Image",
                                &image.preserveAspect)) {
                RecordImmediateEdit("Toggle Image Preserve Aspect",
                                    preserveAspectBefore, selectionBefore);
                status_ = "Modified Image aspect preservation.";
            }
            if (image.type == ImageType::Filled) {
                const char* fillMethod =
                    image.fillMethod == ImageFillMethod::Vertical
                        ? "Vertical"
                        : "Horizontal";
                if (ImGui::BeginCombo("Fill Method##Image", fillMethod)) {
                    if (ImGui::Selectable(
                            "Horizontal",
                            image.fillMethod ==
                                ImageFillMethod::Horizontal)) {
                        const std::string methodBefore =
                            WorldSerializer::Serialize(world_);
                        image.fillMethod = ImageFillMethod::Horizontal;
                        RecordImmediateEdit("Modify Image Fill Method",
                                            methodBefore, selectionBefore);
                        status_ = "Modified Image fill method.";
                    }
                    if (ImGui::Selectable(
                            "Vertical",
                            image.fillMethod == ImageFillMethod::Vertical)) {
                        const std::string methodBefore =
                            WorldSerializer::Serialize(world_);
                        image.fillMethod = ImageFillMethod::Vertical;
                        RecordImmediateEdit("Modify Image Fill Method",
                                            methodBefore, selectionBefore);
                        status_ = "Modified Image fill method.";
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::DragFloat("Fill Amount##Image",
                                     &image.fillAmount, 0.01f, 0.0f, 1.0f,
                                     "%.2f",
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Image fill amount.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Image Fill Amount");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
                const std::string reverseBefore =
                    WorldSerializer::Serialize(world_);
                if (ImGui::Checkbox("Reverse Fill##Image",
                                    &image.fillReverse)) {
                    RecordImmediateEdit("Toggle Image Reverse Fill",
                                        reverseBefore, selectionBefore);
                    status_ = "Modified Image fill direction.";
                }
                ImGui::TextDisabled(
                    image.fillMethod == ImageFillMethod::Horizontal
                        ? "Fills left-to-right; Reverse fills right-to-left."
                        : "Fills bottom-to-top; Reverse fills top-to-bottom.");
            }
            const UiAnchorChoice& imageAnchor =
                GetUiAnchorChoice(image.anchor);
            if (ImGui::BeginCombo("Anchor##Image", imageAnchor.label)) {
                for (const UiAnchorChoice& choice : kUiAnchorChoices) {
                    if (ImGui::Selectable(choice.label,
                                          image.anchor == choice.value)) {
                        const std::string anchorBefore =
                            WorldSerializer::Serialize(world_);
                        image.anchor = choice.value;
                        RecordImmediateEdit("Modify Image Anchor",
                                            anchorBefore, selectionBefore);
                        status_ = "Modified Image anchor.";
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled(
                "Position is an offset from the selected anchor.");
            if (ImGui::DragFloat2("Pivot##Image", &image.pivot.x, 0.01f,
                                  0.0f, 1.0f, "%.2f",
                                  ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Image pivot.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Image Pivot");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            ImGui::TextDisabled(
                "Pivot (0,0) is top-left; (1,1) is bottom-right.");

            const std::string textureLabel =
                image.texturePath.empty() ? "None (solid color)"
                                          : image.texturePath;
            if (ImGui::Button((textureLabel + "##ImageTexture").c_str(),
                              {-FLT_MIN, 0.0f})) {
                ImGui::OpenPopup("ImageTexturePicker");
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kTextureAssetDragPayload);
                    payload != nullptr && payload->IsDelivery() &&
                    payload->DataSize > 1 &&
                    static_cast<const char*>(
                        payload->Data)[payload->DataSize - 1] == '\0') {
                    AssignImageTexture(selection_,
                                       static_cast<const char*>(payload->Data));
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopup("ImageTexturePicker")) {
                if (ImGui::MenuItem("None (solid color)", nullptr,
                                    image.texturePath.empty())) {
                    const std::string clearBefore =
                        WorldSerializer::Serialize(world_);
                    const std::string previousPath = image.texturePath;
                    image.texturePath.clear();
                    loadedTextures_.erase(previousPath);
                    RecordImmediateEdit("Clear Image Texture", clearBefore,
                                        selectionBefore);
                    status_ = "Cleared Image texture.";
                }
                ImGui::Separator();
                for (const std::filesystem::path& textureAsset : textureAssets_) {
                    const std::string reference =
                        "asset://" +
                        textureAsset.lexically_relative("assets").generic_string();
                    const std::string label =
                        textureAsset.filename().generic_string() + "##Image" +
                        textureAsset.generic_string();
                    if (ImGui::MenuItem(label.c_str(), nullptr,
                                        image.texturePath == reference)) {
                        AssignImageTexture(selection_, textureAsset);
                    }
                }
                ImGui::EndPopup();
            }

            if (ImGui::DragFloat2("Position##Image", &image.position.x, 1.0f,
                                  -1000000.0f, 1000000.0f, "%.1f",
                                  ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Image position.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Image");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragFloat2("Size##Image", &image.size.x, 1.0f, 0.0f,
                                  1000000.0f, "%.1f",
                                  ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Image size.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Image");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::ColorEdit4("Color##Image", &image.color.x)) {
                RefreshDirty();
                status_ = "Modified Image color.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Image");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }

            const TextureHandle texture =
                image.texturePath.empty()
                    ? TextureHandle{}
                    : loadedTextures_.contains(image.texturePath)
                          ? loadedTextures_.at(image.texturePath)
                          : TextureHandle{};
            const bool textureAvailable =
                texture.IsValid() && ctx_ != nullptr &&
                ctx_->rendering.texture != nullptr &&
                ctx_->rendering.texture->IsValidTexture(texture);
            ImGui::BeginDisabled(!textureAvailable);
            if (ImGui::Button("Set Native Size##Image")) {
                const std::string nativeSizeBefore =
                    WorldSerializer::Serialize(world_);
                image.size = {
                    static_cast<float>(
                        ctx_->rendering.texture->GetWidth(texture)),
                    static_cast<float>(
                        ctx_->rendering.texture->GetHeight(texture)),
                };
                RecordImmediateEdit("Set Image Native Size",
                                    nativeSizeBefore, selectionBefore);
                status_ = "Set Image to its texture's native size.";
            }
            ImGui::EndDisabled();
            if (textureAvailable) {
                const D3D12_GPU_DESCRIPTOR_HANDLE handle =
                    ctx_->rendering.texture->GetGpuHandle(texture);
                ImGui::Image(static_cast<ImTextureID>(handle.ptr),
                             {64.0f, 64.0f});
            } else if (!image.texturePath.empty()) {
                ImGui::TextDisabled("Texture is loading or unavailable.");
            }

            const WorldEntity* ancestor = entity;
            while (ancestor != nullptr && !ancestor->canvas) {
                ancestor = ancestor->parent.IsValid()
                               ? world_.Find(ancestor->parent)
                               : nullptr;
            }
            if (ancestor == nullptr) {
                ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                                   "Image requires a Canvas on this Entity or an ancestor.");
            }
        }
    }

    if (entity->button) {
        ImGui::SeparatorText("Button");
        if (ImGui::Button("Remove Button")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->button.reset();
            RecordImmediateEdit("Remove Button", before, selectionBefore);
            status_ = "Removed Button.";
        } else {
            ButtonComponent& button = *entity->button;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Button", &button.enabled)) {
                RecordImmediateEdit("Toggle Button", std::move(before),
                                    selectionBefore);
                status_ = "Toggled Button.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Interactable##Button",
                                &button.interactable)) {
                RecordImmediateEdit("Toggle Button Interactable",
                                    std::move(before), selectionBefore);
                status_ = "Toggled Button interactable.";
            }
            const char* navigation =
                button.navigation == ButtonNavigationMode::None
                    ? "None"
                    : button.navigation ==
                              ButtonNavigationMode::Explicit
                          ? "Explicit"
                          : "Automatic";
            if (ImGui::BeginCombo("Navigation##Button", navigation)) {
                const auto selectNavigation =
                    [&](ButtonNavigationMode value, const char* label) {
                        if (ImGui::Selectable(label,
                                              button.navigation == value)) {
                            const std::string navigationBefore =
                                WorldSerializer::Serialize(world_);
                            button.navigation = value;
                            RecordImmediateEdit(
                                "Change Button Navigation",
                                std::move(navigationBefore), selectionBefore);
                            status_ = "Changed Button navigation.";
                        }
                    };
                selectNavigation(ButtonNavigationMode::Automatic,
                                 "Automatic");
                selectNavigation(ButtonNavigationMode::Explicit,
                                 "Explicit");
                selectNavigation(ButtonNavigationMode::None, "None");
                ImGui::EndCombo();
            }
            if (button.navigation ==
                ButtonNavigationMode::Explicit) {
                const auto editNavigationTarget =
                    [&](const char* label, const char* popup,
                        EntityId& target) {
                        const WorldEntity* targetEntity =
                            world_.Find(target);
                        std::string targetLabel =
                            targetEntity != nullptr
                                ? targetEntity->name
                                : target.IsValid() ? "Missing Entity"
                                                   : "None";
                        targetLabel += "##";
                        targetLabel += label;
                        ImGui::TextUnformatted(label);
                        ImGui::SameLine();
                        if (ImGui::Button(targetLabel.c_str(),
                                          {-FLT_MIN, 0.0f})) {
                            ImGui::OpenPopup(popup);
                        }
                        const auto assignTarget = [&](EntityId value) {
                            if (target == value) {
                                return;
                            }
                            const std::string targetBefore =
                                WorldSerializer::Serialize(world_);
                            target = value;
                            RecordImmediateEdit(
                                "Assign Button Navigation",
                                targetBefore, selectionBefore);
                            status_ =
                                "Assigned Button navigation target.";
                        };
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(
                                        kEntityDragPayload);
                                payload != nullptr &&
                                payload->IsDelivery() &&
                                payload->DataSize ==
                                    sizeof(EntityId)) {
                                EntityId dropped{};
                                std::memcpy(&dropped, payload->Data,
                                            sizeof(dropped));
                                const WorldEntity* droppedEntity =
                                    world_.Find(dropped);
                                if (droppedEntity != nullptr &&
                                    dropped != entity->id &&
                                    (droppedEntity->button ||
                                     droppedEntity->slider)) {
                                    assignTarget(dropped);
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                        if (ImGui::BeginPopup(popup)) {
                            if (ImGui::MenuItem(
                                    "None", nullptr,
                                    !target.IsValid())) {
                                assignTarget({});
                            }
                            ImGui::Separator();
                            for (const WorldEntity& candidate :
                                 world_.Entities()) {
                                if (candidate.id == entity->id ||
                                    (!candidate.button &&
                                     !candidate.slider)) {
                                    continue;
                                }
                                const std::string candidateLabel =
                                    candidate.name + "##" +
                                    candidate.id.ToString();
                                if (ImGui::MenuItem(
                                        candidateLabel.c_str(), nullptr,
                                        candidate.id == target)) {
                                    assignTarget(candidate.id);
                                }
                            }
                            ImGui::EndPopup();
                        }
                    };
                editNavigationTarget(
                    "Select On Left", "SelectOnLeftPicker",
                    button.selectOnLeft);
                editNavigationTarget(
                    "Select On Right", "SelectOnRightPicker",
                    button.selectOnRight);
                editNavigationTarget(
                    "Select On Up", "SelectOnUpPicker",
                    button.selectOnUp);
                editNavigationTarget(
                    "Select On Down", "SelectOnDownPicker",
                    button.selectOnDown);
            }
            const auto editButtonColor =
                [&](const char* label, DirectX::XMFLOAT4& color) {
                    if (ImGui::ColorEdit4(label, &color.x)) {
                        RefreshDirty();
                        status_ = "Modified Button colors.";
                    }
                    if (ImGui::IsItemActivated()) {
                        BeginHistoryEdit("Modify Button Color");
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        CommitHistoryEdit();
                    }
                };
            editButtonColor("Normal Color##Button", button.normalColor);
            editButtonColor("Hovered Color##Button", button.hoveredColor);
            editButtonColor("Pressed Color##Button", button.pressedColor);
            editButtonColor("Disabled Color##Button", button.disabledColor);
            if (ImGui::DragFloat("Fade Duration##Button",
                                 &button.fadeDuration, 0.01f, 0.0f, 10.0f,
                                 "%.2f s")) {
                button.fadeDuration =
                    std::clamp(button.fadeDuration, 0.0f, 10.0f);
                RefreshDirty();
                status_ = "Modified Button fade duration.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Button Fade Duration");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (!entity->image) {
                ImGui::TextColored(
                    {1.0f, 0.72f, 0.25f, 1.0f},
                    "Button requires an Image on the same Entity.");
            }
            if (entity->scripts.empty()) {
                ImGui::TextDisabled(
                    "Add a Script and override OnButtonClick to handle clicks.");
            }
        }
    }

    if (entity->toggle) {
        ImGui::SeparatorText("Toggle");
        if (ImGui::Button("Remove Toggle")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->toggle.reset();
            RecordImmediateEdit("Remove Toggle", before, selectionBefore);
            status_ = "Removed Toggle.";
        } else {
            ToggleComponent& toggle = *entity->toggle;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Toggle", &toggle.enabled)) {
                RecordImmediateEdit("Toggle Toggle", std::move(before),
                                    selectionBefore);
                status_ = "Toggled Toggle.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Is On##Toggle", &toggle.isOn)) {
                RecordImmediateEdit("Change Toggle Value",
                                    std::move(before), selectionBefore);
                status_ = "Changed Toggle value.";
            }
            if (ImGui::ColorEdit4("Checkmark Color##Toggle",
                                  &toggle.checkmarkColor.x)) {
                RefreshDirty();
                status_ = "Modified Toggle checkmark color.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Toggle Checkmark Color");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragFloat("Checkmark Scale##Toggle",
                                 &toggle.checkmarkScale, 0.01f, 0.0f, 1.0f,
                                 "%.2f")) {
                toggle.checkmarkScale =
                    std::clamp(toggle.checkmarkScale, 0.0f, 1.0f);
                RefreshDirty();
                status_ = "Modified Toggle checkmark scale.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Toggle Checkmark Scale");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (!entity->button || !entity->image) {
                ImGui::TextColored(
                    {1.0f, 0.72f, 0.25f, 1.0f},
                    "Toggle requires Button and Image on the same Entity.");
            }
            if (entity->scripts.empty()) {
                ImGui::TextDisabled(
                    "Override OnToggleValueChanged to handle value changes.");
            }
        }
    }

    if (entity->slider) {
        ImGui::SeparatorText("Slider");
        if (ImGui::Button("Remove Slider")) {
            const std::string before =
                WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->slider.reset();
            RecordImmediateEdit("Remove Slider", before,
                                selectionBefore);
            status_ = "Removed Slider.";
        } else {
            SliderComponent& slider = *entity->slider;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Slider",
                                &slider.enabled)) {
                RecordImmediateEdit("Toggle Slider",
                                    std::move(before),
                                    selectionBefore);
                status_ = "Toggled Slider.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Interactable##Slider",
                                &slider.interactable)) {
                RecordImmediateEdit("Toggle Slider Interactable",
                                    std::move(before),
                                    selectionBefore);
                status_ = "Toggled Slider interaction.";
            }
            if (ImGui::DragFloat("Min Value##Slider",
                                 &slider.minValue, 0.1f,
                                 -1000000.0f, 999999.0f, "%.3f",
                                 ImGuiSliderFlags_AlwaysClamp)) {
                slider.minValue =
                    (std::min)(slider.minValue,
                               slider.maxValue - 0.001f);
                slider.value = std::clamp(
                    slider.value, slider.minValue, slider.maxValue);
                RefreshDirty();
                status_ = "Modified Slider range.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Slider Range");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragFloat("Max Value##Slider",
                                 &slider.maxValue, 0.1f,
                                 -999999.0f, 1000000.0f, "%.3f",
                                 ImGuiSliderFlags_AlwaysClamp)) {
                slider.maxValue =
                    (std::max)(slider.maxValue,
                               slider.minValue + 0.001f);
                slider.value = std::clamp(
                    slider.value, slider.minValue, slider.maxValue);
                RefreshDirty();
                status_ = "Modified Slider range.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Slider Range");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::SliderFloat("Value##Slider", &slider.value,
                                   slider.minValue, slider.maxValue,
                                   "%.3f")) {
                if (slider.wholeNumbers) {
                    slider.value = std::clamp(
                        std::round(slider.value), slider.minValue,
                        slider.maxValue);
                }
                RefreshDirty();
                status_ = "Modified Slider value.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Slider Value");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Whole Numbers##Slider",
                                &slider.wholeNumbers)) {
                if (slider.wholeNumbers) {
                    slider.value = std::clamp(
                        std::round(slider.value), slider.minValue,
                        slider.maxValue);
                }
                RecordImmediateEdit("Toggle Slider Whole Numbers",
                                    std::move(before),
                                    selectionBefore);
                status_ = "Changed Slider whole-number mode.";
            }
            const char* direction =
                slider.direction == SliderDirection::RightToLeft
                    ? "Right To Left"
                    : slider.direction == SliderDirection::BottomToTop
                          ? "Bottom To Top"
                          : slider.direction ==
                                    SliderDirection::TopToBottom
                                ? "Top To Bottom"
                                : "Left To Right";
            if (ImGui::BeginCombo("Direction##Slider", direction)) {
                const auto selectDirection =
                    [&](SliderDirection value, const char* label) {
                        if (ImGui::Selectable(
                                label, slider.direction == value)) {
                            const std::string directionBefore =
                                WorldSerializer::Serialize(world_);
                            slider.direction = value;
                            RecordImmediateEdit(
                                "Change Slider Direction",
                                std::move(directionBefore),
                                selectionBefore);
                            status_ = "Changed Slider direction.";
                        }
                    };
                selectDirection(SliderDirection::LeftToRight,
                                "Left To Right");
                selectDirection(SliderDirection::RightToLeft,
                                "Right To Left");
                selectDirection(SliderDirection::BottomToTop,
                                "Bottom To Top");
                selectDirection(SliderDirection::TopToBottom,
                                "Top To Bottom");
                ImGui::EndCombo();
            }
            const auto editSliderColor =
                [&](const char* label, DirectX::XMFLOAT4& color) {
                    if (ImGui::ColorEdit4(label, &color.x)) {
                        RefreshDirty();
                        status_ = "Modified Slider colors.";
                    }
                    if (ImGui::IsItemActivated()) {
                        BeginHistoryEdit("Modify Slider Color");
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        CommitHistoryEdit();
                    }
                };
            editSliderColor("Fill Color##Slider",
                            slider.fillColor);
            editSliderColor("Handle Color##Slider",
                            slider.handleColor);
            if (ImGui::DragFloat("Handle Size##Slider",
                                 &slider.handleSize, 1.0f, 0.0f,
                                 1000000.0f, "%.1f",
                                 ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Slider handle size.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Slider Handle Size");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (!entity->image) {
                ImGui::TextColored(
                    {1.0f, 0.72f, 0.25f, 1.0f},
                    "Slider requires an Image on the same Entity.");
            }
            if (entity->scripts.empty()) {
                ImGui::TextDisabled(
                    "Override OnSliderValueChanged to handle value changes.");
            }
        }
    }

    if (entity->dropdown) {
        ImGui::SeparatorText("Dropdown");
        if (ImGui::Button("Remove Dropdown")) {
            const std::string before =
                WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->dropdown.reset();
            RecordImmediateEdit("Remove Dropdown", before,
                                selectionBefore);
            status_ = "Removed Dropdown.";
        } else {
            DropdownComponent& dropdown = *entity->dropdown;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Dropdown",
                                &dropdown.enabled)) {
                RecordImmediateEdit("Toggle Dropdown",
                                    std::move(before),
                                    selectionBefore);
                status_ = "Toggled Dropdown.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Interactable##Dropdown",
                                &dropdown.interactable)) {
                RecordImmediateEdit("Toggle Dropdown Interactable",
                                    std::move(before),
                                    selectionBefore);
                status_ = "Toggled Dropdown interaction.";
            }
            const char* selectedOption =
                dropdown.options[static_cast<size_t>(
                    std::clamp(dropdown.value, 0,
                               static_cast<int32_t>(
                                   dropdown.options.size() - 1u)))]
                    .c_str();
            if (ImGui::BeginCombo("Value##Dropdown",
                                  selectedOption)) {
                for (size_t optionIndex = 0;
                     optionIndex < dropdown.options.size();
                     ++optionIndex) {
                    const bool selected =
                        dropdown.value ==
                        static_cast<int32_t>(optionIndex);
                    if (ImGui::Selectable(
                            dropdown.options[optionIndex].c_str(),
                            selected)) {
                        const std::string valueBefore =
                            WorldSerializer::Serialize(world_);
                        dropdown.value =
                            static_cast<int32_t>(optionIndex);
                        RecordImmediateEdit(
                            "Change Dropdown Value",
                            std::move(valueBefore),
                            selectionBefore);
                        status_ = "Changed Dropdown value.";
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SeparatorText("Options");
            for (size_t optionIndex = 0;
                 optionIndex < dropdown.options.size();) {
                ImGui::PushID(
                    static_cast<int>(optionIndex));
                std::array<char, 257> optionBuffer{};
                const std::string& option =
                    dropdown.options[optionIndex];
                std::copy_n(
                    option.data(),
                    (std::min)(option.size(),
                               optionBuffer.size() - 1u),
                    optionBuffer.data());
                ImGui::SetNextItemWidth(-150.0f);
                if (ImGui::InputText("##Option",
                                     optionBuffer.data(),
                                     optionBuffer.size()) &&
                    optionBuffer[0] != '\0') {
                    dropdown.options[optionIndex] =
                        optionBuffer.data();
                    RefreshDirty();
                    status_ = "Modified Dropdown option.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit(
                        "Modify Dropdown Option");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
                ImGui::SameLine();
                if (optionIndex == 0u) {
                    ImGui::BeginDisabled();
                }
                const bool moveUp =
                    ImGui::SmallButton("Up");
                if (optionIndex == 0u) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                if (optionIndex + 1u >=
                    dropdown.options.size()) {
                    ImGui::BeginDisabled();
                }
                const bool moveDown =
                    ImGui::SmallButton("Down");
                if (optionIndex + 1u >=
                    dropdown.options.size()) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                const bool canRemove =
                    dropdown.options.size() > 1u;
                if (!canRemove) {
                    ImGui::BeginDisabled();
                }
                const bool remove =
                    ImGui::SmallButton("Remove");
                if (!canRemove) {
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
                if (moveUp || moveDown) {
                    const std::string moveBefore =
                        WorldSerializer::Serialize(world_);
                    const size_t destination =
                        moveUp ? optionIndex - 1u
                               : optionIndex + 1u;
                    std::swap(dropdown.options[optionIndex],
                              dropdown.options[destination]);
                    if (dropdown.value ==
                        static_cast<int32_t>(optionIndex)) {
                        dropdown.value =
                            static_cast<int32_t>(destination);
                    } else if (
                        dropdown.value ==
                        static_cast<int32_t>(destination)) {
                        dropdown.value =
                            static_cast<int32_t>(optionIndex);
                    }
                    RecordImmediateEdit(
                        "Move Dropdown Option",
                        std::move(moveBefore),
                        selectionBefore);
                    status_ = "Moved Dropdown option.";
                    break;
                }
                if (remove) {
                    const std::string removeBefore =
                        WorldSerializer::Serialize(world_);
                    dropdown.options.erase(
                        dropdown.options.begin() +
                        static_cast<std::ptrdiff_t>(
                            optionIndex));
                    dropdown.value = std::clamp(
                        dropdown.value, 0,
                        static_cast<int32_t>(
                            dropdown.options.size() - 1u));
                    RecordImmediateEdit(
                        "Remove Dropdown Option",
                        std::move(removeBefore),
                        selectionBefore);
                    status_ = "Removed Dropdown option.";
                    continue;
                }
                ++optionIndex;
            }
            if (dropdown.options.size() < 256u &&
                ImGui::Button("Add Option##Dropdown")) {
                const std::string addBefore =
                    WorldSerializer::Serialize(world_);
                dropdown.options.push_back("Option");
                RecordImmediateEdit(
                    "Add Dropdown Option",
                    std::move(addBefore), selectionBefore);
                status_ = "Added Dropdown option.";
            }
            const auto editDropdownColor =
                [&](const char* label,
                    DirectX::XMFLOAT4& color) {
                    if (ImGui::ColorEdit4(label, &color.x)) {
                        RefreshDirty();
                        status_ = "Modified Dropdown colors.";
                    }
                    if (ImGui::IsItemActivated()) {
                        BeginHistoryEdit(
                            "Modify Dropdown Color");
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        CommitHistoryEdit();
                    }
                };
            editDropdownColor("Item Color##Dropdown",
                              dropdown.itemColor);
            editDropdownColor(
                "Highlighted Color##Dropdown",
                dropdown.highlightedColor);
            if (ImGui::DragFloat(
                    "Item Height##Dropdown",
                    &dropdown.itemHeight, 1.0f, 1.0f,
                    1000000.0f, "%.1f",
                    ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Dropdown item height.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit(
                    "Modify Dropdown Item Height");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (!entity->button || !entity->image ||
                !entity->text) {
                ImGui::TextColored(
                    {1.0f, 0.72f, 0.25f, 1.0f},
                    "Dropdown requires Button, Image, and Text on the same Entity.");
            }
            if (entity->scripts.empty()) {
                ImGui::TextDisabled(
                    "Override OnDropdownValueChanged to handle value changes.");
            }
        }
    }

    if (entity->inputField) {
        ImGui::SeparatorText("Input Field");
        if (ImGui::Button("Remove Input Field")) {
            const std::string before =
                WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->inputField.reset();
            RecordImmediateEdit("Remove InputField", before,
                                selectionBefore);
            status_ = "Removed Input Field.";
        } else {
            InputFieldComponent& inputField =
                *entity->inputField;
            const EntityId selectionBefore = selection_;
            std::string before =
                WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##InputField",
                                &inputField.enabled)) {
                RecordImmediateEdit("Toggle InputField",
                                    std::move(before),
                                    selectionBefore);
                status_ = "Toggled Input Field.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox(
                    "Interactable##InputField",
                    &inputField.interactable)) {
                RecordImmediateEdit(
                    "Toggle InputField Interactable",
                    std::move(before), selectionBefore);
                status_ = "Toggled Input Field interaction.";
            }
            std::array<char, 4097> textBuffer{};
            std::copy_n(
                inputField.text.data(),
                (std::min)(inputField.text.size(),
                           textBuffer.size() - 1u),
                textBuffer.data());
            if (ImGui::InputText("Text##InputField",
                                 textBuffer.data(),
                                 textBuffer.size())) {
                inputField.text = textBuffer.data();
                RefreshDirty();
                status_ = "Modified Input Field text.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify InputField Text");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            std::array<char, 1025> placeholderBuffer{};
            std::copy_n(
                inputField.placeholder.data(),
                (std::min)(inputField.placeholder.size(),
                           placeholderBuffer.size() - 1u),
                placeholderBuffer.data());
            if (ImGui::InputText(
                    "Placeholder##InputField",
                    placeholderBuffer.data(),
                    placeholderBuffer.size())) {
                inputField.placeholder =
                    placeholderBuffer.data();
                RefreshDirty();
                status_ =
                    "Modified Input Field placeholder.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit(
                    "Modify InputField Placeholder");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragInt(
                    "Character Limit##InputField",
                    &inputField.characterLimit, 1.0f, 0,
                    4096, "%d",
                    ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ =
                    "Modified Input Field character limit.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit(
                    "Modify InputField Character Limit");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            const char* contentType =
                inputField.contentType ==
                        InputFieldContentType::Password
                    ? "Password"
                    : "Standard";
            if (ImGui::BeginCombo(
                    "Content Type##InputField",
                    contentType)) {
                const auto selectContentType =
                    [&](InputFieldContentType value,
                        const char* label) {
                        if (ImGui::Selectable(
                                label,
                                inputField.contentType ==
                                    value)) {
                            const std::string typeBefore =
                                WorldSerializer::Serialize(
                                    world_);
                            inputField.contentType = value;
                            RecordImmediateEdit(
                                "Change InputField Content Type",
                                std::move(typeBefore),
                                selectionBefore);
                            status_ =
                                "Changed Input Field content type.";
                        }
                    };
                selectContentType(
                    InputFieldContentType::Standard,
                    "Standard");
                selectContentType(
                    InputFieldContentType::Password,
                    "Password");
                ImGui::EndCombo();
            }
            if (!entity->button || !entity->image ||
                !entity->text) {
                ImGui::TextColored(
                    {1.0f, 0.72f, 0.25f, 1.0f},
                    "Input Field requires Button, Image, and Text on the same Entity.");
            }
            if (entity->scripts.empty()) {
                ImGui::TextDisabled(
                    "Override OnInputFieldValueChanged or OnInputFieldSubmit to handle input.");
            }
        }
    }

    if (entity->materialOverride) {
        ImGui::SeparatorText("Material Override");
        if (ImGui::Button("Remove Material Override")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->materialOverride.reset();
            RecordImmediateEdit("Remove Material Override", before, selectionBefore);
            status_ = "Removed Material Override.";
        } else {
            MaterialOverrideComponent& material = *entity->materialOverride;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##MaterialOverride", &material.enabled)) {
                RecordImmediateEdit("Toggle Material Override", std::move(before),
                                    selectionBefore);
            }
            int blendMode = static_cast<int>(material.blendMode);
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("Blend Mode##MaterialOverride", &blendMode,
                             "Opaque\0Cutout\0Transparent\0")) {
                material.blendMode = static_cast<MaterialSurfaceBlendMode>(blendMode);
                RecordImmediateEdit("Change Material Blend Mode", std::move(before),
                                    selectionBefore);
            }
            if (material.blendMode == MaterialSurfaceBlendMode::Cutout) {
                if (ImGui::DragFloat("Alpha Cutoff##MaterialOverride", &material.alphaCutoff,
                                     0.01f, 0.0f, 1.0f, "%.3f",
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Material Override.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Alpha Cutoff");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            }
            int cullMode = static_cast<int>(material.cullMode);
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("Cull Mode##MaterialOverride", &cullMode,
                             "None (Double-Sided)\0Front\0Back\0")) {
                material.cullMode = static_cast<MaterialSurfaceCullMode>(cullMode);
                RecordImmediateEdit("Change Material Cull Mode", std::move(before),
                                    selectionBefore);
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Depth Write##MaterialOverride", &material.depthWrite)) {
                RecordImmediateEdit("Toggle Material Depth Write", std::move(before),
                                    selectionBefore);
            }
            if (material.blendMode == MaterialSurfaceBlendMode::Transparent &&
                ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Transparent materials always disable depth writes at draw time.");
            }
            if (ImGui::ColorEdit4("Base Color##MaterialOverride", &material.baseColor.x,
                                  ImGuiColorEditFlags_Float)) {
                RefreshDirty();
                status_ = "Modified Material Override.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Material Base Color");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            auto drawMaterialFloat = [&](const char* label, float& value) {
                if (ImGui::DragFloat(label, &value, 0.01f, 0.0f, 1.0f, "%.3f",
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Material Override.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Material Override");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            drawMaterialFloat("Metallic##MaterialOverride", material.metallic);
            drawMaterialFloat("Roughness##MaterialOverride", material.roughness);
            std::array<char, 512> texturePathBuffer{};
            strncpy_s(texturePathBuffer.data(), texturePathBuffer.size(),
                      material.baseColorTexturePath.c_str(), _TRUNCATE);
            if (ImGui::InputText("Base Color Texture", texturePathBuffer.data(),
                                 texturePathBuffer.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (texturePathBuffer[0] == '\0') {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    const std::string previousPath = material.baseColorTexturePath;
                    material.baseColorTexturePath.clear();
                    loadedTextures_.erase(previousPath);
                    RecordImmediateEdit("Clear Base Color Texture", clearBefore,
                                        selectionBefore);
                    status_ = "Cleared Base Color texture.";
                } else {
                    AssignBaseColorTexture(selection_, texturePathBuffer.data());
                }
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kTextureAssetDragPayload);
                    payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                    static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                    AssignBaseColorTexture(selection_, static_cast<const char*>(payload->Data));
                }
                ImGui::EndDragDropTarget();
            }
            if (!material.baseColorTexturePath.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##BaseColorTexture")) {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    const std::string previousPath = material.baseColorTexturePath;
                    material.baseColorTexturePath.clear();
                    loadedTextures_.erase(previousPath);
                    RecordImmediateEdit("Clear Base Color Texture", clearBefore,
                                        selectionBefore);
                    status_ = "Cleared Base Color texture.";
                }
                const TextureHandle texture = ResolveBaseColorTexture(material);
                if (texture.IsValid() && ctx_ != nullptr && ctx_->rendering.texture != nullptr &&
                    ctx_->rendering.texture->IsValidTexture(texture)) {
                    const D3D12_GPU_DESCRIPTOR_HANDLE handle =
                        ctx_->rendering.texture->GetGpuHandle(texture);
                    ImGui::Image(static_cast<ImTextureID>(handle.ptr), {64.0f, 64.0f});
                } else {
                    ImGui::TextDisabled("Texture is loading or unavailable.");
                }
            }
            if (ImGui::DragFloat("Normal Strength##MaterialOverride", &material.normalStrength,
                                 0.01f, 0.0f, 4.0f, "%.3f",
                                 ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Material Override.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Normal Strength");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            std::array<char, 512> normalPathBuffer{};
            strncpy_s(normalPathBuffer.data(), normalPathBuffer.size(),
                      material.normalTexturePath.c_str(), _TRUNCATE);
            if (ImGui::InputText("Normal Texture", normalPathBuffer.data(),
                                 normalPathBuffer.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (normalPathBuffer[0] == '\0') {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    const std::string previousPath = material.normalTexturePath;
                    material.normalTexturePath.clear();
                    loadedLinearTextures_.erase(previousPath);
                    RecordImmediateEdit("Clear Normal Texture", clearBefore, selectionBefore);
                    status_ = "Cleared Normal texture.";
                } else {
                    AssignNormalTexture(selection_, normalPathBuffer.data());
                }
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kTextureAssetDragPayload);
                    payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                    static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                    AssignNormalTexture(selection_, static_cast<const char*>(payload->Data));
                }
                ImGui::EndDragDropTarget();
            }
            if (!material.normalTexturePath.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##NormalTexture")) {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    const std::string previousPath = material.normalTexturePath;
                    material.normalTexturePath.clear();
                    loadedLinearTextures_.erase(previousPath);
                    RecordImmediateEdit("Clear Normal Texture", clearBefore, selectionBefore);
                    status_ = "Cleared Normal texture.";
                }
                const TextureHandle texture = ResolveNormalTexture(material);
                if (texture.IsValid() && ctx_ != nullptr && ctx_->rendering.texture != nullptr &&
                    ctx_->rendering.texture->IsValidTexture(texture)) {
                    const D3D12_GPU_DESCRIPTOR_HANDLE handle =
                        ctx_->rendering.texture->GetGpuHandle(texture);
                    ImGui::Image(static_cast<ImTextureID>(handle.ptr), {64.0f, 64.0f});
                } else {
                    ImGui::TextDisabled("Normal texture is loading or unavailable.");
                }
            }
            int packing = static_cast<int>(material.pbrTexturePacking);
            const std::string packingBefore = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("PBR Texture Packing", &packing,
                             "Separate\0ORM (R=AO, G=Roughness, B=Metallic)\0Metallic-Roughness (G=Roughness, B=Metallic)\0")) {
                material.pbrTexturePacking =
                    static_cast<MaterialPbrTexturePacking>(packing);
                RecordImmediateEdit("Change PBR Texture Packing", packingBefore,
                                    selectionBefore);
            }
            using TextureAssignFunction =
                void (EditorScene::*)(EntityId, const std::filesystem::path&);
            auto drawLinearTextureSlot = [&](const char* label, const char* id,
                                             std::string& path,
                                             TextureAssignFunction assignTexture) {
                std::array<char, 512> pathBuffer{};
                strncpy_s(pathBuffer.data(), pathBuffer.size(), path.c_str(), _TRUNCATE);
                const std::string inputLabel = std::string(label) + "##" + id;
                if (ImGui::InputText(inputLabel.c_str(), pathBuffer.data(), pathBuffer.size(),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    if (pathBuffer[0] == '\0') {
                        const std::string clearBefore = WorldSerializer::Serialize(world_);
                        const std::string previousPath = path;
                        path.clear();
                        loadedLinearTextures_.erase(previousPath);
                        RecordImmediateEdit(std::string("Clear ") + label, clearBefore,
                                            selectionBefore);
                        status_ = std::string("Cleared ") + label + ".";
                    } else {
                        (this->*assignTexture)(selection_, pathBuffer.data());
                    }
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload(kTextureAssetDragPayload);
                        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                        (this->*assignTexture)(selection_,
                                               static_cast<const char*>(payload->Data));
                    }
                    ImGui::EndDragDropTarget();
                }
                if (path.empty()) {
                    return;
                }
                ImGui::SameLine();
                const std::string clearId = std::string("Clear##") + id;
                if (ImGui::SmallButton(clearId.c_str())) {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    const std::string previousPath = path;
                    path.clear();
                    loadedLinearTextures_.erase(previousPath);
                    RecordImmediateEdit(std::string("Clear ") + label, clearBefore,
                                        selectionBefore);
                    status_ = std::string("Cleared ") + label + ".";
                    return;
                }
                const TextureHandle texture = ResolveLinearTexture(path);
                if (texture.IsValid() && ctx_ != nullptr && ctx_->rendering.texture != nullptr &&
                    ctx_->rendering.texture->IsValidTexture(texture)) {
                    const D3D12_GPU_DESCRIPTOR_HANDLE handle =
                        ctx_->rendering.texture->GetGpuHandle(texture);
                    ImGui::Image(static_cast<ImTextureID>(handle.ptr), {64.0f, 64.0f});
                } else {
                    ImGui::TextDisabled("Texture is loading or unavailable.");
                }
            };
            drawLinearTextureSlot("Roughness Texture", "RoughnessTexture",
                                  material.roughnessTexturePath,
                                  &EditorScene::AssignRoughnessTexture);
            drawLinearTextureSlot("Metallic Texture", "MetallicTexture",
                                  material.metallicTexturePath,
                                  &EditorScene::AssignMetallicTexture);
            if (material.pbrTexturePacking != MaterialPbrTexturePacking::Separate &&
                (material.roughnessTexturePath.empty() ||
                 material.roughnessTexturePath != material.metallicTexturePath)) {
                ImGui::TextColored({1.0f, 0.7f, 0.25f, 1.0f},
                                   "Packed PBR textures must use the same asset in both slots.");
            }
            if (!entity->meshRenderer) {
                ImGui::TextDisabled("Add a Mesh Renderer to display this material.");
            }
        }
    }

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

