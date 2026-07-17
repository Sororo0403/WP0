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
        path.extension() != L".wp0project") {
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
} // namespace

RecentProjectsStore::RecentProjectsStore(std::filesystem::path path) : path_(std::move(path)) {}

std::vector<RecentProject> RecentProjectsStore::Load() const {
    std::vector<RecentProject> projects;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path_, error) || error ||
        std::filesystem::file_size(path_, error) > kMaxSettingsBytes || error) {
        return projects;
    }
    try {
        std::ifstream stream(path_);
        nlohmann::json json;
        stream >> json;
        if (!json.is_object() || json.value("version", 0u) != 1u ||
            !json.contains("projects") || !json["projects"].is_array()) {
            return {};
        }
        for (const auto& item : json["projects"]) {
            if (projects.size() >= kMaxRecentProjects || !item.is_object() ||
                !item.contains("name") || !item["name"].is_string() ||
                !item.contains("manifest") || !item["manifest"].is_string()) {
                continue;
            }
            const std::string name = item["name"].get<std::string>();
            const auto manifest = NormalizeExistingManifest(
                Utf8ToPath(item["manifest"].get<std::string>()));
            if (!name.empty() && !manifest.empty()) {
                projects.push_back({name, manifest});
            }
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
