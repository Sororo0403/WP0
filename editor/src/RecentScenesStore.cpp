#include "RecentScenesStore.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <optional>
#include <system_error>

namespace {
constexpr size_t kMaxRecentScenes = 10u;
constexpr uintmax_t kMaxSettingsBytes = 1024u * 1024u;

bool HasParentTraversal(const std::filesystem::path& path) {
    return std::ranges::any_of(path, [](const std::filesystem::path& part) {
        return part == L"..";
    });
}

std::optional<std::filesystem::path> ResolveScene(const std::filesystem::path& root,
                                                  const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory() || HasParentTraversal(relative) ||
        relative.extension() != L".likescene") {
        return std::nullopt;
    }
    std::error_code error;
    const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return std::nullopt;
    }
    const std::filesystem::path scene =
        std::filesystem::weakly_canonical(canonicalRoot / relative, error);
    if (error || !std::filesystem::is_regular_file(scene, error) || error) {
        return std::nullopt;
    }
    const std::filesystem::path check = std::filesystem::relative(scene, canonicalRoot, error);
    if (error || check.empty() || check.is_absolute() || HasParentTraversal(check)) {
        return std::nullopt;
    }
    return scene;
}
} // namespace

RecentScenesStore::RecentScenesStore(std::filesystem::path settingsPath,
                                     std::filesystem::path sceneRoot)
    : settingsPath_(std::move(settingsPath)), sceneRoot_(std::move(sceneRoot)) {}

std::vector<std::filesystem::path> RecentScenesStore::Load() const {
    std::vector<std::filesystem::path> scenes;
    std::error_code error;
    if (!std::filesystem::is_regular_file(settingsPath_, error) || error ||
        std::filesystem::file_size(settingsPath_, error) > kMaxSettingsBytes || error) {
        return scenes;
    }
    try {
        std::ifstream stream(settingsPath_);
        nlohmann::json json;
        stream >> json;
        if (!json.is_object() || json.value("version", 0u) != 1u ||
            !json.contains("scenes") || !json["scenes"].is_array()) {
            return {};
        }
        for (const auto& item : json["scenes"]) {
            if (scenes.size() >= kMaxRecentScenes || !item.is_string()) {
                continue;
            }
            const std::optional<std::filesystem::path> scene =
                ResolveScene(sceneRoot_, std::filesystem::path(item.get<std::string>()));
            if (scene) {
                scenes.push_back(*scene);
            }
        }
    } catch (const std::exception&) {
        return {};
    }
    return scenes;
}

bool RecentScenesStore::Save(const std::vector<std::filesystem::path>& scenes) const {
    nlohmann::json json = {{"version", 1u}, {"scenes", nlohmann::json::array()}};
    std::error_code error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(sceneRoot_, error);
    if (error) {
        return false;
    }
    for (const std::filesystem::path& scene : scenes) {
        if (json["scenes"].size() >= kMaxRecentScenes) {
            break;
        }
        const std::filesystem::path relative = std::filesystem::relative(scene, root, error);
        if (!error && !relative.empty() && !relative.is_absolute() &&
            !HasParentTraversal(relative) && relative.extension() == L".likescene") {
            json["scenes"].push_back(relative.generic_string());
        }
        error.clear();
    }
    std::filesystem::create_directories(settingsPath_.parent_path(), error);
    if (error) {
        return false;
    }
    const std::filesystem::path temporary = settingsPath_.wstring() + L".tmp";
    try {
        std::ofstream stream(temporary, std::ios::trunc);
        stream << json.dump(2) << '\n';
        stream.close();
        if (!stream) {
            return false;
        }
    } catch (const std::exception&) {
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), settingsPath_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}
