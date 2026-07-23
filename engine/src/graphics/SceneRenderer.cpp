#include "graphics/SceneRenderer.h"

#include "camera/Camera.h"
#include "graphics/RenderScene.h"
#include "graphics/RenderSurface.h"
#include "model/MeshRenderer.h"

void SceneRenderer::Initialize(MeshRenderer* meshRenderer) {
    meshRenderer_ = meshRenderer;
    stats_ = {};
}

bool SceneRenderer::Render(const RenderScene& scene, const Camera& camera, RenderSurface& surface,
                           const DirectX::XMFLOAT4& clearColor,
                           const RenderScene* depthTestedOverlay) {
    stats_ = {};
    if (!IsReady() || !surface.IsReady()) {
        return false;
    }

    surface.BeginScenePass(clearColor);
    meshRenderer_->PreDraw();
    auto drawItems = [&](const RenderScene& source, bool includeInStats) {
        for (const RenderMeshItem& item : source.OpaqueMeshes()) {
            if (item.vertexBufferOverride.BufferLocation != 0) {
                meshRenderer_->DrawMeshWithVertexBuffer(
                    *item.mesh, item.vertexBufferOverride, item.material, item.transform, camera,
                    item.textureId, item.normalTextureId);
            } else if (IsValidResourceId(item.pipelineId)) {
                meshRenderer_->DrawMeshWithPipeline(item.pipelineId, *item.mesh, item.material,
                                                    item.transform, camera, item.textureId,
                                                    item.normalTextureId);
            } else {
                meshRenderer_->DrawMesh(*item.mesh, item.material, item.transform, camera,
                                        item.textureId, item.normalTextureId);
            }
            stats_.opaqueDraws += includeInStats ? 1u : 0u;
        }
        for (const RenderMeshItem& item : source.TransparentMeshes()) {
            if (item.vertexBufferOverride.BufferLocation != 0) {
                meshRenderer_->DrawMeshWithVertexBuffer(
                    *item.mesh, item.vertexBufferOverride, item.material, item.transform, camera,
                    item.textureId, item.normalTextureId);
            } else if (IsValidResourceId(item.pipelineId)) {
                meshRenderer_->DrawMeshWithPipeline(item.pipelineId, *item.mesh, item.material,
                                                    item.transform, camera, item.textureId,
                                                    item.normalTextureId);
            } else {
                meshRenderer_->DrawMesh(*item.mesh, item.material, item.transform, camera,
                                        item.textureId, item.normalTextureId);
            }
            stats_.transparentDraws += includeInStats ? 1u : 0u;
        }
    };
    drawItems(scene, true);
    if (depthTestedOverlay != nullptr) {
        drawItems(*depthTestedOverlay, false);
    }
    meshRenderer_->PostDraw();
    surface.EndScenePass();
    return true;
}

bool SceneRenderer::IsReady() const {
    return meshRenderer_ != nullptr && meshRenderer_->IsReady();
}

const SceneRendererStats& SceneRenderer::GetStats() const {
    return stats_;
}
