#include "ProjectDescriptor.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace {
bool IsSafeRelative(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    const std::filesystem::path parent(L"..");
    return std::none_of(path.begin(), path.end(),
                        [&parent](const std::filesystem::path& part) { return part == parent; });
}

bool IsInside(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    return !error && IsSafeRelative(relative);
}

std::filesystem::path FindManifest(const std::filesystem::path& input, std::string& error) {
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(input, filesystemError)) {
        return input;
    }
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::directory_iterator(input, filesystemError)) {
        if (filesystemError) {
            break;
        }
        if (entry.is_regular_file(filesystemError) && entry.path().extension() == L".wp0project") {
            if (!found.empty()) {
                error = "Project directory contains more than one .wp0project file.";
                return {};
            }
            found = entry.path();
        }
    }
    if (found.empty() && error.empty()) {
        error = "Project manifest was not found.";
    }
    return found;
}
} // namespace

bool ProjectDescriptor::Load(const std::filesystem::path& path, ProjectDescriptor& descriptor,
                             std::string& error) {
    error.clear();
    const std::filesystem::path manifest = FindManifest(path, error);
    if (manifest.empty()) {
        return false;
    }
    std::ifstream stream(manifest);
    if (!stream) {
        error = "Project manifest could not be opened.";
        return false;
    }
    nlohmann::json json;
    try {
        stream >> json;
        if (!json.is_object() || !json.contains("schemaVersion") ||
            !json["schemaVersion"].is_number_unsigned() || !json.contains("projectId") ||
            !json["projectId"].is_string() || !json.contains("name") ||
            !json["name"].is_string() || !json.contains("assetRoot") ||
            !json["assetRoot"].is_string() || !json.contains("sceneRoot") ||
            !json["sceneRoot"].is_string() || !json.contains("startupScene") ||
            !json["startupScene"].is_string()) {
            error = "Project manifest has missing or invalid fields.";
            return false;
        }
        descriptor.schemaVersion = json["schemaVersion"].get<uint32_t>();
        descriptor.projectId = json["projectId"].get<std::string>();
        descriptor.name = json["name"].get<std::string>();
        descriptor.manifestPath = std::filesystem::absolute(manifest).lexically_normal();
        descriptor.root = descriptor.manifestPath.parent_path();
        const auto assetRelative = std::filesystem::path(json["assetRoot"].get<std::string>());
        const auto sceneRelative = std::filesystem::path(json["sceneRoot"].get<std::string>());
        const auto startupRelative = std::filesystem::path(json["startupScene"].get<std::string>());
        if (descriptor.schemaVersion != 1u || descriptor.projectId.empty() ||
            descriptor.name.empty() || !IsSafeRelative(assetRelative) ||
            !IsSafeRelative(sceneRelative) || !IsSafeRelative(startupRelative)) {
            error = "Project manifest contains unsupported or unsafe values.";
            return false;
        }
        descriptor.assetRoot = (descriptor.root / assetRelative).lexically_normal();
        descriptor.sceneRoot = (descriptor.root / sceneRelative).lexically_normal();
        descriptor.startupScene = (descriptor.root / startupRelative).lexically_normal();
        if (!IsInside(descriptor.root, descriptor.assetRoot) ||
            !IsInside(descriptor.root, descriptor.sceneRoot) ||
            !IsInside(descriptor.sceneRoot, descriptor.startupScene)) {
            error = "Project paths escape the project directory.";
            return false;
        }
    } catch (const std::exception&) {
        error = "Project manifest is not valid JSON.";
        return false;
    }
    return true;
}
