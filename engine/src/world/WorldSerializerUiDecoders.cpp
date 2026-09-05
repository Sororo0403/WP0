#include "internal/WorldSerializerComponentDecoders.h"

#include <limits>

using namespace WorldSerializerJson;

namespace WorldSerializerDecoding {
namespace {
bool DecodeCanvasRequiredFields(const Json& encoded, CanvasComponent& component) {
    if (!encoded.is_object() || !encoded.contains("enabled") ||
        !encoded["enabled"].is_boolean() || !encoded.contains("referenceResolution") ||
        !DecodeFloat2(encoded["referenceResolution"], component.referenceResolution) ||
        component.referenceResolution.x < 1.0f ||
        component.referenceResolution.y < 1.0f ||
        component.referenceResolution.x > 16384.0f ||
        component.referenceResolution.y > 16384.0f) {
        return false;
    }
    component.enabled = encoded["enabled"].get<bool>();
    return true;
}

bool DecodeCanvasModes(const Json& encoded, CanvasComponent& component,
                       std::string* error) {
    if (encoded.contains("scaleMode") &&
        !DecodeCanvasScaleMode(encoded["scaleMode"], component.scaleMode)) {
        SetError(error, "Scene Canvas scale mode is invalid.");
        return false;
    }
    if (encoded.contains("screenMatchMode") &&
        !DecodeCanvasScreenMatchMode(encoded["screenMatchMode"],
                                     component.screenMatchMode)) {
        SetError(error, "Scene Canvas screen match mode is invalid.");
        return false;
    }
    return true;
}

bool DecodeCanvasMatch(const Json& encoded, CanvasComponent& component,
                       std::string* error) {
    if (encoded.contains("matchWidthOrHeight")) {
        if (!encoded["matchWidthOrHeight"].is_number()) {
            SetError(error, "Scene Canvas match value is invalid.");
            return false;
        }
        component.matchWidthOrHeight = encoded["matchWidthOrHeight"].get<float>();
    }
    if (!std::isfinite(component.matchWidthOrHeight) ||
        component.matchWidthOrHeight < 0.0f || component.matchWidthOrHeight > 1.0f) {
        SetError(error, "Scene Canvas match value is invalid.");
        return false;
    }
    return true;
}

bool DecodeCanvasSortingOrder(const Json& encoded, CanvasComponent& component,
                              std::string* error) {
    if (!encoded.contains("sortingOrder")) {
        return true;
    }
    const Json& value = encoded["sortingOrder"];
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        SetError(error, "Scene Canvas sorting order is invalid.");
        return false;
    }
    if (value.is_number_unsigned()) {
        const uint64_t decoded = value.get<uint64_t>();
        if (decoded > 1000000u) {
            SetError(error, "Scene Canvas sorting order is invalid.");
            return false;
        }
        component.sortingOrder = static_cast<int32_t>(decoded);
        return true;
    }
    const int64_t decoded = value.get<int64_t>();
    if (decoded < -1000000 || decoded > 1000000) {
        SetError(error, "Scene Canvas sorting order is invalid.");
        return false;
    }
    component.sortingOrder = static_cast<int32_t>(decoded);
    return true;
}

bool DecodeCanvasComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("Canvas")) {
        return true;
    }
    const Json& canvas = encoded["components"]["Canvas"];
    CanvasComponent component{};
    if (!DecodeCanvasRequiredFields(canvas, component)) {
        SetError(error, "Scene Canvas component is invalid.");
        return false;
    }
    if (!DecodeCanvasModes(canvas, component, error) ||
        !DecodeCanvasMatch(canvas, component, error) ||
        !DecodeCanvasSortingOrder(canvas, component, error)) {
        return false;
    }
    entity.canvas = component;
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

bool IsUnitColor(const DirectX::XMFLOAT4& color) {
    return color.x >= 0.0f && color.x <= 1.0f &&
           color.y >= 0.0f && color.y <= 1.0f &&
           color.z >= 0.0f && color.z <= 1.0f &&
           color.w >= 0.0f && color.w <= 1.0f;
}

bool IsBoundedString(const std::string& value, size_t maximumLength) {
    return value.size() <= maximumLength &&
           value.find('\0') == std::string::npos;
}

bool IsFiniteInRange(float value, float minimum, float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool TryDecodeInt32(const Json& encoded, int32_t& value) {
    if (!encoded.is_number_integer() && !encoded.is_number_unsigned()) {
        return false;
    }
    if (encoded.is_number_unsigned()) {
        const uint64_t decoded = encoded.get<uint64_t>();
        if (decoded > static_cast<uint64_t>((std::numeric_limits<int32_t>::max)())) {
            return false;
        }
        value = static_cast<int32_t>(decoded);
        return true;
    }
    const int64_t decoded = encoded.get<int64_t>();
    if (decoded < static_cast<int64_t>((std::numeric_limits<int32_t>::min)()) ||
        decoded > static_cast<int64_t>((std::numeric_limits<int32_t>::max)())) {
        return false;
    }
    value = static_cast<int32_t>(decoded);
    return true;
}

bool IsBoundedPosition(const DirectX::XMFLOAT2& position) {
    return std::abs(position.x) <= 1000000.0f &&
           std::abs(position.y) <= 1000000.0f;
}

bool DecodeOptionalFloat(const Json& object, const char* name, float& value,
                         const char* message, std::string* error) {
    if (!object.contains(name)) {
        return true;
    }
    if (!object[name].is_number()) {
        SetError(error, message);
        return false;
    }
    value = object[name].get<float>();
    return true;
}

bool DecodeTextRequiredFields(const Json& encoded, TextComponent& component) {
    if (!encoded.is_object() || !encoded.contains("enabled") ||
        !encoded["enabled"].is_boolean() || !encoded.contains("text") ||
        !encoded["text"].is_string() || !encoded.contains("position") ||
        !DecodeFloat2(encoded["position"], component.position) ||
        !encoded.contains("fontSize") || !encoded["fontSize"].is_number() ||
        !encoded.contains("color") ||
        !DecodeFloat4(encoded["color"], component.color) ||
        !encoded.contains("alignment") || !encoded["alignment"].is_string()) {
        return false;
    }
    component.enabled = encoded["enabled"].get<bool>();
    component.text = encoded["text"].get<std::string>();
    component.fontSize = encoded["fontSize"].get<float>();
    return true;
}

bool DecodeTextAlignment(const Json& encoded, TextAlignment& alignment,
                         std::string* error) {
    const std::string value = encoded["alignment"].get<std::string>();
    if (value == "Left") {
        alignment = TextAlignment::Left;
    } else if (value == "Center") {
        alignment = TextAlignment::Center;
    } else if (value == "Right") {
        alignment = TextAlignment::Right;
    } else {
        SetError(error, "Scene Text alignment is invalid.");
        return false;
    }
    return true;
}

bool DecodeTextOptions(const Json& encoded, TextComponent& component,
                       std::string* error) {
    if (encoded.contains("font")) {
        if (!encoded["font"].is_string()) {
            SetError(error, "Scene Text font is invalid.");
            return false;
        }
        component.fontPath = encoded["font"].get<std::string>();
    }
    if (encoded.contains("anchor") &&
        !DecodeUiAnchor(encoded["anchor"], component.anchor)) {
        SetError(error, "Scene Text anchor is invalid.");
        return false;
    }
    return DecodeOptionalFloat(encoded, "lineSpacing", component.lineSpacing,
                               "Scene Text line spacing is invalid.", error) &&
           DecodeOptionalFloat(encoded, "wrapWidth", component.wrapWidth,
                               "Scene Text wrap width is invalid.", error);
}

bool IsValidTextSettings(const TextComponent& component) {
    return IsBoundedString(component.text, 4096u) &&
           IsBoundedString(component.fontPath, 1024u) &&
           IsBoundedPosition(component.position) &&
           IsFiniteInRange(component.fontSize, 1.0f, 512.0f) &&
           IsFiniteInRange(component.lineSpacing, 0.0f, 512.0f) &&
           IsFiniteInRange(component.wrapWidth, 0.0f, 16384.0f) &&
           IsUnitColor(component.color);
}

bool DecodeTextComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("Text")) {
        return true;
    }
    const Json& encodedText = encoded["components"]["Text"];
    TextComponent component{};
    if (!DecodeTextRequiredFields(encodedText, component)) {
        SetError(error, "Scene Text component is invalid.");
        return false;
    }
    if (!DecodeTextAlignment(encodedText, component.alignment, error) ||
        !DecodeTextOptions(encodedText, component, error)) {
        return false;
    }
    if (!IsValidTextSettings(component)) {
        SetError(error, "Scene Text settings are invalid.");
        return false;
    }
    entity.text = std::move(component);
    return true;
}

bool DecodeImageRequiredFields(const Json& encoded, ImageComponent& component) {
    if (!encoded.is_object() || !encoded.contains("enabled") ||
        !encoded["enabled"].is_boolean() || !encoded.contains("texture") ||
        !encoded["texture"].is_string() || !encoded.contains("position") ||
        !DecodeFloat2(encoded["position"], component.position) ||
        !encoded.contains("size") || !DecodeFloat2(encoded["size"], component.size) ||
        !encoded.contains("color") || !DecodeFloat4(encoded["color"], component.color)) {
        return false;
    }
    component.enabled = encoded["enabled"].get<bool>();
    component.texturePath = encoded["texture"].get<std::string>();
    return true;
}

bool DecodeImageLayoutOptions(const Json& encoded, ImageComponent& component,
                              std::string* error) {
    if (encoded.contains("anchor") &&
        !DecodeUiAnchor(encoded["anchor"], component.anchor)) {
        SetError(error, "Scene Image anchor is invalid.");
        return false;
    }
    if (encoded.contains("pivot")) {
        if (!DecodeFloat2(encoded["pivot"], component.pivot)) {
            SetError(error, "Scene Image pivot is invalid.");
            return false;
        }
    } else {
        component.pivot = UiAnchorFactor(component.anchor);
    }
    return true;
}

bool DecodeImageFillOptions(const Json& encoded, ImageComponent& component,
                            std::string* error) {
    if (encoded.contains("type") &&
        !DecodeImageType(encoded["type"], component.type)) {
        SetError(error, "Scene Image type is invalid.");
        return false;
    }
    if (encoded.contains("fillMethod") &&
        !DecodeImageFillMethod(encoded["fillMethod"], component.fillMethod)) {
        SetError(error, "Scene Image fill method is invalid.");
        return false;
    }
    if (!DecodeOptionalFloat(encoded, "fillAmount", component.fillAmount,
                             "Scene Image fill amount is invalid.", error)) {
        return false;
    }
    if (encoded.contains("fillReverse")) {
        if (!encoded["fillReverse"].is_boolean()) {
            SetError(error, "Scene Image fill direction is invalid.");
            return false;
        }
        component.fillReverse = encoded["fillReverse"].get<bool>();
    }
    if (encoded.contains("preserveAspect")) {
        if (!encoded["preserveAspect"].is_boolean()) {
            SetError(error, "Scene Image preserve aspect setting is invalid.");
            return false;
        }
        component.preserveAspect = encoded["preserveAspect"].get<bool>();
    }
    return true;
}

bool IsValidImageSettings(const ImageComponent& component) {
    const bool validSize = component.size.x >= 0.0f && component.size.x <= 1000000.0f &&
                           component.size.y >= 0.0f && component.size.y <= 1000000.0f;
    const bool validPivot = component.pivot.x >= 0.0f && component.pivot.x <= 1.0f &&
                            component.pivot.y >= 0.0f && component.pivot.y <= 1.0f;
    return IsBoundedString(component.texturePath, 1024u) && validSize && validPivot &&
           IsBoundedPosition(component.position) &&
           IsFiniteInRange(component.fillAmount, 0.0f, 1.0f) &&
           IsUnitColor(component.color);
}

bool DecodeImageComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("Image")) {
        return true;
    }
    const Json& encodedImage = encoded["components"]["Image"];
    ImageComponent component{};
    if (!DecodeImageRequiredFields(encodedImage, component)) {
        SetError(error, "Scene Image component is invalid.");
        return false;
    }
    if (!DecodeImageLayoutOptions(encodedImage, component, error) ||
        !DecodeImageFillOptions(encodedImage, component, error)) {
        return false;
    }
    if (!IsValidImageSettings(component)) {
        SetError(error, "Scene Image settings are invalid.");
        return false;
    }
    entity.image = std::move(component);
    return true;
}

bool DecodeButtonRequiredFields(const Json& encoded, ButtonComponent& component) {
    if (!encoded.is_object() || !encoded.contains("enabled") ||
        !encoded["enabled"].is_boolean() || !encoded.contains("interactable") ||
        !encoded["interactable"].is_boolean() || !encoded.contains("normalColor") ||
        !DecodeFloat4(encoded["normalColor"], component.normalColor) ||
        !encoded.contains("hoveredColor") ||
        !DecodeFloat4(encoded["hoveredColor"], component.hoveredColor) ||
        !encoded.contains("pressedColor") ||
        !DecodeFloat4(encoded["pressedColor"], component.pressedColor)) {
        return false;
    }
    component.enabled = encoded["enabled"].get<bool>();
    component.interactable = encoded["interactable"].get<bool>();
    return true;
}

bool DecodeButtonOptions(const Json& encoded, ButtonComponent& component,
                         std::string* error) {
    if (encoded.contains("disabledColor") &&
        !DecodeFloat4(encoded["disabledColor"], component.disabledColor)) {
        SetError(error, "Scene Button component is invalid.");
        return false;
    }
    if (!DecodeOptionalFloat(encoded, "fadeDuration", component.fadeDuration,
                             "Scene Button component is invalid.", error)) {
        return false;
    }
    if (encoded.contains("navigation") &&
        !DecodeButtonNavigationMode(encoded["navigation"], component.navigation)) {
        SetError(error, "Scene Button component is invalid.");
        return false;
    }
    return true;
}

bool DecodeNavigationTarget(const Json& encoded, const char* name, EntityId& target) {
    if (!encoded.contains(name) || encoded[name].is_null()) {
        return true;
    }
    return encoded[name].is_string() &&
           EntityId::TryParse(encoded[name].get_ref<const std::string&>(), target);
}

bool DecodeButtonNavigation(const Json& encoded, ButtonComponent& component) {
    return DecodeNavigationTarget(encoded, "selectOnLeft", component.selectOnLeft) &&
           DecodeNavigationTarget(encoded, "selectOnRight", component.selectOnRight) &&
           DecodeNavigationTarget(encoded, "selectOnUp", component.selectOnUp) &&
           DecodeNavigationTarget(encoded, "selectOnDown", component.selectOnDown);
}

bool IsValidButtonSettings(const ButtonComponent& component) {
    return IsUnitColor(component.normalColor) && IsUnitColor(component.hoveredColor) &&
           IsUnitColor(component.pressedColor) &&
           IsUnitColor(component.disabledColor) &&
           component.navigation >= ButtonNavigationMode::Automatic &&
           component.navigation <= ButtonNavigationMode::None &&
           IsFiniteInRange(component.fadeDuration, 0.0f, 10.0f);
}

bool DecodeButtonComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("Button")) {
        return true;
    }
    const Json& encodedButton = encoded["components"]["Button"];
    ButtonComponent component{};
    if (!DecodeButtonRequiredFields(encodedButton, component)) {
        SetError(error, "Scene Button component is invalid.");
        return false;
    }
    if (!DecodeButtonOptions(encodedButton, component, error)) {
        return false;
    }
    if (!DecodeButtonNavigation(encodedButton, component)) {
        SetError(error, "Scene Button navigation is invalid.");
        return false;
    }
    if (!IsValidButtonSettings(component)) {
        SetError(error, "Scene Button settings are invalid.");
        return false;
    }
    entity.button = std::move(component);
    return true;
}

bool DecodeToggleFields(const Json& encoded, ToggleComponent& component) {
    if (!encoded.is_object() || !encoded.contains("enabled") ||
        !encoded["enabled"].is_boolean() || !encoded.contains("isOn") ||
        !encoded["isOn"].is_boolean() || !encoded.contains("checkmarkColor") ||
        !DecodeFloat4(encoded["checkmarkColor"], component.checkmarkColor) ||
        !encoded.contains("checkmarkScale") || !encoded["checkmarkScale"].is_number()) {
        return false;
    }
    component.enabled = encoded["enabled"].get<bool>();
    component.isOn = encoded["isOn"].get<bool>();
    component.checkmarkScale = encoded["checkmarkScale"].get<float>();
    return true;
}

bool DecodeToggleComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("Toggle")) {
        return true;
    }
    ToggleComponent component{};
    if (!DecodeToggleFields(encoded["components"]["Toggle"], component)) {
        SetError(error, "Scene Toggle component is invalid.");
        return false;
    }
    if (!IsUnitColor(component.checkmarkColor) ||
        !IsFiniteInRange(component.checkmarkScale, 0.0f, 1.0f)) {
        SetError(error, "Scene Toggle settings are invalid.");
        return false;
    }
    entity.toggle = std::move(component);
    return true;
}

bool DecodeSliderScalarFields(const Json& encoded, SliderComponent& component) {
    if (!encoded.is_object() || !encoded.contains("enabled") ||
        !encoded["enabled"].is_boolean() || !encoded.contains("interactable") ||
        !encoded["interactable"].is_boolean() || !encoded.contains("minValue") ||
        !encoded["minValue"].is_number() || !encoded.contains("maxValue") ||
        !encoded["maxValue"].is_number() || !encoded.contains("value") ||
        !encoded["value"].is_number() || !encoded.contains("wholeNumbers") ||
        !encoded["wholeNumbers"].is_boolean()) {
        return false;
    }
    component.enabled = encoded["enabled"].get<bool>();
    component.interactable = encoded["interactable"].get<bool>();
    component.minValue = encoded["minValue"].get<float>();
    component.maxValue = encoded["maxValue"].get<float>();
    component.value = encoded["value"].get<float>();
    component.wholeNumbers = encoded["wholeNumbers"].get<bool>();
    return true;
}

bool DecodeSliderVisualFields(const Json& encoded, SliderComponent& component) {
    if (!encoded.contains("direction") ||
        !DecodeSliderDirection(encoded["direction"], component.direction) ||
        !encoded.contains("fillColor") ||
        !DecodeFloat4(encoded["fillColor"], component.fillColor) ||
        !encoded.contains("handleColor") ||
        !DecodeFloat4(encoded["handleColor"], component.handleColor) ||
        !encoded.contains("handleSize") || !encoded["handleSize"].is_number()) {
        return false;
    }
    component.handleSize = encoded["handleSize"].get<float>();
    return true;
}

bool IsValidSliderSettings(const SliderComponent& component) {
    return std::isfinite(component.minValue) && std::isfinite(component.maxValue) &&
           std::isfinite(component.value) && component.minValue < component.maxValue &&
           component.value >= component.minValue && component.value <= component.maxValue &&
           IsUnitColor(component.fillColor) && IsUnitColor(component.handleColor) &&
           IsFiniteInRange(component.handleSize, 0.0f, 1000000.0f);
}

bool DecodeSliderComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("Slider")) {
        return true;
    }
    const Json& encodedSlider = encoded["components"]["Slider"];
    SliderComponent component{};
    if (!DecodeSliderScalarFields(encodedSlider, component) ||
        !DecodeSliderVisualFields(encodedSlider, component)) {
        SetError(error, "Scene Slider component is invalid.");
        return false;
    }
    if (!IsValidSliderSettings(component)) {
        SetError(error, "Scene Slider settings are invalid.");
        return false;
    }
    entity.slider = std::move(component);
    return true;
}

bool DecodeDropdownCoreFields(const Json& encoded, DropdownComponent& component) {
    if (!encoded.is_object() || !encoded.contains("enabled") ||
        !encoded["enabled"].is_boolean() || !encoded.contains("interactable") ||
        !encoded["interactable"].is_boolean() || !encoded.contains("options") ||
        !encoded["options"].is_array() || encoded["options"].empty() ||
        encoded["options"].size() > 256u || !encoded.contains("value") ||
        !TryDecodeInt32(encoded["value"], component.value)) {
        return false;
    }
    component.enabled = encoded["enabled"].get<bool>();
    component.interactable = encoded["interactable"].get<bool>();
    return true;
}

bool DecodeDropdownVisualFields(const Json& encoded, DropdownComponent& component) {
    if (!encoded.contains("itemColor") ||
        !DecodeFloat4(encoded["itemColor"], component.itemColor) ||
        !encoded.contains("highlightedColor") ||
        !DecodeFloat4(encoded["highlightedColor"], component.highlightedColor) ||
        !encoded.contains("itemHeight") || !encoded["itemHeight"].is_number()) {
        return false;
    }
    component.itemHeight = encoded["itemHeight"].get<float>();
    return true;
}

bool DecodeDropdownOptions(const Json& encoded, DropdownComponent& component,
                           std::string* error) {
    component.options.clear();
    for (const Json& encodedOption : encoded["options"]) {
        if (!encodedOption.is_string()) {
            SetError(error, "Scene Dropdown option is invalid.");
            return false;
        }
        std::string option = encodedOption.get<std::string>();
        if (option.empty() || !IsBoundedString(option, 256u)) {
            SetError(error, "Scene Dropdown option is invalid.");
            return false;
        }
        component.options.push_back(std::move(option));
    }
    return true;
}

bool IsValidDropdownSettings(const DropdownComponent& component) {
    return component.value >= 0 &&
           static_cast<size_t>(component.value) < component.options.size() &&
           IsUnitColor(component.itemColor) &&
           IsUnitColor(component.highlightedColor) &&
           std::isfinite(component.itemHeight) && component.itemHeight > 0.0f &&
           component.itemHeight <= 1000000.0f;
}

bool DecodeDropdownComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("Dropdown")) {
        return true;
    }
    const Json& encodedDropdown = encoded["components"]["Dropdown"];
    DropdownComponent component{};
    if (!DecodeDropdownCoreFields(encodedDropdown, component) ||
        !DecodeDropdownVisualFields(encodedDropdown, component)) {
        SetError(error, "Scene Dropdown component is invalid.");
        return false;
    }
    if (!DecodeDropdownOptions(encodedDropdown, component, error)) {
        return false;
    }
    if (!IsValidDropdownSettings(component)) {
        SetError(error, "Scene Dropdown settings are invalid.");
        return false;
    }
    entity.dropdown = std::move(component);
    return true;
}

bool DecodeInputFieldRequiredFields(const Json& encoded,
                                    InputFieldComponent& component) {
    if (!encoded.is_object() || !encoded.contains("enabled") ||
        !encoded["enabled"].is_boolean() || !encoded.contains("interactable") ||
        !encoded["interactable"].is_boolean() || !encoded.contains("text") ||
        !encoded["text"].is_string() || !encoded.contains("placeholder") ||
        !encoded["placeholder"].is_string() || !encoded.contains("characterLimit") ||
        !TryDecodeInt32(encoded["characterLimit"], component.characterLimit) ||
        !encoded.contains("contentType") || !encoded["contentType"].is_string()) {
        return false;
    }
    component.enabled = encoded["enabled"].get<bool>();
    component.interactable = encoded["interactable"].get<bool>();
    component.text = encoded["text"].get<std::string>();
    component.placeholder = encoded["placeholder"].get<std::string>();
    return true;
}

bool DecodeInputFieldContentType(const Json& encoded,
                                 InputFieldContentType& contentType) {
    const std::string value = encoded["contentType"].get<std::string>();
    if (value == "Standard") {
        contentType = InputFieldContentType::Standard;
        return true;
    }
    if (value == "Password") {
        contentType = InputFieldContentType::Password;
        return true;
    }
    return false;
}

bool IsValidInputFieldSettings(const InputFieldComponent& component) {
    return IsBoundedString(component.text, 4096u) &&
           IsBoundedString(component.placeholder, 1024u) &&
           component.characterLimit >= 0 && component.characterLimit <= 4096;
}

bool DecodeInputFieldComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("InputField")) {
        return true;
    }
    const Json& encodedInputField = encoded["components"]["InputField"];
    InputFieldComponent component{};
    if (!DecodeInputFieldRequiredFields(encodedInputField, component)) {
        SetError(error, "Scene InputField component is invalid.");
        return false;
    }
    if (!DecodeInputFieldContentType(encodedInputField, component.contentType)) {
        SetError(error, "Scene InputField content type is invalid.");
        return false;
    }
    if (!IsValidInputFieldSettings(component)) {
        SetError(error, "Scene InputField settings are invalid.");
        return false;
    }
    entity.inputField = std::move(component);
    return true;
}

} // namespace

bool DecodeUiComponents(const Json& encoded, WorldEntity& entity, std::string* error) {
    return DecodeCanvasComponent(encoded, entity, error) &&
           DecodeCanvasGroupComponent(encoded, entity, error) &&
           DecodeEventSystemComponent(encoded, entity, error) &&
           DecodeTextComponent(encoded, entity, error) &&
           DecodeImageComponent(encoded, entity, error) &&
           DecodeButtonComponent(encoded, entity, error) &&
           DecodeToggleComponent(encoded, entity, error) &&
           DecodeSliderComponent(encoded, entity, error) &&
           DecodeDropdownComponent(encoded, entity, error) &&
           DecodeInputFieldComponent(encoded, entity, error);
}

} // namespace WorldSerializerDecoding
