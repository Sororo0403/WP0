#include "internal/WorldSerializerComponentDecoders.h"

using namespace WorldSerializerJson;

namespace WorldSerializerDecoding {
namespace {
bool DecodeCanvasComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("Canvas")) {
            const Json& canvas = encoded["components"]["Canvas"];
            CanvasComponent component{};
            if (!canvas.is_object() || !canvas.contains("enabled") ||
                !canvas["enabled"].is_boolean() ||
                !canvas.contains("referenceResolution") ||
                !DecodeFloat2(canvas["referenceResolution"],
                              component.referenceResolution) ||
                component.referenceResolution.x < 1.0f ||
                component.referenceResolution.y < 1.0f ||
                component.referenceResolution.x > 16384.0f ||
                component.referenceResolution.y > 16384.0f) {
                SetError(error, "Scene Canvas component is invalid.");
                return false;
            }
            component.enabled = canvas["enabled"].get<bool>();
            if (canvas.contains("scaleMode") &&
                !DecodeCanvasScaleMode(canvas["scaleMode"],
                                       component.scaleMode)) {
                SetError(error, "Scene Canvas scale mode is invalid.");
                return false;
            }
            if (canvas.contains("screenMatchMode") &&
                !DecodeCanvasScreenMatchMode(canvas["screenMatchMode"],
                                             component.screenMatchMode)) {
                SetError(error, "Scene Canvas screen match mode is invalid.");
                return false;
            }
            if (canvas.contains("matchWidthOrHeight")) {
                if (!canvas["matchWidthOrHeight"].is_number()) {
                    SetError(error, "Scene Canvas match value is invalid.");
                    return false;
                }
                component.matchWidthOrHeight =
                    canvas["matchWidthOrHeight"].get<float>();
            }
            if (!std::isfinite(component.matchWidthOrHeight) ||
                component.matchWidthOrHeight < 0.0f ||
                component.matchWidthOrHeight > 1.0f) {
                SetError(error, "Scene Canvas match value is invalid.");
                return false;
            }
            if (canvas.contains("sortingOrder")) {
                if (!canvas["sortingOrder"].is_number_integer()) {
                    SetError(error, "Scene Canvas sorting order is invalid.");
                    return false;
                }
                if (canvas["sortingOrder"].is_number_unsigned()) {
                    const uint64_t sortingOrder =
                        canvas["sortingOrder"].get<uint64_t>();
                    if (sortingOrder > 1000000u) {
                        SetError(error, "Scene Canvas sorting order is invalid.");
                        return false;
                    }
                    component.sortingOrder =
                        static_cast<int32_t>(sortingOrder);
                } else {
                    const int64_t sortingOrder =
                        canvas["sortingOrder"].get<int64_t>();
                    if (sortingOrder < -1000000 ||
                        sortingOrder > 1000000) {
                        SetError(error, "Scene Canvas sorting order is invalid.");
                        return false;
                    }
                    component.sortingOrder =
                        static_cast<int32_t>(sortingOrder);
                }
            }
            entity.canvas = component;
        }
    return true;
}

bool DecodeCanvasGroupComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("CanvasGroup")) {
            const Json& encodedGroup =
                encoded["components"]["CanvasGroup"];
            CanvasGroupComponent component{};
            if (!encodedGroup.is_object() ||
                !encodedGroup.contains("enabled") ||
                !encodedGroup["enabled"].is_boolean() ||
                !encodedGroup.contains("alpha") ||
                !encodedGroup["alpha"].is_number() ||
                !encodedGroup.contains("interactable") ||
                !encodedGroup["interactable"].is_boolean() ||
                !encodedGroup.contains("blocksRaycasts") ||
                !encodedGroup["blocksRaycasts"].is_boolean()) {
                SetError(error,
                         "Scene CanvasGroup component is invalid.");
                return false;
            }
            component.enabled = encodedGroup["enabled"].get<bool>();
            component.alpha = encodedGroup["alpha"].get<float>();
            component.interactable =
                encodedGroup["interactable"].get<bool>();
            component.blocksRaycasts =
                encodedGroup["blocksRaycasts"].get<bool>();
            if (!std::isfinite(component.alpha) ||
                component.alpha < 0.0f || component.alpha > 1.0f) {
                SetError(error,
                         "Scene CanvasGroup settings are invalid.");
                return false;
            }
            entity.canvasGroup = std::move(component);
        }
    return true;
}

bool DecodeEventSystemComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("EventSystem")) {
            const Json& encodedEventSystem =
                encoded["components"]["EventSystem"];
            EventSystemComponent component{};
            if (!encodedEventSystem.is_object() ||
                !encodedEventSystem.contains("enabled") ||
                !encodedEventSystem["enabled"].is_boolean() ||
                !encodedEventSystem.contains("firstSelected") ||
                !encodedEventSystem.contains(
                    "sendNavigationEvents") ||
                !encodedEventSystem["sendNavigationEvents"]
                     .is_boolean()) {
                SetError(error,
                         "Scene EventSystem component is invalid.");
                return false;
            }
            const Json& firstSelected =
                encodedEventSystem["firstSelected"];
            if (!firstSelected.is_null() &&
                (!firstSelected.is_string() ||
                 !EntityId::TryParse(
                     firstSelected.get_ref<const std::string&>(),
                     component.firstSelected))) {
                SetError(error,
                         "Scene EventSystem first selection is invalid.");
                return false;
            }
            component.enabled =
                encodedEventSystem["enabled"].get<bool>();
            component.sendNavigationEvents =
                encodedEventSystem["sendNavigationEvents"].get<bool>();
            entity.eventSystem = std::move(component);
        }
    return true;
}

bool DecodeTextComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("Text")) {
            const Json& encodedText = encoded["components"]["Text"];
            TextComponent component{};
            if (!encodedText.is_object() || !encodedText.contains("enabled") ||
                !encodedText["enabled"].is_boolean() ||
                !encodedText.contains("text") ||
                !encodedText["text"].is_string() ||
                !encodedText.contains("position") ||
                !DecodeFloat2(encodedText["position"], component.position) ||
                !encodedText.contains("fontSize") ||
                !encodedText["fontSize"].is_number() ||
                !encodedText.contains("color") ||
                !DecodeFloat4(encodedText["color"], component.color) ||
                !encodedText.contains("alignment") ||
                !encodedText["alignment"].is_string()) {
                SetError(error, "Scene Text component is invalid.");
                return false;
            }
            component.enabled = encodedText["enabled"].get<bool>();
            component.text = encodedText["text"].get<std::string>();
            if (encodedText.contains("font")) {
                if (!encodedText["font"].is_string()) {
                    SetError(error, "Scene Text font is invalid.");
                    return false;
                }
                component.fontPath = encodedText["font"].get<std::string>();
            }
            component.fontSize = encodedText["fontSize"].get<float>();
            const std::string alignment =
                encodedText["alignment"].get<std::string>();
            if (alignment == "Left") {
                component.alignment = TextAlignment::Left;
            } else if (alignment == "Center") {
                component.alignment = TextAlignment::Center;
            } else if (alignment == "Right") {
                component.alignment = TextAlignment::Right;
            } else {
                SetError(error, "Scene Text alignment is invalid.");
                return false;
            }
            if (encodedText.contains("anchor") &&
                !DecodeUiAnchor(encodedText["anchor"], component.anchor)) {
                SetError(error, "Scene Text anchor is invalid.");
                return false;
            }
            if (encodedText.contains("lineSpacing")) {
                if (!encodedText["lineSpacing"].is_number()) {
                    SetError(error, "Scene Text line spacing is invalid.");
                    return false;
                }
                component.lineSpacing =
                    encodedText["lineSpacing"].get<float>();
            }
            if (encodedText.contains("wrapWidth")) {
                if (!encodedText["wrapWidth"].is_number()) {
                    SetError(error, "Scene Text wrap width is invalid.");
                    return false;
                }
                component.wrapWidth = encodedText["wrapWidth"].get<float>();
            }
            if (component.text.size() > 4096u ||
                component.text.find('\0') != std::string::npos ||
                component.fontPath.size() > 1024u ||
                component.fontPath.find('\0') != std::string::npos ||
                std::abs(component.position.x) > 1000000.0f ||
                std::abs(component.position.y) > 1000000.0f ||
                !std::isfinite(component.fontSize) || component.fontSize < 1.0f ||
                component.fontSize > 512.0f ||
                !std::isfinite(component.lineSpacing) ||
                component.lineSpacing < 0.0f ||
                component.lineSpacing > 512.0f ||
                !std::isfinite(component.wrapWidth) ||
                component.wrapWidth < 0.0f ||
                component.wrapWidth > 16384.0f ||
                component.color.x < 0.0f ||
                component.color.x > 1.0f || component.color.y < 0.0f ||
                component.color.y > 1.0f || component.color.z < 0.0f ||
                component.color.z > 1.0f || component.color.w < 0.0f ||
                component.color.w > 1.0f) {
                SetError(error, "Scene Text settings are invalid.");
                return false;
            }
            entity.text = std::move(component);
        }
    return true;
}

bool DecodeImageComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("Image")) {
            const Json& encodedImage = encoded["components"]["Image"];
            ImageComponent component{};
            if (!encodedImage.is_object() ||
                !encodedImage.contains("enabled") ||
                !encodedImage["enabled"].is_boolean() ||
                !encodedImage.contains("texture") ||
                !encodedImage["texture"].is_string() ||
                !encodedImage.contains("position") ||
                !DecodeFloat2(encodedImage["position"], component.position) ||
                !encodedImage.contains("size") ||
                !DecodeFloat2(encodedImage["size"], component.size) ||
                !encodedImage.contains("color") ||
                !DecodeFloat4(encodedImage["color"], component.color)) {
                SetError(error, "Scene Image component is invalid.");
                return false;
            }
            component.enabled = encodedImage["enabled"].get<bool>();
            component.texturePath =
                encodedImage["texture"].get<std::string>();
            if (encodedImage.contains("anchor") &&
                !DecodeUiAnchor(encodedImage["anchor"],
                                component.anchor)) {
                SetError(error, "Scene Image anchor is invalid.");
                return false;
            }
            if (encodedImage.contains("pivot")) {
                if (!DecodeFloat2(encodedImage["pivot"], component.pivot)) {
                    SetError(error, "Scene Image pivot is invalid.");
                    return false;
                }
            } else {
                component.pivot = UiAnchorFactor(component.anchor);
            }
            if (encodedImage.contains("type") &&
                !DecodeImageType(encodedImage["type"], component.type)) {
                SetError(error, "Scene Image type is invalid.");
                return false;
            }
            if (encodedImage.contains("fillMethod") &&
                !DecodeImageFillMethod(encodedImage["fillMethod"],
                                       component.fillMethod)) {
                SetError(error, "Scene Image fill method is invalid.");
                return false;
            }
            if (encodedImage.contains("fillAmount")) {
                if (!encodedImage["fillAmount"].is_number()) {
                    SetError(error, "Scene Image fill amount is invalid.");
                    return false;
                }
                component.fillAmount =
                    encodedImage["fillAmount"].get<float>();
            }
            if (encodedImage.contains("fillReverse")) {
                if (!encodedImage["fillReverse"].is_boolean()) {
                    SetError(error, "Scene Image fill direction is invalid.");
                    return false;
                }
                component.fillReverse =
                    encodedImage["fillReverse"].get<bool>();
            }
            if (encodedImage.contains("preserveAspect")) {
                if (!encodedImage["preserveAspect"].is_boolean()) {
                    SetError(error, "Scene Image preserve aspect setting is invalid.");
                    return false;
                }
                component.preserveAspect =
                    encodedImage["preserveAspect"].get<bool>();
            }
            if (component.texturePath.size() > 1024u ||
                component.texturePath.find('\0') != std::string::npos ||
                component.size.x < 0.0f || component.size.y < 0.0f ||
                component.size.x > 1000000.0f ||
                component.size.y > 1000000.0f ||
                std::abs(component.position.x) > 1000000.0f ||
                std::abs(component.position.y) > 1000000.0f ||
                component.pivot.x < 0.0f || component.pivot.x > 1.0f ||
                component.pivot.y < 0.0f || component.pivot.y > 1.0f ||
                !std::isfinite(component.fillAmount) ||
                component.fillAmount < 0.0f ||
                component.fillAmount > 1.0f ||
                component.color.x < 0.0f || component.color.x > 1.0f ||
                component.color.y < 0.0f || component.color.y > 1.0f ||
                component.color.z < 0.0f || component.color.z > 1.0f ||
                component.color.w < 0.0f || component.color.w > 1.0f) {
                SetError(error, "Scene Image settings are invalid.");
                return false;
            }
            entity.image = std::move(component);
        }
    return true;
}

bool DecodeButtonComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("Button")) {
            const Json& encodedButton = encoded["components"]["Button"];
            ButtonComponent component{};
            if (!encodedButton.is_object() ||
                !encodedButton.contains("enabled") ||
                !encodedButton["enabled"].is_boolean() ||
                !encodedButton.contains("interactable") ||
                !encodedButton["interactable"].is_boolean() ||
                !encodedButton.contains("normalColor") ||
                !DecodeFloat4(encodedButton["normalColor"],
                              component.normalColor) ||
                !encodedButton.contains("hoveredColor") ||
                !DecodeFloat4(encodedButton["hoveredColor"],
                              component.hoveredColor) ||
                !encodedButton.contains("pressedColor") ||
                !DecodeFloat4(encodedButton["pressedColor"],
                              component.pressedColor)) {
                SetError(error, "Scene Button component is invalid.");
                return false;
            }
            if (encodedButton.contains("disabledColor") &&
                !DecodeFloat4(encodedButton["disabledColor"],
                              component.disabledColor)) {
                SetError(error, "Scene Button component is invalid.");
                return false;
            }
            if (encodedButton.contains("fadeDuration")) {
                if (!encodedButton["fadeDuration"].is_number()) {
                    SetError(error, "Scene Button component is invalid.");
                    return false;
                }
                component.fadeDuration =
                    encodedButton["fadeDuration"].get<float>();
            }
            if (encodedButton.contains("navigation") &&
                !DecodeButtonNavigationMode(encodedButton["navigation"],
                                            component.navigation)) {
                SetError(error, "Scene Button component is invalid.");
                return false;
            }
            const auto decodeNavigationTarget =
                [&](const char* name, EntityId& target) {
                    if (!encodedButton.contains(name)) {
                        return true;
                    }
                    const Json& encodedTarget = encodedButton[name];
                    return encodedTarget.is_null() ||
                           (encodedTarget.is_string() &&
                            EntityId::TryParse(
                                encodedTarget
                                    .get_ref<const std::string&>(),
                                target));
                };
            if (!decodeNavigationTarget(
                    "selectOnLeft", component.selectOnLeft) ||
                !decodeNavigationTarget(
                    "selectOnRight", component.selectOnRight) ||
                !decodeNavigationTarget(
                    "selectOnUp", component.selectOnUp) ||
                !decodeNavigationTarget(
                    "selectOnDown", component.selectOnDown)) {
                SetError(error, "Scene Button navigation is invalid.");
                return false;
            }
            component.enabled = encodedButton["enabled"].get<bool>();
            component.interactable =
                encodedButton["interactable"].get<bool>();
            const auto validColor = [](const DirectX::XMFLOAT4& color) {
                return color.x >= 0.0f && color.x <= 1.0f &&
                       color.y >= 0.0f && color.y <= 1.0f &&
                       color.z >= 0.0f && color.z <= 1.0f &&
                       color.w >= 0.0f && color.w <= 1.0f;
            };
            if (!validColor(component.normalColor) ||
                !validColor(component.hoveredColor) ||
                !validColor(component.pressedColor) ||
                !validColor(component.disabledColor) ||
                component.navigation < ButtonNavigationMode::Automatic ||
                component.navigation > ButtonNavigationMode::None ||
                !std::isfinite(component.fadeDuration) ||
                component.fadeDuration < 0.0f ||
                component.fadeDuration > 10.0f) {
                SetError(error, "Scene Button settings are invalid.");
                return false;
            }
            entity.button = std::move(component);
        }
    return true;
}

bool DecodeToggleComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("Toggle")) {
            const Json& encodedToggle = encoded["components"]["Toggle"];
            ToggleComponent component{};
            if (!encodedToggle.is_object() ||
                !encodedToggle.contains("enabled") ||
                !encodedToggle["enabled"].is_boolean() ||
                !encodedToggle.contains("isOn") ||
                !encodedToggle["isOn"].is_boolean() ||
                !encodedToggle.contains("checkmarkColor") ||
                !DecodeFloat4(encodedToggle["checkmarkColor"],
                              component.checkmarkColor) ||
                !encodedToggle.contains("checkmarkScale") ||
                !encodedToggle["checkmarkScale"].is_number()) {
                SetError(error, "Scene Toggle component is invalid.");
                return false;
            }
            component.enabled = encodedToggle["enabled"].get<bool>();
            component.isOn = encodedToggle["isOn"].get<bool>();
            component.checkmarkScale =
                encodedToggle["checkmarkScale"].get<float>();
            const DirectX::XMFLOAT4& color = component.checkmarkColor;
            if (color.x < 0.0f || color.x > 1.0f ||
                color.y < 0.0f || color.y > 1.0f ||
                color.z < 0.0f || color.z > 1.0f ||
                color.w < 0.0f || color.w > 1.0f ||
                !std::isfinite(component.checkmarkScale) ||
                component.checkmarkScale < 0.0f ||
                component.checkmarkScale > 1.0f) {
                SetError(error, "Scene Toggle settings are invalid.");
                return false;
            }
            entity.toggle = std::move(component);
        }
    return true;
}

bool DecodeSliderComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("Slider")) {
            const Json& encodedSlider =
                encoded["components"]["Slider"];
            SliderComponent component{};
            if (!encodedSlider.is_object() ||
                !encodedSlider.contains("enabled") ||
                !encodedSlider["enabled"].is_boolean() ||
                !encodedSlider.contains("interactable") ||
                !encodedSlider["interactable"].is_boolean() ||
                !encodedSlider.contains("minValue") ||
                !encodedSlider["minValue"].is_number() ||
                !encodedSlider.contains("maxValue") ||
                !encodedSlider["maxValue"].is_number() ||
                !encodedSlider.contains("value") ||
                !encodedSlider["value"].is_number() ||
                !encodedSlider.contains("wholeNumbers") ||
                !encodedSlider["wholeNumbers"].is_boolean() ||
                !encodedSlider.contains("direction") ||
                !DecodeSliderDirection(encodedSlider["direction"],
                                       component.direction) ||
                !encodedSlider.contains("fillColor") ||
                !DecodeFloat4(encodedSlider["fillColor"],
                              component.fillColor) ||
                !encodedSlider.contains("handleColor") ||
                !DecodeFloat4(encodedSlider["handleColor"],
                              component.handleColor) ||
                !encodedSlider.contains("handleSize") ||
                !encodedSlider["handleSize"].is_number()) {
                SetError(error, "Scene Slider component is invalid.");
                return false;
            }
            component.enabled =
                encodedSlider["enabled"].get<bool>();
            component.interactable =
                encodedSlider["interactable"].get<bool>();
            component.minValue =
                encodedSlider["minValue"].get<float>();
            component.maxValue =
                encodedSlider["maxValue"].get<float>();
            component.value = encodedSlider["value"].get<float>();
            component.wholeNumbers =
                encodedSlider["wholeNumbers"].get<bool>();
            component.handleSize =
                encodedSlider["handleSize"].get<float>();
            const auto validColor =
                [](const DirectX::XMFLOAT4& color) {
                    return color.x >= 0.0f && color.x <= 1.0f &&
                           color.y >= 0.0f && color.y <= 1.0f &&
                           color.z >= 0.0f && color.z <= 1.0f &&
                           color.w >= 0.0f && color.w <= 1.0f;
                };
            if (!std::isfinite(component.minValue) ||
                !std::isfinite(component.maxValue) ||
                !std::isfinite(component.value) ||
                component.minValue >= component.maxValue ||
                component.value < component.minValue ||
                component.value > component.maxValue ||
                !validColor(component.fillColor) ||
                !validColor(component.handleColor) ||
                !std::isfinite(component.handleSize) ||
                component.handleSize < 0.0f ||
                component.handleSize > 1000000.0f) {
                SetError(error, "Scene Slider settings are invalid.");
                return false;
            }
            entity.slider = std::move(component);
        }
    return true;
}

bool DecodeDropdownComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("Dropdown")) {
            const Json& encodedDropdown =
                encoded["components"]["Dropdown"];
            DropdownComponent component{};
            if (!encodedDropdown.is_object() ||
                !encodedDropdown.contains("enabled") ||
                !encodedDropdown["enabled"].is_boolean() ||
                !encodedDropdown.contains("interactable") ||
                !encodedDropdown["interactable"].is_boolean() ||
                !encodedDropdown.contains("options") ||
                !encodedDropdown["options"].is_array() ||
                encodedDropdown["options"].empty() ||
                encodedDropdown["options"].size() > 256u ||
                !encodedDropdown.contains("value") ||
                !encodedDropdown["value"].is_number_integer() ||
                !encodedDropdown.contains("itemColor") ||
                !DecodeFloat4(encodedDropdown["itemColor"],
                              component.itemColor) ||
                !encodedDropdown.contains("highlightedColor") ||
                !DecodeFloat4(encodedDropdown["highlightedColor"],
                              component.highlightedColor) ||
                !encodedDropdown.contains("itemHeight") ||
                !encodedDropdown["itemHeight"].is_number()) {
                SetError(error, "Scene Dropdown component is invalid.");
                return false;
            }
            component.options.clear();
            for (const Json& encodedOption :
                 encodedDropdown["options"]) {
                if (!encodedOption.is_string()) {
                    SetError(error,
                             "Scene Dropdown option is invalid.");
                    return false;
                }
                std::string option =
                    encodedOption.get<std::string>();
                if (option.empty() || option.size() > 256u ||
                    option.find('\0') != std::string::npos) {
                    SetError(error,
                             "Scene Dropdown option is invalid.");
                    return false;
                }
                component.options.push_back(std::move(option));
            }
            component.enabled =
                encodedDropdown["enabled"].get<bool>();
            component.interactable =
                encodedDropdown["interactable"].get<bool>();
            component.value =
                encodedDropdown["value"].get<int32_t>();
            component.itemHeight =
                encodedDropdown["itemHeight"].get<float>();
            const auto validColor =
                [](const DirectX::XMFLOAT4& color) {
                    return color.x >= 0.0f && color.x <= 1.0f &&
                           color.y >= 0.0f && color.y <= 1.0f &&
                           color.z >= 0.0f && color.z <= 1.0f &&
                           color.w >= 0.0f && color.w <= 1.0f;
                };
            if (component.value < 0 ||
                static_cast<size_t>(component.value) >=
                    component.options.size() ||
                !validColor(component.itemColor) ||
                !validColor(component.highlightedColor) ||
                !std::isfinite(component.itemHeight) ||
                component.itemHeight <= 0.0f ||
                component.itemHeight > 1000000.0f) {
                SetError(error,
                         "Scene Dropdown settings are invalid.");
                return false;
            }
            entity.dropdown = std::move(component);
        }
    return true;
}

bool DecodeInputFieldComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("InputField")) {
            const Json& encodedInputField =
                encoded["components"]["InputField"];
            InputFieldComponent component{};
            if (!encodedInputField.is_object() ||
                !encodedInputField.contains("enabled") ||
                !encodedInputField["enabled"].is_boolean() ||
                !encodedInputField.contains("interactable") ||
                !encodedInputField["interactable"].is_boolean() ||
                !encodedInputField.contains("text") ||
                !encodedInputField["text"].is_string() ||
                !encodedInputField.contains("placeholder") ||
                !encodedInputField["placeholder"].is_string() ||
                !encodedInputField.contains("characterLimit") ||
                !encodedInputField["characterLimit"]
                     .is_number_integer() ||
                !encodedInputField.contains("contentType") ||
                !encodedInputField["contentType"].is_string()) {
                SetError(error,
                         "Scene InputField component is invalid.");
                return false;
            }
            component.enabled =
                encodedInputField["enabled"].get<bool>();
            component.interactable =
                encodedInputField["interactable"].get<bool>();
            component.text =
                encodedInputField["text"].get<std::string>();
            component.placeholder =
                encodedInputField["placeholder"]
                    .get<std::string>();
            component.characterLimit =
                encodedInputField["characterLimit"]
                    .get<int32_t>();
            const std::string contentType =
                encodedInputField["contentType"]
                    .get<std::string>();
            if (contentType == "Standard") {
                component.contentType =
                    InputFieldContentType::Standard;
            } else if (contentType == "Password") {
                component.contentType =
                    InputFieldContentType::Password;
            } else {
                SetError(error,
                         "Scene InputField content type is invalid.");
                return false;
            }
            if (component.text.size() > 4096u ||
                component.text.find('\0') != std::string::npos ||
                component.placeholder.size() > 1024u ||
                component.placeholder.find('\0') !=
                    std::string::npos ||
                component.characterLimit < 0 ||
                component.characterLimit > 4096) {
                SetError(error,
                         "Scene InputField settings are invalid.");
                return false;
            }
            entity.inputField = std::move(component);
        }
    return true;
}

} // namespace

bool DecodeUiComponents(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!DecodeCanvasComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeCanvasGroupComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeEventSystemComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeTextComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeImageComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeButtonComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeToggleComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeSliderComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeDropdownComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeInputFieldComponent(encoded, entity, error)) {
        return false;
    }
    return true;
}

} // namespace WorldSerializerDecoding
