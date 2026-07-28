#include "AssetImportPlanner.h"
#include "EditorScene.h"
#include "PlayerPackageBuilder.h"
#include "PlayerProjectValidator.h"
#include "ProjectDescriptor.h"
#include "RuntimeSceneLoader.h"
#include "ScriptAsset.h"
#include "ScriptBuildService.h"
#include "core/AssetManager.h"
#include "core/MathUtils.h"
#include "core/WinApp.h"
#include "font/TextRenderer.h"
#include "graphics/DirectXCommon.h"
#include "graphics/LightingScene.h"
#include "graphics/RenderScene.h"
#include "graphics/ShaderPaths.h"
#include "graphics/SrvManager.h"
#include "imgui.h"
#include "imgui/ImguiManager.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "input/Input.h"
#include "internal/EditorSceneViewportUtils.h"
#include "model/MeshRenderer.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "sound/ISoundService.h"
#include "sprite/SpriteRenderer.h"
#include "texture/TextureManager.h"
#include "world/WorldCollision.h"
#include "world/WorldSerializer.h"

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>

#ifdef DrawText
#undef DrawText
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

void EditorScene::Update() {
    UpdateEditorSimulation();
    UpdateSceneViewResources();
    UpdateGameViewResources();
    UpdatePreviewResources();
}

void EditorScene::UpdateEditorSimulation() {
    if (!playerMode_) {
        UpdateScriptCompilation();
    }
    if (playModeState_ == PlayModeState::Playing && ctx_ != nullptr) {
        UpdateRuntimeWorld(ctx_->frame.deltaTime);
    } else if (playModeState_ == PlayModeState::Edit && ctx_ != nullptr) {
        UpdateEditAnimatorPreview(ctx_->frame.deltaTime);
    }
    ResolveMeshResources();
}

void EditorScene::UpdateSceneViewResources() {
    if (sceneViewSurface_.IsReady() && sceneViewPostProcess_.IsReady() && ctx_ != nullptr &&
        ctx_->rendering.dxCommon != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording() &&
        (requestedSceneWidth_ != sceneViewSurface_.GetWidth() ||
         requestedSceneHeight_ != sceneViewSurface_.GetHeight())) {
        const int width = (std::max)(1, requestedSceneWidth_);
        const int height = (std::max)(1, requestedSceneHeight_);
        if (sceneViewSurface_.Resize(width, height) &&
            sceneViewPostProcess_.Resize(width, height)) {
            sceneViewCamera_.SetAspect(static_cast<float>(width) / static_cast<float>(height));
        } else {
            status_ = "Scene View resize failed.";
        }
    }
    if (!postProcessInitializationAttempted_ && sceneViewSurface_.IsReady() && ctx_ != nullptr &&
        ctx_->rendering.dxCommon != nullptr && ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        postProcessInitializationAttempted_ = true;
        sceneViewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                         sceneViewSurface_.GetWidth(),
                                         sceneViewSurface_.GetHeight());
        if (!sceneViewPostProcess_.IsReady()) {
            status_ = "Scene View PostProcess initialization failed.";
        }
    }
}

void EditorScene::UpdateGameViewResources() {
    if (gameViewSurface_.IsReady() && gameViewPostProcess_.IsReady() && ctx_ != nullptr &&
        ctx_->rendering.dxCommon != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording() &&
        (requestedGameWidth_ != gameViewSurface_.GetWidth() ||
         requestedGameHeight_ != gameViewSurface_.GetHeight())) {
        const int width = (std::max)(1, requestedGameWidth_);
        const int height = (std::max)(1, requestedGameHeight_);
        if (!gameViewSurface_.Resize(width, height) ||
            !gameViewPostProcess_.Resize(width, height)) {
            status_ = "Game View resize failed.";
        }
    }
    if (!gamePostProcessInitializationAttempted_ && gameViewSurface_.IsReady() && ctx_ != nullptr &&
        ctx_->rendering.dxCommon != nullptr && ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        gamePostProcessInitializationAttempted_ = true;
        gameViewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                        gameViewSurface_.GetWidth(), gameViewSurface_.GetHeight());
        if (!gameViewPostProcess_.IsReady()) {
            status_ = "Game View PostProcess initialization failed.";
        }
    }
}

void EditorScene::UpdatePreviewResources() {
    if (!cameraPreviewPostProcessInitializationAttempted_ && cameraPreviewSurface_.IsReady() &&
        ctx_ != nullptr && ctx_->rendering.dxCommon != nullptr && ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        cameraPreviewPostProcessInitializationAttempted_ = true;
        cameraPreviewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                             cameraPreviewSurface_.GetWidth(),
                                             cameraPreviewSurface_.GetHeight());
        if (!cameraPreviewPostProcess_.IsReady()) {
            status_ = "Camera Preview PostProcess initialization failed.";
        }
    }
    if (!assetPreviewPostProcessInitializationAttempted_ && assetPreviewSurface_.IsReady() &&
        ctx_ != nullptr && ctx_->rendering.dxCommon != nullptr && ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        assetPreviewPostProcessInitializationAttempted_ = true;
        assetPreviewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                            assetPreviewSurface_.GetWidth(),
                                            assetPreviewSurface_.GetHeight());
        if (!assetPreviewPostProcess_.IsReady()) {
            status_ = "Asset Preview PostProcess initialization failed.";
        }
    }
    UpdateAssetPreview();
}
