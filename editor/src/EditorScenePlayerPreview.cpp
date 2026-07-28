#include "EditorScene.h"

#include "PlayerProjectValidator.h"
#include "ProjectDescriptor.h"

#include <Windows.h>

#include <array>

bool EditorScene::LaunchPlayerPreview() {
    if (!CanLaunchPlayerPreview() || !ValidatePlayerPreviewProject()) {
        return false;
    }
    std::filesystem::path executable;
    if (!TryLocateEditorExecutable(executable) || !StartPlayerPreviewProcess(executable)) {
        return false;
    }
    status_ = "Launched Player Preview.";
    return true;
}

bool EditorScene::CanLaunchPlayerPreview() {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before running the Player Preview.";
        return false;
    }
    if (dirty_) {
        status_ = "Save the scene before running the Player Preview.";
        return false;
    }
    if (playerSettingsDirty_ || physicsSettingsDirty_ || inputSettingsDirty_) {
        status_ = "Save Project Settings before running the Player Preview.";
        return false;
    }
    if (scriptBuildInProgress_ || scriptBuildPending_) {
        status_ = "Wait for Project Script compilation before running the Player Preview.";
        return false;
    }
    return true;
}

bool EditorScene::ValidatePlayerPreviewProject() {
    ProjectDescriptor project;
    std::string error;
    if (!ProjectDescriptor::Load(projectRoot_, project, error) ||
        !PlayerProjectValidator::Validate(project, error)) {
        status_ = "Could not run Player Preview: " + error;
        return false;
    }
    return true;
}

bool EditorScene::TryLocateEditorExecutable(std::filesystem::path& executable) {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0u || length >= buffer.size()) {
        status_ = "Could not locate the Editor executable.";
        return false;
    }
    executable = std::filesystem::path(std::wstring(buffer.data(), length));
    return true;
}

bool EditorScene::StartPlayerPreviewProcess(const std::filesystem::path& executable) {
    std::wstring command =
        L"\"" + executable.wstring() + L"\" --player --project \"" + projectRoot_.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0u,
                        nullptr, projectRoot_.c_str(), &startup, &process)) {
        status_ = "Could not launch the Player Preview.";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
