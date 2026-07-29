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
class ModelManager;
struct Model;
struct Sprite;
struct PlayerPackageRequest;
struct ProjectDescriptor;
struct SceneLighting;

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
    void InitializeInputSettings(const SceneContext& ctx);
    void InitializeProjectScripts(const SceneContext& ctx);
    void InitializeDocking(const SceneContext& ctx);
    bool InitializeViewSurfaces(const SceneContext& ctx);
    bool InitializeRenderingResources(const SceneContext& ctx);
    void InitializeEditorCameras();
    bool TryResolveSceneLight(const WorldEntity& entity, DirectX::XMFLOAT4X4& worldMatrix,
                              DirectX::XMFLOAT3& direction) const;
    void UpdateEditorSimulation();
    void UpdateSceneViewResources();
    void UpdateGameViewResources();
    void UpdatePreviewResources();

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
    void DrawFileMenu();
    void DrawBuildMenu();
    void DrawEditMenu();
    void DrawViewMenu();
    void DrawRuntimeControls();
    void DrawEditorTitle();
    bool LaunchPlayerPreview();
    bool CanLaunchPlayerPreview();
    bool ValidatePlayerPreviewProject();
    bool TryLocateEditorExecutable(std::filesystem::path& executable);
    bool StartPlayerPreviewProcess(const std::filesystem::path& executable);
    bool BuildPlayerPackage(std::filesystem::path* destination = nullptr);
    bool CanBuildPlayerPackage();
    bool TryLoadPlayerBuildProject(ProjectDescriptor& project, std::string& error);
    bool TryLocatePlayerExecutable(std::filesystem::path& executable);
    PlayerPackageRequest CreatePlayerPackageRequest(
        const ProjectDescriptor& project, const std::filesystem::path& executable) const;
    void SetPlayerPackageBuildStatus(const std::filesystem::path& destination,
                                     const std::string& warning);
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
    bool IsRuntimeUiEntityInteractable(const WorldEntity& entity) const;
    bool DispatchPendingInputFieldEvents();
    bool DispatchPendingDropdownChanges();
    bool DispatchPendingSliderChanges();
    bool DispatchPendingButtonClicks();
    bool DispatchPendingRuntimeUiEvents();
    bool ApplyPendingRuntimeSceneLoad();
    void UpdateRuntimeAnimators(float deltaTime);
    void EndRuntimeWorld();
    void EndRuntimeAnimators();
    struct RuntimeAudioSource;
    bool BeginRuntimeAudio(std::string* error = nullptr);
    void UpdateRuntimeAudio();
    const WorldEntity* FindRuntimeAudioListener() const;
    void UpdateRuntimeAudioListener(ISoundService& sound) const;
    void StopRuntimeAudioVoices(RuntimeAudioSource& runtime, ISoundService& sound);
    uint32_t PlayRuntimeAudioVoice(const RuntimeAudioSource& runtime,
                                   const AudioSourceComponent& source,
                                   ISoundService& sound, bool loop) const;
    void StartRuntimeAudioVoice(RuntimeAudioSource& runtime,
                                const AudioSourceComponent& source,
                                ISoundService& sound);
    void ProcessRuntimeAudioPlayback(
        RuntimeAudioSource& runtime, const AudioSourceComponent& source,
        AudioSourceComponent::RuntimeCommand command, uint32_t pendingOneShots,
        ISoundService& sound);
    void UpdateRuntimeAudioVoiceSettings(const RuntimeAudioSource& runtime,
                                         const AudioSourceComponent& source,
                                         ISoundService& sound) const;
    void UpdateRuntimeAudioSource(RuntimeAudioSource& runtime, ISoundService& sound);
    void PauseRuntimeAudio(bool paused);
    void EndRuntimeAudio();
    [[nodiscard]] bool IsInPlayMode() const;
    void DrawUnsavedChangesDialog();
    void DrawEntityRenameDialog();
    void PrepareEntityRenamePopup();
    WorldEntity* ResolveEntityRenameTarget();
    void DrawEntityRenameInput(bool& renameRequested, bool& cancelRequested);
    void CommitEntityRename(WorldEntity& entity);
    void CancelEntityRename();
    void DrawDockSpace();
    void DrawPanels();
    void DrawHierarchyAndProjectPanels();
    void DrawScenePanelWindow();
    void DrawGamePanelWindow();
    void DrawConsoleAndInspectorPanels();
    void DrawProjectPanel();
    void DrawHierarchyPanel();
    void DrawEntityNode(EntityId id);
    std::vector<EntityId> GetVisibleHierarchyChildren(EntityId id) const;
    int BuildHierarchyNodeFlags(EntityId id, bool filtering, bool hasChildren) const;
    bool DrawHierarchyNodeHeader(EntityId id, int flags, bool editing, ImVec2& nodeMin,
                                 ImVec2& nodeMax);
    void HandleHierarchyNodeSelection(EntityId id);
    bool DrawHierarchyEntityContextMenu(EntityId id, bool editing);
    void DrawHierarchyEntityDragSource(EntityId id, bool editing);
    void DrawHierarchyEntityDropTarget(EntityId id, bool editing, const ImVec2& nodeMin,
                                       const ImVec2& nodeMax);
    void DrawInspectorPanel();
    void DrawEntityHeaderAndTransformInspector(
        WorldEntity* entity, const std::vector<EntityId>& inspectedEntities);
    void DrawSelectedEntitiesActive(
        const WorldEntity& entity, const std::vector<EntityId>& inspectedEntities);
    void DrawEntitySelectionIdentity(
        WorldEntity& entity, const std::vector<EntityId>& inspectedEntities);
    void DrawSelectedEntitiesLayer(
        const WorldEntity& entity, const std::vector<EntityId>& inspectedEntities);
    void ResetSelectedTransforms(const std::vector<EntityId>& inspectedEntities);
    void PasteSelectedTransforms(const std::vector<EntityId>& inspectedEntities);
    void DrawTransformToolbar(
        const WorldEntity& entity, const std::vector<EntityId>& inspectedEntities);
    void DrawTransformField(
        WorldEntity& entity, const std::vector<EntityId>& inspectedEntities,
        const char* label, DirectX::XMFLOAT3 TransformComponent::* member,
        float speed, bool scale);
    void DrawAddComponentInspector(WorldEntity* entity);
    void DrawAddComponentScriptDropTarget();
    void AddInspectorComponent(const char* historyLabel, const char* status,
                               const std::function<void()>& addComponent);
    void DrawAddRenderingComponents(WorldEntity& entity);
    void DrawAddAudioAnimationComponents(WorldEntity& entity);
    void DrawAddUiFoundationComponents(WorldEntity& entity);
    void DrawAddUiControlComponents(WorldEntity& entity);
    void DrawAddPhysicsComponents(WorldEntity& entity);
    void DrawAddScriptComponent(WorldEntity& entity);
    void DrawMeshRendererInspector(WorldEntity* entity);
    void DrawScriptsInspector(WorldEntity* entity);
    bool DrawMaterialOverrideHeader(WorldEntity* entity);
    void DrawMaterialSurfaceSettings(MaterialOverrideComponent& material,
                                     EntityId selectionBefore);
    void DrawMaterialFloat(const char* label, float& value, float minimum, float maximum,
                           const char* historyLabel = "Modify Material Override");
    void DrawBaseColorTextureSlot(MaterialOverrideComponent& material, EntityId selectionBefore);
    void DrawNormalTextureSettings(MaterialOverrideComponent& material, EntityId selectionBefore);
    using MaterialTextureAssignFunction =
        void (EditorScene::*)(EntityId, const std::filesystem::path&);
    void DrawMaterialLinearTextureSlot(const char* label, const char* id, std::string& path,
                                       EntityId selectionBefore,
                                       MaterialTextureAssignFunction assignTexture);
    void DrawMaterialPbrTextureSettings(MaterialOverrideComponent& material,
                                        EntityId selectionBefore);
    void DrawMaterialTexturePreview(TextureHandle texture, const char* unavailableText) const;
    bool DrawScriptEntryInspector(WorldEntity* entity, size_t scriptIndex);
    void DrawScriptPropertiesInspector(WorldEntity* entity, BehaviorComponent& behavior,
                                       EntityId selectionBefore);
    bool DrawScalarScriptPropertyInspector(BehaviorComponent& behavior,
                                           const ScriptPropertyDefinition& definition,
                                           EntityId selectionBefore);
    bool DrawAssetScriptPropertyInspector(WorldEntity* entity,
                                          BehaviorComponent& behavior,
                                          const ScriptPropertyDefinition& definition,
                                          EntityId selectionBefore);
    std::string GetAssetScriptPropertyValue(
        const BehaviorComponent& behavior,
        const ScriptPropertyDefinition& definition) const;
    void AssignAssetScriptProperty(BehaviorComponent& behavior,
                                   const ScriptPropertyDefinition& definition,
                                   const std::string& value,
                                   EntityId selectionBefore);
    void DrawAnimationClipScriptProperty(WorldEntity* entity,
                                         BehaviorComponent& behavior,
                                         const ScriptPropertyDefinition& definition,
                                         EntityId selectionBefore);
    void DrawInputActionScriptProperty(BehaviorComponent& behavior,
                                       const ScriptPropertyDefinition& definition,
                                       EntityId selectionBefore);
    void DrawSceneScriptProperty(BehaviorComponent& behavior,
                                 const ScriptPropertyDefinition& definition,
                                 EntityId selectionBefore);
    bool AcceptsScriptInputAction(const ScriptPropertyDefinition& definition,
                                  const InputActionBinding& binding) const;
    bool DrawStringScriptPropertyInspector(BehaviorComponent& behavior,
                                           const ScriptPropertyDefinition& definition);
    bool DrawEntityScriptPropertyInspector(BehaviorComponent& behavior,
                                           const ScriptPropertyDefinition& definition,
                                           EntityId selectionBefore);
    void DrawBoxColliderInspector(WorldEntity* entity);
    void DrawCharacterControllerInspector(WorldEntity* entity);
    void DrawCameraInspector(WorldEntity* entity);
    void DrawLightInspector(WorldEntity* entity);
    void DrawAudioSourceInspector(WorldEntity* entity);
    void DrawAudioListenerInspector(WorldEntity* entity);
    void DrawAnimatorInspector(WorldEntity* entity);
    bool DrawAnimatorHeader(WorldEntity* entity);
    void DrawAnimatorRuntimeStatus(const AnimatorComponent& animator) const;
    const Model* ResolveAnimatorInspectorModel(const WorldEntity& entity) const;
    void DrawAnimatorClipSelection(const WorldEntity& entity, AnimatorComponent& animator,
                                   const Model* model, EntityId selectionBefore);
    void DrawAnimatorPreviewControls(EntityId entity, const Model* model);
    void DrawAnimatorCheckbox(const char* label, bool& value, const char* historyLabel,
                              EntityId selectionBefore);
    void DrawAnimatorPlaybackSettings(const WorldEntity& entity, AnimatorComponent& animator,
                                      EntityId selectionBefore);
    void SynchronizeAnimatorPreviewSettings(EntityId entity, const AnimatorComponent& animator);
    void DrawCanvasInspector(WorldEntity* entity);
    void DrawCanvasGroupInspector(WorldEntity* entity);
    void DrawEventSystemInspector(WorldEntity* entity);
    void DrawTextInspector(WorldEntity* entity);
    bool DrawTextRemoval(WorldEntity* entity);
    void DrawTextGeneralSettings(TextComponent& text, EntityId selectionBefore);
    void DrawTextContentAndPosition(TextComponent& text);
    void DrawTextFontPicker(TextComponent& text, EntityId selectionBefore);
    void DrawTextFontStatus(const TextComponent& text);
    void DrawTextTypographySettings(TextComponent& text);
    void DrawTextColorAndAlignment(TextComponent& text, EntityId selectionBefore);
    void DrawTextCanvasRequirement(const WorldEntity* entity);
    void DrawImageInspector(WorldEntity* entity);
    bool DrawImageRemoval(WorldEntity* entity);
    void DrawImageGeneralSettings(ImageComponent& image, EntityId selectionBefore);
    void DrawImageFillSettings(ImageComponent& image, EntityId selectionBefore);
    void DrawImageAnchorAndPivotSettings(ImageComponent& image, EntityId selectionBefore);
    void DrawImageTextureSettings(ImageComponent& image, EntityId selectionBefore);
    void DrawImageLayoutAndColorSettings(ImageComponent& image);
    void DrawImageTexturePreview(ImageComponent& image, EntityId selectionBefore);
    void DrawImageCanvasRequirement(const WorldEntity* entity);
    void DrawButtonInspector(WorldEntity* entity);
    void DrawToggleInspector(WorldEntity* entity);
    void DrawSliderInspector(WorldEntity* entity);
    void DrawDropdownInspector(WorldEntity* entity);
    void DrawInputFieldInspector(WorldEntity* entity);
    void DrawMaterialOverrideInspector(WorldEntity* entity);
    void DrawConsolePanel();
    void DrawProjectSettingsWindow();
    void DrawProjectGeneralSettings();
    void DrawProjectPlayerSettings();
    void DrawProjectPhysicsSettings();
    void DrawProjectInputSettings();
    void DrawInputActionDialogs(Input* input);
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
    enum class UiEntityPreset : uint8_t {
        Canvas,
        Text,
        Image,
        Button,
        Toggle,
        Slider,
        Dropdown,
        InputField,
    };
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
    void AssignImageTexture(EntityId entity, const std::filesystem::path& path);
    void AssignTextFont(EntityId entity, const std::filesystem::path& path);
    void HandleSceneAssetDrop(const ImVec2& imageMin, const ImVec2& imageMax);
    void HandleSceneCameraControls(const ImVec2& imageMin, const ImVec2& imageMax,
                                   bool imageHovered);
    void UpdateSceneCameraNavigationState(bool imageHovered, bool& beginCapture);
    void ReadSceneCameraPointerDelta(const ImVec2& imageMin, const ImVec2& imageMax,
                                     bool beginCapture, float& pointerDeltaX,
                                     float& pointerDeltaY);
    void RotateSceneCamera(float pointerDeltaX, float pointerDeltaY);
    void MoveSceneCamera(bool imageHovered, float pointerDeltaX, float pointerDeltaY);
    bool FocusSceneCameraOnSelection();
    bool AlignSelectedCameraToSceneView();
    bool AlignSceneViewToSelectedCamera();
    void HandleSceneContextMenu(const ImVec2& imageMin, const ImVec2& imageMax,
                                bool imageHovered);
    void CreateEmptyEntity(const DirectX::XMFLOAT3& position, EntityId parent = {});
    void CreatePrimitiveEntity(MeshPrimitive primitive, const DirectX::XMFLOAT3& position,
                               EntityId parent = {});
    void CreateUiEntity(UiEntityPreset preset, EntityId parent = {});
    void DeleteSelection();
    void CreateModelEntityFromAsset(const std::filesystem::path& path,
                                    const DirectX::XMFLOAT3& position);
    bool SaveSelectionAsPrefab();
    bool TryPreparePrefabSave(EntityId& root, std::filesystem::path& destination);
    std::unordered_set<EntityId, EntityIdHash> CollectPrefabEntityIds(EntityId root) const;
    std::vector<WorldEntity> BuildPrefabEntities(
        EntityId root, const std::unordered_set<EntityId, EntityIdHash>& includedIds) const;
    void ClearExternalPrefabEntityReferences(
        WorldEntity& entity,
        const std::unordered_set<EntityId, EntityIdHash>& includedIds) const;
    bool SavePrefabEntities(std::vector<WorldEntity> entities,
                            const std::filesystem::path& destination);
    void SelectSavedPrefabAsset(const std::filesystem::path& destination);
    bool InstantiatePrefabAsset(
        const std::filesystem::path& path, EntityId parent = {},
        std::optional<DirectX::XMFLOAT3> position = std::nullopt);
    bool TryResolvePrefabAsset(const std::filesystem::path& path,
                               std::filesystem::path& resolved);
    bool TryLoadPrefabAsset(const std::filesystem::path& path, World& prefab);
    bool TryInstantiatePrefabWorld(const World& prefab, EntityId parent,
                                   std::vector<EntityId>& roots);
    void PositionInstantiatedPrefab(const std::vector<EntityId>& roots,
                                    const std::optional<DirectX::XMFLOAT3>& position);
    void SelectInstantiatedPrefab(const std::vector<EntityId>& roots);
    bool TryNormalizeModelAssetReference(const std::filesystem::path& path,
                                         std::string& assetPath);
    bool TryNormalizeTextureAssetReference(const std::filesystem::path& path,
                                           std::string& assetPath);
    bool TryNormalizeFontAssetReference(const std::filesystem::path& path,
                                        std::string& assetPath);
    bool TryNormalizeAudioAssetReference(const std::filesystem::path& path,
                                         std::string& assetPath);
    bool TryNormalizeScriptAssetReference(const std::filesystem::path& path,
                                          std::string& assetPath,
                                          std::filesystem::path& physicalPath);
    void RefreshAssetBrowser();
    void ResetAssetBrowserCache();
    void RefreshSceneAssetList();
    std::filesystem::path ResolveAssetBrowserDirectory();
    void RefreshAssetBrowserEntries(const std::filesystem::path& currentDirectory);
    void RefreshProjectAssetLists();
    void AddProjectAsset(const std::filesystem::path& physicalPath,
                         const std::filesystem::path& relativePath);
    void NavigateAssetBrowser(const std::filesystem::path& relativeDirectory);
    void DrawAssetBrowserBreadcrumbs();
    enum class AssetBrowserEntryKind : uint8_t {
        Directory,
        Prefab,
        Texture,
        Audio,
        Font,
        Script,
        ScriptHeader,
        Model,
    };
    AssetBrowserEntryKind ClassifyAssetBrowserEntry(const std::filesystem::path& relativePath,
                                                    bool directory) const;
    std::string BuildAssetBrowserEntryLabel(const std::filesystem::path& relativePath,
                                            AssetBrowserEntryKind kind) const;
    bool OpenAssetScriptSource(const std::filesystem::path& relativePath,
                               const std::string& logicalId);
    void ActivateAssetBrowserEntry(const std::filesystem::path& relativePath,
                                   const std::filesystem::path& logicalPath,
                                   AssetBrowserEntryKind kind);
    void DrawAssetBrowserEntryDragSource(const std::string& logicalId,
                                         AssetBrowserEntryKind kind);
    void DrawAssetTextureAssignmentMenu(const std::filesystem::path& logicalPath);
    void DrawAssetBrowserEntryContextMenu(const std::filesystem::path& relativePath,
                                          const std::filesystem::path& logicalPath,
                                          AssetBrowserEntryKind kind);
    void DrawAssetBrowserEntry(const std::filesystem::path& relativePath,
                               bool directory);
    void DrawSelectedAssetDetails();
    void DrawAssetPreviewPopup();
    [[nodiscard]] bool IsAssetPreviewPopupReady() const;
    void DrawAssetPreviewModelSummary(const Model& model) const;
    void DrawAssetPreviewAnimationControls(ModelManager& modelManager, Model& model);
    void EnsureAssetPreviewAnimation(ModelManager& modelManager, Model& model,
                                     const std::vector<std::string>& animationNames);
    void DrawAssetPreviewAnimationSelector(ModelManager& modelManager,
                                           const std::vector<std::string>& animationNames);
    void DrawAssetPreviewPlaybackControls(ModelManager& modelManager, Model& model);
    void SeekAssetPreviewAnimation(ModelManager& modelManager, Model& model, float duration,
                                   float time);
    void UpdateAssetPreviewAnimation(ModelManager& modelManager, Model& model);
    void DrawAssetPreviewSeekButtons(ModelManager& modelManager, Model& model, float duration);
    void DrawAssetPreviewTimeline(ModelManager& modelManager, Model& model, float duration);
    void AdvanceAssetPreviewAnimation(ModelManager& modelManager, const Model& model);
    void DrawAssetPreviewViewport(ModelManager& modelManager);
    void RenderAssetPreview(ModelManager& modelManager);
    void HandleAssetPreviewRotation();
    void DrawAudioAssetPreview(const std::filesystem::path& physicalPath);
    void SynchronizeAudioPreviewSelection();
    bool IsAudioPreviewPlaying(const ISoundService* sound) const;
    void DrawAudioPreviewControls(ISoundService* sound,
                                  const std::filesystem::path& physicalPath, bool playing);
    void StartAudioAssetPreview(ISoundService& sound,
                                const std::filesystem::path& physicalPath);
    void DrawAudioPreviewInfo(const ISoundService* sound) const;
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
    void SubmitRenderEntity(const WorldEntity& entity, ModelManager* models);
    void SubmitRenderMesh(const WorldEntity& entity, const ModelManager* models,
                          const Transform& transform, uint32_t meshId, uint32_t materialId,
                          uint32_t textureId, uint32_t normalTextureId,
                          const D3D12_VERTEX_BUFFER_VIEW* vertexBufferOverride);
    void ApplyMaterialOverride(RenderMeshItem& item,
                               const MaterialOverrideComponent& materialOverride) const;
    void BuildEditorOverlayScene();
    [[nodiscard]] bool DrawGameUi(int width, int height,
                                  bool gameCameraAvailable);
    bool PrepareGameUiFrame(int width, int height);
    bool CanPointAtGameUi(const ImVec2& imageScreenMin, const ImVec2& mouse,
                          bool uiEventsEnabled) const;
    DirectX::XMFLOAT2 CalculateGameUiPointer(const ImVec2& imageScreenMin, const ImVec2& mouse,
                                             int width, int height) const;
    bool CanNavigateGameUi(const EventSystemComponent* eventSystem, bool uiEventsEnabled) const;
    bool IsGameUiButtonInteractable(EntityId entity) const;
    bool IsGameUiSliderInteractable(EntityId entity) const;
    bool TryCalculateRuntimeGameUiCanvasLayout(
        const WorldEntity& entity, int width, int height, float& scale,
        DirectX::XMFLOAT2& origin, DirectX::XMFLOAT2& referenceResolution) const;
    bool TryCalculateRuntimeGameUiImageRect(const WorldEntity& entity, int width, int height,
                                            float& left, float& top, float& right,
                                            float& bottom) const;
    void SetGameUiSliderValue(WorldEntity& entity, float requestedValue);
    void SetGameUiSliderValueFromPointer(WorldEntity& entity, int width, int height,
                                         const DirectX::XMFLOAT2& pointer);
    void QueueGameUiInputFieldEvent(EntityId entity, const std::string& text, bool submitted);
    EntityId CollectGameUiControls(
        bool canPoint, const DirectX::XMFLOAT2& pointer, int width, int height,
        std::vector<EntityId>& selectableButtons,
        std::unordered_map<EntityId, DirectX::XMFLOAT2, EntityIdHash>& selectableCenters) const;
    WorldEntity* FindOpenGameUiDropdown();
    int32_t UpdateGameUiDropdownHover(WorldEntity* openDropdownEntity, bool canPoint,
                                      const DirectX::XMFLOAT2& pointer, int width, int height,
                                      EntityId& hoveredButton);
    WorldEntity* FindActiveGameUiInputField();
    void InitializeGameUiSelection(const std::vector<EntityId>& selectableButtons,
                                   const EventSystemComponent* eventSystem, bool uiEventsEnabled);
    void UpdateActiveGameUiInputField(WorldEntity* activeInputFieldEntity, bool canNavigateUi);
    bool NavigateGameUiTab(const std::vector<EntityId>& selectableButtons, bool canNavigateUi);
    bool NavigateOpenGameUiDropdown(WorldEntity* openDropdownEntity, bool canNavigateUi);
    void HandleGameUiPointerInteractions(EntityId hoveredButton, int32_t hoveredDropdownOption,
                                         bool hoveredButtonInteractable,
                                         bool hoveredSliderInteractable,
                                         const DirectX::XMFLOAT2& pointer, int width, int height);
    bool HandleGameUiSubmit(bool canNavigateUi);
    void HandleGameUiKeyboardSlider(bool canNavigateUi, bool navigatedUi);
    void HandleGameUiCancel(bool canNavigateUi);
    void DrawGameUiDropdownPopup(int width, int height);
    void DrawGameUiVisuals(int width, int height, EntityId hoveredButton, bool submitHeld);
    void PreserveGameUiImageAspect(Sprite& sprite, const ImageComponent& image,
                                   TextureHandle texture) const;
    void ApplyGameUiImageFill(Sprite& sprite, const ImageComponent& image) const;
    DirectX::XMFLOAT4 UpdateGameUiButtonColor(const WorldEntity& entity,
                                              bool groupInteractable,
                                              EntityId hoveredButton, bool submitHeld);
    void DrawGameUiToggle(const WorldEntity& entity, const Sprite& sprite, float groupAlpha);
    void DrawGameUiSlider(const WorldEntity& entity, const Sprite& sliderTrack, float scale,
                          float groupAlpha, bool groupInteractable);
    std::string ResolveGameUiDisplayText(const WorldEntity& entity) const;
    void DrawGameUiText(const WorldEntity& entity, float scale,
                        const DirectX::XMFLOAT2& canvasOrigin,
                        const DirectX::XMFLOAT2& referenceResolution, float groupAlpha);
    enum class UiNavigationDirection : uint8_t {
        None,
        Left,
        Right,
        Up,
        Down,
    };
    bool NavigateGameUiDirection(
        const std::vector<EntityId>& selectableButtons,
        const std::unordered_map<EntityId, DirectX::XMFLOAT2, EntityIdHash>& selectableCenters,
        bool canNavigateUi, bool focusedSlider, bool gameCameraAvailable, bool dropdownOpen);
    [[nodiscard]] UiNavigationDirection ReadGameUiNavigationDirection(
        bool canNavigateUi, bool focusedSlider, bool dropdownOpen) const;
    [[nodiscard]] bool IsGameUiDirectionalNavigationEnabled(
        bool canNavigateUi, bool focusedSlider, bool dropdownOpen) const;
    [[nodiscard]] EntityId ResolveExplicitGameUiNavigationTarget(
        const WorldEntity& control, UiNavigationDirection direction) const;
    void NavigateGameUiExplicit(
        const WorldEntity& control, UiNavigationDirection direction,
        const std::vector<EntityId>& selectableButtons);
    void NavigateGameUiAutomatic(
        UiNavigationDirection direction, const std::vector<EntityId>& selectableButtons,
        const std::unordered_map<EntityId, DirectX::XMFLOAT2, EntityIdHash>& selectableCenters);
    [[nodiscard]] EntityId FindBestGameUiNavigationTarget(
        UiNavigationDirection direction, const DirectX::XMFLOAT2& currentCenter,
        const std::vector<EntityId>& selectableButtons,
        const std::unordered_map<EntityId, DirectX::XMFLOAT2, EntityIdHash>& selectableCenters)
        const;
    [[nodiscard]] float CalculateGameUiNavigationScore(
        UiNavigationDirection direction, const DirectX::XMFLOAT2& currentCenter,
        const DirectX::XMFLOAT2& candidateCenter) const;
    bool TryCalculateGameUiCanvasLayout(
        const WorldEntity& entity, const ImVec2& imageMin, const ImVec2& imageMax, float& scale,
        DirectX::XMFLOAT2& origin, DirectX::XMFLOAT2& referenceResolution) const;
    bool TryCalculateGameUiRect(const WorldEntity& entity, const ImVec2& imageMin,
                                const ImVec2& imageMax, ImVec2& minimum,
                                ImVec2& maximum) const;
    bool TryCalculateGameUiImageRect(const WorldEntity& entity, const ImVec2& imageMin,
                                     const ImVec2& imageMax, ImVec2& minimum,
                                     ImVec2& maximum, float* canvasScale = nullptr) const;
    void UpdateGameUiDragAndResize(const ImVec2& imageMin, const ImVec2& imageMax);
    enum class UiResizeHandle : uint8_t;
    void ResetGameUiEditingState();
    EntityId FindHoveredGameUiEntity(const ImVec2& mouse, const ImVec2& imageMin,
                                     const ImVec2& imageMax) const;
    UiResizeHandle FindHoveredGameUiResizeHandle(const ImVec2& mouse,
                                                  const ImVec2& imageMin,
                                                  const ImVec2& imageMax) const;
    void HandleGameUiPointerInput(bool imageHovered, EntityId hovered,
                                  UiResizeHandle hoveredResizeHandle);
    void HandleGameUiKeyboardNudge(const ImVec2& imageMin, const ImVec2& imageMax);
    void DrawGameUiSelectionOverlay(const ImVec2& imageMin, const ImVec2& imageMax) const;
    void UpdateGameUiEditingCursor(EntityId hovered,
                                   UiResizeHandle hoveredResizeHandle) const;
    void HandleGameUiEditing(const ImVec2& imageMin,
                             const ImVec2& imageMax);
    void UpdateAssetPreview();
    void ResetAssetPreviewState(const std::filesystem::path& relative);
    bool TryResolveAssetPreviewPath(const std::filesystem::path& relative,
                                    std::filesystem::path& physical) const;
    const Model* LoadAssetPreviewModel(const std::filesystem::path& relative,
                                       const std::filesystem::path& physical);
    void FrameAssetPreviewModel(const Model& model);
    bool UpdateGameViewCamera();
    bool UpdateCameraFromEntity(EntityId entity, Camera& camera, int width, int height) const;
    bool DrawSelectedCameraPreview(const ImVec2& imageMin, const ImVec2& imageMax);
    const WorldEntity* ResolveSelectedCameraPreviewEntity() const;
    bool PrepareSelectedCameraPreviewRect(const WorldEntity& entity, const ImVec2& imageMin,
                                          const ImVec2& imageMax, ImVec2& previewMin,
                                          ImVec2& previewMax);
    void RenderSelectedCameraPreview();
    bool DrawSelectedCameraPreviewOverlay(const WorldEntity& entity, const ImVec2& previewMin,
                                          const ImVec2& previewMax) const;
    void PickSceneEntity(const ImVec2& imageMin, const ImVec2& imageMax, bool imageHovered);
    EntityId FindClosestSceneComponent(const ImVec2& imageMin, const ImVec2& imageMax,
                                       const ImVec2& mouse) const;
    bool TryRaycastSceneMesh(const ImVec2& imageMin, const ImVec2& imageMax, EntityId& picked) const;
    void ApplyScenePick(EntityId picked);
    void DrawSceneComponentGizmos(const ImVec2& imageMin, const ImVec2& imageMax) const;
    bool HasSceneComponentGizmo(const WorldEntity& entity) const;
    bool IsSceneComponentGizmoEnabled(const WorldEntity& entity) const;
    bool ShouldDrawScenePhysicsShapes(const WorldEntity& entity) const;
    uint32_t ResolveSceneComponentGizmoColor(const WorldEntity& entity,
                                             bool active, bool selected,
                                             bool enabled) const;
    void DrawSceneCameraGizmoIcon(const ImVec2& center, uint32_t color) const;
    void DrawSceneLightGizmoIcon(const ImVec2& center, uint32_t color) const;
    void DrawSceneAudioSourceGizmoIcon(const ImVec2& center, uint32_t color) const;
    void DrawSceneAudioListenerGizmoIcon(const ImVec2& center, uint32_t color) const;
    void DrawSceneComponentGizmoIcon(const WorldEntity& entity,
                                     const ImVec2& center, uint32_t color) const;
    void DrawScenePhysicsLayerLabel(const WorldEntity& entity,
                                    const ImVec2& center) const;
    void DrawSingleSceneComponentGizmo(const WorldEntity& entity,
                                       const ImVec2& imageMin,
                                       const ImVec2& imageMax) const;
    void DrawSceneActiveComponentGuides(const WorldEntity& entity,
                                        const DirectX::XMFLOAT4X4& worldMatrix,
                                        const ImVec2& imageMin, const ImVec2& imageMax,
                                        bool active) const;
    void DrawScenePhysicsGizmos(const WorldEntity& entity, const ImVec2& imageMin,
                                const ImVec2& imageMax, bool active,
                                bool drawPhysicsShapes) const;
    void DrawSceneSelectionOutline(const ImVec2& imageMin, const ImVec2& imageMax) const;
    void DrawSceneGizmoToolbar();
    bool DrawBoxColliderGizmo(const ImVec2& imageMin, const ImVec2& imageMax);
    bool DrawCharacterControllerGizmo(const ImVec2& imageMin, const ImVec2& imageMax);
    bool DrawSceneTransformGizmo(const ImVec2& imageMin, const ImVec2& imageMax);
    void ResolveMeshResources();
    void ResolveModels();
    void ResolveMaterialTextures();
    void ResolveUiTextures();
    void ResolveFonts();
    void ResolveLinearMaterialTextures();
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
    EntityId gameUiDragEntity_{};
    EntityId gameUiResizeEntity_{};
    enum class UiResizeHandle : uint8_t {
        None,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };
    UiResizeHandle gameUiResizeHandle_ = UiResizeHandle::None;
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
    EntityId focusedButton_{};
    EntityId pressedButton_{};
    EntityId activeSlider_{};
    EntityId openDropdown_{};
    int32_t dropdownHighlightedIndex_ = 0;
    EntityId activeInputField_{};
    bool runtimeInitialUiSelectionApplied_ = false;
    std::vector<EntityId> pendingButtonClicks_;
    struct SliderValueChange {
        EntityId entity{};
        float value = 0.0f;
    };
    std::vector<SliderValueChange> pendingSliderValueChanges_;
    struct DropdownValueChange {
        EntityId entity{};
        int32_t value = 0;
    };
    std::vector<DropdownValueChange> pendingDropdownValueChanges_;
    struct InputFieldEvent {
        EntityId entity{};
        std::string text;
        bool submitted = false;
    };
    std::vector<InputFieldEvent> pendingInputFieldEvents_;
    struct ButtonColorTransition {
        DirectX::XMFLOAT4 current{1.0f, 1.0f, 1.0f, 1.0f};
        DirectX::XMFLOAT4 start{1.0f, 1.0f, 1.0f, 1.0f};
        DirectX::XMFLOAT4 target{1.0f, 1.0f, 1.0f, 1.0f};
        float elapsed = 0.0f;
        bool initialized = false;
    };
    std::unordered_map<EntityId, ButtonColorTransition, EntityIdHash>
        buttonColorTransitions_;
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
    std::unordered_map<std::string, FontHandle> loadedFonts_;
    std::vector<std::filesystem::path> modelAssets_;
    std::vector<std::filesystem::path> textureAssets_;
    std::vector<std::filesystem::path> audioAssets_;
    std::vector<std::filesystem::path> fontAssets_;
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
