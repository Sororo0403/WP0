#include "internal/WorldEntityValidation.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <unordered_set>

namespace {
bool IsFinite(const DirectX::XMFLOAT2& value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool IsFinite(const DirectX::XMFLOAT3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool IsFinite(const DirectX::XMFLOAT4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::isfinite(value.w);
}

bool IsUnitColor(const DirectX::XMFLOAT4& color) {
    return IsFinite(color) && color.x >= 0.0f && color.x <= 1.0f &&
           color.y >= 0.0f && color.y <= 1.0f && color.z >= 0.0f &&
           color.z <= 1.0f && color.w >= 0.0f && color.w <= 1.0f;
}

bool IsBoundedText(const std::string& value, size_t maximumLength) {
    return value.size() <= maximumLength && value.find('\0') == std::string::npos;
}

void ResetRuntimeState(WorldEntity& entity) {
    if (entity.audioSource) {
        entity.audioSource->runtimeCommand = AudioSourceComponent::RuntimeCommand::None;
        entity.audioSource->pendingOneShots = 0u;
        entity.audioSource->runtimePlaying = false;
    }
    if (!entity.animator) {
        return;
    }
    AnimatorComponent& animator = *entity.animator;
    animator.runtimeCommand = AnimatorComponent::RuntimeCommand::None;
    animator.runtimeRequestedClip.clear();
    animator.runtimeClip.clear();
    animator.runtimeLoop = true;
    animator.runtimeFadeDuration = 0.0f;
    animator.runtimePlaying = false;
    animator.runtimeFinished = false;
    animator.runtimeTime = 0.0f;
    animator.runtimeDuration = 0.0f;
    animator.runtimeNormalizedTime = 0.0f;
    animator.runtimeTransitioning = false;
    animator.runtimeTransitionProgress = 0.0f;
}

const char* ValidateIdentityAndTransform(const WorldEntity& entity) {
    if (entity.layer >= PhysicsSettings::kLayerCount) {
        return "Scene contains an invalid Entity Layer.";
    }
    if (!IsFinite(entity.transform.position) ||
        !IsFinite(entity.transform.rotationDegrees) ||
        !IsFinite(entity.transform.scale)) {
        return "Scene contains a non-finite transform value.";
    }
    return nullptr;
}

const char* ValidateMeshRenderer(const WorldEntity& entity) {
    if (!entity.meshRenderer) {
        return nullptr;
    }
    const MeshRendererComponent& renderer = *entity.meshRenderer;
    const bool validSource = renderer.sourceType == MeshSourceType::Primitive ||
                             renderer.sourceType == MeshSourceType::Model;
    const bool validPrimitive = renderer.primitive >= MeshPrimitive::Box &&
                                renderer.primitive <= MeshPrimitive::Cylinder;
    return validSource && validPrimitive && IsBoundedText(renderer.modelPath, 1024u)
               ? nullptr
               : "Scene contains an invalid MeshRenderer component.";
}

bool IsValidMaterialColor(const MaterialOverrideComponent& material) {
    return IsFinite(material.baseColor) && material.baseColor.x >= 0.0f &&
           material.baseColor.y >= 0.0f && material.baseColor.z >= 0.0f &&
           material.baseColor.w >= 0.0f && material.baseColor.w <= 1.0f;
}

bool IsValidMaterialScalars(const MaterialOverrideComponent& material) {
    return std::isfinite(material.metallic) && material.metallic >= 0.0f &&
           material.metallic <= 1.0f && std::isfinite(material.roughness) &&
           material.roughness >= 0.0f && material.roughness <= 1.0f &&
           std::isfinite(material.normalStrength) && material.normalStrength >= 0.0f &&
           std::isfinite(material.alphaCutoff) && material.alphaCutoff >= 0.0f &&
           material.alphaCutoff <= 1.0f;
}

bool IsValidMaterialTextures(const MaterialOverrideComponent& material) {
    return IsBoundedText(material.baseColorTexturePath, 1024u) &&
           IsBoundedText(material.normalTexturePath, 1024u) &&
           IsBoundedText(material.roughnessTexturePath, 1024u) &&
           IsBoundedText(material.metallicTexturePath, 1024u);
}

bool IsValidMaterialEnums(const MaterialOverrideComponent& material) {
    return material.pbrTexturePacking >= MaterialPbrTexturePacking::Separate &&
           material.pbrTexturePacking <= MaterialPbrTexturePacking::MetallicRoughness &&
           material.blendMode >= MaterialSurfaceBlendMode::Opaque &&
           material.blendMode <= MaterialSurfaceBlendMode::Transparent &&
           material.cullMode >= MaterialSurfaceCullMode::None &&
           material.cullMode <= MaterialSurfaceCullMode::Back;
}

const char* ValidateMaterial(const WorldEntity& entity) {
    if (!entity.materialOverride) {
        return nullptr;
    }
    const MaterialOverrideComponent& material = *entity.materialOverride;
    if (!IsValidMaterialColor(material) || !IsValidMaterialScalars(material) ||
        !IsValidMaterialTextures(material) || !IsValidMaterialEnums(material)) {
        return "Scene contains an invalid MaterialOverride component.";
    }
    return nullptr;
}

bool IsValidAnimator(const AnimatorComponent& animator) {
    return IsBoundedText(animator.clip, 256u) && std::isfinite(animator.speed) &&
           animator.speed >= 0.0f && animator.speed <= 100.0f;
}

bool IsValidAudioSource(const AudioSourceComponent& source) {
    return IsBoundedText(source.clipPath, 1024u) && std::isfinite(source.volume) &&
           source.volume >= 0.0f && source.volume <= 1.0f &&
           std::isfinite(source.pitch) && source.pitch >= AudioSourceComponent::kMinPitch &&
           source.pitch <= AudioSourceComponent::kMaxPitch &&
           std::isfinite(source.minDistance) && std::isfinite(source.maxDistance) &&
           source.minDistance >= 0.0f && source.maxDistance > source.minDistance;
}

const char* ValidateMedia(const WorldEntity& entity) {
    if (entity.animator) {
        const AnimatorComponent& animator = *entity.animator;
        if (!IsValidAnimator(animator)) {
            return "Scene contains an invalid Animator component.";
        }
    }
    if (!entity.audioSource) {
        return nullptr;
    }
    const AudioSourceComponent& source = *entity.audioSource;
    if (!IsValidAudioSource(source)) {
        return "Scene contains an invalid AudioSource component.";
    }
    return nullptr;
}

bool IsValidCanvas(const CanvasComponent& canvas) {
    return IsFinite(canvas.referenceResolution) && canvas.referenceResolution.x >= 1.0f &&
           canvas.referenceResolution.y >= 1.0f &&
           canvas.referenceResolution.x <= 16384.0f &&
           canvas.referenceResolution.y <= 16384.0f &&
           canvas.scaleMode >= CanvasScaleMode::ConstantPixelSize &&
           canvas.scaleMode <= CanvasScaleMode::ScaleWithScreenSize &&
           canvas.screenMatchMode >= CanvasScreenMatchMode::MatchWidthOrHeight &&
           canvas.screenMatchMode <= CanvasScreenMatchMode::Shrink &&
           std::isfinite(canvas.matchWidthOrHeight) &&
           canvas.matchWidthOrHeight >= 0.0f && canvas.matchWidthOrHeight <= 1.0f;
}

const char* ValidateCanvas(const WorldEntity& entity) {
    if (entity.canvas) {
        const CanvasComponent& canvas = *entity.canvas;
        if (!IsValidCanvas(canvas) ||
            canvas.sortingOrder < -1000000 || canvas.sortingOrder > 1000000) {
            return "Scene contains an invalid Canvas component.";
        }
    }
    if (entity.canvasGroup) {
        const float alpha = entity.canvasGroup->alpha;
        if (!std::isfinite(alpha) || alpha < 0.0f || alpha > 1.0f) {
            return "Scene contains an invalid CanvasGroup component.";
        }
    }
    return nullptr;
}

bool IsValidTextLayout(const TextComponent& text) {
    return IsFinite(text.position) && std::abs(text.position.x) <= 1000000.0f &&
           std::abs(text.position.y) <= 1000000.0f && std::isfinite(text.fontSize) &&
           text.fontSize >= 1.0f && text.fontSize <= 512.0f &&
           std::isfinite(text.lineSpacing) && text.lineSpacing >= 0.0f &&
           text.lineSpacing <= 512.0f && std::isfinite(text.wrapWidth) &&
           text.wrapWidth >= 0.0f && text.wrapWidth <= 16384.0f;
}

bool IsValidTextStyle(const TextComponent& text) {
    return IsUnitColor(text.color) && text.alignment >= TextAlignment::Left &&
           text.alignment <= TextAlignment::Right && text.anchor >= UiAnchor::TopLeft &&
           text.anchor <= UiAnchor::BottomRight;
}

const char* ValidateText(const WorldEntity& entity) {
    if (!entity.text) {
        return nullptr;
    }
    const TextComponent& text = *entity.text;
    if (!IsBoundedText(text.text, 4096u) || !IsBoundedText(text.fontPath, 1024u) ||
        !IsValidTextLayout(text) || !IsValidTextStyle(text)) {
        return "Scene contains an invalid Text component.";
    }
    return nullptr;
}

bool IsValidImageLayout(const ImageComponent& image) {
    return IsFinite(image.position) && IsFinite(image.size) && IsFinite(image.pivot) &&
           image.size.x >= 0.0f && image.size.y >= 0.0f &&
           image.size.x <= 1000000.0f && image.size.y <= 1000000.0f &&
           std::abs(image.position.x) <= 1000000.0f &&
           std::abs(image.position.y) <= 1000000.0f && image.pivot.x >= 0.0f &&
           image.pivot.x <= 1.0f && image.pivot.y >= 0.0f && image.pivot.y <= 1.0f;
}

bool IsValidImageStyle(const ImageComponent& image) {
    return IsUnitColor(image.color) && image.anchor >= UiAnchor::TopLeft &&
           image.anchor <= UiAnchor::BottomRight && image.type >= ImageType::Simple &&
           image.type <= ImageType::Filled &&
           image.fillMethod >= ImageFillMethod::Horizontal &&
           image.fillMethod <= ImageFillMethod::Vertical &&
           std::isfinite(image.fillAmount) && image.fillAmount >= 0.0f &&
           image.fillAmount <= 1.0f;
}

const char* ValidateImage(const WorldEntity& entity) {
    if (!entity.image) {
        return nullptr;
    }
    const ImageComponent& image = *entity.image;
    if (!IsBoundedText(image.texturePath, 1024u) || !IsValidImageLayout(image) ||
        !IsValidImageStyle(image)) {
        return "Scene contains an invalid Image component.";
    }
    return nullptr;
}

bool IsValidButton(const ButtonComponent& button) {
    return IsUnitColor(button.normalColor) && IsUnitColor(button.hoveredColor) &&
           IsUnitColor(button.pressedColor) && IsUnitColor(button.disabledColor) &&
           button.navigation >= ButtonNavigationMode::Automatic &&
           button.navigation <= ButtonNavigationMode::None &&
           std::isfinite(button.fadeDuration) && button.fadeDuration >= 0.0f &&
           button.fadeDuration <= 10.0f;
}

bool IsValidToggle(const ToggleComponent& toggle) {
    return IsUnitColor(toggle.checkmarkColor) && std::isfinite(toggle.checkmarkScale) &&
           toggle.checkmarkScale >= 0.0f && toggle.checkmarkScale <= 1.0f;
}

const char* ValidateButtonAndToggle(const WorldEntity& entity) {
    if (entity.button) {
        const ButtonComponent& button = *entity.button;
        if (!IsValidButton(button)) {
            return "Scene contains an invalid Button component.";
        }
    }
    if (entity.toggle) {
        const ToggleComponent& toggle = *entity.toggle;
        if (!IsValidToggle(toggle)) {
            return "Scene contains an invalid Toggle component.";
        }
    }
    return nullptr;
}

const char* ValidateSlider(const WorldEntity& entity) {
    if (!entity.slider) {
        return nullptr;
    }
    const SliderComponent& slider = *entity.slider;
    if (!std::isfinite(slider.minValue) || !std::isfinite(slider.maxValue) ||
        !std::isfinite(slider.value) || slider.minValue >= slider.maxValue ||
        slider.value < slider.minValue || slider.value > slider.maxValue ||
        slider.direction < SliderDirection::LeftToRight ||
        slider.direction > SliderDirection::TopToBottom ||
        !IsUnitColor(slider.fillColor) || !IsUnitColor(slider.handleColor) ||
        !std::isfinite(slider.handleSize) || slider.handleSize < 0.0f ||
        slider.handleSize > 1000000.0f) {
        return "Scene contains an invalid Slider component.";
    }
    return nullptr;
}

const char* ValidateDropdown(const WorldEntity& entity) {
    if (!entity.dropdown) {
        return nullptr;
    }
    const DropdownComponent& dropdown = *entity.dropdown;
    const bool validOptions = !dropdown.options.empty() && dropdown.options.size() <= 256u &&
                              std::ranges::all_of(dropdown.options, [](const std::string& option) {
                                  return !option.empty() && IsBoundedText(option, 256u);
                              });
    if (!validOptions || dropdown.value < 0 ||
        static_cast<size_t>(dropdown.value) >= dropdown.options.size() ||
        !IsUnitColor(dropdown.itemColor) || !IsUnitColor(dropdown.highlightedColor) ||
        !std::isfinite(dropdown.itemHeight) || dropdown.itemHeight <= 0.0f ||
        dropdown.itemHeight > 1000000.0f) {
        return "Scene contains an invalid Dropdown component.";
    }
    return nullptr;
}

const char* ValidateInputField(const WorldEntity& entity) {
    if (!entity.inputField) {
        return nullptr;
    }
    const InputFieldComponent& input = *entity.inputField;
    if (!IsBoundedText(input.text, 4096u) || !IsBoundedText(input.placeholder, 1024u) ||
        input.characterLimit < 0 || input.characterLimit > 4096 ||
        input.contentType < InputFieldContentType::Standard ||
        input.contentType > InputFieldContentType::Password) {
        return "Scene contains an invalid InputField component.";
    }
    return nullptr;
}

bool HasValidScriptPropertyHeader(const ScriptPropertyValue& property) {
    return !property.name.empty() && IsBoundedText(property.name, 128u) &&
           property.type >= ScriptPropertyType::Float &&
           property.type <= ScriptPropertyType::Scene;
}

bool HasValidScriptPropertyValue(const ScriptPropertyValue& property) {
    if (property.type == ScriptPropertyType::Float) {
        return std::isfinite(property.floatValue);
    }
    if (property.type == ScriptPropertyType::Vector3) {
        return std::isfinite(property.vector3Value.x) &&
               std::isfinite(property.vector3Value.y) &&
               std::isfinite(property.vector3Value.z);
    }
    if (property.type == ScriptPropertyType::String ||
        property.type == ScriptPropertyType::AnimationClip ||
        property.type == ScriptPropertyType::Scene) {
        return IsBoundedText(property.stringValue, 1024u);
    }
    if (property.type == ScriptPropertyType::InputAction) {
        return IsBoundedText(property.stringValue, 64u);
    }
    return true;
}

bool IsValidScriptProperty(const ScriptPropertyValue& property) {
    if (!HasValidScriptPropertyHeader(property)) {
        return false;
    }
    return HasValidScriptPropertyValue(property);
}

const char* ValidateScripts(const WorldEntity& entity) {
    for (const BehaviorComponent& script : entity.scripts) {
        if (!IsBoundedText(script.type, 128u) ||
            !IsBoundedText(script.scriptAssetPath, 1024u) ||
            (script.type.empty() &&
             (!script.scriptAssetPath.empty() || !script.properties.empty()))) {
            return "Scene contains an invalid Script component.";
        }
        if (script.properties.size() > 128u) {
            return "Scene contains too many Script properties.";
        }
        std::unordered_set<std::string> propertyNames;
        const bool invalidProperty = std::ranges::any_of(
            script.properties, [&propertyNames](const ScriptPropertyValue& property) {
                return !IsValidScriptProperty(property) ||
                       !propertyNames.insert(property.name).second;
            });
        if (invalidProperty) {
            return "Scene contains an invalid Script property.";
        }
    }
    return nullptr;
}

bool IsValidBoxCollider(const BoxColliderComponent& collider) {
    return IsFinite(collider.center) && IsFinite(collider.size) &&
           collider.size.x >= 0.001f && collider.size.y >= 0.001f &&
           collider.size.z >= 0.001f && collider.size.x <= 1000000.0f &&
           collider.size.y <= 1000000.0f && collider.size.z <= 1000000.0f;
}

bool IsValidControllerDimensions(const CharacterControllerComponent& controller) {
    return IsFinite(controller.center) && std::isfinite(controller.radius) &&
           std::isfinite(controller.height) && controller.radius >= 0.001f &&
           controller.height >= controller.radius * 2.0f;
}

bool IsValidControllerMovement(const CharacterControllerComponent& controller) {
    return std::isfinite(controller.slopeLimitDegrees) &&
           std::isfinite(controller.stepOffset) && std::isfinite(controller.skinWidth) &&
           std::isfinite(controller.minMoveDistance) &&
           controller.slopeLimitDegrees >= 0.0f &&
           controller.slopeLimitDegrees <= 90.0f && controller.stepOffset >= 0.0f &&
           controller.stepOffset <= controller.height && controller.skinWidth >= 0.0f &&
           controller.skinWidth < controller.radius && controller.minMoveDistance >= 0.0f;
}

const char* ValidatePhysics(const WorldEntity& entity) {
    if (entity.boxCollider) {
        const BoxColliderComponent& collider = *entity.boxCollider;
        if (!IsValidBoxCollider(collider)) {
            return "Scene contains an invalid BoxCollider component.";
        }
    }
    if (!entity.characterController) {
        return nullptr;
    }
    const CharacterControllerComponent& controller = *entity.characterController;
    if (!IsValidControllerDimensions(controller) ||
        !IsValidControllerMovement(controller)) {
        return "Scene contains an invalid CharacterController component.";
    }
    return nullptr;
}

const char* ValidateComponents(const WorldEntity& entity) {
    const char* (*validators[])(const WorldEntity&) = {
        ValidateIdentityAndTransform, ValidateMeshRenderer, ValidateMaterial, ValidateMedia,
        ValidateCanvas, ValidateText, ValidateImage, ValidateButtonAndToggle,
        ValidateSlider, ValidateDropdown, ValidateInputField, ValidateScripts,
        ValidatePhysics};
    for (const auto validate : validators) {
        if (const char* error = validate(entity); error != nullptr) {
            return error;
        }
    }
    return nullptr;
}
} // namespace

bool WorldEntityValidation::PrepareAndValidate(std::vector<WorldEntity>& entities,
                                               std::string& error) {
    std::unordered_set<EntityId, EntityIdHash> ids;
    ids.reserve(entities.size());
    for (WorldEntity& entity : entities) {
        ResetRuntimeState(entity);
        if (!entity.id.IsValid() || !ids.insert(entity.id).second) {
            error = "Scene contains an invalid or duplicate entity id.";
            return false;
        }
        if (const char* validationError = ValidateComponents(entity);
            validationError != nullptr) {
            error = validationError;
            return false;
        }
        if (entity.name.empty()) {
            entity.name = "Entity";
        }
    }
    error.clear();
    return true;
}
