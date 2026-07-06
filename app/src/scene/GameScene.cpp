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
} // namespace

void GameScene::Initialize(const SceneContext& ctx) {
    BaseScene::Initialize(ctx);
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

void GameScene::Draw() {}

void GameScene::DrawPostProcessOverlay() {
#ifdef _DEBUG
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Combat Phase 3")) {
        ImGui::Text(
            "Controls: WASD move / J/X light / K/Y heavy / L or LB guard / Space or B sway / R reset");
        ImGui::Separator();
        ImGui::Text("Combat frame: %llu",
                    static_cast<unsigned long long>(debug_.combatFrame));
        ImGui::Text("Fixed steps this render frame: %d", debug_.fixedStepsThisFrame);
        ImGui::Text("Hitstop frames: %d", hitstopFrames_);
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
        ImGui::ProgressBar(playerHp, ImVec2(-1.0f, 0.0f), "Player HP");
        ImGui::ProgressBar(enemyHp, ImVec2(-1.0f, 0.0f), "Enemy HP");

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
    hitstopFrames_ = 0;
    enemyTrainingCooldown_ = 45;
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
    enemy_.hp = 100;
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

    if (hitstopFrames_ > 0) {
        --hitstopFrames_;
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
}

void GameScene::UpdatePlayerIdle() {
    if (!ctx_ || !ctx_->systems.input) {
        return;
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
    move = Normalize(move);
    player_.position.x += move.x * kPlayerMoveSpeed * kFixedCombatDt;
    player_.position.z += move.z * kPlayerMoveSpeed * kFixedCombatDt;

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
    actor.position.x -= actor.facing.x * stepDistance;
    actor.position.z -= actor.facing.z * stepDistance;

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

    const Vec2 toDefender{defender.position.x - attacker.position.x,
                          defender.position.z - attacker.position.z};
    const float forwardDistance = Dot(toDefender, attacker.facing);
    const Vec2 right{attacker.facing.z, -attacker.facing.x};
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
}

void GameScene::FaceActorToward(CombatActor& actor, const CombatActor& target) {
    actor.facing = Normalize(
        Vec2{target.position.x - actor.position.x, target.position.z - actor.position.z});
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
        Normalize(Vec2{attacker.position.x - defender.position.x,
                       attacker.position.z - defender.position.z});
    return Dot(defender.facing, toAttacker) >= 0.25f;
}

const GameScene::AttackData& GameScene::GetAttackData(MoveId move) {
    static constexpr AttackData kNone{MoveId::None, "None", 0, 0, 0, 1, 0, 0,
                                      MoveId::None, MoveId::None, 0.0f, 0.0f,
                                      0, 0, 0, 0};
    static constexpr std::array<AttackData, 9> kAttacks{{
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
    static constexpr DodgeData kDodge{};
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
