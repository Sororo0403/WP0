#include "runtime/Behavior.h"
#include "runtime/ScriptModuleApi.h"
#include "world/World.h"
#include "world/WorldCollision.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <string>

namespace {
constexpr char kTargetName[] = "Player";

class ChasePlayer final : public Behavior {
public:
    void OnConfigure(const ScriptPropertyValueView* properties, size_t count) override;
    void OnStart(World& world, EntityId entity) override;
    void OnUpdate(World& world, EntityId entity, float deltaTime) override;
    void OnTriggerEnter(World& world, EntityId entity, EntityId other) override;

private:
    void FindTarget(const World& world, EntityId self);
    void UpdateAnimation(World& world, EntityId self, bool moving);

    EntityId target_{};
    EntityId configuredTarget_{};
    float moveSpeed_ = 2.5f;
    float catchDistance_ = 0.8f;
    float gravity_ = -24.0f;
    std::string idleAnimation_ = "Idle";
    std::string moveAnimation_ = "Run";
    float animationFadeDuration_ = 0.2f;
    float verticalVelocity_ = 0.0f;
    bool caught_ = false;
    bool moving_ = false;
};

void ChasePlayer::OnConfigure(const ScriptPropertyValueView* properties, size_t count) {
    if (const ScriptPropertyValueView* target = FindScriptProperty(
            properties, count, "Target", ScriptPropertyType::Entity)) {
        configuredTarget_ = target->entityValue;
    }
    if (const ScriptPropertyValueView* speed = FindScriptProperty(
            properties, count, "Move Speed", ScriptPropertyType::Float)) {
        moveSpeed_ = speed->floatValue;
    }
    if (const ScriptPropertyValueView* distance = FindScriptProperty(
            properties, count, "Catch Distance", ScriptPropertyType::Float)) {
        catchDistance_ = distance->floatValue;
    }
    if (const ScriptPropertyValueView* gravity = FindScriptProperty(
            properties, count, "Gravity", ScriptPropertyType::Float)) {
        gravity_ = gravity->floatValue;
    }
    if (const ScriptPropertyValueView* animation = FindScriptProperty(
            properties, count, "Idle Animation", ScriptPropertyType::AnimationClip)) {
        idleAnimation_ = animation->stringValue != nullptr ? animation->stringValue : "";
    }
    if (const ScriptPropertyValueView* animation = FindScriptProperty(
            properties, count, "Move Animation", ScriptPropertyType::AnimationClip)) {
        moveAnimation_ = animation->stringValue != nullptr ? animation->stringValue : "";
    }
    if (const ScriptPropertyValueView* duration = FindScriptProperty(
            properties, count, "Animation Fade", ScriptPropertyType::Float)) {
        animationFadeDuration_ = duration->floatValue;
    }
}

void ChasePlayer::FindTarget(const World& world, EntityId self) {
    target_ = {};
    for (const WorldEntity& candidate : world.Entities()) {
        if (candidate.id != self && candidate.name == kTargetName) {
            target_ = candidate.id;
            return;
        }
    }
}

void ChasePlayer::OnStart(World& world, EntityId entity) {
    verticalVelocity_ = 0.0f;
    caught_ = false;
    moving_ = false;
    target_ = configuredTarget_;
    if (!target_.IsValid()) {
        FindTarget(world, entity);
    }
    if (!idleAnimation_.empty()) {
        world.PlayAnimation(entity, idleAnimation_, true);
    }
}

void ChasePlayer::UpdateAnimation(World& world, EntityId self, bool moving) {
    if (moving == moving_) {
        return;
    }
    moving_ = moving;
    const std::string& animation = moving ? moveAnimation_ : idleAnimation_;
    if (!animation.empty()) {
        world.CrossFadeAnimation(self, animation, animationFadeDuration_, true);
    }
}

void ChasePlayer::OnUpdate(World& world, EntityId entity, float deltaTime) {
    WorldEntity* chaser = world.Find(entity);
    if (chaser == nullptr || !std::isfinite(deltaTime) || deltaTime <= 0.0f) {
        return;
    }
    const WorldEntity* target = world.Find(target_);
    if (target == nullptr && !configuredTarget_.IsValid()) {
        FindTarget(world, entity);
        target = world.Find(target_);
    }

    DirectX::XMFLOAT3 horizontalMotion{};
    bool wantsToMove = false;
    if (target != nullptr && !caught_) {
        DirectX::XMFLOAT4X4 chaserWorld{};
        DirectX::XMFLOAT4X4 targetWorld{};
        const bool hasWorldPositions = world.TryGetWorldMatrix(entity, chaserWorld) &&
                                       world.TryGetWorldMatrix(target_, targetWorld);
        const float deltaX = hasWorldPositions
                                 ? targetWorld._41 - chaserWorld._41
                                 : target->transform.position.x - chaser->transform.position.x;
        const float deltaZ = hasWorldPositions
                                 ? targetWorld._43 - chaserWorld._43
                                 : target->transform.position.z - chaser->transform.position.z;
        const float distanceSquared = deltaX * deltaX + deltaZ * deltaZ;
        if (distanceSquared <= catchDistance_ * catchDistance_) {
            caught_ = true;
        } else {
            const float inverseDistance = 1.0f / std::sqrt(distanceSquared);
            horizontalMotion.x = deltaX * inverseDistance * moveSpeed_ * deltaTime;
            horizontalMotion.z = deltaZ * inverseDistance * moveSpeed_ * deltaTime;
            wantsToMove = true;
            chaser->transform.rotationDegrees.y = DirectX::XMConvertToDegrees(
                std::atan2(deltaX, deltaZ));
        }
    }
    UpdateAnimation(world, entity, wantsToMove);

    constexpr float terminalVelocity = -50.0f;
    verticalVelocity_ =
        (std::max)(verticalVelocity_ + gravity_ * deltaTime, terminalVelocity);
    horizontalMotion.y = verticalVelocity_ * deltaTime;
    const CharacterMoveResult movement =
        MoveCharacterController(world, entity, horizontalMotion);
    if ((static_cast<uint8_t>(movement.flags) &
         static_cast<uint8_t>(CharacterCollisionFlags::Below)) != 0u &&
        verticalVelocity_ < 0.0f) {
        verticalVelocity_ = 0.0f;
    }
}

void ChasePlayer::OnTriggerEnter(World& world, EntityId entity, EntityId other) {
    (void)world;
    (void)entity;
    if (other == target_) {
        caught_ = true;
    }
}

Behavior* CreateChasePlayer(Input* input) {
    (void)input;
    return new (std::nothrow) ChasePlayer();
}
} // namespace

ScriptTypeRegistration GetChasePlayerScriptRegistration() {
    static constexpr std::array properties = {
        ScriptPropertyDescriptor{"Target", ScriptPropertyType::Entity},
        ScriptPropertyDescriptor{"Move Speed", ScriptPropertyType::Float, 2.5f, 0.0f,
                                 20.0f},
        ScriptPropertyDescriptor{"Catch Distance", ScriptPropertyType::Float, 0.8f,
                                 0.1f, 10.0f},
        ScriptPropertyDescriptor{"Gravity", ScriptPropertyType::Float, -24.0f, -100.0f,
                                 0.0f},
        ScriptPropertyDescriptor{.name = "Idle Animation",
                                 .type = ScriptPropertyType::AnimationClip,
                                 .defaultString = "Idle"},
        ScriptPropertyDescriptor{.name = "Move Animation",
                                 .type = ScriptPropertyType::AnimationClip,
                                 .defaultString = "Run"},
        ScriptPropertyDescriptor{.name = "Animation Fade",
                                 .type = ScriptPropertyType::Float,
                                 .defaultFloat = 0.2f,
                                 .minimumFloat = 0.0f,
                                 .maximumFloat = 2.0f},
    };
    return {"ChasePlayer", "asset://Scripts/ChasePlayer.cpp", &CreateChasePlayer,
            {.characterController = true}, properties.data(), properties.size()};
}
