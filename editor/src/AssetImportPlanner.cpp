#include "AssetImportPlanner.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace {

bool HasParentTraversal(const std::filesystem::path& path) {
    return std::ranges::any_of(path, [](const std::filesystem::path& part) {
        return part == L"..";
    });
}

bool IsPathWithinRoot(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return false;
    }
    const std::filesystem::path relative =
        std::filesystem::relative(canonicalPath, canonicalRoot, error);
    return !error && !relative.empty() && !relative.is_absolute() &&
           !HasParentTraversal(relative);
}

bool IsModelAsset(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    constexpr std::string_view extensions[] = {".fbx", ".obj", ".gltf", ".glb",
                                                ".dae", ".3ds", ".ply"};
    return std::ranges::find(extensions, extension) != std::end(extensions);
}

bool IsTextureAsset(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    constexpr std::string_view extensions[] = {
        ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".hdr", ".exr"};
    return std::ranges::find(extensions, extension) != std::end(extensions);
}

bool IsAudioAsset(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    constexpr std::string_view extensions[] = {".wav", ".mp3", ".aac", ".m4a", ".wma"};
    return std::ranges::find(extensions, extension) != std::end(extensions);
}

bool IsImportableAssetFile(const std::filesystem::path& path) {
    if (IsModelAsset(path) || IsAudioAsset(path)) {
        return true;
    }
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    constexpr std::string_view dependencyExtensions[] = {
        ".bin", ".mtl", ".png", ".jpg", ".jpeg", ".tga",
        ".bmp", ".dds", ".hdr", ".exr"};
    return std::ranges::find(dependencyExtensions, extension) !=
           std::end(dependencyExtensions);
}

std::optional<std::string> DecodeLocalAssetUri(std::string_view uri) {
    if (uri.empty() || uri.starts_with("data:") || uri.find("://") != std::string_view::npos ||
        uri.find_first_of("?#") != std::string_view::npos) {
        return std::nullopt;
    }
    auto hexValue = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        return character >= 'a' && character <= 'f' ? character - 'a' + 10 : -1;
    };
    std::string decoded;
    decoded.reserve(uri.size());
    for (size_t index = 0; index < uri.size(); ++index) {
        if (uri[index] != '%') {
            decoded.push_back(uri[index]);
            continue;
        }
        if (index + 2u >= uri.size()) {
            return std::nullopt;
        }
        const int high = hexValue(uri[index + 1u]);
        const int low = hexValue(uri[index + 2u]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2u;
    }
    return decoded;
}

bool AddAssetImportFile(std::vector<AssetImport::File>& files,
                        const std::filesystem::path& source,
                        const std::filesystem::path& relativeDestination,
                        std::string& errorMessage) {
    const std::filesystem::path normalized = relativeDestination.lexically_normal();
    if (normalized.empty() || normalized.is_absolute() || normalized.has_root_name() ||
        normalized.has_root_directory() || HasParentTraversal(normalized)) {
        errorMessage = "An asset dependency contains an unsafe path.";
        return false;
    }
    const auto existing = std::ranges::find_if(files, [&normalized](const AssetImport::File& item) {
        return _wcsicmp(item.relativeDestination.c_str(), normalized.c_str()) == 0;
    });
    if (existing != files.end()) {
        std::error_code error;
        if (std::filesystem::equivalent(existing->source, source, error) && !error) {
            return true;
        }
        errorMessage = "Multiple import files target the same asset path.";
        return false;
    }
    files.push_back({source, normalized});
    return true;
}

bool CollectGltfDependencies(const std::filesystem::path& gltfPath,
                             std::vector<AssetImport::File>& files,
                             std::string& errorMessage) {
    std::ifstream stream(gltfPath, std::ios::binary);
    const nlohmann::json document = nlohmann::json::parse(stream, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        errorMessage = "Could not parse glTF dependencies: " + gltfPath.filename().string();
        return false;
    }
    const auto collectUris = [&](const char* arrayName) {
        const auto array = document.find(arrayName);
        if (array == document.end() || !array->is_array()) {
            return true;
        }
        for (const nlohmann::json& entry : *array) {
            const auto uriValue = entry.find("uri");
            if (uriValue == entry.end() || !uriValue->is_string()) {
                continue;
            }
            const std::string uri = uriValue->get<std::string>();
            if (uri.starts_with("data:")) {
                continue;
            }
            const std::optional<std::string> decoded = DecodeLocalAssetUri(uri);
            if (!decoded) {
                errorMessage = "glTF contains an unsupported external URI: " + uri;
                return false;
            }
            const std::u8string decodedUtf8(
                reinterpret_cast<const char8_t*>(decoded->data()), decoded->size());
            const std::filesystem::path relative =
                std::filesystem::path(decodedUtf8).lexically_normal();
            const std::filesystem::path source = gltfPath.parent_path() / relative;
            if (!IsPathWithinRoot(gltfPath.parent_path(), source)) {
                errorMessage = "glTF dependency escapes its source folder: " + uri;
                return false;
            }
            std::error_code error;
            if (!std::filesystem::is_regular_file(source, error) || error) {
                errorMessage = "Missing glTF dependency: " + source.string();
                return false;
            }
            if (!AddAssetImportFile(files, source, relative, errorMessage)) {
                return false;
            }
        }
        return true;
    };
    return collectUris("buffers") && collectUris("images");
}

bool HaveEqualContentsInternal(const std::filesystem::path& left,
                            const std::filesystem::path& right) {
    std::error_code error;
    if (std::filesystem::equivalent(left, right, error) && !error) {
        return true;
    }
    error.clear();
    const uintmax_t leftSize = std::filesystem::file_size(left, error);
    if (error) {
        return false;
    }
    const uintmax_t rightSize = std::filesystem::file_size(right, error);
    if (error || leftSize != rightSize) {
        return false;
    }
    std::ifstream leftStream(left, std::ios::binary);
    std::ifstream rightStream(right, std::ios::binary);
    std::array<char, 64 * 1024> leftBuffer{};
    std::array<char, 64 * 1024> rightBuffer{};
    while (leftStream && rightStream) {
        leftStream.read(leftBuffer.data(), leftBuffer.size());
        rightStream.read(rightBuffer.data(), rightBuffer.size());
        if (leftStream.gcount() != rightStream.gcount() ||
            !std::equal(leftBuffer.begin(), leftBuffer.begin() + leftStream.gcount(),
                        rightBuffer.begin())) {
            return false;
        }
    }
    return leftStream.eof() && rightStream.eof();
}

std::vector<std::string> TokenizeAssetReferenceLine(std::string_view line) {
    std::vector<std::string> tokens;
    size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() &&
               std::isspace(static_cast<unsigned char>(line[index]))) {
            ++index;
        }
        if (index == line.size() || line[index] == '#') {
            break;
        }
        std::string token;
        char quote = '\0';
        if (line[index] == '"' || line[index] == '\'') {
            quote = line[index++];
        }
        while (index < line.size()) {
            const char character = line[index];
            if (quote != '\0' && character == quote) {
                ++index;
                break;
            }
            if (quote == '\0' &&
                (std::isspace(static_cast<unsigned char>(character)) || character == '#')) {
                break;
            }
            if (character == '\\' && index + 1u < line.size()) {
                const char escaped = line[index + 1u];
                if (std::isspace(static_cast<unsigned char>(escaped)) || escaped == '"' ||
                    escaped == '\'' || escaped == '\\' || escaped == '#') {
                    token.push_back(escaped);
                    index += 2u;
                    continue;
                }
            }
            token.push_back(character);
            ++index;
        }
        if (!token.empty()) {
            tokens.push_back(std::move(token));
        }
        while (index < line.size() &&
               !std::isspace(static_cast<unsigned char>(line[index])) && line[index] != '#') {
            ++index;
        }
    }
    return tokens;
}

std::string JoinAssetReferenceTokens(const std::vector<std::string>& tokens, size_t first) {
    std::string result;
    for (size_t index = first; index < tokens.size(); ++index) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += tokens[index];
    }
    return result;
}

std::filesystem::path AssetPathFromUtf8(std::string_view text) {
    const std::u8string utf8(reinterpret_cast<const char8_t*>(text.data()), text.size());
    return std::filesystem::path(utf8);
}

bool IsNumericAssetToken(std::string_view token) {
    if (token.empty()) {
        return false;
    }
    char* end = nullptr;
    const std::string value(token);
    std::strtof(value.c_str(), &end);
    return end == value.c_str() + value.size();
}

std::optional<std::string>
ExtractMtlTextureReference(const std::vector<std::string>& tokens) {
    size_t index = 1;
    while (index < tokens.size() && tokens[index].starts_with('-')) {
        std::string option = tokens[index++];
        std::ranges::transform(option, option.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (option == "-o" || option == "-s" || option == "-t") {
            size_t values = 0;
            while (index < tokens.size() && values < 3u &&
                   IsNumericAssetToken(tokens[index])) {
                ++index;
                ++values;
            }
        } else {
            const size_t values = option == "-mm" ? 2u : 1u;
            index = (std::min)(tokens.size(), index + values);
        }
    }
    if (index >= tokens.size()) {
        return std::nullopt;
    }
    return JoinAssetReferenceTokens(tokens, index);
}

bool AddLocalObjDependency(const std::filesystem::path& objRoot,
                           const std::filesystem::path& referenceBase,
                           std::string_view reference,
                           std::vector<AssetImport::File>& files,
                           std::string& errorMessage,
                           std::filesystem::path* resolvedSource = nullptr) {
    const std::filesystem::path relative = AssetPathFromUtf8(reference).lexically_normal();
    const std::filesystem::path source = referenceBase / relative;
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory() || !IsPathWithinRoot(objRoot, source)) {
        errorMessage = "OBJ dependency escapes its source folder: " + std::string(reference);
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error) || error) {
        errorMessage = "Missing OBJ dependency: " + source.string();
        return false;
    }
    const std::filesystem::path baseRelative = referenceBase.lexically_relative(objRoot);
    const std::filesystem::path destination = (baseRelative / relative).lexically_normal();
    if (!AddAssetImportFile(files, source, destination, errorMessage)) {
        return false;
    }
    if (resolvedSource != nullptr) {
        *resolvedSource = source;
    }
    return true;
}

bool CollectMtlTextures(const std::filesystem::path& objRoot,
                        const std::filesystem::path& mtlPath,
                        std::vector<AssetImport::File>& files,
                        std::string& errorMessage) {
    constexpr uintmax_t kMaxMaterialFileBytes = 16u * 1024u * 1024u;
    std::error_code error;
    if (std::filesystem::file_size(mtlPath, error) > kMaxMaterialFileBytes || error) {
        errorMessage = "MTL file is too large or unreadable: " + mtlPath.filename().string();
        return false;
    }
    std::ifstream stream(mtlPath);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.size() > 64u * 1024u) {
            errorMessage = "MTL contains an excessively long line.";
            return false;
        }
        const std::vector<std::string> tokens = TokenizeAssetReferenceLine(line);
        if (tokens.size() < 2u) {
            continue;
        }
        std::string directive = tokens.front();
        std::ranges::transform(directive, directive.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        const bool textureDirective = directive.starts_with("map_") || directive == "bump" ||
                                      directive == "disp" || directive == "decal" ||
                                      directive == "refl" || directive == "norm";
        if (!textureDirective) {
            continue;
        }
        const std::optional<std::string> reference = ExtractMtlTextureReference(tokens);
        if (!reference ||
            !AddLocalObjDependency(objRoot, mtlPath.parent_path(), *reference, files,
                                   errorMessage)) {
            if (!reference) {
                errorMessage = "MTL texture directive is missing a filename.";
            }
            return false;
        }
    }
    if (!stream.eof()) {
        errorMessage = "Could not read MTL file: " + mtlPath.filename().string();
        return false;
    }
    return true;
}

bool CollectObjDependencies(const std::filesystem::path& objPath,
                            std::vector<AssetImport::File>& files,
                            std::string& errorMessage) {
    constexpr uintmax_t kMaxObjFileBytes = 256u * 1024u * 1024u;
    std::error_code error;
    if (std::filesystem::file_size(objPath, error) > kMaxObjFileBytes || error) {
        errorMessage = "OBJ file is too large or unreadable: " + objPath.filename().string();
        return false;
    }
    std::ifstream stream(objPath);
    std::string line;
    std::unordered_set<std::wstring> parsedMaterials;
    while (std::getline(stream, line)) {
        if (line.size() > 64u * 1024u) {
            errorMessage = "OBJ contains an excessively long line.";
            return false;
        }
        const std::vector<std::string> tokens = TokenizeAssetReferenceLine(line);
        if (tokens.size() < 2u || _stricmp(tokens.front().c_str(), "mtllib") != 0) {
            continue;
        }

        std::vector<std::string> materialReferences;
        const std::string combined = JoinAssetReferenceTokens(tokens, 1u);
        const std::filesystem::path combinedPath = objPath.parent_path() /
                                                   AssetPathFromUtf8(combined);
        error.clear();
        if (std::filesystem::is_regular_file(combinedPath, error) && !error) {
            materialReferences.push_back(combined);
        } else {
            materialReferences.assign(tokens.begin() + 1, tokens.end());
        }

        for (const std::string& reference : materialReferences) {
            std::filesystem::path mtlPath;
            if (!AddLocalObjDependency(objPath.parent_path(), objPath.parent_path(), reference,
                                       files, errorMessage, &mtlPath)) {
                return false;
            }
            std::error_code canonicalError;
            std::filesystem::path canonical =
                std::filesystem::weakly_canonical(mtlPath, canonicalError);
            if (canonicalError) {
                errorMessage = "Could not resolve MTL dependency: " + mtlPath.string();
                return false;
            }
            std::wstring key = canonical.wstring();
            std::ranges::transform(key, key.begin(), [](wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            });
            if (parsedMaterials.insert(std::move(key)).second &&
                !CollectMtlTextures(objPath.parent_path(), mtlPath, files, errorMessage)) {
                return false;
            }
        }
    }
    if (!stream.eof()) {
        errorMessage = "Could not read OBJ file: " + objPath.filename().string();
        return false;
    }
    return true;
}

} // namespace

namespace AssetImport {

bool IsModelFile(const std::filesystem::path& path) {
    return IsModelAsset(path);
}

bool IsTextureFile(const std::filesystem::path& path) {
    return IsTextureAsset(path);
}

bool IsAudioFile(const std::filesystem::path& path) {
    return IsAudioAsset(path);
}

bool IsSelectableFile(const std::filesystem::path& path) {
    return IsImportableAssetFile(path);
}

bool BuildPlan(const std::vector<std::filesystem::path>& selectedFiles,
               std::vector<File>& files, std::string& errorMessage) {
    files.clear();
    errorMessage.clear();
    const bool containsModel =
        std::ranges::any_of(selectedFiles, [](const std::filesystem::path& path) {
            return IsModelAsset(path);
        });
    const bool standaloneAssetsOnly = !selectedFiles.empty() &&
        std::ranges::all_of(selectedFiles, [](const auto& path) {
            return IsTextureAsset(path) || IsAudioAsset(path);
        });
    if (selectedFiles.empty() || (!containsModel && !standaloneAssetsOnly)) {
        errorMessage = "Asset import requires a supported model, texture, or audio file.";
        return false;
    }
    for (const std::filesystem::path& source : selectedFiles) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(source, error) || error ||
            !IsImportableAssetFile(source)) {
            errorMessage = "Asset import contains an invalid or unsupported file.";
            return false;
        }
        if (!AddAssetImportFile(files, source, source.filename(), errorMessage)) {
            return false;
        }
        std::string extension = source.extension().string();
        std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (extension == ".gltf" &&
            !CollectGltfDependencies(source, files, errorMessage)) {
            return false;
        }
        if (extension == ".obj" &&
            !CollectObjDependencies(source, files, errorMessage)) {
            return false;
        }
    }
    return true;
}

bool HaveEqualContents(const std::filesystem::path& left,
                       const std::filesystem::path& right) {
    return HaveEqualContentsInternal(left, right);
}

} // namespace AssetImport
