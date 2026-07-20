#include "ProjectLauncher.h"

#include "ProjectDescriptor.h"

#include <Windows.h>
#include <commctrl.h>
#include <shobjidl.h>

#include <string>

#pragma comment(linker,                                                                                \
                "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' "        \
                "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "       \
                "language='*'\"")

namespace {
constexpr int kBrowseButtonId = 2000;
constexpr int kCreateButtonId = 2001;
constexpr int kRecentButtonBase = 3000;

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

std::optional<std::filesystem::path> BrowseForProject() {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        if (SUCCEEDED(comResult)) {
            CoUninitialize();
        }
        return std::nullopt;
    }
    const COMDLG_FILTERSPEC filters[] = {{L"LikeEngine Project", L"*.likeproject"}};
    dialog->SetFileTypes(1u, filters);
    dialog->SetDefaultExtension(L"likeproject");
    dialog->SetTitle(L"Open LikeEngine Project");

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

std::optional<std::filesystem::path> BrowseForNewProjectDirectory() {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        if (SUCCEEDED(comResult)) {
            CoUninitialize();
        }
        return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Select a New or Empty Project Directory");

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

std::string PathNameToUtf8(const std::filesystem::path& path) {
    const std::wstring value = path.filename().wstring();
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

std::optional<std::filesystem::path> CreateProject() {
    const std::optional<std::filesystem::path> directory = BrowseForNewProjectDirectory();
    if (!directory) {
        return std::nullopt;
    }
    ProjectDescriptor descriptor;
    std::string error;
    if (!ProjectDescriptor::Create(*directory, PathNameToUtf8(*directory), descriptor, error)) {
        MessageBoxA(nullptr, error.c_str(), "LikeEngine Project Manager", MB_OK | MB_ICONERROR);
        return std::nullopt;
    }
    return descriptor.manifestPath;
}
} // namespace

std::optional<std::filesystem::path>
ProjectLauncher::ChooseProject(const std::vector<RecentProject>& recentProjects) {
    std::vector<std::wstring> labels;
    std::vector<TASKDIALOG_BUTTON> buttons;
    labels.reserve(recentProjects.size() + 2u);
    buttons.reserve(recentProjects.size() + 2u);
    for (size_t index = 0u; index < recentProjects.size(); ++index) {
        labels.push_back(Utf8ToWide(recentProjects[index].name) + L"\n" +
                         recentProjects[index].manifestPath.wstring());
        buttons.push_back(
            {kRecentButtonBase + static_cast<int>(index), labels.back().c_str()});
    }
    labels.push_back(L"Create New Project...\nCreate a project in a new or empty directory");
    buttons.push_back({kCreateButtonId, labels.back().c_str()});
    labels.push_back(L"Browse...\nOpen another .likeproject file");
    buttons.push_back({kBrowseButtonId, labels.back().c_str()});

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION |
                     TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"LikeEngine Project Manager";
    config.pszMainInstruction = L"Open a project";
    config.pszContent = recentProjects.empty() ? L"No recent projects were found."
                                                : L"Recent projects";
    config.cButtons = static_cast<UINT>(buttons.size());
    config.pButtons = buttons.data();
    config.nDefaultButton = recentProjects.empty() ? kCreateButtonId : kRecentButtonBase;

    for (;;) {
        int selectedButton = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&config, &selectedButton, nullptr, nullptr))) {
            return BrowseForProject();
        }
        if (selectedButton == kBrowseButtonId) {
            return BrowseForProject();
        }
        if (selectedButton == kCreateButtonId) {
            if (std::optional<std::filesystem::path> created = CreateProject()) {
                return created;
            }
            continue;
        }
        const int recentIndex = selectedButton - kRecentButtonBase;
        if (recentIndex >= 0 && static_cast<size_t>(recentIndex) < recentProjects.size()) {
            return recentProjects[static_cast<size_t>(recentIndex)].manifestPath;
        }
        return std::nullopt;
    }
}
