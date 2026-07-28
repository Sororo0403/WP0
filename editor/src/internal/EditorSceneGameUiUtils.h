#pragma once

#include "imgui.h"
#include "world/World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace EditorSceneGameUiUtils {
inline size_t CountUtf8Codepoints(std::string_view text) {
    return static_cast<size_t>(std::ranges::count_if(
        text, [](char value) { return (static_cast<unsigned char>(value) & 0xc0u) != 0x80u; }));
}

inline void PopUtf8Codepoint(std::string& text) {
    if (text.empty()) {
        return;
    }
    size_t index = text.size() - 1u;
    while (index > 0u && (static_cast<unsigned char>(text[index]) & 0xc0u) == 0x80u) {
        --index;
    }
    text.erase(index);
}

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

inline const UiAnchorChoice& GetUiAnchorChoice(UiAnchor anchor) {
    const auto choice =
        std::ranges::find_if(kUiAnchorChoices, [anchor](const UiAnchorChoice& candidate) {
            return candidate.value == anchor;
        });
    return choice != kUiAnchorChoices.end() ? *choice : kUiAnchorChoices.front();
}

inline const CanvasComponent* FindEnabledCanvas(const World& world, const WorldEntity& entity) {
    const WorldEntity* current = &entity;
    while (current != nullptr) {
        if (current->canvas) {
            return current->canvas->enabled ? &*current->canvas : nullptr;
        }
        current = current->parent.IsValid() ? world.Find(current->parent) : nullptr;
    }
    return nullptr;
}

struct UiGroupState {
    float alpha = 1.0f;
    bool interactable = true;
    bool blocksRaycasts = true;
};

inline UiGroupState GetUiGroupState(const World& world, const WorldEntity& entity) {
    UiGroupState state{};
    const WorldEntity* current = &entity;
    while (current != nullptr) {
        if (current->canvasGroup && current->canvasGroup->enabled) {
            const CanvasGroupComponent& group = *current->canvasGroup;
            state.alpha *= group.alpha;
            state.interactable = state.interactable && group.interactable;
            state.blocksRaycasts = state.blocksRaycasts && group.blocksRaycasts;
        }
        current = current->parent.IsValid() ? world.Find(current->parent) : nullptr;
    }
    return state;
}

inline const WorldEntity* FindEventSystemEntity(const World& world) {
    for (const WorldEntity& entity : world.Entities()) {
        if (entity.eventSystem) {
            return &entity;
        }
    }
    return nullptr;
}

inline void CalculateCanvasLayout(const CanvasComponent& canvas, float width, float height,
                                  float offsetX, float offsetY, float& scale,
                                  DirectX::XMFLOAT2& origin, DirectX::XMFLOAT2& layoutResolution) {
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
            scale = std::exp2(std::lerp(std::log2(widthScale), std::log2(heightScale),
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
        offsetX + (width - canvas.referenceResolution.x * scale) * 0.5f,
        offsetY + (height - canvas.referenceResolution.y * scale) * 0.5f,
    };
}

struct OrderedUiEntity {
    const WorldEntity* entity = nullptr;
    int32_t sortingOrder = 0;
};

inline std::vector<OrderedUiEntity> GetOrderedUiEntities(const World& world) {
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
    std::stable_sort(result.begin(), result.end(),
                     [](const OrderedUiEntity& left, const OrderedUiEntity& right) {
                         return left.sortingOrder < right.sortingOrder;
                     });
    return result;
}

} // namespace EditorSceneGameUiUtils
