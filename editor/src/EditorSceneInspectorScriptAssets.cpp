#include "EditorScene.h"

#include "imgui.h"
#include "input/Input.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "world/WorldSerializer.h"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <vector>

std::string EditorScene::GetAssetScriptPropertyValue(
    const BehaviorComponent& behavior,
    const ScriptPropertyDefinition& definition) const {
    const auto stored =
        std::ranges::find(behavior.properties, definition.name, &ScriptPropertyValue::name);
    return stored != behavior.properties.end() && stored->type == definition.type
               ? stored->stringValue
               : definition.defaultString;
}

void EditorScene::AssignAssetScriptProperty(
    BehaviorComponent& behavior,
    const ScriptPropertyDefinition& definition,
    const std::string& value,
    EntityId selectionBefore) {
    const std::string propertyBefore = WorldSerializer::Serialize(world_);
    auto stored =
        std::ranges::find(behavior.properties, definition.name, &ScriptPropertyValue::name);
    if (stored == behavior.properties.end()) {
        behavior.properties.push_back({});
        stored = std::prev(behavior.properties.end());
    }
    *stored = {};
    stored->name = definition.name;
    stored->type = definition.type;
    stored->stringValue = value;

    const char* historyName = "Modify Script Property";
    const char* status = "Modified Script Animation Clip property.";
    if (definition.type == ScriptPropertyType::InputAction) {
        historyName = "Modify Script Input Action Property";
        status = "Modified Script Input Action property.";
    } else if (definition.type == ScriptPropertyType::Scene) {
        historyName = "Modify Script Scene Property";
        status = "Modified Script Scene property.";
    }
    RecordImmediateEdit(historyName, propertyBefore, selectionBefore);
    status_ = status;
}

void EditorScene::DrawAnimationClipScriptProperty(
    WorldEntity* entity,
    BehaviorComponent& behavior,
    const ScriptPropertyDefinition& definition,
    EntityId selectionBefore) {
    const std::string value = GetAssetScriptPropertyValue(behavior, definition);
    const ModelHandle modelHandle =
        entity->meshRenderer ? ResolveModel(*entity->meshRenderer) : ModelHandle{};
    const Model* animationModel =
        modelHandle.IsValid() && ctx_ != nullptr && ctx_->rendering.model
            ? ctx_->rendering.model->GetModel(modelHandle)
            : nullptr;
    const std::string preview = value.empty() ? "None" : value;
    if (ImGui::BeginCombo(definition.name.c_str(), preview.c_str())) {
        if (ImGui::Selectable("None", value.empty())) {
            AssignAssetScriptProperty(behavior, definition, {}, selectionBefore);
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
                    AssignAssetScriptProperty(behavior, definition, clip, selectionBefore);
                }
            }
        }
        ImGui::EndCombo();
    }
    if (animationModel == nullptr || animationModel->animations.empty()) {
        ImGui::TextDisabled("Assign an animated Model to choose an Animation Clip.");
    } else if (!value.empty() && !animationModel->animations.contains(value)) {
        ImGui::TextDisabled("The selected Animation Clip was not found.");
    }
}

bool EditorScene::AcceptsScriptInputAction(
    const ScriptPropertyDefinition& definition,
    const InputActionBinding& binding) const {
    switch (definition.inputActionKind) {
        case ScriptInputActionKind::Any:
            return true;
        case ScriptInputActionKind::Button:
            return binding.type == InputActionType::Button;
        case ScriptInputActionKind::Axis:
            return binding.type == InputActionType::Axis;
    }
    return false;
}

void EditorScene::DrawInputActionScriptProperty(
    BehaviorComponent& behavior,
    const ScriptPropertyDefinition& definition,
    EntityId selectionBefore) {
    const std::string value = GetAssetScriptPropertyValue(behavior, definition);
    Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    const std::string resolvedName =
        input != nullptr ? input->GetActionName(value) : std::string{};
    const std::string preview =
        value.empty() ? "None" : resolvedName.empty() ? value : resolvedName;

    ImGui::BeginDisabled(input == nullptr);
    if (ImGui::BeginCombo(definition.name.c_str(), preview.c_str())) {
        if (ImGui::Selectable("None", value.empty())) {
            AssignAssetScriptProperty(behavior, definition, {}, selectionBefore);
        }
        for (const std::string& action : input->GetActionNames()) {
            const InputActionBinding* binding = input->GetActionBinding(action);
            if (binding == nullptr || !AcceptsScriptInputAction(definition, *binding)) {
                continue;
            }
            if (ImGui::Selectable(action.c_str(), resolvedName == action)) {
                AssignAssetScriptProperty(
                    behavior, definition, input->GetActionId(action), selectionBefore);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    if (input == nullptr) {
        ImGui::TextDisabled("Input service is unavailable.");
    } else if (!value.empty()) {
        const InputActionBinding* selected = input->GetActionBinding(value);
        if (selected == nullptr) {
            ImGui::TextDisabled("The selected Input Action was not found.");
        } else if (!AcceptsScriptInputAction(definition, *selected)) {
            ImGui::TextDisabled("The selected Input Action has the wrong type.");
        }
    }
}

void EditorScene::DrawSceneScriptProperty(
    BehaviorComponent& behavior,
    const ScriptPropertyDefinition& definition,
    EntityId selectionBefore) {
    const std::string value = GetAssetScriptPropertyValue(behavior, definition);
    const std::string preview = value.empty() ? "None" : value;
    if (ImGui::BeginCombo(definition.name.c_str(), preview.c_str())) {
        if (ImGui::Selectable("None", value.empty())) {
            AssignAssetScriptProperty(behavior, definition, {}, selectionBefore);
        }
        for (const std::filesystem::path& scene : sceneAssets_) {
            const std::string reference = scene.generic_string();
            if (ImGui::Selectable(reference.c_str(), value == reference)) {
                AssignAssetScriptProperty(behavior, definition, reference, selectionBefore);
            }
        }
        ImGui::EndCombo();
    }
    if (!value.empty() &&
        std::ranges::none_of(sceneAssets_, [&value](const std::filesystem::path& scene) {
            return scene.generic_string() == value;
        })) {
        ImGui::TextDisabled("The selected Scene was not found.");
    }
}

bool EditorScene::DrawAssetScriptPropertyInspector(
    WorldEntity* entity,
    BehaviorComponent& behavior,
    const ScriptPropertyDefinition& definition,
    EntityId selectionBefore) {
    switch (definition.type) {
        case ScriptPropertyType::AnimationClip:
            DrawAnimationClipScriptProperty(entity, behavior, definition, selectionBefore);
            return true;
        case ScriptPropertyType::InputAction:
            DrawInputActionScriptProperty(behavior, definition, selectionBefore);
            return true;
        case ScriptPropertyType::Scene:
            DrawSceneScriptProperty(behavior, definition, selectionBefore);
            return true;
        default:
            return false;
    }
}
