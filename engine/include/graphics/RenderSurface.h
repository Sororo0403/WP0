#pragma once

#include <DirectXMath.h>
#include <d3d12.h>
#include <memory>

class DirectXCommon;
class SrvManager;

/// <summary>
/// 1つの描画Viewが所有する、SwapChainから独立した描画面。
/// HDR SceneColor、Depth、UI表示用LDR Outputをまとめて管理する。
/// </summary>
class RenderSurface {
public:
    RenderSurface();
    ~RenderSurface();

    RenderSurface(const RenderSurface&) = delete;
    RenderSurface& operator=(const RenderSurface&) = delete;

    bool Initialize(DirectXCommon* dxCommon, SrvManager* srvManager, int width, int height);
    bool Resize(int width, int height);
    void ReleaseCompletedFrameResources();
    bool Release();
    bool Release(bool allowFrameAbort);

    void BeginScenePass(const DirectX::XMFLOAT4& clearColor);
    void EndScenePass();
    void BeginOutputPass(const DirectX::XMFLOAT4& clearColor);
    void EndOutputPass();
    void TransitionDepthToShaderResource();
    void TransitionDepthToWrite();

    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE GetSceneColorGpuHandle() const;
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE GetDepthGpuHandle() const;
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuHandle() const;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE GetOutputRtvHandle() const;
    [[nodiscard]] int GetWidth() const;
    [[nodiscard]] int GetHeight() const;
    [[nodiscard]] bool IsReady() const;

private:
    struct State;

    bool CreateResources(int width, int height);
    bool Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& current,
                    D3D12_RESOURCE_STATES next);
    void ApplyViewportAndScissor() const;

    DirectXCommon* dxCommon_ = nullptr;
    SrvManager* srvManager_ = nullptr;
    std::unique_ptr<State> state_;
};
