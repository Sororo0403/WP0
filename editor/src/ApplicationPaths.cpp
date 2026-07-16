#include "ApplicationPaths.h"

#include <Windows.h>
#include <ShlObj.h>

#include <system_error>
#include <vector>

namespace {
std::filesystem::path ExecutablePath() {
    std::vector<wchar_t> buffer(512u);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0u) {
            return {};
        }
        if (length < buffer.size() - 1u) {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2u);
    }
}

std::filesystem::path LocalAppData() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &value))) {
        return {};
    }
    const std::filesystem::path path(value);
    CoTaskMemFree(value);
    return path;
}

std::filesystem::path FindRepositoryRoot(std::filesystem::path directory) {
    std::error_code error;
    for (size_t depth = 0u; depth < 12u && !directory.empty(); ++depth) {
        if (std::filesystem::is_directory(directory / L"engine" / L"resources", error) &&
            !error) {
            return directory;
        }
        error.clear();
        const auto parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return {};
}
} // namespace

ApplicationPaths ApplicationPaths::Discover() {
    ApplicationPaths paths{};
    paths.executable = ExecutablePath();
    paths.installRoot = paths.executable.parent_path();
    paths.engineResources = paths.installRoot / L"resources" / L"engine";
    paths.editorResources = paths.installRoot / L"resources" / L"editor";

    std::error_code error;
    if (!std::filesystem::is_directory(paths.engineResources, error) || error) {
        error.clear();
        std::filesystem::path repository = FindRepositoryRoot(paths.installRoot);
        if (repository.empty()) {
            repository = FindRepositoryRoot(std::filesystem::current_path(error));
        }
        if (!repository.empty()) {
            paths.engineResources = repository / L"engine" / L"resources";
            paths.editorResources = repository / L"editor" / L"resources";
        }
    }

    const std::filesystem::path local = LocalAppData();
    paths.userData = (local.empty() ? paths.installRoot : local) / L"WP0" / L"Editor";
    paths.cache = paths.userData / L"cache";
    return paths;
}
