#include "EditorScene.h"

#include "imgui.h"
#include "input/Input.h"

#include <Windows.h>

#include <array>
#include <cstring>
#include <ranges>
#include <string_view>
#include <unordered_map>

struct InputActionUsage {
    size_t total = 0u;
    size_t button = 0u;
    size_t axis = 0u;
    size_t any = 0u;
    size_t stable = 0u;
    size_t legacy = 0u;
};

namespace {
struct InputKeyChoice {
    int value = -1;
    const char* label = "";
};

constexpr std::array<InputKeyChoice, 42> kInputKeyChoices = {{
    {-1, "None"},
    {DIK_A, "A"},
    {DIK_B, "B"},
    {DIK_C, "C"},
    {DIK_D, "D"},
    {DIK_E, "E"},
    {DIK_F, "F"},
    {DIK_G, "G"},
    {DIK_H, "H"},
    {DIK_I, "I"},
    {DIK_J, "J"},
    {DIK_K, "K"},
    {DIK_L, "L"},
    {DIK_M, "M"},
    {DIK_N, "N"},
    {DIK_O, "O"},
    {DIK_P, "P"},
    {DIK_Q, "Q"},
    {DIK_R, "R"},
    {DIK_S, "S"},
    {DIK_T, "T"},
    {DIK_U, "U"},
    {DIK_V, "V"},
    {DIK_W, "W"},
    {DIK_X, "X"},
    {DIK_Y, "Y"},
    {DIK_Z, "Z"},
    {DIK_0, "0"},
    {DIK_1, "1"},
    {DIK_2, "2"},
    {DIK_3, "3"},
    {DIK_4, "4"},
    {DIK_5, "5"},
    {DIK_6, "6"},
    {DIK_7, "7"},
    {DIK_8, "8"},
    {DIK_9, "9"},
    {DIK_SPACE, "Space"},
    {DIK_LSHIFT, "Left Shift"},
    {DIK_RSHIFT, "Right Shift"},
    {DIK_LCONTROL, "Left Ctrl"},
    {DIK_RCONTROL, "Right Ctrl"},
}};

struct InputGamepadButtonChoice {
    WORD value = 0;
    const char* label = "";
};

constexpr std::array<InputGamepadButtonChoice, 13> kInputGamepadButtonChoices = {{
    {0, "None"},
    {XINPUT_GAMEPAD_A, "A"},
    {XINPUT_GAMEPAD_B, "B"},
    {XINPUT_GAMEPAD_X, "X"},
    {XINPUT_GAMEPAD_Y, "Y"},
    {XINPUT_GAMEPAD_LEFT_SHOULDER, "Left Shoulder"},
    {XINPUT_GAMEPAD_RIGHT_SHOULDER, "Right Shoulder"},
    {XINPUT_GAMEPAD_LEFT_THUMB, "Left Stick"},
    {XINPUT_GAMEPAD_RIGHT_THUMB, "Right Stick"},
    {XINPUT_GAMEPAD_DPAD_UP, "D-Pad Up"},
    {XINPUT_GAMEPAD_DPAD_DOWN, "D-Pad Down"},
    {XINPUT_GAMEPAD_START, "Start"},
    {XINPUT_GAMEPAD_BACK, "Back"},
}};

struct InputAxisChoice {
    InputActionAxisSource value = InputActionAxisSource::None;
    const char* label = "";
};

constexpr std::array<InputAxisChoice, 7> kInputAxisChoices = {{
    {InputActionAxisSource::None, "None"},
    {InputActionAxisSource::GamepadLeftX, "Left Stick X"},
    {InputActionAxisSource::GamepadLeftY, "Left Stick Y"},
    {InputActionAxisSource::GamepadRightX, "Right Stick X"},
    {InputActionAxisSource::GamepadRightY, "Right Stick Y"},
    {InputActionAxisSource::GamepadLeftTrigger, "Left Trigger"},
    {InputActionAxisSource::GamepadRightTrigger, "Right Trigger"},
}};

using InputActionUsageMap = std::unordered_map<std::string, InputActionUsage>;

void RecordInputActionUsage(InputActionUsageMap& usages, const Input& input,
                            const std::string_view identifier,
                            const ScriptInputActionKind kind) {
    const std::string resolvedName = input.GetActionName(identifier);
    const std::string_view actionName =
        resolvedName.empty() ? identifier : std::string_view(resolvedName);
    if (actionName.empty()) {
        return;
    }
    InputActionUsage& usage = usages[std::string(actionName)];
    ++usage.total;
    const std::string resolvedId = input.GetActionId(identifier);
    resolvedId.empty() || identifier != resolvedId ? ++usage.legacy : ++usage.stable;
    switch (kind) {
        case ScriptInputActionKind::Button:
            ++usage.button;
            break;
        case ScriptInputActionKind::Axis:
            ++usage.axis;
            break;
        case ScriptInputActionKind::Any:
            ++usage.any;
            break;
    }
}

void CollectStoredInputActionUsages(const BehaviorComponent& script, const Input& input,
                                    InputActionUsageMap& usages) {
    for (const ScriptPropertyValue& property : script.properties) {
        if (property.type == ScriptPropertyType::InputAction) {
            RecordInputActionUsage(usages, input, property.stringValue,
                                   ScriptInputActionKind::Any);
        }
    }
}

void CollectDefinedInputActionUsages(
    const BehaviorComponent& script, const std::vector<ScriptPropertyDefinition>& definitions,
    const Input& input, InputActionUsageMap& usages) {
    for (const ScriptPropertyDefinition& definition : definitions) {
        if (definition.type != ScriptPropertyType::InputAction) {
            continue;
        }
        const auto stored =
            std::ranges::find(script.properties, definition.name, &ScriptPropertyValue::name);
        const std::string_view value =
            stored != script.properties.end() &&
                    stored->type == ScriptPropertyType::InputAction
                ? std::string_view(stored->stringValue)
                : std::string_view(definition.defaultString);
        RecordInputActionUsage(usages, input, value, definition.inputActionKind);
    }
}

InputActionUsageMap CollectInputActionUsages(const World& world,
                                             const BehaviorRegistry& registry,
                                             const Input& input) {
    InputActionUsageMap usages;
    for (const WorldEntity& entity : world.Entities()) {
        for (const BehaviorComponent& script : entity.scripts) {
            const std::vector<ScriptPropertyDefinition>* definitions =
                registry.Properties(script.type);
            if (definitions == nullptr) {
                CollectStoredInputActionUsages(script, input, usages);
            } else {
                CollectDefinedInputActionUsages(script, *definitions, input, usages);
            }
        }
    }
    return usages;
}

InputActionUsage FindInputActionUsage(const InputActionUsageMap& usages,
                                      const std::string& name) {
    const auto entry = usages.find(name);
    return entry != usages.end() ? entry->second : InputActionUsage{};
}

bool DrawKeyCombo(const char* label, int& value) {
    const auto selected = std::ranges::find(kInputKeyChoices, value, &InputKeyChoice::value);
    const char* preview = selected != kInputKeyChoices.end() ? selected->label : "Unknown";
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (const InputKeyChoice& choice : kInputKeyChoices) {
            if (ImGui::Selectable(choice.label, value == choice.value)) {
                value = choice.value;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool DrawGamepadButtonCombo(const char* label, WORD& value) {
    const auto selected =
        std::ranges::find(kInputGamepadButtonChoices, value, &InputGamepadButtonChoice::value);
    const char* preview =
        selected != kInputGamepadButtonChoices.end() ? selected->label : "Unknown";
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (const InputGamepadButtonChoice& choice : kInputGamepadButtonChoices) {
            if (ImGui::Selectable(choice.label, value == choice.value)) {
                value = choice.value;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool DrawAxisCombo(const char* label, InputActionAxisSource& value) {
    const auto selected = std::ranges::find(kInputAxisChoices, value, &InputAxisChoice::value);
    const char* preview = selected != kInputAxisChoices.end() ? selected->label : "Unknown";
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (const InputAxisChoice& choice : kInputAxisChoices) {
            if (ImGui::Selectable(choice.label, value == choice.value)) {
                value = choice.value;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool DrawInputActionTypeCombo(InputActionBinding& binding) {
    const char* preview = binding.type == InputActionType::Axis ? "Axis" : "Button";
    bool changed = false;
    if (ImGui::BeginCombo("Type", preview)) {
        if (ImGui::Selectable("Button", binding.type == InputActionType::Button)) {
            binding.type = InputActionType::Button;
            changed = true;
        }
        if (ImGui::Selectable("Axis", binding.type == InputActionType::Axis)) {
            binding.type = InputActionType::Axis;
            changed = true;
        }
        ImGui::EndCombo();
    }
    return changed;
}

void DrawInputActionUsage(const InputActionBinding& binding,
                          const InputActionUsage& usage) {
    if (usage.total == 0u) {
        ImGui::TextDisabled("No Script references.");
    } else {
        ImGui::TextDisabled("%zu Script reference%s (Stable: %zu, Legacy: %zu)", usage.total,
                            usage.total == 1u ? "" : "s", usage.stable, usage.legacy);
        ImGui::TextDisabled("Expected kind: Button %zu, Axis %zu, Any %zu", usage.button,
                            usage.axis, usage.any);
    }
    const size_t incompatible =
        binding.type == InputActionType::Button ? usage.axis : usage.button;
    if (incompatible != 0u) {
        ImGui::TextColored({1.0f, 0.45f, 0.35f, 1.0f},
                           "%zu Script reference%s expect%s the other Action type.",
                           incompatible, incompatible == 1u ? "" : "s",
                           incompatible == 1u ? "s" : "");
    }
}
}  // namespace

bool EditorScene::SaveInputSettings() {
    Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (input == nullptr) {
        status_ = "Error: Could not save Input Settings: Input service is unavailable.";
        return false;
    }
    std::string error;
    if (!inputSettingsStore_.Save(*input, error)) {
        status_ = "Error: Could not save Input Settings: " + error;
        return false;
    }
    inputSettingsDirty_ = false;
    status_ = "Saved Input Settings.";
    return true;
}

size_t EditorScene::UpgradeInputActionReferences() {
    Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (input == nullptr || IsInPlayMode()) {
        return 0u;
    }
    size_t upgraded = 0u;
    for (const WorldEntity& snapshot : world_.Entities()) {
        WorldEntity* entity = world_.Find(snapshot.id);
        if (entity == nullptr) {
            continue;
        }
        for (BehaviorComponent& script : entity->scripts) {
            for (ScriptPropertyValue& property : script.properties) {
                if (property.type != ScriptPropertyType::InputAction) {
                    continue;
                }
                const std::string id = input->GetActionId(property.stringValue);
                if (!id.empty() && property.stringValue != id) {
                    property.stringValue = id;
                    ++upgraded;
                }
            }
        }
    }
    if (upgraded != 0u) {
        RefreshDirty();
    }
    return upgraded;
}

void EditorScene::DrawProjectInputSettings() {
    ImGui::SeparatorText("Input Actions");
    ImGui::TextDisabled("Project file: %s", inputSettingsStore_.Path().generic_string().c_str());
    ImGui::TextWrapped("Named Actions combine keyboard and gamepad bindings used by C++ Scripts.");
    Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    ImGui::BeginDisabled(IsInPlayMode() || input == nullptr);
    if (input == nullptr) {
        ImGui::TextDisabled("Input service is unavailable.");
        ImGui::EndDisabled();
        return;
    }
    DrawInputSettingsToolbar(*input);
    DrawInputActionBindings(*input);
    DrawInputActionDialogs(*input);
    ImGui::EndDisabled();
}

void EditorScene::DrawInputSettingsToolbar(Input& input) {
    ImGui::BeginDisabled(!inputSettingsDirty_);
    if (ImGui::Button("Save##InputSettings")) {
        SaveInputSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert##InputSettings")) {
        RevertInputSettings(input);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset Defaults##InputSettings")) {
        input.ResetDefaultActionBindings();
        inputSettingsDirty_ = true;
        status_ = "Reset Input Actions to defaults.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Action")) {
        OpenCreateInputActionDialog();
    }
    if (inputSettingsDirty_) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.75f, 0.25f, 1.0f}, "Unsaved changes");
    }
}

void EditorScene::RevertInputSettings(Input& input) {
    std::string error;
    if (inputSettingsStore_.Load(input, error)) {
        inputSettingsDirty_ = false;
        status_ = "Reverted Input Settings.";
    } else {
        status_ = "Error: Could not reload Input Settings: " + error;
    }
}

void EditorScene::OpenCreateInputActionDialog() {
    inputActionNameBuffer_.fill('\0');
    newInputActionType_ = InputActionType::Button;
    showCreateInputActionDialog_ = true;
    focusInputActionNameInput_ = true;
}

void EditorScene::DrawInputActionBindings(Input& input) {
    const InputActionUsageMap usages =
        CollectInputActionUsages(world_, behaviorRegistry_, input);
    if (ImGui::BeginChild("InputActionBindings", {0.0f, 300.0f},
                          ImGuiChildFlags_Borders)) {
        for (const std::string& name : input.GetActionNames()) {
            const InputActionBinding* binding = input.GetActionBinding(name);
            if (binding != nullptr) {
                DrawInputActionBinding(input, name, FindInputActionUsage(usages, name));
            }
        }
    }
    ImGui::EndChild();
}

void EditorScene::DrawInputActionBinding(Input& input, const std::string& name,
                                         const InputActionUsage& usage) {
    const InputActionBinding* stored = input.GetActionBinding(name);
    if (stored == nullptr) {
        return;
    }
    InputActionBinding binding = *stored;
    ImGui::PushID(name.c_str());
    ImGui::SeparatorText(name.c_str());
    ImGui::TextDisabled("ID: %s", input.GetActionId(name).c_str());
    bool changed = DrawInputActionTypeCombo(binding);
    DrawInputActionUsage(binding, usage);
    changed |= DrawInputActionBindingFields(binding);
    if (changed && input.SetActionBinding(name, binding)) {
        inputSettingsDirty_ = true;
        status_ = "Modified Input Action: " + name;
    }
    if (ImGui::SmallButton("Rename")) {
        OpenRenameInputActionDialog(name);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) {
        OpenDeleteInputActionDialog(name);
    }
    ImGui::PopID();
}

bool EditorScene::DrawInputActionBindingFields(InputActionBinding& binding) {
    bool changed = false;
    if (binding.type == InputActionType::Axis) {
        changed |= DrawKeyCombo("Negative Key", binding.negativeKey);
        changed |= DrawKeyCombo("Positive Key", binding.positiveKeys[0]);
        changed |= DrawAxisCombo("Gamepad Axis", binding.gamepadAxis);
    } else {
        changed |= DrawKeyCombo("Primary Key", binding.positiveKeys[0]);
        changed |= DrawKeyCombo("Alternate Key", binding.positiveKeys[1]);
        changed |= DrawGamepadButtonCombo("Gamepad Button", binding.gamepadButton);
    }
    return changed;
}

void EditorScene::OpenRenameInputActionDialog(const std::string& name) {
    pendingInputActionName_ = name;
    inputActionNameBuffer_.fill('\0');
    strncpy_s(inputActionNameBuffer_.data(), inputActionNameBuffer_.size(), name.c_str(),
              _TRUNCATE);
    showRenameInputActionDialog_ = true;
    focusInputActionNameInput_ = true;
}

void EditorScene::OpenDeleteInputActionDialog(const std::string& name) {
    pendingInputActionName_ = name;
    showDeleteInputActionDialog_ = true;
}

void EditorScene::DrawInputActionDialogs(Input& input) {
    DrawCreateInputActionDialog(input);
    DrawRenameInputActionDialog(input);
    DrawDeleteInputActionDialog(input);
}

void EditorScene::DrawCreateInputActionDialog(Input& input) {
    if (showCreateInputActionDialog_) {
        ImGui::OpenPopup("Create Input Action");
        showCreateInputActionDialog_ = false;
    }
    if (!ImGui::BeginPopupModal("Create Input Action", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (focusInputActionNameInput_) {
        ImGui::SetKeyboardFocusHere();
        focusInputActionNameInput_ = false;
    }
    ImGui::SetNextItemWidth(300.0f);
    const bool submitted =
        ImGui::InputText("Name", inputActionNameBuffer_.data(), inputActionNameBuffer_.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth(300.0f);
    InputActionBinding newBinding{};
    newBinding.type = newInputActionType_;
    if (DrawInputActionTypeCombo(newBinding)) {
        newInputActionType_ = newBinding.type;
    }
    ImGui::TextDisabled("Names must be unique and at most 64 characters.");
    if (submitted || ImGui::Button("Create", {100.0f, 0.0f})) {
        const std::string name(inputActionNameBuffer_.data());
        if (input.GetActionBinding(name) != nullptr) {
            status_ = "Error: Input Action already exists: " + name;
        } else if (input.SetActionBinding(name, newBinding)) {
            inputSettingsDirty_ = true;
            status_ = "Created Input Action: " + name;
            ImGui::CloseCurrentPopup();
        } else {
            status_ = "Error: Invalid Input Action name.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {100.0f, 0.0f})) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorScene::DrawRenameInputActionDialog(Input& input) {
    if (showRenameInputActionDialog_) {
        ImGui::OpenPopup("Rename Input Action");
        showRenameInputActionDialog_ = false;
    }
    if (!ImGui::BeginPopupModal("Rename Input Action", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const InputActionUsage usage =
        FindInputActionUsage(CollectInputActionUsages(world_, behaviorRegistry_, input),
                             pendingInputActionName_);
    ImGui::TextDisabled("Current name: %s", pendingInputActionName_.c_str());
    if (usage.legacy != 0u) {
        ImGui::TextColored({1.0f, 0.75f, 0.25f, 1.0f},
                           "%zu Script reference%s will keep the old name.", usage.legacy,
                           usage.legacy == 1u ? "" : "s");
        ImGui::TextWrapped("Update those Script properties before or after renaming.");
    }
    if (usage.stable != 0u) {
        ImGui::TextDisabled("%zu stable reference%s will remain connected.", usage.stable,
                            usage.stable == 1u ? "" : "s");
    }
    if (focusInputActionNameInput_) {
        ImGui::SetKeyboardFocusHere();
        focusInputActionNameInput_ = false;
    }
    ImGui::SetNextItemWidth(300.0f);
    const bool submitted =
        ImGui::InputText("New Name", inputActionNameBuffer_.data(),
                         inputActionNameBuffer_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    if (submitted || ImGui::Button("Rename", {100.0f, 0.0f})) {
        const std::string newName(inputActionNameBuffer_.data());
        if (input.RenameActionBinding(pendingInputActionName_, newName)) {
            status_ = "Renamed Input Action: " + pendingInputActionName_ + " -> " + newName;
            pendingInputActionName_.clear();
            inputSettingsDirty_ = true;
            ImGui::CloseCurrentPopup();
        } else {
            status_ = "Error: Input Action name is invalid or already exists.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {100.0f, 0.0f})) {
        pendingInputActionName_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorScene::DrawDeleteInputActionDialog(Input& input) {
    if (showDeleteInputActionDialog_) {
        ImGui::OpenPopup("Remove Input Action");
        showDeleteInputActionDialog_ = false;
    }
    if (!ImGui::BeginPopupModal("Remove Input Action", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const InputActionUsage usage =
        FindInputActionUsage(CollectInputActionUsages(world_, behaviorRegistry_, input),
                             pendingInputActionName_);
    ImGui::Text("Remove '%s'?", pendingInputActionName_.c_str());
    if (usage.total != 0u) {
        ImGui::TextColored({1.0f, 0.45f, 0.35f, 1.0f},
                           "%zu Script reference%s will become missing.", usage.total,
                           usage.total == 1u ? "" : "s");
    }
    ImGui::TextDisabled("The change is not permanent until Save is pressed.");
    if (ImGui::Button("Remove", {100.0f, 0.0f})) {
        const std::string removedName = pendingInputActionName_;
        if (input.RemoveActionBinding(removedName)) {
            inputSettingsDirty_ = true;
            status_ = "Removed Input Action: " + removedName;
        }
        pendingInputActionName_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {100.0f, 0.0f})) {
        pendingInputActionName_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
