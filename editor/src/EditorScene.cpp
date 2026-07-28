#include "EditorScene.h"

#include "AssetImportPlanner.h"
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
#include "imgui/ImguiManager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "input/Input.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "model/MeshRenderer.h"
#include "sound/ISoundService.h"
#include "sprite/SpriteRenderer.h"
#include "texture/TextureManager.h"
#include "world/WorldSerializer.h"
#include "world/WorldCollision.h"

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>

#ifdef DrawText
#undef DrawText
#endif

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

namespace {
size_t CountUtf8Codepoints(std::string_view text) {
    return static_cast<size_t>(std::ranges::count_if(
        text, [](char value) {
            return (static_cast<unsigned char>(value) & 0xc0u) !=
                   0x80u;
        }));
}

void PopUtf8Codepoint(std::string& text) {
    if (text.empty()) {
        return;
    }
    size_t index = text.size() - 1u;
    while (index > 0u &&
           (static_cast<unsigned char>(text[index]) & 0xc0u) ==
               0x80u) {
        --index;
    }
    text.erase(index);
}

constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoCollapse;

constexpr const char* kPrimitiveNames[] = {"Box", "Sphere", "Plane", "Cylinder"};
constexpr const char* kEntityDragPayload = "EDITOR_ENTITY";
constexpr const char* kModelAssetDragPayload = "EDITOR_MODEL_ASSET";
constexpr const char* kTextureAssetDragPayload = "EDITOR_TEXTURE_ASSET";
constexpr const char* kAudioAssetDragPayload = "EDITOR_AUDIO_ASSET";
constexpr const char* kFontAssetDragPayload = "EDITOR_FONT_ASSET";
constexpr const char* kScriptAssetDragPayload = "EDITOR_SCRIPT_ASSET";
constexpr const char* kPrefabAssetDragPayload = "EDITOR_PREFAB_ASSET";
constexpr size_t kMaxHistoryEntries = 128;
constexpr size_t kMaxRecentScenes = 10;
constexpr float kRuntimeStepDeltaTime = 1.0f / 60.0f;

struct UiAnchorChoice {
    UiAnchor value = UiAnchor::TopLeft;
    const char* label = "";
    DirectX::XMFLOAT2 factor{};
};

constexpr std::array<UiAnchorChoice, 9> kUiAnchorChoices = {{
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

const UiAnchorChoice& GetUiAnchorChoice(UiAnchor anchor) {
    const auto choice = std::ranges::find_if(
        kUiAnchorChoices,
        [anchor](const UiAnchorChoice& candidate) {
            return candidate.value == anchor;
        });
    return choice != kUiAnchorChoices.end() ? *choice
                                            : kUiAnchorChoices.front();
}

const CanvasComponent* FindEnabledCanvas(const World& world,
                                         const WorldEntity& entity) {
    const WorldEntity* current = &entity;
    while (current != nullptr) {
        if (current->canvas) {
            return current->canvas->enabled ? &*current->canvas : nullptr;
        }
        current = current->parent.IsValid() ? world.Find(current->parent)
                                            : nullptr;
    }
    return nullptr;
}

struct UiGroupState {
    float alpha = 1.0f;
    bool interactable = true;
    bool blocksRaycasts = true;
};

UiGroupState GetUiGroupState(const World& world,
                             const WorldEntity& entity) {
    UiGroupState state{};
    const WorldEntity* current = &entity;
    while (current != nullptr) {
        if (current->canvasGroup && current->canvasGroup->enabled) {
            const CanvasGroupComponent& group = *current->canvasGroup;
            state.alpha *= group.alpha;
            state.interactable =
                state.interactable && group.interactable;
            state.blocksRaycasts =
                state.blocksRaycasts && group.blocksRaycasts;
        }
        current = current->parent.IsValid()
                      ? world.Find(current->parent)
                      : nullptr;
    }
    return state;
}

const WorldEntity* FindEventSystemEntity(const World& world) {
    for (const WorldEntity& entity : world.Entities()) {
        if (entity.eventSystem) {
            return &entity;
        }
    }
    return nullptr;
}

void CalculateCanvasLayout(const CanvasComponent& canvas, float width,
                           float height, float offsetX, float offsetY,
                           float& scale, DirectX::XMFLOAT2& origin,
                           DirectX::XMFLOAT2& layoutResolution) {
    if (canvas.scaleMode == CanvasScaleMode::ConstantPixelSize) {
        scale = 1.0f;
        origin = {offsetX, offsetY};
        layoutResolution = {width, height};
        return;
    }
    layoutResolution = canvas.referenceResolution;
    const float widthScale = width / canvas.referenceResolution.x;
    const float heightScale = height / canvas.referenceResolution.y;
    switch (canvas.screenMatchMode) {
    case CanvasScreenMatchMode::MatchWidthOrHeight:
        scale = std::exp2(std::lerp(std::log2(widthScale),
                                    std::log2(heightScale),
                                    canvas.matchWidthOrHeight));
        break;
    case CanvasScreenMatchMode::Expand:
        scale = (std::min)(widthScale, heightScale);
        break;
    case CanvasScreenMatchMode::Shrink:
        scale = (std::max)(widthScale, heightScale);
        break;
    }
    origin = {
        offsetX +
            (width - canvas.referenceResolution.x * scale) * 0.5f,
        offsetY +
            (height - canvas.referenceResolution.y * scale) * 0.5f,
    };
}

struct OrderedUiEntity {
    const WorldEntity* entity = nullptr;
    int32_t sortingOrder = 0;
};

std::vector<OrderedUiEntity> GetOrderedUiEntities(const World& world) {
    std::vector<OrderedUiEntity> result;
    for (const WorldEntity& entity : world.Entities()) {
        if ((!entity.text && !entity.image && !entity.button) ||
            !world.IsActiveInHierarchy(entity.id)) {
            continue;
        }
        const CanvasComponent* canvas = FindEnabledCanvas(world, entity);
        if (canvas != nullptr) {
            result.push_back({&entity, canvas->sortingOrder});
        }
    }
    std::stable_sort(
        result.begin(), result.end(),
        [](const OrderedUiEntity& left, const OrderedUiEntity& right) {
            return left.sortingOrder < right.sortingOrder;
        });
    return result;
}

struct InputKeyChoice {
    int value;
    const char* label;
};

constexpr std::array<InputKeyChoice, 42> kInputKeyChoices = {{
    {-1, "None"},
    {DIK_A, "A"}, {DIK_B, "B"}, {DIK_C, "C"}, {DIK_D, "D"},
    {DIK_E, "E"}, {DIK_F, "F"}, {DIK_G, "G"}, {DIK_H, "H"},
    {DIK_I, "I"}, {DIK_J, "J"}, {DIK_K, "K"}, {DIK_L, "L"},
    {DIK_M, "M"}, {DIK_N, "N"}, {DIK_O, "O"}, {DIK_P, "P"},
    {DIK_Q, "Q"}, {DIK_R, "R"}, {DIK_S, "S"}, {DIK_T, "T"},
    {DIK_U, "U"}, {DIK_V, "V"}, {DIK_W, "W"}, {DIK_X, "X"},
    {DIK_Y, "Y"}, {DIK_Z, "Z"},
    {DIK_0, "0"}, {DIK_1, "1"}, {DIK_2, "2"}, {DIK_3, "3"},
    {DIK_4, "4"}, {DIK_5, "5"}, {DIK_6, "6"}, {DIK_7, "7"},
    {DIK_8, "8"}, {DIK_9, "9"},
    {DIK_SPACE, "Space"},
    {DIK_LSHIFT, "Left Shift"}, {DIK_RSHIFT, "Right Shift"},
    {DIK_LCONTROL, "Left Ctrl"}, {DIK_RCONTROL, "Right Ctrl"},
}};

struct InputGamepadButtonChoice {
    WORD value = 0;
    const char* label = "";
};

constexpr std::array<InputGamepadButtonChoice, 13> kInputGamepadButtonChoices = {{
    {0, "None"},
    {XINPUT_GAMEPAD_A, "A"}, {XINPUT_GAMEPAD_B, "B"},
    {XINPUT_GAMEPAD_X, "X"}, {XINPUT_GAMEPAD_Y, "Y"},
    {XINPUT_GAMEPAD_LEFT_SHOULDER, "Left Shoulder"},
    {XINPUT_GAMEPAD_RIGHT_SHOULDER, "Right Shoulder"},
    {XINPUT_GAMEPAD_LEFT_THUMB, "Left Stick"},
    {XINPUT_GAMEPAD_RIGHT_THUMB, "Right Stick"},
    {XINPUT_GAMEPAD_DPAD_UP, "D-Pad Up"},
    {XINPUT_GAMEPAD_DPAD_DOWN, "D-Pad Down"},
    {XINPUT_GAMEPAD_START, "Start"}, {XINPUT_GAMEPAD_BACK, "Back"},
}};

struct InputAxisChoice {
    InputActionAxisSource value;
    const char* label;
};

constexpr std::array<InputAxisChoice, 7> kInputAxisChoices = {{
    {InputActionAxisSource::None, "None"},
    {InputActionAxisSource::GamepadLeftX, "Left Stick X"},
    {InputActionAxisSource::GamepadLeftY, "Left Stick Y"},
    {InputActionAxisSource::GamepadRightX, "Right Stick X"},
    {InputActionAxisSource::GamepadRightY, "Right Stick Y"},
    {InputActionAxisSource::GamepadLeftTrigger, "Left Trigger"},
    {InputActionAxisSource::GamepadRightTrigger, "Right Trigger"},
}};

struct InputActionUsage {
    size_t total = 0u;
    size_t button = 0u;
    size_t axis = 0u;
    size_t any = 0u;
    size_t stable = 0u;
    size_t legacy = 0u;
};

std::unordered_map<std::string, InputActionUsage> CollectInputActionUsages(
    const World& world, const BehaviorRegistry& registry, const Input& input) {
    std::unordered_map<std::string, InputActionUsage> usages;
    const auto countReference = [&usages, &input](
                                    std::string_view actionIdentifier,
                                    ScriptInputActionKind kind) {
        const std::string resolvedName = input.GetActionName(actionIdentifier);
        const std::string_view actionName =
            resolvedName.empty() ? actionIdentifier : std::string_view(resolvedName);
        if (actionName.empty()) {
            return;
        }
        InputActionUsage& usage = usages[std::string(actionName)];
        ++usage.total;
        const std::string resolvedId = input.GetActionId(actionIdentifier);
        if (!resolvedId.empty() && actionIdentifier == resolvedId) {
            ++usage.stable;
        } else {
            ++usage.legacy;
        }
        switch (kind) {
        case ScriptInputActionKind::Button:
            ++usage.button;
            break;
        case ScriptInputActionKind::Axis:
            ++usage.axis;
            break;
        case ScriptInputActionKind::Any:
            ++usage.any;
            break;
        }
    };
    for (const WorldEntity& entity : world.Entities()) {
        for (const BehaviorComponent& script : entity.scripts) {
            const std::vector<ScriptPropertyDefinition>* definitions =
                registry.Properties(script.type);
            if (definitions == nullptr) {
                for (const ScriptPropertyValue& property : script.properties) {
                    if (property.type == ScriptPropertyType::InputAction) {
                        countReference(property.stringValue,
                                       ScriptInputActionKind::Any);
                    }
                }
                continue;
            }
            for (const ScriptPropertyDefinition& definition : *definitions) {
                if (definition.type != ScriptPropertyType::InputAction) {
                    continue;
                }
                const auto stored =
                    std::ranges::find(script.properties, definition.name,
                                      &ScriptPropertyValue::name);
                const std::string_view value =
                    stored != script.properties.end() &&
                            stored->type == ScriptPropertyType::InputAction
                        ? std::string_view(stored->stringValue)
                        : std::string_view(definition.defaultString);
                countReference(value, definition.inputActionKind);
            }
        }
    }
    return usages;
}

ImU32 PhysicsDebugLayerColor(uint8_t layer, bool enabled = true) {
    constexpr std::array<std::array<uint8_t, 3>, 8> colors = {{
        {80, 230, 130},
        {80, 170, 255},
        {255, 105, 105},
        {245, 205, 75},
        {185, 120, 255},
        {65, 220, 215},
        {255, 150, 75},
        {245, 110, 190},
    }};
    const auto& color = colors[layer % colors.size()];
    return IM_COL32(color[0], color[1], color[2], enabled ? 220 : 80);
}

bool ContainsCaseInsensitive(std::string value, std::string query) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::ranges::transform(query, query.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value.find(query) != std::string::npos;
}

std::string LowercaseAscii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool HasParentTraversal(const std::filesystem::path& path) {
    return std::ranges::any_of(path, [](const std::filesystem::path& part) {
        return part == L"..";
    });
}

bool IsPathWithinRoot(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return false;
    }
    const std::filesystem::path relative =
        std::filesystem::relative(canonicalPath, canonicalRoot, error);
    return !error && !relative.empty() && !relative.is_absolute() &&
           !HasParentTraversal(relative);
}

Transform DecomposeTransform(const DirectX::XMFLOAT4X4& matrix) {
    using namespace DirectX;
    XMVECTOR scale;
    XMVECTOR rotation;
    XMVECTOR translation;
    Transform result{};
    if (XMMatrixDecompose(&scale, &rotation, &translation, XMLoadFloat4x4(&matrix))) {
        XMStoreFloat3(&result.scale, scale);
        XMStoreFloat4(&result.rotation, rotation);
        XMStoreFloat3(&result.position, translation);
    }
    return result;
}

bool IsPathAtOrWithinRoot(const std::filesystem::path& root,
                          const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, error);
    return !error &&
           (canonicalPath == canonicalRoot || IsPathWithinRoot(canonicalRoot, canonicalPath));
}

bool IsValidAssetFilename(std::string_view filename) {
    if (filename.empty() || filename == "." || filename == ".." ||
        filename.ends_with('.') || filename.ends_with(' ')) {
        return false;
    }
    constexpr std::string_view invalidCharacters = "<>:\"/\\|?*";
    return std::ranges::none_of(filename, [invalidCharacters](unsigned char character) {
        return character < 32u || invalidCharacters.find(static_cast<char>(character)) !=
                                      std::string_view::npos;
    });
}

std::optional<std::filesystem::path> AssetRelativeFromReference(std::string_view reference) {
    constexpr std::string_view uriPrefix = "asset://";
    constexpr std::string_view projectPrefix = "assets/";
    if (reference.starts_with(uriPrefix)) {
        reference.remove_prefix(uriPrefix.size());
    } else if (reference.starts_with(projectPrefix)) {
        reference.remove_prefix(projectPrefix.size());
    } else {
        return std::nullopt;
    }
    const std::filesystem::path relative =
        std::filesystem::path(std::string(reference)).lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory() || HasParentTraversal(relative)) {
        return std::nullopt;
    }
    return relative;
}

bool AssetPathMatches(const std::filesystem::path& candidate,
                      const std::filesystem::path& target, bool directory) {
    const std::string candidateText = candidate.lexically_normal().generic_string();
    const std::string targetText = target.lexically_normal().generic_string();
    return candidateText == targetText ||
           (directory && candidateText.starts_with(targetText + '/'));
}

bool TryDecomposeTransformComponent(const DirectX::XMMATRIX& matrix,
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
    const bool finite = std::ranges::all_of(values, [](float value) {
        return std::isfinite(value);
    });
    if (!finite) {
        return false;
    }
    transform.position = decomposedTranslation;
    transform.rotationDegrees = decomposedRotation;
    transform.scale = decomposedScale;
    return true;
}

bool TryGetModelBounds(const Model& model, DirectX::XMFLOAT3& boundsMin,
                       DirectX::XMFLOAT3& boundsMax) {
    bool found = false;
    for (const ModelSubMesh& subMesh : model.subMeshes) {
        if (subMesh.vertexCount == 0u) {
            continue;
        }
        if (!found) {
            boundsMin = subMesh.sourceBoundsMin;
            boundsMax = subMesh.sourceBoundsMax;
            found = true;
            continue;
        }
        boundsMin.x = (std::min)(boundsMin.x, subMesh.sourceBoundsMin.x);
        boundsMin.y = (std::min)(boundsMin.y, subMesh.sourceBoundsMin.y);
        boundsMin.z = (std::min)(boundsMin.z, subMesh.sourceBoundsMin.z);
        boundsMax.x = (std::max)(boundsMax.x, subMesh.sourceBoundsMax.x);
        boundsMax.y = (std::max)(boundsMax.y, subMesh.sourceBoundsMax.y);
        boundsMax.z = (std::max)(boundsMax.z, subMesh.sourceBoundsMax.z);
    }
    return found;
}

bool IntersectRayBounds(DirectX::FXMVECTOR rayOrigin, DirectX::FXMVECTOR rayDirection,
                        const DirectX::XMFLOAT3& boundsMin,
                        const DirectX::XMFLOAT3& boundsMax, float& distance) {
    DirectX::XMFLOAT3 origin{};
    DirectX::XMFLOAT3 direction{};
    DirectX::XMStoreFloat3(&origin, rayOrigin);
    DirectX::XMStoreFloat3(&direction, rayDirection);

    const float extent = (std::max)({boundsMax.x - boundsMin.x, boundsMax.y - boundsMin.y,
                                     boundsMax.z - boundsMin.z});
    const float padding = (std::max)(0.01f, extent * 0.005f);
    const float minimum[3] = {boundsMin.x - padding, boundsMin.y - padding,
                              boundsMin.z - padding};
    const float maximum[3] = {boundsMax.x + padding, boundsMax.y + padding,
                              boundsMax.z + padding};
    const float rayOriginValues[3] = {origin.x, origin.y, origin.z};
    const float rayDirectionValues[3] = {direction.x, direction.y, direction.z};
    float entry = 0.0f;
    float exit = (std::numeric_limits<float>::max)();
    for (size_t axis = 0; axis < 3; ++axis) {
        if (std::abs(rayDirectionValues[axis]) < 1.0e-7f) {
            if (rayOriginValues[axis] < minimum[axis] ||
                rayOriginValues[axis] > maximum[axis]) {
                return false;
            }
            continue;
        }
        float nearDistance =
            (minimum[axis] - rayOriginValues[axis]) / rayDirectionValues[axis];
        float farDistance =
            (maximum[axis] - rayOriginValues[axis]) / rayDirectionValues[axis];
        if (nearDistance > farDistance) {
            std::swap(nearDistance, farDistance);
        }
        entry = (std::max)(entry, nearDistance);
        exit = (std::min)(exit, farDistance);
        if (entry > exit) {
            return false;
        }
    }
    distance = entry;
    return exit >= 0.0f;
}

bool BuildSceneRay(const Camera& camera, const ImVec2& imageMin, const ImVec2& imageMax,
                   const ImVec2& screenPosition, DirectX::XMVECTOR& rayOrigin,
                   DirectX::XMVECTOR& rayDirection) {
    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    if (width <= 0.0f || height <= 0.0f) {
        return false;
    }
    using namespace DirectX;
    rayOrigin = XMVector3Unproject(
        XMVectorSet(screenPosition.x - imageMin.x, screenPosition.y - imageMin.y, 0.0f, 1.0f),
        0.0f, 0.0f, width, height, 0.0f, 1.0f, camera.GetProj(), camera.GetView(),
        XMMatrixIdentity());
    const XMVECTOR farPoint = XMVector3Unproject(
        XMVectorSet(screenPosition.x - imageMin.x, screenPosition.y - imageMin.y, 1.0f, 1.0f),
        0.0f, 0.0f, width, height, 0.0f, 1.0f, camera.GetProj(), camera.GetView(),
        XMMatrixIdentity());
    rayDirection = XMVector3Normalize(XMVectorSubtract(farPoint, rayOrigin));
    DirectX::XMFLOAT3 origin{};
    DirectX::XMFLOAT3 direction{};
    XMStoreFloat3(&origin, rayOrigin);
    XMStoreFloat3(&direction, rayDirection);
    return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z) &&
           std::isfinite(direction.x) && std::isfinite(direction.y) &&
           std::isfinite(direction.z);
}

bool ProjectScenePoint(const Camera& camera, const DirectX::XMFLOAT3& worldPosition,
                       const ImVec2& imageMin, const ImVec2& imageMax, ImVec2& screenPosition,
                       bool requireInside = true) {
    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    if (width <= 0.0f || height <= 0.0f) {
        return false;
    }
    const DirectX::XMVECTOR clip = DirectX::XMVector4Transform(
        DirectX::XMVectorSet(worldPosition.x, worldPosition.y, worldPosition.z, 1.0f),
        camera.GetViewProjection());
    const float clipW = DirectX::XMVectorGetW(clip);
    if (!std::isfinite(clipW) || clipW <= 1.0e-5f) {
        return false;
    }
    const float ndcX = DirectX::XMVectorGetX(clip) / clipW;
    const float ndcY = DirectX::XMVectorGetY(clip) / clipW;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY) || std::abs(ndcX) > 10000.0f ||
        std::abs(ndcY) > 10000.0f) {
        return false;
    }
    screenPosition = {imageMin.x + (ndcX * 0.5f + 0.5f) * width,
                      imageMin.y + (0.5f - ndcY * 0.5f) * height};
    return !requireInside ||
           (screenPosition.x >= imageMin.x && screenPosition.x <= imageMax.x &&
            screenPosition.y >= imageMin.y && screenPosition.y <= imageMax.y);
}

bool TryGetCameraPreviewRect(const ImVec2& imageMin, const ImVec2& imageMax,
                             ImVec2& previewMin, ImVec2& previewMax) {
    const float availableWidth = imageMax.x - imageMin.x;
    const float availableHeight = imageMax.y - imageMin.y;
    if (availableWidth < 360.0f || availableHeight < 240.0f) {
        return false;
    }
    constexpr float margin = 12.0f;
    const float width = (std::min)(240.0f, availableWidth * 0.36f);
    const float height = width * 9.0f / 16.0f;
    previewMax = {imageMax.x - margin, imageMin.y + margin + height};
    previewMin = {previewMax.x - width, imageMin.y + margin};
    return true;
}

DirectX::XMFLOAT3 CalculateScenePlacementPosition(const Camera& camera,
                                                  const ImVec2& imageMin,
                                                  const ImVec2& imageMax,
                                                  const ImVec2& screenPosition) {
    DirectX::XMVECTOR rayOrigin{};
    DirectX::XMVECTOR rayDirection{};
    DirectX::XMFLOAT3 position{};
    if (!BuildSceneRay(camera, imageMin, imageMax, screenPosition, rayOrigin, rayDirection)) {
        return position;
    }
    DirectX::XMFLOAT3 origin{};
    DirectX::XMFLOAT3 direction{};
    DirectX::XMStoreFloat3(&origin, rayOrigin);
    DirectX::XMStoreFloat3(&direction, rayDirection);
    float distance = 5.0f;
    if (std::abs(direction.y) > 1.0e-5f) {
        const float groundDistance = -origin.y / direction.y;
        if (groundDistance >= 0.0f) {
            distance = groundDistance;
        }
    }
    DirectX::XMStoreFloat3(
        &position,
        DirectX::XMVectorAdd(rayOrigin, DirectX::XMVectorScale(rayDirection, distance)));
    position.y = 0.0f;
    return position;
}

bool IsPrefabAsset(const std::filesystem::path& path) {
    return LowercaseAscii(path.extension().string()) == ".likeprefab";
}

}

EditorScene::EditorScene(std::filesystem::path projectRoot, std::filesystem::path assetRoot,
                         std::filesystem::path sceneRoot,
                         std::filesystem::path startupScene,
                         std::filesystem::path recentScenesPath,
                         std::filesystem::path imguiSettingsPath,
                         std::function<void()> requestClose, bool playerMode)
    : requestClose_(std::move(requestClose)), playerMode_(playerMode),
      projectRoot_(std::move(projectRoot)),
      assetRoot_(std::move(assetRoot)), sceneRoot_(std::move(sceneRoot)),
      startupScenePath_(startupScene),
      imguiSettingsPath_(std::move(imguiSettingsPath)),
      playerSettingsStore_(projectRoot_ / L"settings" / L"player.json"),
      physicsSettingsStore_(projectRoot_ / L"settings" / L"physics.json"),
      inputSettingsStore_(projectRoot_ / L"settings" / L"input.json"),
      recentScenesStore_(std::move(recentScenesPath), sceneRoot_),
      scenePath_(startupScene), runtimeScenePath_(std::move(startupScene)) {
    if (playerMode_) {
        showHierarchyPanel_ = false;
        showProjectPanel_ = false;
        showScenePanel_ = false;
        showConsolePanel_ = false;
        showInspectorPanel_ = false;
    }
    std::string playerSettingsError;
    const bool playerSettingsLoaded =
        playerSettingsStore_.Load(playerSettings_, playerSettingsError);
    std::string physicsSettingsError;
    const bool physicsSettingsLoaded =
        physicsSettingsStore_.Load(physicsSettings_, physicsSettingsError);
    world_.SetPhysicsSettings(physicsSettings_);
    recentScenePaths_ = recentScenesStore_.Load();
    std::error_code error;
    if (std::filesystem::is_regular_file(scenePath_, error) && !error) {
        if (!LoadScene(scenePath_)) {
            NewScene(false);
        }
    } else {
        NewScene(false);
    }
    ClearHistory(true);
    if (!physicsSettingsLoaded) {
        status_ = "Warning: Could not load Physics Settings: " + physicsSettingsError;
    } else if (!playerSettingsLoaded) {
        status_ = "Warning: Could not load Player Settings: " + playerSettingsError;
    }
}

void EditorScene::Initialize(const SceneContext& ctx) {
    BaseScene::Initialize(ctx);
    std::string inputSettingsError;
    if (ctx.systems.input == nullptr ||
        !inputSettingsStore_.Load(*ctx.systems.input, inputSettingsError)) {
        status_ = "Warning: Could not load Input Settings: " +
                  (inputSettingsError.empty() ? std::string("Input service is unavailable.")
                                              : inputSettingsError);
    }
    std::string behaviorRequirementError;
    std::string scriptModuleError;
    if ((!playerMode_ &&
         !ScriptBuildService::BuildIfNeeded(projectRoot_, scriptModuleError)) ||
        !projectScripts_.Load(projectRoot_, ctx.systems.input, behaviorRegistry_,
                              scriptModuleError)) {
        status_ = "Error: " + scriptModuleError;
    } else if (!ValidateWorldBehaviorRequirements(&behaviorRequirementError)) {
        status_ = "Error: Scene contains an invalid Behavior: " +
                  behaviorRequirementError;
    } else {
        const size_t upgradedReferences = UpgradeInputActionReferences();
        if (upgradedReferences != 0u) {
            status_ = "Upgraded " + std::to_string(upgradedReferences) +
                      " Input Action reference(s) to stable IDs.";
        }
    }
    if (ctx.systems.imgui == nullptr ||
        !ctx.systems.imgui->ConfigureDocking(imguiSettingsPath_)) {
        status_ = "Could not configure the Editor docking layout.";
    }
    if (ctx.rendering.dxCommon == nullptr || ctx.rendering.srv == nullptr ||
        !sceneViewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 960, 540)) {
        status_ = "Scene View RenderSurface initialization failed.";
        return;
    }
    if (!gameViewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 960, 540)) {
        status_ = "Game View RenderSurface initialization failed.";
    }
    if (!cameraPreviewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 320, 180)) {
        status_ = "Camera Preview RenderSurface initialization failed.";
    }
    if (!assetPreviewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 320, 320)) {
        status_ = "Asset Preview RenderSurface initialization failed.";
    }
    if (ctx.rendering.model == nullptr || ctx.rendering.meshRenderer == nullptr ||
        ctx.rendering.texture == nullptr) {
        status_ = "Scene View rendering services are unavailable.";
        return;
    }
    sceneRenderer_.Initialize(ctx.rendering.meshRenderer);
    sceneGridPipelineId_ = ctx.rendering.meshRenderer->CreatePipeline(
        ShaderPaths::MeshVS, ShaderPaths::EditorGridPS);
    Material material{};
    material.enableTexture = 0;
    const uint32_t whiteTexture = ctx.rendering.texture->GetWhiteTextureId();
    primitiveModels_[static_cast<size_t>(MeshPrimitive::Box)] =
        ctx.rendering.model->CreateBoxHandle(whiteTexture, material);
    primitiveModels_[static_cast<size_t>(MeshPrimitive::Sphere)] =
        ctx.rendering.model->CreateSphereHandle(whiteTexture, material);
    primitiveModels_[static_cast<size_t>(MeshPrimitive::Plane)] =
        ctx.rendering.model->CreatePlaneHandle(whiteTexture, material);
    primitiveModels_[static_cast<size_t>(MeshPrimitive::Cylinder)] =
        ctx.rendering.model->CreateCylinderHandle(whiteTexture, material);
    sceneViewCamera_.SetPosition({0.0f, 0.35f, -4.0f});
    sceneViewCamera_.SetRotation({0.08f, 0.0f, 0.0f});
    sceneViewCamera_.Initialize(960.0f / 540.0f);
    gameViewCamera_.Initialize(960.0f / 540.0f);
    cameraPreviewCamera_.Initialize(16.0f / 9.0f);
    assetPreviewCamera_.SetPosition({0.0f, 0.0f, -4.0f});
    assetPreviewCamera_.SetRotation({0.0f, 0.0f, 0.0f});
    assetPreviewCamera_.Initialize(1.0f);
    RefreshAssetBrowser();
    ResolveMeshResources();
    InitializeScriptMonitoring();
    if (playerMode_) {
        EnterPlayMode();
    }
}

void EditorScene::Update() {
    if (!playerMode_) {
        UpdateScriptCompilation();
    }
    if (playModeState_ == PlayModeState::Playing && ctx_ != nullptr) {
        UpdateRuntimeWorld(ctx_->frame.deltaTime);
    } else if (playModeState_ == PlayModeState::Edit && ctx_ != nullptr) {
        UpdateEditAnimatorPreview(ctx_->frame.deltaTime);
    }
    ResolveMeshResources();
    if (sceneViewSurface_.IsReady() && sceneViewPostProcess_.IsReady() && ctx_ != nullptr &&
        ctx_->rendering.dxCommon != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording() &&
        (requestedSceneWidth_ != sceneViewSurface_.GetWidth() ||
         requestedSceneHeight_ != sceneViewSurface_.GetHeight())) {
        const int width = (std::max)(1, requestedSceneWidth_);
        const int height = (std::max)(1, requestedSceneHeight_);
        if (sceneViewSurface_.Resize(width, height) &&
            sceneViewPostProcess_.Resize(width, height)) {
            sceneViewCamera_.SetAspect(static_cast<float>(width) / static_cast<float>(height));
        } else {
            status_ = "Scene View resize failed.";
        }
    }
    if (!postProcessInitializationAttempted_ && sceneViewSurface_.IsReady() && ctx_ != nullptr &&
        ctx_->rendering.dxCommon != nullptr && ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        postProcessInitializationAttempted_ = true;
        sceneViewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                         sceneViewSurface_.GetWidth(),
                                         sceneViewSurface_.GetHeight());
        if (!sceneViewPostProcess_.IsReady()) {
            status_ = "Scene View PostProcess initialization failed.";
        }
    }
    if (gameViewSurface_.IsReady() && gameViewPostProcess_.IsReady() && ctx_ != nullptr &&
        ctx_->rendering.dxCommon != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording() &&
        (requestedGameWidth_ != gameViewSurface_.GetWidth() ||
         requestedGameHeight_ != gameViewSurface_.GetHeight())) {
        const int width = (std::max)(1, requestedGameWidth_);
        const int height = (std::max)(1, requestedGameHeight_);
        if (!gameViewSurface_.Resize(width, height) ||
            !gameViewPostProcess_.Resize(width, height)) {
            status_ = "Game View resize failed.";
        }
    }
    if (!gamePostProcessInitializationAttempted_ && gameViewSurface_.IsReady() &&
        ctx_ != nullptr && ctx_->rendering.dxCommon != nullptr &&
        ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        gamePostProcessInitializationAttempted_ = true;
        gameViewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                        gameViewSurface_.GetWidth(),
                                        gameViewSurface_.GetHeight());
        if (!gameViewPostProcess_.IsReady()) {
            status_ = "Game View PostProcess initialization failed.";
        }
    }
    if (!cameraPreviewPostProcessInitializationAttempted_ && cameraPreviewSurface_.IsReady() &&
        ctx_ != nullptr && ctx_->rendering.dxCommon != nullptr &&
        ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        cameraPreviewPostProcessInitializationAttempted_ = true;
        cameraPreviewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                             cameraPreviewSurface_.GetWidth(),
                                             cameraPreviewSurface_.GetHeight());
        if (!cameraPreviewPostProcess_.IsReady()) {
            status_ = "Camera Preview PostProcess initialization failed.";
        }
    }
    if (!assetPreviewPostProcessInitializationAttempted_ && assetPreviewSurface_.IsReady() &&
        ctx_ != nullptr && ctx_->rendering.dxCommon != nullptr && ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        assetPreviewPostProcessInitializationAttempted_ = true;
        assetPreviewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                            assetPreviewSurface_.GetWidth(),
                                            assetPreviewSurface_.GetHeight());
        if (!assetPreviewPostProcess_.IsReady()) {
            status_ = "Asset Preview PostProcess initialization failed.";
        }
    }
    UpdateAssetPreview();
}

void EditorScene::SubmitLighting(LightingScene& lightingScene) {
    SceneLighting lighting{};
    bool directionalAssigned = false;
    size_t pointLightIndex = 0u;
    bool spotAssigned = false;
    for (const WorldEntity& entity : world_.Entities()) {
        if (!world_.IsActiveInHierarchy(entity.id) || !entity.light ||
            !entity.light->enabled || entity.light->intensity <= 0.0f) {
            continue;
        }
        DirectX::XMFLOAT4X4 storedWorld{};
        if (!world_.TryGetWorldMatrix(entity.id, storedWorld)) {
            continue;
        }
        const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&storedWorld);
        DirectX::XMVECTOR direction = DirectX::XMVector3TransformNormal(
            DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), world);
        if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(direction)) <= 1.0e-8f) {
            continue;
        }
        direction = DirectX::XMVector3Normalize(direction);
        DirectX::XMFLOAT3 storedDirection{};
        DirectX::XMStoreFloat3(&storedDirection, direction);
        const LightComponent& component = *entity.light;
        if (component.type == LightType::Directional && !directionalAssigned) {
            lighting.keyLightDirection = storedDirection;
            lighting.keyLightColor = {component.color.x * component.intensity,
                                      component.color.y * component.intensity,
                                      component.color.z * component.intensity, 1.0f};
            directionalAssigned = true;
        } else if (component.type == LightType::Point &&
                   pointLightIndex < lighting.pointLights.size()) {
            PointLight& point = lighting.pointLights[pointLightIndex++];
            point.positionRange = {storedWorld._41, storedWorld._42, storedWorld._43,
                                   component.range};
            point.colorIntensity = {component.color.x, component.color.y, component.color.z,
                                    component.intensity};
        } else if (component.type == LightType::Spot && !spotAssigned) {
            SpotLight& spot = lighting.spotLight;
            spot.positionRange = {storedWorld._41, storedWorld._42, storedWorld._43,
                                  component.range};
            spot.direction = {storedDirection.x, storedDirection.y, storedDirection.z, 0.0f};
            spot.colorIntensity = {component.color.x, component.color.y, component.color.z,
                                   component.intensity};
            spot.angleParams = {
                std::cos(DirectX::XMConvertToRadians(component.innerAngleDegrees)),
                std::cos(DirectX::XMConvertToRadians(component.outerAngleDegrees)), 2.4f, 1.0f};
            spotAssigned = true;
        }
    }
    lightingScene.SetSceneLighting(lighting);
}

void EditorScene::Draw() {}

void EditorScene::DrawPostProcessOverlay() {
    ImGuizmo::BeginFrame();
    CaptureConsoleStatus();
    if (playerMode_) {
        if (gameInputCaptured_ &&
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ReleaseGameInputCapture();
        }
        DrawPanels();
        return;
    }
    HandleEditorShortcuts();
    DrawMainMenu();
    DrawDockSpace();
    DrawUnsavedChangesDialog();
    DrawEntityRenameDialog();
    DrawAssetOperationDialogs();
    DrawPanels();
    DrawProjectSettingsWindow();
    CaptureConsoleStatus();
}

bool EditorScene::OnCloseRequested() {
    if (IsInPlayMode()) {
        StopPlayMode();
    }
    if (physicsSettingsDirty_ && !SavePhysicsSettings()) {
        return false;
    }
    if (playerSettingsDirty_ && !SavePlayerSettings()) {
        return false;
    }
    if (inputSettingsDirty_ && !SaveInputSettings()) {
        return false;
    }
    if (!dirty_) {
        return true;
    }
    RequestSceneAction(PendingSceneAction::Exit);
    return false;
}

void EditorScene::OnFilesDropped(std::span<const std::filesystem::path> files, int screenX,
                                 int screenY) {
    if (playerMode_) {
        return;
    }
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before importing assets.";
        return;
    }
    const bool overProject = static_cast<float>(screenX) >= projectPanelMinX_ &&
                             static_cast<float>(screenX) < projectPanelMaxX_ &&
                             static_cast<float>(screenY) >= projectPanelMinY_ &&
                             static_cast<float>(screenY) < projectPanelMaxY_;
    if (!overProject) {
        status_ = "Drop model files onto the Project panel to import them.";
        return;
    }
    ImportAssetFiles(std::vector<std::filesystem::path>(files.begin(), files.end()));
}

bool EditorScene::LaunchPlayerPreview() {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before running the Player Preview.";
        return false;
    }
    if (dirty_) {
        status_ = "Save the scene before running the Player Preview.";
        return false;
    }
    if (playerSettingsDirty_ || physicsSettingsDirty_ || inputSettingsDirty_) {
        status_ = "Save Project Settings before running the Player Preview.";
        return false;
    }
    if (scriptBuildInProgress_ || scriptBuildPending_) {
        status_ = "Wait for Project Script compilation before running the Player Preview.";
        return false;
    }
    ProjectDescriptor project;
    std::string validationError;
    if (!ProjectDescriptor::Load(projectRoot_, project, validationError) ||
        !PlayerProjectValidator::Validate(project, validationError)) {
        status_ = "Could not run Player Preview: " + validationError;
        return false;
    }
    std::array<wchar_t, 32768> executableBuffer{};
    const DWORD executableLength = GetModuleFileNameW(
        nullptr, executableBuffer.data(),
        static_cast<DWORD>(executableBuffer.size()));
    if (executableLength == 0u ||
        executableLength >= executableBuffer.size()) {
        status_ = "Could not locate the Editor executable.";
        return false;
    }
    const std::filesystem::path executable(
        std::wstring(executableBuffer.data(), executableLength));
    std::wstring command = L"\"" + executable.wstring() +
                           L"\" --player --project \"" +
                           projectRoot_.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr,
                        nullptr, FALSE, 0u, nullptr, projectRoot_.c_str(),
                        &startup, &process)) {
        status_ = "Could not launch the Player Preview.";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    status_ = "Launched Player Preview.";
    return true;
}

bool EditorScene::BuildPlayerPackage(std::filesystem::path* destination) {
    if (IsInPlayMode() || dirty_ || playerSettingsDirty_ || physicsSettingsDirty_ ||
        inputSettingsDirty_) {
        status_ = "Save the scene and Project Settings before building.";
        return false;
    }
    if (scriptBuildInProgress_ || scriptBuildPending_) {
        status_ = "Wait for Project Script compilation before building.";
        return false;
    }
    ProjectDescriptor project;
    std::string error;
    if (!ProjectDescriptor::Load(projectRoot_, project, error) ||
        !PlayerProjectValidator::Validate(project, error)) {
        status_ = "Could not build Player: " + error;
        return false;
    }
    std::array<wchar_t, 32768> executableBuffer{};
    const DWORD executableLength = GetModuleFileNameW(
        nullptr, executableBuffer.data(),
        static_cast<DWORD>(executableBuffer.size()));
    if (executableLength == 0u ||
        executableLength >= executableBuffer.size()) {
        status_ = "Could not locate the Player executable.";
        return false;
    }
#ifdef _DEBUG
    constexpr char configuration[] = "Debug";
    constexpr wchar_t outputName[] = L"windows-x64-debug";
#else
    constexpr char configuration[] = "Release";
    constexpr wchar_t outputName[] = L"windows-x64";
#endif
    const PlayerPackageRequest request{
        .executable = std::filesystem::path(
            std::wstring(executableBuffer.data(), executableLength)),
        .projectRoot = project.root,
        .manifest = project.manifestPath,
        .assetRoot = project.assetRoot,
        .sceneRoot = project.sceneRoot,
        .destination = project.root / L"build" / outputName,
        .configuration = configuration,
    };
    if (!PlayerPackageBuilder::Build(request, error)) {
        status_ = "Could not build Player: " + error;
        return false;
    }
    if (destination != nullptr) {
        *destination = request.destination;
    }
    status_ = "Built Player package: " +
              request.destination.generic_string();
    if (!error.empty()) {
        status_ += " Warning: " + error;
    }
    return true;
}

bool EditorScene::LaunchPackagedPlayer(
    const std::filesystem::path& package) {
    const std::filesystem::path executable = package / L"Game.exe";
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(executable, filesystemError) ||
        filesystemError) {
        status_ = "Could not run Player: Game.exe was not found in the package.";
        return false;
    }
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr,
                        nullptr, FALSE, 0u, nullptr, package.c_str(), &startup,
                        &process)) {
        status_ = "Could not run the packaged Player.";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    status_ = "Built and launched Player package: " +
              package.generic_string();
    return true;
}

bool EditorScene::BuildAndRunPlayerPackage() {
    std::filesystem::path package;
    return BuildPlayerPackage(&package) && LaunchPackagedPlayer(package);
}

void EditorScene::DrawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        const bool editing = !IsInPlayMode();
        if (ImGui::MenuItem("New Scene", "Ctrl+N", false, editing)) {
            RequestSceneAction(PendingSceneAction::NewScene);
        }
        if (ImGui::MenuItem("Open Scene...", "Ctrl+O", false, editing)) {
            RequestSceneAction(PendingSceneAction::OpenScene);
        }
        if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, editing)) {
            SaveScene();
        }
        if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, editing)) {
            SaveSceneAs();
        }
        if (ImGui::BeginMenu("Recent Scenes", editing && !recentScenePaths_.empty())) {
            for (const std::filesystem::path& path : recentScenePaths_) {
                std::error_code error;
                std::filesystem::path label =
                    std::filesystem::relative(path, sceneRoot_, error);
                if (error) {
                    label = path.filename();
                }
                const std::string text = label.generic_string();
                if (ImGui::MenuItem(text.c_str())) {
                    RequestSceneAction(PendingSceneAction::OpenScene, path);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reload Scene", nullptr, false, editing)) {
            RequestSceneAction(PendingSceneAction::ReloadScene);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            RequestSceneAction(PendingSceneAction::Exit);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Build")) {
        if (ImGui::MenuItem("Build Project", nullptr, false,
                            !IsInPlayMode())) {
            BuildPlayerPackage();
        }
        if (ImGui::MenuItem("Build And Run", "F9", false,
                            !IsInPlayMode())) {
            BuildAndRunPlayerPackage();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Run Project", "F8", false,
                            !IsInPlayMode())) {
            LaunchPlayerPreview();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool editing = !IsInPlayMode();
        const bool canUndo = editing && !undoHistory_.empty();
        const bool canRedo = editing && !redoHistory_.empty();
        const bool canDuplicate = editing && world_.Contains(selection_);
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
            Undo();
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) {
            Redo();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A", false, !world_.Empty())) {
            SelectAllHierarchyEntities();
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, canDuplicate)) {
            CopySelection();
        }
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, canDuplicate)) {
            CutSelection();
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false,
                            editing && !entityClipboard_.empty())) {
            PasteEntityClipboard();
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, canDuplicate)) {
            DuplicateSelection();
        }
        if (ImGui::MenuItem("Delete", "Delete", false, canDuplicate)) {
            DeleteSelection();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Project Settings...")) {
            showProjectSettings_ = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Hierarchy", nullptr, &showHierarchyPanel_);
            ImGui::MenuItem("Project", nullptr, &showProjectPanel_);
            ImGui::MenuItem("Scene", nullptr, &showScenePanel_);
            ImGui::MenuItem("Game", nullptr, &showGamePanel_);
            ImGui::MenuItem("Console", nullptr, &showConsolePanel_);
            ImGui::MenuItem("Inspector", nullptr, &showInspectorPanel_);
            ImGui::Separator();
            if (ImGui::MenuItem("Show All Panels")) {
                showHierarchyPanel_ = true;
                showProjectPanel_ = true;
                showScenePanel_ = true;
                showGamePanel_ = true;
                showConsolePanel_ = true;
                showInspectorPanel_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reset Panel Layout")) {
            showHierarchyPanel_ = true;
            showProjectPanel_ = true;
            showScenePanel_ = true;
            showGamePanel_ = true;
            showConsolePanel_ = true;
            showInspectorPanel_ = true;
            resetDockLayoutRequested_ = true;
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    const bool playing = playModeState_ == PlayModeState::Playing;
    const bool paused = playModeState_ == PlayModeState::Paused;
    if (paused) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.18f, 0.48f, 0.24f, 1.0f});
    }
    ImGui::BeginDisabled(playing);
    if (ImGui::Button(paused ? "Resume" : "Play")) {
        if (playModeState_ == PlayModeState::Edit) {
            EnterPlayMode();
        } else if (paused) {
            TogglePlayPause();
        }
    }
    ImGui::EndDisabled();
    if (paused) {
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (paused) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.58f, 0.40f, 0.12f, 1.0f});
    }
    if (ImGui::Button("Pause")) {
        TogglePlayPause();
    }
    if (paused) {
        ImGui::PopStyleColor();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!paused);
    if (ImGui::Button("Step")) {
        StepRuntimeWorld();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Advance the paused Runtime World by one 1/60-second update (F7).");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!IsInPlayMode());
    if (ImGui::Button("Stop")) {
        StopPlayMode();
    }
    ImGui::EndDisabled();
    std::string editorLabel = "LikeEngine Editor - ";
    editorLabel += scenePath_.empty() ? "Untitled" : scenePath_.filename().string();
    if (dirty_) {
        editorLabel += " *";
    }
    const bool titlePlaying = playModeState_ == PlayModeState::Playing;
    const bool titlePaused = playModeState_ == PlayModeState::Paused;
    if (titlePlaying) {
        editorLabel += "  [PLAYING]";
    } else if (titlePaused) {
        editorLabel += "  [PAUSED]";
    }
    if (IsInPlayMode()) {
        char runtimeStatus[64]{};
        sprintf_s(runtimeStatus, "  Frame %llu | %.2fs",
                  static_cast<unsigned long long>(runtimeFrameCount_),
                  runtimeElapsedSeconds_);
        editorLabel += runtimeStatus;
    }
    ImGui::TextUnformatted(editorLabel.c_str());
    ImGui::EndMainMenuBar();
}

void EditorScene::DrawUnsavedChangesDialog() {
    if (showUnsavedChangesDialog_) {
        ImGui::OpenPopup("Unsaved Changes");
        showUnsavedChangesDialog_ = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted("The current scene has unsaved changes.");
    ImGui::TextUnformatted("Save before continuing?");
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(100.0f, 0.0f))) {
        if (SaveScene()) {
            const PendingSceneAction action = pendingSceneAction_;
            const std::filesystem::path path = pendingScenePath_;
            pendingSceneAction_ = PendingSceneAction::None;
            pendingScenePath_.clear();
            ImGui::CloseCurrentPopup();
            ExecuteSceneAction(action, path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't Save", ImVec2(100.0f, 0.0f))) {
        const PendingSceneAction action = pendingSceneAction_;
        const std::filesystem::path path = pendingScenePath_;
        pendingSceneAction_ = PendingSceneAction::None;
        pendingScenePath_.clear();
        ImGui::CloseCurrentPopup();
        ExecuteSceneAction(action, path);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
        pendingSceneAction_ = PendingSceneAction::None;
        pendingScenePath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorScene::DrawEntityRenameDialog() {
    if (showEntityRenameDialog_) {
        ImGui::OpenPopup("Rename Entity");
        showEntityRenameDialog_ = false;
        focusEntityRenameInput_ = true;
    }
    if (!ImGui::BeginPopupModal("Rename Entity", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (IsInPlayMode()) {
        renameEntity_ = {};
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    WorldEntity* entity = world_.Find(renameEntity_);
    if (entity == nullptr) {
        renameEntity_ = {};
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextDisabled("ID: %s", renameEntity_.ToString().c_str());
    if (focusEntityRenameInput_) {
        ImGui::SetKeyboardFocusHere();
        focusEntityRenameInput_ = false;
    }
    ImGui::SetNextItemWidth(320.0f);
    const bool submitted = ImGui::InputText(
        "##EntityName", renameBuffer_.data(), renameBuffer_.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (submitted || ImGui::Button("Rename", ImVec2(100.0f, 0.0f))) {
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        entity->name = renameBuffer_.data();
        if (entity->name.empty()) {
            entity->name = "Entity";
        }
        selection_ = renameEntity_;
        renameEntity_ = {};
        RecordImmediateEdit("Rename Entity", before, selectionBefore);
        status_ = "Renamed the entity.";
        ImGui::CloseCurrentPopup();
    } else {
        ImGui::SameLine();
        if (cancel || ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
            renameEntity_ = {};
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

void EditorScene::DrawDockSpace() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockspaceId = ImHashStr("LikeEngineEditorDockSpace");
    if (resetDockLayoutRequested_) {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        resetDockLayoutRequested_ = false;
    }
    if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodePos(dockspaceId, viewport->WorkPos);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

        ImGuiID mainDock = dockspaceId;
        ImGuiID leftDock{};
        ImGuiID rightDock{};
        ImGuiID projectDock{};
        ImGuiID consoleDock{};
        ImGui::DockBuilderSplitNode(mainDock, ImGuiDir_Left, 0.22f, &leftDock, &mainDock);
        ImGui::DockBuilderSplitNode(mainDock, ImGuiDir_Right, 0.31f, &rightDock, &mainDock);
        ImGui::DockBuilderSplitNode(leftDock, ImGuiDir_Down, 0.28f, &projectDock, &leftDock);
        ImGui::DockBuilderSplitNode(mainDock, ImGuiDir_Down, 0.28f, &consoleDock, &mainDock);
        ImGui::DockBuilderDockWindow("Hierarchy", leftDock);
        ImGui::DockBuilderDockWindow("Project", projectDock);
        ImGui::DockBuilderDockWindow("Game", mainDock);
        ImGui::DockBuilderDockWindow("Scene", mainDock);
        ImGui::DockBuilderDockWindow("Console", consoleDock);
        ImGui::DockBuilderDockWindow("Inspector", rightDock);
        ImGui::DockBuilderFinish(dockspaceId);
        status_ = "Initialized the default docking layout.";
    }
    ImGui::DockSpaceOverViewport(dockspaceId, viewport);
}

void EditorScene::DrawPanels() {
    sceneViewSurface_.ReleaseCompletedFrameResources();
    gameViewSurface_.ReleaseCompletedFrameResources();
    cameraPreviewSurface_.ReleaseCompletedFrameResources();
    assetPreviewSurface_.ReleaseCompletedFrameResources();
    projectPanelMinX_ = 0.0f;
    projectPanelMinY_ = 0.0f;
    projectPanelMaxX_ = 0.0f;
    projectPanelMaxY_ = 0.0f;
    Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (input != nullptr) {
        input->SetQueryEnabled(false, false, false);
    }
    if (gameInputCaptured_ &&
        (playModeState_ != PlayModeState::Playing || !showGamePanel_)) {
        ReleaseGameInputCapture();
    }
    if (showHierarchyPanel_) {
        if (ImGui::Begin("Hierarchy", &showHierarchyPanel_, kPanelFlags)) {
            DrawHierarchyPanel();
        }
        ImGui::End();
    }

    if (showProjectPanel_) {
        if (ImGui::Begin("Project", &showProjectPanel_, kPanelFlags)) {
            ImGui::BeginDisabled(IsInPlayMode());
            DrawProjectPanel();
            ImGui::EndDisabled();
        }
        ImGui::End();
    }

    if (showScenePanel_) {
        if (ImGui::Begin("Scene", &showScenePanel_, kPanelFlags)) {
            DrawSceneGizmoToolbar();
            ImGui::Separator();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            requestedSceneWidth_ = (std::max)(1, static_cast<int>(std::lround(available.x)));
            requestedSceneHeight_ = (std::max)(1, static_cast<int>(std::lround(available.y)));
            if (sceneViewSurface_.IsReady() && sceneViewPostProcess_.IsReady() && ctx_ != nullptr &&
                ctx_->rendering.dxCommon != nullptr && ctx_->rendering.model != nullptr) {
                const ImVec2 expectedImageMin = ImGui::GetCursorScreenPos();
                const ImVec2 expectedImageMax = {
                    expectedImageMin.x + static_cast<float>(requestedSceneWidth_),
                    expectedImageMin.y + static_cast<float>(requestedSceneHeight_)};
                ImVec2 expectedPreviewMin{};
                ImVec2 expectedPreviewMax{};
                const WorldEntity* previewEntity = world_.Find(selection_);
                const bool cameraPreviewHovered =
                    previewEntity != nullptr && previewEntity->camera &&
                    cameraPreviewSurface_.IsReady() && cameraPreviewPostProcess_.IsReady() &&
                    TryGetCameraPreviewRect(expectedImageMin, expectedImageMax,
                                            expectedPreviewMin, expectedPreviewMax) &&
                    ImGui::IsMouseHoveringRect(expectedPreviewMin, expectedPreviewMax);
                const bool expectedImageHovered =
                    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                    ImGui::IsMouseHoveringRect(expectedImageMin, expectedImageMax) &&
                    !cameraPreviewHovered;
                HandleSceneCameraControls(expectedImageMin, expectedImageMax,
                                          expectedImageHovered);
                BuildRenderScene();
                BuildEditorOverlayScene();
                sceneRenderer_.Render(renderScene_, sceneViewCamera_, sceneViewSurface_,
                                      {0.025f, 0.035f, 0.055f, 1.0f},
                                      &editorOverlayScene_);
                sceneViewSurface_.TransitionDepthToShaderResource();
                sceneViewSurface_.BeginOutputPass({0.0f, 0.0f, 0.0f, 1.0f});
                const PostProcessOutputTarget target{
                    sceneViewSurface_.GetOutputRtvHandle(),
                    static_cast<uint32_t>(sceneViewSurface_.GetWidth()),
                    static_cast<uint32_t>(sceneViewSurface_.GetHeight()),
                    DirectXCommon::kBackBufferFormat,
                };
                sceneViewPostProcess_.DrawToTarget(sceneViewSurface_.GetSceneColorGpuHandle(),
                                                   sceneViewSurface_.GetDepthGpuHandle(), target);
                sceneViewSurface_.EndOutputPass();
                sceneViewSurface_.TransitionDepthToWrite();
                ctx_->rendering.dxCommon->SetBackBufferRenderTarget(false, false);
                const D3D12_GPU_DESCRIPTOR_HANDLE output = sceneViewSurface_.GetOutputGpuHandle();
                ImGui::Image(static_cast<ImTextureID>(output.ptr),
                             ImVec2(static_cast<float>(requestedSceneWidth_),
                                    static_cast<float>(requestedSceneHeight_)));
                const ImVec2 imageMin = ImGui::GetItemRectMin();
                const ImVec2 imageMax = ImGui::GetItemRectMax();
                const bool imageHovered = ImGui::IsItemHovered() && !cameraPreviewHovered;
                if (!IsInPlayMode()) {
                    HandleSceneAssetDrop(imageMin, imageMax);
                    HandleSceneContextMenu(imageMin, imageMax, imageHovered);
                }
                DrawSceneComponentGizmos(imageMin, imageMax);
                DrawSceneSelectionOutline(imageMin, imageMax);
                bool sceneGizmoHovered = false;
                if (!IsInPlayMode()) {
                    if (boxColliderGizmoMode_ != BoxColliderGizmoMode::None) {
                        sceneGizmoHovered = DrawBoxColliderGizmo(imageMin, imageMax);
                    } else if (characterControllerGizmoMode_ !=
                               CharacterControllerGizmoMode::None) {
                        sceneGizmoHovered =
                            DrawCharacterControllerGizmo(imageMin, imageMax);
                    } else {
                        sceneGizmoHovered = DrawSceneTransformGizmo(imageMin, imageMax);
                    }
                }
                if (IsInPlayMode() || !sceneGizmoHovered) {
                    PickSceneEntity(imageMin, imageMax, imageHovered);
                }
                if (imageHovered) {
                    constexpr const char* cameraHint =
                        "RMB Look  |  WASD/QE Move  |  MMB Pan  |  Wheel Dolly  |  F Focus";
                    const ImVec2 hintSize = ImGui::CalcTextSize(cameraHint);
                    const ImVec2 hintMin = {imageMin.x + 8.0f,
                                            imageMax.y - hintSize.y - 12.0f};
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled({hintMin.x - 4.0f, hintMin.y - 2.0f},
                                            {hintMin.x + hintSize.x + 4.0f,
                                             hintMin.y + hintSize.y + 2.0f},
                                            IM_COL32(20, 24, 32, 190), 3.0f);
                    drawList->AddText(hintMin, IM_COL32(220, 225, 235, 230), cameraHint);
                }
                DrawSelectedCameraPreview(imageMin, imageMax);
            } else {
                ImGui::TextDisabled("Scene View RenderSurface is not ready.");
            }
        }
        ImGui::End();
    }

    if (showGamePanel_) {
        if (focusGamePanelRequested_) {
            ImGui::SetNextWindowFocus();
            focusGamePanelRequested_ = false;
        }
        ImGuiWindowFlags gameWindowFlags = kPanelFlags;
        bool* gameWindowOpen = &showGamePanel_;
        if (playerMode_) {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            gameWindowFlags |= ImGuiWindowFlags_NoDecoration |
                               ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoBringToFrontOnFocus;
            gameWindowOpen = nullptr;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        }
        const bool gameWindowVisible =
            ImGui::Begin("Game", gameWindowOpen, gameWindowFlags);
        if (playerMode_) {
            ImGui::PopStyleVar();
        }
        if (gameWindowVisible) {
            const bool gameViewFocused =
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (gameInputCaptured_ && !gameViewFocused) {
                ReleaseGameInputCapture();
                status_ = "Released Game input because the Game View lost focus.";
            }
            const ImVec2 available = ImGui::GetContentRegionAvail();
            requestedGameWidth_ = (std::max)(1, static_cast<int>(std::lround(available.x)));
            requestedGameHeight_ = (std::max)(1, static_cast<int>(std::lround(available.y)));
            if (!gameViewSurface_.IsReady() || !gameViewPostProcess_.IsReady() ||
                ctx_ == nullptr || ctx_->rendering.dxCommon == nullptr ||
                ctx_->rendering.model == nullptr) {
                ReleaseGameInputCapture();
                ImGui::TextDisabled("Game View RenderSurface is not ready.");
            } else {
                const bool hasGameCamera = UpdateGameViewCamera();
                if (hasGameCamera) {
                    BuildRenderScene();
                } else {
                    renderScene_.BeginFrame();
                    ReleaseGameInputCapture();
                }
                sceneRenderer_.Render(renderScene_, gameViewCamera_, gameViewSurface_,
                                      {0.025f, 0.035f, 0.055f, 1.0f});
                gameViewSurface_.TransitionDepthToShaderResource();
                gameViewSurface_.BeginOutputPass({0.0f, 0.0f, 0.0f, 1.0f});
                const PostProcessOutputTarget target{
                    gameViewSurface_.GetOutputRtvHandle(),
                    static_cast<uint32_t>(gameViewSurface_.GetWidth()),
                    static_cast<uint32_t>(gameViewSurface_.GetHeight()),
                    DirectXCommon::kBackBufferFormat,
                };
                gameViewPostProcess_.DrawToTarget(gameViewSurface_.GetSceneColorGpuHandle(),
                                                  gameViewSurface_.GetDepthGpuHandle(), target);
                const bool gameUiHovered =
                    DrawGameUi(gameViewSurface_.GetWidth(),
                               gameViewSurface_.GetHeight(),
                               hasGameCamera);
                gameViewSurface_.EndOutputPass();
                gameViewSurface_.TransitionDepthToWrite();
                ctx_->rendering.dxCommon->SetBackBufferRenderTarget(false, false);
                const D3D12_GPU_DESCRIPTOR_HANDLE output =
                    gameViewSurface_.GetOutputGpuHandle();
                ImGui::Image(static_cast<ImTextureID>(output.ptr),
                             ImVec2(static_cast<float>(requestedGameWidth_),
                                    static_cast<float>(requestedGameHeight_)));
                const ImVec2 gameImageMin = ImGui::GetItemRectMin();
                const ImVec2 gameImageMax = ImGui::GetItemRectMax();
                const bool gameImageHovered = ImGui::IsItemHovered();
                if (playModeState_ == PlayModeState::Edit) {
                    HandleGameUiEditing(gameImageMin, gameImageMax);
                }
                if (hasGameCamera &&
                    playModeState_ == PlayModeState::Playing && gameImageHovered &&
                    !gameUiHovered &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                    !gameInputCaptured_) {
                    POINT cursor{};
                    if (GetCursorPos(&cursor)) {
                        gameInputCursorRestoreX_ = cursor.x;
                        gameInputCursorRestoreY_ = cursor.y;
                    }
                    focusedButton_ = {};
                    pressedButton_ = {};
                    activeSlider_ = {};
                    openDropdown_ = {};
                    activeInputField_ = {};
                    gameInputCaptured_ = true;
                    status_ = "Game input captured. Press Escape to release.";
                }
                if (gameInputCaptured_) {
                    const int cursorCenterX = static_cast<int>(
                        std::lround((gameImageMin.x + gameImageMax.x) * 0.5f));
                    const int cursorCenterY = static_cast<int>(
                        std::lround((gameImageMin.y + gameImageMax.y) * 0.5f));
                    SetCursorPos(cursorCenterX, cursorCenterY);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                }
                if (input != nullptr && playModeState_ == PlayModeState::Playing) {
                    input->SetQueryEnabled(gameViewFocused,
                                           gameViewFocused &&
                                               (gameImageHovered || gameInputCaptured_),
                                           gameViewFocused);
                }
                if (!hasGameCamera && !playerMode_) {
                    constexpr const char* kNoCameraHint =
                        "No Primary Camera - displaying Runtime UI only";
                    const ImVec2 hintSize = ImGui::CalcTextSize(kNoCameraHint);
                    const ImVec2 hintMin{gameImageMin.x + 10.0f,
                                         gameImageMin.y + 10.0f};
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled(
                        {hintMin.x - 4.0f, hintMin.y - 2.0f},
                        {hintMin.x + hintSize.x + 4.0f,
                         hintMin.y + hintSize.y + 2.0f},
                        IM_COL32(20, 24, 32, 190), 3.0f);
                    drawList->AddText(hintMin,
                                      IM_COL32(255, 196, 90, 240),
                                      kNoCameraHint);
                } else if (playModeState_ == PlayModeState::Playing) {
                    const char* captureHint = gameInputCaptured_
                                                  ? "Input captured - Esc to release"
                                                  : "Click Game View to capture input";
                    const ImVec2 hintSize = ImGui::CalcTextSize(captureHint);
                    const ImVec2 hintMin{gameImageMin.x + 10.0f, gameImageMin.y + 10.0f};
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled({hintMin.x - 4.0f, hintMin.y - 2.0f},
                                            {hintMin.x + hintSize.x + 4.0f,
                                             hintMin.y + hintSize.y + 2.0f},
                                            IM_COL32(20, 24, 32, 190), 3.0f);
                    drawList->AddText(hintMin, IM_COL32(220, 225, 235, 230),
                                      captureHint);
                }
            }
        }
        ImGui::End();
    }

    if (showConsolePanel_) {
        if (ImGui::Begin("Console", &showConsolePanel_, kPanelFlags)) {
            DrawConsolePanel();
        }
        ImGui::End();
    }

    if (showInspectorPanel_) {
        if (ImGui::Begin("Inspector", &showInspectorPanel_, kPanelFlags)) {
            ImGui::BeginDisabled(IsInPlayMode());
            DrawInspectorPanel();
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
}

bool EditorScene::SavePhysicsSettings() {
    std::string error;
    if (!physicsSettingsStore_.Save(physicsSettings_, error)) {
        status_ = "Error: Could not save Physics Settings: " + error;
        return false;
    }
    world_.SetPhysicsSettings(physicsSettings_);
    physicsSettingsDirty_ = false;
    status_ = "Saved Physics Settings.";
    return true;
}

bool EditorScene::SavePlayerSettings() {
    std::string error;
    if (!playerSettingsStore_.Save(playerSettings_, error)) {
        status_ = "Error: Could not save Player Settings: " + error;
        return false;
    }
    playerSettingsDirty_ = false;
    status_ = "Saved Player Settings.";
    return true;
}

bool EditorScene::SaveInputSettings() {
    Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (input == nullptr) {
        status_ = "Error: Could not save Input Settings: Input service is unavailable.";
        return false;
    }
    std::string error;
    if (!inputSettingsStore_.Save(*input, error)) {
        status_ = "Error: Could not save Input Settings: " + error;
        return false;
    }
    inputSettingsDirty_ = false;
    status_ = "Saved Input Settings.";
    return true;
}

size_t EditorScene::UpgradeInputActionReferences() {
    Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (input == nullptr || IsInPlayMode()) {
        return 0u;
    }
    size_t upgraded = 0u;
    for (const WorldEntity& snapshot : world_.Entities()) {
        WorldEntity* entity = world_.Find(snapshot.id);
        if (entity == nullptr) {
            continue;
        }
        for (BehaviorComponent& script : entity->scripts) {
            for (ScriptPropertyValue& property : script.properties) {
                if (property.type != ScriptPropertyType::InputAction) {
                    continue;
                }
                const std::string id = input->GetActionId(property.stringValue);
                if (!id.empty() && property.stringValue != id) {
                    property.stringValue = id;
                    ++upgraded;
                }
            }
        }
    }
    if (upgraded != 0u) {
        RefreshDirty();
    }
    return upgraded;
}

void EditorScene::DrawProjectSettingsWindow() {
    if (!showProjectSettings_) {
        return;
    }
    ImGui::SetNextWindowSize({760.0f, 620.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project Settings", &showProjectSettings_)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("General");
    std::error_code startupSceneError;
    std::filesystem::path startupSceneLabel =
        std::filesystem::relative(startupScenePath_, sceneRoot_,
                                  startupSceneError);
    if (startupSceneError) {
        startupSceneLabel = startupScenePath_.filename();
    }
    ImGui::Text("Startup Scene");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", startupSceneLabel.generic_string().c_str());
    std::error_code currentSceneError;
    const bool currentSceneCanStart =
        !scenePath_.empty() &&
        std::filesystem::is_regular_file(scenePath_, currentSceneError) &&
        !currentSceneError && scenePath_ != startupScenePath_;
    ImGui::BeginDisabled(IsInPlayMode() || !currentSceneCanStart);
    if (ImGui::Button("Set Current Scene as Startup")) {
        ProjectDescriptor project;
        std::string error;
        if (ProjectDescriptor::SetStartupScene(projectRoot_, scenePath_,
                                               project, error)) {
            startupScenePath_ = project.startupScene;
            std::error_code labelError;
            std::filesystem::path label = std::filesystem::relative(
                startupScenePath_, sceneRoot_, labelError);
            if (labelError) {
                label = startupScenePath_.filename();
            }
            status_ = "Set Startup Scene: " + label.generic_string();
        } else {
            status_ = "Error: Could not set Startup Scene: " + error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(
        "The Player starts from this saved scene.");

    ImGui::SeparatorText("Player");
    ImGui::TextDisabled("Project file: %s",
                        playerSettingsStore_.Path().generic_string().c_str());
    ImGui::TextWrapped(
        "These settings apply the next time Player Preview or a packaged Player starts.");
    ImGui::BeginDisabled(IsInPlayMode());
    ImGui::BeginDisabled(!playerSettingsDirty_);
    if (ImGui::Button("Save##PlayerSettings")) {
        SavePlayerSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert##PlayerSettings")) {
        PlayerSettings restored{};
        std::string error;
        if (playerSettingsStore_.Load(restored, error)) {
            playerSettings_ = restored;
            playerSettingsDirty_ = false;
            status_ = "Reverted Player Settings.";
        } else {
            status_ = "Error: Could not reload Player Settings: " + error;
        }
    }
    ImGui::EndDisabled();
    if (playerSettingsDirty_) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.75f, 0.25f, 1.0f}, "Unsaved changes");
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputInt("Width", &playerSettings_.width)) {
        playerSettings_.width =
            std::clamp(playerSettings_.width, 320, 16384);
        playerSettingsDirty_ = true;
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputInt("Height", &playerSettings_.height)) {
        playerSettings_.height =
            std::clamp(playerSettings_.height, 180, 16384);
        playerSettingsDirty_ = true;
    }
    if (ImGui::Checkbox("Borderless Fullscreen",
                        &playerSettings_.fullscreen)) {
        playerSettingsDirty_ = true;
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Physics");
    ImGui::TextDisabled("Project file: %s",
                        physicsSettingsStore_.Path().generic_string().c_str());
    ImGui::TextWrapped("Define project Layers and which Layer pairs are allowed to collide. "
                       "The matrix filters Character Controller blocking and Trigger events.");
    ImGui::BeginDisabled(IsInPlayMode());
    ImGui::BeginDisabled(!physicsSettingsDirty_);
    if (ImGui::Button("Save")) {
        SavePhysicsSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        PhysicsSettings restored{};
        std::string error;
        if (physicsSettingsStore_.Load(restored, error)) {
            physicsSettings_ = std::move(restored);
            world_.SetPhysicsSettings(physicsSettings_);
            physicsSettingsDirty_ = false;
            status_ = "Reverted Physics Settings.";
        } else {
            status_ = "Error: Could not reload Physics Settings: " + error;
        }
    }
    ImGui::EndDisabled();
    if (physicsSettingsDirty_) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.75f, 0.25f, 1.0f}, "Unsaved changes");
    }

    ImGui::SeparatorText("Layers");
    ImGui::TextDisabled("Layer 0 is reserved as Default. Empty Layer names are unused.");
    if (ImGui::BeginChild("PhysicsLayers", {0.0f, 220.0f}, ImGuiChildFlags_Borders)) {
        for (size_t index = 0u; index < PhysicsSettings::kLayerCount; ++index) {
            ImGui::PushID(static_cast<int>(index));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%2zu", index);
            ImGui::SameLine();
            std::array<char, 65> buffer{};
            strncpy_s(buffer.data(), buffer.size(),
                      physicsSettings_.layerNames[index].c_str(), _TRUNCATE);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::BeginDisabled(index == 0u);
            if (ImGui::InputText("##LayerName", buffer.data(), buffer.size())) {
                physicsSettings_.layerNames[index] = buffer.data();
                physicsSettingsDirty_ = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    std::vector<size_t> definedLayers;
    for (size_t index = 0u; index < PhysicsSettings::kLayerCount; ++index) {
        if (!physicsSettings_.layerNames[index].empty()) {
            definedLayers.push_back(index);
        }
    }
    ImGui::SeparatorText("Layer Collision Matrix");
    ImGui::TextDisabled("A checked pair will be allowed to collide.");
    std::vector<std::string> columnLabels;
    columnLabels.reserve(definedLayers.size());
    for (size_t layer : definedLayers) {
        columnLabels.push_back(std::to_string(layer));
    }
    constexpr ImGuiTableFlags matrixFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("PhysicsCollisionMatrix",
                          static_cast<int>(definedLayers.size() + 1u), matrixFlags,
                          {0.0f, 250.0f})) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        for (const std::string& label : columnLabels) {
            ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthFixed, 30.0f);
        }
        ImGui::TableHeadersRow();
        for (size_t row = 0u; row < definedLayers.size(); ++row) {
            const size_t first = definedLayers[row];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%zu: %s", first, physicsSettings_.layerNames[first].c_str());
            for (size_t column = 0u; column < definedLayers.size(); ++column) {
                ImGui::TableSetColumnIndex(static_cast<int>(column + 1u));
                if (column > row) {
                    continue;
                }
                const size_t second = definedLayers[column];
                bool collide = physicsSettings_.LayersCollide(first, second);
                ImGui::PushID(static_cast<int>(first));
                ImGui::PushID(static_cast<int>(second));
                if (ImGui::Checkbox("##Collide", &collide)) {
                    physicsSettings_.SetLayersCollide(first, second, collide);
                    world_.SetPhysicsSettings(physicsSettings_);
                    physicsSettingsDirty_ = true;
                }
                ImGui::PopID();
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Input Actions");
    ImGui::TextDisabled("Project file: %s",
                        inputSettingsStore_.Path().generic_string().c_str());
    ImGui::TextWrapped(
        "Named Actions combine keyboard and gamepad bindings used by C++ Scripts.");
    Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    ImGui::BeginDisabled(IsInPlayMode() || input == nullptr);
    if (input == nullptr) {
        ImGui::TextDisabled("Input service is unavailable.");
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(!inputSettingsDirty_);
    if (ImGui::Button("Save##InputSettings")) {
        SaveInputSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert##InputSettings")) {
        std::string error;
        if (inputSettingsStore_.Load(*input, error)) {
            inputSettingsDirty_ = false;
            status_ = "Reverted Input Settings.";
        } else {
            status_ = "Error: Could not reload Input Settings: " + error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset Defaults##InputSettings")) {
        input->ResetDefaultActionBindings();
        inputSettingsDirty_ = true;
        status_ = "Reset Input Actions to defaults.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Action")) {
        inputActionNameBuffer_.fill('\0');
        newInputActionType_ = InputActionType::Button;
        showCreateInputActionDialog_ = true;
        focusInputActionNameInput_ = true;
    }
    if (inputSettingsDirty_) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.75f, 0.25f, 1.0f}, "Unsaved changes");
    }

    const auto drawKeyCombo = [](const char* label, int& value) {
        const auto selected = std::ranges::find(kInputKeyChoices, value,
                                                &InputKeyChoice::value);
        const char* preview =
            selected != kInputKeyChoices.end() ? selected->label : "Unknown";
        bool changed = false;
        if (ImGui::BeginCombo(label, preview)) {
            for (const InputKeyChoice& choice : kInputKeyChoices) {
                if (ImGui::Selectable(choice.label, value == choice.value)) {
                    value = choice.value;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    };
    const auto drawGamepadButtonCombo = [](const char* label, WORD& value) {
        const auto selected =
            std::ranges::find(kInputGamepadButtonChoices, value,
                              &InputGamepadButtonChoice::value);
        const char* preview = selected != kInputGamepadButtonChoices.end()
                                  ? selected->label
                                  : "Unknown";
        bool changed = false;
        if (ImGui::BeginCombo(label, preview)) {
            for (const InputGamepadButtonChoice& choice :
                 kInputGamepadButtonChoices) {
                if (ImGui::Selectable(choice.label, value == choice.value)) {
                    value = choice.value;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    };
    const auto drawAxisCombo = [](const char* label,
                                  InputActionAxisSource& value) {
        const auto selected =
            std::ranges::find(kInputAxisChoices, value, &InputAxisChoice::value);
        const char* preview =
            selected != kInputAxisChoices.end() ? selected->label : "Unknown";
        bool changed = false;
        if (ImGui::BeginCombo(label, preview)) {
            for (const InputAxisChoice& choice : kInputAxisChoices) {
                if (ImGui::Selectable(choice.label, value == choice.value)) {
                    value = choice.value;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    };

    const std::unordered_map<std::string, InputActionUsage> inputActionUsages =
        CollectInputActionUsages(world_, behaviorRegistry_, *input);
    if (ImGui::BeginChild("InputActionBindings", {0.0f, 300.0f},
                          ImGuiChildFlags_Borders)) {
        for (const std::string& name : input->GetActionNames()) {
            const InputActionBinding* stored = input->GetActionBinding(name);
            if (stored == nullptr) {
                continue;
            }
            InputActionBinding binding = *stored;
            ImGui::PushID(name.c_str());
            ImGui::SeparatorText(name.c_str());
            ImGui::TextDisabled("ID: %s", input->GetActionId(name).c_str());
            bool changed = false;
            const char* typePreview =
                binding.type == InputActionType::Axis ? "Axis" : "Button";
            if (ImGui::BeginCombo("Type", typePreview)) {
                if (ImGui::Selectable("Button",
                                      binding.type == InputActionType::Button)) {
                    binding.type = InputActionType::Button;
                    changed = true;
                }
                if (ImGui::Selectable("Axis",
                                      binding.type == InputActionType::Axis)) {
                    binding.type = InputActionType::Axis;
                    changed = true;
                }
                ImGui::EndCombo();
            }
            const auto usageEntry = inputActionUsages.find(name);
            const InputActionUsage usage =
                usageEntry != inputActionUsages.end() ? usageEntry->second
                                                      : InputActionUsage{};
            if (usage.total == 0u) {
                ImGui::TextDisabled("No Script references.");
            } else {
                ImGui::TextDisabled(
                    "%zu Script reference%s (Stable: %zu, Legacy: %zu)",
                    usage.total, usage.total == 1u ? "" : "s", usage.stable,
                    usage.legacy);
                ImGui::TextDisabled("Expected kind: Button %zu, Axis %zu, Any %zu",
                                    usage.button, usage.axis, usage.any);
            }
            const size_t incompatibleReferences =
                binding.type == InputActionType::Button ? usage.axis : usage.button;
            if (incompatibleReferences != 0u) {
                ImGui::TextColored(
                    {1.0f, 0.45f, 0.35f, 1.0f},
                    "%zu Script reference%s expect%s the other Action type.",
                    incompatibleReferences,
                    incompatibleReferences == 1u ? "" : "s",
                    incompatibleReferences == 1u ? "s" : "");
            }
            const bool axisAction = binding.type == InputActionType::Axis;
            if (axisAction) {
                changed |= drawKeyCombo("Negative Key", binding.negativeKey);
                changed |= drawKeyCombo("Positive Key", binding.positiveKeys[0]);
                changed |= drawAxisCombo("Gamepad Axis", binding.gamepadAxis);
            } else {
                changed |= drawKeyCombo("Primary Key", binding.positiveKeys[0]);
                changed |= drawKeyCombo("Alternate Key", binding.positiveKeys[1]);
                changed |= drawGamepadButtonCombo("Gamepad Button",
                                                  binding.gamepadButton);
            }
            if (changed && input->SetActionBinding(name, binding)) {
                inputSettingsDirty_ = true;
                status_ = "Modified Input Action: " + name;
            }
            if (ImGui::SmallButton("Rename")) {
                pendingInputActionName_ = name;
                inputActionNameBuffer_.fill('\0');
                strncpy_s(inputActionNameBuffer_.data(),
                          inputActionNameBuffer_.size(), name.c_str(), _TRUNCATE);
                showRenameInputActionDialog_ = true;
                focusInputActionNameInput_ = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                pendingInputActionName_ = name;
                showDeleteInputActionDialog_ = true;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (showCreateInputActionDialog_) {
        ImGui::OpenPopup("Create Input Action");
        showCreateInputActionDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Create Input Action", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (focusInputActionNameInput_) {
            ImGui::SetKeyboardFocusHere();
            focusInputActionNameInput_ = false;
        }
        ImGui::SetNextItemWidth(300.0f);
        const bool submitted = ImGui::InputText(
            "Name", inputActionNameBuffer_.data(), inputActionNameBuffer_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        const char* newTypePreview =
            newInputActionType_ == InputActionType::Axis ? "Axis" : "Button";
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("Type", newTypePreview)) {
            if (ImGui::Selectable(
                    "Button", newInputActionType_ == InputActionType::Button)) {
                newInputActionType_ = InputActionType::Button;
            }
            if (ImGui::Selectable(
                    "Axis", newInputActionType_ == InputActionType::Axis)) {
                newInputActionType_ = InputActionType::Axis;
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Names must be unique and at most 64 characters.");
        if (submitted || ImGui::Button("Create", {100.0f, 0.0f})) {
            InputActionBinding binding{};
            binding.type = newInputActionType_;
            const std::string name(inputActionNameBuffer_.data());
            if (input->GetActionBinding(name) != nullptr) {
                status_ = "Error: Input Action already exists: " + name;
            } else if (input->SetActionBinding(name, binding)) {
                inputSettingsDirty_ = true;
                status_ = "Created Input Action: " + name;
                ImGui::CloseCurrentPopup();
            } else {
                status_ = "Error: Invalid Input Action name.";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100.0f, 0.0f})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (showRenameInputActionDialog_) {
        ImGui::OpenPopup("Rename Input Action");
        showRenameInputActionDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Rename Input Action", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Current name: %s",
                            pendingInputActionName_.c_str());
        const auto usageEntry =
            inputActionUsages.find(pendingInputActionName_);
        const InputActionUsage usage =
            usageEntry != inputActionUsages.end() ? usageEntry->second
                                                  : InputActionUsage{};
        if (usage.legacy != 0u) {
            ImGui::TextColored(
                {1.0f, 0.75f, 0.25f, 1.0f},
                "%zu Script reference%s will keep the old name.",
                usage.legacy, usage.legacy == 1u ? "" : "s");
            ImGui::TextWrapped(
                "Update those Script properties before or after renaming.");
        }
        if (usage.stable != 0u) {
            ImGui::TextDisabled("%zu stable reference%s will remain connected.",
                                usage.stable,
                                usage.stable == 1u ? "" : "s");
        }
        if (focusInputActionNameInput_) {
            ImGui::SetKeyboardFocusHere();
            focusInputActionNameInput_ = false;
        }
        ImGui::SetNextItemWidth(300.0f);
        const bool submitted = ImGui::InputText(
            "New Name", inputActionNameBuffer_.data(),
            inputActionNameBuffer_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        if (submitted || ImGui::Button("Rename", {100.0f, 0.0f})) {
            const std::string newName(inputActionNameBuffer_.data());
            if (input->RenameActionBinding(pendingInputActionName_, newName)) {
                status_ = "Renamed Input Action: " + pendingInputActionName_ +
                          " -> " + newName;
                pendingInputActionName_.clear();
                inputSettingsDirty_ = true;
                ImGui::CloseCurrentPopup();
            } else {
                status_ =
                    "Error: Input Action name is invalid or already exists.";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100.0f, 0.0f})) {
            pendingInputActionName_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (showDeleteInputActionDialog_) {
        ImGui::OpenPopup("Remove Input Action");
        showDeleteInputActionDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Remove Input Action", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove '%s'?", pendingInputActionName_.c_str());
        const auto usageEntry =
            inputActionUsages.find(pendingInputActionName_);
        const InputActionUsage usage =
            usageEntry != inputActionUsages.end() ? usageEntry->second
                                                  : InputActionUsage{};
        if (usage.total != 0u) {
            ImGui::TextColored(
                {1.0f, 0.45f, 0.35f, 1.0f},
                "%zu Script reference%s will become missing.",
                usage.total, usage.total == 1u ? "" : "s");
        }
        ImGui::TextDisabled("The change is not permanent until Save is pressed.");
        if (ImGui::Button("Remove", {100.0f, 0.0f})) {
            const std::string removedName = pendingInputActionName_;
            if (input->RemoveActionBinding(removedName)) {
                inputSettingsDirty_ = true;
                status_ = "Removed Input Action: " + removedName;
            }
            pendingInputActionName_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100.0f, 0.0f})) {
            pendingInputActionName_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    ImGui::End();
}

void EditorScene::AssignModelAsset(EntityId entityId, const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeModelAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const std::string previousPath =
        entity->meshRenderer ? entity->meshRenderer->modelPath : std::string{};
    if (!entity->meshRenderer) {
        entity->meshRenderer = MeshRendererComponent{};
        if (!entity->materialOverride) {
            entity->materialOverride = MaterialOverrideComponent{};
        }
    }
    entity->meshRenderer->sourceType = MeshSourceType::Model;
    entity->meshRenderer->modelPath = assetPath;
    loadedModels_.erase(previousPath);
    loadedModels_.erase(assetPath);
    animatorModels_.clear();
    selection_ = entityId;
    RecordImmediateEdit("Assign Model Asset", before, selectionBefore);
    status_ = "Assigned model asset: " + assetPath;
}

void EditorScene::AssignAudioAsset(EntityId entityId, const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeAudioAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (!entity->audioSource) {
        entity->audioSource = AudioSourceComponent{};
    }
    entity->audioSource->clipPath = assetPath;
    selection_ = entityId;
    RecordImmediateEdit("Assign Audio Asset", before, selectionBefore);
    status_ = "Assigned audio asset: " + assetPath;
}

void EditorScene::DrawAudioAssetPreview(const std::filesystem::path& physicalPath) {
    const std::filesystem::path selected = selectedAsset_.lexically_normal();
    if (assetPreviewAsset_ != selected) {
        StopAudioAssetPreview();
        audioPreviewSoundId_ = ISoundService::kInvalidSoundId;
        assetPreviewAsset_ = selected;
        assetPreviewModel_ = {};
        assetPreviewPlan_.clear();
        assetPreviewError_.clear();
    }
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    const bool playing = sound != nullptr &&
                         audioPreviewVoice_ != ISoundService::kInvalidVoiceHandle &&
                         sound->IsPlaying(audioPreviewVoice_);
    ImGui::BeginDisabled(sound == nullptr);
    if (ImGui::SmallButton(playing ? "Restart Preview" : "Play Preview")) {
        StopAudioAssetPreview();
        uint32_t soundId = ISoundService::kInvalidSoundId;
        if (!sound->TryLoad(physicalPath.wstring(), soundId)) {
            status_ = "Audio preview failed: the file could not be decoded.";
        } else {
            audioPreviewSoundId_ = soundId;
            audioPreviewVoice_ = sound->Play(soundId);
            status_ = audioPreviewVoice_ != ISoundService::kInvalidVoiceHandle
                          ? "Playing audio preview: " + physicalPath.filename().string()
                          : "Audio preview failed: the audio device is unavailable.";
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (ImGui::SmallButton("Stop Preview")) {
        StopAudioAssetPreview();
        status_ = "Stopped audio preview.";
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (sound != nullptr && audioPreviewSoundId_ != ISoundService::kInvalidSoundId) {
        if (const ISoundService::SoundInfo* info = sound->GetInfo(audioPreviewSoundId_)) {
            ImGui::TextDisabled("Duration: %.2f s   Channels: %u   Sample Rate: %u Hz",
                                info->durationSeconds, static_cast<unsigned>(info->channels),
                                info->sampleRate);
        }
    }
}

void EditorScene::StopAudioAssetPreview() {
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    if (sound != nullptr && audioPreviewVoice_ != ISoundService::kInvalidVoiceHandle) {
        sound->Stop(audioPreviewVoice_);
    }
    audioPreviewVoice_ = ISoundService::kInvalidVoiceHandle;
}

void EditorScene::AssignScriptAsset(EntityId entityId, const std::filesystem::path& path,
                                    std::optional<size_t> scriptIndex) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    std::filesystem::path physicalPath;
    if (!TryNormalizeScriptAssetReference(path, assetPath, physicalPath)) {
        return;
    }
    const std::string_view scriptType =
        behaviorRegistry_.TypeFromSourceAsset(assetPath);
    if (scriptType.empty()) {
        status_ = "C++ Script source is not registered by the Project Script module: " +
                  assetPath;
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    BehaviorComponent component{};
    component.type = scriptType;
    component.scriptAssetPath = assetPath;
    if (scriptIndex) {
        if (*scriptIndex >= entity->scripts.size()) {
            status_ = "The target Script component no longer exists.";
            return;
        }
        component.enabled = entity->scripts[*scriptIndex].enabled;
        entity->scripts[*scriptIndex] = std::move(component);
    } else {
        entity->scripts.push_back(std::move(component));
    }
    (void)behaviorRegistry_.EnsureRequirements(scriptType, *entity);
    selection_ = entityId;
    RecordImmediateEdit(scriptIndex ? "Replace Script" : "Add Script", before,
                        selectionBefore);
    status_ = std::string(scriptIndex ? "Replaced" : "Added") +
              " Script component: " + assetPath;
}

void EditorScene::ClearScriptAsset(EntityId entityId, size_t scriptIndex) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr || scriptIndex >= entity->scripts.size()) {
        status_ = "The target Script component no longer exists.";
        return;
    }
    BehaviorComponent& component = entity->scripts[scriptIndex];
    if (component.type.empty() && component.scriptAssetPath.empty()) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const bool enabled = component.enabled;
    component = BehaviorComponent{};
    component.enabled = enabled;
    RecordImmediateEdit("Clear Script", before, selectionBefore);
    status_ = "Cleared Script component assignment.";
}

void EditorScene::AssignBaseColorTexture(EntityId entityId,
                                         const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeTextureAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const std::string previousPath = entity->materialOverride
                                         ? entity->materialOverride->baseColorTexturePath
                                         : std::string{};
    if (!entity->materialOverride) {
        entity->materialOverride = MaterialOverrideComponent{};
    }
    entity->materialOverride->baseColorTexturePath = assetPath;
    loadedTextures_.erase(previousPath);
    loadedTextures_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Base Color Texture", before, selectionBefore);
    status_ = "Assigned Base Color texture: " + assetPath;
}

void EditorScene::AssignImageTexture(EntityId entityId,
                                     const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr || !entity->image) {
        status_ = "The target Image component no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeTextureAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const std::string previousPath = entity->image->texturePath;
    entity->image->texturePath = assetPath;
    loadedTextures_.erase(previousPath);
    loadedTextures_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Image Texture", before, selectionBefore);
    status_ = "Assigned Image texture: " + assetPath;
}

void EditorScene::AssignTextFont(EntityId entityId,
                                 const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr || !entity->text) {
        status_ = "The target Text component no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeFontAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    entity->text->fontPath = assetPath;
    loadedFonts_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Text Font", before, selectionBefore);
    status_ = "Assigned Text font: " + assetPath;
}

void EditorScene::AssignNormalTexture(EntityId entityId,
                                      const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeTextureAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const std::string previousPath = entity->materialOverride
                                         ? entity->materialOverride->normalTexturePath
                                         : std::string{};
    if (!entity->materialOverride) {
        entity->materialOverride = MaterialOverrideComponent{};
    }
    entity->materialOverride->normalTexturePath = assetPath;
    loadedLinearTextures_.erase(previousPath);
    loadedLinearTextures_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Normal Texture", before, selectionBefore);
    status_ = "Assigned Normal texture: " + assetPath;
}

void EditorScene::AssignRoughnessTexture(EntityId entityId,
                                         const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeTextureAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (!entity->materialOverride) {
        entity->materialOverride = MaterialOverrideComponent{};
    }
    entity->materialOverride->roughnessTexturePath = assetPath;
    loadedLinearTextures_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Roughness Texture", before, selectionBefore);
    status_ = "Assigned Roughness texture: " + assetPath;
}

void EditorScene::AssignMetallicTexture(EntityId entityId,
                                        const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeTextureAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (!entity->materialOverride) {
        entity->materialOverride = MaterialOverrideComponent{};
    }
    entity->materialOverride->metallicTexturePath = assetPath;
    loadedLinearTextures_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Metallic Texture", before, selectionBefore);
    status_ = "Assigned Metallic texture: " + assetPath;
}

bool EditorScene::TryNormalizeModelAssetReference(const std::filesystem::path& path,
                                                  std::string& assetPath) {
    if (!AssetImport::IsModelFile(path)) {
        status_ = "The dropped model asset is invalid.";
        return false;
    }
    const std::optional<std::filesystem::path> resolvedPath = ResolveProjectAssetPath(path);
    std::error_code error;
    if (!resolvedPath || !std::filesystem::is_regular_file(*resolvedPath, error) || error) {
        status_ = "The dropped model asset no longer exists.";
        return false;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    assetPath = normalized.generic_string();
    if (normalized.begin() != normalized.end() && *normalized.begin() == "assets") {
        assetPath = "asset://" + normalized.lexically_relative("assets").generic_string();
    }
    if (assetPath.size() > 1024u) {
        status_ = "The dropped model asset path is too long.";
        return false;
    }
    return true;
}

bool EditorScene::TryNormalizeTextureAssetReference(const std::filesystem::path& path,
                                                    std::string& assetPath) {
    if (!AssetImport::IsTextureFile(path)) {
        status_ = "The dropped texture asset is invalid.";
        return false;
    }
    const std::optional<std::filesystem::path> resolvedPath = ResolveProjectAssetPath(path);
    std::error_code error;
    if (!resolvedPath || !std::filesystem::is_regular_file(*resolvedPath, error) || error) {
        status_ = "The dropped texture asset no longer exists.";
        return false;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    assetPath = normalized.generic_string();
    if (normalized.begin() != normalized.end() && *normalized.begin() == "assets") {
        assetPath = "asset://" + normalized.lexically_relative("assets").generic_string();
    }
    if (assetPath.size() > 1024u) {
        status_ = "The dropped texture asset path is too long.";
        return false;
    }
    return true;
}

void EditorScene::HandleSceneAssetDrop(const ImVec2& imageMin, const ImVec2& imageMax) {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kModelAssetDragPayload);
        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
        const DirectX::XMFLOAT3 position = CalculateScenePlacementPosition(
            sceneViewCamera_, imageMin, imageMax, ImGui::GetMousePos());
        CreateModelEntityFromAsset(static_cast<const char*>(payload->Data), position);
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPrefabAssetDragPayload);
        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
        const DirectX::XMFLOAT3 position = CalculateScenePlacementPosition(
            sceneViewCamera_, imageMin, imageMax, ImGui::GetMousePos());
        InstantiatePrefabAsset(static_cast<const char*>(payload->Data), {}, position);
    }
    ImGui::EndDragDropTarget();
}

void EditorScene::HandleSceneCameraControls(const ImVec2& imageMin,
                                            const ImVec2& imageMax,
                                            bool imageHovered) {
    ImGuiIO& io = ImGui::GetIO();
    if (imageHovered && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        FocusSceneCameraOnSelection();
    }
    const bool beginLook =
        imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const bool beginPan =
        imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    if (beginLook) {
        sceneCameraNavigating_ = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        sceneCameraNavigating_ = false;
    }
    if (beginPan) {
        sceneCameraPanning_ = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        sceneCameraPanning_ = false;
    }

    const int cursorCenterX =
        static_cast<int>(std::lround((imageMin.x + imageMax.x) * 0.5f));
    const int cursorCenterY =
        static_cast<int>(std::lround((imageMin.y + imageMax.y) * 0.5f));
    const bool beginCapture = (beginLook || beginPan) && !sceneCameraCursorCaptured_;
    if (beginCapture) {
        POINT cursor{};
        if (GetCursorPos(&cursor)) {
            sceneCameraCursorRestoreX_ = cursor.x;
            sceneCameraCursorRestoreY_ = cursor.y;
        }
        sceneCameraPointerTravel_ = 0.0f;
        sceneCameraCursorCaptured_ = true;
        SetCursorPos(cursorCenterX, cursorCenterY);
    }

    float pointerDeltaX = 0.0f;
    float pointerDeltaY = 0.0f;
    if (sceneCameraCursorCaptured_ && !beginCapture &&
        (sceneCameraNavigating_ || sceneCameraPanning_)) {
        POINT cursor{};
        if (GetCursorPos(&cursor)) {
            pointerDeltaX = static_cast<float>(cursor.x - cursorCenterX);
            pointerDeltaY = static_cast<float>(cursor.y - cursorCenterY);
            sceneCameraPointerTravel_ +=
                std::sqrt(pointerDeltaX * pointerDeltaX + pointerDeltaY * pointerDeltaY);
        }
        SetCursorPos(cursorCenterX, cursorCenterY);
    }
    if (sceneCameraCursorCaptured_ && !sceneCameraNavigating_ && !sceneCameraPanning_) {
        SetCursorPos(sceneCameraCursorRestoreX_, sceneCameraCursorRestoreY_);
        sceneCameraCursorCaptured_ = false;
    }

    DirectX::XMFLOAT3 rotation = sceneViewCamera_.GetRotation();
    if (sceneCameraNavigating_) {
        constexpr float mouseSensitivity = 0.004f;
        rotation.x = std::clamp(rotation.x + pointerDeltaY * mouseSensitivity,
                                -DirectX::XM_PIDIV2 + 0.01f,
                                DirectX::XM_PIDIV2 - 0.01f);
        rotation.y += pointerDeltaX * mouseSensitivity;
        if (pointerDeltaX != 0.0f || pointerDeltaY != 0.0f) {
            sceneViewCamera_.SetRotation(rotation);
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    const DirectX::XMMATRIX orientation =
        DirectX::XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, 0.0f);
    const DirectX::XMVECTOR right = DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), orientation);
    const DirectX::XMVECTOR up = DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), orientation);
    const DirectX::XMVECTOR forward = DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), orientation);
    DirectX::XMVECTOR movement = DirectX::XMVectorZero();
    if (sceneCameraNavigating_) {
        if (ImGui::IsKeyDown(ImGuiKey_W)) {
            movement = DirectX::XMVectorAdd(movement, forward);
        }
        if (ImGui::IsKeyDown(ImGuiKey_S)) {
            movement = DirectX::XMVectorSubtract(movement, forward);
        }
        if (ImGui::IsKeyDown(ImGuiKey_D)) {
            movement = DirectX::XMVectorAdd(movement, right);
        }
        if (ImGui::IsKeyDown(ImGuiKey_A)) {
            movement = DirectX::XMVectorSubtract(movement, right);
        }
        if (ImGui::IsKeyDown(ImGuiKey_E)) {
            movement = DirectX::XMVectorAdd(movement, up);
        }
        if (ImGui::IsKeyDown(ImGuiKey_Q)) {
            movement = DirectX::XMVectorSubtract(movement, up);
        }
    }

    const float movementLengthSquared =
        DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(movement));
    DirectX::XMVECTOR position =
        DirectX::XMLoadFloat3(&sceneViewCamera_.GetPosition());
    bool positionChanged = false;
    if (movementLengthSquared > 0.0f) {
        const float deltaTime = std::clamp(io.DeltaTime, 0.0f, 0.1f);
        const float speed = io.KeyShift ? 12.0f : 4.0f;
        movement = DirectX::XMVectorScale(DirectX::XMVector3Normalize(movement),
                                          speed * deltaTime);
        position = DirectX::XMVectorAdd(position, movement);
        positionChanged = true;
    }
    if (sceneCameraPanning_ &&
        (pointerDeltaX != 0.0f || pointerDeltaY != 0.0f)) {
        constexpr float panSensitivity = 0.01f;
        position = DirectX::XMVectorAdd(
            position,
            DirectX::XMVectorScale(right, -pointerDeltaX * panSensitivity));
        position = DirectX::XMVectorAdd(
            position,
            DirectX::XMVectorScale(up, pointerDeltaY * panSensitivity));
        positionChanged = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
    if (imageHovered && io.MouseWheel != 0.0f) {
        position = DirectX::XMVectorAdd(
            position, DirectX::XMVectorScale(forward, io.MouseWheel * 0.75f));
        positionChanged = true;
    }
    if (positionChanged) {
        DirectX::XMFLOAT3 updatedPosition{};
        DirectX::XMStoreFloat3(&updatedPosition, position);
        sceneViewCamera_.SetPosition(updatedPosition);
    }
}

bool EditorScene::FocusSceneCameraOnSelection() {
    const WorldEntity* entity = world_.Find(selection_);
    DirectX::XMFLOAT4X4 worldMatrix{};
    if (entity == nullptr || !world_.TryGetWorldMatrix(selection_, worldMatrix)) {
        status_ = "Select an entity before focusing the Scene camera.";
        return false;
    }

    DirectX::XMFLOAT3 localCenter{};
    float radius = 1.0f;
    if (entity->meshRenderer && ctx_ != nullptr && ctx_->rendering.model != nullptr) {
        const ModelHandle handle = ResolveModel(*entity->meshRenderer);
        const Model* model = handle.IsValid() ? ctx_->rendering.model->GetModel(handle) : nullptr;
        DirectX::XMFLOAT3 boundsMin{};
        DirectX::XMFLOAT3 boundsMax{};
        if (model != nullptr && TryGetModelBounds(*model, boundsMin, boundsMax)) {
            localCenter = {(boundsMin.x + boundsMax.x) * 0.5f,
                           (boundsMin.y + boundsMax.y) * 0.5f,
                           (boundsMin.z + boundsMax.z) * 0.5f};
            const float extentX = (boundsMax.x - boundsMin.x) * 0.5f;
            const float extentY = (boundsMax.y - boundsMin.y) * 0.5f;
            const float extentZ = (boundsMax.z - boundsMin.z) * 0.5f;
            radius = (std::max)(0.1f, std::sqrt(extentX * extentX + extentY * extentY +
                                               extentZ * extentZ));
        }
    }

    const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&worldMatrix);
    const DirectX::XMVECTOR center = DirectX::XMVector3TransformCoord(
        DirectX::XMLoadFloat3(&localCenter), world);
    const float scaleX = DirectX::XMVectorGetX(DirectX::XMVector3Length(world.r[0]));
    const float scaleY = DirectX::XMVectorGetX(DirectX::XMVector3Length(world.r[1]));
    const float scaleZ = DirectX::XMVectorGetX(DirectX::XMVector3Length(world.r[2]));
    radius *= (std::max)({scaleX, scaleY, scaleZ, 0.001f});

    const DirectX::XMFLOAT3 rotation = sceneViewCamera_.GetRotation();
    const DirectX::XMMATRIX orientation =
        DirectX::XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, 0.0f);
    const DirectX::XMVECTOR forward = DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), orientation);
    const float distance =
        (std::max)(1.0f, radius / std::tan(sceneViewCamera_.GetFovY() * 0.5f) * 1.25f);
    const DirectX::XMVECTOR position =
        DirectX::XMVectorSubtract(center, DirectX::XMVectorScale(forward, distance));
    DirectX::XMFLOAT3 focusedPosition{};
    DirectX::XMStoreFloat3(&focusedPosition, position);
    sceneViewCamera_.SetPosition(focusedPosition);
    sceneViewCamera_.SetClipRange(0.01f,
                                  (std::max)(1000.0f, distance + radius * 4.0f));
    status_ = "Focused the Scene camera on " + entity->name + ".";
    return true;
}

bool EditorScene::AlignSelectedCameraToSceneView() {
    WorldEntity* entity = world_.Find(selection_);
    if (entity == nullptr || !entity->camera) {
        status_ = "Select a Camera before aligning it to the Scene View.";
        return false;
    }

    using namespace DirectX;
    XMFLOAT4X4 currentWorld{};
    TransformComponent currentWorldTransform{};
    if (!world_.TryGetWorldMatrix(entity->id, currentWorld) ||
        !TryDecomposeTransformComponent(XMLoadFloat4x4(&currentWorld),
                                        currentWorldTransform)) {
        status_ = "Could not read the Camera world transform.";
        return false;
    }

    const XMFLOAT3 position = sceneViewCamera_.GetPosition();
    const XMFLOAT3 rotation = sceneViewCamera_.GetRotation();
    XMMATRIX local =
        XMMatrixScaling(currentWorldTransform.scale.x, currentWorldTransform.scale.y,
                        currentWorldTransform.scale.z) *
        XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z) *
        XMMatrixTranslation(position.x, position.y, position.z);
    if (entity->parent.IsValid()) {
        XMFLOAT4X4 parentWorld{};
        if (!world_.TryGetWorldMatrix(entity->parent, parentWorld)) {
            status_ = "Could not read the Camera parent transform.";
            return false;
        }
        XMVECTOR determinant{};
        const XMMATRIX inverseParent =
            XMMatrixInverse(&determinant, XMLoadFloat4x4(&parentWorld));
        const float determinantValue = XMVectorGetX(determinant);
        if (!std::isfinite(determinantValue) || std::abs(determinantValue) <= 1.0e-8f) {
            status_ = "Cannot align a Camera under a singular parent transform.";
            return false;
        }
        local *= inverseParent;
    }

    TransformComponent aligned{};
    if (!TryDecomposeTransformComponent(local, aligned)) {
        status_ = "Could not calculate the aligned Camera transform.";
        return false;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    entity->transform = aligned;
    RecordImmediateEdit("Align Camera to Scene View", before, selectionBefore);
    status_ = "Aligned " + entity->name + " to the Scene View.";
    return true;
}

bool EditorScene::AlignSceneViewToSelectedCamera() {
    const WorldEntity* entity = world_.Find(selection_);
    DirectX::XMFLOAT4X4 worldMatrix{};
    TransformComponent worldTransform{};
    if (entity == nullptr || !entity->camera) {
        status_ = "Select a Camera before moving the Scene View.";
        return false;
    }
    if (!world_.TryGetWorldMatrix(entity->id, worldMatrix) ||
        !TryDecomposeTransformComponent(DirectX::XMLoadFloat4x4(&worldMatrix),
                                        worldTransform)) {
        status_ = "Could not read the Camera world transform.";
        return false;
    }
    sceneViewCamera_.SetPosition(worldTransform.position);
    sceneViewCamera_.SetRotation(
        {DirectX::XMConvertToRadians(worldTransform.rotationDegrees.x),
         DirectX::XMConvertToRadians(worldTransform.rotationDegrees.y),
         DirectX::XMConvertToRadians(worldTransform.rotationDegrees.z)});
    status_ = "Moved the Scene View to " + entity->name + ".";
    return true;
}

void EditorScene::HandleSceneContextMenu(const ImVec2& imageMin, const ImVec2& imageMax,
                                         bool imageHovered) {
    const bool rightClick = sceneCameraPointerTravel_ <= 3.0f;
    if (imageHovered && rightClick && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        sceneContextCreatePosition_ = CalculateScenePlacementPosition(
            sceneViewCamera_, imageMin, imageMax,
            {static_cast<float>(sceneCameraCursorRestoreX_),
             static_cast<float>(sceneCameraCursorRestoreY_)});
        ImGui::OpenPopup("SceneContext");
    }
    if (!ImGui::BeginPopup("SceneContext")) {
        return;
    }
    ImGui::TextDisabled("Create at %.2f, %.2f, %.2f", sceneContextCreatePosition_.x,
                        sceneContextCreatePosition_.y, sceneContextCreatePosition_.z);
    ImGui::Separator();
    DrawCreateEntityMenu(sceneContextCreatePosition_);
    ImGui::EndPopup();
}

void EditorScene::CreateModelEntityFromAsset(const std::filesystem::path& path,
                                             const DirectX::XMFLOAT3& position) {
    std::string assetPath;
    if (!TryNormalizeModelAssetReference(path, assetPath)) {
        return;
    }
    const std::optional<std::filesystem::path> physicalPath = ResolveProjectAssetPath(path);
    std::vector<AssetImport::File> importPlan;
    std::string importError;
    if (!physicalPath ||
        !AssetImport::BuildPlan({*physicalPath}, importPlan, importError)) {
        status_ = "Could not create model entity: " + importError;
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    std::string entityName = path.stem().string();
    if (entityName.empty()) {
        entityName = "Model";
    }
    const EntityId entityId = world_.CreateEntity(std::move(entityName));
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "Could not create an entity for the model asset.";
        return;
    }
    entity->transform.position = position;
    entity->meshRenderer = MeshRendererComponent{};
    entity->meshRenderer->sourceType = MeshSourceType::Model;
    entity->meshRenderer->modelPath = assetPath;
    entity->materialOverride = MaterialOverrideComponent{};
    loadedModels_.erase(assetPath);
    animatorModels_.clear();
    selection_ = entityId;
    RecordImmediateEdit("Create Model Entity", before, selectionBefore);
    status_ = "Created model entity: " + assetPath;
}

bool EditorScene::SaveSelectionAsPrefab() {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before creating a Prefab.";
        return false;
    }
    SynchronizeHierarchySelection();
    const std::vector<EntityId> roots = GetTopLevelSelectedEntities();
    if (roots.size() != 1u) {
        status_ = "Select exactly one entity hierarchy to create a Prefab.";
        return false;
    }
    const WorldEntity* rootEntity = world_.Find(roots.front());
    if (rootEntity == nullptr) {
        status_ = "The selected entity no longer exists.";
        return false;
    }
    const std::optional<std::filesystem::path> destination =
        ShowSavePrefabDialog(rootEntity->name);
    if (!destination) {
        status_ = "Prefab save cancelled.";
        return false;
    }

    std::unordered_set<EntityId, EntityIdHash> includedIds;
    includedIds.insert(roots.front());
    for (const WorldEntity& candidate : world_.Entities()) {
        EntityId current = candidate.parent;
        for (size_t depth = 0u; current.IsValid() && depth <= world_.Entities().size();
             ++depth) {
            if (current == roots.front()) {
                includedIds.insert(candidate.id);
                break;
            }
            const WorldEntity* parent = world_.Find(current);
            current = parent != nullptr ? parent->parent : EntityId{};
        }
    }

    std::vector<WorldEntity> entities;
    entities.reserve(includedIds.size());
    for (const WorldEntity& source : world_.Entities()) {
        if (!includedIds.contains(source.id)) {
            continue;
        }
        WorldEntity prefabEntity = source;
        if (prefabEntity.id == roots.front()) {
            prefabEntity.parent = {};
        }
        for (BehaviorComponent& script : prefabEntity.scripts) {
            for (ScriptPropertyValue& property : script.properties) {
                if (property.type == ScriptPropertyType::Entity &&
                    property.entityValue.IsValid() &&
                    !includedIds.contains(property.entityValue)) {
                    property.entityValue = {};
                }
            }
        }
        entities.push_back(std::move(prefabEntity));
    }
    World prefab;
    std::string error;
    if (!prefab.ReplaceEntities(std::move(entities), &error) ||
        !WorldSerializer::Save(prefab, *destination, &error)) {
        status_ = "Prefab save failed: " + error;
        return false;
    }
    RefreshAssetBrowser();
    std::error_code relativeError;
    selectedAsset_ = std::filesystem::relative(*destination, assetRoot_, relativeError);
    if (relativeError) {
        selectedAsset_.clear();
    }
    status_ = "Saved Prefab: " + destination->string();
    return true;
}

bool EditorScene::TryNormalizeFontAssetReference(
    const std::filesystem::path& path, std::string& assetPath) {
    if (!AssetImport::IsFontFile(path)) {
        status_ = "The dropped font asset is invalid.";
        return false;
    }
    const std::optional<std::filesystem::path> resolvedPath =
        ResolveProjectAssetPath(path);
    std::error_code error;
    if (!resolvedPath ||
        !std::filesystem::is_regular_file(*resolvedPath, error) || error) {
        status_ = "The dropped font asset no longer exists.";
        return false;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    assetPath = normalized.generic_string();
    if (normalized.begin() != normalized.end() &&
        *normalized.begin() == "assets") {
        assetPath =
            "asset://" +
            normalized.lexically_relative("assets").generic_string();
    }
    if (assetPath.size() > 1024u) {
        status_ = "The dropped font asset path is too long.";
        return false;
    }
    return true;
}

bool EditorScene::TryNormalizeAudioAssetReference(const std::filesystem::path& path,
                                                  std::string& assetPath) {
    if (!AssetImport::IsAudioFile(path)) {
        status_ = "The dropped audio asset is invalid.";
        return false;
    }
    const std::optional<std::filesystem::path> resolvedPath = ResolveProjectAssetPath(path);
    std::error_code error;
    if (!resolvedPath || !std::filesystem::is_regular_file(*resolvedPath, error) || error) {
        status_ = "The dropped audio asset no longer exists.";
        return false;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    assetPath = normalized.generic_string();
    if (normalized.begin() != normalized.end() && *normalized.begin() == "assets") {
        assetPath = "asset://" + normalized.lexically_relative("assets").generic_string();
    }
    if (assetPath.size() > 1024u) {
        status_ = "The dropped audio asset path is too long.";
        return false;
    }
    return true;
}

bool EditorScene::InstantiatePrefabAsset(
    const std::filesystem::path& path, EntityId parent,
    std::optional<DirectX::XMFLOAT3> position) {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before instantiating a Prefab.";
        return false;
    }
    const std::optional<std::filesystem::path> resolved = ResolveProjectAssetPath(path);
    std::error_code filesystemError;
    if (!resolved || !IsPrefabAsset(*resolved) ||
        !std::filesystem::is_regular_file(*resolved, filesystemError) || filesystemError ||
        !IsPathWithinRoot(assetRoot_, *resolved)) {
        status_ = "The Prefab asset is invalid or outside the project assets directory.";
        return false;
    }
    World prefab;
    std::string error;
    if (!WorldSerializer::Load(*resolved, prefab, &error)) {
        status_ = "Prefab load failed: " + error;
        return false;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    std::vector<EntityId> roots;
    if (!world_.InstantiateEntityHierarchies(prefab, parent, roots, &error) ||
        roots.empty()) {
        status_ = "Prefab instantiate failed: " + error;
        return false;
    }
    if (position && roots.size() == 1u) {
        if (WorldEntity* root = world_.Find(roots.front())) {
            root->transform.position = *position;
        }
    }
    hierarchySelection_.clear();
    hierarchySelection_.insert(roots.begin(), roots.end());
    selection_ = roots.front();
    hierarchySelectionAnchor_ = selection_;
    RecordImmediateEdit("Instantiate Prefab", before, selectionBefore);
    status_ = "Instantiated Prefab: " + resolved->filename().string();
    return true;
}

void EditorScene::RefreshAssetBrowser() {
    assetPreviewAsset_.clear();
    assetPreviewModel_ = {};
    assetPreviewPlan_.clear();
    assetPreviewError_.clear();
    modelAssets_.clear();
    textureAssets_.clear();
    audioAssets_.clear();
    fontAssets_.clear();
    scriptAssets_.clear();
    prefabAssets_.clear();
    sceneAssets_.clear();
    assetBrowserEntries_.clear();
    std::error_code error;
    std::filesystem::recursive_directory_iterator sceneIterator(
        sceneRoot_, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator sceneEnd;
    while (!error && sceneIterator != sceneEnd) {
        if (sceneIterator->is_regular_file(error) && !error &&
            LowercaseAscii(sceneIterator->path().extension().string()) == ".likescene") {
            std::filesystem::path relative =
                std::filesystem::relative(sceneIterator->path(), sceneRoot_, error);
            if (!error) {
                sceneAssets_.push_back(relative.lexically_normal());
            }
        }
        sceneIterator.increment(error);
    }
    std::ranges::sort(sceneAssets_, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
    error.clear();
    if (!std::filesystem::is_directory(assetRoot_, error) || error) {
        return;
    }

    std::filesystem::path currentDirectory =
        (assetRoot_ / currentAssetDirectory_).lexically_normal();
    if ((!currentAssetDirectory_.empty() &&
         !IsPathWithinRoot(assetRoot_, currentDirectory)) ||
        !std::filesystem::is_directory(currentDirectory, error) || error) {
        currentAssetDirectory_.clear();
        currentDirectory = assetRoot_;
        error.clear();
    }

    std::filesystem::directory_iterator directoryIterator(
        currentDirectory, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator directoryEnd;
    while (!error && directoryIterator != directoryEnd) {
        const std::filesystem::directory_entry entry = *directoryIterator;
        const bool directory = entry.is_directory(error);
        if (!error && directory && IsPathWithinRoot(assetRoot_, entry.path())) {
            const std::filesystem::path relative =
                std::filesystem::relative(entry.path(), assetRoot_, error);
            if (!error) {
                assetBrowserEntries_.push_back({relative.lexically_normal(), true});
            }
        } else if (!error && entry.is_regular_file(error) && !error &&
                   (AssetImport::IsModelFile(entry.path()) ||
                    AssetImport::IsTextureFile(entry.path()) ||
                    AssetImport::IsAudioFile(entry.path()) ||
                    AssetImport::IsFontFile(entry.path()) ||
                    IsPrefabAsset(entry.path()) ||
                    ScriptAssets::IsScriptFile(entry.path()) ||
                    ScriptAssets::IsScriptSourceFile(entry.path()))) {
            const std::filesystem::path relative =
                std::filesystem::relative(entry.path(), assetRoot_, error);
            if (!error) {
                assetBrowserEntries_.push_back({relative.lexically_normal(), false});
            }
        }
        error.clear();
        directoryIterator.increment(error);
    }
    std::ranges::sort(assetBrowserEntries_, [](const AssetBrowserEntry& left,
                                               const AssetBrowserEntry& right) {
        if (left.directory != right.directory) {
            return left.directory;
        }
        return left.relativePath.generic_string() < right.relativePath.generic_string();
    });

    error.clear();
    std::filesystem::recursive_directory_iterator iterator(
        assetRoot_, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) && !error &&
            (AssetImport::IsModelFile(iterator->path()) ||
             AssetImport::IsTextureFile(iterator->path()) ||
             AssetImport::IsAudioFile(iterator->path()) ||
             AssetImport::IsFontFile(iterator->path()) ||
             IsPrefabAsset(iterator->path()) ||
             ScriptAssets::IsScriptFile(iterator->path()))) {
            std::filesystem::path relative =
                std::filesystem::relative(iterator->path(), assetRoot_, error);
            if (!error) {
                auto& assets = IsPrefabAsset(iterator->path())
                                   ? prefabAssets_
                               : AssetImport::IsTextureFile(iterator->path())
                                   ? textureAssets_
                               : AssetImport::IsAudioFile(iterator->path())
                                   ? audioAssets_
                               : AssetImport::IsFontFile(iterator->path())
                                   ? fontAssets_
                               : ScriptAssets::IsScriptFile(iterator->path())
                                         ? scriptAssets_
                                         : modelAssets_;
                assets.push_back((std::filesystem::path("assets") / relative).lexically_normal());
            }
        }
        iterator.increment(error);
    }
    std::ranges::sort(modelAssets_, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
    std::ranges::sort(textureAssets_, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
    std::ranges::sort(audioAssets_, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
    std::ranges::sort(fontAssets_, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
    std::ranges::sort(scriptAssets_, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
    std::ranges::sort(prefabAssets_, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
}

void EditorScene::NavigateAssetBrowser(
    const std::filesystem::path& relativeDirectory) {
    const std::filesystem::path normalized = relativeDirectory.lexically_normal();
    if (normalized.is_absolute() || normalized.has_root_name() ||
        normalized.has_root_directory() || HasParentTraversal(normalized)) {
        status_ = "Asset Browser rejected an invalid directory.";
        return;
    }
    const std::filesystem::path physical =
        normalized == L"." ? assetRoot_ : assetRoot_ / normalized;
    std::error_code error;
    if (!std::filesystem::is_directory(physical, error) || error ||
        (normalized != L"." && !normalized.empty() &&
         !IsPathWithinRoot(assetRoot_, physical))) {
        status_ = "Asset Browser folder no longer exists.";
        return;
    }
    pendingAssetDirectory_ = normalized == L"." ? std::filesystem::path{} : normalized;
}

std::optional<std::filesystem::path>
EditorScene::ResolveProjectAssetPath(const std::filesystem::path& path) const {
    const std::filesystem::path resolved = AssetManager::ResolvePathStrict(path);
    return resolved.empty() ? std::nullopt
                            : std::optional<std::filesystem::path>(resolved);
}

EditorScene::HistoryState EditorScene::CaptureHistoryState() const {
    return {WorldSerializer::Serialize(world_), selection_};
}

bool EditorScene::RestoreHistoryState(const HistoryState& state) {
    World restored;
    std::string error;
    if (!WorldSerializer::Deserialize(state.world, restored, &error)) {
        status_ = "History restore failed: " + error;
        return false;
    }
    world_ = std::move(restored);
    world_.SetPhysicsSettings(physicsSettings_);
    selection_ = world_.Contains(state.selection) ? state.selection : EntityId{};
    hierarchySelection_.clear();
    if (selection_.IsValid()) {
        hierarchySelection_.insert(selection_);
    }
    hierarchySelectionAnchor_ = selection_;
    RefreshDirty();
    return true;
}

void EditorScene::BeginHistoryEdit(std::string label) {
    if (IsInPlayMode()) {
        return;
    }
    if (!pendingHistoryEdit_) {
        pendingHistoryEdit_ = PendingHistoryEdit{std::move(label), CaptureHistoryState()};
    }
}

void EditorScene::CommitHistoryEdit() {
    if (IsInPlayMode()) {
        pendingHistoryEdit_.reset();
        return;
    }
    if (!pendingHistoryEdit_) {
        return;
    }
    PendingHistoryEdit pending = std::move(*pendingHistoryEdit_);
    pendingHistoryEdit_.reset();
    HistoryState after = CaptureHistoryState();
    if (pending.before.world == after.world && pending.before.selection == after.selection) {
        return;
    }
    undoHistory_.push_back(
        {std::move(pending.label), std::move(pending.before), std::move(after)});
    if (undoHistory_.size() > kMaxHistoryEntries) {
        undoHistory_.erase(undoHistory_.begin());
    }
    redoHistory_.clear();
    RefreshDirty();
}

void EditorScene::RecordImmediateEdit(std::string label, std::string before,
                                      EntityId selectionBefore) {
    if (IsInPlayMode()) {
        pendingHistoryEdit_.reset();
        return;
    }
    pendingHistoryEdit_.reset();
    HistoryState after = CaptureHistoryState();
    if (before == after.world && selectionBefore == after.selection) {
        return;
    }
    undoHistory_.push_back({std::move(label), {std::move(before), selectionBefore},
                            std::move(after)});
    if (undoHistory_.size() > kMaxHistoryEntries) {
        undoHistory_.erase(undoHistory_.begin());
    }
    redoHistory_.clear();
    RefreshDirty();
}

void EditorScene::Undo() {
    if (IsInPlayMode()) {
        return;
    }
    CommitHistoryEdit();
    if (undoHistory_.empty()) {
        return;
    }
    HistoryEntry entry = std::move(undoHistory_.back());
    undoHistory_.pop_back();
    if (!RestoreHistoryState(entry.before)) {
        undoHistory_.push_back(std::move(entry));
        return;
    }
    status_ = "Undo: " + entry.label;
    redoHistory_.push_back(std::move(entry));
}

void EditorScene::Redo() {
    if (IsInPlayMode()) {
        return;
    }
    CommitHistoryEdit();
    if (redoHistory_.empty()) {
        return;
    }
    HistoryEntry entry = std::move(redoHistory_.back());
    redoHistory_.pop_back();
    if (!RestoreHistoryState(entry.after)) {
        redoHistory_.push_back(std::move(entry));
        return;
    }
    status_ = "Redo: " + entry.label;
    undoHistory_.push_back(std::move(entry));
}

void EditorScene::ClearHistory(bool markClean) {
    undoHistory_.clear();
    redoHistory_.clear();
    pendingHistoryEdit_.reset();
    savedWorldSnapshot_ = markClean ? WorldSerializer::Serialize(world_) : std::string{};
    RefreshDirty();
}

void EditorScene::RefreshDirty() {
    if (IsInPlayMode()) {
        return;
    }
    dirty_ = WorldSerializer::Serialize(world_) != savedWorldSnapshot_;
}

void EditorScene::ResolveMeshResources() {
    if (ctx_ == nullptr || ctx_->rendering.model == nullptr ||
        ctx_->rendering.texture == nullptr) {
        return;
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.meshRenderer || entity.meshRenderer->sourceType != MeshSourceType::Model ||
            entity.meshRenderer->modelPath.empty() ||
            loadedModels_.contains(entity.meshRenderer->modelPath)) {
            continue;
        }
        if (!ResolveProjectAssetPath(entity.meshRenderer->modelPath)) {
            continue;
        }
        loadedModels_.emplace(entity.meshRenderer->modelPath,
                              ctx_->rendering.model->LoadHandle(
                                  std::filesystem::path(entity.meshRenderer->modelPath).wstring()));
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.materialOverride || entity.materialOverride->baseColorTexturePath.empty() ||
            loadedTextures_.contains(entity.materialOverride->baseColorTexturePath)) {
            continue;
        }
        const std::optional<std::filesystem::path> resolved =
            ResolveProjectAssetPath(entity.materialOverride->baseColorTexturePath);
        if (!resolved || !AssetImport::IsTextureFile(*resolved)) {
            continue;
        }
        loadedTextures_.emplace(
            entity.materialOverride->baseColorTexturePath,
            TextureHandle(ctx_->rendering.texture->LoadSrgb(resolved->wstring())));
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.image || entity.image->texturePath.empty() ||
            loadedTextures_.contains(entity.image->texturePath)) {
            continue;
        }
        const std::optional<std::filesystem::path> resolved =
            ResolveProjectAssetPath(entity.image->texturePath);
        if (!resolved || !AssetImport::IsTextureFile(*resolved)) {
            continue;
        }
        loadedTextures_.emplace(
            entity.image->texturePath,
            TextureHandle(ctx_->rendering.texture->LoadSrgb(resolved->wstring())));
    }
    if (ctx_->rendering.font != nullptr) {
        for (const WorldEntity& entity : world_.Entities()) {
            if (!entity.text || entity.text->fontPath.empty() ||
                loadedFonts_.contains(entity.text->fontPath)) {
                continue;
            }
            const std::optional<std::filesystem::path> resolved =
                ResolveProjectAssetPath(entity.text->fontPath);
            if (!resolved || !AssetImport::IsFontFile(*resolved)) {
                continue;
            }
            loadedFonts_.emplace(
                entity.text->fontPath,
                ctx_->rendering.font->LoadFont(resolved->wstring()));
        }
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.materialOverride) {
            continue;
        }
        const std::string* paths[] = {
            &entity.materialOverride->normalTexturePath,
            &entity.materialOverride->roughnessTexturePath,
            &entity.materialOverride->metallicTexturePath,
        };
        for (const std::string* path : paths) {
            if (path->empty() || loadedLinearTextures_.contains(*path)) {
                continue;
            }
            const std::optional<std::filesystem::path> resolved =
                ResolveProjectAssetPath(*path);
            if (!resolved || !AssetImport::IsTextureFile(*resolved)) {
                continue;
            }
            loadedLinearTextures_.emplace(
                *path, TextureHandle(ctx_->rendering.texture->LoadLinear(resolved->wstring())));
        }
    }
}

ModelHandle EditorScene::ResolveModel(const MeshRendererComponent& component) const {
    if (component.sourceType == MeshSourceType::Primitive) {
        const size_t index = static_cast<size_t>(component.primitive);
        return index < std::size(primitiveModels_) ? primitiveModels_[index] : ModelHandle{};
    }
    const auto found = loadedModels_.find(component.modelPath);
    return found != loadedModels_.end() ? found->second : ModelHandle{};
}

TextureHandle EditorScene::ResolveBaseColorTexture(
    const MaterialOverrideComponent& component) const {
    const auto found = loadedTextures_.find(component.baseColorTexturePath);
    return found != loadedTextures_.end() ? found->second : TextureHandle{};
}

TextureHandle EditorScene::ResolveNormalTexture(
    const MaterialOverrideComponent& component) const {
    return ResolveLinearTexture(component.normalTexturePath);
}

TextureHandle EditorScene::ResolveLinearTexture(const std::string& path) const {
    const auto found = loadedLinearTextures_.find(path);
    return found != loadedLinearTextures_.end() ? found->second : TextureHandle{};
}

bool EditorScene::UpdateGameViewCamera() {
    const WorldEntity* primaryCamera = nullptr;
    for (const WorldEntity& entity : world_.Entities()) {
        if (world_.IsActiveInHierarchy(entity.id) && entity.camera &&
            entity.camera->enabled && entity.camera->primary) {
            primaryCamera = &entity;
            break;
        }
    }
    if (primaryCamera == nullptr) {
        return false;
    }

    return UpdateCameraFromEntity(primaryCamera->id, gameViewCamera_,
                                  gameViewSurface_.GetWidth(), gameViewSurface_.GetHeight());
}

bool EditorScene::UpdateCameraFromEntity(EntityId entityId, Camera& targetCamera, int width,
                                         int height) const {
    const WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr || !world_.IsActiveInHierarchy(entityId) || !entity->camera) {
        return false;
    }

    DirectX::XMFLOAT4X4 worldMatrix{};
    TransformComponent worldTransform{};
    if (!world_.TryGetWorldMatrix(entity->id, worldMatrix) ||
        !TryDecomposeTransformComponent(DirectX::XMLoadFloat4x4(&worldMatrix),
                                        worldTransform)) {
        return false;
    }
    targetCamera.SetPosition(worldTransform.position);
    targetCamera.SetRotation(
        {DirectX::XMConvertToRadians(worldTransform.rotationDegrees.x),
         DirectX::XMConvertToRadians(worldTransform.rotationDegrees.y),
         DirectX::XMConvertToRadians(worldTransform.rotationDegrees.z)});
    const CameraComponent& component = *entity->camera;
    targetCamera.SetAspect(static_cast<float>((std::max)(1, width)) /
                           static_cast<float>((std::max)(1, height)));
    if (component.projection == CameraProjection::Perspective) {
        targetCamera.SetPerspectiveFovDeg(component.fieldOfViewDegrees);
    } else {
        targetCamera.SetOrthographicHeight(component.orthographicHeight);
    }
    targetCamera.SetClipRange(component.nearClip, component.farClip);
    return true;
}

bool EditorScene::DrawSelectedCameraPreview(const ImVec2& imageMin,
                                            const ImVec2& imageMax) {
    const WorldEntity* entity = world_.Find(selection_);
    ImVec2 previewMin{};
    ImVec2 previewMax{};
    if (entity == nullptr || !world_.IsActiveInHierarchy(selection_) || !entity->camera ||
        !cameraPreviewSurface_.IsReady() ||
        !cameraPreviewPostProcess_.IsReady() || ctx_ == nullptr ||
        ctx_->rendering.dxCommon == nullptr || ctx_->rendering.model == nullptr ||
        !TryGetCameraPreviewRect(imageMin, imageMax, previewMin, previewMax) ||
        !UpdateCameraFromEntity(entity->id, cameraPreviewCamera_,
                                cameraPreviewSurface_.GetWidth(),
                                cameraPreviewSurface_.GetHeight())) {
        return false;
    }

    sceneRenderer_.Render(renderScene_, cameraPreviewCamera_, cameraPreviewSurface_,
                          {0.025f, 0.035f, 0.055f, 1.0f});
    cameraPreviewSurface_.TransitionDepthToShaderResource();
    cameraPreviewSurface_.BeginOutputPass({0.0f, 0.0f, 0.0f, 1.0f});
    const PostProcessOutputTarget target{
        cameraPreviewSurface_.GetOutputRtvHandle(),
        static_cast<uint32_t>(cameraPreviewSurface_.GetWidth()),
        static_cast<uint32_t>(cameraPreviewSurface_.GetHeight()),
        DirectXCommon::kBackBufferFormat,
    };
    cameraPreviewPostProcess_.DrawToTarget(cameraPreviewSurface_.GetSceneColorGpuHandle(),
                                           cameraPreviewSurface_.GetDepthGpuHandle(), target);
    cameraPreviewSurface_.EndOutputPass();
    cameraPreviewSurface_.TransitionDepthToWrite();
    ctx_->rendering.dxCommon->SetBackBufferRenderTarget(false, false);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const D3D12_GPU_DESCRIPTOR_HANDLE output = cameraPreviewSurface_.GetOutputGpuHandle();
    drawList->AddImage(static_cast<ImTextureID>(output.ptr), previewMin, previewMax);
    drawList->AddRect(previewMin, previewMax, IM_COL32(255, 184, 56, 255), 3.0f, 0, 2.0f);
    const std::string label = "Camera Preview  |  " + entity->name;
    const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
    drawList->AddRectFilled(previewMin,
                            {previewMin.x + (std::min)(previewMax.x - previewMin.x,
                                                      textSize.x + 12.0f),
                             previewMin.y + textSize.y + 8.0f},
                            IM_COL32(18, 22, 30, 220), 3.0f,
                            ImDrawFlags_RoundCornersTopLeft);
    drawList->AddText({previewMin.x + 6.0f, previewMin.y + 4.0f},
                      IM_COL32(240, 242, 248, 255), label.c_str());
    return ImGui::IsMouseHoveringRect(previewMin, previewMax);
}

void EditorScene::UpdateAssetPreview() {
    const std::filesystem::path relative = selectedAsset_.lexically_normal();
    if (relative == assetPreviewAsset_) {
        return;
    }
    StopAudioAssetPreview();
    audioPreviewSoundId_ = ISoundService::kInvalidSoundId;
    assetPreviewAsset_ = relative;
    assetPreviewModel_ = {};
    assetPreviewAnimation_.clear();
    assetPreviewAnimationLoop_ = true;
    assetPreviewAnimationSpeed_ = 1.0f;
    assetPreviewRotationDegrees_ = {0.0f, 180.0f};
    assetPreviewPlan_.clear();
    assetPreviewError_.clear();
    assetPreviewTransform_ = {};
    if (relative.empty() || ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        return;
    }

    const std::filesystem::path physical = assetRoot_ / relative;
    std::error_code error;
    if (!std::filesystem::is_regular_file(physical, error) || error ||
        !AssetImport::IsModelFile(physical)) {
        return;
    }
    if (!AssetImport::BuildPlan({physical}, assetPreviewPlan_, assetPreviewError_)) {
        status_ = "Asset preview dependency validation failed: " + assetPreviewError_;
        return;
    }
    const std::string previewKey = physical.lexically_normal().generic_string();
    const auto cachedPreview = assetPreviewModels_.find(previewKey);
    if (cachedPreview != assetPreviewModels_.end()) {
        assetPreviewModel_ = cachedPreview->second;
    } else {
        assetPreviewModel_ = ctx_->rendering.model->LoadUniqueHandle(physical.wstring());
        if (assetPreviewModel_.IsValid()) {
            assetPreviewModels_.emplace(previewKey, assetPreviewModel_);
        }
    }
    const Model* model = assetPreviewModel_.IsValid()
                             ? ctx_->rendering.model->GetModel(assetPreviewModel_)
                             : nullptr;
    if (model == nullptr) {
        assetPreviewError_ = "The selected model could not be loaded for preview.";
        status_ = "Asset preview failed for assets/" + relative.generic_string() +
                  ": model loading failed.";
        return;
    }

    DirectX::XMFLOAT3 boundsMin{};
    DirectX::XMFLOAT3 boundsMax{};
    if (!TryGetModelBounds(*model, boundsMin, boundsMax)) {
        assetPreviewCamera_.SetPosition({0.0f, 0.0f, -4.0f});
        assetPreviewCamera_.SetClipRange(0.01f, 1000.0f);
        return;
    }
    const DirectX::XMFLOAT3 center{
        (boundsMin.x + boundsMax.x) * 0.5f,
        (boundsMin.y + boundsMax.y) * 0.5f,
        (boundsMin.z + boundsMax.z) * 0.5f,
    };
    const float extentX = (boundsMax.x - boundsMin.x) * 0.5f;
    const float extentY = (boundsMax.y - boundsMin.y) * 0.5f;
    const float extentZ = (boundsMax.z - boundsMin.z) * 0.5f;
    const float radius = (std::max)(0.05f, std::sqrt(extentX * extentX + extentY * extentY +
                                                     extentZ * extentZ));
    const float distance =
        (std::max)(0.25f, radius / std::tan(assetPreviewCamera_.GetFovY() * 0.5f) * 1.25f);
    assetPreviewTransform_.position = {-center.x, -center.y, -center.z};
    assetPreviewCamera_.SetPosition({0.0f, 0.0f, -distance});
    assetPreviewCamera_.SetClipRange((std::max)(0.01f, distance - radius * 2.0f),
                                     distance + radius * 4.0f);
}

bool EditorScene::DrawGameUi(int width, int height,
                             bool gameCameraAvailable) {
    if (ctx_ == nullptr || ctx_->rendering.text == nullptr ||
        ctx_->rendering.spriteRenderer == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    TextRenderer& textRenderer = *ctx_->rendering.text;
    SpriteRenderer& spriteRenderer = *ctx_->rendering.spriteRenderer;
    if (!textRenderer.IsReady() || !spriteRenderer.IsReady()) {
        return false;
    }

    const auto resolveCanvasLayout =
        [&](const WorldEntity& entity, float& scale,
            DirectX::XMFLOAT2& origin,
            DirectX::XMFLOAT2& referenceResolution) {
            const CanvasComponent* canvas =
                FindEnabledCanvas(world_, entity);
            if (canvas == nullptr) {
                return false;
            }
            CalculateCanvasLayout(
                *canvas, static_cast<float>(width),
                static_cast<float>(height), 0.0f, 0.0f, scale, origin,
                referenceResolution);
            return true;
    };
    const std::vector<OrderedUiEntity> orderedUiEntities =
        GetOrderedUiEntities(world_);
    const WorldEntity* eventSystemEntity =
        FindEventSystemEntity(world_);
    const EventSystemComponent* eventSystem =
        eventSystemEntity != nullptr
            ? &*eventSystemEntity->eventSystem
            : nullptr;
    const bool uiEventsEnabled =
        eventSystem == nullptr ||
        (eventSystem->enabled &&
         world_.IsActiveInHierarchy(eventSystemEntity->id));
    for (auto transition = buttonColorTransitions_.begin();
         transition != buttonColorTransitions_.end();) {
        const WorldEntity* entity = world_.Find(transition->first);
        if (entity == nullptr || !entity->button || !entity->image) {
            transition = buttonColorTransitions_.erase(transition);
        } else {
            ++transition;
        }
    }

    const ImVec2 imageScreenMin = ImGui::GetCursorScreenPos();
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool canPoint =
        playModeState_ == PlayModeState::Playing &&
        uiEventsEnabled && !gameInputCaptured_ &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
        requestedGameWidth_ > 0 && requestedGameHeight_ > 0 &&
        mouse.x >= imageScreenMin.x && mouse.y >= imageScreenMin.y &&
        mouse.x <= imageScreenMin.x + requestedGameWidth_ &&
        mouse.y <= imageScreenMin.y + requestedGameHeight_;
    const DirectX::XMFLOAT2 pointer{
        (mouse.x - imageScreenMin.x) * static_cast<float>(width) /
            static_cast<float>((std::max)(1, requestedGameWidth_)),
        (mouse.y - imageScreenMin.y) * static_cast<float>(height) /
            static_cast<float>((std::max)(1, requestedGameHeight_)),
    };
    const auto calculateImageRect =
        [&](const WorldEntity& entity, float& left, float& top,
            float& right, float& bottom) {
            if (!entity.image || !entity.image->enabled) {
                return false;
            }
            float scale = 1.0f;
            DirectX::XMFLOAT2 origin{};
            DirectX::XMFLOAT2 referenceResolution{};
            if (!resolveCanvasLayout(entity, scale, origin,
                                     referenceResolution)) {
                return false;
            }
            const ImageComponent& image = *entity.image;
            const DirectX::XMFLOAT2 anchor =
                GetUiAnchorChoice(image.anchor).factor;
            left = origin.x +
                   (referenceResolution.x * anchor.x +
                    image.position.x - image.size.x * image.pivot.x) *
                       scale;
            top = origin.y +
                  (referenceResolution.y * anchor.y +
                   image.position.y - image.size.y * image.pivot.y) *
                      scale;
            right = left + image.size.x * scale;
            bottom = top + image.size.y * scale;
            return true;
        };
    const auto setSliderValue =
        [&](WorldEntity& entity, float requestedValue) {
            SliderComponent& slider = *entity.slider;
            float value = std::clamp(requestedValue, slider.minValue,
                                     slider.maxValue);
            if (slider.wholeNumbers) {
                value = std::clamp(std::round(value),
                                   slider.minValue, slider.maxValue);
            }
            if (value == slider.value) {
                return;
            }
            slider.value = value;
            const auto pending = std::ranges::find(
                pendingSliderValueChanges_, entity.id,
                &SliderValueChange::entity);
            if (pending != pendingSliderValueChanges_.end()) {
                pending->value = value;
            } else {
                pendingSliderValueChanges_.push_back(
                    {entity.id, value});
            }
        };
    const auto setSliderValueFromPointer =
        [&](WorldEntity& entity) {
            float left = 0.0f;
            float top = 0.0f;
            float right = 0.0f;
            float bottom = 0.0f;
            if (!calculateImageRect(entity, left, top, right,
                                    bottom)) {
                return;
            }
            SliderComponent& slider = *entity.slider;
            float normalized = 0.0f;
            if (slider.direction == SliderDirection::LeftToRight ||
                slider.direction == SliderDirection::RightToLeft) {
                normalized =
                    (pointer.x - left) /
                    (std::max)(right - left, 0.0001f);
                if (slider.direction ==
                    SliderDirection::RightToLeft) {
                    normalized = 1.0f - normalized;
                }
            } else {
                normalized =
                    (pointer.y - top) /
                    (std::max)(bottom - top, 0.0001f);
                if (slider.direction ==
                    SliderDirection::BottomToTop) {
                    normalized = 1.0f - normalized;
                }
            }
            normalized = std::clamp(normalized, 0.0f, 1.0f);
            setSliderValue(
                entity,
                std::lerp(slider.minValue, slider.maxValue,
                          normalized));
        };
    const auto queueInputFieldEvent =
        [&](EntityId entity, const std::string& text,
            bool submitted) {
            if (!submitted) {
                const auto pending = std::ranges::find_if(
                    pendingInputFieldEvents_,
                    [&](const InputFieldEvent& event) {
                        return event.entity == entity &&
                               !event.submitted;
                    });
                if (pending !=
                    pendingInputFieldEvents_.end()) {
                    pending->text = text;
                    return;
                }
            }
            pendingInputFieldEvents_.push_back(
                {entity, text, submitted});
        };

    EntityId hoveredButton{};
    std::vector<EntityId> selectableButtons;
    std::unordered_map<EntityId, DirectX::XMFLOAT2, EntityIdHash>
        selectableCenters;
    for (const OrderedUiEntity& entry : orderedUiEntities) {
        const WorldEntity& entity = *entry.entity;
        const bool isButton =
            entity.button && entity.button->enabled &&
            (!entity.toggle || entity.toggle->enabled) &&
            (!entity.dropdown || entity.dropdown->enabled) &&
            (!entity.inputField || entity.inputField->enabled);
        const bool isSlider =
            entity.slider && entity.slider->enabled;
        if ((!isButton && !isSlider) || !entity.image ||
            !entity.image->enabled) {
            continue;
        }
        const UiGroupState groupState =
            GetUiGroupState(world_, entity);
        const bool controlInteractable =
            groupState.interactable &&
            (isSlider ? entity.slider->interactable
                      : entity.button->interactable &&
                            (!entity.dropdown ||
                             entity.dropdown->interactable) &&
                            (!entity.inputField ||
                             entity.inputField->interactable));
        const bool navigationEnabled =
            isSlider ||
            entity.button->navigation !=
                ButtonNavigationMode::None;
        if (controlInteractable && navigationEnabled) {
            selectableButtons.push_back(entity.id);
            float left = 0.0f;
            float top = 0.0f;
            float right = 0.0f;
            float bottom = 0.0f;
            if (calculateImageRect(entity, left, top, right, bottom)) {
                selectableCenters.emplace(
                    entity.id,
                    DirectX::XMFLOAT2{(left + right) * 0.5f,
                                     (top + bottom) * 0.5f});
            }
        }
        if (canPoint && groupState.blocksRaycasts) {
            float left = 0.0f;
            float top = 0.0f;
            float right = 0.0f;
            float bottom = 0.0f;
            if (calculateImageRect(entity, left, top, right, bottom) &&
                pointer.x >= left && pointer.x <= right &&
                pointer.y >= top && pointer.y <= bottom) {
                hoveredButton = entity.id;
            }
        }
    }

    WorldEntity* openDropdownEntity =
        world_.Find(openDropdown_);
    if (openDropdownEntity == nullptr ||
        !openDropdownEntity->dropdown ||
        !openDropdownEntity->dropdown->enabled ||
        !openDropdownEntity->dropdown->interactable ||
        !openDropdownEntity->button ||
        !openDropdownEntity->button->enabled ||
        !openDropdownEntity->button->interactable ||
        !openDropdownEntity->image ||
        !GetUiGroupState(world_, *openDropdownEntity)
             .interactable) {
        openDropdown_ = {};
        openDropdownEntity = nullptr;
    }
    int32_t hoveredDropdownOption = -1;
    float dropdownLeft = 0.0f;
    float dropdownTop = 0.0f;
    float dropdownRight = 0.0f;
    float dropdownBottom = 0.0f;
    float dropdownScale = 1.0f;
    if (openDropdownEntity != nullptr &&
        calculateImageRect(*openDropdownEntity, dropdownLeft,
                           dropdownTop, dropdownRight,
                           dropdownBottom)) {
        DirectX::XMFLOAT2 dropdownOrigin{};
        DirectX::XMFLOAT2 dropdownResolution{};
        resolveCanvasLayout(*openDropdownEntity, dropdownScale,
                            dropdownOrigin, dropdownResolution);
        const DropdownComponent& dropdown =
            *openDropdownEntity->dropdown;
        const float rowHeight =
            dropdown.itemHeight * dropdownScale;
        if (canPoint && pointer.x >= dropdownLeft &&
            pointer.x <= dropdownRight &&
            pointer.y >= dropdownBottom &&
            pointer.y <
                dropdownBottom +
                    rowHeight *
                        static_cast<float>(
                            dropdown.options.size())) {
            hoveredDropdownOption =
                static_cast<int32_t>(
                    (pointer.y - dropdownBottom) /
                    rowHeight);
            dropdownHighlightedIndex_ =
                hoveredDropdownOption;
            hoveredButton = {};
        }
    }
    const bool dropdownWasOpen =
        openDropdownEntity != nullptr;
    WorldEntity* activeInputFieldEntity =
        world_.Find(activeInputField_);
    if (activeInputFieldEntity == nullptr ||
        !activeInputFieldEntity->inputField ||
        !activeInputFieldEntity->inputField->enabled ||
        !activeInputFieldEntity->inputField->interactable ||
        !activeInputFieldEntity->button ||
        !activeInputFieldEntity->button->enabled ||
        !activeInputFieldEntity->button->interactable ||
        !GetUiGroupState(world_, *activeInputFieldEntity)
             .interactable) {
        activeInputField_ = {};
        activeInputFieldEntity = nullptr;
    }

    if (focusedButton_.IsValid() &&
        std::ranges::find(selectableButtons, focusedButton_) ==
            selectableButtons.end()) {
        focusedButton_ = {};
    }
    if (playModeState_ == PlayModeState::Playing &&
        uiEventsEnabled &&
        !runtimeInitialUiSelectionApplied_) {
        runtimeInitialUiSelectionApplied_ = true;
        if (eventSystem != nullptr &&
            std::ranges::find(selectableButtons,
                              eventSystem->firstSelected) !=
                selectableButtons.end()) {
            focusedButton_ = eventSystem->firstSelected;
        }
    }
    const bool canNavigateUi =
        playModeState_ == PlayModeState::Playing &&
        uiEventsEnabled &&
        !gameInputCaptured_ &&
        (eventSystem == nullptr ||
         eventSystem->sendNavigationEvents) &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput;
    const Input* runtimeInput =
        ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (canNavigateUi && activeInputFieldEntity != nullptr) {
        InputFieldComponent& inputField =
            *activeInputFieldEntity->inputField;
        bool changed = false;
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true) &&
            !inputField.text.empty()) {
            PopUtf8Codepoint(inputField.text);
            changed = true;
        }
        for (const ImWchar character :
             ImGui::GetIO().InputQueueCharacters) {
            if (character < 0x20u || character == 0x7fu) {
                continue;
            }
            if (inputField.characterLimit > 0 &&
                CountUtf8Codepoints(inputField.text) >=
                    static_cast<size_t>(
                        inputField.characterLimit)) {
                break;
            }
            char encoded[5]{};
            const int length = ImTextCharToUtf8(
                encoded,
                static_cast<unsigned int>(character));
            if (length <= 0 ||
                inputField.text.size() +
                        static_cast<size_t>(length) >
                    4096u) {
                continue;
            }
            inputField.text.append(
                encoded, static_cast<size_t>(length));
            changed = true;
        }
        if (changed) {
            queueInputFieldEvent(
                activeInputField_,
                inputField.text, false);
        }
    }
    const WorldEntity* focusedEntity =
        world_.Find(focusedButton_);
    const bool focusedSlider =
        focusedEntity != nullptr && focusedEntity->slider &&
        focusedEntity->slider->enabled &&
        focusedEntity->slider->interactable;
    const bool keyboardNavigation =
        canNavigateUi && !activeInputField_.IsValid() &&
        ImGui::IsKeyPressed(ImGuiKey_Tab, false);
    bool navigatedUi = false;
    if (keyboardNavigation && !selectableButtons.empty()) {
        const bool selectPrevious = ImGui::GetIO().KeyShift;
        const auto focused =
            std::ranges::find(selectableButtons, focusedButton_);
        if (focused == selectableButtons.end()) {
            focusedButton_ = selectPrevious
                                 ? selectableButtons.back()
                                 : selectableButtons.front();
        } else {
            const size_t index =
                static_cast<size_t>(focused - selectableButtons.begin());
            focusedButton_ =
                selectPrevious
                    ? selectableButtons[(index + selectableButtons.size() - 1u) %
                                        selectableButtons.size()]
                    : selectableButtons[(index + 1u) %
                                        selectableButtons.size()];
        }
        navigatedUi = true;
    }
    if (canNavigateUi && openDropdownEntity != nullptr) {
        const int32_t optionCount = static_cast<int32_t>(
            openDropdownEntity->dropdown->options.size());
        const bool selectPrevious =
            ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
            (runtimeInput != nullptr &&
             runtimeInput->IsGamepadButtonTrigger(
                 XINPUT_GAMEPAD_DPAD_UP));
        const bool selectNext =
            ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
            (runtimeInput != nullptr &&
             runtimeInput->IsGamepadButtonTrigger(
                 XINPUT_GAMEPAD_DPAD_DOWN));
        if (selectPrevious) {
            dropdownHighlightedIndex_ =
                (dropdownHighlightedIndex_ + optionCount - 1) %
                optionCount;
            navigatedUi = true;
        } else if (selectNext) {
            dropdownHighlightedIndex_ =
                (dropdownHighlightedIndex_ + 1) % optionCount;
            navigatedUi = true;
        }
    }
    enum class UiNavigationDirection : uint8_t {
        None,
        Left,
        Right,
        Up,
        Down,
    };
    UiNavigationDirection navigationDirection =
        UiNavigationDirection::None;
    if (canNavigateUi && !focusedSlider &&
        !activeInputField_.IsValid() &&
        openDropdownEntity == nullptr) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
            (runtimeInput != nullptr &&
             runtimeInput->IsGamepadButtonTrigger(
                 XINPUT_GAMEPAD_DPAD_LEFT))) {
            navigationDirection = UiNavigationDirection::Left;
        } else if (
            ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
            (runtimeInput != nullptr &&
             runtimeInput->IsGamepadButtonTrigger(
                 XINPUT_GAMEPAD_DPAD_RIGHT))) {
            navigationDirection = UiNavigationDirection::Right;
        } else if (
            ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
            (runtimeInput != nullptr &&
             runtimeInput->IsGamepadButtonTrigger(
                 XINPUT_GAMEPAD_DPAD_UP))) {
            navigationDirection = UiNavigationDirection::Up;
        } else if (
            ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
            (runtimeInput != nullptr &&
             runtimeInput->IsGamepadButtonTrigger(
                 XINPUT_GAMEPAD_DPAD_DOWN))) {
            navigationDirection = UiNavigationDirection::Down;
        }
    }
    if (navigationDirection != UiNavigationDirection::None &&
        !selectableButtons.empty() &&
        (focusedButton_.IsValid() || !gameCameraAvailable)) {
        const WorldEntity* currentControl =
            world_.Find(focusedButton_);
        const bool explicitNavigation =
            currentControl != nullptr && currentControl->button &&
            currentControl->button->navigation ==
                ButtonNavigationMode::Explicit;
        if (explicitNavigation) {
            EntityId target{};
            switch (navigationDirection) {
            case UiNavigationDirection::Left:
                target = currentControl->button->selectOnLeft;
                break;
            case UiNavigationDirection::Right:
                target = currentControl->button->selectOnRight;
                break;
            case UiNavigationDirection::Up:
                target = currentControl->button->selectOnUp;
                break;
            case UiNavigationDirection::Down:
                target = currentControl->button->selectOnDown;
                break;
            case UiNavigationDirection::None:
                break;
            }
            if (std::ranges::find(selectableButtons, target) !=
                selectableButtons.end()) {
                focusedButton_ = target;
            }
        } else if (const auto currentCenter =
                       selectableCenters.find(focusedButton_);
                   currentCenter == selectableCenters.end()) {
            focusedButton_ = selectableButtons.front();
        } else {
            EntityId best{};
            float bestScore =
                (std::numeric_limits<float>::max)();
            for (const EntityId candidate : selectableButtons) {
                if (candidate == focusedButton_) {
                    continue;
                }
                const auto candidateCenter =
                    selectableCenters.find(candidate);
                if (candidateCenter == selectableCenters.end()) {
                    continue;
                }
                const float deltaX =
                    candidateCenter->second.x -
                    currentCenter->second.x;
                const float deltaY =
                    candidateCenter->second.y -
                    currentCenter->second.y;
                float forward = 0.0f;
                float perpendicular = 0.0f;
                switch (navigationDirection) {
                case UiNavigationDirection::Left:
                    forward = -deltaX;
                    perpendicular = std::abs(deltaY);
                    break;
                case UiNavigationDirection::Right:
                    forward = deltaX;
                    perpendicular = std::abs(deltaY);
                    break;
                case UiNavigationDirection::Up:
                    forward = -deltaY;
                    perpendicular = std::abs(deltaX);
                    break;
                case UiNavigationDirection::Down:
                    forward = deltaY;
                    perpendicular = std::abs(deltaX);
                    break;
                case UiNavigationDirection::None:
                    break;
                }
                if (forward <= 0.0f) {
                    continue;
                }
                const float score =
                    forward +
                    perpendicular * perpendicular /
                        (forward + 0.001f);
                if (score < bestScore) {
                    best = candidate;
                    bestScore = score;
                }
            }
            if (best.IsValid()) {
                focusedButton_ = best;
            }
        }
        navigatedUi = true;
    }

    const WorldEntity* hoveredEntity = world_.Find(hoveredButton);
    const bool hoveredButtonInteractable =
        hoveredEntity != nullptr && hoveredEntity->button &&
        hoveredEntity->button->enabled &&
        hoveredEntity->button->interactable &&
        (!hoveredEntity->dropdown ||
         (hoveredEntity->dropdown->enabled &&
          hoveredEntity->dropdown->interactable)) &&
        (!hoveredEntity->inputField ||
         (hoveredEntity->inputField->enabled &&
          hoveredEntity->inputField->interactable)) &&
        GetUiGroupState(world_, *hoveredEntity).interactable;
    const bool hoveredSliderInteractable =
        hoveredEntity != nullptr && hoveredEntity->slider &&
        hoveredEntity->slider->enabled &&
        hoveredEntity->slider->interactable &&
        GetUiGroupState(world_, *hoveredEntity).interactable;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (activeInputField_.IsValid() &&
            hoveredButton != activeInputField_) {
            activeInputField_ = {};
            activeInputFieldEntity = nullptr;
        }
        if (openDropdownEntity != nullptr &&
            hoveredDropdownOption >= 0) {
            DropdownComponent& dropdown =
                *openDropdownEntity->dropdown;
            if (dropdown.value != hoveredDropdownOption) {
                dropdown.value = hoveredDropdownOption;
                pendingDropdownValueChanges_.push_back(
                    {openDropdown_, dropdown.value});
            }
            focusedButton_ = openDropdown_;
            openDropdown_ = {};
            openDropdownEntity = nullptr;
            pressedButton_ = {};
            activeSlider_ = {};
        } else if (hoveredSliderInteractable) {
            activeSlider_ = hoveredButton;
            pressedButton_ = {};
            focusedButton_ = hoveredButton;
            setSliderValueFromPointer(*world_.Find(activeSlider_));
        } else {
            activeSlider_ = {};
            if (openDropdownEntity != nullptr &&
                hoveredButton != openDropdown_) {
                openDropdown_ = {};
                openDropdownEntity = nullptr;
            }
            pressedButton_ =
                hoveredButtonInteractable ? hoveredButton
                                          : EntityId{};
            focusedButton_ =
                hoveredButtonInteractable ? hoveredButton
                                          : EntityId{};
        }
    }
    if (activeSlider_.IsValid() &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        WorldEntity* sliderEntity = world_.Find(activeSlider_);
        if (sliderEntity != nullptr && sliderEntity->slider &&
            sliderEntity->slider->enabled &&
            sliderEntity->slider->interactable &&
            GetUiGroupState(world_, *sliderEntity).interactable) {
            setSliderValueFromPointer(*sliderEntity);
        } else {
            activeSlider_ = {};
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (pressedButton_.IsValid() &&
            pressedButton_ == hoveredButton &&
            hoveredButtonInteractable) {
            WorldEntity* clicked =
                world_.Find(pressedButton_);
            if (clicked != nullptr && clicked->dropdown &&
                clicked->dropdown->enabled &&
                clicked->dropdown->interactable) {
                if (openDropdown_ == pressedButton_) {
                    openDropdown_ = {};
                    openDropdownEntity = nullptr;
                } else {
                    openDropdown_ = pressedButton_;
                    openDropdownEntity = clicked;
                    dropdownHighlightedIndex_ =
                        clicked->dropdown->value;
                }
            } else if (
                clicked != nullptr && clicked->inputField &&
                clicked->inputField->enabled &&
                clicked->inputField->interactable) {
                activeInputField_ = clicked->id;
                activeInputFieldEntity = clicked;
                openDropdown_ = {};
                openDropdownEntity = nullptr;
            } else {
                pendingButtonClicks_.push_back(
                    pressedButton_);
            }
        }
        pressedButton_ = {};
        activeSlider_ = {};
    }
    const bool gamepadSubmit =
        canNavigateUi && runtimeInput != nullptr &&
        runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_A);
    const bool submitHeld =
        canNavigateUi &&
        (ImGui::IsKeyDown(ImGuiKey_Enter) ||
         ImGui::IsKeyDown(ImGuiKey_Space) ||
         (runtimeInput != nullptr &&
          runtimeInput->IsGamepadButtonPress(XINPUT_GAMEPAD_A)));
    const WorldEntity* submitEntity = world_.Find(focusedButton_);
    if (canNavigateUi && submitEntity != nullptr &&
        submitEntity->button && submitEntity->button->enabled &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
         gamepadSubmit)) {
        const bool inputFieldSubmit =
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            gamepadSubmit;
        if (submitEntity->inputField &&
            submitEntity->inputField->enabled &&
            submitEntity->inputField->interactable) {
            if (inputFieldSubmit) {
                if (activeInputField_ == focusedButton_) {
                    queueInputFieldEvent(
                        focusedButton_,
                        submitEntity->inputField->text, true);
                    activeInputField_ = {};
                    activeInputFieldEntity = nullptr;
                } else {
                    activeInputField_ = focusedButton_;
                    activeInputFieldEntity =
                        world_.Find(activeInputField_);
                    openDropdown_ = {};
                    openDropdownEntity = nullptr;
                }
            }
        } else if (submitEntity->dropdown &&
            submitEntity->dropdown->enabled &&
            submitEntity->dropdown->interactable) {
            if (openDropdown_ == focusedButton_) {
                DropdownComponent& dropdown =
                    *world_.Find(openDropdown_)->dropdown;
                if (dropdown.value !=
                    dropdownHighlightedIndex_) {
                    dropdown.value =
                        dropdownHighlightedIndex_;
                    pendingDropdownValueChanges_.push_back(
                        {openDropdown_, dropdown.value});
                }
                openDropdown_ = {};
                openDropdownEntity = nullptr;
            } else {
                openDropdown_ = focusedButton_;
                openDropdownEntity =
                    world_.Find(openDropdown_);
                dropdownHighlightedIndex_ =
                    submitEntity->dropdown->value;
            }
        } else {
            pendingButtonClicks_.push_back(focusedButton_);
        }
    }
    WorldEntity* keyboardSlider = world_.Find(focusedButton_);
    if (canNavigateUi && !navigatedUi &&
        !activeInputField_.IsValid() &&
        keyboardSlider != nullptr &&
        keyboardSlider->slider && keyboardSlider->slider->enabled &&
        keyboardSlider->slider->interactable &&
        GetUiGroupState(world_, *keyboardSlider).interactable) {
        SliderComponent& slider = *keyboardSlider->slider;
        const bool horizontal =
            slider.direction == SliderDirection::LeftToRight ||
            slider.direction == SliderDirection::RightToLeft;
        const bool negative =
            horizontal
                ? (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
                   (runtimeInput != nullptr &&
                    runtimeInput->IsGamepadButtonTrigger(
                        XINPUT_GAMEPAD_DPAD_LEFT)))
                : (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                   (runtimeInput != nullptr &&
                    runtimeInput->IsGamepadButtonTrigger(
                        XINPUT_GAMEPAD_DPAD_DOWN)));
        const bool positive =
            horizontal
                ? (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
                   (runtimeInput != nullptr &&
                    runtimeInput->IsGamepadButtonTrigger(
                        XINPUT_GAMEPAD_DPAD_RIGHT)))
                : (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                   (runtimeInput != nullptr &&
                    runtimeInput->IsGamepadButtonTrigger(
                        XINPUT_GAMEPAD_DPAD_UP)));
        if (negative || positive) {
            const bool reverse =
                slider.direction == SliderDirection::RightToLeft ||
                slider.direction == SliderDirection::TopToBottom;
            const float step =
                slider.wholeNumbers
                    ? 1.0f
                    : (slider.maxValue - slider.minValue) * 0.1f;
            const float visualDirection =
                positive ? 1.0f : -1.0f;
            setSliderValue(*keyboardSlider,
                           slider.value +
                               visualDirection * (reverse ? -step : step));
        }
    }
    if (canNavigateUi &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
         (runtimeInput != nullptr &&
          runtimeInput->IsGamepadButtonTrigger(
              XINPUT_GAMEPAD_B)))) {
        if (activeInputField_.IsValid()) {
            activeInputField_ = {};
            activeInputFieldEntity = nullptr;
        } else if (openDropdown_.IsValid()) {
            openDropdown_ = {};
        } else {
            focusedButton_ = {};
        }
    }

    spriteRenderer.UpdateProjection(width, height);
    spriteRenderer.PreDraw(true);
    for (const OrderedUiEntity& entry : orderedUiEntities) {
        const WorldEntity& entity = *entry.entity;
        const bool drawsText = entity.text && entity.text->enabled;
        const bool drawsImage = entity.image && entity.image->enabled;
        if (!drawsText && !drawsImage) {
            continue;
        }

        float scale = 1.0f;
        DirectX::XMFLOAT2 canvasOrigin{};
        DirectX::XMFLOAT2 referenceResolution{};
        if (!resolveCanvasLayout(entity, scale, canvasOrigin,
                                 referenceResolution)) {
            continue;
        }
        const UiGroupState groupState =
            GetUiGroupState(world_, entity);
        if (drawsImage) {
            const ImageComponent& image = *entity.image;
            const auto loaded = loadedTextures_.find(image.texturePath);
            const TextureHandle texture =
                loaded != loadedTextures_.end() &&
                        loaded->second.IsValid()
                    ? loaded->second
                    : TextureHandle{};
            const DirectX::XMFLOAT2 anchor =
                GetUiAnchorChoice(image.anchor).factor;
            Sprite sprite{};
            sprite.position = {
                canvasOrigin.x +
                    (referenceResolution.x * anchor.x + image.position.x -
                     image.size.x * image.pivot.x) *
                        scale,
                canvasOrigin.y +
                    (referenceResolution.y * anchor.y + image.position.y -
                     image.size.y * image.pivot.y) *
                        scale,
            };
            sprite.size = {
                image.size.x * scale,
                image.size.y * scale,
            };
            if (image.preserveAspect && texture.IsValid() &&
                ctx_->rendering.texture != nullptr &&
                ctx_->rendering.texture->IsValidTexture(texture)) {
                const float textureWidth = static_cast<float>(
                    ctx_->rendering.texture->GetWidth(texture));
                const float textureHeight = static_cast<float>(
                    ctx_->rendering.texture->GetHeight(texture));
                if (textureWidth > 0.0f && textureHeight > 0.0f &&
                    sprite.size.x > 0.0f && sprite.size.y > 0.0f) {
                    const DirectX::XMFLOAT2 availableSize = sprite.size;
                    const float textureAspect =
                        textureWidth / textureHeight;
                    const float availableAspect =
                        availableSize.x / availableSize.y;
                    if (availableAspect > textureAspect) {
                        sprite.size.x = availableSize.y * textureAspect;
                    } else {
                        sprite.size.y = availableSize.x / textureAspect;
                    }
                    sprite.position.x +=
                        (availableSize.x - sprite.size.x) * image.pivot.x;
                    sprite.position.y +=
                        (availableSize.y - sprite.size.y) * image.pivot.y;
                }
            }
            const Sprite sliderTrack = sprite;
            if (image.type == ImageType::Filled) {
                const float fillAmount =
                    std::clamp(image.fillAmount, 0.0f, 1.0f);
                if (image.fillMethod == ImageFillMethod::Horizontal) {
                    const float fullWidth = sprite.size.x;
                    const float fullUvWidth = sprite.uvSize.x;
                    sprite.size.x = fullWidth * fillAmount;
                    sprite.uvSize.x = fullUvWidth * fillAmount;
                    if (image.fillReverse) {
                        sprite.position.x +=
                            fullWidth * (1.0f - fillAmount);
                        sprite.uvLeftTop.x +=
                            fullUvWidth * (1.0f - fillAmount);
                    }
                } else {
                    const float fullHeight = sprite.size.y;
                    const float fullUvHeight = sprite.uvSize.y;
                    sprite.size.y = fullHeight * fillAmount;
                    sprite.uvSize.y = fullUvHeight * fillAmount;
                    if (!image.fillReverse) {
                        sprite.position.y +=
                            fullHeight * (1.0f - fillAmount);
                        sprite.uvLeftTop.y +=
                            fullUvHeight * (1.0f - fillAmount);
                    }
                }
            }
            DirectX::XMFLOAT4 stateColor{1.0f, 1.0f, 1.0f, 1.0f};
            if (entity.button && entity.button->enabled) {
                const ButtonComponent& button = *entity.button;
                const bool interactable =
                    button.interactable && groupState.interactable;
                DirectX::XMFLOAT4 targetColor =
                    interactable ? button.normalColor
                                 : button.disabledColor;
                if (interactable &&
                    (entity.id == hoveredButton ||
                     entity.id == focusedButton_)) {
                    const bool pointerPressed =
                        entity.id == hoveredButton &&
                        entity.id == pressedButton_ &&
                        ImGui::IsMouseDown(ImGuiMouseButton_Left);
                    const bool navigationPressed =
                        entity.id == focusedButton_ && submitHeld;
                    targetColor =
                        pointerPressed || navigationPressed
                            ? button.pressedColor
                            : button.hoveredColor;
                }
                ButtonColorTransition& transition =
                    buttonColorTransitions_[entity.id];
                const auto colorsEqual =
                    [](const DirectX::XMFLOAT4& lhs,
                       const DirectX::XMFLOAT4& rhs) {
                        return lhs.x == rhs.x && lhs.y == rhs.y &&
                               lhs.z == rhs.z && lhs.w == rhs.w;
                    };
                if (!transition.initialized) {
                    transition.current = targetColor;
                    transition.start = targetColor;
                    transition.target = targetColor;
                    transition.initialized = true;
                } else if (!colorsEqual(transition.target, targetColor)) {
                    transition.start = transition.current;
                    transition.target = targetColor;
                    transition.elapsed = 0.0f;
                }
                if (button.fadeDuration <= 0.0f) {
                    transition.current = targetColor;
                } else if (!colorsEqual(transition.current,
                                        transition.target)) {
                    const float deltaTime =
                        std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
                    transition.elapsed += deltaTime;
                    const float amount = std::clamp(
                        transition.elapsed / button.fadeDuration, 0.0f, 1.0f);
                    transition.current = {
                        std::lerp(transition.start.x, transition.target.x,
                                  amount),
                        std::lerp(transition.start.y, transition.target.y,
                                  amount),
                        std::lerp(transition.start.z, transition.target.z,
                                  amount),
                        std::lerp(transition.start.w, transition.target.w,
                                  amount),
                    };
                }
                stateColor = transition.current;
            }
            sprite.color = {
                image.color.x * stateColor.x,
                image.color.y * stateColor.y,
                image.color.z * stateColor.z,
                image.color.w * stateColor.w * groupState.alpha,
            };
            sprite.textureId =
                texture.IsValid()
                    ? texture.Get()
                    : kInvalidResourceId;
            spriteRenderer.Draw(sprite);
            if (entity.toggle && entity.toggle->enabled &&
                entity.toggle->isOn) {
                const ToggleComponent& toggle = *entity.toggle;
                Sprite checkmark{};
                checkmark.size = {
                    sprite.size.x * toggle.checkmarkScale,
                    sprite.size.y * toggle.checkmarkScale,
                };
                checkmark.position = {
                    sprite.position.x +
                        (sprite.size.x - checkmark.size.x) * 0.5f,
                    sprite.position.y +
                        (sprite.size.y - checkmark.size.y) * 0.5f,
                };
                checkmark.color = toggle.checkmarkColor;
                checkmark.color.w *= groupState.alpha;
                checkmark.textureId = kInvalidResourceId;
                spriteRenderer.Draw(checkmark);
            }
            if (entity.slider && entity.slider->enabled) {
                const SliderComponent& slider = *entity.slider;
                const float normalized = std::clamp(
                    (slider.value - slider.minValue) /
                        (slider.maxValue - slider.minValue),
                    0.0f, 1.0f);
                const float interactionAlpha =
                    slider.interactable && groupState.interactable
                        ? 1.0f
                        : 0.5f;
                Sprite fill = sliderTrack;
                fill.textureId = kInvalidResourceId;
                fill.color = slider.fillColor;
                fill.color.w *=
                    groupState.alpha * interactionAlpha;
                if (slider.direction ==
                        SliderDirection::LeftToRight ||
                    slider.direction ==
                        SliderDirection::RightToLeft) {
                    const float fullWidth = fill.size.x;
                    fill.size.x *= normalized;
                    if (slider.direction ==
                        SliderDirection::RightToLeft) {
                        fill.position.x +=
                            fullWidth - fill.size.x;
                    }
                } else {
                    const float fullHeight = fill.size.y;
                    fill.size.y *= normalized;
                    if (slider.direction ==
                        SliderDirection::BottomToTop) {
                        fill.position.y +=
                            fullHeight - fill.size.y;
                    }
                }
                if (fill.size.x > 0.0f && fill.size.y > 0.0f) {
                    spriteRenderer.Draw(fill);
                }

                Sprite handle{};
                const float handleSize =
                    slider.handleSize * scale;
                handle.size = {handleSize, handleSize};
                if (slider.direction ==
                        SliderDirection::LeftToRight ||
                    slider.direction ==
                        SliderDirection::RightToLeft) {
                    float position = normalized;
                    if (slider.direction ==
                        SliderDirection::RightToLeft) {
                        position = 1.0f - position;
                    }
                    handle.position = {
                        sliderTrack.position.x +
                            sliderTrack.size.x * position -
                            handleSize * 0.5f,
                        sliderTrack.position.y +
                            (sliderTrack.size.y - handleSize) * 0.5f,
                    };
                } else {
                    float position = normalized;
                    if (slider.direction ==
                        SliderDirection::BottomToTop) {
                        position = 1.0f - position;
                    }
                    handle.position = {
                        sliderTrack.position.x +
                            (sliderTrack.size.x - handleSize) * 0.5f,
                        sliderTrack.position.y +
                            sliderTrack.size.y * position -
                            handleSize * 0.5f,
                    };
                }
                handle.color = slider.handleColor;
                handle.color.w *=
                    groupState.alpha * interactionAlpha;
                handle.textureId = kInvalidResourceId;
                if (handleSize > 0.0f) {
                    spriteRenderer.Draw(handle);
                }
            }
        }
        if (!drawsText) {
            continue;
        }
        const TextComponent& text = *entity.text;
        std::string displayText = text.text;
        if (entity.inputField &&
            entity.inputField->enabled) {
            const InputFieldComponent& inputField =
                *entity.inputField;
            if (inputField.text.empty()) {
                displayText =
                    entity.id == activeInputField_
                        ? std::string{}
                        : inputField.placeholder;
            } else if (
                inputField.contentType ==
                InputFieldContentType::Password) {
                displayText.assign(
                    CountUtf8Codepoints(inputField.text), '*');
            } else {
                displayText = inputField.text;
            }
            if (entity.id == activeInputField_) {
                displayText.push_back('|');
            }
        } else if (
            entity.dropdown && entity.dropdown->enabled &&
            !entity.dropdown->options.empty() &&
            entity.dropdown->value >= 0 &&
            static_cast<size_t>(entity.dropdown->value) <
                entity.dropdown->options.size()) {
            displayText = entity.dropdown->options[
                static_cast<size_t>(
                    entity.dropdown->value)];
        }
        TextStyle style{};
        if (const auto loadedFont = loadedFonts_.find(text.fontPath);
            loadedFont != loadedFonts_.end()) {
            style.font = loadedFont->second;
        }
        style.pixelSize = text.fontSize * scale;
        style.lineSpacing = text.lineSpacing * scale;
        style.wrapWidth = text.wrapWidth * scale;
        style.horizontalAlignment =
            text.alignment == TextAlignment::Center
                ? TextHorizontalAlignment::Center
                : text.alignment == TextAlignment::Right
                      ? TextHorizontalAlignment::Right
                      : TextHorizontalAlignment::Left;
        style.color = text.color;
        style.color.w *= groupState.alpha;
        const DirectX::XMFLOAT2 anchor =
            GetUiAnchorChoice(text.anchor).factor;
        DirectX::XMFLOAT2 position{
            canvasOrigin.x +
                (referenceResolution.x * anchor.x + text.position.x) *
                    scale,
            canvasOrigin.y +
                (referenceResolution.y * anchor.y + text.position.y) *
                    scale,
        };
        if (text.alignment != TextAlignment::Left || anchor.y > 0.0f) {
            const TextLayoutMetrics metrics =
                textRenderer.MeasureText(displayText, style);
            if (text.alignment != TextAlignment::Left) {
                position.x -= text.alignment == TextAlignment::Center
                                  ? metrics.size.x * 0.5f
                                  : metrics.size.x;
            }
            position.y -= metrics.size.y * anchor.y;
        }
        textRenderer.DrawText(displayText, position, style);
    }

    openDropdownEntity = world_.Find(openDropdown_);
    if (openDropdownEntity != nullptr &&
        openDropdownEntity->dropdown &&
        openDropdownEntity->text &&
        calculateImageRect(*openDropdownEntity, dropdownLeft,
                           dropdownTop, dropdownRight,
                           dropdownBottom)) {
        const DropdownComponent& dropdown =
            *openDropdownEntity->dropdown;
        const TextComponent& text =
            *openDropdownEntity->text;
        const UiGroupState groupState =
            GetUiGroupState(world_, *openDropdownEntity);
        float popupScale = 1.0f;
        DirectX::XMFLOAT2 popupOrigin{};
        DirectX::XMFLOAT2 popupResolution{};
        resolveCanvasLayout(*openDropdownEntity, popupScale,
                            popupOrigin, popupResolution);
        TextStyle popupStyle{};
        if (const auto loadedFont =
                loadedFonts_.find(text.fontPath);
            loadedFont != loadedFonts_.end()) {
            popupStyle.font = loadedFont->second;
        }
        popupStyle.pixelSize = text.fontSize * popupScale;
        popupStyle.horizontalAlignment =
            TextHorizontalAlignment::Center;
        popupStyle.color = text.color;
        popupStyle.color.w *= groupState.alpha;
        const float rowHeight =
            dropdown.itemHeight * popupScale;
        for (size_t optionIndex = 0;
             optionIndex < dropdown.options.size();
             ++optionIndex) {
            Sprite item{};
            item.position = {
                dropdownLeft,
                dropdownBottom +
                    rowHeight *
                        static_cast<float>(optionIndex),
            };
            item.size = {
                dropdownRight - dropdownLeft,
                rowHeight,
            };
            item.color =
                static_cast<int32_t>(optionIndex) ==
                        dropdownHighlightedIndex_
                    ? dropdown.highlightedColor
                    : dropdown.itemColor;
            item.color.w *= groupState.alpha;
            item.textureId = kInvalidResourceId;
            spriteRenderer.Draw(item);

            const TextLayoutMetrics metrics =
                textRenderer.MeasureText(
                    dropdown.options[optionIndex],
                    popupStyle);
            const DirectX::XMFLOAT2 labelPosition{
                (dropdownLeft + dropdownRight -
                 metrics.size.x) *
                    0.5f,
                item.position.y +
                    (rowHeight - metrics.size.y) * 0.5f,
            };
            textRenderer.DrawText(
                dropdown.options[optionIndex],
                labelPosition, popupStyle);
        }
    }
    spriteRenderer.PostDraw();

    if (ctx_->systems.winApp != nullptr) {
        spriteRenderer.UpdateProjection(ctx_->systems.winApp->GetWidth(),
                                        ctx_->systems.winApp->GetHeight());
    }

    return hoveredButton.IsValid() ||
           hoveredDropdownOption >= 0 ||
           (dropdownWasOpen &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left));
}

void EditorScene::HandleGameUiEditing(const ImVec2& imageMin,
                                      const ImVec2& imageMax) {
    if (playModeState_ != PlayModeState::Edit || ctx_ == nullptr ||
        imageMax.x <= imageMin.x || imageMax.y <= imageMin.y) {
        gameUiDragEntity_ = {};
        gameUiResizeEntity_ = {};
        gameUiResizeHandle_ = UiResizeHandle::None;
        return;
    }

    TextRenderer* textRenderer = ctx_->rendering.text;
    const auto resolveCanvasLayout =
        [&](const WorldEntity& entity, float& scale,
            DirectX::XMFLOAT2& origin,
            DirectX::XMFLOAT2& referenceResolution) {
            const CanvasComponent* canvas =
                FindEnabledCanvas(world_, entity);
            if (canvas == nullptr) {
                return false;
            }
            const float width = imageMax.x - imageMin.x;
            const float height = imageMax.y - imageMin.y;
            CalculateCanvasLayout(*canvas, width, height, imageMin.x,
                                  imageMin.y, scale, origin,
                                  referenceResolution);
            return true;
        };

    const auto calculateRect =
        [&](const WorldEntity& entity, ImVec2& minimum,
            ImVec2& maximum) {
            if (!world_.IsActiveInHierarchy(entity.id)) {
                return false;
            }
            float scale = 1.0f;
            DirectX::XMFLOAT2 origin{};
            DirectX::XMFLOAT2 referenceResolution{};
            if (!resolveCanvasLayout(entity, scale, origin,
                                     referenceResolution)) {
                return false;
            }
            bool hasRect = false;
            const auto includeRect = [&](const ImVec2& rectMin,
                                         const ImVec2& rectMax) {
                if (!hasRect) {
                    minimum = rectMin;
                    maximum = rectMax;
                    hasRect = true;
                    return;
                }
                minimum.x = (std::min)(minimum.x, rectMin.x);
                minimum.y = (std::min)(minimum.y, rectMin.y);
                maximum.x = (std::max)(maximum.x, rectMax.x);
                maximum.y = (std::max)(maximum.y, rectMax.y);
            };
            if (entity.image && entity.image->enabled) {
                const ImageComponent& image = *entity.image;
                const DirectX::XMFLOAT2 anchor =
                    GetUiAnchorChoice(image.anchor).factor;
                const ImVec2 rectMin{
                    origin.x +
                        (referenceResolution.x * anchor.x +
                         image.position.x - image.size.x * image.pivot.x) *
                            scale,
                    origin.y +
                        (referenceResolution.y * anchor.y +
                         image.position.y - image.size.y * image.pivot.y) *
                            scale,
                };
                includeRect(
                    rectMin,
                    {rectMin.x + image.size.x * scale,
                     rectMin.y + image.size.y * scale});
            }
            if (entity.text && entity.text->enabled &&
                textRenderer != nullptr && textRenderer->IsReady()) {
                const TextComponent& text = *entity.text;
                TextStyle style{};
                if (const auto loadedFont =
                        loadedFonts_.find(text.fontPath);
                    loadedFont != loadedFonts_.end()) {
                    style.font = loadedFont->second;
                }
                style.pixelSize = text.fontSize * scale;
                style.lineSpacing = text.lineSpacing * scale;
                style.wrapWidth = text.wrapWidth * scale;
                const TextLayoutMetrics metrics =
                    textRenderer->MeasureText(text.text, style);
                const DirectX::XMFLOAT2 anchor =
                    GetUiAnchorChoice(text.anchor).factor;
                ImVec2 rectMin{
                    origin.x +
                        (referenceResolution.x * anchor.x +
                         text.position.x) *
                            scale,
                    origin.y +
                        (referenceResolution.y * anchor.y +
                         text.position.y) *
                            scale -
                        metrics.size.y * anchor.y,
                };
                if (text.alignment == TextAlignment::Center) {
                    rectMin.x -= metrics.size.x * 0.5f;
                } else if (text.alignment == TextAlignment::Right) {
                    rectMin.x -= metrics.size.x;
                }
                includeRect(
                    rectMin,
                    {rectMin.x + metrics.size.x,
                     rectMin.y + metrics.size.y});
            }
            return hasRect;
        };

    const auto calculateImageRect =
        [&](const WorldEntity& entity, ImVec2& minimum,
            ImVec2& maximum, float* canvasScale = nullptr) {
            if (!world_.IsActiveInHierarchy(entity.id) || !entity.image ||
                !entity.image->enabled) {
                return false;
            }
            float scale = 1.0f;
            DirectX::XMFLOAT2 origin{};
            DirectX::XMFLOAT2 referenceResolution{};
            if (!resolveCanvasLayout(entity, scale, origin,
                                     referenceResolution)) {
                return false;
            }
            const ImageComponent& image = *entity.image;
            const DirectX::XMFLOAT2 anchor =
                GetUiAnchorChoice(image.anchor).factor;
            minimum = {
                origin.x +
                    (referenceResolution.x * anchor.x +
                     image.position.x - image.size.x * image.pivot.x) *
                        scale,
                origin.y +
                    (referenceResolution.y * anchor.y +
                     image.position.y - image.size.y * image.pivot.y) *
                        scale,
            };
            maximum = {
                minimum.x + image.size.x * scale,
                minimum.y + image.size.y * scale,
            };
            if (canvasScale != nullptr) {
                *canvasScale = scale;
            }
            return true;
        };

    const ImVec2 mouse = ImGui::GetMousePos();
    const bool imageHovered = ImGui::IsItemHovered();
    EntityId hovered{};
    if (imageHovered) {
        for (const OrderedUiEntity& entry : GetOrderedUiEntities(world_)) {
            const WorldEntity& entity = *entry.entity;
            ImVec2 minimum{};
            ImVec2 maximum{};
            if (calculateRect(entity, minimum, maximum) &&
                mouse.x >= minimum.x && mouse.x <= maximum.x &&
                mouse.y >= minimum.y && mouse.y <= maximum.y) {
                hovered = entity.id;
            }
        }
    }

    const WorldEntity* selectedBeforeInput = world_.Find(selection_);
    ImVec2 selectedImageMin{};
    ImVec2 selectedImageMax{};
    const bool selectedHasImage =
        selectedBeforeInput != nullptr &&
        calculateImageRect(*selectedBeforeInput, selectedImageMin,
                           selectedImageMax);
    constexpr float kResizeHandleRadius = 6.0f;
    constexpr float kResizeHitRadius = 9.0f;
    UiResizeHandle hoveredResizeHandle = UiResizeHandle::None;
    if (imageHovered && selectedHasImage) {
        const auto hitHandle = [&](const ImVec2& position) {
            return std::abs(mouse.x - position.x) <= kResizeHitRadius &&
                   std::abs(mouse.y - position.y) <= kResizeHitRadius;
        };
        if (hitHandle(selectedImageMin)) {
            hoveredResizeHandle = UiResizeHandle::TopLeft;
        } else if (hitHandle(
                       {selectedImageMax.x, selectedImageMin.y})) {
            hoveredResizeHandle = UiResizeHandle::TopRight;
        } else if (hitHandle(
                       {selectedImageMin.x, selectedImageMax.y})) {
            hoveredResizeHandle = UiResizeHandle::BottomLeft;
        } else if (hitHandle(selectedImageMax)) {
            hoveredResizeHandle = UiResizeHandle::BottomRight;
        }
    }

    if (imageHovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoveredResizeHandle != UiResizeHandle::None) {
            gameUiResizeEntity_ = selection_;
            gameUiResizeHandle_ = hoveredResizeHandle;
            gameUiDragEntity_ = {};
            BeginHistoryEdit("Resize UI Image");
        } else if (hovered.IsValid()) {
            SelectHierarchyEntity(hovered, false, false);
            gameUiDragEntity_ = hovered;
            gameUiResizeEntity_ = {};
            gameUiResizeHandle_ = UiResizeHandle::None;
            BeginHistoryEdit("Move UI Element");
        } else {
            ClearHierarchySelection();
            gameUiDragEntity_ = {};
            gameUiResizeEntity_ = {};
            gameUiResizeHandle_ = UiResizeHandle::None;
        }
    }

    if (gameUiResizeEntity_.IsValid() &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        WorldEntity* entity = world_.Find(gameUiResizeEntity_);
        if (entity != nullptr && entity->image &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            float scale = 1.0f;
            ImVec2 rectMin{};
            ImVec2 rectMax{};
            if (calculateImageRect(*entity, rectMin, rectMax, &scale) &&
                scale > 0.0f) {
                const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
                ImageComponent& image = *entity->image;
                const DirectX::XMFLOAT2 previousSize = image.size;
                const float horizontalDelta = mouseDelta.x / scale;
                const float verticalDelta = mouseDelta.y / scale;
                const bool resizesLeft =
                    gameUiResizeHandle_ == UiResizeHandle::TopLeft ||
                    gameUiResizeHandle_ == UiResizeHandle::BottomLeft;
                const bool resizesTop =
                    gameUiResizeHandle_ == UiResizeHandle::TopLeft ||
                    gameUiResizeHandle_ == UiResizeHandle::TopRight;
                image.size.x = std::clamp(
                    image.size.x +
                        (resizesLeft ? -horizontalDelta
                                     : horizontalDelta),
                    1.0f, 1000000.0f);
                image.size.y = std::clamp(
                    image.size.y +
                        (resizesTop ? -verticalDelta : verticalDelta),
                    1.0f, 1000000.0f);
                const DirectX::XMFLOAT2 sizeDelta{
                    image.size.x - previousSize.x,
                    image.size.y - previousSize.y,
                };
                const DirectX::XMFLOAT2 minimumDelta{
                    resizesLeft ? -sizeDelta.x : 0.0f,
                    resizesTop ? -sizeDelta.y : 0.0f,
                };
                const DirectX::XMFLOAT2 positionDelta{
                    minimumDelta.x + sizeDelta.x * image.pivot.x,
                    minimumDelta.y + sizeDelta.y * image.pivot.y,
                };
                image.position.x = std::clamp(
                    image.position.x + positionDelta.x, -1000000.0f,
                    1000000.0f);
                image.position.y = std::clamp(
                    image.position.y + positionDelta.y, -1000000.0f,
                    1000000.0f);
                if (entity->text) {
                    const DirectX::XMFLOAT2 centerDelta{
                        minimumDelta.x + sizeDelta.x * 0.5f,
                        minimumDelta.y + sizeDelta.y * 0.5f,
                    };
                    entity->text->position.x = std::clamp(
                        entity->text->position.x + centerDelta.x,
                        -1000000.0f, 1000000.0f);
                    entity->text->position.y = std::clamp(
                        entity->text->position.y + centerDelta.y,
                        -1000000.0f, 1000000.0f);
                }
                RefreshDirty();
                status_ = "Resized UI Image in the Game View.";
            }
        }
    } else if (gameUiResizeEntity_.IsValid()) {
        CommitHistoryEdit();
        gameUiResizeEntity_ = {};
        gameUiResizeHandle_ = UiResizeHandle::None;
    } else if (gameUiDragEntity_.IsValid() &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        WorldEntity* entity = world_.Find(gameUiDragEntity_);
        if (entity != nullptr &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            float scale = 1.0f;
            DirectX::XMFLOAT2 origin{};
            DirectX::XMFLOAT2 referenceResolution{};
            if (resolveCanvasLayout(*entity, scale, origin,
                                    referenceResolution) &&
                scale > 0.0f) {
                const ImVec2 delta = ImGui::GetIO().MouseDelta;
                if (entity->image) {
                    entity->image->position.x = std::clamp(
                        entity->image->position.x + delta.x / scale,
                        -1000000.0f, 1000000.0f);
                    entity->image->position.y = std::clamp(
                        entity->image->position.y + delta.y / scale,
                        -1000000.0f, 1000000.0f);
                }
                if (entity->text) {
                    entity->text->position.x = std::clamp(
                        entity->text->position.x + delta.x / scale,
                        -1000000.0f, 1000000.0f);
                    entity->text->position.y = std::clamp(
                        entity->text->position.y + delta.y / scale,
                        -1000000.0f, 1000000.0f);
                }
                RefreshDirty();
                status_ = "Moved UI element in the Game View.";
            }
        }
    } else if (gameUiDragEntity_.IsValid()) {
        CommitHistoryEdit();
        gameUiDragEntity_ = {};
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (!gameUiDragEntity_.IsValid() &&
        !gameUiResizeEntity_.IsValid() &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt) {
        DirectX::XMFLOAT2 nudge{};
        const float distance = io.KeyShift ? 10.0f : 1.0f;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
            nudge.x -= distance;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
            nudge.x += distance;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
            nudge.y -= distance;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
            nudge.y += distance;
        }
        WorldEntity* entity = world_.Find(selection_);
        ImVec2 selectedRectMin{};
        ImVec2 selectedRectMax{};
        if ((nudge.x != 0.0f || nudge.y != 0.0f) &&
            entity != nullptr &&
            calculateRect(*entity, selectedRectMin, selectedRectMax)) {
            const std::string before = WorldSerializer::Serialize(world_);
            const auto movePosition =
                [&nudge](DirectX::XMFLOAT2& position) {
                    position.x = std::clamp(
                        position.x + nudge.x, -1000000.0f, 1000000.0f);
                    position.y = std::clamp(
                        position.y + nudge.y, -1000000.0f, 1000000.0f);
                };
            if (entity->image) {
                movePosition(entity->image->position);
            }
            if (entity->text) {
                movePosition(entity->text->position);
            }
            RecordImmediateEdit("Nudge UI Element", before, selection_);
            status_ = io.KeyShift
                          ? "Moved UI element by 10 pixels."
                          : "Moved UI element by 1 pixel.";
        }
    }

    const WorldEntity* selected = world_.Find(selection_);
    ImVec2 selectedMin{};
    ImVec2 selectedMax{};
    if (selected != nullptr &&
        calculateRect(*selected, selectedMin, selectedMax)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(imageMin, imageMax, true);
        drawList->AddRect(selectedMin, selectedMax,
                          IM_COL32(255, 190, 60, 255), 0.0f, 0, 2.0f);
        ImVec2 imageRectMin{};
        ImVec2 imageRectMax{};
        if (calculateImageRect(*selected, imageRectMin, imageRectMax)) {
            const std::array<ImVec2, 4> handles{
                imageRectMin,
                ImVec2{imageRectMax.x, imageRectMin.y},
                ImVec2{imageRectMin.x, imageRectMax.y},
                imageRectMax,
            };
            for (const ImVec2& handle : handles) {
                drawList->AddRectFilled(
                    {handle.x - kResizeHandleRadius,
                     handle.y - kResizeHandleRadius},
                    {handle.x + kResizeHandleRadius,
                     handle.y + kResizeHandleRadius},
                    IM_COL32(255, 190, 60, 255));
            }
        }
        drawList->PopClipRect();
    }
    const UiResizeHandle cursorHandle =
        gameUiResizeEntity_.IsValid() ? gameUiResizeHandle_
                                      : hoveredResizeHandle;
    if (cursorHandle == UiResizeHandle::TopLeft ||
        cursorHandle == UiResizeHandle::BottomRight) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    } else if (cursorHandle == UiResizeHandle::TopRight ||
               cursorHandle == UiResizeHandle::BottomLeft) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    } else if (gameUiDragEntity_.IsValid()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else if (hovered.IsValid()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
}

void EditorScene::BuildRenderScene() {
    renderScene_.BeginFrame();
    ModelManager* models = ctx_ ? ctx_->rendering.model : nullptr;
    if (models == nullptr) {
        return;
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (!world_.IsActiveInHierarchy(entity.id) || !entity.meshRenderer ||
            !entity.meshRenderer->enabled ||
            !entity.materialOverride || !entity.materialOverride->enabled) {
            continue;
        }
        const auto runtimeAnimator = std::ranges::find_if(
            runtimeAnimators_, [&entity](const RuntimeAnimator& runtime) {
                return runtime.entity == entity.id;
            });
        const bool runtimeAnimated = runtimeAnimator != runtimeAnimators_.end();
        const bool editPreviewAnimated =
            !runtimeAnimated && editAnimatorPreviewEntity_ == entity.id &&
            editAnimatorPreviewModel_.IsValid();
        const bool animated = runtimeAnimated || editPreviewAnimated;
        const ModelHandle handle =
            runtimeAnimated       ? runtimeAnimator->model
            : editPreviewAnimated ? editAnimatorPreviewModel_
                                  : ResolveModel(*entity.meshRenderer);
        const Model* model = handle.IsValid() ? models->GetModel(handle) : nullptr;
        DirectX::XMFLOAT4X4 worldMatrix{};
        if (model == nullptr || !world_.TryGetWorldMatrix(entity.id, worldMatrix)) {
            continue;
        }
        if (animated) {
            models->PrepareSkinning(handle);
        }
        DirectX::XMMATRIX renderWorld = DirectX::XMLoadFloat4x4(&worldMatrix);
        if (animated && model->hasRootAnimation) {
            renderWorld = DirectX::XMLoadFloat4x4(&model->rootAnimationMatrix) * renderWorld;
        }
        DirectX::XMStoreFloat4x4(&worldMatrix, renderWorld);
        const Transform transform = DecomposeTransform(worldMatrix);
        auto submit = [&](uint32_t meshId, uint32_t materialId, uint32_t textureId,
                          uint32_t normalTextureId,
                          const D3D12_VERTEX_BUFFER_VIEW* vertexBufferOverride = nullptr) {
            if (!IsValidResourceId(meshId)) {
                return;
            }
            RenderMeshItem item{};
            item.mesh = &models->GetMesh(meshId);
            if (IsValidResourceId(materialId)) {
                item.material = models->GetMaterial(materialId);
            }
            if (entity.materialOverride && entity.materialOverride->enabled) {
                item.material.color = entity.materialOverride->baseColor;
                item.material.metallic = entity.materialOverride->metallic;
                item.material.roughness = entity.materialOverride->roughness;
                item.material.normalStrength = entity.materialOverride->normalStrength;
                switch (entity.materialOverride->blendMode) {
                case MaterialSurfaceBlendMode::Opaque:
                    item.material.blendMode = static_cast<int32_t>(BlendMode::Opaque);
                    break;
                case MaterialSurfaceBlendMode::Cutout:
                    item.material.blendMode = static_cast<int32_t>(BlendMode::Cutout);
                    break;
                case MaterialSurfaceBlendMode::Transparent:
                    item.material.blendMode = static_cast<int32_t>(BlendMode::Transparent);
                    break;
                }
                item.material.alphaCutoff = entity.materialOverride->alphaCutoff;
                switch (entity.materialOverride->cullMode) {
                case MaterialSurfaceCullMode::None:
                    item.material.cullMode = static_cast<int32_t>(MaterialCullMode::None);
                    break;
                case MaterialSurfaceCullMode::Front:
                    item.material.cullMode = static_cast<int32_t>(MaterialCullMode::Front);
                    break;
                case MaterialSurfaceCullMode::Back:
                    item.material.cullMode = static_cast<int32_t>(MaterialCullMode::Back);
                    break;
                }
                item.material.depthWrite = entity.materialOverride->depthWrite ? 1 : 0;
                const TextureHandle overrideTexture =
                    ResolveBaseColorTexture(*entity.materialOverride);
                if (overrideTexture.IsValid()) {
                    item.textureId = overrideTexture.Get();
                    item.material.baseColorTextureId = overrideTexture.Get();
                    item.material.enableTexture = 1;
                }
                const TextureHandle normalTexture =
                    ResolveNormalTexture(*entity.materialOverride);
                if (normalTexture.IsValid()) {
                    item.normalTextureId = normalTexture.Get();
                    item.material.normalTextureId = normalTexture.Get();
                    item.material.enableNormalMap = 1;
                }
                const TextureHandle roughnessTexture =
                    ResolveLinearTexture(entity.materialOverride->roughnessTexturePath);
                const TextureHandle metallicTexture =
                    ResolveLinearTexture(entity.materialOverride->metallicTexturePath);
                if (roughnessTexture.IsValid()) {
                    item.material.roughnessTextureId = roughnessTexture.Get();
                }
                if (metallicTexture.IsValid()) {
                    item.material.metallicTextureId = metallicTexture.Get();
                }
                switch (entity.materialOverride->pbrTexturePacking) {
                case MaterialPbrTexturePacking::Separate:
                    item.material.pbrTexturePacking =
                        static_cast<int32_t>(PbrTexturePacking::Separate);
                    break;
                case MaterialPbrTexturePacking::OcclusionRoughnessMetallic:
                    item.material.pbrTexturePacking =
                        static_cast<int32_t>(PbrTexturePacking::OcclusionRoughnessMetallic);
                    break;
                case MaterialPbrTexturePacking::MetallicRoughness:
                    item.material.pbrTexturePacking =
                        static_cast<int32_t>(PbrTexturePacking::MetallicRoughness);
                    break;
                }
            }
            item.transform = transform;
            if (!IsValidResourceId(item.textureId)) {
                item.textureId = textureId;
            }
            if (!IsValidResourceId(item.normalTextureId)) {
                item.normalTextureId = normalTextureId;
            }
            item.objectId = static_cast<uint32_t>(EntityIdHash{}(entity.id));
            if (vertexBufferOverride != nullptr) {
                item.vertexBufferOverride = *vertexBufferOverride;
            }
            renderScene_.SubmitMesh(item);
        };
        if (!model->subMeshes.empty()) {
            for (const ModelSubMesh& subMesh : model->subMeshes) {
                const D3D12_VERTEX_BUFFER_VIEW* animatedVertices =
                    animated && subMesh.skinCluster.skinnedVertexResource
                        ? &subMesh.skinCluster.skinnedVertexBufferView
                        : nullptr;
                submit(subMesh.meshId, subMesh.materialId, subMesh.textureId,
                       subMesh.normalTextureId, animatedVertices);
            }
        } else {
            submit(model->meshId, model->materialId, model->textureId, kInvalidResourceId);
        }
    }
}

void EditorScene::BuildEditorOverlayScene() {
    editorOverlayScene_.BeginFrame();
    if (!showSceneGrid_ || !IsValidResourceId(sceneGridPipelineId_) || ctx_ == nullptr ||
        ctx_->rendering.model == nullptr) {
        return;
    }
    ModelManager* models = ctx_->rendering.model;
    const ModelHandle planeHandle =
        primitiveModels_[static_cast<size_t>(MeshPrimitive::Plane)];
    const Model* plane = planeHandle.IsValid() ? models->GetModel(planeHandle) : nullptr;
    if (plane == nullptr || !IsValidResourceId(plane->meshId)) {
        return;
    }

    RenderMeshItem grid{};
    grid.mesh = &models->GetMesh(plane->meshId);
    grid.material.color = {1.0f, 1.0f, 1.0f, 0.45f};
    grid.material.enableTexture = 0;
    grid.material.blendMode = static_cast<int32_t>(BlendMode::Transparent);
    grid.material.cullMode = static_cast<int32_t>(MaterialCullMode::None);
    grid.material.depthWrite = 0;
    grid.transform.scale = {100.0f, 100.0f, 1.0f};
    DirectX::XMStoreFloat4(
        &grid.transform.rotation,
        DirectX::XMQuaternionRotationAxis(DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
                                         -DirectX::XM_PIDIV2));
    grid.pipelineId = sceneGridPipelineId_;
    grid.flags = RenderObjectFlags::Transparent;
    editorOverlayScene_.SubmitMesh(grid);
}

void EditorScene::PickSceneEntity(const ImVec2& imageMin, const ImVec2& imageMax,
                                  bool imageHovered) {
    if (!imageHovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        return;
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    EntityId closestComponent{};
    float closestComponentDistanceSquared = 14.0f * 14.0f;
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.camera && !entity.light && !entity.audioSource && !entity.audioListener &&
            !entity.boxCollider && !entity.characterController) {
            continue;
        }
        DirectX::XMFLOAT4X4 worldMatrix{};
        ImVec2 screenPosition{};
        if (!world_.TryGetWorldMatrix(entity.id, worldMatrix) ||
            !ProjectScenePoint(sceneViewCamera_,
                               {worldMatrix._41, worldMatrix._42, worldMatrix._43}, imageMin,
                               imageMax, screenPosition)) {
            continue;
        }
        const float deltaX = mouse.x - screenPosition.x;
        const float deltaY = mouse.y - screenPosition.y;
        const float distanceSquared = deltaX * deltaX + deltaY * deltaY;
        if (distanceSquared <= closestComponentDistanceSquared) {
            closestComponent = entity.id;
            closestComponentDistanceSquared = distanceSquared;
        }
    }
    if (closestComponent.IsValid()) {
        const ImGuiIO& io = ImGui::GetIO();
        SelectHierarchyEntity(closestComponent, io.KeyCtrl, false);
        if (selection_ == closestComponent &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            FocusSceneCameraOnSelection();
        }
        return;
    }
    if (ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        if (!ImGui::GetIO().KeyCtrl) {
            ClearHierarchySelection();
        }
        return;
    }
    using namespace DirectX;
    XMVECTOR nearPoint{};
    XMVECTOR rayDirection{};
    if (!BuildSceneRay(sceneViewCamera_, imageMin, imageMax, ImGui::GetMousePos(), nearPoint,
                       rayDirection)) {
        return;
    }

    EntityId closest{};
    float closestDistance = (std::numeric_limits<float>::max)();
    ModelManager* models = ctx_->rendering.model;
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.meshRenderer || !entity.meshRenderer->enabled) {
            continue;
        }
        const ModelHandle handle = ResolveModel(*entity.meshRenderer);
        const Model* model = handle.IsValid() ? models->GetModel(handle) : nullptr;
        XMFLOAT3 boundsMin{};
        XMFLOAT3 boundsMax{};
        XMFLOAT4X4 worldMatrix{};
        if (model == nullptr || !TryGetModelBounds(*model, boundsMin, boundsMax) ||
            !world_.TryGetWorldMatrix(entity.id, worldMatrix)) {
            continue;
        }
        XMVECTOR determinant{};
        const XMMATRIX inverseWorld = XMMatrixInverse(&determinant, XMLoadFloat4x4(&worldMatrix));
        const float determinantValue = XMVectorGetX(determinant);
        if (!std::isfinite(determinantValue) || std::abs(determinantValue) < 1.0e-8f) {
            continue;
        }
        const XMVECTOR localOrigin = XMVector3TransformCoord(nearPoint, inverseWorld);
        const XMVECTOR localDirection = XMVector3TransformNormal(rayDirection, inverseWorld);
        float hitDistance = 0.0f;
        if (IntersectRayBounds(localOrigin, localDirection, boundsMin, boundsMax, hitDistance) &&
            hitDistance < closestDistance) {
            closest = entity.id;
            closestDistance = hitDistance;
        }
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (closest.IsValid()) {
        SelectHierarchyEntity(closest, io.KeyCtrl, false);
    } else if (!io.KeyCtrl) {
        ClearHierarchySelection();
    }
    if (closest.IsValid() && selection_ == closest &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        FocusSceneCameraOnSelection();
    }
}

void EditorScene::DrawSceneComponentGizmos(const ImVec2& imageMin,
                                           const ImVec2& imageMax) const {
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
        ImU32 color = entity.camera
                          ? IM_COL32(90, 185, 255, 230)
                          : (entity.light
                                 ? IM_COL32(255, 215, 80, 230)
                                 : (entity.audioSource
                                        ? IM_COL32(190, 120, 255, 230)
                                        : (entity.audioListener
                                               ? IM_COL32(80, 215, 230, 230)
                                               : IM_COL32(80, 230, 130, 230))));
        const bool physicsLayerVisible =
            (physicsDebugLayerMask_ & (uint32_t{1} << entity.layer)) != 0u;
        const bool entityActive = world_.IsActiveInHierarchy(entity.id);
        const bool drawPhysicsShapes = showPhysicsDebug_ && entityActive &&
                                       physicsLayerVisible &&
                                       (entity.boxCollider || entity.characterController);
        if (active) {
            color = IM_COL32(255, 184, 56, 255);
        } else if (selected) {
            color = IM_COL32(90, 190, 255, 255);
        }
        const bool enabled = entityActive &&
                             ((entity.camera && entity.camera->enabled) ||
                               (entity.light && entity.light->enabled) ||
                               (entity.audioSource && entity.audioSource->enabled) ||
                               (entity.audioListener && entity.audioListener->enabled) ||
                               (entity.boxCollider && entity.boxCollider->enabled) ||
                               (entity.characterController &&
                                entity.characterController->enabled));
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
                drawList->AddLine({center.x + direction.x * 7.0f,
                                   center.y + direction.y * 7.0f},
                                  {center.x + direction.x * 11.0f,
                                   center.y + direction.y * 11.0f},
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
                              {center.x + 6.0f, center.y + 6.0f}, color, 1.0f, 0,
                              1.8f);
        }
        if (active || drawPhysicsShapes) {
            using namespace DirectX;
            const XMMATRIX world = XMLoadFloat4x4(&worldMatrix);
            const XMVECTOR origin = XMVectorSet(worldMatrix._41, worldMatrix._42,
                                                worldMatrix._43, 1.0f);
            auto normalizedAxis = [&](float x, float y, float z) {
                XMVECTOR axis = XMVector3TransformNormal(XMVectorSet(x, y, z, 0.0f), world);
                return XMVectorGetX(XMVector3LengthSq(axis)) > 1.0e-8f
                           ? XMVector3Normalize(axis)
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
                const float aspect = static_cast<float>((std::max)(1, gameViewSurface_.GetWidth())) /
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
                const ImU32 guideColor = camera.enabled ? IM_COL32(90, 185, 255, 190)
                                                        : IM_COL32(90, 185, 255, 80);
                for (const auto& edge : edges) {
                    drawWorldLine(corners[edge[0]], corners[edge[1]], guideColor);
                }
            }

            if (active && entity.light) {
                const LightComponent& light = *entity.light;
                const ImU32 guideColor = light.enabled ? IM_COL32(255, 215, 80, 190)
                                                       : IM_COL32(255, 215, 80, 80);
                const XMFLOAT3 worldOrigin = worldPoint(0.0f, 0.0f, 0.0f);
                auto drawCircle = [&](float radius, XMVECTOR axisA, XMVECTOR axisB,
                                      XMVECTOR circleCenter = DirectX::g_XMZero) {
                    constexpr int segments = 32;
                    XMFLOAT3 previous{};
                    for (int index = 0; index <= segments; ++index) {
                        const float angle = XM_2PI * static_cast<float>(index) /
                                            static_cast<float>(segments);
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
                    const float coneRadius =
                        light.range * std::tan(XMConvertToRadians(guideAngle));
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
                const ImU32 minColor = sourceEnabled ? IM_COL32(220, 155, 255, 220)
                                                     : IM_COL32(220, 155, 255, 80);
                const ImU32 maxColor = sourceEnabled ? IM_COL32(155, 95, 255, 150)
                                                     : IM_COL32(155, 95, 255, 60);
                auto drawRangeCircle = [&](float radius, XMVECTOR axisA, XMVECTOR axisB,
                                           ImU32 guideColor, float thickness) {
                    constexpr int segments = 48;
                    XMFLOAT3 previous{};
                    for (int index = 0; index <= segments; ++index) {
                        const float angle = XM_2PI * static_cast<float>(index) /
                                            static_cast<float>(segments);
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
                    const XMVECTOR colliderRight = XMVector3Rotate(g_XMIdentityR0,
                                                                    colliderRotation);
                    const XMVECTOR colliderUp = XMVector3Rotate(g_XMIdentityR1,
                                                                 colliderRotation);
                    const XMVECTOR colliderForward = XMVector3Rotate(g_XMIdentityR2,
                                                                      colliderRotation);
                    const float halfX = collider.size.x * 0.5f;
                    const float halfY = collider.size.y * 0.5f;
                    const float halfZ = collider.size.z * 0.5f;
                    std::array<XMFLOAT3, 8> corners{};
                    size_t cornerIndex = 0;
                    for (int z = -1; z <= 1; z += 2) {
                        for (int y = -1; y <= 1; y += 2) {
                            for (int x = -1; x <= 1; x += 2) {
                                XMStoreFloat3(
                                    &corners[cornerIndex++],
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
                    const ImU32 guideColor = entity.boxCollider->isTrigger
                                                 ? (entity.boxCollider->enabled
                                                        ? IM_COL32(255, 170, 70, 220)
                                                        : IM_COL32(255, 170, 70, 80))
                                             : (!active && showPhysicsDebug_)
                                                 ? PhysicsDebugLayerColor(
                                                       entity.layer,
                                                       entity.boxCollider->enabled)
                                                 : (entity.boxCollider->enabled
                                                        ? IM_COL32(80, 230, 130, 210)
                                                        : IM_COL32(80, 230, 130, 80));
                    for (const auto& edge : colliderEdges) {
                        drawWorldLine(corners[edge[0]], corners[edge[1]], guideColor,
                                      1.5f);
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
                            : (entity.characterController->enabled
                                   ? IM_COL32(70, 220, 210, 220)
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
                            const float angle = XM_2PI * static_cast<float>(index) /
                                                static_cast<float>(segments);
                            const XMFLOAT3 point =
                                capsulePoint(std::cos(angle) * capsule.radius, y,
                                             std::sin(angle) * capsule.radius);
                            drawWorldLine(previous, point, guideColor, 1.5f);
                            previous = point;
                        }
                    }
                    drawWorldLine(capsulePoint(capsule.radius, -segmentHalfHeight, 0.0f),
                                  capsulePoint(capsule.radius, segmentHalfHeight, 0.0f),
                                  guideColor, 1.5f);
                    drawWorldLine(capsulePoint(-capsule.radius, -segmentHalfHeight, 0.0f),
                                  capsulePoint(-capsule.radius, segmentHalfHeight, 0.0f),
                                  guideColor, 1.5f);
                    drawWorldLine(capsulePoint(0.0f, -segmentHalfHeight, capsule.radius),
                                  capsulePoint(0.0f, segmentHalfHeight, capsule.radius),
                                  guideColor, 1.5f);
                    drawWorldLine(capsulePoint(0.0f, -segmentHalfHeight, -capsule.radius),
                                  capsulePoint(0.0f, segmentHalfHeight, -capsule.radius),
                                  guideColor, 1.5f);
                    auto drawCapArc = [&](bool xPlane, bool top) {
                        const float baseY = top ? segmentHalfHeight : -segmentHalfHeight;
                        const float angleStart = top ? 0.0f : XM_PI;
                        XMFLOAT3 previous = xPlane
                                                ? capsulePoint(capsule.radius, baseY, 0.0f)
                                                : capsulePoint(0.0f, baseY, capsule.radius);
                        if (!top) {
                            previous = xPlane
                                           ? capsulePoint(-capsule.radius, baseY, 0.0f)
                                           : capsulePoint(0.0f, baseY, -capsule.radius);
                        }
                        for (int index = 1; index <= segments; ++index) {
                            const float angle = angleStart + XM_PI *
                                static_cast<float>(index) / static_cast<float>(segments);
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

void EditorScene::DrawSceneSelectionOutline(const ImVec2& imageMin,
                                            const ImVec2& imageMax) const {
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
        const ImU32 outlineColor = active ? IM_COL32(255, 184, 56, 255)
                                          : IM_COL32(90, 190, 255, 220);
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
        labelPosition.y =
            std::clamp(labelPosition.y - textSize.y - 8.0f, labelMinY, labelMaxY);
        drawList->AddRectFilled(labelPosition,
                                {labelPosition.x + textSize.x + 6.0f,
                                 labelPosition.y + textSize.y + 4.0f},
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
            const auto color = ImGui::ColorConvertU32ToFloat4(
                PhysicsDebugLayerColor(static_cast<uint8_t>(layer)));
            ImGui::TextColored(color, "\u25a0");
            ImGui::SameLine();
            bool visible = (physicsDebugLayerMask_ & (uint32_t{1} << layer)) != 0u;
            const std::string label = std::to_string(layer) + ": " +
                                      physicsSettings_.layerNames[layer];
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

bool EditorScene::DrawBoxColliderGizmo(const ImVec2& imageMin,
                                       const ImVec2& imageMax) {
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
                        XMMatrixAffineTransformation(XMVectorReplicate(1.0f),
                                                     XMVectorZero(), colliderRotation,
                                                     colliderCenter));
    } else {
        XMStoreFloat4x4(&gizmoMatrix,
                        XMMatrixScaling(worldCollider.size.x, worldCollider.size.y,
                                        worldCollider.size.z) *
                            XMMatrixRotationQuaternion(colliderRotation) *
                            XMMatrixTranslationFromVector(colliderCenter));
    }

    XMFLOAT4X4 view{};
    XMFLOAT4X4 projection{};
    XMStoreFloat4x4(&view, sceneViewCamera_.GetView());
    XMStoreFloat4x4(&projection, sceneViewCamera_.GetProj());
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x,
                      imageMax.y - imageMin.y);
    ImGuizmo::SetOrthographic(false);

    const ImGuizmo::OPERATION operation =
        boxColliderGizmoMode_ == BoxColliderGizmoMode::Center ? ImGuizmo::TRANSLATE
                                                              : ImGuizmo::SCALE;
    float snapValues[3]{};
    std::ranges::fill(snapValues, boxColliderGizmoMode_ == BoxColliderGizmoMode::Center
                                       ? translationSnap_
                                       : scaleSnap_);
    const bool snapActive = gizmoSnapEnabled_ || ImGui::GetIO().KeyCtrl;
    const bool manipulated = ImGuizmo::Manipulate(
        &view._11, &projection._11, operation, ImGuizmo::LOCAL, &gizmoMatrix._11, nullptr,
        snapActive ? snapValues : nullptr);
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
bool EditorScene::DrawCharacterControllerGizmo(const ImVec2& imageMin,
                                               const ImVec2& imageMax) {
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
        XMStoreFloat4x4(
            &gizmoMatrix,
            XMMatrixTranslation(worldCapsule.center.x, worldCapsule.center.y,
                                worldCapsule.center.z));
    } else {
        XMStoreFloat4x4(
            &gizmoMatrix,
            XMMatrixScaling(worldDiameter, worldCapsule.height, worldDiameter) *
                XMMatrixTranslation(worldCapsule.center.x, worldCapsule.center.y,
                                    worldCapsule.center.z));
    }

    XMFLOAT4X4 view{};
    XMFLOAT4X4 projection{};
    XMStoreFloat4x4(&view, sceneViewCamera_.GetView());
    XMStoreFloat4x4(&projection, sceneViewCamera_.GetProj());
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x,
                      imageMax.y - imageMin.y);
    ImGuizmo::SetOrthographic(false);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Radius) {
        operation = ImGuizmo::SCALE_X | ImGuizmo::SCALE_Z;
    } else if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Height) {
        operation = ImGuizmo::SCALE_Y;
    }
    float snapValues[3]{};
    std::ranges::fill(
        snapValues,
        characterControllerGizmoMode_ == CharacterControllerGizmoMode::Center
            ? translationSnap_
            : scaleSnap_);
    const bool snapActive = gizmoSnapEnabled_ || ImGui::GetIO().KeyCtrl;
    const bool manipulated = ImGuizmo::Manipulate(
        &view._11, &projection._11, operation, ImGuizmo::WORLD, &gizmoMatrix._11, nullptr,
        snapActive ? snapValues : nullptr);
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
                if (characterControllerGizmoMode_ ==
                    CharacterControllerGizmoMode::Radius) {
                    const float changedX =
                        std::abs(std::abs(storedManipulatedScale.x) - worldDiameter);
                    const float changedZ =
                        std::abs(std::abs(storedManipulatedScale.z) - worldDiameter);
                    const float newWorldDiameter =
                        changedX >= changedZ ? std::abs(storedManipulatedScale.x)
                                             : std::abs(storedManipulatedScale.z);
                    const float radialScale =
                        (std::max)(minimumScale,
                                   (std::max)(std::abs(storedEntityScale.x),
                                              std::abs(storedEntityScale.z)));
                    controller.radius =
                        (std::max)(0.001f, newWorldDiameter * 0.5f / radialScale);
                    controller.height =
                        (std::max)(controller.height, controller.radius * 2.0f);
                    controller.skinWidth =
                        (std::min)(controller.skinWidth,
                                   (std::max)(0.0f, controller.radius - 0.001f));
                } else {
                    const float verticalScale =
                        (std::max)(minimumScale, std::abs(storedEntityScale.y));
                    controller.height =
                        (std::max)(controller.radius * 2.0f,
                                   std::abs(storedManipulatedScale.y) / verticalScale);
                    controller.stepOffset =
                        (std::min)(controller.stepOffset, controller.height);
                }
                RefreshDirty();
            }
        }
    }

    if (!usingNow && gizmoWasUsing_) {
        CommitHistoryEdit();
        if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Center) {
            status_ = "Modified CharacterController center from Scene View.";
        } else if (characterControllerGizmoMode_ ==
                   CharacterControllerGizmoMode::Radius) {
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
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x,
                      imageMax.y - imageMin.y);
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
    const bool manipulated = ImGuizmo::Manipulate(
        &view._11, &projection._11, operation, mode, &worldMatrix._11, nullptr,
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
        if (std::isfinite(pivotDeterminantValue) &&
            std::abs(pivotDeterminantValue) > 1.0e-8f) {
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
        status_ = transformedCount > 1u
                      ? "Transformed " + std::to_string(transformedCount) +
                            " entities from Scene View."
                      : "Transformed entity from Scene View.";
    }
    gizmoWasUsing_ = usingNow;
    return ImGuizmo::IsOver() || usingNow;
}

