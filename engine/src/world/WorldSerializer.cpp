#include "world/WorldSerializer.h"

#include "world/World.h"

#include "nlohmann/json.hpp"

#include <cmath>
#include <exception>
#include <fstream>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;

Json EncodeFloat3(const DirectX::XMFLOAT3& value) {
    return Json::array({value.x, value.y, value.z});
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
