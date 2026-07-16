#include "ApplicationPaths.h"
#include "EditorScene.h"
#include "ProjectDescriptor.h"

#include "core/AssetManager.h"
#include "core/EngineRuntime.h"

#include <Windows.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {
std::optional<std::filesystem::path> OpenProjectDialog() {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        if (SUCCEEDED(comResult)) {
            CoUninitialize();
        }
        return std::nullopt;
    }
    const COMDLG_FILTERSPEC filters[] = {{L"WP0 Project", L"*.wp0project"}};
    dialog->SetFileTypes(1u, filters);
    dialog->SetDefaultExtension(L"wp0project");
    dialog->SetTitle(L"Open WP0 Project");

    std::optional<std::filesystem::path> selected;
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                selected = std::filesystem::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return selected;
}

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

std::optional<std::filesystem::path> FindDevelopmentProject(const ApplicationPaths& paths) {
    const auto candidate = paths.engineResources.parent_path().parent_path() / L"projects" /
                           L"sandbox" / L"sandbox.wp0project";
    std::error_code error;
    return std::filesystem::is_regular_file(candidate, error) && !error
               ? std::optional<std::filesystem::path>(candidate)
               : std::nullopt;
}

void ShowError(const std::string& message) {
    MessageBoxA(nullptr, message.c_str(), "WP0 Editor", MB_OK | MB_ICONERROR);
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

    const ApplicationPaths paths = ApplicationPaths::Discover();
    std::optional<std::filesystem::path> projectPath = ParseProjectArgument();
    if (!projectPath) {
        projectPath = FindDevelopmentProject(paths);
    }
    if (!projectPath) {
        projectPath = OpenProjectDialog();
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

    AssetManager::SetEngineResourceRoot(paths.engineResources);
    AssetManager::SetProjectAssetRoot(project.assetRoot);
    AssetManager::SetUserDataRoot(paths.userData);

    EngineRuntimeConfig config{};
    config.width = 1600;
    config.height = 900;
    config.title = L"WP0 Editor - " + Utf8ToWide(project.name);
    config.cursorVisible = true;
    config.logPath = (paths.userData / L"logs" / L"editor.log").wstring();

    EngineRuntime runtime;
    if (!runtime.Initialize(instance, showCommand, config)) {
        return -1;
    }
    if (!runtime.SetScene(std::make_unique<EditorScene>(
            project.root, project.assetRoot, project.startupScene,
            [&runtime]() { runtime.RequestClose(); }))) {
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
