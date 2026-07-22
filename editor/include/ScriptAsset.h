#pragma once

#include <filesystem>
#include <string>
#include <string_view>

struct ScriptAsset {
    std::string type;
};

namespace ScriptAssets {
[[nodiscard]] bool IsScriptFile(const std::filesystem::path& path);
[[nodiscard]] bool Deserialize(std::string_view json, ScriptAsset& asset,
                               std::string* error = nullptr);
[[nodiscard]] bool Load(const std::filesystem::path& path, ScriptAsset& asset,
                        std::string* error = nullptr);
}
