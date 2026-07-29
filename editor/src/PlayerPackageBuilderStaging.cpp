#include "internal/PlayerPackageBuilderInternal.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <ranges>
#include <system_error>

namespace PlayerPackageBuilderInternal {
namespace {
bool CopyDirectory(const std::filesystem::path& source,
                   const std::filesystem::path& destination,
                   std::error_code& error) {
    if (!std::filesystem::is_directory(source, error) || error) {
        return false;
    }
    std::filesystem::copy(
        source, destination,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing,
        error);
    return !error;
}

bool IsCppSourceFile(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::ranges::transform(extension, extension.begin(), ::towlower);
    return extension == L".cpp" || extension == L".h" || extension == L".hpp";
}

bool CopyProjectAssetEntry(const std::filesystem::directory_entry& entry,
                           const std::filesystem::path& source,
                           const std::filesystem::path& destination,
                           std::error_code& error) {
    const std::filesystem::path relative =
        std::filesystem::relative(entry.path(), source, error);
    if (error) {
        return false;
    }
    const std::filesystem::path target = destination / relative;
    if (entry.is_directory(error)) {
        std::filesystem::create_directories(target, error);
    } else if (entry.is_regular_file(error) && !IsCppSourceFile(entry.path())) {
        std::filesystem::create_directories(target.parent_path(), error);
        if (!error) {
            std::filesystem::copy_file(
                entry.path(), target,
                std::filesystem::copy_options::overwrite_existing, error);
        }
    }
    return !error;
}

bool CopyProjectAssets(const std::filesystem::path& source,
                       const std::filesystem::path& destination,
                       std::error_code& error) {
    if (!std::filesystem::is_directory(source, error) || error) {
        return false;
    }
    std::filesystem::create_directories(destination, error);
    if (error) {
        return false;
    }
    for (std::filesystem::recursive_directory_iterator iterator(source, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (!CopyProjectAssetEntry(*iterator, source, destination, error)) {
            return false;
        }
    }
    return !error;
}

bool CopyRuntimeDlls(const std::filesystem::path& executableDirectory,
                     const std::filesystem::path& staging, std::string& error) {
    std::error_code filesystemError;
    for (const auto& entry :
         std::filesystem::directory_iterator(executableDirectory, filesystemError)) {
        if (filesystemError) {
            error = "Could not enumerate Player runtime files.";
            return false;
        }
        if (!entry.is_regular_file(filesystemError) ||
            entry.path().extension() != L".dll") {
            continue;
        }
        std::filesystem::copy_file(
            entry.path(), staging / entry.path().filename(),
            std::filesystem::copy_options::overwrite_existing, filesystemError);
        if (filesystemError) {
            error = "Could not copy a Player runtime DLL.";
            return false;
        }
    }
    return true;
}

bool StageRuntimeFiles(const PlayerPackageRequest& request, const PackagePlan& plan,
                       std::string& error) {
    std::error_code filesystemError;
    std::filesystem::create_directories(plan.staging / L"project", filesystemError);
    if (filesystemError) {
        error = "Could not create the Player package directory.";
        return false;
    }
    std::filesystem::copy_file(
        request.executable, plan.staging / L"Game.exe",
        std::filesystem::copy_options::overwrite_existing, filesystemError);
    if (filesystemError) {
        error = "Could not copy the Player executable.";
        return false;
    }
    const std::filesystem::path executableDirectory = request.executable.parent_path();
    if (!CopyRuntimeDlls(executableDirectory, plan.staging, error)) {
        return false;
    }
    if (!CopyDirectory(executableDirectory / L"resources", plan.staging / L"resources",
                       filesystemError)) {
        error = "Could not copy Player resources.";
        return false;
    }
    return true;
}

bool StageProjectDirectories(const PlayerPackageRequest& request,
                             const std::filesystem::path& packagedProject,
                             std::string& error) {
    std::error_code filesystemError;
    std::filesystem::copy_file(
        request.manifest, packagedProject / request.manifest.filename(),
        std::filesystem::copy_options::overwrite_existing, filesystemError);
    if (filesystemError ||
        !CopyProjectAssets(request.assetRoot, packagedProject / L"assets", filesystemError) ||
        !CopyDirectory(request.sceneRoot, packagedProject / L"scenes", filesystemError)) {
        error = "Could not copy the project content.";
        return false;
    }
    const std::filesystem::path settings = request.projectRoot / L"settings";
    if (std::filesystem::is_directory(settings, filesystemError) &&
        !CopyDirectory(settings, packagedProject / L"settings", filesystemError)) {
        error = "Could not copy Project Settings.";
        return false;
    }
    return true;
}

bool StageProjectScriptAssembly(const PlayerPackageRequest& request,
                                const std::filesystem::path& packagedProject,
                                std::string& error) {
    std::error_code filesystemError;
    const std::filesystem::path scriptAssembly =
        request.projectRoot / L"library" / L"ScriptAssemblies" / L"x64" /
        std::filesystem::path(request.configuration) / L"ProjectScripts.dll";
    if (!std::filesystem::is_regular_file(scriptAssembly, filesystemError) ||
        filesystemError) {
        error = "Project Script DLL is missing.";
        return false;
    }
    const std::filesystem::path packagedAssembly =
        packagedProject / L"library" / L"ScriptAssemblies" / L"x64" /
        std::filesystem::path(request.configuration);
    std::filesystem::create_directories(packagedAssembly, filesystemError);
    std::filesystem::copy_file(
        scriptAssembly, packagedAssembly / L"ProjectScripts.dll",
        std::filesystem::copy_options::overwrite_existing, filesystemError);
    if (filesystemError) {
        error = "Could not copy the Project Script DLL.";
        return false;
    }
    return true;
}

bool WritePackageMarker(const PackagePlan& plan, std::string& error) {
    std::ofstream marker(plan.staging / kPackageMarker, std::ios::trunc);
    marker << "LikeEngine Player Package\n";
    marker.close();
    if (!marker) {
        error = "Could not mark the Player package.";
        return false;
    }
    return true;
}
}  // namespace

bool StagePlayerPackage(const PlayerPackageRequest& request, const PackagePlan& plan,
                        std::string& error) {
    if (!StageRuntimeFiles(request, plan, error)) {
        return false;
    }
    const std::filesystem::path packagedProject = plan.staging / L"project";
    return StageProjectDirectories(request, packagedProject, error) &&
           StageProjectScriptAssembly(request, packagedProject, error) &&
           WritePackageMarker(plan, error);
}
}  // namespace PlayerPackageBuilderInternal
