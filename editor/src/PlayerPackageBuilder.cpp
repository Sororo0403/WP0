#include "PlayerPackageBuilder.h"

#include <system_error>

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

bool IsDirectChild(const std::filesystem::path& parent,
                   const std::filesystem::path& child) {
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(child, parent, error);
    return !error && !relative.empty() && !relative.is_absolute() &&
           *relative.begin() != L"..";
}
} // namespace

bool PlayerPackageBuilder::Build(const PlayerPackageRequest& request,
                                 std::string& error) {
    error.clear();
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(request.executable, filesystemError) ||
        filesystemError ||
        !std::filesystem::is_regular_file(request.manifest, filesystemError) ||
        filesystemError ||
        (request.configuration != "Debug" &&
         request.configuration != "Release") ||
        request.destination.empty()) {
        error = "Player package request is invalid.";
        return false;
    }
    const std::filesystem::path destination =
        std::filesystem::absolute(request.destination, filesystemError)
            .lexically_normal();
    if (filesystemError || destination == destination.root_path() ||
        destination == request.projectRoot.lexically_normal() ||
        !IsDirectChild(destination.parent_path(), destination)) {
        error = "Player package destination is unsafe.";
        return false;
    }
    if (std::filesystem::exists(destination, filesystemError) ||
        filesystemError) {
        error = "Player package destination already exists.";
        return false;
    }
    const std::filesystem::path staging =
        destination.wstring() + L".building";
    if (std::filesystem::exists(staging, filesystemError) || filesystemError) {
        error = "Player package staging directory already exists.";
        return false;
    }

    const auto fail = [&](const char* message) {
        std::error_code cleanupError;
        std::filesystem::remove_all(staging, cleanupError);
        error = message;
        return false;
    };
    std::filesystem::create_directories(staging / L"project", filesystemError);
    if (filesystemError) {
        return fail("Could not create the Player package directory.");
    }
    std::filesystem::copy_file(request.executable, staging / L"Game.exe",
                               std::filesystem::copy_options::overwrite_existing,
                               filesystemError);
    if (filesystemError) {
        return fail("Could not copy the Player executable.");
    }
    const std::filesystem::path executableDirectory =
        request.executable.parent_path();
    for (const auto& entry :
         std::filesystem::directory_iterator(executableDirectory,
                                             filesystemError)) {
        if (filesystemError) {
            return fail("Could not enumerate Player runtime files.");
        }
        if (entry.is_regular_file(filesystemError) &&
            entry.path().extension() == L".dll") {
            std::filesystem::copy_file(
                entry.path(), staging / entry.path().filename(),
                std::filesystem::copy_options::overwrite_existing,
                filesystemError);
            if (filesystemError) {
                return fail("Could not copy a Player runtime DLL.");
            }
        }
    }
    if (!CopyDirectory(executableDirectory / L"resources",
                       staging / L"resources", filesystemError)) {
        return fail("Could not copy Player resources.");
    }

    const std::filesystem::path packagedProject = staging / L"project";
    std::filesystem::copy_file(
        request.manifest, packagedProject / request.manifest.filename(),
        std::filesystem::copy_options::overwrite_existing, filesystemError);
    if (filesystemError ||
        !CopyDirectory(request.assetRoot, packagedProject / L"assets",
                       filesystemError) ||
        !CopyDirectory(request.sceneRoot, packagedProject / L"scenes",
                       filesystemError)) {
        return fail("Could not copy the project content.");
    }
    const std::filesystem::path settings = request.projectRoot / L"settings";
    if (std::filesystem::is_directory(settings, filesystemError) &&
        !CopyDirectory(settings, packagedProject / L"settings",
                       filesystemError)) {
        return fail("Could not copy Project Settings.");
    }
    filesystemError.clear();
    const std::filesystem::path scriptAssembly =
        request.projectRoot / L"library" / L"ScriptAssemblies" / L"x64" /
        std::filesystem::path(request.configuration) / L"ProjectScripts.dll";
    if (!std::filesystem::is_regular_file(scriptAssembly, filesystemError) ||
        filesystemError) {
        return fail("Project Script DLL is missing.");
    }
    const std::filesystem::path packagedAssembly =
        packagedProject / L"library" / L"ScriptAssemblies" / L"x64" /
        std::filesystem::path(request.configuration);
    std::filesystem::create_directories(packagedAssembly, filesystemError);
    std::filesystem::copy_file(
        scriptAssembly, packagedAssembly / L"ProjectScripts.dll",
        std::filesystem::copy_options::overwrite_existing, filesystemError);
    if (filesystemError) {
        return fail("Could not copy the Project Script DLL.");
    }
    std::filesystem::rename(staging, destination, filesystemError);
    if (filesystemError) {
        return fail("Could not finish the Player package.");
    }
    return true;
}
