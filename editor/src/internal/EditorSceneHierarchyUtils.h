#pragma once

#include "core/MathUtils.h"
#include "world/World.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>

namespace EditorSceneHierarchyUtils {
inline constexpr const char* kPrimitiveNames[] = {
    "Box", "Sphere", "Plane", "Cylinder"};
inline constexpr const char* kEntityDragPayload = "EDITOR_ENTITY";
inline constexpr const char* kModelAssetDragPayload = "EDITOR_MODEL_ASSET";
inline constexpr const char* kTextureAssetDragPayload = "EDITOR_TEXTURE_ASSET";
inline constexpr const char* kAudioAssetDragPayload = "EDITOR_AUDIO_ASSET";
inline constexpr const char* kFontAssetDragPayload = "EDITOR_FONT_ASSET";
inline constexpr const char* kScriptAssetDragPayload = "EDITOR_SCRIPT_ASSET";
inline constexpr const char* kPrefabAssetDragPayload = "EDITOR_PREFAB_ASSET";

struct UiAnchorChoice {
    UiAnchor value = UiAnchor::TopLeft;
    const char* label = "";
    DirectX::XMFLOAT2 factor{};
};

inline constexpr std::array<UiAnchorChoice, 9> kUiAnchorChoices = {{
    {UiAnchor::TopLeft, "Top Left", {0.0f, 0.0f}},
    {UiAnchor::TopCenter, "Top Center", {0.5f, 0.0f}},
    {UiAnchor::TopRight, "Top Right", {1.0f, 0.0f}},
    {UiAnchor::MiddleLeft, "Middle Left", {0.0f, 0.5f}},
    {UiAnchor::Center, "Center", {0.5f, 0.5f}},
    {UiAnchor::MiddleRight, "Middle Right", {1.0f, 0.5f}},
    {UiAnchor::BottomLeft, "Bottom Left", {0.0f, 1.0f}},
    {UiAnchor::BottomCenter, "Bottom Center", {0.5f, 1.0f}},
    {UiAnchor::BottomRight, "Bottom Right", {1.0f, 1.0f}},
}};

inline const UiAnchorChoice& GetUiAnchorChoice(UiAnchor anchor) {
    const auto choice = std::ranges::find_if(
        kUiAnchorChoices, [anchor](const UiAnchorChoice& candidate) {
            return candidate.value == anchor;
        });
    return choice != kUiAnchorChoices.end() ? *choice
                                            : kUiAnchorChoices.front();
}

inline bool ContainsCaseInsensitive(std::string value, std::string query) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::ranges::transform(query, query.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value.find(query) != std::string::npos;
}

inline bool TryDecomposeTransformComponent(const DirectX::XMMATRIX& matrix,
                                           TransformComponent& transform) {
    using namespace DirectX;
    XMVECTOR scale{};
    XMVECTOR rotation{};
    XMVECTOR translation{};
    if (!XMMatrixDecompose(&scale, &rotation, &translation, matrix)) {
        return false;
    }
    XMFLOAT3 decomposedScale{};
    XMFLOAT3 decomposedTranslation{};
    XMStoreFloat3(&decomposedScale, scale);
    XMStoreFloat3(&decomposedTranslation, translation);
    const XMFLOAT3 decomposedRotation =
        MathUtils::RotationDegreesFromQuaternion(rotation, transform.rotationDegrees);
    const float values[] = {
        decomposedTranslation.x, decomposedTranslation.y, decomposedTranslation.z,
        decomposedRotation.x, decomposedRotation.y, decomposedRotation.z,
        decomposedScale.x, decomposedScale.y, decomposedScale.z,
    };
    if (!std::ranges::all_of(values, [](float value) {
            return std::isfinite(value);
        })) {
        return false;
    }
    transform.position = decomposedTranslation;
    transform.rotationDegrees = decomposedRotation;
    transform.scale = decomposedScale;
    return true;
}
} // namespace EditorSceneHierarchyUtils
