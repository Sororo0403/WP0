#include "InputSettingsStore.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <unordered_set>

namespace {
constexpr uintmax_t kMaxSettingsBytes = 1024u * 1024u;
constexpr size_t kMaxActions = 128u;

std::string LegacyActionId(std::string_view name) {
    if (name == "MoveHorizontal") {
        return "00000000-0000-4000-8000-000000000001";
    }
    if (name == "MoveVertical") {
        return "00000000-0000-4000-8000-000000000002";
    }
    if (name == "Sprint") {
        return "00000000-0000-4000-8000-000000000003";
    }
    if (name == "Jump") {
        return "00000000-0000-4000-8000-000000000004";
    }
    uint64_t first = 14695981039346656037ull;
    uint64_t second = 1099511628211ull;
    for (const unsigned char character : name) {
        first = (first ^ character) * 1099511628211ull;
        second = (second ^ character) * 14695981039346656037ull;
    }
    std::array<char, 37> text{};
    sprintf_s(text.data(), text.size(), "%08x-%04x-%04x-%04x-%012llx",
              static_cast<unsigned int>(first >> 32u),
              static_cast<unsigned int>((first >> 16u) & 0xffffu),
              static_cast<unsigned int>(first & 0xffffu),
              static_cast<unsigned int>(second >> 48u),
              static_cast<unsigned long long>(second & 0xffffffffffffull));
    return text.data();
}

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
        const uint32_t version = json.value("version", 0u);
        if (!stream || !json.is_object() || (version != 1u && version != 2u) ||
            !json.contains("actions") || !json["actions"].is_array() ||
            json["actions"].size() > kMaxActions) {
            error = "Input settings JSON structure is invalid.";
            return false;
        }
        Input staged;
        staged.ClearActionBindings();
        std::unordered_set<std::string> actionNames;
        std::unordered_set<std::string> actionIds;
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
                !action.contains("type") || !action["type"].is_string() ||
                (version == 2u &&
                 (!action.contains("id") || !action["id"].is_string()))) {
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
            const std::string id =
                version == 2u ? action["id"].get<std::string>()
                              : LegacyActionId(name);
            const bool added =
                actionIds.insert(id).second &&
                staged.SetActionBinding(name, binding, id);
            if (!added) {
                error = "Input Action binding is invalid.";
                return false;
            }
        }
        input.ClearActionBindings();
        for (const std::string& name : staged.GetActionNames()) {
            const InputActionBinding* binding = staged.GetActionBinding(name);
            const std::string id = staged.GetActionId(name);
            if (binding == nullptr ||
                !input.SetActionBinding(name, *binding, id)) {
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
            {"id", input.GetActionId(name)},
            {"name", name},
            {"negativeKey", binding->negativeKey},
            {"positiveKeys", binding->positiveKeys},
            {"gamepadButton", binding->gamepadButton},
            {"gamepadAxis", AxisName(binding->gamepadAxis)},
            {"type", ActionTypeName(binding->type)},
        });
    }
    nlohmann::json json = {
        {"version", 2u},
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
