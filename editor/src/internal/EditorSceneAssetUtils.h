#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace EditorSceneAssetUtils {
inline constexpr const char* kModelAssetDragPayload = "EDITOR_MODEL_ASSET";
inline constexpr const char* kTextureAssetDragPayload = "EDITOR_TEXTURE_ASSET";
inline constexpr const char* kAudioAssetDragPayload = "EDITOR_AUDIO_ASSET";
inline constexpr const char* kFontAssetDragPayload = "EDITOR_FONT_ASSET";
inline constexpr const char* kScriptAssetDragPayload = "EDITOR_SCRIPT_ASSET";
inline constexpr const char* kPrefabAssetDragPayload = "EDITOR_PREFAB_ASSET";

inline bool ContainsCaseInsensitive(std::string value, std::string query) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::ranges::transform(query, query.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value.find(query) != std::string::npos;
}

inline std::string LowercaseAscii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

inline bool HasParentTraversal(const std::filesystem::path& path) {
    return std::ranges::any_of(path, [](const std::filesystem::path& part) {
        return part == L"..";
    });
}

inline bool IsPathWithinRoot(const std::filesystem::path& root,
                             const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const std::filesystem::path canonicalPath =
        std::filesystem::weakly_canonical(path, error);
    if (error) {
        return false;
    }
    const std::filesystem::path relative =
        std::filesystem::relative(canonicalPath, canonicalRoot, error);
    return !error && !relative.empty() && !relative.is_absolute() &&
           !HasParentTraversal(relative);
}

inline bool IsPathAtOrWithinRoot(const std::filesystem::path& root,
                                 const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const std::filesystem::path canonicalPath =
        std::filesystem::weakly_canonical(path, error);
    return !error &&
           (canonicalPath == canonicalRoot ||
            IsPathWithinRoot(canonicalRoot, canonicalPath));
}

inline bool IsValidAssetFilename(std::string_view filename) {
    if (filename.empty() || filename == "." || filename == ".." ||
        filename.ends_with('.') || filename.ends_with(' ')) {
        return false;
    }
    constexpr std::string_view invalidCharacters = "<>:\"/\\|?*";
    return std::ranges::none_of(filename, [invalidCharacters](unsigned char character) {
        return character < 32u ||
               invalidCharacters.find(static_cast<char>(character)) !=
                   std::string_view::npos;
    });
}

inline std::optional<std::filesystem::path>
AssetRelativeFromReference(std::string_view reference) {
    constexpr std::string_view uriPrefix = "asset://";
    constexpr std::string_view projectPrefix = "assets/";
    if (reference.starts_with(uriPrefix)) {
        reference.remove_prefix(uriPrefix.size());
    } else if (reference.starts_with(projectPrefix)) {
        reference.remove_prefix(projectPrefix.size());
    } else {
        return std::nullopt;
    }
    const std::filesystem::path relative =
        std::filesystem::path(std::string(reference)).lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory() || HasParentTraversal(relative)) {
        return std::nullopt;
    }
    return relative;
}

inline bool AssetPathMatches(const std::filesystem::path& candidate,
                             const std::filesystem::path& target, bool directory) {
    const std::string candidateText =
        candidate.lexically_normal().generic_string();
    const std::string targetText = target.lexically_normal().generic_string();
    return candidateText == targetText ||
           (directory && candidateText.starts_with(targetText + '/'));
}

inline bool IsPrefabAsset(const std::filesystem::path& path) {
    return LowercaseAscii(path.extension().string()) == ".likeprefab";
}
} // namespace EditorSceneAssetUtils
