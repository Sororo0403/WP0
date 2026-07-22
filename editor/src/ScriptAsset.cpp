#include "ScriptAsset.h"

#include <algorithm>
#include <cctype>

bool ScriptAssets::IsScriptFile(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension == ".cpp";
}

bool ScriptAssets::IsScriptSourceFile(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension == ".cpp" || extension == ".h" || extension == ".hpp";
}
