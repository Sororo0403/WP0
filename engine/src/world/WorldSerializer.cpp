#include "world/WorldSerializer.h"

#include "world/World.h"

#include "nlohmann/json.hpp"

#include <cmath>
#include <exception>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;

Json EncodeFloat2(const DirectX::XMFLOAT2& value) {
    return Json::array({value.x, value.y});
}

Json EncodeFloat3(const DirectX::XMFLOAT3& value) {
    return Json::array({value.x, value.y, value.z});
}

Json EncodeFloat4(const DirectX::XMFLOAT4& value) {
    return Json::array({value.x, value.y, value.z, value.w});
}

bool DecodeFloat2(const Json& value, DirectX::XMFLOAT2& result) {
    if (!value.is_array() || value.size() != 2u) {
        return false;
    }
    try {
        result = {value[0].get<float>(), value[1].get<float>()};
    } catch (const std::exception&) {
        return false;
    }
    return std::isfinite(result.x) && std::isfinite(result.y);
}

bool DecodeFloat3(const Json& value, DirectX::XMFLOAT3& result) {
    if (!value.is_array() || value.size() != 3u) {
        return false;
    }
    try {
        result = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
    } catch (const std::exception&) {
        return false;
    }
    return std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z);
}

bool DecodeFloat4(const Json& value, DirectX::XMFLOAT4& result) {
    if (!value.is_array() || value.size() != 4u) {
        return false;
    }
    try {
        result = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>(),
                  value[3].get<float>()};
    } catch (const std::exception&) {
        return false;
    }
    return std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z) &&
           std::isfinite(result.w);
}

const char* EncodeUiAnchor(UiAnchor anchor) {
    switch (anchor) {
    case UiAnchor::TopLeft:
        return "TopLeft";
    case UiAnchor::TopCenter:
        return "TopCenter";
    case UiAnchor::TopRight:
        return "TopRight";
    case UiAnchor::MiddleLeft:
        return "MiddleLeft";
    case UiAnchor::Center:
        return "Center";
    case UiAnchor::MiddleRight:
        return "MiddleRight";
    case UiAnchor::BottomLeft:
        return "BottomLeft";
    case UiAnchor::BottomCenter:
        return "BottomCenter";
    case UiAnchor::BottomRight:
        return "BottomRight";
    }
    return "TopLeft";
}

bool DecodeUiAnchor(const Json& value, UiAnchor& anchor) {
    if (!value.is_string()) {
        return false;
    }
    const std::string encoded = value.get<std::string>();
    if (encoded == "TopLeft") {
        anchor = UiAnchor::TopLeft;
    } else if (encoded == "TopCenter") {
        anchor = UiAnchor::TopCenter;
    } else if (encoded == "TopRight") {
        anchor = UiAnchor::TopRight;
    } else if (encoded == "MiddleLeft") {
        anchor = UiAnchor::MiddleLeft;
    } else if (encoded == "Center") {
        anchor = UiAnchor::Center;
    } else if (encoded == "MiddleRight") {
        anchor = UiAnchor::MiddleRight;
    } else if (encoded == "BottomLeft") {
        anchor = UiAnchor::BottomLeft;
    } else if (encoded == "BottomCenter") {
        anchor = UiAnchor::BottomCenter;
    } else if (encoded == "BottomRight") {
        anchor = UiAnchor::BottomRight;
    } else {
        return false;
    }
    return true;
}

DirectX::XMFLOAT2 UiAnchorFactor(UiAnchor anchor) {
    switch (anchor) {
    case UiAnchor::TopLeft:
        return {0.0f, 0.0f};
    case UiAnchor::TopCenter:
        return {0.5f, 0.0f};
    case UiAnchor::TopRight:
        return {1.0f, 0.0f};
    case UiAnchor::MiddleLeft:
        return {0.0f, 0.5f};
    case UiAnchor::Center:
        return {0.5f, 0.5f};
    case UiAnchor::MiddleRight:
        return {1.0f, 0.5f};
    case UiAnchor::BottomLeft:
        return {0.0f, 1.0f};
    case UiAnchor::BottomCenter:
        return {0.5f, 1.0f};
    case UiAnchor::BottomRight:
        return {1.0f, 1.0f};
    }
    return {0.0f, 0.0f};
}

const char* EncodeImageType(ImageType type) {
    return type == ImageType::Filled ? "Filled" : "Simple";
}

bool DecodeImageType(const Json& value, ImageType& type) {
    if (!value.is_string()) {
        return false;
    }
    const std::string encoded = value.get<std::string>();
    if (encoded == "Simple") {
        type = ImageType::Simple;
    } else if (encoded == "Filled") {
        type = ImageType::Filled;
    } else {
        return false;
    }
    return true;
}

const char* EncodeImageFillMethod(ImageFillMethod method) {
    return method == ImageFillMethod::Vertical ? "Vertical"
                                               : "Horizontal";
}

bool DecodeImageFillMethod(const Json& value, ImageFillMethod& method) {
    if (!value.is_string()) {
        return false;
    }
    const std::string encoded = value.get<std::string>();
    if (encoded == "Horizontal") {
        method = ImageFillMethod::Horizontal;
    } else if (encoded == "Vertical") {
        method = ImageFillMethod::Vertical;
    } else {
        return false;
    }
    return true;
}

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}
} // namespace

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
            encodedCanvas["sortingOrder"] = entity.canvas->sortingOrder;
            encoded["components"]["Canvas"] = std::move(encodedCanvas);
        }
        if (entity.text) {
            const TextComponent& text = *entity.text;
            Json encodedText;
            encodedText["enabled"] = text.enabled;
            encodedText["text"] = text.text;
            encodedText["position"] = EncodeFloat2(text.position);
            encodedText["fontSize"] = text.fontSize;
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
            encodedButton["normalColor"] = EncodeFloat4(button.normalColor);
            encodedButton["hoveredColor"] = EncodeFloat4(button.hoveredColor);
            encodedButton["pressedColor"] = EncodeFloat4(button.pressedColor);
            encodedButton["disabledColor"] = EncodeFloat4(button.disabledColor);
            encoded["components"]["Button"] = std::move(encodedButton);
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

bool WorldSerializer::Deserialize(std::string_view text, World& world, std::string* error) {
    Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        SetError(error, "Scene JSON is invalid.");
        return false;
    }
    if (!root.contains("version") || root["version"] != Json(1) ||
        !root.contains("entities") ||
        !root["entities"].is_array()) {
        SetError(error, "Scene schema or version is unsupported.");
        return false;
    }

    std::vector<WorldEntity> entities;
    entities.reserve(root["entities"].size());
    for (const Json& encoded : root["entities"]) {
        if (!encoded.is_object() || !encoded.contains("id") || !encoded["id"].is_string() ||
            !encoded.contains("name") || !encoded["name"].is_string()) {
            SetError(error, "Scene entity is missing required fields.");
            return false;
        }
        WorldEntity entity{};
        if (!EntityId::TryParse(encoded["id"].get_ref<const std::string&>(), entity.id)) {
            SetError(error, "Scene entity id is invalid.");
            return false;
        }
        entity.name = encoded["name"].get<std::string>();
        if (encoded.contains("active")) {
            if (!encoded["active"].is_boolean()) {
                SetError(error, "Scene Entity active state is invalid.");
                return false;
            }
            entity.active = encoded["active"].get<bool>();
        }
        if (encoded.contains("layer")) {
            if (!encoded["layer"].is_number_unsigned() ||
                encoded["layer"].get<uint64_t>() >= PhysicsSettings::kLayerCount) {
                SetError(error, "Scene Entity Layer is invalid.");
                return false;
            }
            entity.layer = encoded["layer"].get<uint8_t>();
        }
        if (encoded.contains("parent") && !encoded["parent"].is_null()) {
            if (!encoded["parent"].is_string() ||
                !EntityId::TryParse(encoded["parent"].get_ref<const std::string&>(),
                                    entity.parent)) {
                SetError(error, "Scene parent id is invalid.");
                return false;
            }
        }
        if (!encoded.contains("components") || !encoded["components"].is_object() ||
            !encoded["components"].contains("Transform")) {
            SetError(error, "Scene entity has no Transform component.");
            return false;
        }
        const Json& transform = encoded["components"]["Transform"];
        if (!transform.is_object() || !transform.contains("position") ||
            !transform.contains("rotation") || !transform.contains("scale") ||
            !DecodeFloat3(transform["position"], entity.transform.position) ||
            !DecodeFloat3(transform["rotation"], entity.transform.rotationDegrees) ||
            !DecodeFloat3(transform["scale"], entity.transform.scale)) {
            SetError(error, "Scene Transform component is invalid.");
            return false;
        }
        if (encoded["components"].contains("MeshRenderer")) {
            const Json& renderer = encoded["components"]["MeshRenderer"];
            if (!renderer.is_object() || !renderer.contains("enabled") ||
                !renderer["enabled"].is_boolean() || !renderer.contains("source") ||
                !renderer["source"].is_string() || !renderer.contains("primitive") ||
                !renderer["primitive"].is_number_unsigned() || !renderer.contains("model") ||
                !renderer["model"].is_string()) {
                SetError(error, "Scene MeshRenderer component is invalid.");
                return false;
            }
            MeshRendererComponent component{};
            component.enabled = renderer["enabled"].get<bool>();
            const std::string source = renderer["source"].get<std::string>();
            if (source == "Primitive") {
                component.sourceType = MeshSourceType::Primitive;
            } else if (source == "Model") {
                component.sourceType = MeshSourceType::Model;
            } else {
                SetError(error, "Scene MeshRenderer source is invalid.");
                return false;
            }
            const uint32_t primitive = renderer["primitive"].get<uint32_t>();
            if (primitive > static_cast<uint32_t>(MeshPrimitive::Cylinder)) {
                SetError(error, "Scene MeshRenderer primitive is invalid.");
                return false;
            }
            component.primitive = static_cast<MeshPrimitive>(primitive);
            component.modelPath = renderer["model"].get<std::string>();
            if (component.modelPath.size() > 1024u ||
                component.modelPath.find('\0') != std::string::npos) {
                SetError(error, "Scene MeshRenderer model path is invalid.");
                return false;
            }
            entity.meshRenderer = std::move(component);
        }
        if (encoded["components"].contains("MaterialOverride")) {
            const Json& material = encoded["components"]["MaterialOverride"];
            if (!material.is_object() || !material.contains("enabled") ||
                !material["enabled"].is_boolean() || !material.contains("baseColor") ||
                !material.contains("metallic") || !material["metallic"].is_number() ||
                !material.contains("roughness") || !material["roughness"].is_number() ||
                (material.contains("baseColorTexture") &&
                 !material["baseColorTexture"].is_string()) ||
                (material.contains("normalTexture") &&
                 !material["normalTexture"].is_string()) ||
                (material.contains("normalStrength") &&
                 !material["normalStrength"].is_number()) ||
                (material.contains("roughnessTexture") &&
                 !material["roughnessTexture"].is_string()) ||
                (material.contains("metallicTexture") &&
                 !material["metallicTexture"].is_string()) ||
                (material.contains("pbrTexturePacking") &&
                 !material["pbrTexturePacking"].is_string()) ||
                (material.contains("blendMode") && !material["blendMode"].is_string()) ||
                (material.contains("alphaCutoff") && !material["alphaCutoff"].is_number()) ||
                (material.contains("cullMode") && !material["cullMode"].is_string()) ||
                (material.contains("depthWrite") && !material["depthWrite"].is_boolean())) {
                SetError(error, "Scene MaterialOverride component is invalid.");
                return false;
            }
            MaterialOverrideComponent component{};
            component.enabled = material["enabled"].get<bool>();
            component.metallic = material["metallic"].get<float>();
            component.roughness = material["roughness"].get<float>();
            if (material.contains("baseColorTexture")) {
                component.baseColorTexturePath =
                    material["baseColorTexture"].get<std::string>();
            }
            if (material.contains("normalTexture")) {
                component.normalTexturePath = material["normalTexture"].get<std::string>();
            }
            if (material.contains("normalStrength")) {
                component.normalStrength = material["normalStrength"].get<float>();
            }
            if (material.contains("roughnessTexture")) {
                component.roughnessTexturePath =
                    material["roughnessTexture"].get<std::string>();
            }
            if (material.contains("metallicTexture")) {
                component.metallicTexturePath =
                    material["metallicTexture"].get<std::string>();
            }
            if (material.contains("pbrTexturePacking")) {
                const std::string packing = material["pbrTexturePacking"].get<std::string>();
                if (packing == "Separate") {
                    component.pbrTexturePacking = MaterialPbrTexturePacking::Separate;
                } else if (packing == "ORM") {
                    component.pbrTexturePacking =
                        MaterialPbrTexturePacking::OcclusionRoughnessMetallic;
                } else if (packing == "MetallicRoughness") {
                    component.pbrTexturePacking =
                        MaterialPbrTexturePacking::MetallicRoughness;
                } else {
                    SetError(error, "Scene MaterialOverride PBR packing is invalid.");
                    return false;
                }
            }
            if (material.contains("blendMode")) {
                const std::string blendMode = material["blendMode"].get<std::string>();
                if (blendMode == "Opaque") {
                    component.blendMode = MaterialSurfaceBlendMode::Opaque;
                } else if (blendMode == "Cutout") {
                    component.blendMode = MaterialSurfaceBlendMode::Cutout;
                } else if (blendMode == "Transparent") {
                    component.blendMode = MaterialSurfaceBlendMode::Transparent;
                } else {
                    SetError(error, "Scene MaterialOverride blend mode is invalid.");
                    return false;
                }
            }
            if (material.contains("alphaCutoff")) {
                component.alphaCutoff = material["alphaCutoff"].get<float>();
            }
            if (material.contains("cullMode")) {
                const std::string cullMode = material["cullMode"].get<std::string>();
                if (cullMode == "None") {
                    component.cullMode = MaterialSurfaceCullMode::None;
                } else if (cullMode == "Front") {
                    component.cullMode = MaterialSurfaceCullMode::Front;
                } else if (cullMode == "Back") {
                    component.cullMode = MaterialSurfaceCullMode::Back;
                } else {
                    SetError(error, "Scene MaterialOverride cull mode is invalid.");
                    return false;
                }
            }
            if (material.contains("depthWrite")) {
                component.depthWrite = material["depthWrite"].get<bool>();
            }
            if (!DecodeFloat4(material["baseColor"], component.baseColor) ||
                component.baseColor.x < 0.0f || component.baseColor.y < 0.0f ||
                component.baseColor.z < 0.0f || component.baseColor.w < 0.0f ||
                component.baseColor.w > 1.0f || !std::isfinite(component.metallic) ||
                component.metallic < 0.0f || component.metallic > 1.0f ||
                !std::isfinite(component.roughness) || component.roughness < 0.0f ||
                component.roughness > 1.0f || component.baseColorTexturePath.size() > 1024u ||
                component.baseColorTexturePath.find('\0') != std::string::npos ||
                component.normalTexturePath.size() > 1024u ||
                component.normalTexturePath.find('\0') != std::string::npos ||
                !std::isfinite(component.normalStrength) || component.normalStrength < 0.0f ||
                component.roughnessTexturePath.size() > 1024u ||
                component.roughnessTexturePath.find('\0') != std::string::npos ||
                component.metallicTexturePath.size() > 1024u ||
                component.metallicTexturePath.find('\0') != std::string::npos ||
                !std::isfinite(component.alphaCutoff) || component.alphaCutoff < 0.0f ||
                component.alphaCutoff > 1.0f) {
                SetError(error, "Scene MaterialOverride settings are invalid.");
                return false;
            }
            entity.materialOverride = component;
        }
        if (encoded["components"].contains("Camera")) {
            const Json& camera = encoded["components"]["Camera"];
            if (!camera.is_object() || !camera.contains("enabled") ||
                !camera["enabled"].is_boolean() || !camera.contains("primary") ||
                !camera["primary"].is_boolean() || !camera.contains("projection") ||
                !camera["projection"].is_string() || !camera.contains("fieldOfView") ||
                !camera["fieldOfView"].is_number() ||
                !camera.contains("orthographicHeight") ||
                !camera["orthographicHeight"].is_number() || !camera.contains("nearClip") ||
                !camera["nearClip"].is_number() || !camera.contains("farClip") ||
                !camera["farClip"].is_number()) {
                SetError(error, "Scene Camera component is invalid.");
                return false;
            }
            CameraComponent component{};
            component.enabled = camera["enabled"].get<bool>();
            component.primary = camera["primary"].get<bool>();
            const std::string projection = camera["projection"].get<std::string>();
            if (projection == "Perspective") {
                component.projection = CameraProjection::Perspective;
            } else if (projection == "Orthographic") {
                component.projection = CameraProjection::Orthographic;
            } else {
                SetError(error, "Scene Camera projection is invalid.");
                return false;
            }
            component.fieldOfViewDegrees = camera["fieldOfView"].get<float>();
            component.orthographicHeight = camera["orthographicHeight"].get<float>();
            component.nearClip = camera["nearClip"].get<float>();
            component.farClip = camera["farClip"].get<float>();
            if (!std::isfinite(component.fieldOfViewDegrees) ||
                component.fieldOfViewDegrees < 1.0f ||
                component.fieldOfViewDegrees > 179.0f ||
                !std::isfinite(component.orthographicHeight) ||
                component.orthographicHeight < 0.001f ||
                !std::isfinite(component.nearClip) || component.nearClip < 0.001f ||
                !std::isfinite(component.farClip) ||
                component.farClip <= component.nearClip) {
                SetError(error, "Scene Camera settings are invalid.");
                return false;
            }
            entity.camera = component;
        }
        if (encoded["components"].contains("Light")) {
            const Json& light = encoded["components"]["Light"];
            if (!light.is_object() || !light.contains("enabled") ||
                !light["enabled"].is_boolean() || !light.contains("type") ||
                !light["type"].is_string() || !light.contains("color") ||
                !light.contains("intensity") || !light["intensity"].is_number() ||
                !light.contains("range") || !light["range"].is_number() ||
                !light.contains("innerAngle") || !light["innerAngle"].is_number() ||
                !light.contains("outerAngle") || !light["outerAngle"].is_number()) {
                SetError(error, "Scene Light component is invalid.");
                return false;
            }
            LightComponent component{};
            const std::string type = light["type"].get<std::string>();
            if (type == "Directional") {
                component.type = LightType::Directional;
            } else if (type == "Point") {
                component.type = LightType::Point;
            } else if (type == "Spot") {
                component.type = LightType::Spot;
            } else {
                SetError(error, "Scene Light type is invalid.");
                return false;
            }
            component.enabled = light["enabled"].get<bool>();
            if (!DecodeFloat3(light["color"], component.color)) {
                SetError(error, "Scene Light color is invalid.");
                return false;
            }
            component.intensity = light["intensity"].get<float>();
            component.range = light["range"].get<float>();
            component.innerAngleDegrees = light["innerAngle"].get<float>();
            component.outerAngleDegrees = light["outerAngle"].get<float>();
            if (component.color.x < 0.0f || component.color.y < 0.0f ||
                component.color.z < 0.0f || !std::isfinite(component.intensity) ||
                component.intensity < 0.0f || !std::isfinite(component.range) ||
                component.range < 0.001f || !std::isfinite(component.innerAngleDegrees) ||
                component.innerAngleDegrees < 0.0f ||
                !std::isfinite(component.outerAngleDegrees) ||
                component.outerAngleDegrees <= component.innerAngleDegrees ||
                component.outerAngleDegrees > 179.0f) {
                SetError(error, "Scene Light settings are invalid.");
                return false;
            }
            entity.light = component;
        }
        if (encoded["components"].contains("AudioSource")) {
            const Json& source = encoded["components"]["AudioSource"];
            if (!source.is_object() || !source.contains("enabled") ||
                !source["enabled"].is_boolean() || !source.contains("clip") ||
                !source["clip"].is_string() || !source.contains("playOnAwake") ||
                !source["playOnAwake"].is_boolean() || !source.contains("loop") ||
                !source["loop"].is_boolean() || !source.contains("volume") ||
                !source["volume"].is_number() ||
                (source.contains("pitch") && !source["pitch"].is_number()) ||
                !source.contains("spatial") ||
                !source["spatial"].is_boolean() || !source.contains("minDistance") ||
                !source["minDistance"].is_number() || !source.contains("maxDistance") ||
                !source["maxDistance"].is_number()) {
                SetError(error, "Scene AudioSource component is invalid.");
                return false;
            }
            AudioSourceComponent component{};
            component.enabled = source["enabled"].get<bool>();
            component.clipPath = source["clip"].get<std::string>();
            component.playOnAwake = source["playOnAwake"].get<bool>();
            component.loop = source["loop"].get<bool>();
            component.volume = source["volume"].get<float>();
            component.pitch = source.value("pitch", 1.0f);
            component.spatial = source["spatial"].get<bool>();
            component.minDistance = source["minDistance"].get<float>();
            component.maxDistance = source["maxDistance"].get<float>();
            if (component.clipPath.size() > 1024u ||
                component.clipPath.find('\0') != std::string::npos ||
                !std::isfinite(component.volume) || component.volume < 0.0f ||
                component.volume > 1.0f || !std::isfinite(component.pitch) ||
                component.pitch < AudioSourceComponent::kMinPitch ||
                component.pitch > AudioSourceComponent::kMaxPitch ||
                !std::isfinite(component.minDistance) ||
                !std::isfinite(component.maxDistance) || component.minDistance < 0.0f ||
                component.maxDistance <= component.minDistance) {
                SetError(error, "Scene AudioSource settings are invalid.");
                return false;
            }
            entity.audioSource = std::move(component);
        }
        if (encoded["components"].contains("AudioListener")) {
            const Json& listener = encoded["components"]["AudioListener"];
            if (!listener.is_object() || !listener.contains("enabled") ||
                !listener["enabled"].is_boolean()) {
                SetError(error, "Scene AudioListener component is invalid.");
                return false;
            }
            entity.audioListener = AudioListenerComponent{listener["enabled"].get<bool>()};
        }
        if (encoded["components"].contains("Animator")) {
            const Json& animator = encoded["components"]["Animator"];
            if (!animator.is_object() || !animator.contains("enabled") ||
                !animator["enabled"].is_boolean() || !animator.contains("clip") ||
                !animator["clip"].is_string() || !animator.contains("playOnAwake") ||
                !animator["playOnAwake"].is_boolean() || !animator.contains("loop") ||
                !animator["loop"].is_boolean() || !animator.contains("speed") ||
                !animator["speed"].is_number()) {
                SetError(error, "Scene Animator component is invalid.");
                return false;
            }
            AnimatorComponent component{};
            component.enabled = animator["enabled"].get<bool>();
            component.clip = animator["clip"].get<std::string>();
            component.playOnAwake = animator["playOnAwake"].get<bool>();
            component.loop = animator["loop"].get<bool>();
            component.speed = animator["speed"].get<float>();
            if (animator.contains("lockRootPosition")) {
                if (!animator["lockRootPosition"].is_boolean()) {
                    SetError(error, "Scene Animator settings are invalid.");
                    return false;
                }
                component.lockRootPosition =
                    animator["lockRootPosition"].get<bool>();
            }
            if (component.clip.size() > 256u || component.clip.find('\0') != std::string::npos ||
                !std::isfinite(component.speed) || component.speed < 0.0f ||
                component.speed > 100.0f) {
                SetError(error, "Scene Animator settings are invalid.");
                return false;
            }
            entity.animator = std::move(component);
        }
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
            if (component.text.size() > 4096u ||
                component.text.find('\0') != std::string::npos ||
                std::abs(component.position.x) > 1000000.0f ||
                std::abs(component.position.y) > 1000000.0f ||
                !std::isfinite(component.fontSize) || component.fontSize < 1.0f ||
                component.fontSize > 512.0f || component.color.x < 0.0f ||
                component.color.x > 1.0f || component.color.y < 0.0f ||
                component.color.y > 1.0f || component.color.z < 0.0f ||
                component.color.z > 1.0f || component.color.w < 0.0f ||
                component.color.w > 1.0f) {
                SetError(error, "Scene Text settings are invalid.");
                return false;
            }
            entity.text = std::move(component);
        }
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
                !validColor(component.disabledColor)) {
                SetError(error, "Scene Button settings are invalid.");
                return false;
            }
            entity.button = std::move(component);
        }
        const auto decodeScript = [&](const Json& behavior, BehaviorComponent& component,
                                      bool allowUnassigned) -> bool {
            if (!behavior.is_object() || !behavior.contains("enabled") ||
                !behavior["enabled"].is_boolean() || !behavior.contains("type") ||
                !behavior["type"].is_string()) {
                SetError(error, "Scene Script component is invalid.");
                return false;
            }
            component.enabled = behavior["enabled"].get<bool>();
            component.type = behavior["type"].get<std::string>();
            if (behavior.contains("script")) {
                if (!behavior["script"].is_string()) {
                    SetError(error, "Scene Behavior script asset path is invalid.");
                    return false;
                }
                component.scriptAssetPath = behavior["script"].get<std::string>();
            }
            if (behavior.contains("properties")) {
                const Json& properties = behavior["properties"];
                if (!properties.is_object() || properties.size() > 128u) {
                    SetError(error, "Scene Script properties are invalid.");
                    return false;
                }
                for (const auto& [name, encodedProperty] : properties.items()) {
                    if (name.empty() || name.size() > 128u ||
                        name.find('\0') != std::string::npos ||
                        !encodedProperty.is_object() ||
                        !encodedProperty.contains("type") ||
                        !encodedProperty["type"].is_string() ||
                        !encodedProperty.contains("value")) {
                        SetError(error, "Scene Script property is invalid.");
                        return false;
                    }
                    ScriptPropertyValue property{};
                    property.name = name;
                    const std::string propertyType =
                        encodedProperty["type"].get<std::string>();
                    if (propertyType == "Float") {
                        if (!encodedProperty["value"].is_number()) {
                            SetError(error, "Scene Script float property is invalid.");
                            return false;
                        }
                        property.type = ScriptPropertyType::Float;
                        property.floatValue =
                            encodedProperty["value"].get<float>();
                        if (!std::isfinite(property.floatValue)) {
                            SetError(error, "Scene Script float property is invalid.");
                            return false;
                        }
                    } else if (propertyType == "Entity") {
                        property.type = ScriptPropertyType::Entity;
                        if (!encodedProperty["value"].is_null() &&
                            (!encodedProperty["value"].is_string() ||
                             !EntityId::TryParse(
                                 encodedProperty["value"].get_ref<const std::string&>(),
                                 property.entityValue))) {
                            SetError(error, "Scene Script Entity property is invalid.");
                            return false;
                        }
                    } else if (propertyType == "Boolean") {
                        if (!encodedProperty["value"].is_boolean()) {
                            SetError(error, "Scene Script Boolean property is invalid.");
                            return false;
                        }
                        property.type = ScriptPropertyType::Boolean;
                        property.booleanValue = encodedProperty["value"].get<bool>();
                    } else if (propertyType == "Integer") {
                        const Json& encodedValue = encodedProperty["value"];
                        if ((!encodedValue.is_number_integer() &&
                             !encodedValue.is_number_unsigned()) ||
                            (encodedValue.is_number_unsigned() &&
                             encodedValue.get<uint64_t>() >
                                 static_cast<uint64_t>((std::numeric_limits<int32_t>::max)())) ||
                            (encodedValue.is_number_integer() &&
                             !encodedValue.is_number_unsigned() &&
                             (encodedValue.get<int64_t>() <
                                  static_cast<int64_t>((std::numeric_limits<int32_t>::min)()) ||
                              encodedValue.get<int64_t>() >
                                  static_cast<int64_t>((std::numeric_limits<int32_t>::max)())))) {
                            SetError(error, "Scene Script Integer property is invalid.");
                            return false;
                        }
                        property.type = ScriptPropertyType::Integer;
                        property.integerValue = encodedValue.get<int32_t>();
                    } else if (propertyType == "Vector3") {
                        DirectX::XMFLOAT3 value{};
                        if (!DecodeFloat3(encodedProperty["value"], value)) {
                            SetError(error, "Scene Script Vector3 property is invalid.");
                            return false;
                        }
                        property.type = ScriptPropertyType::Vector3;
                        property.vector3Value = {value.x, value.y, value.z};
                    } else if (propertyType == "String") {
                        if (!encodedProperty["value"].is_string()) {
                            SetError(error, "Scene Script String property is invalid.");
                            return false;
                        }
                        property.type = ScriptPropertyType::String;
                        property.stringValue = encodedProperty["value"].get<std::string>();
                        if (property.stringValue.size() > 1024u ||
                            property.stringValue.find('\0') != std::string::npos) {
                            SetError(error, "Scene Script String property is invalid.");
                            return false;
                        }
                    } else if (propertyType == "AnimationClip") {
                        if (!encodedProperty["value"].is_string()) {
                            SetError(error, "Scene Script AnimationClip property is invalid.");
                            return false;
                        }
                        property.type = ScriptPropertyType::AnimationClip;
                        property.stringValue = encodedProperty["value"].get<std::string>();
                        if (property.stringValue.size() > 1024u ||
                            property.stringValue.find('\0') != std::string::npos) {
                            SetError(error, "Scene Script AnimationClip property is invalid.");
                            return false;
                        }
                    } else if (propertyType == "InputAction") {
                        if (!encodedProperty["value"].is_string()) {
                            SetError(error, "Scene Script InputAction property is invalid.");
                            return false;
                        }
                        property.type = ScriptPropertyType::InputAction;
                        property.stringValue = encodedProperty["value"].get<std::string>();
                        if (property.stringValue.size() > 64u ||
                            property.stringValue.find('\0') != std::string::npos) {
                            SetError(error, "Scene Script InputAction property is invalid.");
                            return false;
                        }
                    } else if (propertyType == "Scene") {
                        if (!encodedProperty["value"].is_string()) {
                            SetError(error, "Scene Script Scene property is invalid.");
                            return false;
                        }
                        property.type = ScriptPropertyType::Scene;
                        property.stringValue = encodedProperty["value"].get<std::string>();
                        if (property.stringValue.size() > 1024u ||
                            property.stringValue.find('\0') != std::string::npos) {
                            SetError(error, "Scene Script Scene property is invalid.");
                            return false;
                        }
                    } else {
                        SetError(error, "Scene Script property type is invalid.");
                        return false;
                    }
                    component.properties.push_back(std::move(property));
                }
            }
            if ((!allowUnassigned && component.type.empty()) ||
                component.type.size() > 128u ||
                component.type.find('\0') != std::string::npos ||
                component.scriptAssetPath.size() > 1024u ||
                component.scriptAssetPath.find('\0') != std::string::npos ||
                (component.type.empty() &&
                 (!component.scriptAssetPath.empty() || !component.properties.empty()))) {
                SetError(error, "Scene Script type is invalid.");
                return false;
            }
            return true;
        };
        if (encoded["components"].contains("Scripts")) {
            const Json& scripts = encoded["components"]["Scripts"];
            if (!scripts.is_array() || scripts.size() > 1024u) {
                SetError(error, "Scene Scripts component list is invalid.");
                return false;
            }
            entity.scripts.reserve(scripts.size());
            for (const Json& encodedScript : scripts) {
                BehaviorComponent component{};
                if (!decodeScript(encodedScript, component, true)) {
                    return false;
                }
                entity.scripts.push_back(std::move(component));
            }
        } else if (encoded["components"].contains("Behavior")) {
            BehaviorComponent component{};
            if (!decodeScript(encoded["components"]["Behavior"], component, false)) {
                return false;
            }
            entity.scripts.push_back(std::move(component));
        }
        if (encoded["components"].contains("BoxCollider")) {
            const Json& collider = encoded["components"]["BoxCollider"];
            BoxColliderComponent component{};
            if (!collider.is_object() || !collider.contains("enabled") ||
                !collider["enabled"].is_boolean() || !collider.contains("center") ||
                !collider.contains("size") || !collider.contains("isTrigger") ||
                !collider["isTrigger"].is_boolean() ||
                !DecodeFloat3(collider["center"], component.center) ||
                !DecodeFloat3(collider["size"], component.size)) {
                SetError(error, "Scene BoxCollider component is invalid.");
                return false;
            }
            component.enabled = collider["enabled"].get<bool>();
            component.isTrigger = collider["isTrigger"].get<bool>();
            if (component.size.x < 0.001f || component.size.y < 0.001f ||
                component.size.z < 0.001f || component.size.x > 1000000.0f ||
                component.size.y > 1000000.0f || component.size.z > 1000000.0f) {
                SetError(error, "Scene BoxCollider size is invalid.");
                return false;
            }
            entity.boxCollider = component;
        }
        if (encoded["components"].contains("CharacterController")) {
            const Json& controller = encoded["components"]["CharacterController"];
            CharacterControllerComponent component{};
            if (!controller.is_object() || !controller.contains("enabled") ||
                !controller["enabled"].is_boolean() || !controller.contains("center") ||
                !controller.contains("radius") || !controller["radius"].is_number() ||
                !controller.contains("height") || !controller["height"].is_number() ||
                !controller.contains("slopeLimit") ||
                !controller["slopeLimit"].is_number() || !controller.contains("stepOffset") ||
                !controller["stepOffset"].is_number() || !controller.contains("skinWidth") ||
                !controller["skinWidth"].is_number() ||
                !controller.contains("minMoveDistance") ||
                !controller["minMoveDistance"].is_number() ||
                !DecodeFloat3(controller["center"], component.center)) {
                SetError(error, "Scene CharacterController component is invalid.");
                return false;
            }
            component.enabled = controller["enabled"].get<bool>();
            component.radius = controller["radius"].get<float>();
            component.height = controller["height"].get<float>();
            component.slopeLimitDegrees = controller["slopeLimit"].get<float>();
            component.stepOffset = controller["stepOffset"].get<float>();
            component.skinWidth = controller["skinWidth"].get<float>();
            component.minMoveDistance = controller["minMoveDistance"].get<float>();
            if (!std::isfinite(component.radius) || !std::isfinite(component.height) ||
                !std::isfinite(component.slopeLimitDegrees) ||
                !std::isfinite(component.stepOffset) || !std::isfinite(component.skinWidth) ||
                !std::isfinite(component.minMoveDistance) || component.radius < 0.001f ||
                component.height < component.radius * 2.0f ||
                component.slopeLimitDegrees < 0.0f ||
                component.slopeLimitDegrees > 90.0f || component.stepOffset < 0.0f ||
                component.stepOffset > component.height || component.skinWidth < 0.0f ||
                component.skinWidth >= component.radius || component.minMoveDistance < 0.0f) {
                SetError(error, "Scene CharacterController settings are invalid.");
                return false;
            }
            entity.characterController = component;
        }
        entities.push_back(std::move(entity));
    }
    return world.ReplaceEntities(std::move(entities), error);
}

bool WorldSerializer::Save(const World& world, const std::filesystem::path& path,
                           std::string* error) {
    try {
        if (path.has_parent_path()) {
            std::error_code directoryError;
            std::filesystem::create_directories(path.parent_path(), directoryError);
            if (directoryError) {
                SetError(error, "Failed to create the scene directory.");
                return false;
            }
        }
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            SetError(error, "Failed to open the scene for writing.");
            return false;
        }
        const std::string serialized = Serialize(world);
        stream.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        if (!stream) {
            SetError(error, "Failed to write the scene.");
            return false;
        }
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool WorldSerializer::Load(const std::filesystem::path& path, World& world,
                           std::string* error) {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            SetError(error, "Failed to open the scene for reading.");
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        return Deserialize(text, world, error);
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }
}
