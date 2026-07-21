#include "runtime/FirstPersonController.h"

#include "input/Input.h"
#include "world/World.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

FirstPersonController::FirstPersonController(Input* input) : input_(input) {}

void FirstPersonController::OnUpdate(World& world, EntityId entity, float deltaTime) {
    WorldEntity* target = world.Find(entity);
    if (target == nullptr || input_ == nullptr) {
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
    if (inputLength <= 0.0001f) {
        return;
    }
    forwardInput /= (std::max)(1.0f, inputLength);
    rightInput /= (std::max)(1.0f, inputLength);

    const float yaw = DirectX::XMConvertToRadians(transform.rotationDegrees.y);
    const float sinYaw = std::sin(yaw);
    const float cosYaw = std::cos(yaw);
    const bool sprinting =
        input_->IsKeyPress(DIK_LSHIFT) || input_->IsKeyPress(DIK_RSHIFT);
    const float speed = sprinting ? 8.0f : 4.0f;
    const float distance = speed * deltaTime;
    transform.position.x +=
        (sinYaw * forwardInput + cosYaw * rightInput) * distance;
    transform.position.z +=
        (cosYaw * forwardInput - sinYaw * rightInput) * distance;
}
