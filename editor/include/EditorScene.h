#pragma once

#include "AssetImportPlanner.h"
#include "InputSettingsStore.h"
#include "PhysicsSettingsStore.h"
#include "PlayerSettingsStore.h"
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
                std::function<void()> requestClose, bool playerMode = false);

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
    bool LaunchPlayerPreview();
    bool BuildPlayerPackage(std::filesystem::path* destination = nullptr);
    bool BuildAndRunPlayerPackage();
    bool LaunchPackagedPlayer(const std::filesystem::path& package);
    void EnterPlayMode();
    void StopPlayMode();
    void TogglePlayPause();
    void StepRuntimeWorld();
    void ReleaseGameInputCapture();
    bool BeginRuntimeWorld(std::string* error = nullptr);
    bool BeginRuntimeAnimators(std::string* error = nullptr);
    bool BeginEditAnimatorPreview(EntityId entity);
    void UpdateEditAnimatorPreview(float deltaTime);
    void EndEditAnimatorPreview();
    [[nodiscard]] bool ValidateWorldBehaviorRequirements(
        std::string* error = nullptr) const;
    void UpdateRuntimeWorld(float deltaTime);
    bool ApplyPendingRuntimeSceneLoad();
    void UpdateRuntimeAnimators(float deltaTime);
    void EndRuntimeWorld();
    void EndRuntimeAnimators();
    bool BeginRuntimeAudio(std::string* error = nullptr);
    void UpdateRuntimeAudio();
    void PauseRuntimeAudio(bool paused);
    void EndRuntimeAudio();
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
    void DrawProjectSettingsWindow();
    bool SavePhysicsSettings();
    bool SavePlayerSettings();
    bool SaveInputSettings();
    size_t UpgradeInputActionReferences();
    void CaptureConsoleStatus();
    void AddConsoleEntry(std::string message, ConsoleSeverity severity,
                         std::filesystem::path sourcePath = {}, uint32_t sourceLine = 0u,
                         uint32_t sourceColumn = 0u);
    bool OpenConsoleSource(const std::filesystem::path& sourcePath, uint32_t sourceLine);
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
    void SetSelectedEntitiesActive(EntityId source, bool active);
    bool CopySelection();
    void CutSelection();
    bool PasteEntityClipboard(EntityId parent = {});
    void RequestEntityRename(EntityId entity);
    bool MoveEntityInHierarchy(EntityId entity, int direction);
    bool MoveSelectionAdjacentTo(EntityId draggedEntity, EntityId sibling, bool after);
    void DuplicateSelection();
    void ReparentSelection(EntityId draggedEntity, EntityId parent);
    void AssignModelAsset(EntityId entity, const std::filesystem::path& path);
    void AssignAudioAsset(EntityId entity, const std::filesystem::path& path);
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
    bool SaveSelectionAsPrefab();
    bool InstantiatePrefabAsset(
        const std::filesystem::path& path, EntityId parent = {},
        std::optional<DirectX::XMFLOAT3> position = std::nullopt);
    bool TryNormalizeModelAssetReference(const std::filesystem::path& path,
                                         std::string& assetPath);
    bool TryNormalizeTextureAssetReference(const std::filesystem::path& path,
                                           std::string& assetPath);
    bool TryNormalizeAudioAssetReference(const std::filesystem::path& path,
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
    void DrawAudioAssetPreview(const std::filesystem::path& physicalPath);
    void StopAudioAssetPreview();
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
    bool UpdateGameViewCamera();
    bool UpdateCameraFromEntity(EntityId entity, Camera& camera, int width, int height) const;
    bool DrawSelectedCameraPreview(const ImVec2& imageMin, const ImVec2& imageMax);
    void PickSceneEntity(const ImVec2& imageMin, const ImVec2& imageMax, bool imageHovered);
    void DrawSceneComponentGizmos(const ImVec2& imageMin, const ImVec2& imageMax) const;
    void DrawSceneSelectionOutline(const ImVec2& imageMin, const ImVec2& imageMax) const;
    void DrawSceneGizmoToolbar();
    bool DrawBoxColliderGizmo(const ImVec2& imageMin, const ImVec2& imageMax);
    bool DrawCharacterControllerGizmo(const ImVec2& imageMin, const ImVec2& imageMax);
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
    [[nodiscard]] std::optional<std::filesystem::path>
    ShowSavePrefabDialog(std::string_view entityName) const;
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
    bool playerMode_ = false;
    std::filesystem::path projectRoot_;
    std::filesystem::path assetRoot_;
    std::filesystem::path sceneRoot_;
    std::filesystem::path startupScenePath_;
    std::filesystem::path imguiSettingsPath_;
    bool resetDockLayoutRequested_ = false;
    bool showHierarchyPanel_ = true;
    bool showProjectPanel_ = true;
    bool showScenePanel_ = true;
    bool showGamePanel_ = true;
    bool showConsolePanel_ = true;
    bool showInspectorPanel_ = true;
    bool showProjectSettings_ = false;
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
    PlayerSettingsStore playerSettingsStore_;
    PlayerSettings playerSettings_{};
    bool playerSettingsDirty_ = false;
    PhysicsSettingsStore physicsSettingsStore_;
    PhysicsSettings physicsSettings_ = PhysicsSettings::Defaults();
    bool physicsSettingsDirty_ = false;
    InputSettingsStore inputSettingsStore_;
    bool inputSettingsDirty_ = false;
    std::array<char, 65> inputActionNameBuffer_{};
    std::string pendingInputActionName_;
    InputActionType newInputActionType_ = InputActionType::Button;
    bool showCreateInputActionDialog_ = false;
    bool showRenameInputActionDialog_ = false;
    bool showDeleteInputActionDialog_ = false;
    bool focusInputActionNameInput_ = false;
    RecentScenesStore recentScenesStore_;
    World world_;
    ProjectScriptLibrary projectScripts_;
    BehaviorRegistry behaviorRegistry_;
    BehaviorSystem runtimeBehaviors_;
    TriggerSystem runtimeTriggers_;
    struct RuntimeAudioSource {
        EntityId entity{};
        uint32_t soundId = kInvalidResourceId;
        uint32_t voice = kInvalidResourceId;
        std::vector<uint32_t> oneShotVoices;
        bool activated = false;
    };
    std::vector<RuntimeAudioSource> runtimeAudioSources_;
    struct RuntimeAnimator {
        EntityId entity{};
        ModelHandle model{};
    };
    std::vector<RuntimeAnimator> runtimeAnimators_;
    std::unordered_map<std::string, ModelHandle> animatorModels_;
    EntityId editAnimatorPreviewEntity_{};
    ModelHandle editAnimatorPreviewModel_{};
    std::string editAnimatorPreviewModelPath_;
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
    std::filesystem::path runtimeScenePath_;
    std::vector<std::filesystem::path> recentScenePaths_;
    PendingSceneAction pendingSceneAction_ = PendingSceneAction::None;
    std::filesystem::path pendingScenePath_;
    std::string status_ = "Editor session started.";
    struct ConsoleEntry {
        std::string message;
        double timestampSeconds = 0.0;
        ConsoleSeverity severity = ConsoleSeverity::Info;
        std::filesystem::path sourcePath;
        uint32_t sourceLine = 0u;
        uint32_t sourceColumn = 0u;
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
    Camera assetPreviewCamera_{};
    ModelHandle assetPreviewModel_{};
    Transform assetPreviewTransform_{};
    DirectX::XMFLOAT2 assetPreviewRotationDegrees_{0.0f, 180.0f};
    std::filesystem::path assetPreviewAsset_;
    std::unordered_map<std::string, ModelHandle> assetPreviewModels_;
    std::string assetPreviewAnimation_;
    bool assetPreviewAnimationLoop_ = true;
    float assetPreviewAnimationSpeed_ = 1.0f;
    std::vector<AssetImport::File> assetPreviewPlan_;
    std::string assetPreviewError_;
    uint32_t audioPreviewSoundId_ = kInvalidResourceId;
    uint32_t audioPreviewVoice_ = kInvalidResourceId;
    ModelHandle primitiveModels_[4]{};
    uint32_t sceneGridPipelineId_ = kInvalidResourceId;
    std::unordered_map<std::string, ModelHandle> loadedModels_;
    std::unordered_map<std::string, TextureHandle> loadedTextures_;
    std::unordered_map<std::string, TextureHandle> loadedLinearTextures_;
    std::vector<std::filesystem::path> modelAssets_;
    std::vector<std::filesystem::path> textureAssets_;
    std::vector<std::filesystem::path> audioAssets_;
    std::vector<std::filesystem::path> scriptAssets_;
    std::vector<std::filesystem::path> prefabAssets_;
    std::vector<std::filesystem::path> sceneAssets_;
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
    enum class BoxColliderGizmoMode : uint8_t {
        None,
        Center,
        Size,
    };
    enum class CharacterControllerGizmoMode : uint8_t {
        None,
        Center,
        Radius,
        Height,
    };
    GizmoOperation gizmoOperation_ = GizmoOperation::Translate;
    GizmoSpace gizmoSpace_ = GizmoSpace::World;
    bool gizmoSnapEnabled_ = false;
    float translationSnap_ = 1.0f;
    float rotationSnapDegrees_ = 15.0f;
    float scaleSnap_ = 0.1f;
    bool showSceneGrid_ = true;
    bool showPhysicsDebug_ = false;
    uint32_t physicsDebugLayerMask_ = 0xffffffffu;
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
    BoxColliderGizmoMode boxColliderGizmoMode_ = BoxColliderGizmoMode::None;
    EntityId boxColliderGizmoEntity_{};
    CharacterControllerGizmoMode characterControllerGizmoMode_ =
        CharacterControllerGizmoMode::None;
    EntityId characterControllerGizmoEntity_{};
    bool postProcessInitializationAttempted_ = false;
    bool gamePostProcessInitializationAttempted_ = false;
    bool cameraPreviewPostProcessInitializationAttempted_ = false;
    bool assetPreviewPostProcessInitializationAttempted_ = false;
    float projectPanelMinX_ = 0.0f;
    float projectPanelMinY_ = 0.0f;
    float projectPanelMaxX_ = 0.0f;
    float projectPanelMaxY_ = 0.0f;
};
