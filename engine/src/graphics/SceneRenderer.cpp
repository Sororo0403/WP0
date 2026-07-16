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
                           const DirectX::XMFLOAT4& clearColor) {
    stats_ = {};
    if (!IsReady() || !surface.IsReady()) {
        return false;
    }

    surface.BeginScenePass(clearColor);
    meshRenderer_->PreDraw();
    for (const RenderMeshItem& item : scene.OpaqueMeshes()) {
        meshRenderer_->DrawMesh(*item.mesh, item.material, item.transform, camera, item.textureId,
                                item.normalTextureId);
        ++stats_.opaqueDraws;
    }
    for (const RenderMeshItem& item : scene.TransparentMeshes()) {
        meshRenderer_->DrawMesh(*item.mesh, item.material, item.transform, camera, item.textureId,
                                item.normalTextureId);
        ++stats_.transparentDraws;
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
