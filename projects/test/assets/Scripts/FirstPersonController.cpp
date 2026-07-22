#include "input/Input.h"
#include "runtime/Behavior.h"
#include "runtime/ScriptModuleApi.h"
#include "world/World.h"
#include "world/WorldCollision.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <new>

namespace {
class FirstPersonController final : public Behavior {
public:
    explicit FirstPersonController(Input* input);

    void OnStart(World& world, EntityId entity) override;
    void OnUpdate(World& world, EntityId entity, float deltaTime) override;

private:
    Input* input_ = nullptr;
    float verticalVelocity_ = 0.0f;
};

FirstPersonController::FirstPersonController(Input* input) : input_(input) {}

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
    constexpr float mouseSensitivity = 0.1f;
    constexpr float gamepadLookSpeed = 120.0f;
    transform.rotationDegrees.x = std::clamp(
        transform.rotationDegrees.x +
            static_cast<float>(input_->GetMouseDY()) * mouseSensitivity -
            input_->GetGamepadRightStickY() * gamepadLookSpeed * deltaTime,
        -89.0f, 89.0f);
    transform.rotationDegrees.y = std::remainder(
        transform.rotationDegrees.y +
            static_cast<float>(input_->GetMouseDX()) * mouseSensitivity +
            input_->GetGamepadRightStickX() * gamepadLookSpeed * deltaTime,
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
    const float speed = sprinting ? 8.0f : 4.0f;
    const float distance = speed * deltaTime;
    constexpr float gravity = -24.0f;
    constexpr float terminalVelocity = -50.0f;
    verticalVelocity_ =
        (std::max)(verticalVelocity_ + gravity * deltaTime, terminalVelocity);
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
    return {"FirstPersonController", "asset://Scripts/FirstPersonController.cpp",
            &CreateFirstPersonController, {.characterController = true}};
}
