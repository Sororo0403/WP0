#include "ProjectScriptLibrary.h"

#include "internal/ProjectScriptLibraryUtils.h"

#include <Windows.h>

#include <chrono>
#include <utility>

namespace {
std::filesystem::path ResolveModulePath(const std::filesystem::path& projectRoot) {
#ifdef _DEBUG
    constexpr wchar_t configuration[] = L"Debug";
#else
    constexpr wchar_t configuration[] = L"Release";
#endif
    return (projectRoot / L"library" / L"ScriptAssemblies" / L"x64" / configuration /
            L"ProjectScripts.dll")
        .lexically_normal();
}

std::filesystem::path CreateLoadPath(const std::filesystem::path& modulePath) {
    const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
    return modulePath.parent_path() / L"Loaded" /
           (L"ProjectScripts_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(value) + L".dll");
}
}

ProjectScriptLibrary::~ProjectScriptLibrary() {
    Unload();
}

ProjectScriptLibrary::ProjectScriptLibrary(ProjectScriptLibrary&& other) noexcept
    : module_(std::exchange(other.module_, nullptr)), path_(std::move(other.path_)),
      loadedPath_(std::move(other.loadedPath_)) {
    other.loadedPath_.clear();
}

ProjectScriptLibrary&
ProjectScriptLibrary::operator=(ProjectScriptLibrary&& other) noexcept {
    if (this != &other) {
        Unload();
        module_ = std::exchange(other.module_, nullptr);
        path_ = std::move(other.path_);
        loadedPath_ = std::move(other.loadedPath_);
        other.loadedPath_.clear();
    }
    return *this;
}

void ProjectScriptLibrary::Unload() {
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
    std::error_code error;
    std::filesystem::remove(loadedPath_, error);
    loadedPath_.clear();
}

bool ProjectScriptLibrary::Load(const std::filesystem::path& projectRoot, Input* input,
                                BehaviorRegistry& registry, std::string& error) {
    error.clear();
    if (module_ != nullptr) {
        error = "Project Script module is already loaded.";
        return false;
    }
    path_ = ResolveModulePath(projectRoot);
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(path_, filesystemError) || filesystemError) {
        error = "Project Script module was not found: " + path_.generic_string();
        return false;
    }

    loadedPath_ = CreateLoadPath(path_);
    void* module = nullptr;
    if (!ProjectScriptLibraryUtils::PrepareAndLoadModule(path_, loadedPath_, module, error)) {
        loadedPath_.clear();
        return false;
    }

    size_t count = 0u;
    const ScriptTypeRegistration* registrations = nullptr;
    if (!ProjectScriptLibraryUtils::ResolveScriptRegistrations(module, registrations, count,
                                                               error) ||
        !ProjectScriptLibraryUtils::ValidateScriptRegistrations(registrations, count, registry,
                                                                error)) {
        ProjectScriptLibraryUtils::ReleaseModule(module);
        return false;
    }
    module_ = module;
    return ProjectScriptLibraryUtils::RegisterScriptTypes(registrations, count, input, registry,
                                                          error);
}

bool ProjectScriptLibrary::IsLoaded() const {
    return module_ != nullptr;
}

const std::filesystem::path& ProjectScriptLibrary::Path() const {
    return path_;
}
