#include "EditorScene.h"

#include "PlayerPackageBuilder.h"
#include "PlayerProjectValidator.h"
#include "ProjectDescriptor.h"

#include <Windows.h>

#include <array>

bool EditorScene::BuildPlayerPackage(std::filesystem::path* destination) {
    if (!CanBuildPlayerPackage()) {
        return false;
    }
    ProjectDescriptor project;
    std::string error;
    if (!TryLoadPlayerBuildProject(project, error)) {
        return false;
    }
    std::filesystem::path executable;
    if (!TryLocatePlayerExecutable(executable)) {
        return false;
    }
    const PlayerPackageRequest request = CreatePlayerPackageRequest(project, executable);
    if (!PlayerPackageBuilder::Build(request, error)) {
        status_ = "Could not build Player: " + error;
        return false;
    }
    if (destination != nullptr) {
        *destination = request.destination;
    }
    SetPlayerPackageBuildStatus(request.destination, error);
    return true;
}

bool EditorScene::CanBuildPlayerPackage() {
    if (IsInPlayMode() || dirty_ || playerSettingsDirty_ || physicsSettingsDirty_ ||
        inputSettingsDirty_) {
        status_ = "Save the scene and Project Settings before building.";
        return false;
    }
    if (scriptBuildInProgress_ || scriptBuildPending_) {
        status_ = "Wait for Project Script compilation before building.";
        return false;
    }
    return true;
}

bool EditorScene::TryLoadPlayerBuildProject(ProjectDescriptor& project, std::string& error) {
    if (!ProjectDescriptor::Load(projectRoot_, project, error) ||
        !PlayerProjectValidator::Validate(project, error)) {
        status_ = "Could not build Player: " + error;
        return false;
    }
    return true;
}

bool EditorScene::TryLocatePlayerExecutable(std::filesystem::path& executable) {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0u || length >= buffer.size()) {
        status_ = "Could not locate the Player executable.";
        return false;
    }
    executable = std::filesystem::path(std::wstring(buffer.data(), length));
    return true;
}

PlayerPackageRequest EditorScene::CreatePlayerPackageRequest(
    const ProjectDescriptor& project, const std::filesystem::path& executable) const {
#ifdef _DEBUG
    constexpr char configuration[] = "Debug";
    constexpr wchar_t outputName[] = L"windows-x64-debug";
#else
    constexpr char configuration[] = "Release";
    constexpr wchar_t outputName[] = L"windows-x64";
#endif
    return {
        .executable = executable,
        .projectRoot = project.root,
        .manifest = project.manifestPath,
        .assetRoot = project.assetRoot,
        .sceneRoot = project.sceneRoot,
        .destination = project.root / L"build" / outputName,
        .configuration = configuration,
    };
}

void EditorScene::SetPlayerPackageBuildStatus(const std::filesystem::path& destination,
                                              const std::string& warning) {
    status_ = "Built Player package: " + destination.generic_string();
    if (!warning.empty()) {
        status_ += " Warning: " + warning;
    }
}
