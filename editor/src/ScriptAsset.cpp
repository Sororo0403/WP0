#include "ScriptAsset.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace {
constexpr size_t kMaxScriptAssetBytes = 64u * 1024u;

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}
}

bool ScriptAssets::IsScriptFile(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension == ".likescript";
}

bool ScriptAssets::Deserialize(std::string_view json, ScriptAsset& asset,
                               std::string* error) {
    try {
        const nlohmann::json encoded = nlohmann::json::parse(json);
        if (!encoded.is_object() || !encoded.contains("version") ||
            !encoded["version"].is_number_unsigned() ||
            encoded["version"].get<uint32_t>() != 1u || !encoded.contains("type") ||
            !encoded["type"].is_string()) {
            SetError(error, "Script asset must contain version 1 and a type string.");
            return false;
        }
        ScriptAsset decoded{};
        decoded.type = encoded["type"].get<std::string>();
        if (decoded.type.empty() || decoded.type.size() > 128u ||
            decoded.type.find('\0') != std::string::npos) {
            SetError(error, "Script asset type is invalid.");
            return false;
        }
        asset = std::move(decoded);
        return true;
    } catch (const nlohmann::json::exception&) {
        SetError(error, "Script asset JSON is invalid.");
        return false;
    }
}

bool ScriptAssets::Load(const std::filesystem::path& path, ScriptAsset& asset,
                        std::string* error) {
    if (!IsScriptFile(path)) {
        SetError(error, "File is not a .likescript asset.");
        return false;
    }
    std::error_code filesystemError;
    const uintmax_t size = std::filesystem::file_size(path, filesystemError);
    if (filesystemError || size > kMaxScriptAssetBytes) {
        SetError(error, "Script asset is missing or too large.");
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        SetError(error, "Script asset could not be opened.");
        return false;
    }
    const std::string contents{std::istreambuf_iterator<char>(stream),
                               std::istreambuf_iterator<char>()};
    return Deserialize(contents, asset, error);
}
