#pragma once

#include "AssetImportPlanner.h"
#include "ProjectScriptLibrary.h"
#include "RecentScenesStore.h"
#include "camera/Camera.h"
#include "graphics/PostProcessSystem.h"
#include "graphics/RenderSurface.h"
#include "graphics/SceneRenderer.h"
#include "graphics/RenderScene.h"
#include "runtime/BehaviorRegistry.h"
#include "runtime/BehaviorSystem.h"
#include "runtime/TriggerSystem.h"
#include "scene/BaseScene.h"
#include "world/World.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ImVec2;

class EditorScene final : public BaseScene {
public:
    EditorScene(std::filesystem::path projectRoot, std::filesystem::path assetRoot,
                std::filesystem::path sceneRoot, std::filesystem::path startupScene,
                std::filesystem::path recentScenesPath, std::filesystem::path imguiSettingsPath,
                std::function<void()> requestClose);

    void Initialize(const SceneContext& ctx) override;
    void Update() override;
    void SubmitLighting(LightingScene& lightingScene) override;
    void Draw() override;
    void DrawPostProcessOverlay() override;
    bool OnCloseRequested() override;
    void OnFilesDropped(std::span<const std::filesystem::path> files, int screenX,
                        int screenY) override;

private:
    enum class ConsoleSeverity : uint8_t {
        Info,
        Warning,
        Error,
    };
    struct ScriptBuildCompletion {
        bool succeeded = false;
        std::string error;
        std::string output;
    };

    void DrawMainMenu();
    void EnterPlayMode();
    void StopPlayMode();
    void TogglePlayPause();
    void StepRuntimeWorld();
    void ReleaseGameInputCapture();
    bool BeginRuntimeWorld(std::string* error = nullptr);
    [[nodiscard]] bool ValidateWorldBehaviorRequirements(
        std::string* error = nullptr) const;
    void UpdateRuntimeWorld(float deltaTime);
    void EndRuntimeWorld();
    [[nodiscard]] bool IsInPlayMode() const;
    void DrawUnsavedChangesDialog();
    void DrawEntityRenameDialog();
    void DrawDockSpace();
    void DrawPanels();
    void DrawProjectPanel();
    void DrawHierarchyPanel();
    void DrawEntityNode(EntityId id);
    void DrawInspectorPanel();
    void DrawConsolePanel();
    void CaptureConsoleStatus();
    void AddConsoleEntry(std::string message, ConsoleSeverity severity);
    void InitializeScriptMonitoring();
    void UpdateScriptCompilation();
    void StartScriptCompilation();
    void FinishScriptCompilation();
    bool ReloadProjectScripts(std::string& error);
    bool DrawCreateEntityMenu(const DirectX::XMFLOAT3& position, EntityId parent = {});
    void HandleEditorShortcuts();
    void SynchronizeHierarchySelection();
    void SelectHierarchyEntity(EntityId entity, bool toggle, bool range);
    void SelectAllHierarchyEntities();
    void ClearHierarchySelection();
    [[nodiscard]] bool IsHierarchyEntitySelected(EntityId entity) const;
    [[nodiscard]] std::vector<EntityId> GetTopLevelSelectedEntities() const;
    void SetSelectedMeshRenderersEnabled(EntityId source, bool enabled);
    bool CopySelection();
    void CutSelection();
    bool PasteEntityClipboard(EntityId parent = {});
    void RequestEntityRename(EntityId entity);
    bool MoveEntityInHierarchy(EntityId entity, int direction);
    bool MoveSelectionAdjacentTo(EntityId draggedEntity, EntityId sibling, bool after);
    void DuplicateSelection();
    void ReparentSelection(EntityId draggedEntity, EntityId parent);
    void AssignModelAsset(EntityId entity, const std::filesystem::path& path);
    void AssignScriptAsset(EntityId entity, const std::filesystem::path& path,
                           std::optional<size_t> scriptIndex = std::nullopt);
    void ClearScriptAsset(EntityId entity, size_t scriptIndex);
    void AssignBaseColorTexture(EntityId entity, const std::filesystem::path& path);
    void AssignNormalTexture(EntityId entity, const std::filesystem::path& path);
    void AssignRoughnessTexture(EntityId entity, const std::filesystem::path& path);
    void AssignMetallicTexture(EntityId entity, const std::filesystem::path& path);
    void HandleSceneAssetDrop(const ImVec2& imageMin, const ImVec2& imageMax);
    void HandleSceneCameraControls(const ImVec2& imageMin, const ImVec2& imageMax,
                                   bool imageHovered);
    bool FocusSceneCameraOnSelection();
    bool AlignSelectedCameraToSceneView();
    bool AlignSceneViewToSelectedCamera();
    void HandleSceneContextMenu(const ImVec2& imageMin, const ImVec2& imageMax,
                                bool imageHovered);
    void CreateEmptyEntity(const DirectX::XMFLOAT3& position, EntityId parent = {});
    void CreatePrimitiveEntity(MeshPrimitive primitive, const DirectX::XMFLOAT3& position,
                               EntityId parent = {});
    void DeleteSelection();
    void CreateModelEntityFromAsset(const std::filesystem::path& path,
                                    const DirectX::XMFLOAT3& position);
    bool TryNormalizeModelAssetReference(const std::filesystem::path& path,
                                         std::string& assetPath);
    bool TryNormalizeTextureAssetReference(const std::filesystem::path& path,
                                           std::string& assetPath);
    bool TryNormalizeScriptAssetReference(const std::filesystem::path& path,
                                          std::string& assetPath,
                                          std::filesystem::path& physicalPath);
    void RefreshAssetBrowser();
    void NavigateAssetBrowser(const std::filesystem::path& relativeDirectory);
    void DrawAssetBrowserBreadcrumbs();
    void DrawAssetBrowserEntry(const std::filesystem::path& relativePath,
                               bool directory);
    void DrawSelectedAssetDetails();
    void DrawAssetPreviewPopup();
    void DrawAssetOperationDialogs();
    void RequestAssetRename(const std::filesystem::path& relativePath, bool directory);
    void RequestAssetDelete(const std::filesystem::path& relativePath, bool directory);
    void RequestCreateAssetFolder();
    bool RenamePendingAsset();
    bool DeletePendingAsset();
    bool DuplicateAsset(const std::filesystem::path& relativePath);
    bool CreatePendingAssetFolder();
    bool ImportAssetFiles();
    bool ImportAssetFiles(const std::vector<std::filesystem::path>& selectedFiles);
    bool RevealAssetInExplorer(const std::filesystem::path& relativePath);
    void SelectAssetReferences(const std::filesystem::path& relativePath, bool directory);
    [[nodiscard]] bool IsAssetReferenced(const std::filesystem::path& relativePath,
                                         bool directory) const;
    [[nodiscard]] size_t CountAssetReferences(const std::filesystem::path& relativePath,
                                              bool directory) const;
    size_t UpdateAssetReferences(const std::filesystem::path& oldRelativePath,
                                 const std::filesystem::path& newRelativePath, bool directory);
    [[nodiscard]] std::optional<std::filesystem::path>
    ResolveProjectAssetPath(const std::filesystem::path& path) const;
    void Undo();
    void Redo();
    void BeginHistoryEdit(std::string label);
    void CommitHistoryEdit();
    void RecordImmediateEdit(std::string label, std::string before, EntityId selectionBefore);
    void ClearHistory(bool markClean);
    void RefreshDirty();
    void BuildRenderScene();
    void BuildEditorOverlayScene();
    void UpdateAssetPreview();
    void BuildAssetPreviewScene();
    bool UpdateGameViewCamera();
    bool UpdateCameraFromEntity(EntityId entity, Camera& camera, int width, int height) const;
    bool DrawSelectedCameraPreview(const ImVec2& imageMin, const ImVec2& imageMax);
    void PickSceneEntity(const ImVec2& imageMin, const ImVec2& imageMax, bool imageHovered);
    void DrawSceneComponentGizmos(const ImVec2& imageMin, const ImVec2& imageMax) const;
    void DrawSceneSelectionOutline(const ImVec2& imageMin, const ImVec2& imageMax) const;
    void DrawSceneGizmoToolbar();
    bool DrawSceneTransformGizmo(const ImVec2& imageMin, const ImVec2& imageMax);
    void ResolveMeshResources();
    ModelHandle ResolveModel(const MeshRendererComponent& component) const;
    TextureHandle ResolveBaseColorTexture(const MaterialOverrideComponent& component) const;
    TextureHandle ResolveNormalTexture(const MaterialOverrideComponent& component) const;
    TextureHandle ResolveLinearTexture(const std::string& path) const;
    enum class PendingSceneAction {
        None,
        NewScene,
        OpenScene,
        ReloadScene,
        Exit,
    };

    void RequestSceneAction(PendingSceneAction action,
                            std::filesystem::path path = {});
    void ExecuteSceneAction(PendingSceneAction action,
                            const std::filesystem::path& path = {});
    void NewScene(bool clearPath);
    bool SaveScene();
    bool SaveSceneAs();
    bool LoadScene(const std::filesystem::path& path);
    void AddRecentScene(const std::filesystem::path& path);
    [[nodiscard]] std::optional<std::filesystem::path> ShowOpenSceneDialog() const;
    [[nodiscard]] std::optional<std::filesystem::path> ShowSaveSceneDialog() const;
    [[nodiscard]] std::vector<std::filesystem::path> ShowImportAssetDialog() const;

    struct HistoryState {
        std::string world;
        EntityId selection{};
    };

    struct HistoryEntry {
        std::string label;
        HistoryState before;
        HistoryState after;
    };

    struct PendingHistoryEdit {
        std::string label;
        HistoryState before;
    };

    [[nodiscard]] HistoryState CaptureHistoryState() const;
    bool RestoreHistoryState(const HistoryState& state);

    std::function<void()> requestClose_;
    std::filesystem::path projectRoot_;
    std::filesystem::path assetRoot_;
    std::filesystem::path sceneRoot_;
    std::filesystem::path imguiSettingsPath_;
    bool resetDockLayoutRequested_ = false;
    bool showHierarchyPanel_ = true;
    bool showProjectPanel_ = true;
    bool showScenePanel_ = true;
    bool showGamePanel_ = true;
    bool showConsolePanel_ = true;
    bool showInspectorPanel_ = true;
    enum class PlayModeState : uint8_t {
        Edit,
        Playing,
        Paused,
    };
    PlayModeState playModeState_ = PlayModeState::Edit;
    std::optional<World> editModeWorld_;
    EntityId playModeSelectionSnapshot_{};
    bool playModeDirtySnapshot_ = false;
    bool focusGamePanelRequested_ = false;
    bool gameInputCaptured_ = false;
    int gameInputCursorRestoreX_ = 0;
    int gameInputCursorRestoreY_ = 0;
    uint64_t runtimeFrameCount_ = 0;
    double runtimeElapsedSeconds_ = 0.0;
    RecentScenesStore recentScenesStore_;
    World world_;
    ProjectScriptLibrary projectScripts_;
    BehaviorRegistry behaviorRegistry_;
    BehaviorSystem runtimeBehaviors_;
    TriggerSystem runtimeTriggers_;
    std::future<ScriptBuildCompletion> scriptBuildFuture_;
    uint64_t scriptSourceFingerprint_ = 0u;
    bool scriptFingerprintInitialized_ = false;
    bool scriptBuildInProgress_ = false;
    bool scriptBuildPending_ = false;
    bool scriptChangesDeferredMessageShown_ = false;
    std::chrono::steady_clock::time_point lastScriptScanTime_{};
    std::chrono::steady_clock::time_point lastScriptChangeTime_{};
    EntityId selection_{};
    std::unordered_set<EntityId, EntityIdHash> hierarchySelection_;
    EntityId hierarchySelectionAnchor_{};
    std::filesystem::path scenePath_;
    std::vector<std::filesystem::path> recentScenePaths_;
    PendingSceneAction pendingSceneAction_ = PendingSceneAction::None;
    std::filesystem::path pendingScenePath_;
    std::string status_ = "Editor session started.";
    struct ConsoleEntry {
        std::string message;
        double timestampSeconds = 0.0;
        ConsoleSeverity severity = ConsoleSeverity::Info;
    };
    std::vector<ConsoleEntry> consoleEntries_;
    std::string lastCapturedStatus_;
    std::array<char, 128> consoleSearch_{};
    bool showConsoleInfo_ = true;
    bool showConsoleWarnings_ = true;
    bool showConsoleErrors_ = true;
    bool consoleScrollToBottom_ = false;
    std::string savedWorldSnapshot_;
    std::vector<HistoryEntry> undoHistory_;
    std::vector<HistoryEntry> redoHistory_;
    std::optional<PendingHistoryEdit> pendingHistoryEdit_;
    std::string entityClipboard_;
    std::optional<TransformComponent> transformClipboard_;
    std::array<char, 128> hierarchySearch_{};
    std::unordered_set<EntityId, EntityIdHash> visibleHierarchyEntities_;
    EntityId renameEntity_{};
    std::array<char, 256> renameBuffer_{};
    bool dirty_ = false;
    bool showUnsavedChangesDialog_ = false;
    bool showEntityRenameDialog_ = false;
    bool focusEntityRenameInput_ = false;
    RenderSurface sceneViewSurface_{};
    PostProcessSystem sceneViewPostProcess_{};
    SceneRenderer sceneRenderer_{};
    RenderScene renderScene_{};
    RenderScene editorOverlayScene_{};
    Camera sceneViewCamera_{};
    RenderSurface gameViewSurface_{};
    PostProcessSystem gameViewPostProcess_{};
    Camera gameViewCamera_{};
    RenderSurface cameraPreviewSurface_{};
    PostProcessSystem cameraPreviewPostProcess_{};
    Camera cameraPreviewCamera_{};
    RenderSurface assetPreviewSurface_{};
    PostProcessSystem assetPreviewPostProcess_{};
    RenderScene assetPreviewScene_{};
    Camera assetPreviewCamera_{};
    ModelHandle assetPreviewModel_{};
    Transform assetPreviewTransform_{};
    std::filesystem::path assetPreviewAsset_;
    std::vector<AssetImport::File> assetPreviewPlan_;
    std::string assetPreviewError_;
    ModelHandle primitiveModels_[4]{};
    uint32_t sceneGridPipelineId_ = kInvalidResourceId;
    std::unordered_map<std::string, ModelHandle> loadedModels_;
    std::unordered_map<std::string, TextureHandle> loadedTextures_;
    std::unordered_map<std::string, TextureHandle> loadedLinearTextures_;
    std::vector<std::filesystem::path> modelAssets_;
    std::vector<std::filesystem::path> textureAssets_;
    std::vector<std::filesystem::path> scriptAssets_;
    struct AssetBrowserEntry {
        std::filesystem::path relativePath;
        bool directory = false;
    };
    std::vector<AssetBrowserEntry> assetBrowserEntries_;
    std::filesystem::path currentAssetDirectory_;
    std::optional<std::filesystem::path> pendingAssetDirectory_;
    std::filesystem::path selectedAsset_;
    std::array<char, 128> assetSearch_{};
    enum class AssetSortMode { Name, Type, Size };
    AssetSortMode assetSortMode_ = AssetSortMode::Name;
    int assetFormatFilter_ = 0;
    bool assetSortAscending_ = true;
    std::filesystem::path pendingAssetOperationPath_;
    std::array<char, 256> assetRenameBuffer_{};
    std::array<char, 256> assetFolderNameBuffer_{};
    bool pendingAssetOperationIsDirectory_ = false;
    bool showAssetRenameDialog_ = false;
    bool showAssetDeleteDialog_ = false;
    bool showCreateAssetFolderDialog_ = false;
    bool focusAssetRenameInput_ = false;
    bool focusAssetFolderNameInput_ = false;
    int requestedSceneWidth_ = 960;
    int requestedSceneHeight_ = 540;
    int requestedGameWidth_ = 960;
    int requestedGameHeight_ = 540;
    enum class GizmoOperation : uint8_t {
        Translate,
        Rotate,
        Scale,
    };
    enum class GizmoSpace : uint8_t {
        Local,
        World,
    };
    GizmoOperation gizmoOperation_ = GizmoOperation::Translate;
    GizmoSpace gizmoSpace_ = GizmoSpace::World;
    bool gizmoSnapEnabled_ = false;
    float translationSnap_ = 1.0f;
    float rotationSnapDegrees_ = 15.0f;
    float scaleSnap_ = 0.1f;
    bool showSceneGrid_ = true;
    bool sceneCameraNavigating_ = false;
    bool sceneCameraPanning_ = false;
    bool sceneCameraCursorCaptured_ = false;
    float sceneCameraPointerTravel_ = 0.0f;
    int sceneCameraCursorRestoreX_ = 0;
    int sceneCameraCursorRestoreY_ = 0;
    DirectX::XMFLOAT3 sceneContextCreatePosition_{};
    EntityId activeGizmoEntity_{};
    DirectX::XMFLOAT4X4 activeGizmoStartWorld_{};
    std::vector<std::pair<EntityId, DirectX::XMFLOAT4X4>> activeGizmoWorldTransforms_;
    bool gizmoWasUsing_ = false;
    bool postProcessInitializationAttempted_ = false;
    bool gamePostProcessInitializationAttempted_ = false;
    bool cameraPreviewPostProcessInitializationAttempted_ = false;
    bool assetPreviewPostProcessInitializationAttempted_ = false;
    float projectPanelMinX_ = 0.0f;
    float projectPanelMinY_ = 0.0f;
    float projectPanelMaxX_ = 0.0f;
    float projectPanelMaxY_ = 0.0f;
};
