#include "EditorScene.h"

#include "font/TextRenderer.h"
#include "imgui.h"
#include "internal/EditorSceneGameUiUtils.h"
#include "sprite/SpriteRenderer.h"
#include "texture/TextureManager.h"

#include <algorithm>
#include <cmath>

using namespace EditorSceneGameUiUtils;

void EditorScene::PreserveGameUiImageAspect(Sprite& sprite, const ImageComponent& image,
                                            TextureHandle texture) const {
    if (!image.preserveAspect || !texture.IsValid() || ctx_->rendering.texture == nullptr ||
        !ctx_->rendering.texture->IsValidTexture(texture)) {
        return;
    }
    const float textureWidth = static_cast<float>(ctx_->rendering.texture->GetWidth(texture));
    const float textureHeight = static_cast<float>(ctx_->rendering.texture->GetHeight(texture));
    if (textureWidth <= 0.0f || textureHeight <= 0.0f || sprite.size.x <= 0.0f ||
        sprite.size.y <= 0.0f) {
        return;
    }
    const DirectX::XMFLOAT2 availableSize = sprite.size;
    const float textureAspect = textureWidth / textureHeight;
    const float availableAspect = availableSize.x / availableSize.y;
    if (availableAspect > textureAspect) {
        sprite.size.x = availableSize.y * textureAspect;
    } else {
        sprite.size.y = availableSize.x / textureAspect;
    }
    sprite.position.x += (availableSize.x - sprite.size.x) * image.pivot.x;
    sprite.position.y += (availableSize.y - sprite.size.y) * image.pivot.y;
}

void EditorScene::ApplyGameUiImageFill(Sprite& sprite, const ImageComponent& image) const {
    if (image.type != ImageType::Filled) {
        return;
    }
    const float fillAmount = std::clamp(image.fillAmount, 0.0f, 1.0f);
    if (image.fillMethod == ImageFillMethod::Horizontal) {
        const float fullWidth = sprite.size.x;
        const float fullUvWidth = sprite.uvSize.x;
        sprite.size.x = fullWidth * fillAmount;
        sprite.uvSize.x = fullUvWidth * fillAmount;
        if (image.fillReverse) {
            sprite.position.x += fullWidth * (1.0f - fillAmount);
            sprite.uvLeftTop.x += fullUvWidth * (1.0f - fillAmount);
        }
        return;
    }
    const float fullHeight = sprite.size.y;
    const float fullUvHeight = sprite.uvSize.y;
    sprite.size.y = fullHeight * fillAmount;
    sprite.uvSize.y = fullUvHeight * fillAmount;
    if (!image.fillReverse) {
        sprite.position.y += fullHeight * (1.0f - fillAmount);
        sprite.uvLeftTop.y += fullUvHeight * (1.0f - fillAmount);
    }
}

DirectX::XMFLOAT4 EditorScene::UpdateGameUiButtonColor(const WorldEntity& entity,
                                                       bool groupInteractable,
                                                       EntityId hoveredButton, bool submitHeld) {
    if (!entity.button || !entity.button->enabled) {
        return {1.0f, 1.0f, 1.0f, 1.0f};
    }
    const ButtonComponent& button = *entity.button;
    const bool interactable = button.interactable && groupInteractable;
    DirectX::XMFLOAT4 targetColor = interactable ? button.normalColor : button.disabledColor;
    if (interactable && (entity.id == hoveredButton || entity.id == focusedButton_)) {
        const bool pointerPressed = entity.id == hoveredButton && entity.id == pressedButton_ &&
                                    ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool navigationPressed = entity.id == focusedButton_ && submitHeld;
        targetColor =
            pointerPressed || navigationPressed ? button.pressedColor : button.hoveredColor;
    }
    ButtonColorTransition& transition = buttonColorTransitions_[entity.id];
    const auto colorsEqual = [](const DirectX::XMFLOAT4& lhs, const DirectX::XMFLOAT4& rhs) {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
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
    } else if (!colorsEqual(transition.current, transition.target)) {
        const float deltaTime = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
        transition.elapsed += deltaTime;
        const float amount = std::clamp(transition.elapsed / button.fadeDuration, 0.0f, 1.0f);
        transition.current = {
            std::lerp(transition.start.x, transition.target.x, amount),
            std::lerp(transition.start.y, transition.target.y, amount),
            std::lerp(transition.start.z, transition.target.z, amount),
            std::lerp(transition.start.w, transition.target.w, amount),
        };
    }
    return transition.current;
}

void EditorScene::DrawGameUiToggle(const WorldEntity& entity, const Sprite& sprite,
                                   float groupAlpha) {
    if (!entity.toggle || !entity.toggle->enabled || !entity.toggle->isOn) {
        return;
    }
    const ToggleComponent& toggle = *entity.toggle;
    Sprite checkmark{};
    checkmark.size = {
        sprite.size.x * toggle.checkmarkScale,
        sprite.size.y * toggle.checkmarkScale,
    };
    checkmark.position = {
        sprite.position.x + (sprite.size.x - checkmark.size.x) * 0.5f,
        sprite.position.y + (sprite.size.y - checkmark.size.y) * 0.5f,
    };
    checkmark.color = toggle.checkmarkColor;
    checkmark.color.w *= groupAlpha;
    checkmark.textureId = kInvalidResourceId;
    ctx_->rendering.spriteRenderer->Draw(checkmark);
}

void EditorScene::DrawGameUiSlider(const WorldEntity& entity, const Sprite& sliderTrack,
                                   float scale, float groupAlpha, bool groupInteractable) {
    if (!entity.slider || !entity.slider->enabled) {
        return;
    }
    const SliderComponent& slider = *entity.slider;
    const float range = slider.maxValue - slider.minValue;
    const float normalized =
        std::abs(range) > 0.0001f
            ? std::clamp((slider.value - slider.minValue) / range, 0.0f, 1.0f)
            : 0.0f;
    const float interactionAlpha = slider.interactable && groupInteractable ? 1.0f : 0.5f;
    Sprite fill = sliderTrack;
    fill.textureId = kInvalidResourceId;
    fill.color = slider.fillColor;
    fill.color.w *= groupAlpha * interactionAlpha;
    if (slider.direction == SliderDirection::LeftToRight ||
        slider.direction == SliderDirection::RightToLeft) {
        const float fullWidth = fill.size.x;
        fill.size.x *= normalized;
        if (slider.direction == SliderDirection::RightToLeft) {
            fill.position.x += fullWidth - fill.size.x;
        }
    } else {
        const float fullHeight = fill.size.y;
        fill.size.y *= normalized;
        if (slider.direction == SliderDirection::BottomToTop) {
            fill.position.y += fullHeight - fill.size.y;
        }
    }
    SpriteRenderer& spriteRenderer = *ctx_->rendering.spriteRenderer;
    if (fill.size.x > 0.0f && fill.size.y > 0.0f) {
        spriteRenderer.Draw(fill);
    }

    Sprite handle{};
    const float handleSize = slider.handleSize * scale;
    handle.size = {handleSize, handleSize};
    if (slider.direction == SliderDirection::LeftToRight ||
        slider.direction == SliderDirection::RightToLeft) {
        const float position =
            slider.direction == SliderDirection::RightToLeft ? 1.0f - normalized : normalized;
        handle.position = {
            sliderTrack.position.x + sliderTrack.size.x * position - handleSize * 0.5f,
            sliderTrack.position.y + (sliderTrack.size.y - handleSize) * 0.5f,
        };
    } else {
        const float position =
            slider.direction == SliderDirection::BottomToTop ? 1.0f - normalized : normalized;
        handle.position = {
            sliderTrack.position.x + (sliderTrack.size.x - handleSize) * 0.5f,
            sliderTrack.position.y + sliderTrack.size.y * position - handleSize * 0.5f,
        };
    }
    handle.color = slider.handleColor;
    handle.color.w *= groupAlpha * interactionAlpha;
    handle.textureId = kInvalidResourceId;
    if (handleSize > 0.0f) {
        spriteRenderer.Draw(handle);
    }
}

std::string EditorScene::ResolveGameUiDisplayText(const WorldEntity& entity) const {
    std::string displayText = entity.text->text;
    if (entity.inputField && entity.inputField->enabled) {
        const InputFieldComponent& inputField = *entity.inputField;
        if (inputField.text.empty()) {
            displayText = entity.id == activeInputField_ ? std::string{} : inputField.placeholder;
        } else if (inputField.contentType == InputFieldContentType::Password) {
            displayText.assign(CountUtf8Codepoints(inputField.text), '*');
        } else {
            displayText = inputField.text;
        }
        if (entity.id == activeInputField_) {
            displayText.push_back('|');
        }
    } else if (entity.dropdown && entity.dropdown->enabled && !entity.dropdown->options.empty() &&
               entity.dropdown->value >= 0 &&
               static_cast<size_t>(entity.dropdown->value) < entity.dropdown->options.size()) {
        displayText = entity.dropdown->options[static_cast<size_t>(entity.dropdown->value)];
    }
    return displayText;
}

void EditorScene::DrawGameUiText(const WorldEntity& entity, float scale,
                                 const DirectX::XMFLOAT2& canvasOrigin,
                                 const DirectX::XMFLOAT2& referenceResolution, float groupAlpha) {
    const TextComponent& text = *entity.text;
    const std::string displayText = ResolveGameUiDisplayText(entity);
    TextStyle style{};
    if (const auto loadedFont = loadedFonts_.find(text.fontPath);
        loadedFont != loadedFonts_.end()) {
        style.font = loadedFont->second;
    }
    style.pixelSize = text.fontSize * scale;
    style.lineSpacing = text.lineSpacing * scale;
    style.wrapWidth = text.wrapWidth * scale;
    style.horizontalAlignment =
        text.alignment == TextAlignment::Center  ? TextHorizontalAlignment::Center
        : text.alignment == TextAlignment::Right ? TextHorizontalAlignment::Right
                                                 : TextHorizontalAlignment::Left;
    style.color = text.color;
    style.color.w *= groupAlpha;
    const DirectX::XMFLOAT2 anchor = GetUiAnchorChoice(text.anchor).factor;
    DirectX::XMFLOAT2 position{
        canvasOrigin.x + (referenceResolution.x * anchor.x + text.position.x) * scale,
        canvasOrigin.y + (referenceResolution.y * anchor.y + text.position.y) * scale,
    };
    TextRenderer& textRenderer = *ctx_->rendering.text;
    if (text.alignment != TextAlignment::Left || anchor.y > 0.0f) {
        const TextLayoutMetrics metrics = textRenderer.MeasureText(displayText, style);
        if (text.alignment != TextAlignment::Left) {
            position.x -=
                text.alignment == TextAlignment::Center ? metrics.size.x * 0.5f : metrics.size.x;
        }
        position.y -= metrics.size.y * anchor.y;
    }
    textRenderer.DrawText(displayText, position, style);
}
