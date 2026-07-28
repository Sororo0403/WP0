#include "EditorScene.h"

#include "graphics/LightingScene.h"

#include <cmath>

namespace {

void ApplyDirectionalLight(SceneLighting& lighting, const LightComponent& component,
                           const DirectX::XMFLOAT3& direction, bool& assigned) {
    if (assigned) {
        return;
    }
    lighting.keyLightDirection = direction;
    lighting.keyLightColor = {component.color.x * component.intensity,
                              component.color.y * component.intensity,
                              component.color.z * component.intensity, 1.0f};
    assigned = true;
}

void ApplyPointLight(SceneLighting& lighting, const LightComponent& component,
                     const DirectX::XMFLOAT4X4& worldMatrix, size_t& index) {
    if (index >= lighting.pointLights.size()) {
        return;
    }
    PointLight& point = lighting.pointLights[index++];
    point.positionRange = {worldMatrix._41, worldMatrix._42, worldMatrix._43, component.range};
    point.colorIntensity = {component.color.x, component.color.y, component.color.z,
                            component.intensity};
}

void ApplySpotLight(SceneLighting& lighting, const LightComponent& component,
                    const DirectX::XMFLOAT4X4& worldMatrix,
                    const DirectX::XMFLOAT3& direction, bool& assigned) {
    if (assigned) {
        return;
    }
    SpotLight& spot = lighting.spotLight;
    spot.positionRange = {worldMatrix._41, worldMatrix._42, worldMatrix._43, component.range};
    spot.direction = {direction.x, direction.y, direction.z, 0.0f};
    spot.colorIntensity = {component.color.x, component.color.y, component.color.z,
                           component.intensity};
    spot.angleParams = {std::cos(DirectX::XMConvertToRadians(component.innerAngleDegrees)),
                        std::cos(DirectX::XMConvertToRadians(component.outerAngleDegrees)), 2.4f,
                        1.0f};
    assigned = true;
}

}  // namespace

void EditorScene::SubmitLighting(LightingScene& lightingScene) {
    SceneLighting lighting{};
    bool directionalAssigned = false;
    size_t pointLightIndex = 0u;
    bool spotAssigned = false;
    for (const WorldEntity& entity : world_.Entities()) {
        DirectX::XMFLOAT4X4 worldMatrix{};
        DirectX::XMFLOAT3 direction{};
        if (!TryResolveSceneLight(entity, worldMatrix, direction)) {
            continue;
        }
        const LightComponent& component = *entity.light;
        switch (component.type) {
            case LightType::Directional:
                ApplyDirectionalLight(lighting, component, direction, directionalAssigned);
                break;
            case LightType::Point:
                ApplyPointLight(lighting, component, worldMatrix, pointLightIndex);
                break;
            case LightType::Spot:
                ApplySpotLight(lighting, component, worldMatrix, direction, spotAssigned);
                break;
        }
    }
    lightingScene.SetSceneLighting(lighting);
}

bool EditorScene::TryResolveSceneLight(const WorldEntity& entity,
                                       DirectX::XMFLOAT4X4& worldMatrix,
                                       DirectX::XMFLOAT3& direction) const {
    if (!world_.IsActiveInHierarchy(entity.id) || !entity.light || !entity.light->enabled ||
        entity.light->intensity <= 0.0f || !world_.TryGetWorldMatrix(entity.id, worldMatrix)) {
        return false;
    }
    const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&worldMatrix);
    DirectX::XMVECTOR worldDirection = DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), world);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(worldDirection)) <= 1.0e-8f) {
        return false;
    }
    worldDirection = DirectX::XMVector3Normalize(worldDirection);
    DirectX::XMStoreFloat3(&direction, worldDirection);
    return true;
}
