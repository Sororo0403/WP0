#include "EditorScene.h"

#include "ScriptBuildService.h"
#include "graphics/ShaderPaths.h"
#include "imgui/ImguiManager.h"
#include "model/MeshRenderer.h"
#include "model/ModelManager.h"
#include "texture/TextureManager.h"

void EditorScene::Initialize(const SceneContext& ctx) {
    BaseScene::Initialize(ctx);
    InitializeInputSettings(ctx);
    InitializeProjectScripts(ctx);
    InitializeDocking(ctx);
    if (!InitializeViewSurfaces(ctx) || !InitializeRenderingResources(ctx)) {
        return;
    }
    InitializeEditorCameras();
    RefreshAssetBrowser();
    ResolveMeshResources();
    InitializeScriptMonitoring();
    if (playerMode_) {
        EnterPlayMode();
    }
}

void EditorScene::InitializeInputSettings(const SceneContext& ctx) {
    std::string error;
    if (ctx.systems.input == nullptr || !inputSettingsStore_.Load(*ctx.systems.input, error)) {
        status_ = "Warning: Could not load Input Settings: " +
                  (error.empty() ? std::string("Input service is unavailable.") : error);
    }
}

void EditorScene::InitializeProjectScripts(const SceneContext& ctx) {
    std::string moduleError;
    if ((!playerMode_ && !ScriptBuildService::BuildIfNeeded(projectRoot_, moduleError)) ||
        !projectScripts_.Load(projectRoot_, ctx.systems.input, behaviorRegistry_, moduleError)) {
        status_ = "Error: " + moduleError;
        return;
    }
    std::string requirementError;
    if (!ValidateWorldBehaviorRequirements(&requirementError)) {
        status_ = "Error: Scene contains an invalid Behavior: " + requirementError;
        return;
    }
    const size_t upgradedReferences = UpgradeInputActionReferences();
    if (upgradedReferences != 0u) {
        status_ = "Upgraded " + std::to_string(upgradedReferences) +
                  " Input Action reference(s) to stable IDs.";
    }
}

void EditorScene::InitializeDocking(const SceneContext& ctx) {
    if (ctx.systems.imgui == nullptr || !ctx.systems.imgui->ConfigureDocking(imguiSettingsPath_)) {
        status_ = "Could not configure the Editor docking layout.";
    }
}

bool EditorScene::InitializeViewSurfaces(const SceneContext& ctx) {
    if (ctx.rendering.dxCommon == nullptr || ctx.rendering.srv == nullptr ||
        !sceneViewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 960, 540)) {
        status_ = "Scene View RenderSurface initialization failed.";
        return false;
    }
    if (!gameViewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 960, 540)) {
        status_ = "Game View RenderSurface initialization failed.";
    }
    if (!cameraPreviewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 320, 180)) {
        status_ = "Camera Preview RenderSurface initialization failed.";
    }
    if (!assetPreviewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 320, 320)) {
        status_ = "Asset Preview RenderSurface initialization failed.";
    }
    return true;
}

bool EditorScene::InitializeRenderingResources(const SceneContext& ctx) {
    if (ctx.rendering.model == nullptr || ctx.rendering.meshRenderer == nullptr ||
        ctx.rendering.texture == nullptr) {
        status_ = "Scene View rendering services are unavailable.";
        return false;
    }
    sceneRenderer_.Initialize(ctx.rendering.meshRenderer);
    sceneGridPipelineId_ =
        ctx.rendering.meshRenderer->CreatePipeline(ShaderPaths::MeshVS, ShaderPaths::EditorGridPS);
    Material material{};
    material.enableTexture = 0;
    const uint32_t whiteTexture = ctx.rendering.texture->GetWhiteTextureId();
    primitiveModels_[static_cast<size_t>(MeshPrimitive::Box)] =
        ctx.rendering.model->CreateBoxHandle(whiteTexture, material);
    primitiveModels_[static_cast<size_t>(MeshPrimitive::Sphere)] =
        ctx.rendering.model->CreateSphereHandle(whiteTexture, material);
    primitiveModels_[static_cast<size_t>(MeshPrimitive::Plane)] =
        ctx.rendering.model->CreatePlaneHandle(whiteTexture, material);
    primitiveModels_[static_cast<size_t>(MeshPrimitive::Cylinder)] =
        ctx.rendering.model->CreateCylinderHandle(whiteTexture, material);
    return true;
}

void EditorScene::InitializeEditorCameras() {
    sceneViewCamera_.SetPosition({0.0f, 0.35f, -4.0f});
    sceneViewCamera_.SetRotation({0.08f, 0.0f, 0.0f});
    sceneViewCamera_.Initialize(960.0f / 540.0f);
    gameViewCamera_.Initialize(960.0f / 540.0f);
    cameraPreviewCamera_.Initialize(16.0f / 9.0f);
    assetPreviewCamera_.SetPosition({0.0f, 0.0f, -4.0f});
    assetPreviewCamera_.SetRotation({0.0f, 0.0f, 0.0f});
    assetPreviewCamera_.Initialize(1.0f);
}
