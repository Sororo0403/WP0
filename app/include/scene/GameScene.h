#pragma once

#include "Engine.h"

#include <array>
#include <cstdint>

class Input;

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
        GetUp,
    };

    enum class CombatStyle {
        Single,
        Crowd,
    };

    enum class EnemyIntent {
        Approach,
        HoldRange,
        Retreat,
        Strafe,
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

    struct FrameInput {
        Vec2 movement{};
        bool guardHeld = false;
        bool dodgeRequested = false;
    };

    struct Vec3 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Box3D {
        Vec3 center{};
        Vec3 half{};
        Vec2 facing{0.0f, 1.0f};
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
        float advanceDistance = 0.0f;
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
        Vec2 aiMoveDirection{};
        Vec2 downSlideDirection{};
        Vec2 knockbackDirection{};
        Vec2 downReelTarget{};
        Vec2 pendingKnockdownDirection{};
        Vec2 orbitCenter{};
        Vec2 lastOrbitTarget{};
        float dodgeDistance = 0.8f;
        float orbitRadius = 1.0f;
        float orbitStartAngle = 0.0f;
        float orbitAngleDelta = 0.0f;
        CombatState state = CombatState::Idle;
        MoveId currentMove = MoveId::None;
        int frameInState = 0;
        int hp = 100;
        int hitstunFrames = 0;
        int guardStunFrames = 0;
        int downFrames = 0;
        int downSlideFrames = 0;
        int downReelFrames = 0;
        int aiCooldownFrames = 0;
        int aiIntentFrames = 0;
        int aiAttackCount = 0;
        float aiStrafePhase = 0.0f;
        float aiStrafeSign = 1.0f;
        float knockbackRemaining = 0.0f;
        int dodgeChainCount = 0;
        EnemyIntent aiIntent = EnemyIntent::Approach;
        uint64_t attackSerial = 0;
        uint64_t lastHitAttackSerial = 0;
        bool hitApplied = false;
        bool pendingKnockdown = false;
        bool pendingKnockdownFromPlayer = false;
        bool pendingKnockdownFinisher = false;
        bool orbitDodgeActive = false;
        bool hasLastOrbitTarget = false;
        uint64_t pendingKnockdownAttackSerial = 0;

        bool IsAlive() const;
    };

    struct CombatDebugState {
        bool lastHitboxActive = false;
        bool lastHitConnected = false;
        bool lastBlocked = false;
        bool lastDodged = false;
        float lastDistance = 0.0f;
        float lastKnockback = 0.0f;
        const char* lastDefense = "None";
        int fixedStepsThisFrame = 0;
        uint64_t combatFrame = 0;
    };

    void ResetPhaseOne();
    void InitializeVisuals();
    void DrawActors(ModelManager& models) const;
    void DrawHurtboxes(ModelManager& models) const;
    void DrawActorHurtbox(ModelManager& models, const CombatActor& actor,
                          ModelHandle model) const;
    void DrawAttackRanges(ModelManager& models) const;
    void DrawCombatMarkers(ModelManager& models) const;
    void DrawDebugCombatants() const;
    void DrawDebugStatus() const;
    void DrawDebugHitInfo() const;
    void DrawDebugInputBuffer() const;
    void DrawDebugMeters() const;
    void CaptureFrameInput();
    Vec2 ReadPlayerMovement(const Input& input) const;
    void CaptureActionInputs(const Input& input);
    void StepCombat();
    void ConsumePendingInputs();
    void ApplyRequestedCombatActions();
    void FaceCombatants();
    void UpdateEnemyCombatants();
    void UpdatePlayerCombatState();
    void UpdateCombatCamera();
    void UpdateFeedbackTimers();
    void UpdatePlayerIdle();
    void UpdatePlayerAttack();
    void UpdatePlayerGuard();
    static void UpdateGuardStun(CombatActor& actor);
    void UpdateDodge(CombatActor& actor);
    static void UpdateDodgeMovement(CombatActor& actor, const DodgeData& dodge,
                                    bool moving);
    bool TryChainDodge(const CombatActor& actor, bool moving, bool recovery);
    static void FinishDodgeIfComplete(CombatActor& actor, const DodgeData& dodge);
    void UpdateHitStun(CombatActor& actor);
    void UpdateDown(CombatActor& actor);
    static void UpdateGetUp(CombatActor& actor);
    void UpdateEnemyActor(CombatActor& actor);
    void UpdateEnemyTraining(CombatActor& actor);
    bool TryStartEnemyTrainingAttack(CombatActor& actor, float distance,
                                     bool supportEnemy);
    MoveId ChooseEnemyTrainingAttack(const CombatActor& actor, float distance,
                                     bool supportEnemy) const;
    static Vec2 ResolveEnemyTrainingMovement(CombatActor& actor, float distance,
                                             Vec2 toPlayer, float& speedScale);
    static void BlendEnemyMoveDirection(CombatActor& actor, Vec2 movement);
    void UpdateEnemyTrainingCooldowns(CombatActor& actor);
    void ApplyAttackMovement(CombatActor& actor, const AttackData& attack);
    void ClampCombatantsToFloor();
    static void ClampActorToFloor(CombatActor& actor);
    static bool CanStartAttack(const CombatActor& actor);
    static bool CanStartGuard(const CombatActor& actor);
    static bool CanStartDodge(const CombatActor& actor);
    void StartAttack(CombatActor& actor, MoveId move);
    static void StartGuard(CombatActor& actor);
    void StartDodge(CombatActor& actor);
    void StartOrbitDodge(CombatActor& actor, Vec2 center, float angleDelta,
                         Vec2 fallbackDirection);
    bool TryStartTargetedOrbitDodge(CombatActor& actor, Vec2 inputDirection);
    bool TryContinueOrbitDodge(CombatActor& actor, Vec2 previousCenter,
                               float previousAngleDelta);
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
    void BeginKnockdown(CombatActor& defender, Vec2 fallAway,
                        bool followupKnockdown, bool dramaticKnockdown);
    bool IsPendingKnockdownReady(const CombatActor& actor) const;
    void StartInterpolatedKnockback(CombatActor& defender, Vec2 direction,
                                    float distance);
    static void UpdateInterpolatedKnockback(CombatActor& actor);
    void ApplyKnockback(CombatActor& defender, Vec2 direction, float distance);
    void AddHitFeedback(const CombatActor& attacker, const AttackData& attack);
    void AddBlockFeedback(const CombatActor& attacker);
    void StartCameraShake(int frames, float magnitude);
    static void FaceActorToward(CombatActor& actor, const CombatActor& target);
    static void FaceActorTowardMovement(CombatActor& actor, Vec2 movement);
    bool IsEnemyActor(const CombatActor& actor) const;
    CombatActor& TargetEnemy();
    const CombatActor& TargetEnemy() const;
    CombatActor& EnemyAt(size_t index);
    const CombatActor& EnemyAt(size_t index) const;
    size_t FindNearestEnemyIndex() const;
    const CombatActor* FindOrbitSwayTarget() const;
    void CycleLockOnTarget(int direction);
    Vec2 ReadMovementInput() const;
    bool IsGuardHeld() const;
    bool IsDodgeRequested() const;
    bool IsAnyEnemyAttacking() const;
    AttackData MakeEffectiveAttackData(const CombatActor& attacker,
                                       const AttackData& attack) const;
    static bool IsDodgeInvulnerable(const CombatActor& actor);
    static bool IsAttackHitboxActive(const CombatActor& actor);
    static bool IsDefenderInAttackArea(const CombatActor& defender,
                                       const CombatActor& attacker,
                                       const AttackData& attack);
    static bool Overlaps(const Box3D& a, const Box3D& b);
    static Box3D MakeHurtbox(const CombatActor& actor);
    static Box3D MakeAttackHitbox(const CombatActor& actor, const AttackData& attack);
    static bool IsSingleStyleFinisher(MoveId move);
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
    static Vec2 DownedBodyAxis(const CombatActor& actor);
    static const char* StateName(CombatState state);
    static const char* CommandName(CombatCommand command);
    static const char* StyleName(CombatStyle style);
    static Material MakeMaterial(float r, float g, float b, float a = 1.0f);
    static Transform MakeActorTransform(const CombatActor& actor, float height,
                                        float widthScale = 1.0f);
    static Transform MakeBoxTransform(const Box3D& box);
    static Transform MakeFloorTransform();
    static Transform MakeAttackRangeTransform(const CombatActor& actor,
                                              const AttackData& attack);
    static float FacingYaw(const Vec2& facing);

    float elapsedSeconds_ = 0.0f;
    float combatAccumulator_ = 0.0f;
    float exGauge_ = 0.0f;
    float combatCameraYaw_ = 0.0f;
    float cameraShakeMagnitude_ = 0.0f;
    int hitstopFrames_ = 0;
    int enemyTrainingCooldown_ = 45;
    int comboCount_ = 0;
    int comboTimerFrames_ = 0;
    int cameraShakeFrames_ = 0;
    int exBoostFrames_ = 0;
    bool exBoostRequested_ = false;
    bool styleSwitchRequested_ = false;
    bool pendingLightInput_ = false;
    bool pendingHeavyInput_ = false;
    bool pendingExBoostInput_ = false;
    bool pendingStyleSwitchInput_ = false;
    bool pendingLockCycleInput_ = false;
    bool lockOnActive_ = false;
    CombatStyle combatStyle_ = CombatStyle::Single;
    uint64_t nextAttackSerial_ = 0;
    size_t targetEnemyIndex_ = 0;

    InputBuffer inputBuffer_{};
    FrameInput pendingInput_{};
    FrameInput combatInput_{};
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
    ModelHandle playerHurtboxModel_{};
    ModelHandle enemyHurtboxModel_{};
};
