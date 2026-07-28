#include "AssetImportPlanner.h"
#include "EditorScene.h"
#include "font/TextRenderer.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "texture/TextureManager.h"

#include <algorithm>
#include <iterator>
#include <ranges>

namespace {

Transform DecomposeRenderTransform(const DirectX::XMFLOAT4X4& matrix) {
    using namespace DirectX;
    XMVECTOR scale;
    XMVECTOR rotation;
    XMVECTOR translation;
    Transform result{};
    if (XMMatrixDecompose(&scale, &rotation, &translation, XMLoadFloat4x4(&matrix))) {
        XMStoreFloat3(&result.scale, scale);
        XMStoreFloat4(&result.rotation, rotation);
        XMStoreFloat3(&result.position, translation);
    }
    return result;
}

int32_t ToBlendMode(MaterialSurfaceBlendMode blendMode) {
    switch (blendMode) {
        case MaterialSurfaceBlendMode::Opaque:
            return static_cast<int32_t>(BlendMode::Opaque);
        case MaterialSurfaceBlendMode::Cutout:
            return static_cast<int32_t>(BlendMode::Cutout);
        case MaterialSurfaceBlendMode::Transparent:
            return static_cast<int32_t>(BlendMode::Transparent);
    }
    return static_cast<int32_t>(BlendMode::Opaque);
}

int32_t ToCullMode(MaterialSurfaceCullMode cullMode) {
    switch (cullMode) {
        case MaterialSurfaceCullMode::None:
            return static_cast<int32_t>(MaterialCullMode::None);
        case MaterialSurfaceCullMode::Front:
            return static_cast<int32_t>(MaterialCullMode::Front);
        case MaterialSurfaceCullMode::Back:
            return static_cast<int32_t>(MaterialCullMode::Back);
    }
    return static_cast<int32_t>(MaterialCullMode::Back);
}

int32_t ToPbrTexturePacking(MaterialPbrTexturePacking packing) {
    switch (packing) {
        case MaterialPbrTexturePacking::Separate:
            return static_cast<int32_t>(PbrTexturePacking::Separate);
        case MaterialPbrTexturePacking::OcclusionRoughnessMetallic:
            return static_cast<int32_t>(PbrTexturePacking::OcclusionRoughnessMetallic);
        case MaterialPbrTexturePacking::MetallicRoughness:
            return static_cast<int32_t>(PbrTexturePacking::MetallicRoughness);
    }
    return static_cast<int32_t>(PbrTexturePacking::Separate);
}

} // namespace

void EditorScene::ResolveMeshResources() {
    if (ctx_ == nullptr || ctx_->rendering.model == nullptr || ctx_->rendering.texture == nullptr) {
        return;
    }
    ResolveModels();
    ResolveMaterialTextures();
    ResolveUiTextures();
    ResolveFonts();
    ResolveLinearMaterialTextures();
}

void EditorScene::ResolveModels() {
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.meshRenderer || entity.meshRenderer->sourceType != MeshSourceType::Model ||
            entity.meshRenderer->modelPath.empty() ||
            loadedModels_.contains(entity.meshRenderer->modelPath) ||
            !ResolveProjectAssetPath(entity.meshRenderer->modelPath)) {
            continue;
        }
        loadedModels_.emplace(entity.meshRenderer->modelPath,
                              ctx_->rendering.model->LoadHandle(
                                  std::filesystem::path(entity.meshRenderer->modelPath).wstring()));
    }
}

void EditorScene::ResolveMaterialTextures() {
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.materialOverride || entity.materialOverride->baseColorTexturePath.empty() ||
            loadedTextures_.contains(entity.materialOverride->baseColorTexturePath)) {
            continue;
        }
        const std::optional<std::filesystem::path> resolved =
            ResolveProjectAssetPath(entity.materialOverride->baseColorTexturePath);
        if (!resolved || !AssetImport::IsTextureFile(*resolved)) {
            continue;
        }
        loadedTextures_.emplace(
            entity.materialOverride->baseColorTexturePath,
            TextureHandle(ctx_->rendering.texture->LoadSrgb(resolved->wstring())));
    }
}

void EditorScene::ResolveUiTextures() {
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.image || entity.image->texturePath.empty() ||
            loadedTextures_.contains(entity.image->texturePath)) {
            continue;
        }
        const std::optional<std::filesystem::path> resolved =
            ResolveProjectAssetPath(entity.image->texturePath);
        if (!resolved || !AssetImport::IsTextureFile(*resolved)) {
            continue;
        }
        loadedTextures_.emplace(
            entity.image->texturePath,
            TextureHandle(ctx_->rendering.texture->LoadSrgb(resolved->wstring())));
    }
}

void EditorScene::ResolveFonts() {
    if (ctx_->rendering.font == nullptr) {
        return;
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.text || entity.text->fontPath.empty() ||
            loadedFonts_.contains(entity.text->fontPath)) {
            continue;
        }
        const std::optional<std::filesystem::path> resolved =
            ResolveProjectAssetPath(entity.text->fontPath);
        if (!resolved || !AssetImport::IsFontFile(*resolved)) {
            continue;
        }
        loadedFonts_.emplace(entity.text->fontPath,
                             ctx_->rendering.font->LoadFont(resolved->wstring()));
    }
}

void EditorScene::ResolveLinearMaterialTextures() {
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.materialOverride) {
            continue;
        }
        const std::string* paths[] = {
            &entity.materialOverride->normalTexturePath,
            &entity.materialOverride->roughnessTexturePath,
            &entity.materialOverride->metallicTexturePath,
        };
        for (const std::string* path : paths) {
            if (path->empty() || loadedLinearTextures_.contains(*path)) {
                continue;
            }
            const std::optional<std::filesystem::path> resolved = ResolveProjectAssetPath(*path);
            if (!resolved || !AssetImport::IsTextureFile(*resolved)) {
                continue;
            }
            loadedLinearTextures_.emplace(
                *path, TextureHandle(ctx_->rendering.texture->LoadLinear(resolved->wstring())));
        }
    }
}

ModelHandle EditorScene::ResolveModel(const MeshRendererComponent& component) const {
    if (component.sourceType == MeshSourceType::Primitive) {
        const size_t index = static_cast<size_t>(component.primitive);
        return index < std::size(primitiveModels_) ? primitiveModels_[index] : ModelHandle{};
    }
    const auto found = loadedModels_.find(component.modelPath);
    return found != loadedModels_.end() ? found->second : ModelHandle{};
}

TextureHandle EditorScene::ResolveBaseColorTexture(
    const MaterialOverrideComponent& component) const {
    const auto found = loadedTextures_.find(component.baseColorTexturePath);
    return found != loadedTextures_.end() ? found->second : TextureHandle{};
}

TextureHandle EditorScene::ResolveNormalTexture(const MaterialOverrideComponent& component) const {
    return ResolveLinearTexture(component.normalTexturePath);
}

TextureHandle EditorScene::ResolveLinearTexture(const std::string& path) const {
    const auto found = loadedLinearTextures_.find(path);
    return found != loadedLinearTextures_.end() ? found->second : TextureHandle{};
}

void EditorScene::BuildRenderScene() {
    renderScene_.BeginFrame();
    ModelManager* models = ctx_ ? ctx_->rendering.model : nullptr;
    if (models == nullptr) {
        return;
    }
    for (const WorldEntity& entity : world_.Entities()) {
        SubmitRenderEntity(entity, models);
    }
}

void EditorScene::SubmitRenderEntity(const WorldEntity& entity, ModelManager* models) {
    if (!world_.IsActiveInHierarchy(entity.id) || !entity.meshRenderer ||
        !entity.meshRenderer->enabled || !entity.materialOverride ||
        !entity.materialOverride->enabled) {
        return;
    }
    const auto runtimeAnimator =
        std::ranges::find_if(runtimeAnimators_, [&entity](const RuntimeAnimator& runtime) {
            return runtime.entity == entity.id;
        });
    const bool runtimeAnimated = runtimeAnimator != runtimeAnimators_.end();
    const bool editPreviewAnimated = !runtimeAnimated && editAnimatorPreviewEntity_ == entity.id &&
                                     editAnimatorPreviewModel_.IsValid();
    const bool animated = runtimeAnimated || editPreviewAnimated;
    const ModelHandle handle = runtimeAnimated       ? runtimeAnimator->model
                               : editPreviewAnimated ? editAnimatorPreviewModel_
                                                     : ResolveModel(*entity.meshRenderer);
    const Model* model = handle.IsValid() ? models->GetModel(handle) : nullptr;
    DirectX::XMFLOAT4X4 worldMatrix{};
    if (model == nullptr || !world_.TryGetWorldMatrix(entity.id, worldMatrix)) {
        return;
    }
    if (animated) {
        models->PrepareSkinning(handle);
    }
    DirectX::XMMATRIX renderWorld = DirectX::XMLoadFloat4x4(&worldMatrix);
    if (animated && model->hasRootAnimation) {
        renderWorld = DirectX::XMLoadFloat4x4(&model->rootAnimationMatrix) * renderWorld;
    }
    DirectX::XMStoreFloat4x4(&worldMatrix, renderWorld);
    const Transform transform = DecomposeRenderTransform(worldMatrix);
    if (!model->subMeshes.empty()) {
        for (const ModelSubMesh& subMesh : model->subMeshes) {
            const D3D12_VERTEX_BUFFER_VIEW* animatedVertices =
                animated && subMesh.skinCluster.skinnedVertexResource
                    ? &subMesh.skinCluster.skinnedVertexBufferView
                    : nullptr;
            SubmitRenderMesh(entity, models, transform, subMesh.meshId, subMesh.materialId,
                             subMesh.textureId, subMesh.normalTextureId, animatedVertices);
        }
        return;
    }
    SubmitRenderMesh(entity, models, transform, model->meshId, model->materialId, model->textureId,
                     kInvalidResourceId, nullptr);
}

void EditorScene::SubmitRenderMesh(const WorldEntity& entity, const ModelManager* models,
                                   const Transform& transform, uint32_t meshId, uint32_t materialId,
                                   uint32_t textureId, uint32_t normalTextureId,
                                   const D3D12_VERTEX_BUFFER_VIEW* vertexBufferOverride) {
    if (!IsValidResourceId(meshId)) {
        return;
    }
    RenderMeshItem item{};
    item.mesh = &models->GetMesh(meshId);
    if (IsValidResourceId(materialId)) {
        item.material = models->GetMaterial(materialId);
    }
    ApplyMaterialOverride(item, *entity.materialOverride);
    item.transform = transform;
    if (!IsValidResourceId(item.textureId)) {
        item.textureId = textureId;
    }
    if (!IsValidResourceId(item.normalTextureId)) {
        item.normalTextureId = normalTextureId;
    }
    item.objectId = static_cast<uint32_t>(EntityIdHash{}(entity.id));
    if (vertexBufferOverride != nullptr) {
        item.vertexBufferOverride = *vertexBufferOverride;
    }
    renderScene_.SubmitMesh(item);
}

void EditorScene::ApplyMaterialOverride(RenderMeshItem& item,
                                        const MaterialOverrideComponent& materialOverride) const {
    item.material.color = materialOverride.baseColor;
    item.material.metallic = materialOverride.metallic;
    item.material.roughness = materialOverride.roughness;
    item.material.normalStrength = materialOverride.normalStrength;
    item.material.blendMode = ToBlendMode(materialOverride.blendMode);
    item.material.alphaCutoff = materialOverride.alphaCutoff;
    item.material.cullMode = ToCullMode(materialOverride.cullMode);
    item.material.depthWrite = materialOverride.depthWrite ? 1 : 0;

    const TextureHandle baseColorTexture = ResolveBaseColorTexture(materialOverride);
    if (baseColorTexture.IsValid()) {
        item.textureId = baseColorTexture.Get();
        item.material.baseColorTextureId = baseColorTexture.Get();
        item.material.enableTexture = 1;
    }
    const TextureHandle normalTexture = ResolveNormalTexture(materialOverride);
    if (normalTexture.IsValid()) {
        item.normalTextureId = normalTexture.Get();
        item.material.normalTextureId = normalTexture.Get();
        item.material.enableNormalMap = 1;
    }
    const TextureHandle roughnessTexture =
        ResolveLinearTexture(materialOverride.roughnessTexturePath);
    if (roughnessTexture.IsValid()) {
        item.material.roughnessTextureId = roughnessTexture.Get();
    }
    const TextureHandle metallicTexture =
        ResolveLinearTexture(materialOverride.metallicTexturePath);
    if (metallicTexture.IsValid()) {
        item.material.metallicTextureId = metallicTexture.Get();
    }
    item.material.pbrTexturePacking = ToPbrTexturePacking(materialOverride.pbrTexturePacking);
}

void EditorScene::BuildEditorOverlayScene() {
    editorOverlayScene_.BeginFrame();
    if (!showSceneGrid_ || !IsValidResourceId(sceneGridPipelineId_) || ctx_ == nullptr ||
        ctx_->rendering.model == nullptr) {
        return;
    }
    ModelManager* models = ctx_->rendering.model;
    const ModelHandle planeHandle = primitiveModels_[static_cast<size_t>(MeshPrimitive::Plane)];
    const Model* plane = planeHandle.IsValid() ? models->GetModel(planeHandle) : nullptr;
    if (plane == nullptr || !IsValidResourceId(plane->meshId)) {
        return;
    }

    RenderMeshItem grid{};
    grid.mesh = &models->GetMesh(plane->meshId);
    grid.material.color = {1.0f, 1.0f, 1.0f, 0.45f};
    grid.material.enableTexture = 0;
    grid.material.blendMode = static_cast<int32_t>(BlendMode::Transparent);
    grid.material.cullMode = static_cast<int32_t>(MaterialCullMode::None);
    grid.material.depthWrite = 0;
    grid.transform.scale = {100.0f, 100.0f, 1.0f};
    DirectX::XMStoreFloat4(&grid.transform.rotation,
                           DirectX::XMQuaternionRotationAxis(
                               DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), -DirectX::XM_PIDIV2));
    grid.pipelineId = sceneGridPipelineId_;
    grid.flags = RenderObjectFlags::Transparent;
    editorOverlayScene_.SubmitMesh(grid);
}
