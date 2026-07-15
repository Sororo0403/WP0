#include "scene/GameScene.h"

#include "input/Input.h"

#include <algorithm>
#include <cmath>
#include <dinput.h>

#ifdef _DEBUG
#include <imgui.h>
#endif

#ifdef DrawText
#undef DrawText
#endif

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kFixedCombatDt = 1.0f / 60.0f;
constexpr int kInputBufferFrames = 8;
constexpr float kPlayerMoveSpeed = 2.4f;
constexpr float kEnemyMoveSpeed = 1.65f;
constexpr int kMaxFixedStepsPerFrame = 5;
constexpr int kEnemyTrainingAttackCooldown = 90;
constexpr int kEnemySupportAttackCooldown = 135;
constexpr float kEnemyPreferredRange = 1.98f;
constexpr float kEnemyRetreatRange = 1.45f;
constexpr float kEnemyEngageRange = 2.38f;
constexpr float kEnemyStrafeSpeedScale = 0.62f;
constexpr float kEnemyRetreatSpeedScale = 0.74f;
constexpr float kEnemyAttackRunSpeedScale = 2.25f;
constexpr float kEnemyMoveBlend = 0.12f;
constexpr int kEnemyIntentMinFrames = 24;
constexpr int kEnemyIntentMaxFrames = 54;
constexpr int kDoubleSwayStartFrame = 6;
constexpr int kMaxDodgeChainCount = 2;
constexpr int kDodgeRecoveryFrames = 5;
constexpr float kOrbitSwayMinRadius = 0.45f;
constexpr float kOrbitSwayMaxRadius = 0.65f;
constexpr float kOrbitSwayArcRadians = kPi * 0.58f;
constexpr float kOrbitSwayTargetRange = 0.95f;
constexpr float kOrbitSwayInputThreshold = 0.6f;
constexpr float kOrbitSwayApproachThreshold = 0.55f;
constexpr int kComboTimerFrames = 90;
constexpr int kKnockdownFrames = 90;
constexpr float kHitKnockbackDistance = 0.18f;
constexpr float kFinisherKnockbackDistance = 0.34f;
constexpr float kBlockKnockbackDistance = 0.08f;
constexpr float kHitPullTargetDistance = 0.72f;
constexpr float kHitPullSpeed = 0.045f;
constexpr float kPlayerAttackHomingRange = 1.75f;
constexpr float kPlayerAttackHomingStrength = 0.42f;
constexpr float kDownAttackDistance = 1.5f;
constexpr float kMaxExGauge = 100.0f;
constexpr float kExBoostCost = 50.0f;
constexpr float kExActionCost = 25.0f;
constexpr int kExBoostFrames = 300;
constexpr float kExBoostGaugeDrainPerFrame = 0.15f;
constexpr float kExActionDistance = 1.45f;
constexpr bool kEnemyInvincibleForDebug = true;
constexpr float kSingleStyleFinisherDamageScale = 1.15f;
constexpr int kSingleStyleFinisherBlockstunBonus = 6;
constexpr float kCrowdStyleDamageScale = 0.65f;
constexpr float kCrowdStyleRangeScale = 1.18f;
constexpr float kCrowdStyleHalfWidthScale = 2.1f;
constexpr float kCameraYawSpeed = 2.35f;
constexpr float kCameraStickDeadZone = 0.18f;
constexpr size_t kEnemyCount = 1;
} // namespace

void GameScene::Initialize(const SceneContext& ctx) {
    BaseScene::Initialize(ctx);
    InitializeVisuals();
    ResetPhaseOne();
}

void GameScene::Update() {
    const float dt = ctx_ ? (std::max)(0.0f, ctx_->frame.deltaTime) : 0.0f;
    elapsedSeconds_ += dt;

    CaptureFrameInput();

    combatAccumulator_ += (std::min)(dt, 0.1f);
    debug_.fixedStepsThisFrame = 0;
    while (combatAccumulator_ >= kFixedCombatDt &&
           debug_.fixedStepsThisFrame < kMaxFixedStepsPerFrame) {
        StepCombat();
        combatAccumulator_ -= kFixedCombatDt;
        ++debug_.fixedStepsThisFrame;
    }

    if (debug_.fixedStepsThisFrame == kMaxFixedStepsPerFrame) {
        combatAccumulator_ = 0.0f;
    }
}

void GameScene::Draw() {
    if (!ctx_ || !ctx_->rendering.model) {
        return;
    }

    ModelManager& models = *ctx_->rendering.model;
    if (!models.IsReady()) {
        return;
    }

    models.PreDraw();
    DrawActors(models);
    DrawAttackRanges(models);
    DrawCombatMarkers(models);
    models.PostDraw();
}

void GameScene::DrawActors(ModelManager& models) const {
    models.Draw(floorModel_, MakeFloorTransform(), combatCamera_);
    models.Draw(playerModel_, MakeActorTransform(player_, 1.7f), combatCamera_);
    models.Draw(enemyModel_, MakeActorTransform(enemy_, 1.55f, 0.9f), combatCamera_);
    for (const CombatActor& enemy : supportEnemies_) {
        if (!enemy.IsAlive()) {
            continue;
        }
        models.Draw(enemyModel_, MakeActorTransform(enemy, 1.55f, 0.9f), combatCamera_);
    }
}

void GameScene::DrawAttackRanges(ModelManager& models) const {
    if (IsAttackHitboxActive(player_)) {
        const AttackData playerAttack =
            MakeEffectiveAttackData(player_, GetAttackData(player_.currentMove));
        models.Draw(attackRangeModel_, MakeAttackRangeTransform(player_, playerAttack),
                    combatCamera_);
    }

    if (IsAttackHitboxActive(enemy_)) {
        models.Draw(attackRangeModel_,
                    MakeAttackRangeTransform(enemy_, GetAttackData(enemy_.currentMove)),
                    combatCamera_);
    }
    for (const CombatActor& enemy : supportEnemies_) {
        if (!enemy.IsAlive()) {
            continue;
        }
        if (IsAttackHitboxActive(enemy)) {
            models.Draw(attackRangeModel_,
                        MakeAttackRangeTransform(enemy, GetAttackData(enemy.currentMove)),
                        combatCamera_);
        }
    }
}

void GameScene::DrawCombatMarkers(ModelManager& models) const {
    if (player_.state == CombatState::Guard || player_.state == CombatState::GuardStun) {
        Transform guard = MakeActorTransform(player_, 0.25f, 1.25f);
        guard.position.y = 1.85f;
        models.Draw(guardMarkerModel_, guard, combatCamera_);
    }

    if (IsExBoostActive()) {
        Transform boost = MakeActorTransform(player_, 0.18f, 1.55f);
        boost.position.y = 2.15f;
        models.Draw(attackRangeModel_, boost, combatCamera_);
    }

    if (combatStyle_ == CombatStyle::Crowd) {
        Transform style = MakeActorTransform(player_, 0.12f, 1.85f);
        style.position.y = 2.38f;
        models.Draw(guardMarkerModel_, style, combatCamera_);
    }

    if (lockOnActive_) {
        Transform lock = MakeActorTransform(TargetEnemy(), 0.16f, 1.35f);
        lock.position.y = 1.95f;
        models.Draw(guardMarkerModel_, lock, combatCamera_);
    }
}

void GameScene::DrawPostProcessOverlay() {
#ifdef _DEBUG
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Combat Phase 11")) {
        ImGui::Text(
            "Controls: WASD move / J/X light / K/Y heavy / C or RB lock toggle / Tab or DPadUp style / E or R2 EX boost / EX+Heavy action / Heavy near downed / L or LB guard / Guard+Heavy counter / Space or B sway / R reset");
        ImGui::Separator();
        ImGui::Text("Combat frame: %llu",
                    static_cast<unsigned long long>(debug_.combatFrame));
        ImGui::Text("Fixed steps this render frame: %d", debug_.fixedStepsThisFrame);
        ImGui::Text("Hitstop frames: %d", hitstopFrames_);
        ImGui::Text("Combo: %d  timer=%d", comboCount_, comboTimerFrames_);
        ImGui::Text("Style: %s", StyleName(combatStyle_));
        ImGui::Text("Lock target: %s Enemy%zu", lockOnActive_ ? "on" : "off",
                    targetEnemyIndex_ + 1);
        ImGui::Text("EX Boost: %s  frames=%d  ready=%s",
                    IsExBoostActive() ? "active" : "off", exBoostFrames_,
                    exGauge_ >= kExBoostCost ? "yes" : "no");
        ImGui::Text("EX Action: %s  drain/frame=%.2f",
                    IsExBoostActive() && exGauge_ >= kExActionCost ? "ready" : "no",
                    kExBoostGaugeDrainPerFrame);
        ImGui::Text("Camera shake frames: %d", cameraShakeFrames_);
        ImGui::Separator();
        DrawDebugCombatants();
        ImGui::Text("Distance: %.2f", debug_.lastDistance);
        ImGui::Text("Hitbox active: %s", debug_.lastHitboxActive ? "yes" : "no");
        ImGui::Text("Last hit connected: %s", debug_.lastHitConnected ? "yes" : "no");
        ImGui::Text("Last defense: %s", debug_.lastDefense);
        ImGui::Text("Blocked: %s  Dodged: %s", debug_.lastBlocked ? "yes" : "no",
                    debug_.lastDodged ? "yes" : "no");
        ImGui::Text("Last knockback: %.2f", debug_.lastKnockback);
        ImGui::Text("Guard front: %s  Sway invul: %s",
                    IsFacingIncomingAttack(player_, TargetEnemy()) ? "yes" : "no",
                    IsDodgeInvulnerable(player_) ? "yes" : "no");
        ImGui::Text("Enemy training cooldown: %d", enemyTrainingCooldown_);
        ImGui::Text("Buffered inputs:");
        for (const BufferedInput& entry : inputBuffer_.entries) {
            if (entry.active) {
                ImGui::BulletText("%s %df", CommandName(entry.command), entry.framesLeft);
            }
        }

        const float playerHp = static_cast<float>((std::max)(player_.hp, 0)) / 100.0f;
        const float enemyHp = static_cast<float>((std::max)(enemy_.hp, 0)) / 100.0f;
        const float exRatio = std::clamp(exGauge_ / kMaxExGauge, 0.0f, 1.0f);
        ImGui::ProgressBar(playerHp, ImVec2(-1.0f, 0.0f), "Player HP");
        ImGui::ProgressBar(enemyHp, ImVec2(-1.0f, 0.0f), "Enemy HP");
        ImGui::ProgressBar(exRatio, ImVec2(-1.0f, 0.0f), "EX");

        ImGui::Separator();
        ImGui::Text("Player position: %.2f, %.2f", player_.position.x,
                    player_.position.z);
        ImGui::Text("Enemy position : %.2f, %.2f", enemy_.position.x, enemy_.position.z);
    }
    ImGui::End();
#endif
}

#ifdef _DEBUG
void GameScene::DrawDebugCombatants() const {
    ImGui::Text("Player: %s  move=%s  frame=%d  hp=%d", StateName(player_.state),
                GetAttackData(player_.currentMove).name, player_.frameInState, player_.hp);
    if (player_.state == CombatState::Attack) {
        const AttackData& attack = GetAttackData(player_.currentMove);
        ImGui::Text("Attack frames: startup=%d active=%d-%d cancel=%d-%d total=%d",
                    attack.startup, attack.startup, attack.startup + attack.active - 1,
                    attack.cancelFrom, attack.cancelTo, attack.total);
        ImGui::Text("Attack advance: %.2f", attack.advanceDistance);
    }
    ImGui::Text("Enemy : %s  move=%s  frame=%d  hp=%d", StateName(enemy_.state),
                GetAttackData(enemy_.currentMove).name, enemy_.frameInState, enemy_.hp);
    for (size_t i = 0; i < supportEnemies_.size(); ++i) {
        const CombatActor& enemy = supportEnemies_[i];
        if (!enemy.IsAlive()) {
            continue;
        }
        ImGui::Text("Enemy%zu: %s  move=%s  frame=%d  hp=%d  attacks=%d", i + 2,
                    StateName(enemy.state), GetAttackData(enemy.currentMove).name,
                    enemy.frameInState, enemy.hp, enemy.aiAttackCount);
    }
}

#else
void GameScene::DrawDebugCombatants() const {}
#endif

void GameScene::InputBuffer::Push(CombatCommand command, int bufferFrames) {
    const auto available = std::find_if(entries.begin(), entries.end(),
                                        [](const BufferedInput& entry) {
                                            return !entry.active;
                                        });
    if (available != entries.end()) {
        available->command = command;
        available->framesLeft = bufferFrames;
        available->active = true;
        return;
    }

    entries.front().command = command;
    entries.front().framesLeft = bufferFrames;
    entries.front().active = true;
}

void GameScene::InputBuffer::Tick() {
    for (BufferedInput& entry : entries) {
        if (!entry.active) {
            continue;
        }

        --entry.framesLeft;
        if (entry.framesLeft <= 0) {
            entry.active = false;
        }
    }
}

bool GameScene::InputBuffer::Consume(CombatCommand command) {
    const auto match = std::find_if(entries.begin(), entries.end(),
                                    [command](const BufferedInput& entry) {
                                        return entry.active && entry.command == command;
                                    });
    if (match != entries.end()) {
        match->active = false;
        return true;
    }
    return false;
}

void GameScene::InputBuffer::Clear() {
    for (BufferedInput& entry : entries) {
        entry.active = false;
        entry.framesLeft = 0;
    }
}

bool GameScene::CombatActor::IsAlive() const {
    return hp > 0;
}

void GameScene::ResetPhaseOne() {
    combatAccumulator_ = 0.0f;
    exGauge_ = 0.0f;
    combatCameraYaw_ = 0.0f;
    cameraShakeMagnitude_ = 0.0f;
    hitstopFrames_ = 0;
    enemyTrainingCooldown_ = 45;
    comboCount_ = 0;
    comboTimerFrames_ = 0;
    cameraShakeFrames_ = 0;
    exBoostFrames_ = 0;
    exBoostRequested_ = false;
    styleSwitchRequested_ = false;
    pendingLightInput_ = false;
    pendingHeavyInput_ = false;
    pendingExBoostInput_ = false;
    pendingStyleSwitchInput_ = false;
    pendingLockCycleInput_ = false;
    lockOnActive_ = false;
    combatStyle_ = CombatStyle::Single;
    nextAttackSerial_ = 0;
    targetEnemyIndex_ = 0;
    inputBuffer_.Clear();
    pendingInput_ = {};
    combatInput_ = {};
    debug_ = {};

    player_ = {};
    player_.name = "Player";
    player_.position = {0.0f, 0.0f};
    player_.facing = {0.0f, -1.0f};
    player_.hp = 100;

    enemy_ = {};
    enemy_.name = "Enemy";
    enemy_.position = {0.0f, 1.35f};
    enemy_.facing = {0.0f, -1.0f};
    enemy_.attackFacing = enemy_.facing;
    enemy_.hp = 100;
    enemy_.aiCooldownFrames = 45;
    enemy_.aiAttackCount = 0;
    enemy_.aiIntent = EnemyIntent::Approach;
    enemy_.aiIntentFrames = 20;
    enemy_.aiMoveDirection = {};
    enemy_.aiStrafePhase = 0.0f;
    enemy_.aiStrafeSign = 1.0f;

    supportEnemies_[0] = {};
    supportEnemies_[0].name = "Enemy2";
    supportEnemies_[0].position = {-1.45f, 0.45f};
    supportEnemies_[0].facing = {0.0f, -1.0f};
    supportEnemies_[0].attackFacing = supportEnemies_[0].facing;
    supportEnemies_[0].hp = 0;
    supportEnemies_[0].aiCooldownFrames = 80;
    supportEnemies_[0].aiAttackCount = 1;
    supportEnemies_[0].aiIntent = EnemyIntent::Strafe;
    supportEnemies_[0].aiIntentFrames = 30;
    supportEnemies_[0].aiMoveDirection = {};
    supportEnemies_[0].aiStrafePhase = 1.7f;
    supportEnemies_[0].aiStrafeSign = -1.0f;

    supportEnemies_[1] = {};
    supportEnemies_[1].name = "Enemy3";
    supportEnemies_[1].position = {1.65f, 1.85f};
    supportEnemies_[1].facing = {0.0f, -1.0f};
    supportEnemies_[1].attackFacing = supportEnemies_[1].facing;
    supportEnemies_[1].hp = 0;
    supportEnemies_[1].aiCooldownFrames = 120;
    supportEnemies_[1].aiAttackCount = 2;
    supportEnemies_[1].aiIntent = EnemyIntent::HoldRange;
    supportEnemies_[1].aiIntentFrames = 30;
    supportEnemies_[1].aiMoveDirection = {};
    supportEnemies_[1].aiStrafePhase = 3.1f;
    supportEnemies_[1].aiStrafeSign = 1.0f;

    UpdateCombatCamera();
}

void GameScene::InitializeVisuals() {
    combatCamera_.Initialize(16.0f / 9.0f);
    combatCamera_.SetPerspectiveFovDeg(50.0f);
    combatCamera_.SetClipRange(0.05f, 200.0f);
    UpdateCombatCamera();

    if (!ctx_ || !ctx_->rendering.model) {
        return;
    }

    ModelManager& models = *ctx_->rendering.model;
    if (!models.IsReady()) {
        return;
    }

    playerModel_ = models.CreateBoxHandle(kInvalidResourceId,
                                          MakeMaterial(0.18f, 0.45f, 1.0f), 0.50f,
                                          1.55f, 0.41f);
    enemyModel_ = models.CreateBoxHandle(kInvalidResourceId,
                                         MakeMaterial(1.0f, 0.28f, 0.18f), 0.50f,
                                         1.42f, 0.41f);
    floorModel_ = models.CreatePlaneHandle(kInvalidResourceId,
                                           MakeMaterial(0.18f, 0.20f, 0.20f));
    attackRangeModel_ = models.CreateBoxHandle(
        kInvalidResourceId, MakeMaterial(1.0f, 0.86f, 0.20f, 0.34f), 1.0f, 0.08f, 1.0f);
    guardMarkerModel_ = models.CreateBoxHandle(kInvalidResourceId,
                                               MakeMaterial(0.15f, 0.9f, 1.0f), 0.8f,
                                               0.25f, 0.12f);
}

void GameScene::CaptureFrameInput() {
    if (!ctx_ || !ctx_->systems.input) {
        return;
    }

    const Input& input = *ctx_->systems.input;
    if (input.IsKeyTrigger(DIK_R)) {
        ResetPhaseOne();
        return;
    }

    float cameraYawInput = 0.0f;
    if (input.IsKeyPress(DIK_LEFT)) {
        cameraYawInput -= 1.0f;
    }
    if (input.IsKeyPress(DIK_RIGHT)) {
        cameraYawInput += 1.0f;
    }
    const float rightStickX = input.GetGamepadRightStickX();
    if (std::abs(rightStickX) > kCameraStickDeadZone) {
        cameraYawInput += rightStickX;
    }
    if (cameraYawInput != 0.0f) {
        const float dt = ctx_ ? (std::max)(0.0f, ctx_->frame.deltaTime) : 0.0f;
        combatCameraYaw_ =
            std::remainder(combatCameraYaw_ + cameraYawInput * kCameraYawSpeed * dt,
                           kPi * 2.0f);
        UpdateCombatCamera();
    }

    pendingInput_.movement = ReadPlayerMovement(input);
    pendingInput_.guardHeld =
        input.IsKeyPress(DIK_L) ||
        input.IsGamepadButtonPress(static_cast<WORD>(XINPUT_GAMEPAD_LEFT_SHOULDER));
    CaptureActionInputs(input);
}

GameScene::Vec2 GameScene::ReadPlayerMovement(const Input& input) const {
    Vec2 inputMove{};
    if (input.IsKeyPress(DIK_A)) {
        inputMove.x -= 1.0f;
    }
    if (input.IsKeyPress(DIK_D)) {
        inputMove.x += 1.0f;
    }
    if (input.IsKeyPress(DIK_W)) {
        inputMove.z += 1.0f;
    }
    if (input.IsKeyPress(DIK_S)) {
        inputMove.z -= 1.0f;
    }

    inputMove.x += input.GetGamepadLeftStickX();
    inputMove.z += input.GetGamepadLeftStickY();
    inputMove = Normalize(inputMove);
    if (HasDirection(inputMove)) {
        const float cameraYaw = combatCamera_.GetRotation().y;
        const Vec2 cameraForward{std::sin(cameraYaw), std::cos(cameraYaw)};
        const Vec2 cameraRight{std::cos(cameraYaw), -std::sin(cameraYaw)};
        return Normalize(Vec2{cameraRight.x * inputMove.x + cameraForward.x * inputMove.z,
                              cameraRight.z * inputMove.x + cameraForward.z * inputMove.z});
    }
    return {};
}

void GameScene::CaptureActionInputs(const Input& input) {
    if (input.IsKeyTrigger(DIK_SPACE) ||
        input.IsGamepadButtonTrigger(static_cast<WORD>(XINPUT_GAMEPAD_B))) {
        pendingInput_.dodgeRequested = true;
    }

    if (input.IsKeyTrigger(DIK_J) ||
        input.IsGamepadButtonTrigger(static_cast<WORD>(XINPUT_GAMEPAD_X))) {
        pendingLightInput_ = true;
    }

    if (input.IsKeyTrigger(DIK_K) ||
        input.IsGamepadButtonTrigger(static_cast<WORD>(XINPUT_GAMEPAD_Y))) {
        pendingHeavyInput_ = true;
    }

    if (input.IsKeyTrigger(DIK_E) || input.IsGamepadRightTriggerTrigger()) {
        pendingExBoostInput_ = true;
    }

    if (input.IsKeyTrigger(DIK_TAB) ||
        input.IsGamepadButtonTrigger(static_cast<WORD>(XINPUT_GAMEPAD_DPAD_UP))) {
        pendingStyleSwitchInput_ = true;
    }

    if (input.IsKeyTrigger(DIK_C) ||
        input.IsGamepadButtonTrigger(static_cast<WORD>(XINPUT_GAMEPAD_RIGHT_SHOULDER))) {
        pendingLockCycleInput_ = true;
    }
}

void GameScene::StepCombat() {
    ++debug_.combatFrame;
    debug_.lastHitboxActive = false;
    debug_.lastHitConnected = false;
    debug_.lastBlocked = false;
    debug_.lastDodged = false;
    debug_.lastKnockback = 0.0f;
    debug_.lastDefense = "None";
    debug_.lastDistance = Distance(player_.position, TargetEnemy().position);

    combatInput_ = pendingInput_;
    pendingInput_.dodgeRequested = false;
    ConsumePendingInputs();
    UpdateFeedbackTimers();
    ApplyRequestedCombatActions();

    if (hitstopFrames_ > 0) {
        --hitstopFrames_;
        UpdateCombatCamera();
        return;
    }

    FaceCombatants();
    UpdateEnemyCombatants();
    UpdatePlayerCombatState();
    inputBuffer_.Tick();
    UpdateCombatCamera();
}

void GameScene::ConsumePendingInputs() {
    if (pendingLightInput_) {
        inputBuffer_.Push(CombatCommand::Light, kInputBufferFrames);
        pendingLightInput_ = false;
    }
    if (pendingHeavyInput_) {
        inputBuffer_.Push(CombatCommand::Heavy, kInputBufferFrames);
        pendingHeavyInput_ = false;
    }
    if (pendingExBoostInput_) {
        exBoostRequested_ = true;
        pendingExBoostInput_ = false;
    }
    if (pendingStyleSwitchInput_) {
        styleSwitchRequested_ = true;
        pendingStyleSwitchInput_ = false;
    }
    if (pendingLockCycleInput_) {
        CycleLockOnTarget(1);
        pendingLockCycleInput_ = false;
    }
}

void GameScene::ApplyRequestedCombatActions() {
    if (exBoostRequested_) {
        TryActivateExBoost();
        exBoostRequested_ = false;
    }

    if (styleSwitchRequested_) {
        TryToggleCombatStyle();
        styleSwitchRequested_ = false;
    }
}

void GameScene::FaceCombatants() {
    if (lockOnActive_) {
        if (!TargetEnemy().IsAlive()) {
            targetEnemyIndex_ = FindNearestEnemyIndex();
            lockOnActive_ = TargetEnemy().IsAlive();
        }
        FaceActorToward(player_, TargetEnemy());
    }
    FaceActorToward(enemy_, player_);
    for (CombatActor& enemy : supportEnemies_) {
        if (!enemy.IsAlive()) {
            continue;
        }
        FaceActorToward(enemy, player_);
    }
}

void GameScene::UpdateEnemyCombatants() {
    UpdateEnemyActor(enemy_);
    for (CombatActor& enemy : supportEnemies_) {
        if (!enemy.IsAlive()) {
            continue;
        }
        UpdateEnemyActor(enemy);
    }

    UpdateEnemyTraining(enemy_);
    for (CombatActor& enemy : supportEnemies_) {
        UpdateEnemyTraining(enemy);
    }
}

void GameScene::UpdatePlayerCombatState() {
    if (player_.state == CombatState::HitStun) {
        UpdateHitStun(player_);
    } else if (player_.state == CombatState::Down) {
        UpdateDown(player_);
    } else if (player_.state == CombatState::GuardStun) {
        UpdateGuardStun(player_);
    } else if (player_.state == CombatState::Dodge) {
        UpdateDodge(player_);
    } else if (player_.state == CombatState::Attack) {
        UpdatePlayerAttack();
    } else if (player_.state == CombatState::Guard) {
        UpdatePlayerGuard();
    } else if (player_.state == CombatState::Idle) {
        UpdatePlayerIdle();
    }
}

void GameScene::UpdateCombatCamera() {
    const Vec2 center{player_.position.x, player_.position.z};
    constexpr float cameraDistance = 4.8f - 1.15f;
    const Vec2 cameraForward{std::sin(combatCameraYaw_), std::cos(combatCameraYaw_)};
    const Vec2 cameraPosition{center.x - cameraForward.x * cameraDistance,
                              center.z - cameraForward.z * cameraDistance};
    combatCamera_.SetPosition({cameraPosition.x, 2.7f, cameraPosition.z});
    if (cameraShakeFrames_ > 0) {
        const float phase = static_cast<float>(debug_.combatFrame);
        const float xShake = std::sin(phase * 1.71f) * cameraShakeMagnitude_;
        const float yShake = std::cos(phase * 2.17f) * cameraShakeMagnitude_ * 0.65f;
        combatCamera_.SetPosition({cameraPosition.x + xShake, 2.7f + yShake,
                                   cameraPosition.z});
    }
    combatCamera_.SetRotation({0.34f, combatCameraYaw_, 0.0f});
}

void GameScene::UpdateFeedbackTimers() {
    if (comboTimerFrames_ > 0) {
        --comboTimerFrames_;
        if (comboTimerFrames_ == 0) {
            comboCount_ = 0;
        }
    }

    if (cameraShakeFrames_ > 0) {
        --cameraShakeFrames_;
        if (cameraShakeFrames_ == 0) {
            cameraShakeMagnitude_ = 0.0f;
        }
    }

    if (exBoostFrames_ > 0) {
        --exBoostFrames_;
        exGauge_ = std::clamp(exGauge_ - kExBoostGaugeDrainPerFrame, 0.0f,
                              kMaxExGauge);
        if (exGauge_ <= 0.0f) {
            exBoostFrames_ = 0;
        }
    }
}

void GameScene::UpdatePlayerIdle() {
    const Vec2 move = ReadMovementInput();
    player_.position.x += move.x * kPlayerMoveSpeed * kFixedCombatDt;
    player_.position.z += move.z * kPlayerMoveSpeed * kFixedCombatDt;
    if (lockOnActive_) {
        FaceActorToward(player_, TargetEnemy());
    } else {
        FaceActorTowardMovement(player_, move);
    }

    if (TryStartDownAttack()) {
        return;
    }

    if (TryStartExAction()) {
        return;
    }

    if (IsDodgeRequested()) {
        StartDodge(player_);
        return;
    }

    if (IsGuardHeld()) {
        StartGuard(player_);
        return;
    }

    if (inputBuffer_.Consume(CombatCommand::Light)) {
        StartAttack(player_, MoveId::L1);
    }
}

void GameScene::UpdatePlayerAttack() {
    ++player_.frameInState;
    const AttackData& attack = GetAttackData(player_.currentMove);
    ApplyAttackMovement(player_, attack);
    if (IsExBoostActive() && player_.frameInState < GetAttackData(player_.currentMove).total - 1 &&
        debug_.combatFrame % 2 == 0) {
        ++player_.frameInState;
        ApplyAttackMovement(player_, attack);
    }

    TryResolveAttackHit(player_, enemy_);
    for (CombatActor& enemy : supportEnemies_) {
        if (!enemy.IsAlive()) {
            continue;
        }
        TryResolveAttackHit(player_, enemy);
    }

    if (TryCancelPlayerAttackToDodge(attack)) {
        return;
    }

    if (TryChainPlayerAttack(attack)) {
        return;
    }

    if (player_.frameInState >= attack.total) {
        player_.state = CombatState::Idle;
        player_.currentMove = MoveId::None;
        player_.frameInState = 0;
        player_.hitApplied = false;
    }
}

void GameScene::UpdatePlayerGuard() {
    ++player_.frameInState;
    for (size_t i = 0; i < kEnemyCount; ++i) {
        if (TryStartCounter(EnemyAt(i))) {
            return;
        }
    }
    if (IsDodgeRequested()) {
        StartDodge(player_);
        return;
    }

    if (!IsGuardHeld()) {
        player_.state = CombatState::Idle;
        player_.frameInState = 0;
    }
}

void GameScene::UpdateGuardStun(CombatActor& actor) {
    ++actor.frameInState;
    if (actor.frameInState >= actor.guardStunFrames) {
        actor.state = actor.IsAlive() ? CombatState::Idle : CombatState::Down;
        actor.frameInState = 0;
        actor.guardStunFrames = 0;
    }
}

void GameScene::UpdateDodge(CombatActor& actor) {
    ++actor.frameInState;
    const DodgeData& dodge = GetDodgeData();
    const bool moving = actor.frameInState <= dodge.total;
    const bool recovery = actor.frameInState > dodge.total &&
                          actor.frameInState <= dodge.total + kDodgeRecoveryFrames;
    UpdateDodgeMovement(actor, dodge, moving);
    if (TryChainDodge(actor, moving, recovery)) {
        return;
    }
    FinishDodgeIfComplete(actor, dodge);
}

void GameScene::UpdateDodgeMovement(CombatActor& actor, const DodgeData& dodge,
                                    bool moving) {
    if (moving && actor.orbitDodgeActive) {
        const float t =
            std::clamp(static_cast<float>(actor.frameInState) /
                           static_cast<float>(dodge.total),
                       0.0f, 1.0f);
        const float easedT = t * t * (3.0f - 2.0f * t);
        const float angle = actor.orbitStartAngle + actor.orbitAngleDelta * easedT;
        actor.position.x = actor.orbitCenter.x + std::cos(angle) * actor.orbitRadius;
        actor.position.z = actor.orbitCenter.z + std::sin(angle) * actor.orbitRadius;
        const Vec2 toCenter{actor.orbitCenter.x - actor.position.x,
                            actor.orbitCenter.z - actor.position.z};
        actor.facing = NormalizeOr(toCenter, actor.facing);
    } else if (moving) {
        const float stepDistance = actor.dodgeDistance / static_cast<float>(dodge.total);
        actor.position.x += actor.dodgeDirection.x * stepDistance;
        actor.position.z += actor.dodgeDirection.z * stepDistance;
    } else if (actor.hasLastOrbitTarget) {
        const Vec2 toTarget{actor.lastOrbitTarget.x - actor.position.x,
                            actor.lastOrbitTarget.z - actor.position.z};
        actor.facing = NormalizeOr(toTarget, actor.facing);
    }
}

bool GameScene::TryChainDodge(const CombatActor& actor, bool moving, bool recovery) {
    if (&actor == &player_ && actor.dodgeChainCount < kMaxDodgeChainCount &&
        moving && actor.frameInState >= kDoubleSwayStartFrame && IsDodgeRequested()) {
        StartDodge(player_);
        debug_.lastDefense = "Double sway";
        return true;
    }

    if (&actor == &player_ && recovery && actor.hasLastOrbitTarget) {
        if (inputBuffer_.Consume(CombatCommand::Heavy)) {
            StartAttack(player_, MoveId::SwayAttack);
            return true;
        }
        if (inputBuffer_.Consume(CombatCommand::Light)) {
            StartAttack(player_, MoveId::L1);
            return true;
        }
    }
    return false;
}

void GameScene::FinishDodgeIfComplete(CombatActor& actor, const DodgeData& dodge) {
    if (actor.frameInState >= dodge.total + kDodgeRecoveryFrames) {
        actor.state = CombatState::Idle;
        actor.frameInState = 0;
        actor.dodgeChainCount = 0;
        actor.dodgeDistance = dodge.distance;
        actor.orbitDodgeActive = false;
        actor.hasLastOrbitTarget = false;
    }
}

void GameScene::UpdateHitStun(CombatActor& actor) {
    ++actor.frameInState;
    if (IsEnemyActor(actor) && player_.state == CombatState::Attack) {
        const Vec2 facing = AttackFacing(player_);
        const Vec2 targetPosition{player_.position.x + facing.x * kHitPullTargetDistance,
                                  player_.position.z + facing.z * kHitPullTargetDistance};
        const Vec2 toTarget{targetPosition.x - actor.position.x,
                            targetPosition.z - actor.position.z};
        if (HasDirection(toTarget)) {
            const float pullDistance = (std::min)(kHitPullSpeed, Distance({0.0f, 0.0f}, toTarget));
            const Vec2 pullDirection = Normalize(toTarget);
            actor.position.x += pullDirection.x * pullDistance;
            actor.position.z += pullDirection.z * pullDistance;
        }
    }
    if (actor.frameInState >= actor.hitstunFrames) {
        actor.state = actor.IsAlive() ? CombatState::Idle : CombatState::Down;
        actor.currentMove = MoveId::None;
        actor.frameInState = 0;
        actor.hitstunFrames = 0;
    }
}

void GameScene::UpdateDown(CombatActor& actor) {
    if (!actor.IsAlive()) {
        return;
    }

    ++actor.frameInState;
    if (actor.downFrames > 0) {
        --actor.downFrames;
    }

    if (actor.downFrames <= 0) {
        actor.state = CombatState::Idle;
        actor.frameInState = 0;
    }
}

void GameScene::UpdateEnemyActor(CombatActor& actor) {
    if (!actor.IsAlive()) {
        return;
    }

    if (actor.state == CombatState::HitStun) {
        UpdateHitStun(actor);
        return;
    }
    if (actor.state == CombatState::Down) {
        UpdateDown(actor);
        return;
    }
    if (actor.state == CombatState::GuardStun) {
        UpdateGuardStun(actor);
        return;
    }
    if (actor.state == CombatState::Attack) {
        ++actor.frameInState;
        const AttackData& attack = GetAttackData(actor.currentMove);
        ApplyAttackMovement(actor, attack);
        TryResolveAttackHit(actor, player_);
        if (actor.frameInState >= attack.total) {
            actor.state = CombatState::Idle;
            actor.currentMove = MoveId::None;
            actor.frameInState = 0;
            actor.hitApplied = false;
        }
    }
}

void GameScene::UpdateEnemyTraining(CombatActor& actor) {
    if (!actor.IsAlive()) {
        return;
    }
    if (actor.state != CombatState::Idle) {
        return;
    }

    const bool supportEnemy = &actor != &enemy_;
    const float distance = Distance(actor.position, player_.position);
    const Vec2 toPlayer =
        Normalize(Vec2{player_.position.x - actor.position.x,
                       player_.position.z - actor.position.z});
    const AttackData& playerAttack = GetAttackData(player_.currentMove);
    const bool playerInRecovery =
        player_.state == CombatState::Attack &&
        player_.frameInState >= playerAttack.startup + playerAttack.active;

    const AttackData& poke = GetAttackData(MoveId::EnemyPoke);
    const AttackData& heavy = GetAttackData(MoveId::EnemyHeavy);
    const bool playerGuarding = player_.state == CombatState::Guard;
    const bool wantsHeavy =
        playerGuarding || playerInRecovery || (!supportEnemy && actor.aiAttackCount >= 2);
    const bool wantsPoke =
        player_.state == CombatState::Idle || player_.state == CombatState::Attack ||
        player_.state == CombatState::Dodge;
    const bool attackReady =
        actor.aiCooldownFrames <= 0 && enemyTrainingCooldown_ <= 0 &&
        !IsAnyEnemyAttacking() && CanStartAttack(actor);

    if (attackReady && (wantsPoke || wantsHeavy)) {
        const bool canHeavy = wantsHeavy && distance <= heavy.range;
        const bool canPoke = wantsPoke && distance <= poke.range;
        if (canHeavy || canPoke) {
            const bool shouldUseHeavy = canHeavy;
            StartAttack(actor, shouldUseHeavy ? MoveId::EnemyHeavy : MoveId::EnemyPoke);
            actor.aiAttackCount = shouldUseHeavy ? 0 : actor.aiAttackCount + 1;
            actor.aiIntent = EnemyIntent::Strafe;
            actor.aiIntentFrames = kEnemyIntentMaxFrames;
            actor.aiCooldownFrames =
                supportEnemy ? kEnemySupportAttackCooldown : kEnemyTrainingAttackCooldown;
            return;
        }

        actor.aiIntent = EnemyIntent::Approach;
    } else {
        actor.aiIntent = EnemyIntent::Strafe;
    }

    actor.aiStrafePhase += 0.035f;
    const Vec2 strafe{toPlayer.z * actor.aiStrafeSign,
                      -toPlayer.x * actor.aiStrafeSign};
    Vec2 movement{};
    float speedScale = kEnemyStrafeSpeedScale;
    if (actor.aiIntent == EnemyIntent::Approach) {
        movement = toPlayer;
        speedScale = kEnemyAttackRunSpeedScale;
    } else {
        const float distanceError = distance - kEnemyPreferredRange;
        const float radialCorrection = std::clamp(distanceError * 1.15f, -0.72f, 0.72f);
        movement = Normalize(Vec2{strafe.x + toPlayer.x * radialCorrection,
                                  strafe.z + toPlayer.z * radialCorrection});
        if (distance < kEnemyRetreatRange) {
            movement = Normalize(Vec2{strafe.x - toPlayer.x * 0.95f,
                                      strafe.z - toPlayer.z * 0.95f});
            speedScale = kEnemyRetreatSpeedScale;
        } else if (distance > kEnemyEngageRange) {
            movement = Normalize(Vec2{strafe.x + toPlayer.x * 0.95f,
                                      strafe.z + toPlayer.z * 0.95f});
            speedScale = 0.9f;
        }
    }

    if (HasDirection(movement)) {
        if (actor.aiIntent == EnemyIntent::Approach) {
            actor.aiMoveDirection = movement;
        } else {
            actor.aiMoveDirection =
                Normalize(Vec2{actor.aiMoveDirection.x * (1.0f - kEnemyMoveBlend) +
                                   movement.x * kEnemyMoveBlend,
                               actor.aiMoveDirection.z * (1.0f - kEnemyMoveBlend) +
                                   movement.z * kEnemyMoveBlend});
        }
    } else {
        actor.aiMoveDirection =
            Normalize(Vec2{actor.aiMoveDirection.x * (1.0f - kEnemyMoveBlend),
                           actor.aiMoveDirection.z * (1.0f - kEnemyMoveBlend)});
        speedScale = 0.0f;
    }
    if (HasDirection(actor.aiMoveDirection) && speedScale > 0.0f) {
        actor.position.x +=
            actor.aiMoveDirection.x * kEnemyMoveSpeed * speedScale * kFixedCombatDt;
        actor.position.z +=
            actor.aiMoveDirection.z * kEnemyMoveSpeed * speedScale * kFixedCombatDt;
    }

    if (actor.aiCooldownFrames > 0) {
        --actor.aiCooldownFrames;
        return;
    }

    if (enemyTrainingCooldown_ > 0) {
        --enemyTrainingCooldown_;
        return;
    }
}

void GameScene::ApplyAttackMovement(CombatActor& actor, const AttackData& attack) {
    const int advanceFrames = attack.startup + attack.active;
    if (actor.state != CombatState::Attack || attack.advanceDistance <= 0.0f ||
        advanceFrames <= 0 || actor.frameInState <= 0 ||
        actor.frameInState > advanceFrames) {
        return;
    }

    Vec2 forward = NormalizeOr(actor.attackFacing, actor.facing);
    if (&actor == &player_) {
        const size_t targetIndex = FindNearestEnemyIndex();
        const CombatActor& target = EnemyAt(targetIndex);
        if (target.IsAlive()) {
            const Vec2 toTarget{target.position.x - actor.position.x,
                                target.position.z - actor.position.z};
            const float targetDistance = Distance(actor.position, target.position);
            const Vec2 targetDirection = Normalize(toTarget);
            if (targetDistance <= kPlayerAttackHomingRange &&
                Dot(forward, targetDirection) > -0.15f) {
                forward = NormalizeOr(
                    Vec2{forward.x * (1.0f - kPlayerAttackHomingStrength) +
                             targetDirection.x * kPlayerAttackHomingStrength,
                         forward.z * (1.0f - kPlayerAttackHomingStrength) +
                             targetDirection.z * kPlayerAttackHomingStrength},
                    forward);
            }
        }
    }
    const float progress =
        static_cast<float>(actor.frameInState - 1) / static_cast<float>(advanceFrames);
    const float frontLoad = std::clamp(1.75f - progress * 1.35f, 0.4f, 1.75f);
    const float stepDistance =
        attack.advanceDistance * frontLoad / static_cast<float>(advanceFrames);
    actor.position.x += forward.x * stepDistance;
    actor.position.z += forward.z * stepDistance;
    actor.attackOrigin = actor.position;
}

bool GameScene::CanStartAttack(const CombatActor& actor) {
    if (!actor.IsAlive()) {
        return false;
    }

    switch (actor.state) {
    case CombatState::Idle:
    case CombatState::Attack:
    case CombatState::Guard:
    case CombatState::Dodge:
        return true;
    case CombatState::GuardStun:
    case CombatState::HitStun:
    case CombatState::Down:
        return false;
    }

    return false;
}

bool GameScene::CanStartGuard(const CombatActor& actor) {
    return actor.IsAlive() && actor.state == CombatState::Idle;
}

bool GameScene::CanStartDodge(const CombatActor& actor) {
    if (!actor.IsAlive()) {
        return false;
    }

    switch (actor.state) {
    case CombatState::Idle:
    case CombatState::Attack:
    case CombatState::Guard:
    case CombatState::Dodge:
        return true;
    case CombatState::GuardStun:
    case CombatState::HitStun:
    case CombatState::Down:
        return false;
    }

    return false;
}

void GameScene::StartAttack(CombatActor& actor, MoveId move) {
    if (!CanStartAttack(actor)) {
        return;
    }

    actor.state = CombatState::Attack;
    actor.currentMove = move;
    actor.frameInState = 0;
    actor.hitApplied = false;
    actor.dodgeChainCount = 0;
    actor.attackSerial = ++nextAttackSerial_;
    actor.attackOrigin = actor.position;
    actor.attackFacing = NormalizeOr(actor.facing, {0.0f, 1.0f});
    actor.hasLastOrbitTarget = false;
}

void GameScene::StartGuard(CombatActor& actor) {
    if (!CanStartGuard(actor)) {
        return;
    }

    actor.state = CombatState::Guard;
    actor.currentMove = MoveId::None;
    actor.frameInState = 0;
    actor.dodgeChainCount = 0;
}

void GameScene::StartDodge(CombatActor& actor) {
    if (!CanStartDodge(actor)) {
        return;
    }

    const bool chainedFromDodge = actor.state == CombatState::Dodge;
    const bool chainedFromOrbitDodge = chainedFromDodge && actor.orbitDodgeActive;
    const Vec2 previousOrbitCenter = actor.orbitCenter;
    const float previousOrbitAngleDelta = actor.orbitAngleDelta;
    actor.state = CombatState::Dodge;
    actor.currentMove = MoveId::None;
    actor.frameInState = 0;
    actor.hitApplied = false;
    actor.dodgeChainCount = chainedFromDodge ? actor.dodgeChainCount + 1 : 1;
    const Vec2 inputDirection = &actor == &player_ ? ReadMovementInput() : Vec2{};
    actor.dodgeDistance = GetDodgeData().distance;
    actor.orbitDodgeActive = false;
    if (!chainedFromOrbitDodge) {
        actor.hasLastOrbitTarget = false;
    }

    if (TryStartTargetedOrbitDodge(actor, inputDirection)) {
        return;
    }
    if (chainedFromOrbitDodge &&
        TryContinueOrbitDodge(actor, previousOrbitCenter, previousOrbitAngleDelta)) {
        return;
    }

    actor.dodgeDirection =
        NormalizeOr(inputDirection, Vec2{-actor.facing.x, -actor.facing.z});
    actor.hasLastOrbitTarget = false;
}

void GameScene::StartOrbitDodge(CombatActor& actor, Vec2 center, float angleDelta,
                                Vec2 fallbackDirection) {
    actor.orbitDodgeActive = true;
    actor.orbitCenter = center;
    actor.orbitRadius = std::clamp(Distance(actor.position, center), kOrbitSwayMinRadius,
                                   kOrbitSwayMaxRadius);
    actor.orbitStartAngle =
        std::atan2(actor.position.z - center.z, actor.position.x - center.x);
    actor.orbitAngleDelta = angleDelta;
    const float endAngle = actor.orbitStartAngle + actor.orbitAngleDelta;
    actor.dodgeDirection = NormalizeOr(Vec2{std::cos(endAngle), std::sin(endAngle)},
                                       fallbackDirection);
    actor.lastOrbitTarget = center;
    actor.hasLastOrbitTarget = true;
    debug_.lastDefense = "Orbit sway";
}

bool GameScene::TryStartTargetedOrbitDodge(CombatActor& actor, Vec2 inputDirection) {
    const CombatActor* orbitTarget = &actor == &player_ ? FindOrbitSwayTarget() : nullptr;
    if (orbitTarget != nullptr && HasDirection(inputDirection)) {
        const CombatActor& target = *orbitTarget;
        const Vec2 toTarget =
            Normalize(Vec2{target.position.x - actor.position.x,
                           target.position.z - actor.position.z});
        const Vec2 orbitRight{toTarget.z, -toTarget.x};
        const float orbitInput = Dot(inputDirection, orbitRight);
        const float approachInput = Dot(inputDirection, toTarget);
        if (std::abs(orbitInput) < kOrbitSwayInputThreshold) {
            if (std::abs(approachInput) >= kOrbitSwayApproachThreshold) {
                const Vec2 playerRight{actor.facing.z, -actor.facing.x};
                const float sideBias = Dot(orbitRight, playerRight);
                StartOrbitDodge(actor, target.position,
                                sideBias >= 0.0f ? kOrbitSwayArcRadians
                                                 : -kOrbitSwayArcRadians,
                                orbitRight);
                debug_.lastDefense =
                    approachInput >= 0.0f ? "Slip orbit sway" : "Retreat orbit sway";
                return true;
            }
            actor.dodgeDirection =
                NormalizeOr(inputDirection, Vec2{-actor.facing.x, -actor.facing.z});
            return true;
        }

        StartOrbitDodge(actor, target.position,
                        orbitInput >= 0.0f ? kOrbitSwayArcRadians
                                           : -kOrbitSwayArcRadians,
                        orbitRight);
        return true;
    }
    return false;
}

bool GameScene::TryContinueOrbitDodge(CombatActor& actor, Vec2 previousCenter,
                                      float previousAngleDelta) {
    if (&actor == &player_) {
        const Vec2 toCenter{previousCenter.x - actor.position.x,
                            previousCenter.z - actor.position.z};
        if (HasDirection(toCenter) &&
            Distance(actor.position, previousCenter) <= kOrbitSwayTargetRange) {
            const Vec2 orbitRight{toCenter.z, -toCenter.x};
            StartOrbitDodge(actor, previousCenter, previousAngleDelta, orbitRight);
            debug_.lastDefense = "Double orbit sway";
            return true;
        }
    }
    return false;
}

bool GameScene::TryStartCounter(CombatActor& attacker) {
    if (player_.state != CombatState::Guard || attacker.state != CombatState::Attack) {
        return false;
    }

    const AttackData& enemyAttack = GetAttackData(attacker.currentMove);
    const int framesUntilImpact = enemyAttack.startup - attacker.frameInState;
    if (framesUntilImpact < 0 || framesUntilImpact > 5) {
        return false;
    }

    if (!inputBuffer_.Consume(CombatCommand::Heavy)) {
        return false;
    }

    attacker.state = CombatState::Idle;
    attacker.currentMove = MoveId::None;
    attacker.frameInState = 0;
    attacker.hitApplied = false;
    attacker.aiCooldownFrames = kEnemySupportAttackCooldown;
    StartAttack(player_, MoveId::CounterAttack);
    hitstopFrames_ = 4;
    debug_.lastDefense = "Counter";
    StartCameraShake(10, 0.06f);
    return true;
}

bool GameScene::TryStartDownAttack() {
    const CombatActor& target = TargetEnemy();
    if (target.state != CombatState::Down || !target.IsAlive()) {
        return false;
    }

    if (Distance(player_.position, target.position) > kDownAttackDistance) {
        return false;
    }

    if (!inputBuffer_.Consume(CombatCommand::Heavy)) {
        return false;
    }

    FaceActorToward(player_, TargetEnemy());
    StartAttack(player_, MoveId::DownAttack);
    return true;
}

bool GameScene::TryStartExAction() {
    const CombatActor& target = TargetEnemy();
    if (!IsExBoostActive() || !target.IsAlive() || target.state == CombatState::Down) {
        return false;
    }

    if (exGauge_ < kExActionCost ||
        Distance(player_.position, target.position) > kExActionDistance) {
        return false;
    }

    if (!inputBuffer_.Consume(CombatCommand::Heavy)) {
        return false;
    }

    exGauge_ = std::clamp(exGauge_ - kExActionCost, 0.0f, kMaxExGauge);
    FaceActorToward(player_, TargetEnemy());
    StartAttack(player_, MoveId::ExAction);
    if (exGauge_ <= 0.0f) {
        exBoostFrames_ = 0;
    }
    hitstopFrames_ = 4;
    StartCameraShake(11, 0.08f);
    return true;
}

bool GameScene::TryActivateExBoost() {
    if (IsExBoostActive() || exGauge_ < kExBoostCost) {
        return false;
    }

    exGauge_ = std::clamp(exGauge_ - kExBoostCost, 0.0f, kMaxExGauge);
    exBoostFrames_ = kExBoostFrames;
    hitstopFrames_ = (std::max)(hitstopFrames_, 4);
    debug_.lastDefense = "EX Boost";
    StartCameraShake(11, 0.08f);
    return true;
}

bool GameScene::IsExBoostActive() const {
    return exBoostFrames_ > 0;
}

bool GameScene::TryToggleCombatStyle() {
    if (player_.state == CombatState::Attack && !IsExBoostActive()) {
        return false;
    }

    combatStyle_ = combatStyle_ == CombatStyle::Single ? CombatStyle::Crowd
                                                       : CombatStyle::Single;
    hitstopFrames_ = (std::max)(hitstopFrames_, 5);
    debug_.lastDefense = "Style switch";
    StartCameraShake(8, 0.035f);
    return true;
}

bool GameScene::TryChainPlayerAttack(const AttackData& attack) {
    const int frame = player_.frameInState;
    if (frame < attack.cancelFrom || frame > attack.cancelTo) {
        return false;
    }

    for (CombatCommand command : {CombatCommand::Heavy, CombatCommand::Light}) {
        const MoveId next = ResolveChainMove(attack, command);
        if (next == MoveId::None) {
            continue;
        }
        if (command == CombatCommand::Heavy &&
            (combatStyle_ != CombatStyle::Single || !IsSingleStyleFinisher(next))) {
            continue;
        }

        if (inputBuffer_.Consume(command)) {
            StartAttack(player_, next);
            return true;
        }
    }

    if (TryStartExAction()) {
        return true;
    }

    return false;
}

bool GameScene::TryCancelPlayerAttackToDodge(const AttackData& attack) {
    const int frame = player_.frameInState;
    if (frame < attack.cancelFrom || frame > attack.cancelTo) {
        return false;
    }

    if (!IsDodgeRequested()) {
        return false;
    }

    StartDodge(player_);
    debug_.lastDefense = "Cancel sway";
    return true;
}

void GameScene::TryResolveAttackHit(CombatActor& attacker, CombatActor& defender) {
    const int frame = attacker.frameInState;
    const AttackData attack =
        MakeEffectiveAttackData(attacker, GetAttackData(attacker.currentMove));
    const bool active = frame >= attack.startup && frame < attack.startup + attack.active;
    debug_.lastHitboxActive = active;
    debug_.lastDistance = Distance(player_.position, TargetEnemy().position);
    const bool alreadyHit = &attacker == &player_
                                ? defender.lastHitAttackSerial == attacker.attackSerial
                                : attacker.hitApplied;

    if (!active || alreadyHit || !defender.IsAlive()) {
        return;
    }

    const Vec2 origin = AttackOrigin(attacker);
    const Vec2 facing = AttackFacing(attacker);
    const Vec2 toDefender{defender.position.x - origin.x,
                          defender.position.z - origin.z};
    const float forwardDistance = Dot(toDefender, facing);
    const Vec2 right{facing.z, -facing.x};
    const float sideDistance = std::abs(Dot(toDefender, right));
    const bool inHitArea = forwardDistance >= 0.0f &&
                           forwardDistance <= attack.range &&
                           sideDistance <= attack.halfWidth;

    if (inHitArea) {
        if (IsDodgeInvulnerable(defender)) {
            if (&attacker == &player_) {
                defender.lastHitAttackSerial = attacker.attackSerial;
            } else {
                attacker.hitApplied = true;
            }
            debug_.lastDodged = true;
            debug_.lastDefense = "Sway invulnerable";
            return;
        }

        if (defender.state == CombatState::Guard &&
            IsFacingIncomingAttack(defender, attacker)) {
            ApplyBlock(attacker, defender, attack);
            return;
        }

        if (defender.state == CombatState::Guard) {
            debug_.lastDefense = "Back attack";
        }
        ApplyHit(attacker, defender, attack);
    }
}

void GameScene::ApplyHit(CombatActor& attacker, CombatActor& defender,
                         const AttackData& attack) {
    if (&attacker == &player_) {
        defender.lastHitAttackSerial = attacker.attackSerial;
    } else {
        attacker.hitApplied = true;
    }
    int damage = attack.damage;
    if (&defender == &player_ && IsExBoostActive()) {
        const int absorbed = (attack.damage + 1) / 2;
        damage -= absorbed;
        exGauge_ = std::clamp(exGauge_ - static_cast<float>(absorbed), 0.0f,
                              kMaxExGauge);
        if (exGauge_ <= 0.0f) {
            exBoostFrames_ = 0;
        }
        debug_.lastDefense = "EX absorb";
    }

    if (!(IsEnemyActor(defender) && kEnemyInvincibleForDebug)) {
        defender.hp = (std::max)(0, defender.hp - damage);
    }
    const bool knockdown = IsKnockdownAttack(attack.id);
    defender.state = defender.IsAlive() && !knockdown ? CombatState::HitStun : CombatState::Down;
    defender.currentMove = MoveId::None;
    defender.frameInState = 0;
    defender.hitstunFrames = attack.hitstun;
    defender.downFrames = defender.state == CombatState::Down ? kKnockdownFrames : 0;
    defender.aiMoveDirection = {};
    if (IsKnockdownAttack(attack.id)) {
        ApplyKnockback(defender, AttackFacing(attacker), kFinisherKnockbackDistance);
    }
    hitstopFrames_ = attack.hitstop;
    debug_.lastHitConnected = true;
    AddHitFeedback(attacker, attack);
}

void GameScene::ApplyBlock(CombatActor& attacker, CombatActor& defender,
                           const AttackData& attack) {
    if (&attacker == &player_) {
        defender.lastHitAttackSerial = attacker.attackSerial;
    } else {
        attacker.hitApplied = true;
    }
    defender.state = CombatState::GuardStun;
    defender.currentMove = MoveId::None;
    defender.frameInState = 0;
    defender.guardStunFrames = attack.blockstun;
    ApplyKnockback(defender, AttackFacing(attacker), kBlockKnockbackDistance);
    const bool guardBreak = &attacker == &player_ &&
                            combatStyle_ == CombatStyle::Single &&
                            IsSingleStyleFinisher(attack.id);
    hitstopFrames_ = guardBreak ? 5 : 3;
    debug_.lastBlocked = true;
    debug_.lastDefense = guardBreak ? "Guard break" : "Guard";
    AddBlockFeedback(attacker);
    if (guardBreak) {
        StartCameraShake(7, 0.032f);
    }
}

void GameScene::ApplyKnockback(CombatActor& defender, Vec2 direction, float distance) {
    const Vec2 knockback = NormalizeOr(direction, defender.facing);
    defender.position.x += knockback.x * distance;
    defender.position.z += knockback.z * distance;
    debug_.lastKnockback = distance;
}

void GameScene::AddHitFeedback(const CombatActor& attacker, const AttackData& attack) {
    if (&attacker != &player_) {
        StartCameraShake(6, 0.018f);
        return;
    }

    ++comboCount_;
    comboTimerFrames_ = kComboTimerFrames;
    exGauge_ = std::clamp(exGauge_ + static_cast<float>(attack.damage) * 0.8f, 0.0f,
                          kMaxExGauge);

    const bool finisher = IsSingleStyleFinisher(attack.id);
    StartCameraShake(finisher ? 10 : 6, finisher ? 0.045f : 0.028f);
}

void GameScene::AddBlockFeedback(const CombatActor& attacker) {
    if (&attacker == &player_) {
        exGauge_ = std::clamp(exGauge_ + 1.5f, 0.0f, kMaxExGauge);
    }
    StartCameraShake(4, 0.014f);
}

void GameScene::StartCameraShake(int frames, float magnitude) {
    cameraShakeFrames_ = (std::max)(cameraShakeFrames_, frames);
    cameraShakeMagnitude_ = (std::max)(cameraShakeMagnitude_, magnitude);
}

void GameScene::FaceActorToward(CombatActor& actor, const CombatActor& target) {
    if (actor.state == CombatState::Attack) {
        return;
    }

    const Vec2 toTarget{target.position.x - actor.position.x,
                        target.position.z - actor.position.z};
    if (!HasDirection(toTarget)) {
        return;
    }

    actor.facing = Normalize(toTarget);
}

void GameScene::FaceActorTowardMovement(CombatActor& actor, Vec2 movement) {
    if (actor.state == CombatState::Attack || !HasDirection(movement)) {
        return;
    }

    actor.facing = Normalize(movement);
}

bool GameScene::IsEnemyActor(const CombatActor& actor) const {
    if (&actor == &enemy_) {
        return true;
    }

    return std::any_of(supportEnemies_.begin(), supportEnemies_.end(),
                       [&actor](const CombatActor& enemy) { return &actor == &enemy; });
}

GameScene::CombatActor& GameScene::TargetEnemy() {
    return EnemyAt(targetEnemyIndex_);
}

const GameScene::CombatActor& GameScene::TargetEnemy() const {
    return EnemyAt(targetEnemyIndex_);
}

GameScene::CombatActor& GameScene::EnemyAt(size_t index) {
    if (index == 0) {
        return enemy_;
    }

    return supportEnemies_[(std::min)(index, kEnemyCount - 1) - 1];
}

const GameScene::CombatActor& GameScene::EnemyAt(size_t index) const {
    if (index == 0) {
        return enemy_;
    }

    return supportEnemies_[(std::min)(index, kEnemyCount - 1) - 1];
}

size_t GameScene::FindNearestEnemyIndex() const {
    size_t bestIndex = 0;
    float bestDistance = 999999.0f;
    for (size_t i = 0; i < kEnemyCount; ++i) {
        const CombatActor& enemy = EnemyAt(i);
        if (!enemy.IsAlive()) {
            continue;
        }

        const float distance = Distance(player_.position, enemy.position);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}

const GameScene::CombatActor* GameScene::FindOrbitSwayTarget() const {
    const CombatActor* best = nullptr;
    float bestDistance = kOrbitSwayTargetRange;
    for (size_t i = 0; i < kEnemyCount; ++i) {
        const CombatActor& enemy = EnemyAt(i);
        if (!enemy.IsAlive()) {
            continue;
        }

        const float distance = Distance(player_.position, enemy.position);
        if (distance <= bestDistance) {
            best = &enemy;
            bestDistance = distance;
        }
    }

    return best;
}

void GameScene::CycleLockOnTarget(int direction) {
    (void)direction;
    if (lockOnActive_) {
        lockOnActive_ = false;
        debug_.lastDefense = "Lock off";
        StartCameraShake(3, 0.012f);
        return;
    }

    targetEnemyIndex_ = FindNearestEnemyIndex();
    if (!TargetEnemy().IsAlive()) {
        debug_.lastDefense = "Lock failed";
        return;
    }

    lockOnActive_ = true;
    debug_.lastDefense = "Lock on";
    StartCameraShake(4, 0.018f);
}

GameScene::Vec2 GameScene::ReadMovementInput() const {
    return combatInput_.movement;
}

bool GameScene::IsGuardHeld() const {
    return combatInput_.guardHeld;
}

bool GameScene::IsDodgeRequested() const {
    return combatInput_.dodgeRequested;
}

bool GameScene::IsAnyEnemyAttacking() const {
    if (enemy_.state == CombatState::Attack) {
        return true;
    }

    return std::any_of(supportEnemies_.begin(), supportEnemies_.end(),
                       [](const CombatActor& enemy) {
                           return enemy.IsAlive() && enemy.state == CombatState::Attack;
                       });
}

GameScene::AttackData GameScene::MakeEffectiveAttackData(
    const CombatActor& attacker, const AttackData& attack) const {
    AttackData effective = attack;
    const bool playerAttack = &attacker == &player_;
    const bool finisher = IsSingleStyleFinisher(attack.id);
    if (playerAttack && combatStyle_ == CombatStyle::Single && finisher) {
        effective.damage =
            (std::max)(1, static_cast<int>(std::round(
                              static_cast<float>(effective.damage) *
                              kSingleStyleFinisherDamageScale)));
        effective.blockstun += kSingleStyleFinisherBlockstunBonus;
        effective.hitstop += 1;
    } else if (playerAttack && combatStyle_ == CombatStyle::Crowd) {
        effective.damage =
            (std::max)(1, static_cast<int>(std::round(
                              static_cast<float>(effective.damage) *
                              kCrowdStyleDamageScale)));
        effective.range *= kCrowdStyleRangeScale;
        effective.halfWidth *= kCrowdStyleHalfWidthScale;
        effective.hitstop = (std::max)(3, effective.hitstop - 1);
    }
    return effective;
}

bool GameScene::IsDodgeInvulnerable(const CombatActor& actor) {
    if (actor.state != CombatState::Dodge) {
        return false;
    }

    const DodgeData& dodge = GetDodgeData();
    return actor.frameInState >= dodge.invulFrom && actor.frameInState <= dodge.invulTo;
}

bool GameScene::IsAttackHitboxActive(const CombatActor& actor) {
    if (actor.state != CombatState::Attack) {
        return false;
    }

    const AttackData& attack = GetAttackData(actor.currentMove);
    return actor.frameInState >= attack.startup &&
           actor.frameInState < attack.startup + attack.active;
}

bool GameScene::IsSingleStyleFinisher(MoveId move) {
    return move == MoveId::F1 || move == MoveId::F2 ||
           move == MoveId::F3 || move == MoveId::F4;
}

bool GameScene::IsFacingIncomingAttack(const CombatActor& defender,
                                       const CombatActor& attacker) {
    const Vec2 toAttacker =
        Normalize(Vec2{AttackOrigin(attacker).x - defender.position.x,
                       AttackOrigin(attacker).z - defender.position.z});
    return Dot(defender.facing, toAttacker) >= 0.25f;
}

bool GameScene::IsKnockdownAttack(MoveId move) {
    return move == MoveId::F4 || move == MoveId::CounterAttack ||
           move == MoveId::DownAttack || move == MoveId::ExAction;
}

const GameScene::AttackData& GameScene::GetAttackData(MoveId move) {
    static constexpr AttackData kNone{MoveId::None, "None", 0, 0, 0, 1, 0, 0,
                                      MoveId::None, MoveId::None, 0.0f, 0.0f,
                                      0, 0, 0, 0, 0.0f};
    static constexpr std::array<AttackData, 14> kAttacks{{
        {MoveId::L1, "L1", 12, 3, 13, 28, 15, 22, MoveId::L2, MoveId::F1, 1.10f,
         0.35f, 10, 14, 9, 5, 0.24f},
        {MoveId::L2, "L2", 14, 3, 15, 32, 18, 26, MoveId::L3, MoveId::F2, 1.15f,
         0.38f, 11, 15, 10, 5, 0.28f},
        {MoveId::L3, "L3", 16, 4, 18, 38, 22, 30, MoveId::L4, MoveId::F3, 1.20f,
         0.42f, 12, 18, 11, 6, 0.32f},
        {MoveId::L4, "L4", 18, 4, 22, 44, 26, 34, MoveId::None, MoveId::F4,
         1.28f, 0.48f, 14, 20, 13, 6, 0.38f},
        {MoveId::F1, "F1", 18, 4, 24, 46, 46, 46, MoveId::None, MoveId::None,
         1.20f, 0.42f, 18, 22, 16, 8, 0.42f},
        {MoveId::F2, "F2", 20, 5, 26, 51, 51, 51, MoveId::None, MoveId::None,
         1.25f, 0.46f, 21, 24, 18, 8, 0.48f},
        {MoveId::F3, "F3", 22, 6, 28, 56, 56, 56, MoveId::None, MoveId::None,
         1.30f, 0.52f, 24, 26, 19, 9, 0.54f},
        {MoveId::F4, "F4", 24, 8, 30, 62, 62, 62, MoveId::None, MoveId::None,
         1.40f, 0.58f, 30, 30, 22, 10, 0.62f},
        {MoveId::SwayAttack, "SWA", 11, 4, 17, 32, 32, 32, MoveId::None,
         MoveId::None, 1.00f, 0.35f, 13, 18, 12, 7, 0.36f},
        {MoveId::CounterAttack, "CTR", 1, 12, 29, 42, 42, 42, MoveId::None,
         MoveId::None, 1.35f, 0.45f, 24, 36, 0, 12, 0.62f},
        {MoveId::DownAttack, "DWN", 15, 5, 18, 38, 38, 38, MoveId::None,
         MoveId::None, 1.20f, 0.45f, 20, 20, 0, 8, 0.34f},
        {MoveId::ExAction, "EX", 4, 6, 32, 42, 42, 42, MoveId::None,
         MoveId::None, 1.55f, 0.60f, 38, 36, 0, 12, 0.95f},
        {MoveId::EnemyPoke, "EnemyPoke", 22, 5, 28, 55, 55, 55, MoveId::None,
         MoveId::None, 1.10f, 0.40f, 8, 14, 12, 5, 0.24f},
        {MoveId::EnemyHeavy, "EnemyHeavy", 36, 7, 34, 77, 77, 77, MoveId::None,
         MoveId::None, 1.45f, 0.55f, 18, 28, 18, 9, 0.42f},
    }};

    const auto match = std::find_if(kAttacks.begin(), kAttacks.end(),
                                    [move](const AttackData& attack) {
                                        return attack.id == move;
                                    });
    return match != kAttacks.end() ? *match : kNone;
}

const GameScene::DodgeData& GameScene::GetDodgeData() {
    static constexpr DodgeData kDodge{10, 2, 5, 0.75f};
    return kDodge;
}

GameScene::MoveId GameScene::ResolveChainMove(const AttackData& attack,
                                              CombatCommand command) {
    switch (command) {
    case CombatCommand::Light:
        return attack.lightChain;
    case CombatCommand::Heavy:
        return attack.heavyChain;
    default:
        return MoveId::None;
    }
}

float GameScene::Distance(const Vec2& a, const Vec2& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

float GameScene::Dot(const Vec2& a, const Vec2& b) {
    return a.x * b.x + a.z * b.z;
}

GameScene::Vec2 GameScene::Normalize(Vec2 value) {
    const float lengthSq = value.x * value.x + value.z * value.z;
    if (lengthSq <= 0.0001f) {
        return {0.0f, 0.0f};
    }

    const float invLength = 1.0f / std::sqrt(lengthSq);
    return {value.x * invLength, value.z * invLength};
}

bool GameScene::HasDirection(Vec2 value) {
    return value.x * value.x + value.z * value.z > 0.0001f;
}

GameScene::Vec2 GameScene::NormalizeOr(Vec2 value, Vec2 fallback) {
    const Vec2 normalized = Normalize(value);
    if (HasDirection(normalized)) {
        return normalized;
    }
    const Vec2 normalizedFallback = Normalize(fallback);
    return HasDirection(normalizedFallback) ? normalizedFallback : Vec2{0.0f, 1.0f};
}

GameScene::Vec2 GameScene::AttackOrigin(const CombatActor& actor) {
    return actor.state == CombatState::Attack ? actor.attackOrigin : actor.position;
}

GameScene::Vec2 GameScene::AttackFacing(const CombatActor& actor) {
    return actor.state == CombatState::Attack ? NormalizeOr(actor.attackFacing, actor.facing)
                                              : NormalizeOr(actor.facing, {0.0f, 1.0f});
}

const char* GameScene::StateName(CombatState state) {
    switch (state) {
    case CombatState::Idle:
        return "Idle";
    case CombatState::Attack:
        return "Attack";
    case CombatState::Guard:
        return "Guard";
    case CombatState::GuardStun:
        return "GuardStun";
    case CombatState::Dodge:
        return "Dodge";
    case CombatState::HitStun:
        return "HitStun";
    case CombatState::Down:
        return "Down";
    default:
        return "Unknown";
    }
}

const char* GameScene::CommandName(CombatCommand command) {
    switch (command) {
    case CombatCommand::Light:
        return "Light";
    case CombatCommand::Heavy:
        return "Heavy";
    default:
        return "Unknown";
    }
}

const char* GameScene::StyleName(CombatStyle style) {
    switch (style) {
    case CombatStyle::Single:
        return "Single";
    case CombatStyle::Crowd:
        return "Crowd";
    default:
        return "Unknown";
    }
}

Material GameScene::MakeMaterial(float r, float g, float b, float a) {
    Material material{};
    material.color = {r, g, b, a};
    material.enableTexture = 0;
    material.roughness = 0.72f;
    material.reflectionStrength = 0.0f;
    material.blendMode =
        static_cast<int32_t>(a < 1.0f ? BlendMode::Transparent : BlendMode::Opaque);
    material.depthWrite = a < 1.0f ? 0 : 1;
    material.cullMode = static_cast<int32_t>(MaterialCullMode::Back);
    return material;
}

Transform GameScene::MakeActorTransform(const CombatActor& actor, float height,
                                        float widthScale) {
    (void)height;
    Transform transform{};
    transform.position = {actor.position.x, 0.0f, actor.position.z};
    transform.scale = {widthScale, 1.0f, widthScale};

    const DirectX::XMVECTOR rotation =
        DirectX::XMQuaternionRotationRollPitchYaw(0.0f, FacingYaw(actor.facing), 0.0f);
    DirectX::XMStoreFloat4(&transform.rotation, rotation);
    return transform;
}

Transform GameScene::MakeFloorTransform() {
    Transform transform{};
    transform.position = {0.0f, 0.0f, 2.0f};
    transform.scale = {12.0f, 12.0f, 1.0f};

    const DirectX::XMVECTOR rotation =
        DirectX::XMQuaternionRotationRollPitchYaw(kPi * 0.5f, 0.0f, 0.0f);
    DirectX::XMStoreFloat4(&transform.rotation, rotation);
    return transform;
}

Transform GameScene::MakeAttackRangeTransform(const CombatActor& actor,
                                              const AttackData& attack) {
    const Vec2 origin = AttackOrigin(actor);
    const Vec2 facing = AttackFacing(actor);
    Transform transform{};
    transform.position = {origin.x + facing.x * (attack.range * 0.5f),
                          0.03f,
                          origin.z + facing.z * (attack.range * 0.5f)};
    transform.scale = {attack.halfWidth * 2.0f, 1.0f, attack.range};

    const DirectX::XMVECTOR rotation =
        DirectX::XMQuaternionRotationRollPitchYaw(0.0f, FacingYaw(facing), 0.0f);
    DirectX::XMStoreFloat4(&transform.rotation, rotation);
    return transform;
}

float GameScene::FacingYaw(const Vec2& facing) {
    return std::atan2(facing.x, facing.z);
}
