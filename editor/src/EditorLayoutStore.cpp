#include "EditorLayoutStore.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <system_error>
#include <utility>

namespace {
constexpr uintmax_t kMaxSettingsBytes = 64u * 1024u;

float NormalizeRatio(float value, float fallback, float minimum, float maximum) {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}
} // namespace

EditorLayoutStore::EditorLayoutStore(std::filesystem::path settingsPath)
    : settingsPath_(std::move(settingsPath)) {}

EditorLayoutSettings EditorLayoutStore::Load() const {
    std::error_code error;
    if (!std::filesystem::is_regular_file(settingsPath_, error) || error ||
        std::filesystem::file_size(settingsPath_, error) > kMaxSettingsBytes || error) {
        return {};
    }
    try {
        std::ifstream stream(settingsPath_);
        nlohmann::json json;
        stream >> json;
        if (!json.is_object() || json.value("version", 0u) != 1u ||
            !json.contains("layout") || !json["layout"].is_object()) {
            return {};
        }
        const nlohmann::json& layout = json["layout"];
        EditorLayoutSettings settings{};
        settings.leftWidthRatio = layout.value("leftWidthRatio", settings.leftWidthRatio);
        settings.rightWidthRatio = layout.value("rightWidthRatio", settings.rightWidthRatio);
        settings.bottomHeightRatio =
            layout.value("bottomHeightRatio", settings.bottomHeightRatio);
        return Normalize(settings);
    } catch (const std::exception&) {
        return {};
    }
}

bool EditorLayoutStore::Save(const EditorLayoutSettings& settings) const {
    const EditorLayoutSettings normalized = Normalize(settings);
    const nlohmann::json json = {
        {"version", 1u},
        {"layout",
         {{"leftWidthRatio", normalized.leftWidthRatio},
          {"rightWidthRatio", normalized.rightWidthRatio},
          {"bottomHeightRatio", normalized.bottomHeightRatio}}},
    };
    std::error_code error;
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

EditorLayoutSettings EditorLayoutStore::Normalize(EditorLayoutSettings settings) {
    const EditorLayoutSettings defaults{};
    settings.leftWidthRatio =
        NormalizeRatio(settings.leftWidthRatio, defaults.leftWidthRatio, 0.12f, 0.38f);
    settings.rightWidthRatio =
        NormalizeRatio(settings.rightWidthRatio, defaults.rightWidthRatio, 0.14f, 0.38f);
    settings.bottomHeightRatio =
        NormalizeRatio(settings.bottomHeightRatio, defaults.bottomHeightRatio, 0.14f, 0.55f);
    constexpr float kMaximumSideRatio = 0.72f;
    const float sideRatio = settings.leftWidthRatio + settings.rightWidthRatio;
    if (sideRatio > kMaximumSideRatio) {
        const float scale = kMaximumSideRatio / sideRatio;
        settings.leftWidthRatio *= scale;
        settings.rightWidthRatio *= scale;
    }
    return settings;
}
