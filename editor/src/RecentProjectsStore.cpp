#include "RecentProjectsStore.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace {
constexpr size_t kMaxRecentProjects = 10u;
constexpr uintmax_t kMaxSettingsBytes = 1024u * 1024u;

std::filesystem::path NormalizeExistingManifest(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error ||
        path.extension() != L".likeproject") {
        return {};
    }
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? std::filesystem::path{} : canonical;
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::filesystem::path Utf8ToPath(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), length);
    return std::filesystem::path(result);
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

bool IsReadableRecentProjectsFile(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
    const uintmax_t size = std::filesystem::file_size(path, error);
    return !error && size <= kMaxSettingsBytes;
}

bool HasValidRecentProjectsStructure(const nlohmann::json& json) {
    return json.is_object() && json.contains("version") &&
           json["version"].is_number_unsigned() && json["version"].get<uint64_t>() == 1u &&
           json.contains("projects") && json["projects"].is_array();
}

bool TryDecodeRecentProject(const nlohmann::json& item, RecentProject& project) {
    if (!item.is_object() || !item.contains("name") || !item["name"].is_string() ||
        !item.contains("manifest") || !item["manifest"].is_string()) {
        return false;
    }
    project.name = item["name"].get<std::string>();
    project.manifestPath =
        NormalizeExistingManifest(Utf8ToPath(item["manifest"].get<std::string>()));
    return !project.name.empty() && !project.manifestPath.empty();
}
} // namespace

RecentProjectsStore::RecentProjectsStore(std::filesystem::path path) : path_(std::move(path)) {}

std::vector<RecentProject> RecentProjectsStore::Load() const {
    std::vector<RecentProject> projects;
    if (!IsReadableRecentProjectsFile(path_)) {
        return projects;
    }
    try {
        std::ifstream stream(path_);
        nlohmann::json json;
        stream >> json;
        if (!HasValidRecentProjectsStructure(json)) {
            return {};
        }
        for (const auto& item : json["projects"]) {
            if (projects.size() >= kMaxRecentProjects) {
                break;
            }
            RecentProject project;
            if (!TryDecodeRecentProject(item, project)) {
                continue;
            }
            projects.push_back(std::move(project));
        }
    } catch (const std::exception&) {
        return {};
    }
    return projects;
}

bool RecentProjectsStore::Record(const RecentProject& project) const {
    const std::filesystem::path manifest = NormalizeExistingManifest(project.manifestPath);
    if (project.name.empty() || manifest.empty()) {
        return false;
    }
    std::vector<RecentProject> projects = Load();
    std::erase_if(projects, [&manifest](const RecentProject& item) {
        return SamePath(item.manifestPath, manifest);
    });
    projects.insert(projects.begin(), {project.name, manifest});
    if (projects.size() > kMaxRecentProjects) {
        projects.resize(kMaxRecentProjects);
    }

    nlohmann::json json = {{"version", 1u}, {"projects", nlohmann::json::array()}};
    for (const RecentProject& item : projects) {
        json["projects"].push_back(
            {{"name", item.name}, {"manifest", PathToUtf8(item.manifestPath)}});
    }
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) {
        return false;
    }
    const std::filesystem::path temporary = path_.wstring() + L".tmp";
    try {
        std::ofstream stream(temporary, std::ios::trunc);
        if (!stream) {
            return false;
        }
        stream << json.dump(2) << '\n';
        stream.close();
        if (!stream) {
            return false;
        }
    } catch (const std::exception&) {
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}
