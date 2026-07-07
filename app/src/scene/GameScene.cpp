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
constexpr float kFixedCombatDt = 1.0f / 60.0f;
constexpr int kInputBufferFrames = 8;
constexpr float kPlayerMoveSpeed = 2.4f;
constexpr int kMaxFixedStepsPerFrame = 5;
constexpr int kEnemyTrainingAttackCooldown = 90;
constexpr int kComboTimerFrames = 90;
constexpr float kMaxExGauge = 100.0f;
constexpr float kPi = 3.14159265358979323846f;
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
    models.Draw(floorModel_, MakeFloorTransform(), combatCamera_);
    models.Draw(playerModel_, MakeActorTransform(player_, 1.7f), combatCamera_);
    models.Draw(enemyModel_, MakeActorTransform(enemy_, 1.55f, 0.9f), combatCamera_);

    if (player_.state == CombatState::Attack) {
        models.Draw(attackRangeModel_,
                    MakeAttackRangeTransform(player_, GetAttackData(player_.currentMove)),
                    combatCamera_);
    }

    if (enemy_.state == CombatState::Attack) {
        models.Draw(attackRangeModel_,
                    MakeAttackRangeTransform(enemy_, GetAttackData(enemy_.currentMove)),
                    combatCamera_);
    }

    if (player_.state == CombatState::Guard || player_.state == CombatState::GuardStun) {
        Transform guard = MakeActorTransform(player_, 0.25f, 1.25f);
        guard.position.y = 1.85f;
        models.Draw(guardMarkerModel_, guard, combatCamera_);
    }
    models.PostDraw();
}

void GameScene::DrawPostProcessOverlay() {
#ifdef _DEBUG
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Combat Phase 4")) {
        ImGui::Text(
            "Controls: WASD move / J/X light / K/Y heavy / L or LB guard / Space or B sway / R reset");
        ImGui::Separator();
        ImGui::Text("Combat frame: %llu",
                    static_cast<unsigned long long>(debug_.combatFrame));
        ImGui::Text("Fixed steps this render frame: %d", debug_.fixedStepsThisFrame);
        ImGui::Text("Hitstop frames: %d", hitstopFrames_);
        ImGui::Text("Combo: %d  timer=%d", comboCount_, comboTimerFrames_);
        ImGui::Text("Camera shake frames: %d", cameraShakeFrames_);
        ImGui::Separator();
        ImGui::Text("Player: %s  move=%s  frame=%d  hp=%d", StateName(player_.state),
                    GetAttackData(player_.currentMove).name, player_.frameInState,
                    player_.hp);
        ImGui::Text("Enemy : %s  move=%s  frame=%d  hp=%d", StateName(enemy_.state),
                    GetAttackData(enemy_.currentMove).name,
                    enemy_.frameInState, enemy_.hp);
        ImGui::Text("Distance: %.2f", debug_.lastDistance);
        ImGui::Text("Hitbox active: %s", debug_.lastHitboxActive ? "yes" : "no");
        ImGui::Text("Last hit connected: %s", debug_.lastHitConnected ? "yes" : "no");
        ImGui::Text("Last defense: %s", debug_.lastDefense);
        ImGui::Text("Blocked: %s  Dodged: %s", debug_.lastBlocked ? "yes" : "no",
                    debug_.lastDodged ? "yes" : "no");
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

void GameScene::InputBuffer::Push(CombatCommand command, int bufferFrames) {
    for (BufferedInput& entry : entries) {
        if (!entry.active) {
            entry.command = command;
            entry.framesLeft = bufferFrames;
            entry.active = true;
            return;
        }
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
    for (BufferedInput& entry : entries) {
        if (entry.active && entry.command == command) {
            entry.active = false;
            return true;
        }
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
    cameraShakeMagnitude_ = 0.0f;
    hitstopFrames_ = 0;
    enemyTrainingCooldown_ = 45;
    comboCount_ = 0;
    comboTimerFrames_ = 0;
    cameraShakeFrames_ = 0;
    inputBuffer_.Clear();
    debug_ = {};

    player_ = {};
    player_.name = "Player";
    player_.position = {0.0f, 0.0f};
    player_.facing = {0.0f, 1.0f};
    player_.hp = 100;

    enemy_ = {};
    enemy_.name = "Enemy";
    enemy_.position = {0.0f, 1.0f};
    enemy_.facing = {0.0f, -1.0f};
    enemy_.attackFacing = enemy_.facing;
    enemy_.hp = 100;

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
                                          MakeMaterial(0.18f, 0.45f, 1.0f), 0.55f,
                                          1.7f, 0.45f);
    enemyModel_ = models.CreateBoxHandle(kInvalidResourceId,
                                         MakeMaterial(1.0f, 0.28f, 0.18f), 0.55f,
                                         1.55f, 0.45f);
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

    if (input.IsKeyTrigger(DIK_J) ||
        input.IsGamepadButtonTrigger(static_cast<WORD>(XINPUT_GAMEPAD_X))) {
        inputBuffer_.Push(CombatCommand::Light, kInputBufferFrames);
    }

    if (input.IsKeyTrigger(DIK_K) ||
        input.IsGamepadButtonTrigger(static_cast<WORD>(XINPUT_GAMEPAD_Y))) {
        inputBuffer_.Push(CombatCommand::Heavy, kInputBufferFrames);
    }
}

void GameScene::StepCombat() {
    ++debug_.combatFrame;
    debug_.lastHitboxActive = false;
    debug_.lastHitConnected = false;
    debug_.lastBlocked = false;
    debug_.lastDodged = false;
    debug_.lastDefense = "None";
    debug_.lastDistance = Distance(player_.position, enemy_.position);
    UpdateFeedbackTimers();

    if (hitstopFrames_ > 0) {
        --hitstopFrames_;
        UpdateCombatCamera();
        return;
    }

    FaceActorToward(player_, enemy_);
    FaceActorToward(enemy_, player_);

    UpdateEnemyTraining();

    if (enemy_.state == CombatState::Attack) {
        ++enemy_.frameInState;
        TryResolveAttackHit(enemy_, player_);
        const AttackData& attack = GetAttackData(enemy_.currentMove);
        if (enemy_.frameInState >= attack.total) {
            enemy_.state = CombatState::Idle;
            enemy_.currentMove = MoveId::None;
            enemy_.frameInState = 0;
            enemy_.hitApplied = false;
            enemyTrainingCooldown_ = kEnemyTrainingAttackCooldown;
        }
    } else if (enemy_.state == CombatState::HitStun) {
        UpdateHitStun(enemy_);
    }

    if (player_.state == CombatState::HitStun) {
        UpdateHitStun(player_);
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

    inputBuffer_.Tick();
    UpdateCombatCamera();
}

void GameScene::UpdateCombatCamera() {
    const Vec2 center{(player_.position.x + enemy_.position.x) * 0.5f,
                      (player_.position.z + enemy_.position.z) * 0.5f};
    const float distance = Distance(player_.position, enemy_.position);
    const float cameraDistance = (std::max)(6.5f, distance + 5.0f);
    combatCamera_.SetPosition({center.x, 4.4f, center.z - cameraDistance});
    if (cameraShakeFrames_ > 0) {
        const float phase = static_cast<float>(debug_.combatFrame);
        const float xShake = std::sin(phase * 1.71f) * cameraShakeMagnitude_;
        const float yShake = std::cos(phase * 2.17f) * cameraShakeMagnitude_ * 0.65f;
        combatCamera_.SetPosition({center.x + xShake, 4.4f + yShake,
                                   center.z - cameraDistance});
    }
    combatCamera_.SetRotation({0.55f, 0.0f, 0.0f});
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
}

void GameScene::UpdatePlayerIdle() {
    const Vec2 move = ReadMovementInput();
    player_.position.x += move.x * kPlayerMoveSpeed * kFixedCombatDt;
    player_.position.z += move.z * kPlayerMoveSpeed * kFixedCombatDt;
    FaceActorToward(player_, enemy_);

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
    TryResolveAttackHit(player_, enemy_);

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
    const float stepDistance = dodge.distance / static_cast<float>(dodge.total);
    actor.position.x += actor.dodgeDirection.x * stepDistance;
    actor.position.z += actor.dodgeDirection.z * stepDistance;

    if (&actor == &player_ && actor.frameInState >= dodge.invulTo + 1 &&
        inputBuffer_.Consume(CombatCommand::Heavy)) {
        StartAttack(player_, MoveId::SwayAttack);
        return;
    }

    if (actor.frameInState >= dodge.total) {
        actor.state = CombatState::Idle;
        actor.frameInState = 0;
    }
}

void GameScene::UpdateHitStun(CombatActor& actor) {
    ++actor.frameInState;
    if (actor.frameInState >= actor.hitstunFrames) {
        actor.state = actor.IsAlive() ? CombatState::Idle : CombatState::Down;
        actor.currentMove = MoveId::None;
        actor.frameInState = 0;
        actor.hitstunFrames = 0;
    }
}

void GameScene::UpdateEnemyTraining() {
    if (!enemy_.IsAlive() || enemy_.state != CombatState::Idle) {
        return;
    }

    if (enemyTrainingCooldown_ > 0) {
        --enemyTrainingCooldown_;
        return;
    }

    if (Distance(enemy_.position, player_.position) <= GetAttackData(MoveId::EnemyPoke).range) {
        StartAttack(enemy_, MoveId::EnemyPoke);
    }
}

void GameScene::StartAttack(CombatActor& actor, MoveId move) {
    actor.state = CombatState::Attack;
    actor.currentMove = move;
    actor.frameInState = 0;
    actor.hitApplied = false;
    actor.attackOrigin = actor.position;
    actor.attackFacing = NormalizeOr(actor.facing, {0.0f, 1.0f});
}

void GameScene::StartGuard(CombatActor& actor) {
    actor.state = CombatState::Guard;
    actor.currentMove = MoveId::None;
    actor.frameInState = 0;
}

void GameScene::StartDodge(CombatActor& actor) {
    actor.state = CombatState::Dodge;
    actor.currentMove = MoveId::None;
    actor.frameInState = 0;
    actor.hitApplied = false;
    const Vec2 inputDirection = &actor == &player_ ? ReadMovementInput() : Vec2{};
    actor.dodgeDirection =
        NormalizeOr(inputDirection, Vec2{-actor.facing.x, -actor.facing.z});
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

        if (inputBuffer_.Consume(command)) {
            StartAttack(player_, next);
            return true;
        }
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
    return true;
}

void GameScene::TryResolveAttackHit(CombatActor& attacker, CombatActor& defender) {
    const int frame = attacker.frameInState;
    const AttackData& attack = GetAttackData(attacker.currentMove);
    const bool active = frame >= attack.startup && frame < attack.startup + attack.active;
    debug_.lastHitboxActive = active;
    debug_.lastDistance = Distance(player_.position, enemy_.position);

    if (!active || attacker.hitApplied || !defender.IsAlive()) {
        return;
    }

    const Vec2 origin = AttackOrigin(attacker);
    const Vec2 facing = AttackFacing(attacker);
    const Vec2 toDefender{defender.position.x - origin.x,
                          defender.position.z - origin.z};
    const float forwardDistance = Dot(toDefender, facing);
    const Vec2 right{facing.z, -facing.x};
    const float sideDistance = std::abs(Dot(toDefender, right));

    if (forwardDistance >= 0.0f && forwardDistance <= attack.range &&
        sideDistance <= attack.halfWidth) {
        if (IsDodgeInvulnerable(defender)) {
            attacker.hitApplied = true;
            debug_.lastDodged = true;
            debug_.lastDefense = "Sway invulnerable";
            return;
        }

        if (defender.state == CombatState::Guard &&
            IsFacingIncomingAttack(defender, attacker)) {
            ApplyBlock(attacker, defender, attack);
            return;
        }

        ApplyHit(attacker, defender, attack);
    }
}

void GameScene::ApplyHit(CombatActor& attacker, CombatActor& defender,
                         const AttackData& attack) {
    attacker.hitApplied = true;
    defender.hp = (std::max)(0, defender.hp - attack.damage);
    defender.state = defender.IsAlive() ? CombatState::HitStun : CombatState::Down;
    defender.currentMove = MoveId::None;
    defender.frameInState = 0;
    defender.hitstunFrames = attack.hitstun;
    hitstopFrames_ = attack.hitstop;
    debug_.lastHitConnected = true;
    AddHitFeedback(attacker, attack);
}

void GameScene::ApplyBlock(CombatActor& attacker, CombatActor& defender,
                           const AttackData& attack) {
    attacker.hitApplied = true;
    defender.state = CombatState::GuardStun;
    defender.currentMove = MoveId::None;
    defender.frameInState = 0;
    defender.guardStunFrames = attack.blockstun;
    hitstopFrames_ = 3;
    debug_.lastBlocked = true;
    debug_.lastDefense = "Guard";
    AddBlockFeedback(attacker);
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

    const bool finisher = attack.id == MoveId::F1 || attack.id == MoveId::F2 ||
                          attack.id == MoveId::F3 || attack.id == MoveId::F4;
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

GameScene::Vec2 GameScene::ReadMovementInput() const {
    if (!ctx_ || !ctx_->systems.input) {
        return {};
    }

    const Input& input = *ctx_->systems.input;
    Vec2 move{};
    if (input.IsKeyPress(DIK_A)) {
        move.x -= 1.0f;
    }
    if (input.IsKeyPress(DIK_D)) {
        move.x += 1.0f;
    }
    if (input.IsKeyPress(DIK_W)) {
        move.z += 1.0f;
    }
    if (input.IsKeyPress(DIK_S)) {
        move.z -= 1.0f;
    }

    move.x += input.GetGamepadLeftStickX();
    move.z += input.GetGamepadLeftStickY();
    return Normalize(move);
}

bool GameScene::IsGuardHeld() const {
    if (!ctx_ || !ctx_->systems.input) {
        return false;
    }

    const Input& input = *ctx_->systems.input;
    return input.IsKeyPress(DIK_L) ||
           input.IsGamepadButtonPress(static_cast<WORD>(XINPUT_GAMEPAD_LEFT_SHOULDER));
}

bool GameScene::IsDodgeRequested() const {
    if (!ctx_ || !ctx_->systems.input) {
        return false;
    }

    const Input& input = *ctx_->systems.input;
    return input.IsKeyTrigger(DIK_SPACE) ||
           input.IsGamepadButtonTrigger(static_cast<WORD>(XINPUT_GAMEPAD_B));
}

bool GameScene::IsDodgeInvulnerable(const CombatActor& actor) {
    if (actor.state != CombatState::Dodge) {
        return false;
    }

    const DodgeData& dodge = GetDodgeData();
    return actor.frameInState >= dodge.invulFrom && actor.frameInState <= dodge.invulTo;
}

bool GameScene::IsFacingIncomingAttack(const CombatActor& defender,
                                       const CombatActor& attacker) {
    const Vec2 toAttacker =
        Normalize(Vec2{AttackOrigin(attacker).x - defender.position.x,
                       AttackOrigin(attacker).z - defender.position.z});
    return Dot(defender.facing, toAttacker) >= 0.25f;
}

const GameScene::AttackData& GameScene::GetAttackData(MoveId move) {
    static constexpr AttackData kNone{MoveId::None, "None", 0, 0, 0, 1, 0, 0,
                                      MoveId::None, MoveId::None, 0.0f, 0.0f,
                                      0, 0, 0, 0};
    static constexpr std::array<AttackData, 10> kAttacks{{
        {MoveId::L1, "L1", 12, 3, 13, 28, 15, 22, MoveId::L2, MoveId::F1, 1.10f,
         0.35f, 10, 14, 9, 5},
        {MoveId::L2, "L2", 14, 3, 15, 32, 18, 26, MoveId::L3, MoveId::F2, 1.15f,
         0.38f, 11, 15, 10, 5},
        {MoveId::L3, "L3", 16, 4, 18, 38, 22, 30, MoveId::L4, MoveId::F3, 1.20f,
         0.42f, 12, 18, 11, 6},
        {MoveId::L4, "L4", 18, 4, 22, 44, 26, 34, MoveId::None, MoveId::F4,
         1.28f, 0.48f, 14, 20, 13, 6},
        {MoveId::F1, "F1", 18, 4, 24, 46, 46, 46, MoveId::None, MoveId::None,
         1.20f, 0.42f, 18, 22, 16, 8},
        {MoveId::F2, "F2", 20, 5, 26, 51, 51, 51, MoveId::None, MoveId::None,
         1.25f, 0.46f, 21, 24, 18, 8},
        {MoveId::F3, "F3", 22, 6, 28, 56, 56, 56, MoveId::None, MoveId::None,
         1.30f, 0.52f, 24, 26, 19, 9},
        {MoveId::F4, "F4", 24, 8, 30, 62, 62, 62, MoveId::None, MoveId::None,
         1.40f, 0.58f, 30, 30, 22, 10},
        {MoveId::SwayAttack, "SWA", 11, 4, 17, 32, 32, 32, MoveId::None,
         MoveId::None, 1.00f, 0.35f, 13, 18, 12, 7},
        {MoveId::EnemyPoke, "EnemyPoke", 22, 5, 28, 55, 55, 55, MoveId::None,
         MoveId::None, 1.10f, 0.40f, 8, 14, 12, 5},
    }};

    for (const AttackData& attack : kAttacks) {
        if (attack.id == move) {
            return attack;
        }
    }

    return kNone;
}

const GameScene::DodgeData& GameScene::GetDodgeData() {
    static constexpr DodgeData kDodge{20, 4, 10, 0.75f};
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
