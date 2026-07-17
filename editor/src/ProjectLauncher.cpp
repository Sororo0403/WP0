#include "ProjectLauncher.h"

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
} // namespace

std::optional<std::filesystem::path>
ProjectLauncher::ChooseProject(const std::vector<RecentProject>& recentProjects) {
    std::vector<std::wstring> labels;
    std::vector<TASKDIALOG_BUTTON> buttons;
    labels.reserve(recentProjects.size() + 1u);
    buttons.reserve(recentProjects.size() + 1u);
    for (size_t index = 0u; index < recentProjects.size(); ++index) {
        labels.push_back(Utf8ToWide(recentProjects[index].name) + L"\n" +
                         recentProjects[index].manifestPath.wstring());
        buttons.push_back(
            {kRecentButtonBase + static_cast<int>(index), labels.back().c_str()});
    }
    labels.push_back(L"Browse...\nOpen another .wp0project file");
    buttons.push_back({kBrowseButtonId, labels.back().c_str()});

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION |
                     TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"WP0 Project Manager";
    config.pszMainInstruction = L"Open a project";
    config.pszContent = recentProjects.empty() ? L"No recent projects were found."
                                                : L"Recent projects";
    config.cButtons = static_cast<UINT>(buttons.size());
    config.pButtons = buttons.data();
    config.nDefaultButton = recentProjects.empty() ? kBrowseButtonId : kRecentButtonBase;

    int selectedButton = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&config, &selectedButton, nullptr, nullptr))) {
        return BrowseForProject();
    }
    if (selectedButton == kBrowseButtonId) {
        return BrowseForProject();
    }
    const int recentIndex = selectedButton - kRecentButtonBase;
    if (recentIndex >= 0 && static_cast<size_t>(recentIndex) < recentProjects.size()) {
        return recentProjects[static_cast<size_t>(recentIndex)].manifestPath;
    }
    return std::nullopt;
}
