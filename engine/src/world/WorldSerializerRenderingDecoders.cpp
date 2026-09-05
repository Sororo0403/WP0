#include "internal/WorldSerializerComponentDecoders.h"

using namespace WorldSerializerJson;

namespace WorldSerializerDecoding {
namespace {
bool DecodeMeshRendererFields(const Json& renderer,
                              MeshRendererComponent& component) {
    if (!renderer.is_object() || !renderer.contains("enabled") ||
        !renderer["enabled"].is_boolean() || !renderer.contains("source") ||
        !renderer["source"].is_string() || !renderer.contains("primitive") ||
        !renderer["primitive"].is_number_unsigned() || !renderer.contains("model") ||
        !renderer["model"].is_string()) {
        return false;
    }
    component.enabled = renderer["enabled"].get<bool>();
    component.modelPath = renderer["model"].get<std::string>();
    return true;
}

bool DecodeMeshSource(const Json& renderer, MeshSourceType& sourceType) {
    const std::string source = renderer["source"].get<std::string>();
    if (source == "Primitive") {
        sourceType = MeshSourceType::Primitive;
        return true;
    }
    if (source == "Model") {
        sourceType = MeshSourceType::Model;
        return true;
    }
    return false;
}

bool DecodeMeshPrimitive(const Json& renderer, MeshPrimitive& primitive) {
    const uint64_t value = renderer["primitive"].get<uint64_t>();
    if (value > static_cast<uint64_t>(MeshPrimitive::Cylinder)) {
        return false;
    }
    primitive = static_cast<MeshPrimitive>(value);
    return true;
}

bool DecodeMeshRendererComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("MeshRenderer")) {
        return true;
    }
    const Json& renderer = encoded["components"]["MeshRenderer"];
    MeshRendererComponent component{};
    if (!DecodeMeshRendererFields(renderer, component)) {
        SetError(error, "Scene MeshRenderer component is invalid.");
        return false;
    }
    if (!DecodeMeshSource(renderer, component.sourceType)) {
        SetError(error, "Scene MeshRenderer source is invalid.");
        return false;
    }
    if (!DecodeMeshPrimitive(renderer, component.primitive)) {
        SetError(error, "Scene MeshRenderer primitive is invalid.");
        return false;
    }
    if (component.modelPath.size() > 1024u ||
        component.modelPath.find('\0') != std::string::npos) {
        SetError(error, "Scene MeshRenderer model path is invalid.");
        return false;
    }
    entity.meshRenderer = std::move(component);
    return true;
}

bool HasRequiredMaterialFields(const Json& material) {
    return material.is_object() && material.contains("enabled") &&
           material["enabled"].is_boolean() && material.contains("baseColor") &&
           material.contains("metallic") && material["metallic"].is_number() &&
           material.contains("roughness") && material["roughness"].is_number();
}

bool HasValidMaterialTextureFields(const Json& material) {
    return
           (!material.contains("baseColorTexture") ||
            material["baseColorTexture"].is_string()) &&
           (!material.contains("normalTexture") || material["normalTexture"].is_string()) &&
           (!material.contains("normalStrength") || material["normalStrength"].is_number()) &&
           (!material.contains("roughnessTexture") ||
            material["roughnessTexture"].is_string()) &&
           (!material.contains("metallicTexture") ||
            material["metallicTexture"].is_string());
}

bool HasValidMaterialSurfaceFields(const Json& material) {
    return
           (!material.contains("pbrTexturePacking") ||
            material["pbrTexturePacking"].is_string()) &&
           (!material.contains("blendMode") || material["blendMode"].is_string()) &&
           (!material.contains("alphaCutoff") || material["alphaCutoff"].is_number()) &&
           (!material.contains("cullMode") || material["cullMode"].is_string()) &&
           (!material.contains("depthWrite") || material["depthWrite"].is_boolean());
}

bool IsMaterialDocumentValid(const Json& material) {
    return HasRequiredMaterialFields(material) &&
           HasValidMaterialTextureFields(material) &&
           HasValidMaterialSurfaceFields(material);
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

bool IsMaterialBaseValid(const MaterialOverrideComponent& component) {
    return component.baseColor.x >= 0.0f && component.baseColor.y >= 0.0f &&
           component.baseColor.z >= 0.0f && component.baseColor.w >= 0.0f &&
           component.baseColor.w <= 1.0f && std::isfinite(component.metallic) &&
           component.metallic >= 0.0f && component.metallic <= 1.0f &&
           std::isfinite(component.roughness) && component.roughness >= 0.0f &&
           component.roughness <= 1.0f;
}

bool IsMaterialTextureDataValid(const MaterialOverrideComponent& component) {
    return component.baseColorTexturePath.size() <= 1024u &&
           component.baseColorTexturePath.find('\0') == std::string::npos &&
           component.normalTexturePath.size() <= 1024u &&
           component.normalTexturePath.find('\0') == std::string::npos &&
           std::isfinite(component.normalStrength) && component.normalStrength >= 0.0f &&
           component.roughnessTexturePath.size() <= 1024u &&
           component.roughnessTexturePath.find('\0') == std::string::npos &&
           component.metallicTexturePath.size() <= 1024u &&
           component.metallicTexturePath.find('\0') == std::string::npos;
}

bool IsMaterialSettingsValid(const MaterialOverrideComponent& component) {
    return IsMaterialBaseValid(component) && IsMaterialTextureDataValid(component) &&
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

bool DecodeCameraIdentity(const Json& camera, CameraComponent& component) {
    if (!camera.is_object() || !camera.contains("enabled") ||
        !camera["enabled"].is_boolean() || !camera.contains("primary") ||
        !camera["primary"].is_boolean() || !camera.contains("projection") ||
        !camera["projection"].is_string()) {
        return false;
    }
    component.enabled = camera["enabled"].get<bool>();
    component.primary = camera["primary"].get<bool>();
    return true;
}

bool DecodeCameraNumbers(const Json& camera, CameraComponent& component) {
    if (!camera.contains("fieldOfView") || !camera["fieldOfView"].is_number() ||
        !camera.contains("orthographicHeight") ||
        !camera["orthographicHeight"].is_number() || !camera.contains("nearClip") ||
        !camera["nearClip"].is_number() || !camera.contains("farClip") ||
        !camera["farClip"].is_number()) {
        return false;
    }
    component.fieldOfViewDegrees = camera["fieldOfView"].get<float>();
    component.orthographicHeight = camera["orthographicHeight"].get<float>();
    component.nearClip = camera["nearClip"].get<float>();
    component.farClip = camera["farClip"].get<float>();
    return true;
}

bool DecodeCameraProjection(const Json& camera, CameraProjection& projection) {
    const std::string value = camera["projection"].get<std::string>();
    if (value == "Perspective") {
        projection = CameraProjection::Perspective;
        return true;
    }
    if (value == "Orthographic") {
        projection = CameraProjection::Orthographic;
        return true;
    }
    return false;
}

bool IsCameraSettingsValid(const CameraComponent& component) {
    return std::isfinite(component.fieldOfViewDegrees) &&
           component.fieldOfViewDegrees >= 1.0f &&
           component.fieldOfViewDegrees <= 179.0f &&
           std::isfinite(component.orthographicHeight) &&
           component.orthographicHeight >= 0.001f && std::isfinite(component.nearClip) &&
           component.nearClip >= 0.001f && std::isfinite(component.farClip) &&
           component.farClip > component.nearClip;
}

bool DecodeCameraComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("Camera")) {
        return true;
    }
    const Json& camera = encoded["components"]["Camera"];
    CameraComponent component{};
    if (!DecodeCameraIdentity(camera, component) ||
        !DecodeCameraNumbers(camera, component)) {
        SetError(error, "Scene Camera component is invalid.");
        return false;
    }
    if (!DecodeCameraProjection(camera, component.projection)) {
        SetError(error, "Scene Camera projection is invalid.");
        return false;
    }
    if (!IsCameraSettingsValid(component)) {
        SetError(error, "Scene Camera settings are invalid.");
        return false;
    }
    entity.camera = component;
    return true;
}

bool DecodeLightFields(const Json& light, LightComponent& component) {
    if (!light.is_object() || !light.contains("enabled") ||
        !light["enabled"].is_boolean() || !light.contains("type") ||
        !light["type"].is_string() || !light.contains("color") ||
        !DecodeFloat3(light["color"], component.color) ||
        !light.contains("intensity") || !light["intensity"].is_number() ||
        !light.contains("range") || !light["range"].is_number()) {
        return false;
    }
    component.enabled = light["enabled"].get<bool>();
    component.intensity = light["intensity"].get<float>();
    component.range = light["range"].get<float>();
    return true;
}

bool DecodeLightAngles(const Json& light, LightComponent& component) {
    if (!light.contains("innerAngle") || !light["innerAngle"].is_number() ||
        !light.contains("outerAngle") || !light["outerAngle"].is_number()) {
        return false;
    }
    component.innerAngleDegrees = light["innerAngle"].get<float>();
    component.outerAngleDegrees = light["outerAngle"].get<float>();
    return true;
}

bool DecodeLightType(const Json& light, LightType& type) {
    const std::string value = light["type"].get<std::string>();
    if (value == "Directional") {
        type = LightType::Directional;
    } else if (value == "Point") {
        type = LightType::Point;
    } else if (value == "Spot") {
        type = LightType::Spot;
    } else {
        return false;
    }
    return true;
}

bool IsLightSettingsValid(const LightComponent& component) {
    return component.color.x >= 0.0f && component.color.y >= 0.0f &&
           component.color.z >= 0.0f && std::isfinite(component.intensity) &&
           component.intensity >= 0.0f && std::isfinite(component.range) &&
           component.range >= 0.001f && std::isfinite(component.innerAngleDegrees) &&
           component.innerAngleDegrees >= 0.0f &&
           std::isfinite(component.outerAngleDegrees) &&
           component.outerAngleDegrees > component.innerAngleDegrees &&
           component.outerAngleDegrees <= 179.0f;
}

bool DecodeLightComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("Light")) {
        return true;
    }
    const Json& light = encoded["components"]["Light"];
    LightComponent component{};
    if (!DecodeLightFields(light, component) || !DecodeLightAngles(light, component)) {
        SetError(error, "Scene Light component is invalid.");
        return false;
    }
    if (!DecodeLightType(light, component.type)) {
        SetError(error, "Scene Light type is invalid.");
        return false;
    }
    if (!IsLightSettingsValid(component)) {
        SetError(error, "Scene Light settings are invalid.");
        return false;
    }
    entity.light = component;
    return true;
}

} // namespace

bool DecodeRenderingComponents(const Json& encoded, WorldEntity& entity, std::string* error) {
    return DecodeMeshRendererComponent(encoded, entity, error) &&
           DecodeMaterialOverrideComponent(encoded, entity, error) &&
           DecodeCameraComponent(encoded, entity, error) &&
           DecodeLightComponent(encoded, entity, error);
}

} // namespace WorldSerializerDecoding
