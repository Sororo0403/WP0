#include "world/WorldSerializer.h"
#include "internal/WorldSerializerJson.h"

using namespace WorldSerializerJson;

std::string WorldSerializer::Serialize(const World& world) {
    Json root;
    root["version"] = 1;
    root["entities"] = Json::array();
    for (const WorldEntity& entity : world.Entities()) {
        Json encoded;
        encoded["id"] = entity.id.ToString();
        encoded["parent"] =
            entity.parent.IsValid() ? Json(entity.parent.ToString()) : Json(nullptr);
        encoded["name"] = entity.name;
        encoded["active"] = entity.active;
        encoded["layer"] = entity.layer;
        encoded["components"]["Transform"]["position"] =
            EncodeFloat3(entity.transform.position);
        encoded["components"]["Transform"]["rotation"] =
            EncodeFloat3(entity.transform.rotationDegrees);
        encoded["components"]["Transform"]["scale"] = EncodeFloat3(entity.transform.scale);
        if (entity.meshRenderer) {
            const MeshRendererComponent& renderer = *entity.meshRenderer;
            Json meshRenderer;
            meshRenderer["enabled"] = renderer.enabled;
            meshRenderer["source"] = renderer.sourceType == MeshSourceType::Primitive
                                         ? "Primitive"
                                         : "Model";
            meshRenderer["primitive"] = static_cast<uint32_t>(renderer.primitive);
            meshRenderer["model"] = renderer.modelPath;
            encoded["components"]["MeshRenderer"] = std::move(meshRenderer);
        }
        if (entity.materialOverride) {
            const MaterialOverrideComponent& material = *entity.materialOverride;
            Json encodedMaterial;
            encodedMaterial["enabled"] = material.enabled;
            encodedMaterial["baseColor"] = EncodeFloat4(material.baseColor);
            encodedMaterial["metallic"] = material.metallic;
            encodedMaterial["roughness"] = material.roughness;
            encodedMaterial["baseColorTexture"] = material.baseColorTexturePath;
            encodedMaterial["normalTexture"] = material.normalTexturePath;
            encodedMaterial["normalStrength"] = material.normalStrength;
            encodedMaterial["roughnessTexture"] = material.roughnessTexturePath;
            encodedMaterial["metallicTexture"] = material.metallicTexturePath;
            switch (material.pbrTexturePacking) {
            case MaterialPbrTexturePacking::Separate:
                encodedMaterial["pbrTexturePacking"] = "Separate";
                break;
            case MaterialPbrTexturePacking::OcclusionRoughnessMetallic:
                encodedMaterial["pbrTexturePacking"] = "ORM";
                break;
            case MaterialPbrTexturePacking::MetallicRoughness:
                encodedMaterial["pbrTexturePacking"] = "MetallicRoughness";
                break;
            }
            switch (material.blendMode) {
            case MaterialSurfaceBlendMode::Opaque:
                encodedMaterial["blendMode"] = "Opaque";
                break;
            case MaterialSurfaceBlendMode::Cutout:
                encodedMaterial["blendMode"] = "Cutout";
                break;
            case MaterialSurfaceBlendMode::Transparent:
                encodedMaterial["blendMode"] = "Transparent";
                break;
            }
            encodedMaterial["alphaCutoff"] = material.alphaCutoff;
            switch (material.cullMode) {
            case MaterialSurfaceCullMode::None:
                encodedMaterial["cullMode"] = "None";
                break;
            case MaterialSurfaceCullMode::Front:
                encodedMaterial["cullMode"] = "Front";
                break;
            case MaterialSurfaceCullMode::Back:
                encodedMaterial["cullMode"] = "Back";
                break;
            }
            encodedMaterial["depthWrite"] = material.depthWrite;
            encoded["components"]["MaterialOverride"] = std::move(encodedMaterial);
        }
        if (entity.camera) {
            const CameraComponent& camera = *entity.camera;
            Json encodedCamera;
            encodedCamera["enabled"] = camera.enabled;
            encodedCamera["primary"] = camera.primary;
            encodedCamera["projection"] =
                camera.projection == CameraProjection::Perspective ? "Perspective"
                                                                   : "Orthographic";
            encodedCamera["fieldOfView"] = camera.fieldOfViewDegrees;
            encodedCamera["orthographicHeight"] = camera.orthographicHeight;
            encodedCamera["nearClip"] = camera.nearClip;
            encodedCamera["farClip"] = camera.farClip;
            encoded["components"]["Camera"] = std::move(encodedCamera);
        }
        if (entity.light) {
            const LightComponent& light = *entity.light;
            Json encodedLight;
            encodedLight["enabled"] = light.enabled;
            switch (light.type) {
            case LightType::Directional:
                encodedLight["type"] = "Directional";
                break;
            case LightType::Point:
                encodedLight["type"] = "Point";
                break;
            case LightType::Spot:
                encodedLight["type"] = "Spot";
                break;
            }
            encodedLight["color"] = EncodeFloat3(light.color);
            encodedLight["intensity"] = light.intensity;
            encodedLight["range"] = light.range;
            encodedLight["innerAngle"] = light.innerAngleDegrees;
            encodedLight["outerAngle"] = light.outerAngleDegrees;
            encoded["components"]["Light"] = std::move(encodedLight);
        }
        if (entity.audioSource) {
            const AudioSourceComponent& source = *entity.audioSource;
            Json encodedSource;
            encodedSource["enabled"] = source.enabled;
            encodedSource["clip"] = source.clipPath;
            encodedSource["playOnAwake"] = source.playOnAwake;
            encodedSource["loop"] = source.loop;
            encodedSource["volume"] = source.volume;
            encodedSource["pitch"] = source.pitch;
            encodedSource["spatial"] = source.spatial;
            encodedSource["minDistance"] = source.minDistance;
            encodedSource["maxDistance"] = source.maxDistance;
            encoded["components"]["AudioSource"] = std::move(encodedSource);
        }
        if (entity.audioListener) {
            Json encodedListener;
            encodedListener["enabled"] = entity.audioListener->enabled;
            encoded["components"]["AudioListener"] = std::move(encodedListener);
        }
        if (entity.animator) {
            const AnimatorComponent& animator = *entity.animator;
            Json encodedAnimator;
            encodedAnimator["enabled"] = animator.enabled;
            encodedAnimator["clip"] = animator.clip;
            encodedAnimator["playOnAwake"] = animator.playOnAwake;
            encodedAnimator["loop"] = animator.loop;
            encodedAnimator["speed"] = animator.speed;
            encodedAnimator["lockRootPosition"] = animator.lockRootPosition;
            encoded["components"]["Animator"] = std::move(encodedAnimator);
        }
        if (entity.canvas) {
            Json encodedCanvas;
            encodedCanvas["enabled"] = entity.canvas->enabled;
            encodedCanvas["referenceResolution"] =
                EncodeFloat2(entity.canvas->referenceResolution);
            encodedCanvas["scaleMode"] =
                EncodeCanvasScaleMode(entity.canvas->scaleMode);
            encodedCanvas["screenMatchMode"] =
                EncodeCanvasScreenMatchMode(entity.canvas->screenMatchMode);
            encodedCanvas["matchWidthOrHeight"] =
                entity.canvas->matchWidthOrHeight;
            encodedCanvas["sortingOrder"] = entity.canvas->sortingOrder;
            encoded["components"]["Canvas"] = std::move(encodedCanvas);
        }
        if (entity.canvasGroup) {
            const CanvasGroupComponent& group = *entity.canvasGroup;
            Json encodedGroup;
            encodedGroup["enabled"] = group.enabled;
            encodedGroup["alpha"] = group.alpha;
            encodedGroup["interactable"] = group.interactable;
            encodedGroup["blocksRaycasts"] = group.blocksRaycasts;
            encoded["components"]["CanvasGroup"] =
                std::move(encodedGroup);
        }
        if (entity.eventSystem) {
            const EventSystemComponent& eventSystem =
                *entity.eventSystem;
            Json encodedEventSystem;
            encodedEventSystem["enabled"] = eventSystem.enabled;
            encodedEventSystem["firstSelected"] =
                eventSystem.firstSelected.IsValid()
                    ? Json(eventSystem.firstSelected.ToString())
                    : Json(nullptr);
            encodedEventSystem["sendNavigationEvents"] =
                eventSystem.sendNavigationEvents;
            encoded["components"]["EventSystem"] =
                std::move(encodedEventSystem);
        }
        if (entity.text) {
            const TextComponent& text = *entity.text;
            Json encodedText;
            encodedText["enabled"] = text.enabled;
            encodedText["text"] = text.text;
            encodedText["font"] = text.fontPath;
            encodedText["position"] = EncodeFloat2(text.position);
            encodedText["fontSize"] = text.fontSize;
            encodedText["lineSpacing"] = text.lineSpacing;
            encodedText["wrapWidth"] = text.wrapWidth;
            encodedText["color"] = EncodeFloat4(text.color);
            encodedText["anchor"] = EncodeUiAnchor(text.anchor);
            switch (text.alignment) {
            case TextAlignment::Left:
                encodedText["alignment"] = "Left";
                break;
            case TextAlignment::Center:
                encodedText["alignment"] = "Center";
                break;
            case TextAlignment::Right:
                encodedText["alignment"] = "Right";
                break;
            }
            encoded["components"]["Text"] = std::move(encodedText);
        }
        if (entity.image) {
            const ImageComponent& image = *entity.image;
            Json encodedImage;
            encodedImage["enabled"] = image.enabled;
            encodedImage["texture"] = image.texturePath;
            encodedImage["position"] = EncodeFloat2(image.position);
            encodedImage["size"] = EncodeFloat2(image.size);
            encodedImage["color"] = EncodeFloat4(image.color);
            encodedImage["anchor"] = EncodeUiAnchor(image.anchor);
            encodedImage["pivot"] = EncodeFloat2(image.pivot);
            encodedImage["type"] = EncodeImageType(image.type);
            encodedImage["fillMethod"] =
                EncodeImageFillMethod(image.fillMethod);
            encodedImage["fillAmount"] = image.fillAmount;
            encodedImage["fillReverse"] = image.fillReverse;
            encodedImage["preserveAspect"] = image.preserveAspect;
            encoded["components"]["Image"] = std::move(encodedImage);
        }
        if (entity.button) {
            const ButtonComponent& button = *entity.button;
            Json encodedButton;
            encodedButton["enabled"] = button.enabled;
            encodedButton["interactable"] = button.interactable;
            encodedButton["navigation"] =
                EncodeButtonNavigationMode(button.navigation);
            encodedButton["normalColor"] = EncodeFloat4(button.normalColor);
            encodedButton["hoveredColor"] = EncodeFloat4(button.hoveredColor);
            encodedButton["pressedColor"] = EncodeFloat4(button.pressedColor);
            encodedButton["disabledColor"] = EncodeFloat4(button.disabledColor);
            encodedButton["fadeDuration"] = button.fadeDuration;
            encodedButton["selectOnLeft"] =
                button.selectOnLeft.IsValid()
                    ? Json(button.selectOnLeft.ToString())
                    : Json(nullptr);
            encodedButton["selectOnRight"] =
                button.selectOnRight.IsValid()
                    ? Json(button.selectOnRight.ToString())
                    : Json(nullptr);
            encodedButton["selectOnUp"] =
                button.selectOnUp.IsValid()
                    ? Json(button.selectOnUp.ToString())
                    : Json(nullptr);
            encodedButton["selectOnDown"] =
                button.selectOnDown.IsValid()
                    ? Json(button.selectOnDown.ToString())
                    : Json(nullptr);
            encoded["components"]["Button"] = std::move(encodedButton);
        }
        if (entity.toggle) {
            const ToggleComponent& toggle = *entity.toggle;
            Json encodedToggle;
            encodedToggle["enabled"] = toggle.enabled;
            encodedToggle["isOn"] = toggle.isOn;
            encodedToggle["checkmarkColor"] =
                EncodeFloat4(toggle.checkmarkColor);
            encodedToggle["checkmarkScale"] = toggle.checkmarkScale;
            encoded["components"]["Toggle"] = std::move(encodedToggle);
        }
        if (entity.slider) {
            const SliderComponent& slider = *entity.slider;
            Json encodedSlider;
            encodedSlider["enabled"] = slider.enabled;
            encodedSlider["interactable"] = slider.interactable;
            encodedSlider["minValue"] = slider.minValue;
            encodedSlider["maxValue"] = slider.maxValue;
            encodedSlider["value"] = slider.value;
            encodedSlider["wholeNumbers"] = slider.wholeNumbers;
            encodedSlider["direction"] =
                EncodeSliderDirection(slider.direction);
            encodedSlider["fillColor"] =
                EncodeFloat4(slider.fillColor);
            encodedSlider["handleColor"] =
                EncodeFloat4(slider.handleColor);
            encodedSlider["handleSize"] = slider.handleSize;
            encoded["components"]["Slider"] =
                std::move(encodedSlider);
        }
        if (entity.dropdown) {
            const DropdownComponent& dropdown = *entity.dropdown;
            Json encodedDropdown;
            encodedDropdown["enabled"] = dropdown.enabled;
            encodedDropdown["interactable"] = dropdown.interactable;
            encodedDropdown["options"] = dropdown.options;
            encodedDropdown["value"] = dropdown.value;
            encodedDropdown["itemColor"] =
                EncodeFloat4(dropdown.itemColor);
            encodedDropdown["highlightedColor"] =
                EncodeFloat4(dropdown.highlightedColor);
            encodedDropdown["itemHeight"] = dropdown.itemHeight;
            encoded["components"]["Dropdown"] =
                std::move(encodedDropdown);
        }
        if (entity.inputField) {
            const InputFieldComponent& inputField =
                *entity.inputField;
            Json encodedInputField;
            encodedInputField["enabled"] = inputField.enabled;
            encodedInputField["interactable"] =
                inputField.interactable;
            encodedInputField["text"] = inputField.text;
            encodedInputField["placeholder"] =
                inputField.placeholder;
            encodedInputField["characterLimit"] =
                inputField.characterLimit;
            encodedInputField["contentType"] =
                inputField.contentType ==
                        InputFieldContentType::Password
                    ? "Password"
                    : "Standard";
            encoded["components"]["InputField"] =
                std::move(encodedInputField);
        }
        if (!entity.scripts.empty()) {
            Json scripts = Json::array();
            for (const BehaviorComponent& script : entity.scripts) {
                Json encodedScript;
                encodedScript["enabled"] = script.enabled;
                encodedScript["type"] = script.type;
                if (!script.scriptAssetPath.empty()) {
                    encodedScript["script"] = script.scriptAssetPath;
                }
                if (!script.properties.empty()) {
                    Json properties = Json::object();
                    for (const ScriptPropertyValue& property : script.properties) {
                        Json encodedProperty;
                        switch (property.type) {
                        case ScriptPropertyType::Float:
                            encodedProperty["type"] = "Float";
                            encodedProperty["value"] = property.floatValue;
                            break;
                        case ScriptPropertyType::Entity:
                            encodedProperty["type"] = "Entity";
                            encodedProperty["value"] = property.entityValue.IsValid()
                                                           ? Json(property.entityValue.ToString())
                                                           : Json(nullptr);
                            break;
                        case ScriptPropertyType::Boolean:
                            encodedProperty["type"] = "Boolean";
                            encodedProperty["value"] = property.booleanValue;
                            break;
                        case ScriptPropertyType::Integer:
                            encodedProperty["type"] = "Integer";
                            encodedProperty["value"] = property.integerValue;
                            break;
                        case ScriptPropertyType::Vector3:
                            encodedProperty["type"] = "Vector3";
                            encodedProperty["value"] = {
                                property.vector3Value.x,
                                property.vector3Value.y,
                                property.vector3Value.z,
                            };
                            break;
                        case ScriptPropertyType::String:
                            encodedProperty["type"] = "String";
                            encodedProperty["value"] = property.stringValue;
                            break;
                        case ScriptPropertyType::AnimationClip:
                            encodedProperty["type"] = "AnimationClip";
                            encodedProperty["value"] = property.stringValue;
                            break;
                        case ScriptPropertyType::InputAction:
                            encodedProperty["type"] = "InputAction";
                            encodedProperty["value"] = property.stringValue;
                            break;
                        case ScriptPropertyType::Scene:
                            encodedProperty["type"] = "Scene";
                            encodedProperty["value"] = property.stringValue;
                            break;
                        }
                        properties[property.name] = std::move(encodedProperty);
                    }
                    encodedScript["properties"] = std::move(properties);
                }
                scripts.push_back(std::move(encodedScript));
            }
            encoded["components"]["Scripts"] = std::move(scripts);
        }
        if (entity.boxCollider) {
            encoded["components"]["BoxCollider"]["enabled"] =
                entity.boxCollider->enabled;
            encoded["components"]["BoxCollider"]["center"] =
                EncodeFloat3(entity.boxCollider->center);
            encoded["components"]["BoxCollider"]["size"] =
                EncodeFloat3(entity.boxCollider->size);
            encoded["components"]["BoxCollider"]["isTrigger"] =
                entity.boxCollider->isTrigger;
        }
        if (entity.characterController) {
            const CharacterControllerComponent& controller = *entity.characterController;
            Json encodedController;
            encodedController["enabled"] = controller.enabled;
            encodedController["center"] = EncodeFloat3(controller.center);
            encodedController["radius"] = controller.radius;
            encodedController["height"] = controller.height;
            encodedController["slopeLimit"] = controller.slopeLimitDegrees;
            encodedController["stepOffset"] = controller.stepOffset;
            encodedController["skinWidth"] = controller.skinWidth;
            encodedController["minMoveDistance"] = controller.minMoveDistance;
            encoded["components"]["CharacterController"] = std::move(encodedController);
        }
        root["entities"].push_back(std::move(encoded));
    }
    return root.dump(2);
}
