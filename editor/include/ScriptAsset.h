#pragma once

#include <filesystem>

namespace ScriptAssets {
[[nodiscard]] bool IsScriptFile(const std::filesystem::path& path);
[[nodiscard]] bool IsScriptSourceFile(const std::filesystem::path& path);
}
