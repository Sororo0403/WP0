#include "internal/WorldSerializerComponentDecoders.h"

using namespace WorldSerializerJson;

namespace WorldSerializerDecoding {
namespace {
bool DecodeBoxColliderComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
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
    return true;
}

bool DecodeCharacterControllerComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
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
    return true;
}

} // namespace

bool DecodePhysicsComponents(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!DecodeBoxColliderComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeCharacterControllerComponent(encoded, entity, error)) {
        return false;
    }
    return true;
}

} // namespace WorldSerializerDecoding
