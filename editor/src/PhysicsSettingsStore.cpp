#include "PhysicsSettingsStore.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_set>

namespace {
constexpr uintmax_t kMaxSettingsBytes = 1024u * 1024u;
constexpr size_t kMaxLayerNameLength = 64u;

std::string LowercaseAscii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool Validate(const PhysicsSettings& settings, std::string& error) {
    if (settings.layerNames[0] != "Default") {
        error = "Physics Layer 0 must be named Default.";
        return false;
    }
    std::unordered_set<std::string> names;
    for (const std::string& name : settings.layerNames) {
        if (name.size() > kMaxLayerNameLength || name.find('\0') != std::string::npos) {
            error = "Physics Layer name is invalid or too long.";
            return false;
        }
        if (!name.empty() && !names.insert(LowercaseAscii(name)).second) {
            error = "Physics Layer names must be unique.";
            return false;
        }
    }
    for (size_t first = 0u; first < PhysicsSettings::kLayerCount; ++first) {
        for (size_t second = 0u; second < PhysicsSettings::kLayerCount; ++second) {
            const bool forward =
                (settings.collisionMasks[first] & (uint32_t{1u} << second)) != 0u;
            const bool reverse =
                (settings.collisionMasks[second] & (uint32_t{1u} << first)) != 0u;
            if (forward != reverse) {
                error = "Physics collision matrix must be symmetric.";
                return false;
            }
        }
    }
    error.clear();
    return true;
}
} // namespace

PhysicsSettingsStore::PhysicsSettingsStore(std::filesystem::path path)
    : path_(std::move(path)) {}

bool PhysicsSettingsStore::Load(PhysicsSettings& settings, std::string& error) const {
    settings = PhysicsSettings::Defaults();
    error.clear();
    std::error_code filesystemError;
    if (!std::filesystem::exists(path_, filesystemError) && !filesystemError) {
        return true;
    }
    if (filesystemError || !std::filesystem::is_regular_file(path_, filesystemError) ||
        filesystemError || std::filesystem::file_size(path_, filesystemError) >
                               kMaxSettingsBytes ||
        filesystemError) {
        error = "Physics settings file is missing, invalid, or too large.";
        return false;
    }
    try {
        std::ifstream stream(path_);
        nlohmann::json json;
        stream >> json;
        if (!stream || !json.is_object() || json.value("version", 0u) != 1u ||
            !json.contains("layers") || !json["layers"].is_array() ||
            json["layers"].size() != PhysicsSettings::kLayerCount ||
            !json.contains("collisionMasks") || !json["collisionMasks"].is_array() ||
            json["collisionMasks"].size() != PhysicsSettings::kLayerCount) {
            error = "Physics settings JSON structure is invalid.";
            return false;
        }
        PhysicsSettings loaded{};
        for (size_t index = 0u; index < PhysicsSettings::kLayerCount; ++index) {
            if (!json["layers"][index].is_string() ||
                !json["collisionMasks"][index].is_number_unsigned()) {
                error = "Physics settings Layer data is invalid.";
                return false;
            }
            loaded.layerNames[index] = json["layers"][index].get<std::string>();
            const uint64_t mask = json["collisionMasks"][index].get<uint64_t>();
            if (mask > 0xffffffffull) {
                error = "Physics collision mask is out of range.";
                return false;
            }
            loaded.collisionMasks[index] = static_cast<uint32_t>(mask);
        }
        if (!Validate(loaded, error)) {
            return false;
        }
        settings = std::move(loaded);
        return true;
    } catch (const std::exception&) {
        error = "Physics settings file is not valid JSON.";
        return false;
    }
}

bool PhysicsSettingsStore::Save(const PhysicsSettings& settings, std::string& error) const {
    if (!Validate(settings, error)) {
        return false;
    }
    nlohmann::json json = {
        {"version", 1u},
        {"layers", settings.layerNames},
        {"collisionMasks", settings.collisionMasks},
    };
    std::error_code filesystemError;
    std::filesystem::create_directories(path_.parent_path(), filesystemError);
    if (filesystemError) {
        error = "Could not create the Physics settings directory.";
        return false;
    }
    const std::filesystem::path temporary = path_.wstring() + L".tmp";
    try {
        std::ofstream stream(temporary, std::ios::trunc);
        stream << json.dump(2) << '\n';
        stream.close();
        if (!stream) {
            error = "Could not write the temporary Physics settings file.";
            return false;
        }
    } catch (const std::exception&) {
        error = "Could not write the Physics settings file.";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, filesystemError);
        error = "Could not replace the Physics settings file.";
        return false;
    }
    error.clear();
    return true;
}

const std::filesystem::path& PhysicsSettingsStore::Path() const {
    return path_;
}
