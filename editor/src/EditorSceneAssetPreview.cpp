#include "EditorScene.h"

#include "AssetImportPlanner.h"
#include "internal/EditorSceneViewportUtils.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "sound/ISoundService.h"

#include <algorithm>
#include <cmath>

using namespace EditorSceneViewportUtils;

void EditorScene::UpdateAssetPreview() {
    const std::filesystem::path relative = selectedAsset_.lexically_normal();
    if (relative == assetPreviewAsset_) {
        return;
    }
    ResetAssetPreviewState(relative);
    std::filesystem::path physical;
    if (!TryResolveAssetPreviewPath(relative, physical)) {
        return;
    }
    const Model* model = LoadAssetPreviewModel(relative, physical);
    if (model != nullptr) {
        FrameAssetPreviewModel(*model);
    }
}

void EditorScene::ResetAssetPreviewState(const std::filesystem::path& relative) {
    StopAudioAssetPreview();
    audioPreviewSoundId_ = ISoundService::kInvalidSoundId;
    assetPreviewAsset_ = relative;
    assetPreviewModel_ = {};
    assetPreviewAnimation_.clear();
    assetPreviewAnimationLoop_ = true;
    assetPreviewAnimationSpeed_ = 1.0f;
    assetPreviewRotationDegrees_ = {0.0f, 180.0f};
    assetPreviewPlan_.clear();
    assetPreviewError_.clear();
    assetPreviewTransform_ = {};
}

bool EditorScene::TryResolveAssetPreviewPath(const std::filesystem::path& relative,
                                             std::filesystem::path& physical) const {
    if (relative.empty() || ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        return false;
    }
    physical = assetRoot_ / relative;
    std::error_code error;
    return std::filesystem::is_regular_file(physical, error) && !error &&
           AssetImport::IsModelFile(physical);
}

const Model* EditorScene::LoadAssetPreviewModel(const std::filesystem::path& relative,
                                                const std::filesystem::path& physical) {
    if (!AssetImport::BuildPlan({physical}, assetPreviewPlan_, assetPreviewError_)) {
        status_ = "Asset preview dependency validation failed: " + assetPreviewError_;
        return nullptr;
    }
    const std::string previewKey = physical.lexically_normal().generic_string();
    const auto cachedPreview = assetPreviewModels_.find(previewKey);
    if (cachedPreview != assetPreviewModels_.end()) {
        assetPreviewModel_ = cachedPreview->second;
    } else {
        assetPreviewModel_ = ctx_->rendering.model->LoadUniqueHandle(physical.wstring());
        if (assetPreviewModel_.IsValid()) {
            assetPreviewModels_.emplace(previewKey, assetPreviewModel_);
        }
    }
    const Model* model = assetPreviewModel_.IsValid()
                             ? ctx_->rendering.model->GetModel(assetPreviewModel_)
                             : nullptr;
    if (model == nullptr) {
        assetPreviewError_ = "The selected model could not be loaded for preview.";
        status_ = "Asset preview failed for assets/" + relative.generic_string() +
                  ": model loading failed.";
    }
    return model;
}

void EditorScene::FrameAssetPreviewModel(const Model& model) {
    DirectX::XMFLOAT3 boundsMin{};
    DirectX::XMFLOAT3 boundsMax{};
    if (!TryGetModelBounds(model, boundsMin, boundsMax)) {
        assetPreviewCamera_.SetPosition({0.0f, 0.0f, -4.0f});
        assetPreviewCamera_.SetClipRange(0.01f, 1000.0f);
        return;
    }
    const DirectX::XMFLOAT3 center{
        (boundsMin.x + boundsMax.x) * 0.5f,
        (boundsMin.y + boundsMax.y) * 0.5f,
        (boundsMin.z + boundsMax.z) * 0.5f,
    };
    const float extentX = (boundsMax.x - boundsMin.x) * 0.5f;
    const float extentY = (boundsMax.y - boundsMin.y) * 0.5f;
    const float extentZ = (boundsMax.z - boundsMin.z) * 0.5f;
    const float radius =
        (std::max)(0.05f, std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ));
    const float distance =
        (std::max)(0.25f, radius / std::tan(assetPreviewCamera_.GetFovY() * 0.5f) * 1.25f);
    assetPreviewTransform_.position = {-center.x, -center.y, -center.z};
    assetPreviewCamera_.SetPosition({0.0f, 0.0f, -distance});
    assetPreviewCamera_.SetClipRange((std::max)(0.01f, distance - radius * 2.0f),
                                     distance + radius * 4.0f);
}
