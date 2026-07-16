#pragma once

namespace ShaderPaths {

inline constexpr const wchar_t* SpriteVS = L"engine://shaders/sprite/SpriteVS.hlsl";
inline constexpr const wchar_t* SpritePS = L"engine://shaders/sprite/SpritePS.hlsl";

inline constexpr const wchar_t* PostProcessVS =
    L"engine://shaders/postprocess/PostProcessVS.hlsl";
inline constexpr const wchar_t* PostProcessPS =
    L"engine://shaders/postprocess/PostProcessPS.hlsl";
inline constexpr const wchar_t* PostProcessCopyPS =
    L"engine://shaders/postprocess/CopyPS.hlsl";
inline constexpr const wchar_t* BloomExtractPS =
    L"engine://shaders/postprocess/BloomExtractPS.hlsl";
inline constexpr const wchar_t* BloomDownsamplePS =
    L"engine://shaders/postprocess/BloomDownsamplePS.hlsl";
inline constexpr const wchar_t* BloomUpsamplePS =
    L"engine://shaders/postprocess/BloomUpsamplePS.hlsl";
inline constexpr const wchar_t* VolumetricLightingPS =
    L"engine://shaders/postprocess/VolumetricLightingPS.hlsl";
inline constexpr const wchar_t* VolumetricLightingCompositePS =
    L"engine://shaders/postprocess/VolumetricLightingCompositePS.hlsl";

inline constexpr const wchar_t* DepthPyramidCS =
    L"engine://shaders/depth/DepthPyramidCS.hlsl";

inline constexpr const wchar_t* SkyboxVS = L"engine://shaders/skybox/SkyboxVS.hlsl";
inline constexpr const wchar_t* SkyboxPS = L"engine://shaders/skybox/SkyboxPS.hlsl";

inline constexpr const wchar_t* MeshVS = L"engine://shaders/mesh/MeshVS.hlsl";
inline constexpr const wchar_t* MeshInstancedVS =
    L"engine://shaders/mesh/MeshInstancedVS.hlsl";
inline constexpr const wchar_t* MeshPS = L"engine://shaders/mesh/MeshPS.hlsl";
inline constexpr const wchar_t* MeshShadowVS = L"engine://shaders/mesh/MeshShadowVS.hlsl";
inline constexpr const wchar_t* MeshShadowInstancedVS =
    L"engine://shaders/mesh/MeshShadowInstancedVS.hlsl";
inline constexpr const wchar_t* MeshShadowPS = L"engine://shaders/mesh/MeshShadowPS.hlsl";
inline constexpr const wchar_t* MeshGpuCullCS = L"engine://shaders/mesh/MeshGpuCullCS.hlsl";
inline constexpr const wchar_t* MeshGpuCullArgsCS =
    L"engine://shaders/mesh/MeshGpuCullArgsCS.hlsl";
inline constexpr const wchar_t* MeshGpuLodCullCS =
    L"engine://shaders/mesh/MeshGpuLodCullCS.hlsl";
inline constexpr const wchar_t* MeshGpuLodCullArgsCS =
    L"engine://shaders/mesh/MeshGpuLodCullArgsCS.hlsl";

inline constexpr const wchar_t* ModelVS = L"engine://shaders/model/ModelVS.hlsl";
inline constexpr const wchar_t* ModelInstancedVS =
    L"engine://shaders/model/ModelInstancedVS.hlsl";
inline constexpr const wchar_t* ModelPS = L"engine://shaders/model/ModelPS.hlsl";
inline constexpr const wchar_t* ModelShadowVS =
    L"engine://shaders/model/ModelShadowVS.hlsl";
inline constexpr const wchar_t* ModelShadowInstancedVS =
    L"engine://shaders/model/ModelShadowInstancedVS.hlsl";
inline constexpr const wchar_t* ModelShadowPS =
    L"engine://shaders/model/ModelShadowPS.hlsl";
inline constexpr const wchar_t* SkinningCS = L"engine://shaders/model/SkinningCS.hlsl";

inline constexpr const wchar_t* ParticleUpdateCS =
    L"engine://shaders/particle/GPUParticleUpdateCS.hlsl";
inline constexpr const wchar_t* ParticleArgsCS =
    L"engine://shaders/particle/GPUParticleArgsCS.hlsl";
inline constexpr const wchar_t* ParticleVS =
    L"engine://shaders/particle/GPUParticleVS.hlsl";
inline constexpr const wchar_t* ParticlePS =
    L"engine://shaders/particle/GPUParticlePS.hlsl";

} // namespace ShaderPaths
