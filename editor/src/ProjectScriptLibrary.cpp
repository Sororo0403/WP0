#include "ProjectScriptLibrary.h"

#include "runtime/BehaviorRegistry.h"
#include "runtime/ScriptModuleApi.h"

#include <Windows.h>

#include <chrono>
#include <memory>
#include <string_view>
#include <vector>

namespace {
constexpr size_t kMaximumScriptTypes = 1024u;

std::filesystem::path ResolveModulePath(const std::filesystem::path& projectRoot) {
#ifdef _DEBUG
    constexpr wchar_t configuration[] = L"Debug";
#else
    constexpr wchar_t configuration[] = L"Release";
#endif
    return (projectRoot / L"Library" / L"ScriptAssemblies" / L"x64" / configuration /
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
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
    }
    std::error_code error;
    std::filesystem::remove(loadedPath_, error);
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
    std::filesystem::create_directories(loadedPath_.parent_path(), filesystemError);
    if (filesystemError ||
        !std::filesystem::copy_file(path_, loadedPath_,
                                    std::filesystem::copy_options::overwrite_existing,
                                    filesystemError)) {
        error = "Project Script module could not be prepared for loading: " +
                path_.generic_string();
        loadedPath_.clear();
        return false;
    }
    HMODULE module = LoadLibraryW(loadedPath_.c_str());
    if (module == nullptr) {
        std::filesystem::remove(loadedPath_, filesystemError);
        loadedPath_.clear();
        error = "Project Script module could not be loaded: " + path_.generic_string();
        return false;
    }
    const auto getVersion = reinterpret_cast<GetScriptModuleApiVersion>(
        GetProcAddress(module, kScriptModuleVersionExport));
    const auto getTypes = reinterpret_cast<GetScriptTypeRegistrations>(
        GetProcAddress(module, kScriptModuleTypesExport));
    if (getVersion == nullptr || getTypes == nullptr ||
        getVersion() != kScriptModuleApiVersion) {
        FreeLibrary(module);
        error = "Project Script module uses an unsupported API version.";
        return false;
    }

    size_t count = 0u;
    const ScriptTypeRegistration* registrations = getTypes(&count);
    if ((count > 0u && registrations == nullptr) || count > kMaximumScriptTypes) {
        FreeLibrary(module);
        error = "Project Script module returned an invalid registration table.";
        return false;
    }
    std::vector<std::string_view> types;
    types.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const ScriptTypeRegistration& registration = registrations[index];
        if (registration.type == nullptr || registration.sourceAsset == nullptr ||
            registration.create == nullptr) {
            FreeLibrary(module);
            error = "Project Script module contains an invalid Script type.";
            return false;
        }
        const std::string_view type(registration.type);
        const std::string_view sourceAsset(registration.sourceAsset);
        if (type.empty() || type.size() > 128u || sourceAsset.empty() ||
            sourceAsset.size() > 1024u || !sourceAsset.starts_with("asset://") ||
            registry.Requirements(type) != nullptr ||
            !registry.TypeFromSourceAsset(sourceAsset).empty()) {
            FreeLibrary(module);
            error = "Project Script module contains an empty or duplicate Script type.";
            return false;
        }
        for (const std::string_view existing : types) {
            if (existing == type) {
                FreeLibrary(module);
                error = "Project Script module contains a duplicate Script type.";
                return false;
            }
        }
        types.push_back(type);
    }

    module_ = module;
    for (size_t index = 0; index < count; ++index) {
        const ScriptTypeRegistration registration = registrations[index];
        const std::string type(registration.type);
        if (!registry.Register(
                type,
                [factory = registration.create, input] {
                    return std::unique_ptr<Behavior>(factory(input));
                },
                registration.requirements, registration.sourceAsset)) {
            error = "Project Script type could not be registered: " + type;
            return false;
        }
    }
    return true;
}

bool ProjectScriptLibrary::IsLoaded() const {
    return module_ != nullptr;
}

const std::filesystem::path& ProjectScriptLibrary::Path() const {
    return path_;
}
