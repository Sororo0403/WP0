#include "EditorScene.h"

#include "imgui.h"
#include "world/WorldSerializer.h"

#include <algorithm>
#include <iterator>
#include <ranges>

bool EditorScene::DrawScalarScriptPropertyInspector(
    BehaviorComponent& behavior, const ScriptPropertyDefinition& definition,
    const EntityId selectionBefore) {
    switch (definition.type) {
    case ScriptPropertyType::Float:
        return DrawFloatScriptProperty(behavior, definition);
    case ScriptPropertyType::Boolean:
        return DrawBooleanScriptProperty(behavior, definition, selectionBefore);
    case ScriptPropertyType::Integer:
        return DrawIntegerScriptProperty(behavior, definition);
    case ScriptPropertyType::Vector3:
        return DrawVector3ScriptProperty(behavior, definition);
    default:
        return false;
    }
}

bool EditorScene::DrawFloatScriptProperty(
    BehaviorComponent& behavior, const ScriptPropertyDefinition& definition) {
    const ScriptPropertyValue* stored = FindScriptPropertyValue(behavior, definition);
    float value = stored != nullptr && stored->type == definition.type
                      ? stored->floatValue
                      : definition.defaultFloat;
    const float speed =
        (std::max)(0.001f, (definition.maximumFloat - definition.minimumFloat) * 0.005f);
    if (ImGui::DragFloat(definition.name.c_str(), &value, speed, definition.minimumFloat,
                         definition.maximumFloat, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
        GetOrCreateScriptPropertyValue(behavior, definition).floatValue = value;
        MarkScriptPropertyModified();
    }
    UpdateScriptPropertyEditHistory();
    return true;
}

bool EditorScene::DrawBooleanScriptProperty(
    BehaviorComponent& behavior, const ScriptPropertyDefinition& definition,
    const EntityId selectionBefore) {
    const ScriptPropertyValue* stored = FindScriptPropertyValue(behavior, definition);
    bool value = stored != nullptr && stored->type == definition.type
                     ? stored->booleanValue
                     : definition.defaultBoolean;
    if (!ImGui::Checkbox(definition.name.c_str(), &value)) {
        return true;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    ScriptPropertyValue& property = GetOrCreateScriptPropertyValue(behavior, definition);
    property = {};
    property.name = definition.name;
    property.type = definition.type;
    property.booleanValue = value;
    RecordImmediateEdit("Modify Script Property", before, selectionBefore);
    status_ = "Modified Script property.";
    return true;
}

bool EditorScene::DrawIntegerScriptProperty(
    BehaviorComponent& behavior, const ScriptPropertyDefinition& definition) {
    const ScriptPropertyValue* stored = FindScriptPropertyValue(behavior, definition);
    int value = stored != nullptr && stored->type == definition.type
                    ? stored->integerValue
                    : definition.defaultInteger;
    if (ImGui::DragInt(definition.name.c_str(), &value, 1.0f, definition.minimumInteger,
                       definition.maximumInteger, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        GetOrCreateScriptPropertyValue(behavior, definition).integerValue = value;
        MarkScriptPropertyModified();
    }
    UpdateScriptPropertyEditHistory();
    return true;
}

bool EditorScene::DrawVector3ScriptProperty(
    BehaviorComponent& behavior, const ScriptPropertyDefinition& definition) {
    const ScriptPropertyValue* stored = FindScriptPropertyValue(behavior, definition);
    const ScriptVector3 value = stored != nullptr && stored->type == definition.type
                                    ? stored->vector3Value
                                    : definition.defaultVector3;
    float components[3]{value.x, value.y, value.z};
    if (ImGui::DragFloat3(definition.name.c_str(), components, 0.1f, 0.0f, 0.0f, "%.3f")) {
        GetOrCreateScriptPropertyValue(behavior, definition).vector3Value = {
            components[0],
            components[1],
            components[2],
        };
        MarkScriptPropertyModified();
    }
    UpdateScriptPropertyEditHistory();
    return true;
}

const ScriptPropertyValue* EditorScene::FindScriptPropertyValue(
    const BehaviorComponent& behavior, const ScriptPropertyDefinition& definition) {
    const auto stored =
        std::ranges::find(behavior.properties, definition.name, &ScriptPropertyValue::name);
    return stored != behavior.properties.end() ? &*stored : nullptr;
}

ScriptPropertyValue& EditorScene::GetOrCreateScriptPropertyValue(
    BehaviorComponent& behavior, const ScriptPropertyDefinition& definition) {
    auto stored =
        std::ranges::find(behavior.properties, definition.name, &ScriptPropertyValue::name);
    bool created = false;
    if (stored == behavior.properties.end()) {
        behavior.properties.push_back({});
        stored = std::prev(behavior.properties.end());
        created = true;
    }
    if (created || stored->type != definition.type) {
        *stored = {};
        stored->name = definition.name;
        stored->type = definition.type;
    }
    return *stored;
}

void EditorScene::MarkScriptPropertyModified() {
    RefreshDirty();
    status_ = "Modified Script property.";
}

void EditorScene::UpdateScriptPropertyEditHistory() {
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify Script Property");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
}
