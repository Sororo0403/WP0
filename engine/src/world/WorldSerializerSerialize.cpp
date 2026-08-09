#include "world/WorldSerializer.h"
#include "internal/WorldSerializerJson.h"

using namespace WorldSerializerJson;

namespace {
Json EncodeEntityReference(EntityId entity) {
    return entity.IsValid() ? Json(entity.ToString()) : Json(nullptr);
}

void EncodeIdentityAndTransform(const WorldEntity& entity, Json& encoded) {
    encoded["id"] = entity.id.ToString();
    encoded["parent"] = EncodeEntityReference(entity.parent);
    encoded["name"] = entity.name;
    encoded["active"] = entity.active;
    encoded["layer"] = entity.layer;
    Json& transform = encoded["components"]["Transform"];
    transform["position"] = EncodeFloat3(entity.transform.position);
    transform["rotation"] = EncodeFloat3(entity.transform.rotationDegrees);
    transform["scale"] = EncodeFloat3(entity.transform.scale);
}

void EncodeMeshRenderer(const WorldEntity& entity, Json& components) {
    if (!entity.meshRenderer) {
        return;
    }
    const MeshRendererComponent& component = *entity.meshRenderer;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["source"] =
        component.sourceType == MeshSourceType::Primitive ? "Primitive" : "Model";
    encoded["primitive"] = static_cast<uint32_t>(component.primitive);
    encoded["model"] = component.modelPath;
    components["MeshRenderer"] = std::move(encoded);
}

const char* EncodePbrPacking(MaterialPbrTexturePacking value) {
    switch (value) {
    case MaterialPbrTexturePacking::Separate:
        return "Separate";
    case MaterialPbrTexturePacking::OcclusionRoughnessMetallic:
        return "ORM";
    case MaterialPbrTexturePacking::MetallicRoughness:
        return "MetallicRoughness";
    }
    return "Separate";
}

const char* EncodeBlendMode(MaterialSurfaceBlendMode value) {
    switch (value) {
    case MaterialSurfaceBlendMode::Opaque:
        return "Opaque";
    case MaterialSurfaceBlendMode::Cutout:
        return "Cutout";
    case MaterialSurfaceBlendMode::Transparent:
        return "Transparent";
    }
    return "Opaque";
}

const char* EncodeCullMode(MaterialSurfaceCullMode value) {
    switch (value) {
    case MaterialSurfaceCullMode::None:
        return "None";
    case MaterialSurfaceCullMode::Front:
        return "Front";
    case MaterialSurfaceCullMode::Back:
        return "Back";
    }
    return "Back";
}

void EncodeMaterialOverride(const WorldEntity& entity, Json& components) {
    if (!entity.materialOverride) {
        return;
    }
    const MaterialOverrideComponent& component = *entity.materialOverride;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["baseColor"] = EncodeFloat4(component.baseColor);
    encoded["metallic"] = component.metallic;
    encoded["roughness"] = component.roughness;
    encoded["baseColorTexture"] = component.baseColorTexturePath;
    encoded["normalTexture"] = component.normalTexturePath;
    encoded["normalStrength"] = component.normalStrength;
    encoded["roughnessTexture"] = component.roughnessTexturePath;
    encoded["metallicTexture"] = component.metallicTexturePath;
    encoded["pbrTexturePacking"] = EncodePbrPacking(component.pbrTexturePacking);
    encoded["blendMode"] = EncodeBlendMode(component.blendMode);
    encoded["alphaCutoff"] = component.alphaCutoff;
    encoded["cullMode"] = EncodeCullMode(component.cullMode);
    encoded["depthWrite"] = component.depthWrite;
    components["MaterialOverride"] = std::move(encoded);
}

void EncodeCamera(const WorldEntity& entity, Json& components) {
    if (!entity.camera) {
        return;
    }
    const CameraComponent& component = *entity.camera;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["primary"] = component.primary;
    encoded["projection"] =
        component.projection == CameraProjection::Perspective ? "Perspective"
                                                              : "Orthographic";
    encoded["fieldOfView"] = component.fieldOfViewDegrees;
    encoded["orthographicHeight"] = component.orthographicHeight;
    encoded["nearClip"] = component.nearClip;
    encoded["farClip"] = component.farClip;
    components["Camera"] = std::move(encoded);
}

const char* EncodeLightType(LightType value) {
    switch (value) {
    case LightType::Directional:
        return "Directional";
    case LightType::Point:
        return "Point";
    case LightType::Spot:
        return "Spot";
    }
    return "Directional";
}

void EncodeLight(const WorldEntity& entity, Json& components) {
    if (!entity.light) {
        return;
    }
    const LightComponent& component = *entity.light;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["type"] = EncodeLightType(component.type);
    encoded["color"] = EncodeFloat3(component.color);
    encoded["intensity"] = component.intensity;
    encoded["range"] = component.range;
    encoded["innerAngle"] = component.innerAngleDegrees;
    encoded["outerAngle"] = component.outerAngleDegrees;
    components["Light"] = std::move(encoded);
}

void EncodeRendering(const WorldEntity& entity, Json& components) {
    EncodeMeshRenderer(entity, components);
    EncodeMaterialOverride(entity, components);
    EncodeCamera(entity, components);
    EncodeLight(entity, components);
}

void EncodeAudioSource(const WorldEntity& entity, Json& components) {
    if (!entity.audioSource) {
        return;
    }
    const AudioSourceComponent& component = *entity.audioSource;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["clip"] = component.clipPath;
    encoded["playOnAwake"] = component.playOnAwake;
    encoded["loop"] = component.loop;
    encoded["volume"] = component.volume;
    encoded["pitch"] = component.pitch;
    encoded["spatial"] = component.spatial;
    encoded["minDistance"] = component.minDistance;
    encoded["maxDistance"] = component.maxDistance;
    components["AudioSource"] = std::move(encoded);
}

void EncodeRuntime(const WorldEntity& entity, Json& components) {
    EncodeAudioSource(entity, components);
    if (entity.audioListener) {
        components["AudioListener"]["enabled"] = entity.audioListener->enabled;
    }
    if (!entity.animator) {
        return;
    }
    const AnimatorComponent& component = *entity.animator;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["clip"] = component.clip;
    encoded["playOnAwake"] = component.playOnAwake;
    encoded["loop"] = component.loop;
    encoded["speed"] = component.speed;
    encoded["lockRootPosition"] = component.lockRootPosition;
    components["Animator"] = std::move(encoded);
}

void EncodeCanvas(const WorldEntity& entity, Json& components) {
    if (entity.canvas) {
        Json encoded;
        encoded["enabled"] = entity.canvas->enabled;
        encoded["referenceResolution"] = EncodeFloat2(entity.canvas->referenceResolution);
        encoded["scaleMode"] = EncodeCanvasScaleMode(entity.canvas->scaleMode);
        encoded["screenMatchMode"] =
            EncodeCanvasScreenMatchMode(entity.canvas->screenMatchMode);
        encoded["matchWidthOrHeight"] = entity.canvas->matchWidthOrHeight;
        encoded["sortingOrder"] = entity.canvas->sortingOrder;
        components["Canvas"] = std::move(encoded);
    }
    if (entity.canvasGroup) {
        const CanvasGroupComponent& component = *entity.canvasGroup;
        Json encoded;
        encoded["enabled"] = component.enabled;
        encoded["alpha"] = component.alpha;
        encoded["interactable"] = component.interactable;
        encoded["blocksRaycasts"] = component.blocksRaycasts;
        components["CanvasGroup"] = std::move(encoded);
    }
    if (entity.eventSystem) {
        const EventSystemComponent& component = *entity.eventSystem;
        Json encoded;
        encoded["enabled"] = component.enabled;
        encoded["firstSelected"] = EncodeEntityReference(component.firstSelected);
        encoded["sendNavigationEvents"] = component.sendNavigationEvents;
        components["EventSystem"] = std::move(encoded);
    }
}

const char* EncodeTextAlignment(TextAlignment value) {
    switch (value) {
    case TextAlignment::Left:
        return "Left";
    case TextAlignment::Center:
        return "Center";
    case TextAlignment::Right:
        return "Right";
    }
    return "Left";
}

void EncodeText(const WorldEntity& entity, Json& components) {
    if (!entity.text) {
        return;
    }
    const TextComponent& component = *entity.text;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["text"] = component.text;
    encoded["font"] = component.fontPath;
    encoded["position"] = EncodeFloat2(component.position);
    encoded["fontSize"] = component.fontSize;
    encoded["lineSpacing"] = component.lineSpacing;
    encoded["wrapWidth"] = component.wrapWidth;
    encoded["color"] = EncodeFloat4(component.color);
    encoded["anchor"] = EncodeUiAnchor(component.anchor);
    encoded["alignment"] = EncodeTextAlignment(component.alignment);
    components["Text"] = std::move(encoded);
}

void EncodeImage(const WorldEntity& entity, Json& components) {
    if (!entity.image) {
        return;
    }
    const ImageComponent& component = *entity.image;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["texture"] = component.texturePath;
    encoded["position"] = EncodeFloat2(component.position);
    encoded["size"] = EncodeFloat2(component.size);
    encoded["color"] = EncodeFloat4(component.color);
    encoded["anchor"] = EncodeUiAnchor(component.anchor);
    encoded["pivot"] = EncodeFloat2(component.pivot);
    encoded["type"] = EncodeImageType(component.type);
    encoded["fillMethod"] = EncodeImageFillMethod(component.fillMethod);
    encoded["fillAmount"] = component.fillAmount;
    encoded["fillReverse"] = component.fillReverse;
    encoded["preserveAspect"] = component.preserveAspect;
    components["Image"] = std::move(encoded);
}

void EncodeButton(const WorldEntity& entity, Json& components) {
    if (!entity.button) {
        return;
    }
    const ButtonComponent& component = *entity.button;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["interactable"] = component.interactable;
    encoded["navigation"] = EncodeButtonNavigationMode(component.navigation);
    encoded["normalColor"] = EncodeFloat4(component.normalColor);
    encoded["hoveredColor"] = EncodeFloat4(component.hoveredColor);
    encoded["pressedColor"] = EncodeFloat4(component.pressedColor);
    encoded["disabledColor"] = EncodeFloat4(component.disabledColor);
    encoded["fadeDuration"] = component.fadeDuration;
    encoded["selectOnLeft"] = EncodeEntityReference(component.selectOnLeft);
    encoded["selectOnRight"] = EncodeEntityReference(component.selectOnRight);
    encoded["selectOnUp"] = EncodeEntityReference(component.selectOnUp);
    encoded["selectOnDown"] = EncodeEntityReference(component.selectOnDown);
    components["Button"] = std::move(encoded);
}

void EncodeToggle(const WorldEntity& entity, Json& components) {
    if (!entity.toggle) {
        return;
    }
    const ToggleComponent& component = *entity.toggle;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["isOn"] = component.isOn;
    encoded["checkmarkColor"] = EncodeFloat4(component.checkmarkColor);
    encoded["checkmarkScale"] = component.checkmarkScale;
    components["Toggle"] = std::move(encoded);
}

void EncodeSlider(const WorldEntity& entity, Json& components) {
    if (!entity.slider) {
        return;
    }
    const SliderComponent& component = *entity.slider;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["interactable"] = component.interactable;
    encoded["minValue"] = component.minValue;
    encoded["maxValue"] = component.maxValue;
    encoded["value"] = component.value;
    encoded["wholeNumbers"] = component.wholeNumbers;
    encoded["direction"] = EncodeSliderDirection(component.direction);
    encoded["fillColor"] = EncodeFloat4(component.fillColor);
    encoded["handleColor"] = EncodeFloat4(component.handleColor);
    encoded["handleSize"] = component.handleSize;
    components["Slider"] = std::move(encoded);
}

void EncodeDropdown(const WorldEntity& entity, Json& components) {
    if (!entity.dropdown) {
        return;
    }
    const DropdownComponent& component = *entity.dropdown;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["interactable"] = component.interactable;
    encoded["options"] = component.options;
    encoded["value"] = component.value;
    encoded["itemColor"] = EncodeFloat4(component.itemColor);
    encoded["highlightedColor"] = EncodeFloat4(component.highlightedColor);
    encoded["itemHeight"] = component.itemHeight;
    components["Dropdown"] = std::move(encoded);
}

void EncodeInputField(const WorldEntity& entity, Json& components) {
    if (!entity.inputField) {
        return;
    }
    const InputFieldComponent& component = *entity.inputField;
    Json encoded;
    encoded["enabled"] = component.enabled;
    encoded["interactable"] = component.interactable;
    encoded["text"] = component.text;
    encoded["placeholder"] = component.placeholder;
    encoded["characterLimit"] = component.characterLimit;
    encoded["contentType"] =
        component.contentType == InputFieldContentType::Password ? "Password" : "Standard";
    components["InputField"] = std::move(encoded);
}

void EncodeUi(const WorldEntity& entity, Json& components) {
    EncodeCanvas(entity, components);
    EncodeText(entity, components);
    EncodeImage(entity, components);
    EncodeButton(entity, components);
    EncodeToggle(entity, components);
    EncodeSlider(entity, components);
    EncodeDropdown(entity, components);
    EncodeInputField(entity, components);
}

Json EncodeScriptProperty(const ScriptPropertyValue& property) {
    Json encoded;
    switch (property.type) {
    case ScriptPropertyType::Float:
        encoded["type"] = "Float";
        encoded["value"] = property.floatValue;
        break;
    case ScriptPropertyType::Entity:
        encoded["type"] = "Entity";
        encoded["value"] = EncodeEntityReference(property.entityValue);
        break;
    case ScriptPropertyType::Boolean:
        encoded["type"] = "Boolean";
        encoded["value"] = property.booleanValue;
        break;
    case ScriptPropertyType::Integer:
        encoded["type"] = "Integer";
        encoded["value"] = property.integerValue;
        break;
    case ScriptPropertyType::Vector3:
        encoded["type"] = "Vector3";
        encoded["value"] = {property.vector3Value.x, property.vector3Value.y,
                            property.vector3Value.z};
        break;
    case ScriptPropertyType::String:
        encoded["type"] = "String";
        encoded["value"] = property.stringValue;
        break;
    case ScriptPropertyType::AnimationClip:
        encoded["type"] = "AnimationClip";
        encoded["value"] = property.stringValue;
        break;
    case ScriptPropertyType::InputAction:
        encoded["type"] = "InputAction";
        encoded["value"] = property.stringValue;
        break;
    case ScriptPropertyType::Scene:
        encoded["type"] = "Scene";
        encoded["value"] = property.stringValue;
        break;
    }
    return encoded;
}

void EncodeScripts(const WorldEntity& entity, Json& components) {
    if (entity.scripts.empty()) {
        return;
    }
    Json scripts = Json::array();
    for (const BehaviorComponent& script : entity.scripts) {
        Json encoded;
        encoded["enabled"] = script.enabled;
        encoded["type"] = script.type;
        if (!script.scriptAssetPath.empty()) {
            encoded["script"] = script.scriptAssetPath;
        }
        if (!script.properties.empty()) {
            Json properties = Json::object();
            for (const ScriptPropertyValue& property : script.properties) {
                properties[property.name] = EncodeScriptProperty(property);
            }
            encoded["properties"] = std::move(properties);
        }
        scripts.push_back(std::move(encoded));
    }
    components["Scripts"] = std::move(scripts);
}

void EncodePhysics(const WorldEntity& entity, Json& components) {
    if (entity.boxCollider) {
        Json encoded;
        encoded["enabled"] = entity.boxCollider->enabled;
        encoded["center"] = EncodeFloat3(entity.boxCollider->center);
        encoded["size"] = EncodeFloat3(entity.boxCollider->size);
        encoded["isTrigger"] = entity.boxCollider->isTrigger;
        components["BoxCollider"] = std::move(encoded);
    }
    if (entity.characterController) {
        const CharacterControllerComponent& component = *entity.characterController;
        Json encoded;
        encoded["enabled"] = component.enabled;
        encoded["center"] = EncodeFloat3(component.center);
        encoded["radius"] = component.radius;
        encoded["height"] = component.height;
        encoded["slopeLimit"] = component.slopeLimitDegrees;
        encoded["stepOffset"] = component.stepOffset;
        encoded["skinWidth"] = component.skinWidth;
        encoded["minMoveDistance"] = component.minMoveDistance;
        components["CharacterController"] = std::move(encoded);
    }
}

Json EncodeEntity(const WorldEntity& entity) {
    Json encoded;
    EncodeIdentityAndTransform(entity, encoded);
    Json& components = encoded["components"];
    EncodeRendering(entity, components);
    EncodeRuntime(entity, components);
    EncodeUi(entity, components);
    EncodeScripts(entity, components);
    EncodePhysics(entity, components);
    return encoded;
}
} // namespace

std::string WorldSerializer::Serialize(const World& world) {
    Json root;
    root["version"] = 1;
    root["entities"] = Json::array();
    for (const WorldEntity& entity : world.Entities()) {
        root["entities"].push_back(EncodeEntity(entity));
    }
    return root.dump(2);
}
