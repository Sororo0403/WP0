#include "graphics/RenderSurface.h"

#include "core/Numeric.h"
#include "graphics/DirectXCommon.h"
#include "graphics/DxHelpers.h"
#include "graphics/GpuResourceHelpers.h"
#include "graphics/GpuResourceLifetime.h"
#include "graphics/SrvManager.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kSceneRtvIndex = 0;
constexpr UINT kOutputRtvIndex = 1;
constexpr UINT kRtvCount = 2;
constexpr UINT kInvalidSrv = (std::numeric_limits<UINT>::max)();
constexpr D3D12_RESOURCE_STATES kShaderReadState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

struct BuiltResources {
    ComPtr<ID3D12Resource> sceneColor;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    UINT sceneSrv = kInvalidSrv;
    UINT depthSrv = kInvalidSrv;
    UINT outputSrv = kInvalidSrv;
};

bool CreateColorResource(ID3D12Device* device, int width, int height, DXGI_FORMAT format,
                         const DirectX::XMFLOAT4& clearColor, ID3D12Resource** resource) {
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
        format, static_cast<UINT64>(width), static_cast<UINT>(height), 1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = format;
    clear.Color[0] = clearColor.x;
    clear.Color[1] = clearColor.y;
    clear.Color[2] = clearColor.z;
    clear.Color[3] = clearColor.w;
    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    return GpuResourceHelpers::CreateCommittedResourceChecked(
        device, &heap, D3D12_HEAP_FLAG_NONE, &desc, kShaderReadState, &clear, resource);
}

bool CreateDepthResource(ID3D12Device* device, int width, int height,
                         ID3D12Resource** resource) {
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DirectXCommon::kDepthResourceFormat, static_cast<UINT64>(width),
        static_cast<UINT>(height), 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DirectXCommon::kDepthStencilFormat;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;
    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    return GpuResourceHelpers::CreateCommittedResourceChecked(
        device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
        resource);
}

D3D12_CPU_DESCRIPTOR_HANDLE OffsetHandle(D3D12_CPU_DESCRIPTOR_HANDLE start, UINT index,
                                         UINT increment) {
    start.ptr += static_cast<SIZE_T>(index) * increment;
    return start;
}
} // namespace

struct RenderSurface::State {
    ComPtr<ID3D12Resource> sceneColor;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    UINT rtvIncrement = 0;
    UINT sceneSrv = kInvalidSrv;
    UINT depthSrv = kInvalidSrv;
    UINT outputSrv = kInvalidSrv;
    int width = 0;
    int height = 0;
    D3D12_RESOURCE_STATES sceneState = kShaderReadState;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES outputState = kShaderReadState;
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    std::vector<std::vector<BuiltResources>> frameDeferredResources;
};

RenderSurface::RenderSurface() : state_(std::make_unique<State>()) {}

RenderSurface::~RenderSurface() {
    Release(true);
}

bool RenderSurface::Initialize(DirectXCommon* dxCommon, SrvManager* srvManager, int width,
                               int height) {
    if (dxCommon == nullptr || dxCommon->GetDevice() == nullptr || srvManager == nullptr ||
        width <= 0 || height <= 0) {
        return false;
    }
    if (!Release()) {
        return false;
    }
    dxCommon_ = dxCommon;
    srvManager_ = srvManager;
    try {
        state_->frameDeferredResources.resize(
            (std::max)(1u, dxCommon->GetSwapChainBufferCount()));
    } catch (...) {
        Release();
        return false;
    }
    if (!CreateResources(width, height)) {
        Release();
        return false;
    }
    return true;
}

bool RenderSurface::Resize(int width, int height) {
    if (width <= 0 || height <= 0 || !dxCommon_ || !srvManager_) {
        return false;
    }
    if (width == state_->width && height == state_->height && IsReady()) {
        return true;
    }
    if (dxCommon_->IsCommandListRecording()) {
        return false;
    }
    return CreateResources(width, height);
}

void RenderSurface::ReleaseCompletedFrameResources() {
    if (dxCommon_ == nullptr || srvManager_ == nullptr ||
        !dxCommon_->IsCommandListRecording()) {
        return;
    }
    const UINT frameIndex = dxCommon_->GetBackBufferIndex();
    if (frameIndex >= state_->frameDeferredResources.size()) {
        return;
    }
    for (BuiltResources& retired : state_->frameDeferredResources[frameIndex]) {
        srvManager_->FreeIfAllocated(retired.sceneSrv);
        srvManager_->FreeIfAllocated(retired.depthSrv);
        srvManager_->FreeIfAllocated(retired.outputSrv);
    }
    state_->frameDeferredResources[frameIndex].clear();
}

bool RenderSurface::Release() {
    return Release(false);
}

bool RenderSurface::Release(bool allowFrameAbort) {
    const bool hasDeferredResources = std::ranges::any_of(
        state_->frameDeferredResources, [](const auto& resources) { return !resources.empty(); });
    const bool hasResources = state_->sceneColor || state_->depth || state_->output ||
                              state_->rtvHeap || state_->dsvHeap || hasDeferredResources;
    if (!CanReleaseGpuResources(dxCommon_, hasResources, allowFrameAbort)) {
        return false;
    }
    if (dxCommon_) {
        dxCommon_->UnregisterFrameRollbacks(this);
    }
    if (srvManager_) {
        srvManager_->FreeIfAllocated(state_->sceneSrv);
        srvManager_->FreeIfAllocated(state_->depthSrv);
        srvManager_->FreeIfAllocated(state_->outputSrv);
        for (auto& frame : state_->frameDeferredResources) {
            for (BuiltResources& retired : frame) {
                srvManager_->FreeIfAllocated(retired.sceneSrv);
                srvManager_->FreeIfAllocated(retired.depthSrv);
                srvManager_->FreeIfAllocated(retired.outputSrv);
            }
        }
    }
    *state_ = {};
    dxCommon_ = nullptr;
    srvManager_ = nullptr;
    return true;
}

bool RenderSurface::CreateResources(int width, int height) {
    ID3D12Device* device = dxCommon_ ? dxCommon_->GetDevice() : nullptr;
    if (device == nullptr || srvManager_ == nullptr || !srvManager_->CanAllocate(3)) {
        return false;
    }

    BuiltResources built{};
    built.sceneSrv = srvManager_->Allocate();
    built.depthSrv = srvManager_->Allocate();
    built.outputSrv = srvManager_->Allocate();
    auto freeBuiltDescriptors = [&]() {
        srvManager_->FreeIfAllocated(built.sceneSrv);
        srvManager_->FreeIfAllocated(built.depthSrv);
        srvManager_->FreeIfAllocated(built.outputSrv);
        built.sceneSrv = built.depthSrv = built.outputSrv = kInvalidSrv;
    };
    if (built.sceneSrv == kInvalidSrv || built.depthSrv == kInvalidSrv ||
        built.outputSrv == kInvalidSrv) {
        freeBuiltDescriptors();
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = kRtvCount;
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&built.rtvHeap))) ||
        FAILED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&built.dsvHeap))) ||
        !CreateColorResource(device, width, height, DirectXCommon::kSceneColorFormat,
                             {0.02f, 0.025f, 0.035f, 1.0f},
                             built.sceneColor.GetAddressOf()) ||
        !CreateDepthResource(device, width, height, built.depth.GetAddressOf()) ||
        !CreateColorResource(device, width, height, DirectXCommon::kBackBufferFormat,
                             {0.75f, 0.08f, 0.12f, 1.0f}, built.output.GetAddressOf())) {
        freeBuiltDescriptors();
        return false;
    }

    built.sceneColor->SetName(L"RenderSurface.SceneColor");
    built.depth->SetName(L"RenderSurface.Depth");
    built.output->SetName(L"RenderSurface.Output");
    const UINT rtvIncrement =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvStart =
        built.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC sceneRtv{};
    sceneRtv.Format = DirectXCommon::kSceneColorFormat;
    sceneRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(built.sceneColor.Get(), &sceneRtv,
                                   OffsetHandle(rtvStart, kSceneRtvIndex, rtvIncrement));
    D3D12_RENDER_TARGET_VIEW_DESC outputRtv{};
    outputRtv.Format = DirectXCommon::kBackBufferFormat;
    outputRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(built.output.Get(), &outputRtv,
                                   OffsetHandle(rtvStart, kOutputRtvIndex, rtvIncrement));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DirectXCommon::kDepthStencilFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(built.depth.Get(), &dsv,
                                   built.dsvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC sceneSrv{};
    sceneSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sceneSrv.Format = DirectXCommon::kSceneColorFormat;
    sceneSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sceneSrv.Texture2D.MipLevels = 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Texture2D.MipLevels = 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC outputSrv = sceneSrv;
    outputSrv.Format = DirectXCommon::kBackBufferFormat;
    device->CreateShaderResourceView(built.sceneColor.Get(), &sceneSrv,
                                     srvManager_->GetCpuHandle(built.sceneSrv));
    device->CreateShaderResourceView(built.depth.Get(), &depthSrv,
                                     srvManager_->GetCpuHandle(built.depthSrv));
    device->CreateShaderResourceView(built.output.Get(), &outputSrv,
                                     srvManager_->GetCpuHandle(built.outputSrv));

    const bool hasCurrent = state_->sceneColor || state_->depth || state_->output;
    const UINT frameIndex = dxCommon_->GetBackBufferIndex();
    if (hasCurrent) {
        if (frameIndex >= state_->frameDeferredResources.size()) {
            freeBuiltDescriptors();
            return false;
        }
        try {
            state_->frameDeferredResources[frameIndex].reserve(
                state_->frameDeferredResources[frameIndex].size() + 1);
        } catch (...) {
            freeBuiltDescriptors();
            return false;
        }
        BuiltResources retired{};
        retired.sceneColor = std::move(state_->sceneColor);
        retired.depth = std::move(state_->depth);
        retired.output = std::move(state_->output);
        retired.rtvHeap = std::move(state_->rtvHeap);
        retired.dsvHeap = std::move(state_->dsvHeap);
        retired.sceneSrv = state_->sceneSrv;
        retired.depthSrv = state_->depthSrv;
        retired.outputSrv = state_->outputSrv;
        state_->frameDeferredResources[frameIndex].push_back(std::move(retired));
    }

    state_->sceneColor = std::move(built.sceneColor);
    state_->depth = std::move(built.depth);
    state_->output = std::move(built.output);
    state_->rtvHeap = std::move(built.rtvHeap);
    state_->dsvHeap = std::move(built.dsvHeap);
    state_->sceneSrv = built.sceneSrv;
    state_->depthSrv = built.depthSrv;
    state_->outputSrv = built.outputSrv;
    state_->rtvIncrement = rtvIncrement;
    state_->width = width;
    state_->height = height;
    state_->sceneState = kShaderReadState;
    state_->depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    state_->outputState = kShaderReadState;
    state_->viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f,
                        1.0f};
    state_->scissor = {0, 0, width, height};
    return IsReady();
}

bool RenderSurface::Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& current,
                               D3D12_RESOURCE_STATES next) {
    ID3D12GraphicsCommandList* commandList = dxCommon_ ? dxCommon_->GetCommandList() : nullptr;
    if (commandList == nullptr || resource == nullptr) {
        return false;
    }
    if (current == next) {
        return true;
    }
    D3D12_RESOURCE_STATES* stateSlot = &current;
    const D3D12_RESOURCE_STATES previous = current;
    if (!dxCommon_->RegisterFrameRollback(this,
                                          [stateSlot, previous]() { *stateSlot = previous; })) {
        return false;
    }
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, current, next);
    commandList->ResourceBarrier(1, &barrier);
    current = next;
    return true;
}

void RenderSurface::ApplyViewportAndScissor() const {
    ID3D12GraphicsCommandList* commandList = dxCommon_ ? dxCommon_->GetCommandList() : nullptr;
    if (commandList == nullptr) {
        return;
    }
    commandList->RSSetViewports(1, &state_->viewport);
    commandList->RSSetScissorRects(1, &state_->scissor);
}

void RenderSurface::BeginScenePass(const DirectX::XMFLOAT4& clearColor) {
    ID3D12GraphicsCommandList* commandList = dxCommon_ ? dxCommon_->GetCommandList() : nullptr;
    if (commandList == nullptr || !IsReady() ||
        !Transition(state_->sceneColor.Get(), state_->sceneState,
                    D3D12_RESOURCE_STATE_RENDER_TARGET) ||
        !Transition(state_->depth.Get(), state_->depthState, D3D12_RESOURCE_STATE_DEPTH_WRITE)) {
        return;
    }
    ApplyViewportAndScissor();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = OffsetHandle(
        state_->rtvHeap->GetCPUDescriptorHandleForHeapStart(), kSceneRtvIndex,
        state_->rtvIncrement);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        state_->dsvHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    const float color[4] = {
        Numeric::ClampFinite(clearColor.x, 0.0f, 1.0f, 0.0f),
        Numeric::ClampFinite(clearColor.y, 0.0f, 1.0f, 0.0f),
        Numeric::ClampFinite(clearColor.z, 0.0f, 1.0f, 0.0f),
        Numeric::ClampFinite(clearColor.w, 0.0f, 1.0f, 1.0f),
    };
    commandList->ClearRenderTargetView(rtv, color, 0, nullptr);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void RenderSurface::EndScenePass() {
    Transition(state_->sceneColor.Get(), state_->sceneState, kShaderReadState);
}

void RenderSurface::BeginOutputPass(const DirectX::XMFLOAT4& clearColor) {
    ID3D12GraphicsCommandList* commandList = dxCommon_ ? dxCommon_->GetCommandList() : nullptr;
    if (commandList == nullptr || !IsReady() ||
        !Transition(state_->output.Get(), state_->outputState,
                    D3D12_RESOURCE_STATE_RENDER_TARGET)) {
        return;
    }
    ApplyViewportAndScissor();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetOutputRtvHandle();
    commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float color[4] = {
        Numeric::ClampFinite(clearColor.x, 0.0f, 1.0f, 0.0f),
        Numeric::ClampFinite(clearColor.y, 0.0f, 1.0f, 0.0f),
        Numeric::ClampFinite(clearColor.z, 0.0f, 1.0f, 0.0f),
        Numeric::ClampFinite(clearColor.w, 0.0f, 1.0f, 1.0f),
    };
    commandList->ClearRenderTargetView(rtv, color, 0, nullptr);
}

void RenderSurface::EndOutputPass() {
    Transition(state_->output.Get(), state_->outputState, kShaderReadState);
}

void RenderSurface::TransitionDepthToShaderResource() {
    Transition(state_->depth.Get(), state_->depthState, kShaderReadState);
}

void RenderSurface::TransitionDepthToWrite() {
    Transition(state_->depth.Get(), state_->depthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderSurface::GetSceneColorGpuHandle() const {
    return IsReady() ? srvManager_->GetGpuHandle(state_->sceneSrv)
                     : D3D12_GPU_DESCRIPTOR_HANDLE{};
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderSurface::GetDepthGpuHandle() const {
    return IsReady() ? srvManager_->GetGpuHandle(state_->depthSrv)
                     : D3D12_GPU_DESCRIPTOR_HANDLE{};
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderSurface::GetOutputGpuHandle() const {
    return IsReady() ? srvManager_->GetGpuHandle(state_->outputSrv)
                     : D3D12_GPU_DESCRIPTOR_HANDLE{};
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderSurface::GetOutputRtvHandle() const {
    if (!state_->rtvHeap) {
        return {};
    }
    return OffsetHandle(state_->rtvHeap->GetCPUDescriptorHandleForHeapStart(), kOutputRtvIndex,
                        state_->rtvIncrement);
}

int RenderSurface::GetWidth() const {
    return state_->width;
}

int RenderSurface::GetHeight() const {
    return state_->height;
}

bool RenderSurface::IsReady() const {
    return dxCommon_ != nullptr && srvManager_ != nullptr && state_->sceneColor && state_->depth &&
           state_->output && state_->rtvHeap && state_->dsvHeap && state_->sceneSrv != kInvalidSrv &&
           state_->depthSrv != kInvalidSrv && state_->outputSrv != kInvalidSrv &&
           state_->width > 0 && state_->height > 0;
}
