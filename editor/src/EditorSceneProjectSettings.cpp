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

namespace {
struct InputKeyChoice {
    int value;
    const char* label;
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
    InputActionAxisSource value;
    const char* label;
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

struct InputActionUsage {
    size_t total = 0u;
    size_t button = 0u;
    size_t axis = 0u;
    size_t any = 0u;
    size_t stable = 0u;
    size_t legacy = 0u;
};

std::unordered_map<std::string, InputActionUsage> CollectInputActionUsages(
    const World& world, const BehaviorRegistry& registry, const Input& input) {
    std::unordered_map<std::string, InputActionUsage> usages;
    const auto countReference = [&usages, &input](std::string_view actionIdentifier,
                                                  ScriptInputActionKind kind) {
        const std::string resolvedName = input.GetActionName(actionIdentifier);
        const std::string_view actionName =
            resolvedName.empty() ? actionIdentifier : std::string_view(resolvedName);
        if (actionName.empty()) {
            return;
        }
        InputActionUsage& usage = usages[std::string(actionName)];
        ++usage.total;
        const std::string resolvedId = input.GetActionId(actionIdentifier);
        if (!resolvedId.empty() && actionIdentifier == resolvedId) {
            ++usage.stable;
        } else {
            ++usage.legacy;
        }
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
    };
    for (const WorldEntity& entity : world.Entities()) {
        for (const BehaviorComponent& script : entity.scripts) {
            const std::vector<ScriptPropertyDefinition>* definitions =
                registry.Properties(script.type);
            if (definitions == nullptr) {
                for (const ScriptPropertyValue& property : script.properties) {
                    if (property.type == ScriptPropertyType::InputAction) {
                        countReference(property.stringValue, ScriptInputActionKind::Any);
                    }
                }
                continue;
            }
            for (const ScriptPropertyDefinition& definition : *definitions) {
                if (definition.type != ScriptPropertyType::InputAction) {
                    continue;
                }
                const auto stored = std::ranges::find(script.properties, definition.name,
                                                      &ScriptPropertyValue::name);
                const std::string_view value =
                    stored != script.properties.end() &&
                            stored->type == ScriptPropertyType::InputAction
                        ? std::string_view(stored->stringValue)
                        : std::string_view(definition.defaultString);
                countReference(value, definition.inputActionKind);
            }
        }
    }
    return usages;
}

} // namespace

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
    ImGui::BeginDisabled(!inputSettingsDirty_);
    if (ImGui::Button("Save##InputSettings")) {
        SaveInputSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert##InputSettings")) {
        std::string error;
        if (inputSettingsStore_.Load(*input, error)) {
            inputSettingsDirty_ = false;
            status_ = "Reverted Input Settings.";
        } else {
            status_ = "Error: Could not reload Input Settings: " + error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset Defaults##InputSettings")) {
        input->ResetDefaultActionBindings();
        inputSettingsDirty_ = true;
        status_ = "Reset Input Actions to defaults.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Action")) {
        inputActionNameBuffer_.fill('\0');
        newInputActionType_ = InputActionType::Button;
        showCreateInputActionDialog_ = true;
        focusInputActionNameInput_ = true;
    }
    if (inputSettingsDirty_) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.75f, 0.25f, 1.0f}, "Unsaved changes");
    }

    const auto drawKeyCombo = [](const char* label, int& value) {
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
    };
    const auto drawGamepadButtonCombo = [](const char* label, WORD& value) {
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
    };
    const auto drawAxisCombo = [](const char* label, InputActionAxisSource& value) {
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
    };

    const std::unordered_map<std::string, InputActionUsage> inputActionUsages =
        CollectInputActionUsages(world_, behaviorRegistry_, *input);
    if (ImGui::BeginChild("InputActionBindings", {0.0f, 300.0f}, ImGuiChildFlags_Borders)) {
        for (const std::string& name : input->GetActionNames()) {
            const InputActionBinding* stored = input->GetActionBinding(name);
            if (stored == nullptr) {
                continue;
            }
            InputActionBinding binding = *stored;
            ImGui::PushID(name.c_str());
            ImGui::SeparatorText(name.c_str());
            ImGui::TextDisabled("ID: %s", input->GetActionId(name).c_str());
            bool changed = false;
            const char* typePreview = binding.type == InputActionType::Axis ? "Axis" : "Button";
            if (ImGui::BeginCombo("Type", typePreview)) {
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
            const auto usageEntry = inputActionUsages.find(name);
            const InputActionUsage usage =
                usageEntry != inputActionUsages.end() ? usageEntry->second : InputActionUsage{};
            if (usage.total == 0u) {
                ImGui::TextDisabled("No Script references.");
            } else {
                ImGui::TextDisabled("%zu Script reference%s (Stable: %zu, Legacy: %zu)",
                                    usage.total, usage.total == 1u ? "" : "s", usage.stable,
                                    usage.legacy);
                ImGui::TextDisabled("Expected kind: Button %zu, Axis %zu, Any %zu", usage.button,
                                    usage.axis, usage.any);
            }
            const size_t incompatibleReferences =
                binding.type == InputActionType::Button ? usage.axis : usage.button;
            if (incompatibleReferences != 0u) {
                ImGui::TextColored({1.0f, 0.45f, 0.35f, 1.0f},
                                   "%zu Script reference%s expect%s the other Action type.",
                                   incompatibleReferences, incompatibleReferences == 1u ? "" : "s",
                                   incompatibleReferences == 1u ? "s" : "");
            }
            const bool axisAction = binding.type == InputActionType::Axis;
            if (axisAction) {
                changed |= drawKeyCombo("Negative Key", binding.negativeKey);
                changed |= drawKeyCombo("Positive Key", binding.positiveKeys[0]);
                changed |= drawAxisCombo("Gamepad Axis", binding.gamepadAxis);
            } else {
                changed |= drawKeyCombo("Primary Key", binding.positiveKeys[0]);
                changed |= drawKeyCombo("Alternate Key", binding.positiveKeys[1]);
                changed |= drawGamepadButtonCombo("Gamepad Button", binding.gamepadButton);
            }
            if (changed && input->SetActionBinding(name, binding)) {
                inputSettingsDirty_ = true;
                status_ = "Modified Input Action: " + name;
            }
            if (ImGui::SmallButton("Rename")) {
                pendingInputActionName_ = name;
                inputActionNameBuffer_.fill('\0');
                strncpy_s(inputActionNameBuffer_.data(), inputActionNameBuffer_.size(),
                          name.c_str(), _TRUNCATE);
                showRenameInputActionDialog_ = true;
                focusInputActionNameInput_ = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                pendingInputActionName_ = name;
                showDeleteInputActionDialog_ = true;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    DrawInputActionDialogs(input);
    ImGui::EndDisabled();
    ImGui::End();
}

void EditorScene::DrawInputActionDialogs(Input* input) {
    const std::unordered_map<std::string, InputActionUsage> inputActionUsages =
        CollectInputActionUsages(world_, behaviorRegistry_, *input);
    if (showCreateInputActionDialog_) {
        ImGui::OpenPopup("Create Input Action");
        showCreateInputActionDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Create Input Action", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (focusInputActionNameInput_) {
            ImGui::SetKeyboardFocusHere();
            focusInputActionNameInput_ = false;
        }
        ImGui::SetNextItemWidth(300.0f);
        const bool submitted =
            ImGui::InputText("Name", inputActionNameBuffer_.data(), inputActionNameBuffer_.size(),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        const char* newTypePreview =
            newInputActionType_ == InputActionType::Axis ? "Axis" : "Button";
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("Type", newTypePreview)) {
            if (ImGui::Selectable("Button", newInputActionType_ == InputActionType::Button)) {
                newInputActionType_ = InputActionType::Button;
            }
            if (ImGui::Selectable("Axis", newInputActionType_ == InputActionType::Axis)) {
                newInputActionType_ = InputActionType::Axis;
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Names must be unique and at most 64 characters.");
        if (submitted || ImGui::Button("Create", {100.0f, 0.0f})) {
            InputActionBinding binding{};
            binding.type = newInputActionType_;
            const std::string name(inputActionNameBuffer_.data());
            if (input->GetActionBinding(name) != nullptr) {
                status_ = "Error: Input Action already exists: " + name;
            } else if (input->SetActionBinding(name, binding)) {
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

    if (showRenameInputActionDialog_) {
        ImGui::OpenPopup("Rename Input Action");
        showRenameInputActionDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Rename Input Action", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Current name: %s", pendingInputActionName_.c_str());
        const auto usageEntry = inputActionUsages.find(pendingInputActionName_);
        const InputActionUsage usage =
            usageEntry != inputActionUsages.end() ? usageEntry->second : InputActionUsage{};
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
            if (input->RenameActionBinding(pendingInputActionName_, newName)) {
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

    if (showDeleteInputActionDialog_) {
        ImGui::OpenPopup("Remove Input Action");
        showDeleteInputActionDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Remove Input Action", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove '%s'?", pendingInputActionName_.c_str());
        const auto usageEntry = inputActionUsages.find(pendingInputActionName_);
        const InputActionUsage usage =
            usageEntry != inputActionUsages.end() ? usageEntry->second : InputActionUsage{};
        if (usage.total != 0u) {
            ImGui::TextColored({1.0f, 0.45f, 0.35f, 1.0f},
                               "%zu Script reference%s will become missing.", usage.total,
                               usage.total == 1u ? "" : "s");
        }
        ImGui::TextDisabled("The change is not permanent until Save is pressed.");
        if (ImGui::Button("Remove", {100.0f, 0.0f})) {
            const std::string removedName = pendingInputActionName_;
            if (input->RemoveActionBinding(removedName)) {
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
}
