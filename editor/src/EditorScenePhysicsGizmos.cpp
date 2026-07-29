#include "EditorScene.h"

#include "imgui.h"
#include "internal/EditorSceneViewportUtils.h"
#include "world/WorldCollision.h"

#include <algorithm>
#include <array>
#include <cmath>

using namespace DirectX;
using namespace EditorSceneViewportUtils;

namespace {
constexpr float kColliderGuideThickness = 1.5f;
constexpr int kCapsuleSegments = 32;

struct SceneLineDrawer {
    const Camera& camera;
    const ImVec2& imageMin;
    const ImVec2& imageMax;

    SceneLineDrawer(const Camera& cameraValue, const ImVec2& minimum,
                    const ImVec2& maximum)
        : camera(cameraValue), imageMin(minimum), imageMax(maximum) {}

    void Draw(const XMFLOAT3& from, const XMFLOAT3& to, const ImU32 color,
              const float thickness = 1.25f) const {
        ImVec2 screenFrom{};
        ImVec2 screenTo{};
        if (ProjectScenePoint(camera, from, imageMin, imageMax, screenFrom, false) &&
            ProjectScenePoint(camera, to, imageMin, imageMax, screenTo, false)) {
            ImGui::GetWindowDrawList()->AddLine(screenFrom, screenTo, color, thickness);
        }
    }
};

std::array<XMFLOAT3, 8> BuildBoxColliderCorners(const OBB& collider) {
    const XMVECTOR center = XMLoadFloat3(&collider.center);
    const XMVECTOR rotation = XMLoadFloat4(&collider.rotation);
    const XMVECTOR right = XMVector3Rotate(g_XMIdentityR0, rotation);
    const XMVECTOR up = XMVector3Rotate(g_XMIdentityR1, rotation);
    const XMVECTOR forward = XMVector3Rotate(g_XMIdentityR2, rotation);
    const XMFLOAT3 halfSize{
        collider.size.x * 0.5f,
        collider.size.y * 0.5f,
        collider.size.z * 0.5f,
    };
    std::array<XMFLOAT3, 8> corners{};
    size_t cornerIndex = 0;
    for (int z = -1; z <= 1; z += 2) {
        for (int y = -1; y <= 1; y += 2) {
            for (int x = -1; x <= 1; x += 2) {
                XMStoreFloat3(&corners[cornerIndex++],
                              center + right * (halfSize.x * x) + up * (halfSize.y * y) +
                                  forward * (halfSize.z * z));
            }
        }
    }
    return corners;
}

ImU32 BoxColliderGuideColor(const WorldEntity& entity, const bool active,
                            const bool showPhysicsDebug) {
    const BoxColliderComponent& collider = *entity.boxCollider;
    if (collider.isTrigger) {
        return collider.enabled ? IM_COL32(255, 170, 70, 220)
                                : IM_COL32(255, 170, 70, 80);
    }
    if (!active && showPhysicsDebug) {
        return PhysicsDebugLayerColor(entity.layer, collider.enabled);
    }
    return collider.enabled ? IM_COL32(80, 230, 130, 210)
                            : IM_COL32(80, 230, 130, 80);
}

void DrawBoxColliderGuide(const World& world, const WorldEntity& entity,
                          const SceneLineDrawer& drawer, const bool active,
                          const bool drawPhysicsShapes, const bool showPhysicsDebug) {
    if (!entity.boxCollider || (!active && !drawPhysicsShapes)) {
        return;
    }
    OBB collider{};
    if (!TryBuildWorldBoxCollider(world, entity.id, collider)) {
        return;
    }
    constexpr size_t edges[][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
        {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    const std::array<XMFLOAT3, 8> corners = BuildBoxColliderCorners(collider);
    const ImU32 color = BoxColliderGuideColor(entity, active, showPhysicsDebug);
    for (const auto& edge : edges) {
        drawer.Draw(corners[edge[0]], corners[edge[1]], color, kColliderGuideThickness);
    }
}

struct CapsuleGuideDrawer {
    const CharacterCapsule& capsule;
    const SceneLineDrawer& lineDrawer;
    float segmentHalfHeight;
    ImU32 color;

    CapsuleGuideDrawer(const CharacterCapsule& capsuleValue,
                       const SceneLineDrawer& drawer, const float halfHeight,
                       const ImU32 guideColor)
        : capsule(capsuleValue), lineDrawer(drawer), segmentHalfHeight(halfHeight),
          color(guideColor) {}

    [[nodiscard]] XMFLOAT3 Point(const float x, const float y, const float z) const {
        return {capsule.center.x + x, capsule.center.y + y, capsule.center.z + z};
    }

    void DrawRing(const float y) const {
        XMFLOAT3 previous = Point(capsule.radius, y, 0.0f);
        for (int index = 1; index <= kCapsuleSegments; ++index) {
            const float angle =
                XM_2PI * static_cast<float>(index) / static_cast<float>(kCapsuleSegments);
            const XMFLOAT3 point =
                Point(std::cos(angle) * capsule.radius, y, std::sin(angle) * capsule.radius);
            lineDrawer.Draw(previous, point, color, kColliderGuideThickness);
            previous = point;
        }
    }

    void DrawSides() const {
        lineDrawer.Draw(Point(capsule.radius, -segmentHalfHeight, 0.0f),
                        Point(capsule.radius, segmentHalfHeight, 0.0f), color,
                        kColliderGuideThickness);
        lineDrawer.Draw(Point(-capsule.radius, -segmentHalfHeight, 0.0f),
                        Point(-capsule.radius, segmentHalfHeight, 0.0f), color,
                        kColliderGuideThickness);
        lineDrawer.Draw(Point(0.0f, -segmentHalfHeight, capsule.radius),
                        Point(0.0f, segmentHalfHeight, capsule.radius), color,
                        kColliderGuideThickness);
        lineDrawer.Draw(Point(0.0f, -segmentHalfHeight, -capsule.radius),
                        Point(0.0f, segmentHalfHeight, -capsule.radius), color,
                        kColliderGuideThickness);
    }

    void DrawCapArc(const bool xPlane, const bool top) const {
        const float baseY = top ? segmentHalfHeight : -segmentHalfHeight;
        const float angleStart = top ? 0.0f : XM_PI;
        XMFLOAT3 previous = xPlane ? Point(capsule.radius, baseY, 0.0f)
                                   : Point(0.0f, baseY, capsule.radius);
        if (!top) {
            previous = xPlane ? Point(-capsule.radius, baseY, 0.0f)
                              : Point(0.0f, baseY, -capsule.radius);
        }
        for (int index = 1; index <= kCapsuleSegments; ++index) {
            const float angle = angleStart + XM_PI * static_cast<float>(index) /
                                                 static_cast<float>(kCapsuleSegments);
            const float horizontal = std::cos(angle) * capsule.radius;
            const float vertical = std::sin(angle) * capsule.radius;
            const XMFLOAT3 point = xPlane ? Point(horizontal, baseY + vertical, 0.0f)
                                          : Point(0.0f, baseY + vertical, horizontal);
            lineDrawer.Draw(previous, point, color, kColliderGuideThickness);
            previous = point;
        }
    }

    void Draw() const {
        DrawRing(-segmentHalfHeight);
        DrawRing(segmentHalfHeight);
        DrawSides();
        DrawCapArc(true, true);
        DrawCapArc(true, false);
        DrawCapArc(false, true);
        DrawCapArc(false, false);
    }
};

void DrawCharacterControllerGuide(const World& world, const WorldEntity& entity,
                                  const SceneLineDrawer& drawer, const bool active,
                                  const bool drawPhysicsShapes,
                                  const bool showPhysicsDebug) {
    if (!entity.characterController || (!active && !drawPhysicsShapes)) {
        return;
    }
    CharacterCapsule capsule{};
    if (!TryBuildWorldCharacterCapsule(world, entity.id, capsule)) {
        return;
    }
    const bool enabled = entity.characterController->enabled;
    const ImU32 color = !active && showPhysicsDebug
                            ? PhysicsDebugLayerColor(entity.layer, enabled)
                            : enabled ? IM_COL32(70, 220, 210, 220)
                                      : IM_COL32(70, 220, 210, 80);
    const float segmentHalfHeight =
        (std::max)(0.0f, capsule.height * 0.5f - capsule.radius);
    CapsuleGuideDrawer(capsule, drawer, segmentHalfHeight, color).Draw();
}
}

void EditorScene::DrawScenePhysicsGizmos(const WorldEntity& entity, const ImVec2& imageMin,
                                         const ImVec2& imageMax, const bool active,
                                         const bool drawPhysicsShapes) const {
    if (!active && !drawPhysicsShapes) {
        return;
    }
    const SceneLineDrawer drawer(sceneViewCamera_, imageMin, imageMax);
    DrawBoxColliderGuide(world_, entity, drawer, active, drawPhysicsShapes, showPhysicsDebug_);
    DrawCharacterControllerGuide(world_, entity, drawer, active, drawPhysicsShapes,
                                 showPhysicsDebug_);
}
