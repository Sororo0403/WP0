#include "ProjectDescriptor.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
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
        if (input.extension() != L".likeproject") {
            error = "Project manifest must use the .likeproject extension.";
            return {};
        }
        return input;
    }
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::directory_iterator(input, filesystemError)) {
        if (filesystemError) {
            break;
        }
        if (entry.is_regular_file(filesystemError) &&
            entry.path().extension() == L".likeproject") {
            if (!found.empty()) {
                error = "Project directory contains more than one .likeproject file.";
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

std::string CreateProjectId() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        return {};
    }
    wchar_t buffer[40]{};
    if (StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer))) <= 0) {
        return {};
    }
    std::wstring value(buffer);
    if (value.size() >= 2u && value.front() == L'{' && value.back() == L'}') {
        value = value.substr(1u, value.size() - 2u);
    }
    std::string result;
    result.reserve(value.size());
    std::ranges::transform(value, std::back_inserter(result), [](wchar_t character) {
        if (character >= L'A' && character <= L'F') {
            character = character - L'A' + L'a';
        }
        return static_cast<char>(character);
    });
    return result;
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
            !IsSafeRelative(sceneRelative) || !IsSafeRelative(startupRelative) ||
            startupRelative.extension() != L".likescene") {
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

bool ProjectDescriptor::Create(const std::filesystem::path& directory, const std::string& name,
                               ProjectDescriptor& descriptor, std::string& error) {
    error.clear();
    if (name.empty() || name.size() > 128u || name.find('\0') != std::string::npos) {
        error = "Project name is empty or too long.";
        return false;
    }

    std::error_code filesystemError;
    const std::filesystem::path root = std::filesystem::absolute(directory, filesystemError)
                                           .lexically_normal();
    if (filesystemError || !std::filesystem::is_directory(root, filesystemError) ||
        filesystemError) {
        error = "Project directory does not exist.";
        return false;
    }
    if (!std::filesystem::is_empty(root, filesystemError) || filesystemError) {
        error = "Select a new or empty directory for the project.";
        return false;
    }

    const std::string projectId = CreateProjectId();
    if (projectId.empty()) {
        error = "Could not generate a project identifier.";
        return false;
    }
    std::filesystem::path manifestName;
    try {
        manifestName = root.filename().wstring() + L".likeproject";
    } catch (const std::exception&) {
        error = "Project directory name is invalid.";
        return false;
    }
    const std::filesystem::path manifest = root / manifestName;
    const std::filesystem::path temporary = manifest.wstring() + L".tmp";
    const std::filesystem::path ignore = root / L".gitignore";
    const std::filesystem::path assets = root / L"assets";
    const std::filesystem::path scenes = root / L"scenes";
    const nlohmann::json json = {
        {"schemaVersion", 1u},
        {"projectId", projectId},
        {"name", name},
        {"assetRoot", "assets"},
        {"sceneRoot", "scenes"},
        {"startupScene", "scenes/untitled.likescene"},
        {"engineVersion", "0.1.0"},
    };

    std::filesystem::create_directory(assets, filesystemError);
    if (filesystemError) {
        error = "Could not create the assets directory.";
        return false;
    }
    std::filesystem::create_directory(scenes, filesystemError);
    if (filesystemError) {
        std::filesystem::remove(assets, filesystemError);
        error = "Could not create the scenes directory.";
        return false;
    }
    try {
        std::ofstream ignoreStream(ignore, std::ios::trunc);
        ignoreStream << "/library/\n/build/\n";
        ignoreStream.close();
        if (!ignoreStream) {
            throw std::ios_base::failure("project ignore write failed");
        }
    } catch (const std::exception&) {
        std::filesystem::remove(scenes, filesystemError);
        std::filesystem::remove(assets, filesystemError);
        error = "Could not write the project ignore file.";
        return false;
    }
    try {
        std::ofstream stream(temporary, std::ios::trunc);
        stream << json.dump(2) << '\n';
        stream.close();
        if (!stream) {
            throw std::ios_base::failure("project manifest write failed");
        }
    } catch (const std::exception&) {
        std::filesystem::remove(temporary, filesystemError);
        std::filesystem::remove(ignore, filesystemError);
        std::filesystem::remove(scenes, filesystemError);
        std::filesystem::remove(assets, filesystemError);
        error = "Could not write the project manifest.";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), manifest.c_str(), MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, filesystemError);
        std::filesystem::remove(ignore, filesystemError);
        std::filesystem::remove(scenes, filesystemError);
        std::filesystem::remove(assets, filesystemError);
        error = "Could not finish writing the project manifest.";
        return false;
    }
    return Load(manifest, descriptor, error);
}

bool ProjectDescriptor::SetStartupScene(const std::filesystem::path& project,
                                        const std::filesystem::path& scene,
                                        ProjectDescriptor& descriptor,
                                        std::string& error) {
    ProjectDescriptor current;
    if (!Load(project, current, error)) {
        return false;
    }

    std::error_code filesystemError;
    const std::filesystem::path absoluteScene =
        std::filesystem::absolute(scene, filesystemError).lexically_normal();
    if (filesystemError || absoluteScene.extension() != L".likescene" ||
        !std::filesystem::is_regular_file(absoluteScene, filesystemError) ||
        filesystemError || !IsInside(current.sceneRoot, absoluteScene)) {
        error = "Startup Scene must be an existing .likescene file inside the scene directory.";
        return false;
    }
    const std::filesystem::path relativeScene =
        std::filesystem::relative(absoluteScene, current.root, filesystemError);
    if (filesystemError || !IsSafeRelative(relativeScene)) {
        error = "Startup Scene path could not be stored safely.";
        return false;
    }

    nlohmann::json json;
    const std::filesystem::path temporary =
        current.manifestPath.wstring() + L".tmp";
    try {
        std::ifstream input(current.manifestPath);
        if (!input) {
            error = "Project manifest could not be opened.";
            return false;
        }
        input >> json;
        input.close();
        json["startupScene"] = relativeScene.generic_string();

        std::ofstream output(temporary, std::ios::trunc);
        output << json.dump(2) << '\n';
        output.close();
        if (!output) {
            std::filesystem::remove(temporary, filesystemError);
            error = "Could not write the Project manifest.";
            return false;
        }
        if (!MoveFileExW(temporary.c_str(), current.manifestPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary, filesystemError);
            error = "Could not finish updating the Project manifest.";
            return false;
        }
    } catch (const std::exception&) {
        std::filesystem::remove(temporary, filesystemError);
        error = "Project manifest could not be updated.";
        return false;
    }
    return Load(current.manifestPath, descriptor, error);
}
