#include "input/Input.h"
#include "runtime/Behavior.h"
#include "runtime/ScriptModuleApi.h"
#include "world/World.h"
#include "world/WorldCollision.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <new>

namespace {
class FirstPersonController final : public Behavior {
public:
    explicit FirstPersonController(Input* input);

    void OnConfigure(const ScriptPropertyValueView* properties, size_t count) override;
    void OnStart(World& world, EntityId entity) override;
    void OnUpdate(World& world, EntityId entity, float deltaTime) override;

private:
    Input* input_ = nullptr;
    float moveSpeed_ = 4.0f;
    float sprintSpeed_ = 8.0f;
    float mouseSensitivity_ = 0.1f;
    float gamepadLookSpeed_ = 120.0f;
    float gravity_ = -24.0f;
    bool invertY_ = false;
    float verticalVelocity_ = 0.0f;
};

FirstPersonController::FirstPersonController(Input* input) : input_(input) {}

void FirstPersonController::OnConfigure(const ScriptPropertyValueView* properties,
                                        size_t count) {
    if (const ScriptPropertyValueView* value = FindScriptProperty(
            properties, count, "Move Speed", ScriptPropertyType::Float)) {
        moveSpeed_ = value->floatValue;
    }
    if (const ScriptPropertyValueView* value = FindScriptProperty(
            properties, count, "Sprint Speed", ScriptPropertyType::Float)) {
        sprintSpeed_ = value->floatValue;
    }
    if (const ScriptPropertyValueView* value = FindScriptProperty(
            properties, count, "Mouse Sensitivity", ScriptPropertyType::Float)) {
        mouseSensitivity_ = value->floatValue;
    }
    if (const ScriptPropertyValueView* value = FindScriptProperty(
            properties, count, "Gamepad Look Speed", ScriptPropertyType::Float)) {
        gamepadLookSpeed_ = value->floatValue;
    }
    if (const ScriptPropertyValueView* value = FindScriptProperty(
            properties, count, "Gravity", ScriptPropertyType::Float)) {
        gravity_ = value->floatValue;
    }
    if (const ScriptPropertyValueView* value = FindScriptProperty(
            properties, count, "Invert Y", ScriptPropertyType::Boolean)) {
        invertY_ = value->booleanValue;
    }
}

void FirstPersonController::OnStart(World& world, EntityId entity) {
    (void)world;
    (void)entity;
    verticalVelocity_ = 0.0f;
}

void FirstPersonController::OnUpdate(World& world, EntityId entity, float deltaTime) {
    WorldEntity* target = world.Find(entity);
    if (target == nullptr || input_ == nullptr || !std::isfinite(deltaTime) ||
        deltaTime <= 0.0f) {
        return;
    }

    TransformComponent& transform = target->transform;
    const float verticalLookSign = invertY_ ? -1.0f : 1.0f;
    transform.rotationDegrees.x = std::clamp(
        transform.rotationDegrees.x +
            verticalLookSign *
                (static_cast<float>(input_->GetMouseDY()) * mouseSensitivity_ -
                 input_->GetGamepadRightStickY() * gamepadLookSpeed_ * deltaTime),
        -89.0f, 89.0f);
    transform.rotationDegrees.y = std::remainder(
        transform.rotationDegrees.y +
            static_cast<float>(input_->GetMouseDX()) * mouseSensitivity_ +
            input_->GetGamepadRightStickX() * gamepadLookSpeed_ * deltaTime,
        360.0f);

    float forwardInput = input_->GetGamepadLeftStickY();
    float rightInput = input_->GetGamepadLeftStickX();
    if (input_->IsKeyPress(DIK_W)) {
        forwardInput += 1.0f;
    }
    if (input_->IsKeyPress(DIK_S)) {
        forwardInput -= 1.0f;
    }
    if (input_->IsKeyPress(DIK_D)) {
        rightInput += 1.0f;
    }
    if (input_->IsKeyPress(DIK_A)) {
        rightInput -= 1.0f;
    }
    const float inputLength =
        std::sqrt(forwardInput * forwardInput + rightInput * rightInput);
    forwardInput /= (std::max)(1.0f, inputLength);
    rightInput /= (std::max)(1.0f, inputLength);

    const float yaw = DirectX::XMConvertToRadians(transform.rotationDegrees.y);
    const float sinYaw = std::sin(yaw);
    const float cosYaw = std::cos(yaw);
    const bool sprinting =
        input_->IsKeyPress(DIK_LSHIFT) || input_->IsKeyPress(DIK_RSHIFT);
    const float speed = sprinting ? sprintSpeed_ : moveSpeed_;
    const float distance = speed * deltaTime;
    constexpr float terminalVelocity = -50.0f;
    verticalVelocity_ =
        (std::max)(verticalVelocity_ + gravity_ * deltaTime, terminalVelocity);
    const DirectX::XMFLOAT3 displacement{
        (sinYaw * forwardInput + cosYaw * rightInput) * distance,
        verticalVelocity_ * deltaTime,
        (cosYaw * forwardInput - sinYaw * rightInput) * distance,
    };
    const CharacterMoveResult movement =
        MoveCharacterController(world, entity, displacement);
    if ((static_cast<uint8_t>(movement.flags) &
         static_cast<uint8_t>(CharacterCollisionFlags::Below)) != 0u &&
        verticalVelocity_ < 0.0f) {
        verticalVelocity_ = 0.0f;
    }
}

Behavior* CreateFirstPersonController(Input* input) {
    return new (std::nothrow) FirstPersonController(input);
}
}

ScriptTypeRegistration GetFirstPersonControllerScriptRegistration() {
    static constexpr std::array properties = {
        ScriptPropertyDescriptor{.name = "Move Speed",
                                 .type = ScriptPropertyType::Float,
                                 .defaultFloat = 4.0f,
                                 .minimumFloat = 0.0f,
                                 .maximumFloat = 20.0f},
        ScriptPropertyDescriptor{.name = "Sprint Speed",
                                 .type = ScriptPropertyType::Float,
                                 .defaultFloat = 8.0f,
                                 .minimumFloat = 0.0f,
                                 .maximumFloat = 40.0f},
        ScriptPropertyDescriptor{.name = "Mouse Sensitivity",
                                 .type = ScriptPropertyType::Float,
                                 .defaultFloat = 0.1f,
                                 .minimumFloat = 0.001f,
                                 .maximumFloat = 2.0f},
        ScriptPropertyDescriptor{.name = "Gamepad Look Speed",
                                 .type = ScriptPropertyType::Float,
                                 .defaultFloat = 120.0f,
                                 .minimumFloat = 0.0f,
                                 .maximumFloat = 720.0f},
        ScriptPropertyDescriptor{.name = "Gravity",
                                 .type = ScriptPropertyType::Float,
                                 .defaultFloat = -24.0f,
                                 .minimumFloat = -100.0f,
                                 .maximumFloat = 0.0f},
        ScriptPropertyDescriptor{.name = "Invert Y",
                                 .type = ScriptPropertyType::Boolean,
                                 .defaultBoolean = false},
    };
    return {"FirstPersonController", "asset://Scripts/FirstPersonController.cpp",
            &CreateFirstPersonController, {.characterController = true},
            properties.data(), properties.size()};
}
