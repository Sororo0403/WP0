#include "InputSettingsStore.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <unordered_set>

namespace {
constexpr uintmax_t kMaxSettingsBytes = 1024u * 1024u;
constexpr size_t kMaxActions = 128u;

const std::array<std::pair<InputActionAxisSource, const char*>, 7> kAxisNames = {{
    {InputActionAxisSource::None, "None"},
    {InputActionAxisSource::GamepadLeftX, "GamepadLeftX"},
    {InputActionAxisSource::GamepadLeftY, "GamepadLeftY"},
    {InputActionAxisSource::GamepadRightX, "GamepadRightX"},
    {InputActionAxisSource::GamepadRightY, "GamepadRightY"},
    {InputActionAxisSource::GamepadLeftTrigger, "GamepadLeftTrigger"},
    {InputActionAxisSource::GamepadRightTrigger, "GamepadRightTrigger"},
}};

const char* AxisName(InputActionAxisSource source) {
    const auto found = std::ranges::find_if(
        kAxisNames, [source](const auto& entry) { return entry.first == source; });
    return found != kAxisNames.end() ? found->second : "None";
}

bool ParseAxis(std::string_view name, InputActionAxisSource& source) {
    const auto found =
        std::ranges::find(kAxisNames, name, [](const auto& entry) {
            return std::string_view(entry.second);
        });
    if (found == kAxisNames.end()) {
        return false;
    }
    source = found->first;
    return true;
}

const char* ActionTypeName(InputActionType type) {
    return type == InputActionType::Axis ? "Axis" : "Button";
}

bool ParseActionType(std::string_view name, InputActionType& type) {
    if (name == "Button") {
        type = InputActionType::Button;
        return true;
    }
    if (name == "Axis") {
        type = InputActionType::Axis;
        return true;
    }
    return false;
}
} // namespace

InputSettingsStore::InputSettingsStore(std::filesystem::path path)
    : path_(std::move(path)) {}

bool InputSettingsStore::Load(Input& input, std::string& error) const {
    error.clear();
    std::error_code filesystemError;
    if (!std::filesystem::exists(path_, filesystemError) && !filesystemError) {
        input.ResetDefaultActionBindings();
        return true;
    }
    if (filesystemError || !std::filesystem::is_regular_file(path_, filesystemError) ||
        filesystemError ||
        std::filesystem::file_size(path_, filesystemError) > kMaxSettingsBytes ||
        filesystemError) {
        error = "Input settings file is missing, invalid, or too large.";
        return false;
    }
    try {
        std::ifstream stream(path_);
        nlohmann::json json;
        stream >> json;
        if (!stream || !json.is_object() || json.value("version", 0u) != 1u ||
            !json.contains("actions") || !json["actions"].is_array() ||
            json["actions"].size() > kMaxActions) {
            error = "Input settings JSON structure is invalid.";
            return false;
        }
        Input staged;
        staged.ClearActionBindings();
        std::unordered_set<std::string> actionNames;
        for (const nlohmann::json& action : json["actions"]) {
            if (!action.is_object() || !action.contains("name") ||
                !action["name"].is_string() || !action.contains("negativeKey") ||
                !action["negativeKey"].is_number_integer() ||
                !action.contains("positiveKeys") ||
                !action["positiveKeys"].is_array() ||
                action["positiveKeys"].size() != 2u ||
                !action["positiveKeys"][0].is_number_integer() ||
                !action["positiveKeys"][1].is_number_integer() ||
                !action.contains("gamepadButton") ||
                !action["gamepadButton"].is_number_unsigned() ||
                !action.contains("gamepadAxis") ||
                !action["gamepadAxis"].is_string() ||
                !action.contains("type") || !action["type"].is_string()) {
                error = "Input Action data is invalid.";
                return false;
            }
            const std::string name = action["name"].get<std::string>();
            const int64_t negativeKey = action["negativeKey"].get<int64_t>();
            const int64_t positiveKey0 = action["positiveKeys"][0].get<int64_t>();
            const int64_t positiveKey1 = action["positiveKeys"][1].get<int64_t>();
            const uint64_t gamepadButton = action["gamepadButton"].get<uint64_t>();
            InputActionBinding binding{};
            if (negativeKey < -1 || negativeKey > 255 ||
                positiveKey0 < -1 || positiveKey0 > 255 ||
                positiveKey1 < -1 || positiveKey1 > 255 ||
                gamepadButton > 0xffffu ||
                !ParseAxis(action["gamepadAxis"].get<std::string>(),
                           binding.gamepadAxis) ||
                !ParseActionType(action["type"].get<std::string>(),
                                 binding.type) ||
                !actionNames.insert(name).second) {
                error = "Input Action value is invalid or duplicated.";
                return false;
            }
            binding.negativeKey = static_cast<int>(negativeKey);
            binding.positiveKeys = {static_cast<int>(positiveKey0),
                                    static_cast<int>(positiveKey1)};
            binding.gamepadButton = static_cast<WORD>(gamepadButton);
            if (!staged.SetActionBinding(name, binding)) {
                error = "Input Action binding is invalid.";
                return false;
            }
        }
        input.ClearActionBindings();
        for (const std::string& name : staged.GetActionNames()) {
            const InputActionBinding* binding = staged.GetActionBinding(name);
            if (binding == nullptr || !input.SetActionBinding(name, *binding)) {
                input.ResetDefaultActionBindings();
                error = "Could not apply Input Action bindings.";
                return false;
            }
        }
        return true;
    } catch (const std::exception&) {
        error = "Input settings file is not valid JSON.";
        return false;
    }
}

bool InputSettingsStore::Save(const Input& input, std::string& error) const {
    nlohmann::json actions = nlohmann::json::array();
    const std::vector<std::string> actionNames = input.GetActionNames();
    if (actionNames.size() > kMaxActions) {
        error = "Input settings contain too many Actions.";
        return false;
    }
    for (const std::string& name : actionNames) {
        const InputActionBinding* binding = input.GetActionBinding(name);
        if (binding == nullptr) {
            error = "Input Action disappeared while saving.";
            return false;
        }
        actions.push_back({
            {"name", name},
            {"negativeKey", binding->negativeKey},
            {"positiveKeys", binding->positiveKeys},
            {"gamepadButton", binding->gamepadButton},
            {"gamepadAxis", AxisName(binding->gamepadAxis)},
            {"type", ActionTypeName(binding->type)},
        });
    }
    nlohmann::json json = {
        {"version", 1u},
        {"actions", std::move(actions)},
    };
    std::error_code filesystemError;
    std::filesystem::create_directories(path_.parent_path(), filesystemError);
    if (filesystemError) {
        error = "Could not create the Input settings directory.";
        return false;
    }
    const std::filesystem::path temporary = path_.wstring() + L".tmp";
    try {
        std::ofstream stream(temporary, std::ios::trunc);
        stream << json.dump(2) << '\n';
        stream.close();
        if (!stream) {
            error = "Could not write the temporary Input settings file.";
            return false;
        }
    } catch (const std::exception&) {
        error = "Could not write the Input settings file.";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, filesystemError);
        error = "Could not replace the Input settings file.";
        return false;
    }
    error.clear();
    return true;
}

const std::filesystem::path& InputSettingsStore::Path() const {
    return path_;
}
