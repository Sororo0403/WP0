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

#include "internal/EditorSceneGameUiUtils.h"

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

using namespace EditorSceneGameUiUtils;

bool EditorScene::TryCalculateGameUiCanvasLayout(const WorldEntity& entity, const ImVec2& imageMin,
                                                 const ImVec2& imageMax, float& scale,
                                                 DirectX::XMFLOAT2& origin,
                                                 DirectX::XMFLOAT2& referenceResolution) const {
    const CanvasComponent* canvas = FindEnabledCanvas(world_, entity);
    if (canvas == nullptr) {
        return false;
    }
    CalculateCanvasLayout(*canvas, imageMax.x - imageMin.x, imageMax.y - imageMin.y, imageMin.x,
                          imageMin.y, scale, origin, referenceResolution);
    return true;
}

bool EditorScene::TryCalculateGameUiRect(const WorldEntity& entity, const ImVec2& imageMin,
                                         const ImVec2& imageMax, ImVec2& minimum,
                                         ImVec2& maximum) const {
    if (!world_.IsActiveInHierarchy(entity.id)) {
        return false;
    }
    float scale = 1.0f;
    DirectX::XMFLOAT2 origin{};
    DirectX::XMFLOAT2 referenceResolution{};
    if (!TryCalculateGameUiCanvasLayout(entity, imageMin, imageMax, scale, origin,
                                        referenceResolution)) {
        return false;
    }
    bool hasRect = false;
    const auto includeRect = [&](const ImVec2& rectMin, const ImVec2& rectMax) {
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
        const DirectX::XMFLOAT2 anchor = GetUiAnchorChoice(image.anchor).factor;
        const ImVec2 rectMin{
            origin.x + (referenceResolution.x * anchor.x + image.position.x -
                        image.size.x * image.pivot.x) *
                           scale,
            origin.y + (referenceResolution.y * anchor.y + image.position.y -
                        image.size.y * image.pivot.y) *
                           scale,
        };
        includeRect(rectMin, {rectMin.x + image.size.x * scale, rectMin.y + image.size.y * scale});
    }
    TextRenderer* textRenderer = ctx_ != nullptr ? ctx_->rendering.text : nullptr;
    if (entity.text && entity.text->enabled && textRenderer != nullptr && textRenderer->IsReady()) {
        const TextComponent& text = *entity.text;
        TextStyle style{};
        if (const auto loadedFont = loadedFonts_.find(text.fontPath);
            loadedFont != loadedFonts_.end()) {
            style.font = loadedFont->second;
        }
        style.pixelSize = text.fontSize * scale;
        style.lineSpacing = text.lineSpacing * scale;
        style.wrapWidth = text.wrapWidth * scale;
        const TextLayoutMetrics metrics = textRenderer->MeasureText(text.text, style);
        const DirectX::XMFLOAT2 anchor = GetUiAnchorChoice(text.anchor).factor;
        ImVec2 rectMin{
            origin.x + (referenceResolution.x * anchor.x + text.position.x) * scale,
            origin.y + (referenceResolution.y * anchor.y + text.position.y) * scale -
                metrics.size.y * anchor.y,
        };
        if (text.alignment == TextAlignment::Center) {
            rectMin.x -= metrics.size.x * 0.5f;
        } else if (text.alignment == TextAlignment::Right) {
            rectMin.x -= metrics.size.x;
        }
        includeRect(rectMin, {rectMin.x + metrics.size.x, rectMin.y + metrics.size.y});
    }
    return hasRect;
}

bool EditorScene::TryCalculateGameUiImageRect(const WorldEntity& entity, const ImVec2& imageMin,
                                              const ImVec2& imageMax, ImVec2& minimum,
                                              ImVec2& maximum, float* canvasScale) const {
    if (!world_.IsActiveInHierarchy(entity.id) || !entity.image || !entity.image->enabled) {
        return false;
    }
    float scale = 1.0f;
    DirectX::XMFLOAT2 origin{};
    DirectX::XMFLOAT2 referenceResolution{};
    if (!TryCalculateGameUiCanvasLayout(entity, imageMin, imageMax, scale, origin,
                                        referenceResolution)) {
        return false;
    }
    const ImageComponent& image = *entity.image;
    const DirectX::XMFLOAT2 anchor = GetUiAnchorChoice(image.anchor).factor;
    minimum = {
        origin.x +
            (referenceResolution.x * anchor.x + image.position.x - image.size.x * image.pivot.x) *
                scale,
        origin.y +
            (referenceResolution.y * anchor.y + image.position.y - image.size.y * image.pivot.y) *
                scale,
    };
    maximum = {minimum.x + image.size.x * scale, minimum.y + image.size.y * scale};
    if (canvasScale != nullptr) {
        *canvasScale = scale;
    }
    return true;
}

void EditorScene::UpdateGameUiDragAndResize(const ImVec2& imageMin, const ImVec2& imageMax) {
    if (gameUiResizeEntity_.IsValid() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        WorldEntity* entity = world_.Find(gameUiResizeEntity_);
        if (entity != nullptr && entity->image &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            float scale = 1.0f;
            ImVec2 rectMin{};
            ImVec2 rectMax{};
            if (TryCalculateGameUiImageRect(*entity, imageMin, imageMax, rectMin, rectMax,
                                            &scale) &&
                scale > 0.0f) {
                const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
                ImageComponent& image = *entity->image;
                const DirectX::XMFLOAT2 previousSize = image.size;
                const float horizontalDelta = mouseDelta.x / scale;
                const float verticalDelta = mouseDelta.y / scale;
                const bool resizesLeft = gameUiResizeHandle_ == UiResizeHandle::TopLeft ||
                                         gameUiResizeHandle_ == UiResizeHandle::BottomLeft;
                const bool resizesTop = gameUiResizeHandle_ == UiResizeHandle::TopLeft ||
                                        gameUiResizeHandle_ == UiResizeHandle::TopRight;
                image.size.x =
                    std::clamp(image.size.x + (resizesLeft ? -horizontalDelta : horizontalDelta),
                               1.0f, 1000000.0f);
                image.size.y = std::clamp(
                    image.size.y + (resizesTop ? -verticalDelta : verticalDelta), 1.0f, 1000000.0f);
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
                image.position.x =
                    std::clamp(image.position.x + positionDelta.x, -1000000.0f, 1000000.0f);
                image.position.y =
                    std::clamp(image.position.y + positionDelta.y, -1000000.0f, 1000000.0f);
                if (entity->text) {
                    const DirectX::XMFLOAT2 centerDelta{
                        minimumDelta.x + sizeDelta.x * 0.5f,
                        minimumDelta.y + sizeDelta.y * 0.5f,
                    };
                    entity->text->position.x = std::clamp(entity->text->position.x + centerDelta.x,
                                                          -1000000.0f, 1000000.0f);
                    entity->text->position.y = std::clamp(entity->text->position.y + centerDelta.y,
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
    } else if (gameUiDragEntity_.IsValid() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        WorldEntity* entity = world_.Find(gameUiDragEntity_);
        if (entity != nullptr && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            float scale = 1.0f;
            DirectX::XMFLOAT2 origin{};
            DirectX::XMFLOAT2 referenceResolution{};
            if (TryCalculateGameUiCanvasLayout(*entity, imageMin, imageMax, scale, origin,
                                               referenceResolution) &&
                scale > 0.0f) {
                const ImVec2 delta = ImGui::GetIO().MouseDelta;
                if (entity->image) {
                    entity->image->position.x = std::clamp(
                        entity->image->position.x + delta.x / scale, -1000000.0f, 1000000.0f);
                    entity->image->position.y = std::clamp(
                        entity->image->position.y + delta.y / scale, -1000000.0f, 1000000.0f);
                }
                if (entity->text) {
                    entity->text->position.x = std::clamp(
                        entity->text->position.x + delta.x / scale, -1000000.0f, 1000000.0f);
                    entity->text->position.y = std::clamp(
                        entity->text->position.y + delta.y / scale, -1000000.0f, 1000000.0f);
                }
                RefreshDirty();
                status_ = "Moved UI element in the Game View.";
            }
        }
    } else if (gameUiDragEntity_.IsValid()) {
        CommitHistoryEdit();
        gameUiDragEntity_ = {};
    }
}

void EditorScene::HandleGameUiEditing(const ImVec2& imageMin, const ImVec2& imageMax) {
    if (playModeState_ != PlayModeState::Edit || ctx_ == nullptr || imageMax.x <= imageMin.x ||
        imageMax.y <= imageMin.y) {
        gameUiDragEntity_ = {};
        gameUiResizeEntity_ = {};
        gameUiResizeHandle_ = UiResizeHandle::None;
        return;
    }

    const ImVec2 mouse = ImGui::GetMousePos();
    const bool imageHovered = ImGui::IsItemHovered();
    EntityId hovered{};
    if (imageHovered) {
        for (const OrderedUiEntity& entry : GetOrderedUiEntities(world_)) {
            const WorldEntity& entity = *entry.entity;
            ImVec2 minimum{};
            ImVec2 maximum{};
            if (TryCalculateGameUiRect(entity, imageMin, imageMax, minimum, maximum) &&
                mouse.x >= minimum.x && mouse.x <= maximum.x && mouse.y >= minimum.y &&
                mouse.y <= maximum.y) {
                hovered = entity.id;
            }
        }
    }

    const WorldEntity* selectedBeforeInput = world_.Find(selection_);
    ImVec2 selectedImageMin{};
    ImVec2 selectedImageMax{};
    const bool selectedHasImage =
        selectedBeforeInput != nullptr &&
        TryCalculateGameUiImageRect(*selectedBeforeInput, imageMin, imageMax, selectedImageMin,
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
        } else if (hitHandle({selectedImageMax.x, selectedImageMin.y})) {
            hoveredResizeHandle = UiResizeHandle::TopRight;
        } else if (hitHandle({selectedImageMin.x, selectedImageMax.y})) {
            hoveredResizeHandle = UiResizeHandle::BottomLeft;
        } else if (hitHandle(selectedImageMax)) {
            hoveredResizeHandle = UiResizeHandle::BottomRight;
        }
    }

    if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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

    UpdateGameUiDragAndResize(imageMin, imageMax);
    const ImGuiIO& io = ImGui::GetIO();
    if (!gameUiDragEntity_.IsValid() && !gameUiResizeEntity_.IsValid() &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput &&
        !io.KeyCtrl && !io.KeyAlt) {
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
        if ((nudge.x != 0.0f || nudge.y != 0.0f) && entity != nullptr &&
            TryCalculateGameUiRect(*entity, imageMin, imageMax, selectedRectMin, selectedRectMax)) {
            const std::string before = WorldSerializer::Serialize(world_);
            const auto movePosition = [&nudge](DirectX::XMFLOAT2& position) {
                position.x = std::clamp(position.x + nudge.x, -1000000.0f, 1000000.0f);
                position.y = std::clamp(position.y + nudge.y, -1000000.0f, 1000000.0f);
            };
            if (entity->image) {
                movePosition(entity->image->position);
            }
            if (entity->text) {
                movePosition(entity->text->position);
            }
            RecordImmediateEdit("Nudge UI Element", before, selection_);
            status_ =
                io.KeyShift ? "Moved UI element by 10 pixels." : "Moved UI element by 1 pixel.";
        }
    }

    const WorldEntity* selected = world_.Find(selection_);
    ImVec2 selectedMin{};
    ImVec2 selectedMax{};
    if (selected != nullptr &&
        TryCalculateGameUiRect(*selected, imageMin, imageMax, selectedMin, selectedMax)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(imageMin, imageMax, true);
        drawList->AddRect(selectedMin, selectedMax, IM_COL32(255, 190, 60, 255), 0.0f, 0, 2.0f);
        ImVec2 imageRectMin{};
        ImVec2 imageRectMax{};
        if (TryCalculateGameUiImageRect(*selected, imageMin, imageMax, imageRectMin,
                                        imageRectMax)) {
            const std::array<ImVec2, 4> handles{
                imageRectMin,
                ImVec2{imageRectMax.x, imageRectMin.y},
                ImVec2{imageRectMin.x, imageRectMax.y},
                imageRectMax,
            };
            for (const ImVec2& handle : handles) {
                drawList->AddRectFilled(
                    {handle.x - kResizeHandleRadius, handle.y - kResizeHandleRadius},
                    {handle.x + kResizeHandleRadius, handle.y + kResizeHandleRadius},
                    IM_COL32(255, 190, 60, 255));
            }
        }
        drawList->PopClipRect();
    }
    const UiResizeHandle cursorHandle =
        gameUiResizeEntity_.IsValid() ? gameUiResizeHandle_ : hoveredResizeHandle;
    if (cursorHandle == UiResizeHandle::TopLeft || cursorHandle == UiResizeHandle::BottomRight) {
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
