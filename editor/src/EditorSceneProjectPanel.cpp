#include "EditorScene.h"

#include "imgui.h"
#include "internal/EditorSceneAssetUtils.h"

#include <array>
#include <iterator>
#include <ranges>
#include <system_error>

using namespace EditorSceneAssetUtils;

namespace {
constexpr std::array<const char*, 26> kFormatLabels = {
    "All formats", "Prefab", "C++ Script", "glTF", "GLB", "OBJ", "FBX", "DAE",
    "3DS",         "PLY",    "PNG",        "JPG",  "JPEG", "TGA", "BMP", "DDS",
    "HDR",         "EXR",    "WAV",        "MP3",  "AAC",  "M4A", "WMA", "TTF",
    "OTF",
};

constexpr std::array<const char*, 26> kFormatExtensions = {
    "",     ".likeprefab", ".cpp", ".gltf", ".glb", ".obj", ".fbx", ".dae", ".3ds",
    ".ply", ".png",        ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".hdr", ".exr",
    ".wav", ".mp3",        ".aac", ".m4a", ".wma", ".ttf", ".otf",
};

constexpr std::array<const char*, 3> kSortLabels = {"Name", "Type", "Size"};
}  // namespace

void EditorScene::DrawProjectPanel() {
    UpdateProjectPanelState();
    DrawProjectPanelToolbar();
    ImGui::Separator();
    DrawAssetBrowserLocation();
    DrawAssetBrowserFilters();
    DrawAssetBrowserEntries();
    if (!selectedAsset_.empty()) {
        DrawSelectedAssetDetails();
    }
}

void EditorScene::UpdateProjectPanelState() {
    const ImVec2 panelPosition = ImGui::GetWindowPos();
    const ImVec2 panelSize = ImGui::GetWindowSize();
    projectPanelMinX_ = panelPosition.x;
    projectPanelMinY_ = panelPosition.y;
    projectPanelMaxX_ = panelPosition.x + panelSize.x;
    projectPanelMaxY_ = panelPosition.y + panelSize.y;
    if (!pendingAssetDirectory_) {
        return;
    }
    currentAssetDirectory_ = std::move(*pendingAssetDirectory_);
    pendingAssetDirectory_.reset();
    selectedAsset_.clear();
    RefreshAssetBrowser();
}

void EditorScene::DrawProjectPanelToolbar() {
    if (ImGui::Button("New")) {
        ImGui::OpenPopup("AssetCreateMenu");
    }
    DrawAssetCreateMenu();
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        RefreshAssetBrowser();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "%zu model(s), %zu texture(s), %zu audio(s), %zu font(s), %zu script(s), "
        "%zu prefab(s)",
        modelAssets_.size(), textureAssets_.size(), audioAssets_.size(), fontAssets_.size(),
        scriptAssets_.size(), prefabAssets_.size());
}

void EditorScene::DrawAssetCreateMenu() {
    if (!ImGui::BeginPopup("AssetCreateMenu")) {
        return;
    }
    if (ImGui::MenuItem("Folder")) {
        RequestCreateAssetFolder();
    }
    if (ImGui::MenuItem("Prefab from Selection...", nullptr, false,
                        selection_.IsValid() && !IsInPlayMode())) {
        SaveSelectionAsPrefab();
    }
    if (ImGui::MenuItem("Import Assets...")) {
        ImportAssetFiles();
    }
    ImGui::EndPopup();
}

void EditorScene::DrawAssetBrowserLocation() {
    if (!currentAssetDirectory_.empty()) {
        if (ImGui::Button("< Back")) {
            NavigateAssetBrowser(currentAssetDirectory_.parent_path());
        }
        ImGui::SameLine();
    }
    DrawAssetBrowserBreadcrumbs();
}

void EditorScene::DrawAssetBrowserBreadcrumbs() {
    if (ImGui::SmallButton("assets")) {
        NavigateAssetBrowser({});
    }
    std::filesystem::path accumulated;
    for (const std::filesystem::path& component : currentAssetDirectory_) {
        if (component == L".") {
            continue;
        }
        accumulated /= component;
        ImGui::SameLine(0.0f, 3.0f);
        ImGui::TextUnformatted(">");
        ImGui::SameLine(0.0f, 3.0f);
        const std::string label = component.string();
        const std::string id = accumulated.generic_string();
        ImGui::PushID(id.c_str());
        if (ImGui::SmallButton(label.c_str())) {
            NavigateAssetBrowser(accumulated);
        }
        ImGui::PopID();
    }
}

void EditorScene::DrawAssetBrowserFilters() {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##AssetSearch", "Search assets...", assetSearch_.data(),
                             assetSearch_.size());
    ImGui::SetNextItemWidth(105.0f);
    ImGui::Combo("##AssetFormat", &assetFormatFilter_, kFormatLabels.data(),
                 static_cast<int>(kFormatLabels.size()));
    ImGui::SameLine();
    int sortMode = static_cast<int>(assetSortMode_);
    ImGui::SetNextItemWidth(75.0f);
    if (ImGui::Combo("##AssetSort", &sortMode, kSortLabels.data(),
                     static_cast<int>(kSortLabels.size()))) {
        assetSortMode_ = static_cast<AssetSortMode>(sortMode);
    }
    ImGui::SameLine();
    if (ImGui::Button(assetSortAscending_ ? "Asc" : "Desc")) {
        assetSortAscending_ = !assetSortAscending_;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Toggle sort direction");
    }
    ImGui::Separator();
}

void EditorScene::DrawAssetBrowserEntries() {
    const float detailsHeight = selectedAsset_.empty() ? 0.0f : 190.0f;
    if (ImGui::BeginChild("AssetBrowserEntries", {0.0f, -detailsHeight},
                          ImGuiChildFlags_None)) {
        const std::string search(assetSearch_.data());
        search.empty() ? DrawAssetDirectoryEntries() : DrawAssetSearchResults(search);
    }
    ImGui::EndChild();
}

void EditorScene::DrawAssetSearchResults(const std::string& search) {
    std::vector<std::filesystem::path> matches;
    AppendAssetSearchMatches(matches, modelAssets_, search);
    AppendAssetSearchMatches(matches, textureAssets_, search);
    AppendAssetSearchMatches(matches, audioAssets_, search);
    AppendAssetSearchMatches(matches, fontAssets_, search);
    AppendAssetSearchMatches(matches, scriptAssets_, search);
    AppendAssetSearchMatches(matches, prefabAssets_, search);
    std::ranges::sort(matches, [this](const auto& left, const auto& right) {
        return CompareAssetPaths(left, right);
    });
    for (const std::filesystem::path& relativePath : matches) {
        DrawAssetBrowserEntry(relativePath, false);
    }
    if (matches.empty()) {
        ImGui::TextDisabled("No matching assets.");
    }
}

void EditorScene::DrawAssetDirectoryEntries() {
    std::vector<AssetBrowserEntry> visibleEntries;
    std::ranges::copy_if(assetBrowserEntries_, std::back_inserter(visibleEntries),
                         [this](const AssetBrowserEntry& entry) {
                             return entry.directory || MatchesAssetFormat(entry.relativePath);
                         });
    std::ranges::sort(visibleEntries, [this](const AssetBrowserEntry& left,
                                             const AssetBrowserEntry& right) {
        return left.directory != right.directory
                   ? left.directory
                   : CompareAssetPaths(left.relativePath, right.relativePath);
    });
    for (const AssetBrowserEntry& entry : visibleEntries) {
        DrawAssetBrowserEntry(entry.relativePath, entry.directory);
    }
    if (visibleEntries.empty()) {
        ImGui::TextDisabled("This folder contains no matching assets or folders.");
    }
}

void EditorScene::AppendAssetSearchMatches(
    std::vector<std::filesystem::path>& matches,
    const std::vector<std::filesystem::path>& assets, const std::string& search) const {
    for (const std::filesystem::path& logicalPath : assets) {
        const std::filesystem::path relativePath = logicalPath.lexically_relative("assets");
        if (ContainsCaseInsensitive(logicalPath.generic_string(), search) &&
            MatchesAssetFormat(relativePath)) {
            matches.push_back(relativePath);
        }
    }
}

bool EditorScene::MatchesAssetFormat(const std::filesystem::path& relativePath) const {
    if (assetFormatFilter_ <= 0 ||
        assetFormatFilter_ >= static_cast<int>(kFormatExtensions.size())) {
        return true;
    }
    return LowercaseAscii(relativePath.extension().string()) ==
           kFormatExtensions[assetFormatFilter_];
}

uintmax_t EditorScene::GetAssetFileSize(const std::filesystem::path& relativePath) const {
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(assetRoot_ / relativePath, error);
    return error ? uintmax_t{0} : size;
}

bool EditorScene::CompareAssetPaths(const std::filesystem::path& left,
                                    const std::filesystem::path& right) const {
    int result = 0;
    if (assetSortMode_ == AssetSortMode::Type) {
        result = LowercaseAscii(left.extension().string())
                     .compare(LowercaseAscii(right.extension().string()));
    } else if (assetSortMode_ == AssetSortMode::Size) {
        const uintmax_t leftSize = GetAssetFileSize(left);
        const uintmax_t rightSize = GetAssetFileSize(right);
        result = leftSize < rightSize ? -1 : (leftSize > rightSize ? 1 : 0);
    }
    if (result == 0) {
        result = LowercaseAscii(left.filename().string())
                     .compare(LowercaseAscii(right.filename().string()));
    }
    return assetSortAscending_ ? result < 0 : result > 0;
}
