#include "internal/WorldSerializerComponentDecoders.h"

using namespace WorldSerializerJson;

namespace WorldSerializerDecoding {
namespace {
bool DecodeBoxColliderFields(const Json& encoded, BoxColliderComponent& component) {
    if (!encoded.is_object() || !encoded.contains("enabled") ||
        !encoded["enabled"].is_boolean() || !encoded.contains("center") ||
        !encoded.contains("size") || !encoded.contains("isTrigger") ||
        !encoded["isTrigger"].is_boolean() ||
        !DecodeFloat3(encoded["center"], component.center) ||
        !DecodeFloat3(encoded["size"], component.size)) {
        return false;
    }
    component.enabled = encoded["enabled"].get<bool>();
    component.isTrigger = encoded["isTrigger"].get<bool>();
    return true;
}

bool IsValidBoxColliderSize(const DirectX::XMFLOAT3& size) {
    return size.x >= 0.001f && size.x <= 1000000.0f &&
           size.y >= 0.001f && size.y <= 1000000.0f &&
           size.z >= 0.001f && size.z <= 1000000.0f;
}

bool DecodeBoxColliderComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("BoxCollider")) {
        return true;
    }
    BoxColliderComponent component{};
    if (!DecodeBoxColliderFields(encoded["components"]["BoxCollider"], component)) {
        SetError(error, "Scene BoxCollider component is invalid.");
        return false;
    }
    if (!IsValidBoxColliderSize(component.size)) {
        SetError(error, "Scene BoxCollider size is invalid.");
        return false;
    }
    entity.boxCollider = component;
    return true;
}

bool DecodeControllerDimensions(const Json& encoded,
                                CharacterControllerComponent& component) {
    if (!encoded.is_object() || !encoded.contains("enabled") ||
        !encoded["enabled"].is_boolean() || !encoded.contains("center") ||
        !DecodeFloat3(encoded["center"], component.center) ||
        !encoded.contains("radius") || !encoded["radius"].is_number() ||
        !encoded.contains("height") || !encoded["height"].is_number()) {
        return false;
    }
    component.enabled = encoded["enabled"].get<bool>();
    component.radius = encoded["radius"].get<float>();
    component.height = encoded["height"].get<float>();
    return true;
}

bool DecodeControllerMovement(const Json& encoded,
                              CharacterControllerComponent& component) {
    if (!encoded.contains("slopeLimit") || !encoded["slopeLimit"].is_number() ||
        !encoded.contains("stepOffset") || !encoded["stepOffset"].is_number() ||
        !encoded.contains("skinWidth") || !encoded["skinWidth"].is_number() ||
        !encoded.contains("minMoveDistance") ||
        !encoded["minMoveDistance"].is_number()) {
        return false;
    }
    component.slopeLimitDegrees = encoded["slopeLimit"].get<float>();
    component.stepOffset = encoded["stepOffset"].get<float>();
    component.skinWidth = encoded["skinWidth"].get<float>();
    component.minMoveDistance = encoded["minMoveDistance"].get<float>();
    return true;
}

bool IsValidControllerDimensions(const CharacterControllerComponent& component) {
    return std::isfinite(component.radius) && std::isfinite(component.height) &&
           component.radius >= 0.001f && component.height >= component.radius * 2.0f;
}

bool IsValidControllerMovement(const CharacterControllerComponent& component) {
    return std::isfinite(component.slopeLimitDegrees) &&
           std::isfinite(component.stepOffset) && std::isfinite(component.skinWidth) &&
           std::isfinite(component.minMoveDistance) &&
           component.slopeLimitDegrees >= 0.0f &&
           component.slopeLimitDegrees <= 90.0f && component.stepOffset >= 0.0f &&
           component.stepOffset <= component.height && component.skinWidth >= 0.0f &&
           component.skinWidth < component.radius && component.minMoveDistance >= 0.0f;
}

bool DecodeCharacterControllerComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("CharacterController")) {
        return true;
    }
    const Json& controller = encoded["components"]["CharacterController"];
    CharacterControllerComponent component{};
    if (!DecodeControllerDimensions(controller, component) ||
        !DecodeControllerMovement(controller, component)) {
        SetError(error, "Scene CharacterController component is invalid.");
        return false;
    }
    if (!IsValidControllerDimensions(component) ||
        !IsValidControllerMovement(component)) {
        SetError(error, "Scene CharacterController settings are invalid.");
        return false;
    }
    entity.characterController = component;
    return true;
}

} // namespace

bool DecodePhysicsComponents(const Json& encoded, WorldEntity& entity, std::string* error) {
    return DecodeBoxColliderComponent(encoded, entity, error) &&
           DecodeCharacterControllerComponent(encoded, entity, error);
}

} // namespace WorldSerializerDecoding
