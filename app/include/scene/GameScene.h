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
        Heavy,
    };

    enum class MoveId {
        None,
        L1,
        L2,
        L3,
        L4,
        F1,
        F2,
        F3,
        F4,
        SwayAttack,
        CounterAttack,
        DownAttack,
        ExAction,
        EnemyPoke,
        EnemyHeavy,
    };

    enum class CombatState {
        Idle,
        Attack,
        Guard,
        GuardStun,
        Dodge,
        HitStun,
        Down,
    };

    enum class CombatStyle {
        Single,
        Crowd,
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
        MoveId id = MoveId::L1;
        const char* name = "L1";
        int startup = 12;
        int active = 3;
        int recovery = 13;
        int total = 28;
        int cancelFrom = 15;
        int cancelTo = 22;
        MoveId lightChain = MoveId::None;
        MoveId heavyChain = MoveId::None;
        float range = 1.1f;
        float halfWidth = 0.35f;
        int damage = 10;
        int hitstun = 14;
        int blockstun = 9;
        int hitstop = 5;
    };

    struct DodgeData {
        int total = 22;
        int invulFrom = 5;
        int invulTo = 13;
        float distance = 0.8f;
    };

    struct CombatActor {
        const char* name = "";
        Vec2 position{};
        Vec2 facing{0.0f, 1.0f};
        Vec2 attackOrigin{};
        Vec2 attackFacing{0.0f, 1.0f};
        Vec2 dodgeDirection{0.0f, -1.0f};
        CombatState state = CombatState::Idle;
        MoveId currentMove = MoveId::None;
        int frameInState = 0;
        int hp = 100;
        int hitstunFrames = 0;
        int guardStunFrames = 0;
        int downFrames = 0;
        int aiCooldownFrames = 0;
        int aiAttackCount = 0;
        int dodgeChainCount = 0;
        uint64_t attackSerial = 0;
        uint64_t lastHitAttackSerial = 0;
        bool hitApplied = false;

        bool IsAlive() const;
    };

    struct CombatDebugState {
        bool lastHitboxActive = false;
        bool lastHitConnected = false;
        bool lastBlocked = false;
        bool lastDodged = false;
        float lastDistance = 0.0f;
        const char* lastDefense = "None";
        int fixedStepsThisFrame = 0;
        uint64_t combatFrame = 0;
    };

    void ResetPhaseOne();
    void InitializeVisuals();
    void CaptureFrameInput();
    void StepCombat();
    void UpdateCombatCamera();
    void UpdateFeedbackTimers();
    void UpdatePlayerIdle();
    void UpdatePlayerAttack();
    void UpdatePlayerGuard();
    void UpdateGuardStun(CombatActor& actor);
    void UpdateDodge(CombatActor& actor);
    void UpdateHitStun(CombatActor& actor);
    void UpdateDown(CombatActor& actor);
    void UpdateEnemyActor(CombatActor& actor);
    void UpdateEnemyTraining(CombatActor& actor);
    void StartAttack(CombatActor& actor, MoveId move);
    void StartGuard(CombatActor& actor);
    void StartDodge(CombatActor& actor);
    bool TryStartCounter(CombatActor& attacker);
    bool TryStartDownAttack();
    bool TryStartExAction();
    bool TryActivateExBoost();
    bool IsExBoostActive() const;
    bool TryToggleCombatStyle();
    bool TryChainPlayerAttack(const AttackData& attack);
    bool TryCancelPlayerAttackToDodge(const AttackData& attack);
    void TryResolveAttackHit(CombatActor& attacker, CombatActor& defender);
    void ApplyHit(CombatActor& attacker, CombatActor& defender, const AttackData& attack);
    void ApplyBlock(CombatActor& attacker, CombatActor& defender, const AttackData& attack);
    void AddHitFeedback(const CombatActor& attacker, const AttackData& attack);
    void AddBlockFeedback(const CombatActor& attacker);
    void StartCameraShake(int frames, float magnitude);
    void FaceActorToward(CombatActor& actor, const CombatActor& target);
    bool IsEnemyActor(const CombatActor& actor) const;
    CombatActor& TargetEnemy();
    const CombatActor& TargetEnemy() const;
    CombatActor& EnemyAt(size_t index);
    const CombatActor& EnemyAt(size_t index) const;
    void CycleLockOnTarget(int direction);
    Vec2 ReadMovementInput() const;
    bool IsGuardHeld() const;
    bool IsDodgeRequested() const;
    bool IsAnyEnemyAttacking() const;
    AttackData MakeEffectiveAttackData(const CombatActor& attacker,
                                       const AttackData& attack) const;
    static bool IsDodgeInvulnerable(const CombatActor& actor);
    static bool IsFacingIncomingAttack(const CombatActor& defender,
                                       const CombatActor& attacker);
    static bool IsKnockdownAttack(MoveId move);
    static const AttackData& GetAttackData(MoveId move);
    static const DodgeData& GetDodgeData();
    static MoveId ResolveChainMove(const AttackData& attack, CombatCommand command);
    static float Distance(const Vec2& a, const Vec2& b);
    static float Dot(const Vec2& a, const Vec2& b);
    static Vec2 Normalize(Vec2 value);
    static bool HasDirection(Vec2 value);
    static Vec2 NormalizeOr(Vec2 value, Vec2 fallback);
    static Vec2 AttackOrigin(const CombatActor& actor);
    static Vec2 AttackFacing(const CombatActor& actor);
    static const char* StateName(CombatState state);
    static const char* CommandName(CombatCommand command);
    static const char* StyleName(CombatStyle style);
    static Material MakeMaterial(float r, float g, float b, float a = 1.0f);
    static Transform MakeActorTransform(const CombatActor& actor, float height,
                                        float widthScale = 1.0f);
    static Transform MakeFloorTransform();
    static Transform MakeAttackRangeTransform(const CombatActor& actor,
                                              const AttackData& attack);
    static float FacingYaw(const Vec2& facing);

    float elapsedSeconds_ = 0.0f;
    float combatAccumulator_ = 0.0f;
    float exGauge_ = 0.0f;
    float cameraShakeMagnitude_ = 0.0f;
    int hitstopFrames_ = 0;
    int enemyTrainingCooldown_ = 45;
    int comboCount_ = 0;
    int comboTimerFrames_ = 0;
    int cameraShakeFrames_ = 0;
    int exBoostFrames_ = 0;
    bool exBoostRequested_ = false;
    bool styleSwitchRequested_ = false;
    CombatStyle combatStyle_ = CombatStyle::Single;
    uint64_t nextAttackSerial_ = 0;
    size_t targetEnemyIndex_ = 0;

    InputBuffer inputBuffer_{};
    CombatActor player_{};
    CombatActor enemy_{};
    std::array<CombatActor, 2> supportEnemies_{};
    CombatDebugState debug_{};

    Camera combatCamera_{};
    ModelHandle playerModel_{};
    ModelHandle enemyModel_{};
    ModelHandle floorModel_{};
    ModelHandle attackRangeModel_{};
    ModelHandle guardMarkerModel_{};
};
