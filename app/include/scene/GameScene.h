#pragma once

#include "Engine.h"

#include <array>
#include <cstdint>

class GameScene : public BaseScene {
public:
    /// <summary>
    /// シーンで使用する共有コンテキストを保持し、初期化ログを出力する。
    /// </summary>
    /// <param name="ctx">エンジンから渡されるシーン共有コンテキスト。</param>
    void Initialize(const SceneContext& ctx) override;

    /// <summary>
    /// フレーム時間と入力状態を使ってシーン状態を更新する。
    /// </summary>
    void Update() override;

    /// <summary>
    /// 通常の3D描画パスでシーン内容を描画する。
    /// </summary>
    void Draw() override;

    /// <summary>
    /// ポストエフェクト後のバックバッファへ2Dオーバーレイを描画する。
    /// </summary>
    void DrawPostProcessOverlay() override;

private:
    enum class CombatCommand {
        Light,
    };

    enum class CombatState {
        Idle,
        Attack,
        HitStun,
        Down,
    };

    struct Vec2 {
        float x = 0.0f;
        float z = 0.0f;
    };

    struct BufferedInput {
        CombatCommand command = CombatCommand::Light;
        int framesLeft = 0;
        bool active = false;
    };

    struct InputBuffer {
        static constexpr int kMaxEntries = 8;
        std::array<BufferedInput, kMaxEntries> entries{};

        void Push(CombatCommand command, int bufferFrames);
        void Tick();
        bool Consume(CombatCommand command);
        void Clear();
    };

    struct AttackData {
        const char* id = "L1";
        int startup = 12;
        int active = 3;
        int recovery = 13;
        int total = 28;
        float range = 1.1f;
        float halfWidth = 0.35f;
        int damage = 10;
        int hitstun = 14;
        int hitstop = 5;
    };

    struct CombatActor {
        const char* name = "";
        Vec2 position{};
        Vec2 facing{0.0f, 1.0f};
        CombatState state = CombatState::Idle;
        int frameInState = 0;
        int hp = 100;
        bool hitApplied = false;

        bool IsAlive() const;
    };

    struct CombatDebugState {
        bool lastHitboxActive = false;
        bool lastHitConnected = false;
        float lastDistance = 0.0f;
        int fixedStepsThisFrame = 0;
        uint64_t combatFrame = 0;
    };

    void ResetPhaseOne();
    void CaptureFrameInput();
    void StepCombat();
    void UpdatePlayerIdle();
    void UpdatePlayerAttack();
    void UpdateHitStun(CombatActor& actor);
    void StartAttack(CombatActor& actor, const AttackData& attack);
    void TryResolvePlayerHit();
    void ApplyHit(CombatActor& attacker, CombatActor& defender, const AttackData& attack);
    void FaceActorToward(CombatActor& actor, const CombatActor& target);
    static float Distance(const Vec2& a, const Vec2& b);
    static float Dot(const Vec2& a, const Vec2& b);
    static Vec2 Normalize(Vec2 value);
    static const char* StateName(CombatState state);

    float elapsedSeconds_ = 0.0f;
    float combatAccumulator_ = 0.0f;
    int hitstopFrames_ = 0;

    InputBuffer inputBuffer_{};
    AttackData lightAttack_{};
    CombatActor player_{};
    CombatActor enemy_{};
    CombatDebugState debug_{};
};
