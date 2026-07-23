#include "PlayerSettingsStore.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <climits>
#include <fstream>

namespace {
constexpr uintmax_t kMaxSettingsBytes = 64u * 1024u;
constexpr int kMinWidth = 320;
constexpr int kMinHeight = 180;
constexpr int kMaxDimension = 16384;

bool Validate(const PlayerSettings& settings, std::string& error) {
    if (settings.width < kMinWidth || settings.width > kMaxDimension ||
        settings.height < kMinHeight || settings.height > kMaxDimension) {
        error = "Player resolution is outside the supported range.";
        return false;
    }
    error.clear();
    return true;
}
} // namespace

PlayerSettingsStore::PlayerSettingsStore(std::filesystem::path path)
    : path_(std::move(path)) {}

bool PlayerSettingsStore::Load(PlayerSettings& settings, std::string& error) const {
    settings = {};
    error.clear();
    std::error_code filesystemError;
    if (!std::filesystem::exists(path_, filesystemError) && !filesystemError) {
        return true;
    }
    if (filesystemError ||
        !std::filesystem::is_regular_file(path_, filesystemError) ||
        filesystemError ||
        std::filesystem::file_size(path_, filesystemError) > kMaxSettingsBytes ||
        filesystemError) {
        error = "Player settings file is missing, invalid, or too large.";
        return false;
    }
    try {
        std::ifstream stream(path_);
        nlohmann::json json;
        stream >> json;
        if (!stream || !json.is_object() || json.value("version", 0u) != 1u ||
            !json.contains("width") || !json["width"].is_number_integer() ||
            !json.contains("height") || !json["height"].is_number_integer() ||
            !json.contains("fullscreen") || !json["fullscreen"].is_boolean()) {
            error = "Player settings JSON structure is invalid.";
            return false;
        }
        const int64_t width = json["width"].get<int64_t>();
        const int64_t height = json["height"].get<int64_t>();
        if (width < INT_MIN || width > INT_MAX || height < INT_MIN ||
            height > INT_MAX) {
            error = "Player resolution is out of range.";
            return false;
        }
        PlayerSettings loaded{
            .width = static_cast<int>(width),
            .height = static_cast<int>(height),
            .fullscreen = json["fullscreen"].get<bool>(),
        };
        if (!Validate(loaded, error)) {
            return false;
        }
        settings = loaded;
        return true;
    } catch (const std::exception&) {
        error = "Player settings file is not valid JSON.";
        return false;
    }
}

bool PlayerSettingsStore::Save(const PlayerSettings& settings,
                               std::string& error) const {
    if (!Validate(settings, error)) {
        return false;
    }
    const nlohmann::json json = {
        {"version", 1u},
        {"width", settings.width},
        {"height", settings.height},
        {"fullscreen", settings.fullscreen},
    };
    std::error_code filesystemError;
    std::filesystem::create_directories(path_.parent_path(), filesystemError);
    if (filesystemError) {
        error = "Could not create the Player settings directory.";
        return false;
    }
    const std::filesystem::path temporary = path_.wstring() + L".tmp";
    try {
        std::ofstream stream(temporary, std::ios::trunc);
        stream << json.dump(2) << '\n';
        stream.close();
        if (!stream) {
            error = "Could not write the temporary Player settings file.";
            return false;
        }
    } catch (const std::exception&) {
        error = "Could not write the Player settings file.";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, filesystemError);
        error = "Could not replace the Player settings file.";
        return false;
    }
    error.clear();
    return true;
}

const std::filesystem::path& PlayerSettingsStore::Path() const {
    return path_;
}
