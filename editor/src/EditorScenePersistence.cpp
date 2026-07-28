#include "EditorScene.h"

#include "AssetImportPlanner.h"
#include "world/WorldSerializer.h"

#include <Windows.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cwchar>

namespace {
constexpr size_t kMaxRecentScenes = 10;

bool HasParentTraversal(const std::filesystem::path& path) {
    return std::ranges::any_of(path, [](const std::filesystem::path& part) {
        return part == L"..";
    });
}

bool IsPathWithinRoot(const std::filesystem::path& root,
                      const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const std::filesystem::path canonicalPath =
        std::filesystem::weakly_canonical(path, error);
    if (error) {
        return false;
    }
    const std::filesystem::path relative =
        std::filesystem::relative(canonicalPath, canonicalRoot, error);
    return !error && !relative.empty() && !relative.is_absolute() &&
           !HasParentTraversal(relative);
}

bool IsPrefabAsset(const std::filesystem::path& path) {
    return _wcsicmp(path.extension().c_str(), L".likeprefab") == 0;
}
} // namespace
void EditorScene::RequestSceneAction(PendingSceneAction action,
                                     std::filesystem::path path) {
    if (action == PendingSceneAction::None) {
        return;
    }
    if (IsInPlayMode()) {
        if (action == PendingSceneAction::Exit) {
            StopPlayMode();
        } else {
            status_ = "Stop Play Mode before changing scenes.";
            return;
        }
    }
    if (!dirty_) {
        ExecuteSceneAction(action, path);
        return;
    }
    pendingSceneAction_ = action;
    pendingScenePath_ = std::move(path);
    showUnsavedChangesDialog_ = true;
}

void EditorScene::ExecuteSceneAction(PendingSceneAction action,
                                     const std::filesystem::path& path) {
    switch (action) {
    case PendingSceneAction::NewScene:
        NewScene(true);
        break;
    case PendingSceneAction::OpenScene:
        if (!path.empty()) {
            LoadScene(path);
        } else if (const std::optional<std::filesystem::path> selected = ShowOpenSceneDialog()) {
            LoadScene(*selected);
        }
        break;
    case PendingSceneAction::ReloadScene:
        if (!scenePath_.empty()) {
            LoadScene(scenePath_);
        }
        break;
    case PendingSceneAction::Exit:
        if (requestClose_) {
            requestClose_();
        }
        break;
    case PendingSceneAction::None:
        break;
    }
}

void EditorScene::NewScene(bool clearPath) {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before creating a scene.";
        return;
    }
    world_.Clear();
    world_.SetPhysicsSettings(physicsSettings_);
    const EntityId camera = world_.CreateEntity("Main Camera");
    if (WorldEntity* cameraEntity = world_.Find(camera)) {
        cameraEntity->transform.position = {0.0f, 2.0f, -5.0f};
        cameraEntity->camera = CameraComponent{};
        cameraEntity->camera->primary = true;
        cameraEntity->audioListener = AudioListenerComponent{};
    }
    const EntityId light = world_.CreateEntity("Directional Light");
    if (WorldEntity* lightEntity = world_.Find(light)) {
        lightEntity->transform.rotationDegrees = {50.0f, -30.0f, 0.0f};
        lightEntity->light = LightComponent{};
    }
    selection_ = world_.CreateEntity("Cube");
    if (WorldEntity* cube = world_.Find(selection_)) {
        cube->meshRenderer = MeshRendererComponent{};
        cube->materialOverride = MaterialOverrideComponent{};
    }
    if (clearPath) {
        scenePath_.clear();
    }
    ClearHistory(false);
    status_ = "Created a new scene.";
}

bool EditorScene::SaveScene() {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before saving the scene.";
        return false;
    }
    if (scenePath_.empty()) {
        return SaveSceneAs();
    }
    std::string error;
    if (!WorldSerializer::Save(world_, scenePath_, &error)) {
        status_ = "Save failed: " + error;
        return false;
    }
    dirty_ = false;
    savedWorldSnapshot_ = WorldSerializer::Serialize(world_);
    AddRecentScene(scenePath_);
    status_ = "Saved scene: " + scenePath_.string();
    return true;
}

bool EditorScene::SaveSceneAs() {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before saving the scene.";
        return false;
    }
    const std::optional<std::filesystem::path> selected = ShowSaveSceneDialog();
    if (!selected) {
        status_ = "Save cancelled.";
        return false;
    }
    const std::filesystem::path previousPath = scenePath_;
    scenePath_ = *selected;
    if (SaveScene()) {
        return true;
    }
    scenePath_ = previousPath;
    return false;
}

bool EditorScene::LoadScene(const std::filesystem::path& path) {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before loading a scene.";
        return false;
    }
    if (path.extension() != L".likescene" || !IsPathWithinRoot(sceneRoot_, path)) {
        status_ = "Load failed: scene must be inside the project scenes directory.";
        return false;
    }
    World loaded;
    std::string error;
    if (!WorldSerializer::Load(path, loaded, &error)) {
        status_ = "Load failed: " + error;
        return false;
    }
    world_ = std::move(loaded);
    world_.SetPhysicsSettings(physicsSettings_);
    scenePath_ = path;
    selection_ = world_.Empty() ? EntityId{} : world_.Entities().front().id;
    dirty_ = false;
    ClearHistory(true);
    AddRecentScene(scenePath_);
    std::string behaviorRequirementError;
    if (ctx_ != nullptr &&
        !ValidateWorldBehaviorRequirements(&behaviorRequirementError)) {
        status_ = "Warning: Loaded scene with an invalid Behavior: " +
                  behaviorRequirementError;
    } else {
        status_ = "Loaded scene: " + scenePath_.string();
    }
    return true;
}

void EditorScene::AddRecentScene(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    std::error_code error;
    const std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return;
    }
    std::erase_if(recentScenePaths_, [&normalized](const std::filesystem::path& item) {
        return _wcsicmp(item.c_str(), normalized.c_str()) == 0;
    });
    recentScenePaths_.insert(recentScenePaths_.begin(), normalized);
    if (recentScenePaths_.size() > kMaxRecentScenes) {
        recentScenePaths_.resize(kMaxRecentScenes);
    }
    recentScenesStore_.Save(recentScenePaths_);
}

std::optional<std::filesystem::path> EditorScene::ShowOpenSceneDialog() const {
    std::array<wchar_t, 32768> buffer{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"LikeEngine Scene (*.likescene)\0*.likescene\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    const std::wstring initialDirectory = sceneRoot_.wstring();
    dialog.lpstrInitialDir = initialDirectory.c_str();
    dialog.lpstrDefExt = L"likescene";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&dialog)) {
        return std::nullopt;
    }
    const std::filesystem::path selected(buffer.data());
    return selected.extension() == L".likescene" && IsPathWithinRoot(sceneRoot_, selected)
               ? std::optional<std::filesystem::path>(selected)
               : std::nullopt;
}

std::optional<std::filesystem::path> EditorScene::ShowSaveSceneDialog() const {
    std::array<wchar_t, 32768> buffer{};
    if (!scenePath_.empty()) {
        const std::wstring filename = scenePath_.filename().wstring();
        wcsncpy_s(buffer.data(), buffer.size(), filename.c_str(), _TRUNCATE);
    } else {
        wcsncpy_s(buffer.data(), buffer.size(), L"untitled.likescene", _TRUNCATE);
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"LikeEngine Scene (*.likescene)\0*.likescene\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    const std::wstring initialDirectory = sceneRoot_.wstring();
    dialog.lpstrInitialDir = initialDirectory.c_str();
    dialog.lpstrDefExt = L"likescene";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   OFN_DONTADDTORECENT;
    if (!GetSaveFileNameW(&dialog)) {
        return std::nullopt;
    }
    const std::filesystem::path selected(buffer.data());
    return selected.extension() == L".likescene" && IsPathWithinRoot(sceneRoot_, selected)
               ? std::optional<std::filesystem::path>(selected)
               : std::nullopt;
}

std::optional<std::filesystem::path> EditorScene::ShowSavePrefabDialog(
    std::string_view entityName) const {
    std::wstring filename = L"Prefab";
    if (!entityName.empty()) {
        const int length = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, entityName.data(),
            static_cast<int>(entityName.size()), nullptr, 0);
        if (length > 0) {
            filename.resize(static_cast<size_t>(length));
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, entityName.data(),
                                static_cast<int>(entityName.size()), filename.data(), length);
        }
    }
    for (wchar_t& character : filename) {
        if (character < L' ' || wcschr(L"\\/:*?\"<>|", character) != nullptr) {
            character = L'_';
        }
    }
    while (!filename.empty() && (filename.back() == L' ' || filename.back() == L'.')) {
        filename.pop_back();
    }
    if (filename.empty()) {
        filename = L"Prefab";
    }
    filename += L".likeprefab";

    std::array<wchar_t, 32768> buffer{};
    wcsncpy_s(buffer.data(), buffer.size(), filename.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"LikeEngine Prefab (*.likeprefab)\0*.likeprefab\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    const std::filesystem::path initialPath = assetRoot_ / currentAssetDirectory_;
    const std::wstring initialDirectory = initialPath.wstring();
    dialog.lpstrInitialDir = initialDirectory.c_str();
    dialog.lpstrDefExt = L"likeprefab";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   OFN_DONTADDTORECENT;
    if (!GetSaveFileNameW(&dialog)) {
        return std::nullopt;
    }
    const std::filesystem::path selected(buffer.data());
    return IsPrefabAsset(selected) && IsPathWithinRoot(assetRoot_, selected)
               ? std::optional<std::filesystem::path>(selected)
               : std::nullopt;
}

std::vector<std::filesystem::path> EditorScene::ShowImportAssetDialog() const {
    std::array<wchar_t, 32768> buffer{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter =
        L"Model, Texture, Audio, and Font Assets\0"
        L"*.fbx;*.obj;*.gltf;*.glb;*.dae;*.3ds;*.ply;*.bin;*.mtl;*.png;*.jpg;*.jpeg;"
        L"*.tga;*.bmp;*.dds;*.hdr;*.exr;*.wav;*.mp3;*.aac;*.m4a;*.wma;*.ttf;*.otf\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   OFN_DONTADDTORECENT | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) {
        return {};
    }

    const std::filesystem::path first(buffer.data());
    const wchar_t* next = buffer.data() + first.native().size() + 1u;
    if (*next == L'\0') {
        return AssetImport::IsSelectableFile(first)
                   ? std::vector<std::filesystem::path>{first}
                   : std::vector<std::filesystem::path>{};
    }

    std::vector<std::filesystem::path> selectedFiles;
    while (*next != L'\0') {
        const std::filesystem::path filename(next);
        const std::filesystem::path selected = first / filename;
        if (!AssetImport::IsSelectableFile(selected)) {
            return {};
        }
        selectedFiles.push_back(selected);
        next += filename.native().size() + 1u;
    }
    return selectedFiles;
}
