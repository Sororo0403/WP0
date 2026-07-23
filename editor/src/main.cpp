#include "ApplicationPaths.h"
#include "EditorScene.h"
#include "ProjectDescriptor.h"
#include "ProjectLauncher.h"
#include "RecentProjectsStore.h"

#include "core/AssetManager.h"
#include "core/EngineRuntime.h"

#include <Windows.h>
#include <shellapi.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {
std::optional<std::filesystem::path> ParseProjectArgument() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) {
        return std::nullopt;
    }
    std::optional<std::filesystem::path> result;
    for (int index = 1; index < count; ++index) {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--project" && index + 1 < count) {
            result = std::filesystem::path(arguments[++index]);
            break;
        }
        if (!argument.starts_with(L"-")) {
            result = std::filesystem::path(argument);
            break;
        }
    }
    LocalFree(arguments);
    return result;
}

bool HasCommandLineFlag(std::wstring_view flag) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) {
        return false;
    }
    bool found = false;
    for (int index = 1; index < count; ++index) {
        if (std::wstring_view(arguments[index]) == flag) {
            found = true;
            break;
        }
    }
    LocalFree(arguments);
    return found;
}

void ShowError(const std::string& message) {
    MessageBoxA(nullptr, message.c_str(), "LikeEngine Editor", MB_OK | MB_ICONERROR);
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return L"Project";
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), length);
    return result;
}
} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance, LPSTR commandLine,
                   int showCommand) {
    (void)previousInstance;
    (void)commandLine;

    bool playerMode = HasCommandLineFlag(L"--player");
    const ApplicationPaths paths = ApplicationPaths::Discover();
    const RecentProjectsStore recentProjects(paths.userData / L"settings" /
                                              L"recent_projects.json");
    std::optional<std::filesystem::path> projectPath = ParseProjectArgument();
    const std::filesystem::path bundledProject =
        paths.installRoot / L"project";
    std::error_code bundledProjectError;
    if (!projectPath &&
        std::filesystem::is_directory(bundledProject, bundledProjectError) &&
        !bundledProjectError) {
        projectPath = bundledProject;
        playerMode = true;
    }
    if (!projectPath) {
        projectPath = ProjectLauncher::ChooseProject(recentProjects.Load());
    }
    if (!projectPath) {
        return 0;
    }

    ProjectDescriptor project;
    std::string projectError;
    if (!ProjectDescriptor::Load(*projectPath, project, projectError)) {
        ShowError(projectError);
        return -1;
    }
    recentProjects.Record({project.name, project.manifestPath});

    AssetManager::SetEngineResourceRoot(paths.engineResources);
    AssetManager::SetProjectAssetRoot(project.assetRoot);
    AssetManager::SetUserDataRoot(paths.userData);

    EngineRuntimeConfig config{};
    config.width = playerMode ? 1280 : 1600;
    config.height = playerMode ? 720 : 900;
    config.title = playerMode ? Utf8ToWide(project.name)
                              : L"LikeEngine Editor - " + Utf8ToWide(project.name);
    config.cursorVisible = true;
    config.logPath =
        (paths.userData / L"logs" /
         (playerMode ? L"player.log" : L"editor.log"))
            .wstring();

    EngineRuntime runtime;
    if (!runtime.Initialize(instance, showCommand, config)) {
        return -1;
    }
    if (!runtime.SetScene(std::make_unique<EditorScene>(
            project.root, project.assetRoot, project.sceneRoot, project.startupScene,
            paths.userData / L"settings" / L"recent_scenes" /
                (Utf8ToWide(project.projectId) + L".json"),
            paths.userData / L"settings" / L"imgui_layout.ini",
            [&runtime]() { runtime.RequestClose(); }, playerMode))) {
        return -1;
    }

    for (;;) {
        const EngineFrameResult result = runtime.Tick();
        if (result == EngineFrameResult::ExitRequested) {
            return 0;
        }
        if (result == EngineFrameResult::Failed) {
            return -1;
        }
    }
}
