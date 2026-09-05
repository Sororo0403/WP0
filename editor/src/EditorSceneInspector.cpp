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
    if (DrawScriptOrderControls(entity, scriptIndex)) {
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
    DrawScriptAssetPicker(behavior, scriptIndex);
    ImGui::TextDisabled("Runtime type: %s", behavior.type.empty() ? "None" : behavior.type.c_str());
    DrawScriptRequirements(entity, behavior, selectionBefore);
    DrawScriptPropertiesInspector(entity, behavior, selectionBefore);
    if (behavior.type == "FirstPersonController" && !entity->characterController) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.25f, 1.0f),
                           "Character Controller is required for collision movement.");
    }
    ImGui::PopID();
    return false;
}

bool EditorScene::DrawScriptOrderControls(WorldEntity* entity, size_t scriptIndex) {
    if (ImGui::Button("Remove Script")) {
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        entity->scripts.erase(entity->scripts.begin() + static_cast<std::ptrdiff_t>(scriptIndex));
        RecordImmediateEdit("Remove Script", before, selectionBefore);
        status_ = "Removed Script component.";
        return true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(scriptIndex == 0u);
    const bool moveUp = ImGui::Button("Move Up");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(scriptIndex + 1u >= entity->scripts.size());
    const bool moveDown = ImGui::Button("Move Down");
    ImGui::EndDisabled();
    if (!moveUp && !moveDown) {
        return false;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const size_t destination = moveUp ? scriptIndex - 1u : scriptIndex + 1u;
    std::swap(entity->scripts[scriptIndex], entity->scripts[destination]);
    RecordImmediateEdit("Reorder Scripts", before, selectionBefore);
    status_ = "Changed Script execution order.";
    return true;
}

void EditorScene::DrawScriptAssetPicker(const BehaviorComponent& behavior, size_t scriptIndex) {
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
    if (!ImGui::BeginPopup("ScriptAssetPicker")) {
        return;
    }
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
            const std::string label = scriptAsset.filename().generic_string() + "##" + assetPath;
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

void EditorScene::DrawScriptRequirements(WorldEntity* entity, BehaviorComponent& behavior,
                                         EntityId selectionBefore) {
    if (behavior.type.empty() || behavior.scriptAssetPath.empty()) {
        ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f}, "Assign a C++ Script asset.");
        return;
    }
    std::string requirementError;
    if (!behaviorRegistry_.ValidateRequirements(behavior.type, *entity, &requirementError)) {
        ImGui::TextColored({1.0f, 0.45f, 0.35f, 1.0f}, "%s", requirementError.c_str());
        const BehaviorRequirements* requirements = behaviorRegistry_.Requirements(behavior.type);
        if (requirements != nullptr && ImGui::Button("Add Required Components")) {
            const std::string before = WorldSerializer::Serialize(world_);
            if (behaviorRegistry_.EnsureRequirements(behavior.type, *entity)) {
                RecordImmediateEdit("Add Script Requirements", before, selectionBefore);
                status_ = "Added required components for Script.";
            } else {
                status_ = "Script requirements could not be added.";
            }
        }
        return;
    }
    const std::string_view registeredSource = behaviorRegistry_.SourceAsset(behavior.type);
    if (registeredSource.empty() || registeredSource != behavior.scriptAssetPath) {
        ImGui::TextColored({1.0f, 0.45f, 0.35f, 1.0f},
                           "The Script asset does not match its registered runtime type.");
    }
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
