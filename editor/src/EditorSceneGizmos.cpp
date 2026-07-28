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

#include "internal/EditorSceneViewportUtils.h"

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

using namespace EditorSceneViewportUtils;

void EditorScene::DrawSceneComponentGizmos(const ImVec2& imageMin, const ImVec2& imageMax) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(imageMin, imageMax, true);
    auto drawWorldLine = [&](const DirectX::XMFLOAT3& from, const DirectX::XMFLOAT3& to,
                             ImU32 color, float thickness = 1.25f) {
        ImVec2 screenFrom{};
        ImVec2 screenTo{};
        if (ProjectScenePoint(sceneViewCamera_, from, imageMin, imageMax, screenFrom, false) &&
            ProjectScenePoint(sceneViewCamera_, to, imageMin, imageMax, screenTo, false)) {
            drawList->AddLine(screenFrom, screenTo, color, thickness);
        }
    };
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.camera && !entity.light && !entity.audioSource && !entity.audioListener &&
            !entity.boxCollider && !entity.characterController) {
            continue;
        }
        DirectX::XMFLOAT4X4 worldMatrix{};
        ImVec2 center{};
        if (!world_.TryGetWorldMatrix(entity.id, worldMatrix) ||
            !ProjectScenePoint(sceneViewCamera_,
                               {worldMatrix._41, worldMatrix._42, worldMatrix._43}, imageMin,
                               imageMax, center)) {
            continue;
        }
        const bool active = entity.id == selection_;
        const bool selected = hierarchySelection_.contains(entity.id);
        ImU32 color =
            entity.camera
                ? IM_COL32(90, 185, 255, 230)
                : (entity.light ? IM_COL32(255, 215, 80, 230)
                                : (entity.audioSource
                                       ? IM_COL32(190, 120, 255, 230)
                                       : (entity.audioListener ? IM_COL32(80, 215, 230, 230)
                                                               : IM_COL32(80, 230, 130, 230))));
        const bool physicsLayerVisible =
            (physicsDebugLayerMask_ & (uint32_t{1} << entity.layer)) != 0u;
        const bool entityActive = world_.IsActiveInHierarchy(entity.id);
        const bool drawPhysicsShapes = showPhysicsDebug_ && entityActive && physicsLayerVisible &&
                                       (entity.boxCollider || entity.characterController);
        if (active) {
            color = IM_COL32(255, 184, 56, 255);
        } else if (selected) {
            color = IM_COL32(90, 190, 255, 255);
        }
        const bool enabled =
            entityActive &&
            ((entity.camera && entity.camera->enabled) || (entity.light && entity.light->enabled) ||
             (entity.audioSource && entity.audioSource->enabled) ||
             (entity.audioListener && entity.audioListener->enabled) ||
             (entity.boxCollider && entity.boxCollider->enabled) ||
             (entity.characterController && entity.characterController->enabled));
        if (!enabled) {
            color = (color & 0x00FFFFFFu) | (100u << 24u);
        }

        if (entity.camera) {
            drawList->AddRect({center.x - 8.0f, center.y - 6.0f},
                              {center.x + 5.0f, center.y + 6.0f}, color, 2.0f, 0, 1.8f);
            drawList->AddTriangle({center.x + 5.0f, center.y - 5.0f},
                                  {center.x + 12.0f, center.y - 9.0f},
                                  {center.x + 12.0f, center.y + 1.0f}, color, 1.8f);
        } else if (entity.light) {
            drawList->AddCircle(center, 5.0f, color, 16, 1.8f);
            for (int index = 0; index < 8; ++index) {
                const float angle = DirectX::XM_2PI * static_cast<float>(index) / 8.0f;
                const ImVec2 direction{std::cos(angle), std::sin(angle)};
                drawList->AddLine({center.x + direction.x * 7.0f, center.y + direction.y * 7.0f},
                                  {center.x + direction.x * 11.0f, center.y + direction.y * 11.0f},
                                  color, 1.5f);
            }
        } else if (entity.audioSource) {
            drawList->AddRect({center.x - 9.0f, center.y - 4.0f},
                              {center.x - 5.0f, center.y + 4.0f}, color, 1.0f, 0, 1.8f);
            drawList->AddTriangle({center.x - 5.0f, center.y - 4.0f},
                                  {center.x + 1.0f, center.y - 8.0f},
                                  {center.x + 1.0f, center.y + 8.0f}, color, 1.8f);
            constexpr int arcSegments = 6;
            for (int arc = 0; arc < 2; ++arc) {
                const float radius = 5.0f + static_cast<float>(arc) * 4.0f;
                ImVec2 previous{};
                for (int index = 0; index <= arcSegments; ++index) {
                    const float angle = -DirectX::XM_PIDIV4 + DirectX::XM_PIDIV2 *
                                                                  static_cast<float>(index) /
                                                                  static_cast<float>(arcSegments);
                    const ImVec2 point{center.x + 1.0f + std::cos(angle) * radius,
                                       center.y + std::sin(angle) * radius};
                    if (index > 0) {
                        drawList->AddLine(previous, point, color, 1.5f);
                    }
                    previous = point;
                }
            }
        } else if (entity.audioListener) {
            drawList->AddCircle(center, 3.0f, color, 16, 1.8f);
            drawList->AddCircle(center, 7.0f, color, 24, 1.5f);
            drawList->AddCircle(center, 11.0f, color, 32, 1.25f);
        } else {
            drawList->AddRect({center.x - 6.0f, center.y - 6.0f},
                              {center.x + 6.0f, center.y + 6.0f}, color, 1.0f, 0, 1.8f);
        }
        if (active || drawPhysicsShapes) {
            using namespace DirectX;
            const XMMATRIX world = XMLoadFloat4x4(&worldMatrix);
            const XMVECTOR origin =
                XMVectorSet(worldMatrix._41, worldMatrix._42, worldMatrix._43, 1.0f);
            auto normalizedAxis = [&](float x, float y, float z) {
                XMVECTOR axis = XMVector3TransformNormal(XMVectorSet(x, y, z, 0.0f), world);
                return XMVectorGetX(XMVector3LengthSq(axis)) > 1.0e-8f ? XMVector3Normalize(axis)
                                                                       : XMVectorSet(x, y, z, 0.0f);
            };
            const XMVECTOR right = normalizedAxis(1.0f, 0.0f, 0.0f);
            const XMVECTOR up = normalizedAxis(0.0f, 1.0f, 0.0f);
            const XMVECTOR forward = normalizedAxis(0.0f, 0.0f, 1.0f);
            auto worldPoint = [&](float x, float y, float z) {
                XMFLOAT3 result{};
                XMStoreFloat3(&result, origin + right * x + up * y + forward * z);
                return result;
            };

            if (active && entity.camera) {
                const CameraComponent& camera = *entity.camera;
                const float aspect =
                    static_cast<float>((std::max)(1, gameViewSurface_.GetWidth())) /
                    static_cast<float>((std::max)(1, gameViewSurface_.GetHeight()));
                const float nearDepth = camera.nearClip;
                const float farDepth =
                    (std::min)(camera.farClip, (std::max)(20.0f, nearDepth + 0.001f));
                float nearHalfHeight = camera.orthographicHeight * 0.5f;
                float farHalfHeight = nearHalfHeight;
                if (camera.projection == CameraProjection::Perspective) {
                    const float tangent =
                        std::tan(XMConvertToRadians(camera.fieldOfViewDegrees) * 0.5f);
                    nearHalfHeight = tangent * nearDepth;
                    farHalfHeight = tangent * farDepth;
                }
                const float nearHalfWidth = nearHalfHeight * aspect;
                const float farHalfWidth = farHalfHeight * aspect;
                const std::array<XMFLOAT3, 8> corners = {
                    worldPoint(-nearHalfWidth, -nearHalfHeight, nearDepth),
                    worldPoint(nearHalfWidth, -nearHalfHeight, nearDepth),
                    worldPoint(nearHalfWidth, nearHalfHeight, nearDepth),
                    worldPoint(-nearHalfWidth, nearHalfHeight, nearDepth),
                    worldPoint(-farHalfWidth, -farHalfHeight, farDepth),
                    worldPoint(farHalfWidth, -farHalfHeight, farDepth),
                    worldPoint(farHalfWidth, farHalfHeight, farDepth),
                    worldPoint(-farHalfWidth, farHalfHeight, farDepth),
                };
                constexpr size_t edges[][2] = {
                    {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                    {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
                };
                const ImU32 guideColor =
                    camera.enabled ? IM_COL32(90, 185, 255, 190) : IM_COL32(90, 185, 255, 80);
                for (const auto& edge : edges) {
                    drawWorldLine(corners[edge[0]], corners[edge[1]], guideColor);
                }
            }

            if (active && entity.light) {
                const LightComponent& light = *entity.light;
                const ImU32 guideColor =
                    light.enabled ? IM_COL32(255, 215, 80, 190) : IM_COL32(255, 215, 80, 80);
                const XMFLOAT3 worldOrigin = worldPoint(0.0f, 0.0f, 0.0f);
                auto drawCircle = [&](float radius, XMVECTOR axisA, XMVECTOR axisB,
                                      XMVECTOR circleCenter = DirectX::g_XMZero) {
                    constexpr int segments = 32;
                    XMFLOAT3 previous{};
                    for (int index = 0; index <= segments; ++index) {
                        const float angle =
                            XM_2PI * static_cast<float>(index) / static_cast<float>(segments);
                        XMFLOAT3 point{};
                        XMStoreFloat3(&point, origin + circleCenter +
                                                  axisA * (std::cos(angle) * radius) +
                                                  axisB * (std::sin(angle) * radius));
                        if (index > 0) {
                            drawWorldLine(previous, point, guideColor);
                        }
                        previous = point;
                    }
                };
                if (light.type == LightType::Directional) {
                    const XMFLOAT3 tip = worldPoint(0.0f, 0.0f, 3.0f);
                    drawWorldLine(worldOrigin, tip, guideColor, 1.75f);
                    drawWorldLine(tip, worldPoint(-0.3f, 0.0f, 2.5f), guideColor, 1.75f);
                    drawWorldLine(tip, worldPoint(0.3f, 0.0f, 2.5f), guideColor, 1.75f);
                    drawWorldLine(tip, worldPoint(0.0f, -0.3f, 2.5f), guideColor, 1.75f);
                    drawWorldLine(tip, worldPoint(0.0f, 0.3f, 2.5f), guideColor, 1.75f);
                } else if (light.type == LightType::Point) {
                    drawCircle(light.range, right, up);
                    drawCircle(light.range, right, forward);
                    drawCircle(light.range, up, forward);
                } else {
                    const float guideAngle = (std::min)(light.outerAngleDegrees, 89.0f);
                    const float coneRadius = light.range * std::tan(XMConvertToRadians(guideAngle));
                    const XMVECTOR coneCenter = forward * light.range;
                    drawCircle(coneRadius, right, up, coneCenter);
                    for (int index = 0; index < 4; ++index) {
                        const float angle = XM_PIDIV2 * static_cast<float>(index);
                        XMFLOAT3 rim{};
                        XMStoreFloat3(&rim, origin + coneCenter +
                                                right * (std::cos(angle) * coneRadius) +
                                                up * (std::sin(angle) * coneRadius));
                        drawWorldLine(worldOrigin, rim, guideColor);
                    }
                }
            }

            if (active && entity.audioSource && entity.audioSource->spatial) {
                const AudioSourceComponent& source = *entity.audioSource;
                const bool sourceEnabled = entityActive && source.enabled;
                const ImU32 minColor =
                    sourceEnabled ? IM_COL32(220, 155, 255, 220) : IM_COL32(220, 155, 255, 80);
                const ImU32 maxColor =
                    sourceEnabled ? IM_COL32(155, 95, 255, 150) : IM_COL32(155, 95, 255, 60);
                auto drawRangeCircle = [&](float radius, XMVECTOR axisA, XMVECTOR axisB,
                                           ImU32 guideColor, float thickness) {
                    constexpr int segments = 48;
                    XMFLOAT3 previous{};
                    for (int index = 0; index <= segments; ++index) {
                        const float angle =
                            XM_2PI * static_cast<float>(index) / static_cast<float>(segments);
                        XMFLOAT3 point{};
                        XMStoreFloat3(&point, origin + axisA * (std::cos(angle) * radius) +
                                                  axisB * (std::sin(angle) * radius));
                        if (index > 0) {
                            drawWorldLine(previous, point, guideColor, thickness);
                        }
                        previous = point;
                    }
                };
                drawRangeCircle(source.minDistance, right, up, minColor, 1.75f);
                drawRangeCircle(source.minDistance, right, forward, minColor, 1.75f);
                drawRangeCircle(source.minDistance, up, forward, minColor, 1.75f);
                drawRangeCircle(source.maxDistance, right, up, maxColor, 1.25f);
                drawRangeCircle(source.maxDistance, right, forward, maxColor, 1.25f);
                drawRangeCircle(source.maxDistance, up, forward, maxColor, 1.25f);
            }

            if (entity.boxCollider && (active || drawPhysicsShapes)) {
                OBB collider{};
                if (TryBuildWorldBoxCollider(world_, entity.id, collider)) {
                    const XMVECTOR colliderCenter = XMLoadFloat3(&collider.center);
                    const XMVECTOR colliderRotation = XMLoadFloat4(&collider.rotation);
                    const XMVECTOR colliderRight =
                        XMVector3Rotate(g_XMIdentityR0, colliderRotation);
                    const XMVECTOR colliderUp = XMVector3Rotate(g_XMIdentityR1, colliderRotation);
                    const XMVECTOR colliderForward =
                        XMVector3Rotate(g_XMIdentityR2, colliderRotation);
                    const float halfX = collider.size.x * 0.5f;
                    const float halfY = collider.size.y * 0.5f;
                    const float halfZ = collider.size.z * 0.5f;
                    std::array<XMFLOAT3, 8> corners{};
                    size_t cornerIndex = 0;
                    for (int z = -1; z <= 1; z += 2) {
                        for (int y = -1; y <= 1; y += 2) {
                            for (int x = -1; x <= 1; x += 2) {
                                XMStoreFloat3(&corners[cornerIndex++],
                                              colliderCenter + colliderRight * (halfX * x) +
                                                  colliderUp * (halfY * y) +
                                                  colliderForward * (halfZ * z));
                            }
                        }
                    }
                    constexpr size_t colliderEdges[][2] = {
                        {0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                        {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
                    };
                    const ImU32 guideColor =
                        entity.boxCollider->isTrigger
                            ? (entity.boxCollider->enabled ? IM_COL32(255, 170, 70, 220)
                                                           : IM_COL32(255, 170, 70, 80))
                        : (!active && showPhysicsDebug_)
                            ? PhysicsDebugLayerColor(entity.layer, entity.boxCollider->enabled)
                            : (entity.boxCollider->enabled ? IM_COL32(80, 230, 130, 210)
                                                           : IM_COL32(80, 230, 130, 80));
                    for (const auto& edge : colliderEdges) {
                        drawWorldLine(corners[edge[0]], corners[edge[1]], guideColor, 1.5f);
                    }
                }
            }

            if (entity.characterController && (active || drawPhysicsShapes)) {
                CharacterCapsule capsule{};
                if (TryBuildWorldCharacterCapsule(world_, entity.id, capsule)) {
                    const ImU32 guideColor =
                        !active && showPhysicsDebug_
                            ? PhysicsDebugLayerColor(entity.layer,
                                                     entity.characterController->enabled)
                            : (entity.characterController->enabled ? IM_COL32(70, 220, 210, 220)
                                                                   : IM_COL32(70, 220, 210, 80));
                    const float segmentHalfHeight =
                        (std::max)(0.0f, capsule.height * 0.5f - capsule.radius);
                    auto capsulePoint = [&](float x, float y, float z) {
                        return XMFLOAT3{capsule.center.x + x, capsule.center.y + y,
                                        capsule.center.z + z};
                    };
                    constexpr int segments = 32;
                    for (float y : {-segmentHalfHeight, segmentHalfHeight}) {
                        XMFLOAT3 previous = capsulePoint(capsule.radius, y, 0.0f);
                        for (int index = 1; index <= segments; ++index) {
                            const float angle =
                                XM_2PI * static_cast<float>(index) / static_cast<float>(segments);
                            const XMFLOAT3 point = capsulePoint(std::cos(angle) * capsule.radius, y,
                                                                std::sin(angle) * capsule.radius);
                            drawWorldLine(previous, point, guideColor, 1.5f);
                            previous = point;
                        }
                    }
                    drawWorldLine(capsulePoint(capsule.radius, -segmentHalfHeight, 0.0f),
                                  capsulePoint(capsule.radius, segmentHalfHeight, 0.0f), guideColor,
                                  1.5f);
                    drawWorldLine(capsulePoint(-capsule.radius, -segmentHalfHeight, 0.0f),
                                  capsulePoint(-capsule.radius, segmentHalfHeight, 0.0f),
                                  guideColor, 1.5f);
                    drawWorldLine(capsulePoint(0.0f, -segmentHalfHeight, capsule.radius),
                                  capsulePoint(0.0f, segmentHalfHeight, capsule.radius), guideColor,
                                  1.5f);
                    drawWorldLine(capsulePoint(0.0f, -segmentHalfHeight, -capsule.radius),
                                  capsulePoint(0.0f, segmentHalfHeight, -capsule.radius),
                                  guideColor, 1.5f);
                    auto drawCapArc = [&](bool xPlane, bool top) {
                        const float baseY = top ? segmentHalfHeight : -segmentHalfHeight;
                        const float angleStart = top ? 0.0f : XM_PI;
                        XMFLOAT3 previous = xPlane ? capsulePoint(capsule.radius, baseY, 0.0f)
                                                   : capsulePoint(0.0f, baseY, capsule.radius);
                        if (!top) {
                            previous = xPlane ? capsulePoint(-capsule.radius, baseY, 0.0f)
                                              : capsulePoint(0.0f, baseY, -capsule.radius);
                        }
                        for (int index = 1; index <= segments; ++index) {
                            const float angle = angleStart + XM_PI * static_cast<float>(index) /
                                                                 static_cast<float>(segments);
                            const float horizontal = std::cos(angle) * capsule.radius;
                            const float vertical = std::sin(angle) * capsule.radius;
                            const XMFLOAT3 point =
                                xPlane ? capsulePoint(horizontal, baseY + vertical, 0.0f)
                                       : capsulePoint(0.0f, baseY + vertical, horizontal);
                            drawWorldLine(previous, point, guideColor, 1.5f);
                            previous = point;
                        }
                    };
                    drawCapArc(true, true);
                    drawCapArc(true, false);
                    drawCapArc(false, true);
                    drawCapArc(false, false);
                }
            }
        }
        if (drawPhysicsShapes) {
            std::string layerLabel = physicsSettings_.layerNames[entity.layer];
            if (layerLabel.empty()) {
                layerLabel = "Layer " + std::to_string(entity.layer);
            }
            const ImVec2 labelPosition{center.x + 10.0f, center.y + 8.0f};
            drawList->AddText({labelPosition.x + 1.0f, labelPosition.y + 1.0f},
                              IM_COL32(0, 0, 0, 220), layerLabel.c_str());
            drawList->AddText(labelPosition, PhysicsDebugLayerColor(entity.layer),
                              layerLabel.c_str());
        }
        if (active && (!entity.meshRenderer || !entity.meshRenderer->enabled)) {
            drawList->AddText({center.x + 14.0f, center.y - ImGui::GetTextLineHeight() * 0.5f},
                              color, entity.name.c_str());
        }
    }
    drawList->PopClipRect();
}

void EditorScene::DrawSceneSelectionOutline(const ImVec2& imageMin, const ImVec2& imageMax) const {
    if (!selection_.IsValid() || ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        return;
    }
    using namespace DirectX;
    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    constexpr size_t edges[][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
        {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(imageMin, imageMax, true);
    auto drawEntityOutline = [&](const WorldEntity& entity, bool active) {
        if (!world_.IsActiveInHierarchy(entity.id) || !entity.meshRenderer ||
            !entity.meshRenderer->enabled) {
            return;
        }
        const ModelHandle handle = ResolveModel(*entity.meshRenderer);
        const Model* model = handle.IsValid() ? ctx_->rendering.model->GetModel(handle) : nullptr;
        XMFLOAT3 boundsMin{};
        XMFLOAT3 boundsMax{};
        XMFLOAT4X4 worldMatrix{};
        if (model == nullptr || !TryGetModelBounds(*model, boundsMin, boundsMax) ||
            !world_.TryGetWorldMatrix(entity.id, worldMatrix)) {
            return;
        }
        const XMMATRIX worldViewProjection =
            XMLoadFloat4x4(&worldMatrix) * sceneViewCamera_.GetViewProjection();
        const XMFLOAT3 corners[8] = {
            {boundsMin.x, boundsMin.y, boundsMin.z}, {boundsMax.x, boundsMin.y, boundsMin.z},
            {boundsMax.x, boundsMax.y, boundsMin.z}, {boundsMin.x, boundsMax.y, boundsMin.z},
            {boundsMin.x, boundsMin.y, boundsMax.z}, {boundsMax.x, boundsMin.y, boundsMax.z},
            {boundsMax.x, boundsMax.y, boundsMax.z}, {boundsMin.x, boundsMax.y, boundsMax.z},
        };
        ImVec2 projected[8]{};
        for (size_t index = 0; index < std::size(corners); ++index) {
            const XMVECTOR clip = XMVector4Transform(
                XMVectorSet(corners[index].x, corners[index].y, corners[index].z, 1.0f),
                worldViewProjection);
            const float clipW = XMVectorGetW(clip);
            if (!std::isfinite(clipW) || clipW <= 1.0e-5f) {
                return;
            }
            const float ndcX = XMVectorGetX(clip) / clipW;
            const float ndcY = XMVectorGetY(clip) / clipW;
            if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) {
                return;
            }
            projected[index] = {imageMin.x + (ndcX * 0.5f + 0.5f) * width,
                                imageMin.y + (0.5f - ndcY * 0.5f) * height};
        }
        const ImU32 outlineColor =
            active ? IM_COL32(255, 184, 56, 255) : IM_COL32(90, 190, 255, 220);
        for (const auto& edge : edges) {
            drawList->AddLine(projected[edge[0]], projected[edge[1]], outlineColor,
                              active ? 2.0f : 1.5f);
        }
        if (!active) {
            return;
        }
        ImVec2 labelPosition = projected[0];
        for (const ImVec2& point : projected) {
            labelPosition.x = (std::min)(labelPosition.x, point.x);
            labelPosition.y = (std::min)(labelPosition.y, point.y);
        }
        const ImVec2 textSize = ImGui::CalcTextSize(entity.name.c_str());
        const float labelMinX = imageMin.x + 3.0f;
        const float labelMinY = imageMin.y + 3.0f;
        const float labelMaxX = (std::max)(labelMinX, imageMax.x - textSize.x - 9.0f);
        const float labelMaxY = (std::max)(labelMinY, imageMax.y - textSize.y - 7.0f);
        labelPosition.x = std::clamp(labelPosition.x, labelMinX, labelMaxX);
        labelPosition.y = std::clamp(labelPosition.y - textSize.y - 8.0f, labelMinY, labelMaxY);
        drawList->AddRectFilled(
            labelPosition,
            {labelPosition.x + textSize.x + 6.0f, labelPosition.y + textSize.y + 4.0f},
            IM_COL32(20, 20, 24, 210), 3.0f);
        drawList->AddText({labelPosition.x + 3.0f, labelPosition.y + 2.0f}, outlineColor,
                          entity.name.c_str());
    };
    for (const WorldEntity& entity : world_.Entities()) {
        const bool selected = hierarchySelection_.contains(entity.id) || entity.id == selection_;
        if (selected && entity.id != selection_) {
            drawEntityOutline(entity, false);
        }
    }
    if (const WorldEntity* active = world_.Find(selection_)) {
        drawEntityOutline(*active, true);
    }
    drawList->PopClipRect();
}

void EditorScene::DrawSceneGizmoToolbar() {
    ImGui::BeginDisabled(IsInPlayMode());
    auto operationButton = [&](const char* label, GizmoOperation operation) {
        const bool selected = gizmoOperation_ == operation;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(label)) {
            gizmoOperation_ = operation;
        }
        if (selected) {
            ImGui::PopStyleColor();
        }
    };
    operationButton("Move (W)", GizmoOperation::Translate);
    ImGui::SameLine();
    operationButton("Rotate (E)", GizmoOperation::Rotate);
    ImGui::SameLine();
    operationButton("Scale (R)", GizmoOperation::Scale);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::RadioButton("Local", gizmoSpace_ == GizmoSpace::Local)) {
        gizmoSpace_ = GizmoSpace::Local;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("World", gizmoSpace_ == GizmoSpace::World)) {
        gizmoSpace_ = GizmoSpace::World;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &showSceneGrid_);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox("Physics", &showPhysicsDebug_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show BoxColliders and Character Controllers in Scene View.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!showPhysicsDebug_);
    if (ImGui::SmallButton("Layers##PhysicsDebugLayers")) {
        ImGui::OpenPopup("Physics Debug Layers");
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("Physics Debug Layers")) {
        ImGui::TextUnformatted("Visible Physics Layers");
        if (ImGui::SmallButton("All")) {
            physicsDebugLayerMask_ = 0xffffffffu;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) {
            physicsDebugLayerMask_ = 0u;
        }
        ImGui::Separator();
        for (size_t layer = 0u; layer < PhysicsSettings::kLayerCount; ++layer) {
            if (physicsSettings_.layerNames[layer].empty()) {
                continue;
            }
            ImGui::PushID(static_cast<int>(layer));
            const auto color =
                ImGui::ColorConvertU32ToFloat4(PhysicsDebugLayerColor(static_cast<uint8_t>(layer)));
            ImGui::TextColored(color, "\u25a0");
            ImGui::SameLine();
            bool visible = (physicsDebugLayerMask_ & (uint32_t{1} << layer)) != 0u;
            const std::string label =
                std::to_string(layer) + ": " + physicsSettings_.layerNames[layer];
            if (ImGui::Checkbox(label.c_str(), &visible)) {
                const uint32_t layerBit = uint32_t{1} << layer;
                if (visible) {
                    physicsDebugLayerMask_ |= layerBit;
                } else {
                    physicsDebugLayerMask_ &= ~layerBit;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(IsInPlayMode());
    ImGui::Checkbox("Snap", &gizmoSnapEnabled_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(72.0f);
    if (gizmoOperation_ == GizmoOperation::Translate) {
        ImGui::DragFloat("##GizmoSnap", &translationSnap_, 0.05f, 0.001f, 1000.0f, "%.3f m",
                         ImGuiSliderFlags_AlwaysClamp);
    } else if (gizmoOperation_ == GizmoOperation::Rotate) {
        ImGui::DragFloat("##GizmoSnap", &rotationSnapDegrees_, 0.5f, 0.1f, 180.0f, "%.1f deg",
                         ImGuiSliderFlags_AlwaysClamp);
    } else {
        ImGui::DragFloat("##GizmoSnap", &scaleSnap_, 0.01f, 0.001f, 10.0f, "%.3f",
                         ImGuiSliderFlags_AlwaysClamp);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enable Snap or hold Ctrl while manipulating.");
    }
    ImGui::EndDisabled();
}

bool EditorScene::DrawBoxColliderGizmo(const ImVec2& imageMin, const ImVec2& imageMax) {
    WorldEntity* entity = world_.Find(selection_);
    if (entity == nullptr || !entity->boxCollider || boxColliderGizmoEntity_ != selection_) {
        if (gizmoWasUsing_) {
            CommitHistoryEdit();
            gizmoWasUsing_ = false;
        }
        boxColliderGizmoMode_ = BoxColliderGizmoMode::None;
        boxColliderGizmoEntity_ = {};
        return false;
    }

    DirectX::XMFLOAT4X4 storedEntityWorld{};
    OBB worldCollider{};
    if (!world_.TryGetWorldMatrix(selection_, storedEntityWorld) ||
        !TryBuildWorldBoxCollider(world_, selection_, worldCollider)) {
        return false;
    }

    using namespace DirectX;
    const XMMATRIX entityWorld = XMLoadFloat4x4(&storedEntityWorld);
    XMVECTOR entityScale{};
    XMVECTOR entityRotation{};
    XMVECTOR entityTranslation{};
    if (!XMMatrixDecompose(&entityScale, &entityRotation, &entityTranslation, entityWorld)) {
        return false;
    }

    XMFLOAT4X4 gizmoMatrix{};
    const XMVECTOR colliderCenter = XMLoadFloat3(&worldCollider.center);
    const XMVECTOR colliderRotation = XMLoadFloat4(&worldCollider.rotation);
    if (boxColliderGizmoMode_ == BoxColliderGizmoMode::Center) {
        XMStoreFloat4x4(&gizmoMatrix,
                        XMMatrixAffineTransformation(XMVectorReplicate(1.0f), XMVectorZero(),
                                                     colliderRotation, colliderCenter));
    } else {
        XMStoreFloat4x4(&gizmoMatrix, XMMatrixScaling(worldCollider.size.x, worldCollider.size.y,
                                                      worldCollider.size.z) *
                                          XMMatrixRotationQuaternion(colliderRotation) *
                                          XMMatrixTranslationFromVector(colliderCenter));
    }

    XMFLOAT4X4 view{};
    XMFLOAT4X4 projection{};
    XMStoreFloat4x4(&view, sceneViewCamera_.GetView());
    XMStoreFloat4x4(&projection, sceneViewCamera_.GetProj());
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x, imageMax.y - imageMin.y);
    ImGuizmo::SetOrthographic(false);

    const ImGuizmo::OPERATION operation = boxColliderGizmoMode_ == BoxColliderGizmoMode::Center
                                              ? ImGuizmo::TRANSLATE
                                              : ImGuizmo::SCALE;
    float snapValues[3]{};
    std::ranges::fill(snapValues, boxColliderGizmoMode_ == BoxColliderGizmoMode::Center
                                      ? translationSnap_
                                      : scaleSnap_);
    const bool snapActive = gizmoSnapEnabled_ || ImGui::GetIO().KeyCtrl;
    const bool manipulated =
        ImGuizmo::Manipulate(&view._11, &projection._11, operation, ImGuizmo::LOCAL,
                             &gizmoMatrix._11, nullptr, snapActive ? snapValues : nullptr);
    const bool usingNow = ImGuizmo::IsUsing();
    if (usingNow && !gizmoWasUsing_) {
        BeginHistoryEdit(boxColliderGizmoMode_ == BoxColliderGizmoMode::Center
                             ? "Modify BoxCollider Center"
                             : "Modify BoxCollider Size");
    }

    if (manipulated) {
        BoxColliderComponent& collider = *entity->boxCollider;
        if (boxColliderGizmoMode_ == BoxColliderGizmoMode::Center) {
            XMVECTOR determinant{};
            const XMMATRIX inverseEntity = XMMatrixInverse(&determinant, entityWorld);
            const float determinantValue = XMVectorGetX(determinant);
            if (std::isfinite(determinantValue) && std::abs(determinantValue) > 1.0e-8f) {
                XMVECTOR scale{};
                XMVECTOR rotation{};
                XMVECTOR translation{};
                if (XMMatrixDecompose(&scale, &rotation, &translation,
                                      XMLoadFloat4x4(&gizmoMatrix))) {
                    XMStoreFloat3(&collider.center,
                                  XMVector3TransformCoord(translation, inverseEntity));
                    RefreshDirty();
                }
            }
        } else {
            XMVECTOR manipulatedScale{};
            XMVECTOR rotation{};
            XMVECTOR translation{};
            if (XMMatrixDecompose(&manipulatedScale, &rotation, &translation,
                                  XMLoadFloat4x4(&gizmoMatrix))) {
                XMFLOAT3 storedManipulatedScale{};
                XMFLOAT3 storedEntityScale{};
                XMStoreFloat3(&storedManipulatedScale, manipulatedScale);
                XMStoreFloat3(&storedEntityScale, entityScale);
                const auto localSize = [](float worldSize, float worldScale) {
                    constexpr float minimumSize = 0.001f;
                    constexpr float minimumScale = 1.0e-6f;
                    return (std::max)(minimumSize,
                                      std::abs(worldSize) /
                                          (std::max)(minimumScale, std::abs(worldScale)));
                };
                collider.size = {localSize(storedManipulatedScale.x, storedEntityScale.x),
                                 localSize(storedManipulatedScale.y, storedEntityScale.y),
                                 localSize(storedManipulatedScale.z, storedEntityScale.z)};
                RefreshDirty();
            }
        }
    }

    if (!usingNow && gizmoWasUsing_) {
        CommitHistoryEdit();
        status_ = boxColliderGizmoMode_ == BoxColliderGizmoMode::Center
                      ? "Modified BoxCollider center from Scene View."
                      : "Modified BoxCollider size from Scene View.";
    }
    gizmoWasUsing_ = usingNow;
    return ImGuizmo::IsOver() || usingNow;
}
bool EditorScene::DrawCharacterControllerGizmo(const ImVec2& imageMin, const ImVec2& imageMax) {
    WorldEntity* entity = world_.Find(selection_);
    if (entity == nullptr || !entity->characterController ||
        characterControllerGizmoEntity_ != selection_) {
        if (gizmoWasUsing_) {
            CommitHistoryEdit();
            gizmoWasUsing_ = false;
        }
        characterControllerGizmoMode_ = CharacterControllerGizmoMode::None;
        characterControllerGizmoEntity_ = {};
        return false;
    }

    DirectX::XMFLOAT4X4 storedEntityWorld{};
    CharacterCapsule worldCapsule{};
    if (!world_.TryGetWorldMatrix(selection_, storedEntityWorld) ||
        !TryBuildWorldCharacterCapsule(world_, selection_, worldCapsule)) {
        return false;
    }

    using namespace DirectX;
    const XMMATRIX entityWorld = XMLoadFloat4x4(&storedEntityWorld);
    XMVECTOR entityScale{};
    XMVECTOR entityRotation{};
    XMVECTOR entityTranslation{};
    if (!XMMatrixDecompose(&entityScale, &entityRotation, &entityTranslation, entityWorld)) {
        return false;
    }
    XMFLOAT3 storedEntityScale{};
    XMStoreFloat3(&storedEntityScale, entityScale);

    const float worldDiameter = worldCapsule.radius * 2.0f;
    XMFLOAT4X4 gizmoMatrix{};
    if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Center) {
        XMStoreFloat4x4(&gizmoMatrix,
                        XMMatrixTranslation(worldCapsule.center.x, worldCapsule.center.y,
                                            worldCapsule.center.z));
    } else {
        XMStoreFloat4x4(&gizmoMatrix,
                        XMMatrixScaling(worldDiameter, worldCapsule.height, worldDiameter) *
                            XMMatrixTranslation(worldCapsule.center.x, worldCapsule.center.y,
                                                worldCapsule.center.z));
    }

    XMFLOAT4X4 view{};
    XMFLOAT4X4 projection{};
    XMStoreFloat4x4(&view, sceneViewCamera_.GetView());
    XMStoreFloat4x4(&projection, sceneViewCamera_.GetProj());
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x, imageMax.y - imageMin.y);
    ImGuizmo::SetOrthographic(false);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Radius) {
        operation = ImGuizmo::SCALE_X | ImGuizmo::SCALE_Z;
    } else if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Height) {
        operation = ImGuizmo::SCALE_Y;
    }
    float snapValues[3]{};
    std::ranges::fill(snapValues,
                      characterControllerGizmoMode_ == CharacterControllerGizmoMode::Center
                          ? translationSnap_
                          : scaleSnap_);
    const bool snapActive = gizmoSnapEnabled_ || ImGui::GetIO().KeyCtrl;
    const bool manipulated =
        ImGuizmo::Manipulate(&view._11, &projection._11, operation, ImGuizmo::WORLD,
                             &gizmoMatrix._11, nullptr, snapActive ? snapValues : nullptr);
    const bool usingNow = ImGuizmo::IsUsing();
    if (usingNow && !gizmoWasUsing_) {
        const char* label = "Modify CharacterController Center";
        if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Radius) {
            label = "Modify CharacterController Radius";
        } else if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Height) {
            label = "Modify CharacterController Height";
        }
        BeginHistoryEdit(label);
    }

    if (manipulated) {
        CharacterControllerComponent& controller = *entity->characterController;
        if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Center) {
            XMVECTOR determinant{};
            const XMMATRIX inverseEntity = XMMatrixInverse(&determinant, entityWorld);
            const float determinantValue = XMVectorGetX(determinant);
            if (std::isfinite(determinantValue) && std::abs(determinantValue) > 1.0e-8f) {
                XMVECTOR scale{};
                XMVECTOR rotation{};
                XMVECTOR translation{};
                if (XMMatrixDecompose(&scale, &rotation, &translation,
                                      XMLoadFloat4x4(&gizmoMatrix))) {
                    XMStoreFloat3(&controller.center,
                                  XMVector3TransformCoord(translation, inverseEntity));
                    RefreshDirty();
                }
            }
        } else {
            XMVECTOR manipulatedScale{};
            XMVECTOR rotation{};
            XMVECTOR translation{};
            if (XMMatrixDecompose(&manipulatedScale, &rotation, &translation,
                                  XMLoadFloat4x4(&gizmoMatrix))) {
                XMFLOAT3 storedManipulatedScale{};
                XMStoreFloat3(&storedManipulatedScale, manipulatedScale);
                constexpr float minimumScale = 1.0e-6f;
                if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Radius) {
                    const float changedX =
                        std::abs(std::abs(storedManipulatedScale.x) - worldDiameter);
                    const float changedZ =
                        std::abs(std::abs(storedManipulatedScale.z) - worldDiameter);
                    const float newWorldDiameter = changedX >= changedZ
                                                       ? std::abs(storedManipulatedScale.x)
                                                       : std::abs(storedManipulatedScale.z);
                    const float radialScale =
                        (std::max)(minimumScale, (std::max)(std::abs(storedEntityScale.x),
                                                            std::abs(storedEntityScale.z)));
                    controller.radius = (std::max)(0.001f, newWorldDiameter * 0.5f / radialScale);
                    controller.height = (std::max)(controller.height, controller.radius * 2.0f);
                    controller.skinWidth = (std::min)(controller.skinWidth,
                                                      (std::max)(0.0f, controller.radius - 0.001f));
                } else {
                    const float verticalScale =
                        (std::max)(minimumScale, std::abs(storedEntityScale.y));
                    controller.height =
                        (std::max)(controller.radius * 2.0f,
                                   std::abs(storedManipulatedScale.y) / verticalScale);
                    controller.stepOffset = (std::min)(controller.stepOffset, controller.height);
                }
                RefreshDirty();
            }
        }
    }

    if (!usingNow && gizmoWasUsing_) {
        CommitHistoryEdit();
        if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Center) {
            status_ = "Modified CharacterController center from Scene View.";
        } else if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Radius) {
            status_ = "Modified CharacterController radius from Scene View.";
        } else {
            status_ = "Modified CharacterController height from Scene View.";
        }
    }
    gizmoWasUsing_ = usingNow;
    return ImGuizmo::IsOver() || usingNow;
}
bool EditorScene::DrawSceneTransformGizmo(const ImVec2& imageMin, const ImVec2& imageMax) {
    WorldEntity* entity = world_.Find(selection_);
    DirectX::XMFLOAT4X4 worldMatrix{};
    if (entity == nullptr || !world_.TryGetWorldMatrix(selection_, worldMatrix)) {
        if (gizmoWasUsing_) {
            CommitHistoryEdit();
            gizmoWasUsing_ = false;
            activeGizmoEntity_ = {};
            activeGizmoWorldTransforms_.clear();
        }
        return false;
    }

    DirectX::XMFLOAT4X4 view{};
    DirectX::XMFLOAT4X4 projection{};
    DirectX::XMStoreFloat4x4(&view, sceneViewCamera_.GetView());
    DirectX::XMStoreFloat4x4(&projection, sceneViewCamera_.GetProj());
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x, imageMax.y - imageMin.y);
    ImGuizmo::SetOrthographic(false);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    if (gizmoOperation_ == GizmoOperation::Rotate) {
        operation = ImGuizmo::ROTATE;
    } else if (gizmoOperation_ == GizmoOperation::Scale) {
        operation = ImGuizmo::SCALE;
    }
    const ImGuizmo::MODE mode =
        gizmoSpace_ == GizmoSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    float snapValues[3]{};
    if (gizmoOperation_ == GizmoOperation::Translate) {
        std::ranges::fill(snapValues, translationSnap_);
    } else if (gizmoOperation_ == GizmoOperation::Rotate) {
        std::ranges::fill(snapValues, rotationSnapDegrees_);
    } else {
        std::ranges::fill(snapValues, scaleSnap_);
    }
    const bool snapActive = gizmoSnapEnabled_ || ImGui::GetIO().KeyCtrl;
    const DirectX::XMFLOAT4X4 worldBeforeManipulation = worldMatrix;
    const bool manipulated =
        ImGuizmo::Manipulate(&view._11, &projection._11, operation, mode, &worldMatrix._11, nullptr,
                             snapActive ? snapValues : nullptr);
    const bool usingNow = ImGuizmo::IsUsing();
    if (usingNow && !gizmoWasUsing_) {
        SynchronizeHierarchySelection();
        const std::vector<EntityId> roots = GetTopLevelSelectedEntities();
        BeginHistoryEdit(roots.size() > 1u ? "Transform Entities" : "Transform Entity");
        activeGizmoEntity_ = selection_;
        activeGizmoStartWorld_ = worldBeforeManipulation;
        activeGizmoWorldTransforms_.clear();
        activeGizmoWorldTransforms_.reserve(roots.size());
        for (EntityId root : roots) {
            DirectX::XMFLOAT4X4 initialWorld{};
            if (world_.TryGetWorldMatrix(root, initialWorld)) {
                activeGizmoWorldTransforms_.emplace_back(root, initialWorld);
            }
        }
    }

    if (manipulated && activeGizmoEntity_ == selection_) {
        using namespace DirectX;
        XMVECTOR pivotDeterminant{};
        const XMMATRIX inverseStartPivot =
            XMMatrixInverse(&pivotDeterminant, XMLoadFloat4x4(&activeGizmoStartWorld_));
        const float pivotDeterminantValue = XMVectorGetX(pivotDeterminant);
        if (std::isfinite(pivotDeterminantValue) && std::abs(pivotDeterminantValue) > 1.0e-8f) {
            const XMMATRIX groupDelta = inverseStartPivot * XMLoadFloat4x4(&worldMatrix);
            bool changed = false;
            for (const auto& [entityId, initialStoredWorld] : activeGizmoWorldTransforms_) {
                WorldEntity* transformed = world_.Find(entityId);
                if (transformed == nullptr) {
                    continue;
                }
                XMMATRIX localMatrix = XMLoadFloat4x4(&initialStoredWorld) * groupDelta;
                bool canApply = true;
                if (transformed->parent.IsValid()) {
                    XMFLOAT4X4 parentWorld{};
                    if (!world_.TryGetWorldMatrix(transformed->parent, parentWorld)) {
                        canApply = false;
                    } else {
                        XMVECTOR parentDeterminant{};
                        const XMMATRIX inverseParent =
                            XMMatrixInverse(&parentDeterminant, XMLoadFloat4x4(&parentWorld));
                        const float parentDeterminantValue = XMVectorGetX(parentDeterminant);
                        if (std::isfinite(parentDeterminantValue) &&
                            std::abs(parentDeterminantValue) > 1.0e-8f) {
                            localMatrix *= inverseParent;
                        } else {
                            canApply = false;
                        }
                    }
                }
                TransformComponent localTransform = transformed->transform;
                if (canApply && TryDecomposeTransformComponent(localMatrix, localTransform)) {
                    transformed->transform = localTransform;
                    changed = true;
                }
            }
            if (changed) {
                RefreshDirty();
            }
        }
    }

    if (!usingNow && gizmoWasUsing_) {
        CommitHistoryEdit();
        activeGizmoEntity_ = {};
        const size_t transformedCount = activeGizmoWorldTransforms_.size();
        activeGizmoWorldTransforms_.clear();
        status_ = transformedCount > 1u ? "Transformed " + std::to_string(transformedCount) +
                                              " entities from Scene View."
                                        : "Transformed entity from Scene View.";
    }
    gizmoWasUsing_ = usingNow;
    return ImGuizmo::IsOver() || usingNow;
}
