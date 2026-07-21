#pragma once

#include <DirectXMath.h>
#include <cstdint>

class Camera;
class MeshRenderer;
class RenderScene;
class RenderSurface;

struct SceneRendererStats {
    uint32_t opaqueDraws = 0;
    uint32_t transparentDraws = 0;
};

/// <summary>
/// RenderSceneを指定されたRenderSurfaceへ描画する。
/// SwapChainやEditor UIには依存しない。
/// </summary>
class SceneRenderer {
public:
    void Initialize(MeshRenderer* meshRenderer);
    bool Render(const RenderScene& scene, const Camera& camera, RenderSurface& surface,
                const DirectX::XMFLOAT4& clearColor,
                const RenderScene* depthTestedOverlay = nullptr);

    [[nodiscard]] bool IsReady() const;
    [[nodiscard]] const SceneRendererStats& GetStats() const;

private:
    MeshRenderer* meshRenderer_ = nullptr;
    SceneRendererStats stats_{};
};
