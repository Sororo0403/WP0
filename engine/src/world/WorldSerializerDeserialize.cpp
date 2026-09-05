#include "world/WorldSerializer.h"
#include "internal/WorldSerializerComponentDecoders.h"

#include <vector>

using namespace WorldSerializerDecoding;
using namespace WorldSerializerJson;

namespace {
bool HasSupportedSchema(const Json& root) {
    return root.contains("version") && root["version"] == Json(1) &&
           root.contains("entities") && root["entities"].is_array();
}

bool DecodeEntityIdentity(const Json& encoded, WorldEntity& entity,
                          std::string* error) {
    if (!encoded.is_object() || !encoded.contains("id") ||
        !encoded["id"].is_string() || !encoded.contains("name") ||
        !encoded["name"].is_string()) {
        SetError(error, "Scene entity is missing required fields.");
        return false;
    }
    if (!EntityId::TryParse(encoded["id"].get_ref<const std::string&>(), entity.id)) {
        SetError(error, "Scene entity id is invalid.");
        return false;
    }
    entity.name = encoded["name"].get<std::string>();
    return true;
}

bool DecodeEntityState(const Json& encoded, WorldEntity& entity,
                       std::string* error) {
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
    return true;
}

bool DecodeEntityParent(const Json& encoded, WorldEntity& entity,
                        std::string* error) {
    if (!encoded.contains("parent") || encoded["parent"].is_null()) {
        return true;
    }
    if (!encoded["parent"].is_string() ||
        !EntityId::TryParse(encoded["parent"].get_ref<const std::string&>(),
                            entity.parent)) {
        SetError(error, "Scene parent id is invalid.");
        return false;
    }
    return true;
}

bool DecodeTransform(const Json& encoded, WorldEntity& entity,
                     std::string* error) {
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
    return true;
}

bool DecodeComponents(const Json& encoded, WorldEntity& entity,
                      std::string* error) {
    return DecodeRenderingComponents(encoded, entity, error) &&
           DecodeRuntimeComponents(encoded, entity, error) &&
           DecodeUiComponents(encoded, entity, error) &&
           DecodeScriptsComponent(encoded["components"], entity, error) &&
           DecodePhysicsComponents(encoded, entity, error);
}

bool DecodeEntity(const Json& encoded, WorldEntity& entity, std::string* error) {
    return DecodeEntityIdentity(encoded, entity, error) &&
           DecodeEntityState(encoded, entity, error) &&
           DecodeEntityParent(encoded, entity, error) &&
           DecodeTransform(encoded, entity, error) &&
           DecodeComponents(encoded, entity, error);
}
} // namespace

bool WorldSerializer::Deserialize(std::string_view text, World& world, std::string* error) {
    Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        SetError(error, "Scene JSON is invalid.");
        return false;
    }
    if (!HasSupportedSchema(root)) {
        SetError(error, "Scene schema or version is unsupported.");
        return false;
    }

    std::vector<WorldEntity> entities;
    entities.reserve(root["entities"].size());
    for (const Json& encoded : root["entities"]) {
        WorldEntity entity{};
        if (!DecodeEntity(encoded, entity, error)) {
            return false;
        }
        entities.push_back(std::move(entity));
    }
    return world.ReplaceEntities(std::move(entities), error);
}
