#include "internal/WorldSerializerComponentDecoders.h"

using namespace WorldSerializerJson;

namespace WorldSerializerDecoding {
namespace {
bool DecodeMeshRendererComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
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
    return true;
}

bool IsMaterialDocumentValid(const Json& material) {
    return material.is_object() && material.contains("enabled") &&
           material["enabled"].is_boolean() && material.contains("baseColor") &&
           material.contains("metallic") && material["metallic"].is_number() &&
           material.contains("roughness") && material["roughness"].is_number() &&
           (!material.contains("baseColorTexture") ||
            material["baseColorTexture"].is_string()) &&
           (!material.contains("normalTexture") || material["normalTexture"].is_string()) &&
           (!material.contains("normalStrength") || material["normalStrength"].is_number()) &&
           (!material.contains("roughnessTexture") ||
            material["roughnessTexture"].is_string()) &&
           (!material.contains("metallicTexture") ||
            material["metallicTexture"].is_string()) &&
           (!material.contains("pbrTexturePacking") ||
            material["pbrTexturePacking"].is_string()) &&
           (!material.contains("blendMode") || material["blendMode"].is_string()) &&
           (!material.contains("alphaCutoff") || material["alphaCutoff"].is_number()) &&
           (!material.contains("cullMode") || material["cullMode"].is_string()) &&
           (!material.contains("depthWrite") || material["depthWrite"].is_boolean());
}

void DecodeMaterialTextures(const Json& material, MaterialOverrideComponent& component) {
    if (material.contains("baseColorTexture")) {
        component.baseColorTexturePath = material["baseColorTexture"].get<std::string>();
    }
    if (material.contains("normalTexture")) {
        component.normalTexturePath = material["normalTexture"].get<std::string>();
    }
    if (material.contains("normalStrength")) {
        component.normalStrength = material["normalStrength"].get<float>();
    }
    if (material.contains("roughnessTexture")) {
        component.roughnessTexturePath = material["roughnessTexture"].get<std::string>();
    }
    if (material.contains("metallicTexture")) {
        component.metallicTexturePath = material["metallicTexture"].get<std::string>();
    }
}

bool DecodePbrPacking(const Json& material, MaterialOverrideComponent& component,
                      std::string* error) {
    if (!material.contains("pbrTexturePacking")) {
        return true;
    }
    const std::string value = material["pbrTexturePacking"].get<std::string>();
    if (value == "Separate") {
        component.pbrTexturePacking = MaterialPbrTexturePacking::Separate;
    } else if (value == "ORM") {
        component.pbrTexturePacking =
            MaterialPbrTexturePacking::OcclusionRoughnessMetallic;
    } else if (value == "MetallicRoughness") {
        component.pbrTexturePacking = MaterialPbrTexturePacking::MetallicRoughness;
    } else {
        SetError(error, "Scene MaterialOverride PBR packing is invalid.");
        return false;
    }
    return true;
}

bool DecodeBlendMode(const Json& material, MaterialOverrideComponent& component,
                     std::string* error) {
    if (!material.contains("blendMode")) {
        return true;
    }
    const std::string value = material["blendMode"].get<std::string>();
    if (value == "Opaque") {
        component.blendMode = MaterialSurfaceBlendMode::Opaque;
    } else if (value == "Cutout") {
        component.blendMode = MaterialSurfaceBlendMode::Cutout;
    } else if (value == "Transparent") {
        component.blendMode = MaterialSurfaceBlendMode::Transparent;
    } else {
        SetError(error, "Scene MaterialOverride blend mode is invalid.");
        return false;
    }
    return true;
}

bool DecodeCullMode(const Json& material, MaterialOverrideComponent& component,
                    std::string* error) {
    if (!material.contains("cullMode")) {
        return true;
    }
    const std::string value = material["cullMode"].get<std::string>();
    if (value == "None") {
        component.cullMode = MaterialSurfaceCullMode::None;
    } else if (value == "Front") {
        component.cullMode = MaterialSurfaceCullMode::Front;
    } else if (value == "Back") {
        component.cullMode = MaterialSurfaceCullMode::Back;
    } else {
        SetError(error, "Scene MaterialOverride cull mode is invalid.");
        return false;
    }
    return true;
}

bool DecodeMaterialSurface(const Json& material, MaterialOverrideComponent& component,
                           std::string* error) {
    if (!DecodePbrPacking(material, component, error) ||
        !DecodeBlendMode(material, component, error) ||
        !DecodeCullMode(material, component, error)) {
        return false;
    }
    if (material.contains("alphaCutoff")) {
        component.alphaCutoff = material["alphaCutoff"].get<float>();
    }
    if (material.contains("depthWrite")) {
        component.depthWrite = material["depthWrite"].get<bool>();
    }
    return true;
}

bool IsMaterialSettingsValid(const MaterialOverrideComponent& component) {
    return component.baseColor.x >= 0.0f && component.baseColor.y >= 0.0f &&
           component.baseColor.z >= 0.0f && component.baseColor.w >= 0.0f &&
           component.baseColor.w <= 1.0f && std::isfinite(component.metallic) &&
           component.metallic >= 0.0f && component.metallic <= 1.0f &&
           std::isfinite(component.roughness) && component.roughness >= 0.0f &&
           component.roughness <= 1.0f &&
           component.baseColorTexturePath.size() <= 1024u &&
           component.baseColorTexturePath.find('\0') == std::string::npos &&
           component.normalTexturePath.size() <= 1024u &&
           component.normalTexturePath.find('\0') == std::string::npos &&
           std::isfinite(component.normalStrength) && component.normalStrength >= 0.0f &&
           component.roughnessTexturePath.size() <= 1024u &&
           component.roughnessTexturePath.find('\0') == std::string::npos &&
           component.metallicTexturePath.size() <= 1024u &&
           component.metallicTexturePath.find('\0') == std::string::npos &&
           std::isfinite(component.alphaCutoff) && component.alphaCutoff >= 0.0f &&
           component.alphaCutoff <= 1.0f;
}

bool DecodeMaterialOverrideComponent(const Json& encoded, WorldEntity& entity,
                                     std::string* error) {
    if (!encoded["components"].contains("MaterialOverride")) {
        return true;
    }
    const Json& material = encoded["components"]["MaterialOverride"];
    if (!IsMaterialDocumentValid(material)) {
        SetError(error, "Scene MaterialOverride component is invalid.");
        return false;
    }
    MaterialOverrideComponent component{};
    component.enabled = material["enabled"].get<bool>();
    component.metallic = material["metallic"].get<float>();
    component.roughness = material["roughness"].get<float>();
    DecodeMaterialTextures(material, component);
    if (!DecodeMaterialSurface(material, component, error)) {
        return false;
    }
    if (!DecodeFloat4(material["baseColor"], component.baseColor) ||
        !IsMaterialSettingsValid(component)) {
        SetError(error, "Scene MaterialOverride settings are invalid.");
        return false;
    }
    entity.materialOverride = component;
    return true;
}

bool DecodeCameraComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
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
    return true;
}

bool DecodeLightComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
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
    return true;
}

} // namespace

bool DecodeRenderingComponents(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!DecodeMeshRendererComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeMaterialOverrideComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeCameraComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeLightComponent(encoded, entity, error)) {
        return false;
    }
    return true;
}

} // namespace WorldSerializerDecoding
