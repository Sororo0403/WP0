#include "AssetImportPlanner.h"
#include "ProjectDescriptor.h"
#include "PhysicsSettingsStore.h"
#include "ProjectScriptLibrary.h"
#include "ScriptBuildService.h"
#include "RecentScenesStore.h"
#include "ScriptAsset.h"
#include "collision/CollisionUtil.h"
#include "animation/Animator.h"
#include "core/AssetManager.h"
#include "core/MathUtils.h"
#include "graphics/Lighting.h"
#include "input/Input.h"
#include "runtime/BehaviorRegistry.h"
#include "runtime/BehaviorSystem.h"
#include "runtime/TriggerSystem.h"
#include "world/World.h"
#include "world/WorldCollision.h"
#include "world/WorldSerializer.h"
#include "../../engine/src/model/internal/ModelPrimitiveFactory.h"

#include <filesystem>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
class LifecycleBehavior final : public Behavior {
public:
    LifecycleBehavior(int& startCount, int& updateCount, int& stopCount, float& lastDeltaTime)
        : startCount_(startCount), updateCount_(updateCount), stopCount_(stopCount),
          lastDeltaTime_(lastDeltaTime) {}

    void OnStart(World& world, EntityId entity) override {
        ++startCount_;
        if (WorldEntity* target = world.Find(entity)) {
            target->transform.position.x += 1.0f;
        }
    }

    void OnUpdate(World& world, EntityId entity, float deltaTime) override {
        ++updateCount_;
        lastDeltaTime_ = deltaTime;
        if (WorldEntity* target = world.Find(entity)) {
            target->transform.position.y += deltaTime;
        }
    }

    void OnStop(World& world, EntityId entity) override {
        (void)world;
        (void)entity;
        ++stopCount_;
    }

private:
    int& startCount_;
    int& updateCount_;
    int& stopCount_;
    float& lastDeltaTime_;
};

class OrderedBehavior final : public Behavior {
public:
    OrderedBehavior(int id, std::vector<int>& events) : id_(id), events_(events) {}

    void OnStart(World&, EntityId) override {
        events_.push_back(id_ * 10 + 1);
    }
    void OnUpdate(World&, EntityId, float) override {
        events_.push_back(id_ * 10 + 2);
    }
    void OnStop(World&, EntityId) override {
        events_.push_back(id_ * 10 + 3);
    }

private:
    int id_ = 0;
    std::vector<int>& events_;
};

struct TriggerEventCounts {
    int enter = 0;
    int stay = 0;
    int exit = 0;
    EntityId lastOther{};
};

class TriggerBehavior final : public Behavior {
public:
    explicit TriggerBehavior(TriggerEventCounts& counts) : counts_(counts) {}

    void OnTriggerEnter(World&, EntityId, EntityId other) override {
        ++counts_.enter;
        counts_.lastOther = other;
    }

    void OnTriggerStay(World&, EntityId, EntityId other) override {
        ++counts_.stay;
        counts_.lastOther = other;
    }

    void OnTriggerExit(World&, EntityId, EntityId other) override {
        ++counts_.exit;
        counts_.lastOther = other;
    }

private:
    TriggerEventCounts& counts_;
};

class ConfigurableBehavior final : public Behavior {
public:
    ConfigurableBehavior(float& speed, EntityId& target, bool& enabled, int32_t& count,
                         ScriptVector3& offset, std::string& state)
        : speed_(speed), target_(target), enabled_(enabled), count_(count),
          offset_(offset), state_(state) {}

    void OnConfigure(const ScriptPropertyValueView* properties, size_t count) override {
        if (const ScriptPropertyValueView* speed = FindScriptProperty(
                properties, count, "Speed", ScriptPropertyType::Float)) {
            speed_ = speed->floatValue;
        }
        if (const ScriptPropertyValueView* target = FindScriptProperty(
                properties, count, "Target", ScriptPropertyType::Entity)) {
            target_ = target->entityValue;
        }
        if (const ScriptPropertyValueView* enabled = FindScriptProperty(
                properties, count, "Aggressive", ScriptPropertyType::Boolean)) {
            enabled_ = enabled->booleanValue;
        }
        if (const ScriptPropertyValueView* value = FindScriptProperty(
                properties, count, "Lives", ScriptPropertyType::Integer)) {
            count_ = value->integerValue;
        }
        if (const ScriptPropertyValueView* offset = FindScriptProperty(
                properties, count, "Offset", ScriptPropertyType::Vector3)) {
            offset_ = offset->vector3Value;
        }
        if (const ScriptPropertyValueView* state = FindScriptProperty(
                properties, count, "State", ScriptPropertyType::String)) {
            state_ = state->stringValue != nullptr ? state->stringValue : "";
        }
    }

private:
    float& speed_;
    EntityId& target_;
    bool& enabled_;
    int32_t& count_;
    ScriptVector3& offset_;
    std::string& state_;
};

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool IsVerticallyCentered(const ModelPrimitiveFactory::PrimitiveMeshData& primitive) {
    float minimum = (std::numeric_limits<float>::max)();
    float maximum = (std::numeric_limits<float>::lowest)();
    for (const ModelVertex& vertex : primitive.vertices) {
        minimum = (std::min)(minimum, vertex.position.y);
        maximum = (std::max)(maximum, vertex.position.y);
    }
    return !primitive.vertices.empty() && std::abs(minimum + maximum) < 0.0001f;
}

const ScriptPropertyValue* FindStoredScriptProperty(const BehaviorComponent& component,
                                                    std::string_view name) {
    const auto found = std::ranges::find(component.properties, name,
                                         &ScriptPropertyValue::name);
    return found == component.properties.end() ? nullptr : &*found;
}

bool HasOutwardWinding(const ModelPrimitiveFactory::PrimitiveMeshData& primitive) {
    if (primitive.indices.empty() || primitive.indices.size() % 3u != 0u) {
        return false;
    }
    for (size_t index = 0; index < primitive.indices.size(); index += 3u) {
        const ModelVertex& a = primitive.vertices[primitive.indices[index]];
        const ModelVertex& b = primitive.vertices[primitive.indices[index + 1u]];
        const ModelVertex& c = primitive.vertices[primitive.indices[index + 2u]];
        const DirectX::XMFLOAT3 ab{b.position.x - a.position.x, b.position.y - a.position.y,
                                  b.position.z - a.position.z};
        const DirectX::XMFLOAT3 ac{c.position.x - a.position.x, c.position.y - a.position.y,
                                  c.position.z - a.position.z};
        const DirectX::XMFLOAT3 cross{ab.y * ac.z - ab.z * ac.y,
                                     ab.z * ac.x - ab.x * ac.z,
                                     ab.x * ac.y - ab.y * ac.x};
        const DirectX::XMFLOAT3 normal{a.normal.x + b.normal.x + c.normal.x,
                                      a.normal.y + b.normal.y + c.normal.y,
                                      a.normal.z + b.normal.z + c.normal.z};
        const float areaSquared = cross.x * cross.x + cross.y * cross.y + cross.z * cross.z;
        const float facing = cross.x * normal.x + cross.y * normal.y + cross.z * normal.z;
        if (areaSquared > 0.00000001f && facing <= 0.0f) {
            return false;
        }
    }
    return true;
}

bool RotationRoundTrips(const DirectX::XMFLOAT3& degrees) {
    using namespace DirectX;
    const XMVECTOR quaternion = XMQuaternionRotationRollPitchYaw(
        XMConvertToRadians(degrees.x), XMConvertToRadians(degrees.y),
        XMConvertToRadians(degrees.z));
    const XMFLOAT3 restored = MathUtils::RotationDegreesFromQuaternion(quaternion, degrees);
    XMFLOAT4X4 expected{};
    XMFLOAT4X4 actual{};
    XMStoreFloat4x4(&expected, XMMatrixRotationRollPitchYaw(
                                   XMConvertToRadians(degrees.x),
                                   XMConvertToRadians(degrees.y),
                                   XMConvertToRadians(degrees.z)));
    XMStoreFloat4x4(&actual, XMMatrixRotationRollPitchYaw(
                                 XMConvertToRadians(restored.x),
                                 XMConvertToRadians(restored.y),
                                 XMConvertToRadians(restored.z)));
    const float* expectedValues = &expected._11;
    const float* actualValues = &actual._11;
    for (size_t index = 0; index < 16u; ++index) {
        if (std::abs(expectedValues[index] - actualValues[index]) > 0.0001f) {
            return false;
        }
    }
    return true;
}
} // namespace

int main() {
    const SceneLighting unlitScene{};
    if (!Check(unlitScene.keyLightColor.x == 0.0f &&
                   unlitScene.keyLightColor.y == 0.0f &&
                   unlitScene.keyLightColor.z == 0.0f &&
                   unlitScene.fillLightColor.x == 0.0f &&
                   unlitScene.fillLightColor.y == 0.0f &&
                   unlitScene.fillLightColor.z == 0.0f &&
                   unlitScene.ambientColor.x == 0.0f &&
                   unlitScene.ambientColor.y == 0.0f &&
                   unlitScene.ambientColor.z == 0.0f &&
                   unlitScene.ambientColor.w == 0.0f &&
                   unlitScene.pointLights[0].colorIntensity.w == 0.0f &&
                   unlitScene.pointLights[1].colorIntensity.w == 0.0f &&
                   unlitScene.spotLight.colorIntensity.w == 0.0f,
               "Default SceneLighting is not unlit.")) {
        return 141;
    }
    Input actionInput;
    const InputActionBinding* moveHorizontal =
        actionInput.GetActionBinding("MoveHorizontal");
    const InputActionBinding* jump = actionInput.GetActionBinding("Jump");
    if (!Check(actionInput.GetActionNames() ==
                       std::vector<std::string>(
                           {"MoveHorizontal", "MoveVertical", "Sprint", "Jump"}) &&
                   moveHorizontal != nullptr &&
                   moveHorizontal->negativeKey == DIK_A &&
                   moveHorizontal->positiveKeys[0] == DIK_D &&
                   moveHorizontal->gamepadAxis ==
                       InputActionAxisSource::GamepadLeftX &&
                   jump != nullptr && jump->positiveKeys[0] == DIK_SPACE &&
                   jump->gamepadButton == XINPUT_GAMEPAD_A &&
                   actionInput.GetActionAxis("Missing") == 0.0f &&
                   !actionInput.IsActionPressed("Missing"),
               "Default Input Action bindings are invalid.")) {
        return 183;
    }
    InputActionBinding customAction{};
    customAction.positiveKeys[0] = DIK_E;
    if (!Check(actionInput.SetActionBinding("Interact", customAction) &&
                   actionInput.GetActionBinding("Interact") != nullptr &&
                   actionInput.RemoveActionBinding("Interact") &&
                   actionInput.GetActionBinding("Interact") == nullptr &&
                   !actionInput.RemoveActionBinding("Interact") &&
                   !actionInput.SetActionBinding("", customAction) &&
                   !actionInput.SetActionBinding(
                       std::string(65u, 'A'), customAction),
               "Input Action binding registration is invalid.")) {
        return 184;
    }
    if (!Check(ScriptAssets::IsScriptFile("Player.cpp") &&
                   ScriptAssets::IsScriptSourceFile("Player.cpp") &&
                   ScriptAssets::IsScriptSourceFile("Player.h") &&
                   !ScriptAssets::IsScriptFile("Player.h"),
               "Script asset validation is invalid.")) {
        return 1;
    }
    std::filesystem::path diagnosticPath;
    uint32_t diagnosticLine = 0u;
    uint32_t diagnosticColumn = 0u;
    if (!Check(ScriptBuildService::ParseDiagnosticLocation(
                   R"(C:\Game Project\assets\Player.cpp(42,7): error C2065: unknown)",
                   diagnosticPath, diagnosticLine, diagnosticColumn) &&
                   diagnosticPath.filename() == L"Player.cpp" && diagnosticLine == 42u &&
                   diagnosticColumn == 7u &&
                   ScriptBuildService::ParseDiagnosticLocation(
                       R"(  C:\Game\assets\Enemy.h(9): warning C4100: unused)",
                       diagnosticPath, diagnosticLine, diagnosticColumn) &&
                   diagnosticLine == 9u && diagnosticColumn == 0u &&
                   !ScriptBuildService::ParseDiagnosticLocation(
                       "Project Scripts build failed.", diagnosticPath, diagnosticLine,
                       diagnosticColumn),
               "Project Script diagnostic location parsing is invalid.")) {
        return 142;
    }

    const std::filesystem::path repositoryRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    ProjectScriptLibrary projectScripts;
    BehaviorRegistry projectBehaviorRegistry;
    std::string projectScriptError;
    const std::filesystem::path testProjectRoot =
        repositoryRoot / L"projects" / L"test";
    const bool projectScriptsLoaded =
        ScriptBuildService::BuildIfNeeded(testProjectRoot, projectScriptError) &&
        projectScripts.Load(testProjectRoot, nullptr, projectBehaviorRegistry,
                            projectScriptError);
    std::unique_ptr<Behavior> firstPerson =
        projectBehaviorRegistry.Create("FirstPersonController");
    std::unique_ptr<Behavior> rotator = projectBehaviorRegistry.Create("Rotator");
    std::unique_ptr<Behavior> chasePlayer =
        projectBehaviorRegistry.Create("ChasePlayer");
    const std::vector<ScriptPropertyDefinition>* chaseProperties =
        projectBehaviorRegistry.Properties("ChasePlayer");
    const std::vector<ScriptPropertyDefinition>* controllerProperties =
        projectBehaviorRegistry.Properties("FirstPersonController");
    if (!Check(projectScriptsLoaded && projectScripts.IsLoaded() && firstPerson && rotator &&
                   chasePlayer &&
                   projectBehaviorRegistry.Requirements("FirstPersonController") != nullptr &&
                   projectBehaviorRegistry.Requirements("FirstPersonController")
                       ->characterController &&
                   controllerProperties != nullptr &&
                   controllerProperties->size() == 12u &&
                   (*controllerProperties)[0].name == "Move Speed" &&
                   (*controllerProperties)[0].defaultFloat == 4.0f &&
                   (*controllerProperties)[1].name == "Sprint Speed" &&
                   (*controllerProperties)[1].defaultFloat == 8.0f &&
                   (*controllerProperties)[5].name == "Jump Height" &&
                   (*controllerProperties)[5].defaultFloat == 1.5f &&
                   (*controllerProperties)[6].name == "Invert Y" &&
                   (*controllerProperties)[6].type == ScriptPropertyType::Boolean &&
                   (*controllerProperties)[7].name == "Idle Animation" &&
                   (*controllerProperties)[7].type == ScriptPropertyType::AnimationClip &&
                   (*controllerProperties)[7].defaultString == "Idle" &&
                   (*controllerProperties)[8].name == "Move Animation" &&
                   (*controllerProperties)[8].defaultString == "Walk" &&
                   (*controllerProperties)[9].name == "Sprint Animation" &&
                   (*controllerProperties)[9].defaultString == "Run" &&
                   (*controllerProperties)[10].name == "Jump Animation" &&
                   (*controllerProperties)[10].defaultString == "Jump" &&
                   (*controllerProperties)[11].name == "Animation Fade" &&
                   (*controllerProperties)[11].defaultFloat == 0.2f &&
                   projectBehaviorRegistry.Requirements("ChasePlayer") != nullptr &&
                   projectBehaviorRegistry.Requirements("ChasePlayer")
                       ->characterController &&
                   chaseProperties != nullptr && chaseProperties->size() == 7u &&
                   (*chaseProperties)[0].name == "Target" &&
                   (*chaseProperties)[0].type == ScriptPropertyType::Entity &&
                   (*chaseProperties)[1].name == "Move Speed" &&
                   (*chaseProperties)[1].defaultFloat == 2.5f &&
                   (*chaseProperties)[4].name == "Idle Animation" &&
                   (*chaseProperties)[4].type == ScriptPropertyType::AnimationClip &&
                   (*chaseProperties)[4].defaultString == "Idle" &&
                   (*chaseProperties)[5].name == "Move Animation" &&
                   (*chaseProperties)[5].type == ScriptPropertyType::AnimationClip &&
                   (*chaseProperties)[5].defaultString == "Run" &&
                   (*chaseProperties)[6].name == "Animation Fade" &&
                   (*chaseProperties)[6].defaultFloat == 0.2f &&
                   projectBehaviorRegistry.TypeFromSourceAsset(
                       "asset://Scripts/FirstPersonController.cpp") ==
                       "FirstPersonController" &&
                   projectBehaviorRegistry.SourceAsset("Rotator") ==
                       "asset://Scripts/Rotator.cpp",
               projectScriptError.empty() ?
                   "Project Script module registration or factory is invalid." :
                   projectScriptError.c_str())) {
        return 140;
    }
    World controllerAnimationWorld;
    const EntityId animatedController =
        controllerAnimationWorld.CreateEntity("Animated Controller");
    controllerAnimationWorld.Find(animatedController)->characterController =
        CharacterControllerComponent{};
    controllerAnimationWorld.Find(animatedController)->animator = AnimatorComponent{};
    BehaviorComponent controllerConfiguration{};
    controllerConfiguration.type = "FirstPersonController";
    ScriptPropertyValue controllerIdleProperty{};
    controllerIdleProperty.name = "Idle Animation";
    controllerIdleProperty.type = ScriptPropertyType::AnimationClip;
    controllerIdleProperty.stringValue = "Stand";
    controllerConfiguration.properties.push_back(controllerIdleProperty);
    BehaviorSystem controllerAnimationBehaviors;
    if (!Check(projectBehaviorRegistry.Configure("FirstPersonController",
                                                  controllerConfiguration, *firstPerson) &&
                   controllerAnimationBehaviors.Attach(animatedController,
                                                        std::move(firstPerson)),
               "FirstPersonController Animation properties could not be configured.")) {
        return 171;
    }
    controllerAnimationBehaviors.Start(controllerAnimationWorld);
    if (!Check(controllerAnimationWorld.Find(animatedController)->animator->runtimeCommand ==
                       AnimatorComponent::RuntimeCommand::Play &&
                   controllerAnimationWorld.Find(animatedController)
                           ->animator->runtimeRequestedClip == "Stand",
               "FirstPersonController did not start its Idle Animation.")) {
        return 172;
    }
    controllerAnimationBehaviors.Clear();
    World chaseAnimationWorld;
    const EntityId animatedChaser = chaseAnimationWorld.CreateEntity("Animated Chaser");
    const EntityId chaseTarget = chaseAnimationWorld.CreateEntity("Chase Target");
    chaseAnimationWorld.Find(animatedChaser)->characterController =
        CharacterControllerComponent{};
    chaseAnimationWorld.Find(animatedChaser)->animator = AnimatorComponent{};
    chaseAnimationWorld.Find(chaseTarget)->transform.position.x = 10.0f;
    BehaviorComponent chaseConfiguration{};
    chaseConfiguration.type = "ChasePlayer";
    ScriptPropertyValue chaseTargetProperty{};
    chaseTargetProperty.name = "Target";
    chaseTargetProperty.type = ScriptPropertyType::Entity;
    chaseTargetProperty.entityValue = chaseTarget;
    ScriptPropertyValue idleAnimationProperty{};
    idleAnimationProperty.name = "Idle Animation";
    idleAnimationProperty.type = ScriptPropertyType::AnimationClip;
    idleAnimationProperty.stringValue = "Wait";
    ScriptPropertyValue moveAnimationProperty{};
    moveAnimationProperty.name = "Move Animation";
    moveAnimationProperty.type = ScriptPropertyType::AnimationClip;
    moveAnimationProperty.stringValue = "Sprint";
    chaseConfiguration.properties = {chaseTargetProperty, idleAnimationProperty,
                                     moveAnimationProperty};
    BehaviorSystem chaseAnimationBehaviors;
    if (!Check(projectBehaviorRegistry.Configure("ChasePlayer", chaseConfiguration,
                                                  *chasePlayer) &&
                   chaseAnimationBehaviors.Attach(animatedChaser, std::move(chasePlayer)),
               "ChasePlayer Animation properties could not be configured.")) {
        return 169;
    }
    chaseAnimationBehaviors.Start(chaseAnimationWorld);
    const bool idleAnimationStarted =
        chaseAnimationWorld.Find(animatedChaser)->animator->runtimeCommand ==
            AnimatorComponent::RuntimeCommand::Play &&
        chaseAnimationWorld.Find(animatedChaser)->animator->runtimeRequestedClip == "Wait";
    chaseAnimationBehaviors.Update(0.1f);
    if (!Check(idleAnimationStarted &&
                   chaseAnimationWorld.Find(animatedChaser)->animator->runtimeCommand ==
                       AnimatorComponent::RuntimeCommand::CrossFade &&
                   chaseAnimationWorld.Find(animatedChaser)->animator->runtimeRequestedClip ==
                       "Sprint",
               "ChasePlayer did not switch from Idle to Move Animation.")) {
        return 170;
    }
    chaseAnimationBehaviors.Clear();
    firstPerson.reset();
    rotator.reset();
    chasePlayer.reset();
    std::string rebuildOutput;
    ProjectScriptLibrary reloadedProjectScripts;
    BehaviorRegistry reloadedProjectBehaviorRegistry;
    const bool projectScriptsReloaded =
        ScriptBuildService::Build(testProjectRoot, projectScriptError, &rebuildOutput) &&
        reloadedProjectScripts.Load(testProjectRoot, nullptr,
                                    reloadedProjectBehaviorRegistry,
                                    projectScriptError);
    if (projectScriptsReloaded) {
        projectBehaviorRegistry = std::move(reloadedProjectBehaviorRegistry);
        projectScripts = std::move(reloadedProjectScripts);
    }
    firstPerson = projectBehaviorRegistry.Create("FirstPersonController");
    rotator = projectBehaviorRegistry.Create("Rotator");
    chasePlayer = projectBehaviorRegistry.Create("ChasePlayer");
    if (!Check(projectScriptsReloaded && !rebuildOutput.empty() && firstPerson && rotator &&
                   chasePlayer,
               projectScriptError.empty() ?
                   "Project Script rebuild and reload is invalid." :
                   projectScriptError.c_str())) {
        return 143;
    }

    World behaviorWorld;
    const EntityId behaviorEntity = behaviorWorld.CreateEntity("Behavior Entity");
    int behaviorStartCount = 0;
    int behaviorUpdateCount = 0;
    int behaviorStopCount = 0;
    float behaviorLastDeltaTime = 0.0f;
    BehaviorRegistry behaviorRegistry;
    if (!Check(behaviorRegistry.Register("Lifecycle", [&] {
                   return std::make_unique<LifecycleBehavior>(
                       behaviorStartCount, behaviorUpdateCount, behaviorStopCount,
                       behaviorLastDeltaTime);
               }) &&
                   !behaviorRegistry.Register("Lifecycle", [] {
                       return std::unique_ptr<Behavior>{};
                   }) &&
                   behaviorRegistry.Create("Missing") == nullptr &&
                   behaviorRegistry.Types().size() == 1u,
               "Behavior type registration is invalid.")) {
        return 127;
    }
    if (!Check(behaviorRegistry.Register(
                   "ControllerDependency", [] { return std::make_unique<Behavior>(); },
                   {.characterController = true}),
               "Behavior requirements could not be registered.")) {
        return 139;
    }
    WorldEntity* dependencyTarget = behaviorWorld.Find(behaviorEntity);
    std::string dependencyError;
    if (!Check(dependencyTarget != nullptr &&
                   !behaviorRegistry.ValidateRequirements("ControllerDependency",
                                                          *dependencyTarget,
                                                          &dependencyError) &&
                   dependencyError.find("CharacterController") != std::string::npos &&
                   behaviorRegistry.EnsureRequirements("ControllerDependency",
                                                       *dependencyTarget) &&
                   dependencyTarget->characterController &&
                   behaviorRegistry.ValidateRequirements("ControllerDependency",
                                                          *dependencyTarget,
                                                          &dependencyError) &&
                   !behaviorRegistry.EnsureRequirements("Missing", *dependencyTarget),
               "Behavior component requirements were not enforced.")) {
        return 140;
    }
    float configuredSpeed = 0.0f;
    EntityId configuredTarget{};
    bool configuredBoolean = false;
    int32_t configuredInteger = 0;
    ScriptVector3 configuredVector{};
    std::string configuredString;
    const EntityId expectedConfiguredTarget = EntityId::New();
    std::vector<ScriptPropertyDefinition> configurableDefinitions = {
        {"Speed", ScriptPropertyType::Float, 2.5f, 0.0f, 20.0f},
        {"Target", ScriptPropertyType::Entity},
    };
    ScriptPropertyDefinition booleanDefinition{};
    booleanDefinition.name = "Aggressive";
    booleanDefinition.type = ScriptPropertyType::Boolean;
    booleanDefinition.defaultBoolean = true;
    configurableDefinitions.push_back(booleanDefinition);
    ScriptPropertyDefinition integerDefinition{};
    integerDefinition.name = "Lives";
    integerDefinition.type = ScriptPropertyType::Integer;
    integerDefinition.defaultInteger = 3;
    integerDefinition.minimumInteger = 1;
    integerDefinition.maximumInteger = 99;
    configurableDefinitions.push_back(integerDefinition);
    ScriptPropertyDefinition vectorDefinition{};
    vectorDefinition.name = "Offset";
    vectorDefinition.type = ScriptPropertyType::Vector3;
    vectorDefinition.defaultVector3 = {1.0f, 2.0f, 3.0f};
    configurableDefinitions.push_back(vectorDefinition);
    ScriptPropertyDefinition stringDefinition{};
    stringDefinition.name = "State";
    stringDefinition.type = ScriptPropertyType::String;
    stringDefinition.defaultString = "Idle";
    configurableDefinitions.push_back(stringDefinition);
    if (!Check(behaviorRegistry.Register(
                   "Configurable",
                   [&] {
                       return std::make_unique<ConfigurableBehavior>(configuredSpeed,
                            configuredTarget, configuredBoolean, configuredInteger,
                            configuredVector, configuredString);
                   },
                   {}, "", std::move(configurableDefinitions)),
               "Script property metadata could not be registered.")) {
        return 146;
    }
    BehaviorComponent configurableComponent{};
    configurableComponent.properties = {
        {"Speed", ScriptPropertyType::Float, 7.5f, {}},
        {"Target", ScriptPropertyType::Entity, 0.0f, expectedConfiguredTarget},
    };
    ScriptPropertyValue booleanValue{};
    booleanValue.name = "Aggressive";
    booleanValue.type = ScriptPropertyType::Boolean;
    booleanValue.booleanValue = true;
    configurableComponent.properties.push_back(booleanValue);
    ScriptPropertyValue integerValue{};
    integerValue.name = "Lives";
    integerValue.type = ScriptPropertyType::Integer;
    integerValue.integerValue = 7;
    configurableComponent.properties.push_back(integerValue);
    ScriptPropertyValue vectorValue{};
    vectorValue.name = "Offset";
    vectorValue.type = ScriptPropertyType::Vector3;
    vectorValue.vector3Value = {4.0f, 5.0f, 6.0f};
    configurableComponent.properties.push_back(vectorValue);
    ScriptPropertyValue stringValue{};
    stringValue.name = "State";
    stringValue.type = ScriptPropertyType::String;
    stringValue.stringValue = "Run";
    configurableComponent.properties.push_back(stringValue);
    std::unique_ptr<Behavior> configurableBehavior =
        behaviorRegistry.Create("Configurable");
    const std::vector<ScriptPropertyDefinition>* configurableProperties =
        behaviorRegistry.Properties("Configurable");
    if (!Check(configurableBehavior && configurableProperties != nullptr &&
                   configurableProperties->size() == 6u &&
                   behaviorRegistry.Configure("Configurable", configurableComponent,
                                              *configurableBehavior) &&
                   configuredSpeed == 7.5f &&
                   configuredTarget == expectedConfiguredTarget && configuredBoolean &&
                   configuredInteger == 7 && configuredVector.x == 4.0f &&
                   configuredVector.y == 5.0f && configuredVector.z == 6.0f &&
                   configuredString == "Run",
               "Script property values were not configured on the Behavior.")) {
        return 147;
    }
    BehaviorSystem behaviors;
    if (!Check(behaviors.Attach(behaviorEntity, behaviorRegistry.Create("Lifecycle")),
               "A valid Runtime Behavior could not be attached.")) {
        return 125;
    }
    behaviors.Start(behaviorWorld);
    behaviors.Update(0.25f);
    behaviorWorld.Find(behaviorEntity)->active = false;
    behaviors.Update(0.5f);
    behaviorWorld.Find(behaviorEntity)->active = true;
    behaviors.Update(0.75f);
    behaviors.Stop();
    const WorldEntity* behaviorTarget = behaviorWorld.Find(behaviorEntity);
    if (!Check(!behaviors.IsRunning() && behaviors.Size() == 1u &&
                   behaviorStartCount == 2 && behaviorUpdateCount == 2 &&
                   behaviorStopCount == 2 && behaviorLastDeltaTime == 0.75f &&
                   behaviorTarget != nullptr && behaviorTarget->transform.position.x == 2.0f &&
                   behaviorTarget->transform.position.y == 1.0f,
               "Runtime Behavior lifecycle or Entity activation handling is invalid.")) {
        return 126;
    }
    std::vector<int> orderedBehaviorEvents;
    BehaviorSystem orderedBehaviors;
    if (!Check(orderedBehaviors.Attach(
                   behaviorEntity,
                   std::make_unique<OrderedBehavior>(1, orderedBehaviorEvents)) &&
                   orderedBehaviors.Attach(
                       behaviorEntity,
                       std::make_unique<OrderedBehavior>(2, orderedBehaviorEvents)),
               "Ordered Runtime Behaviors could not be attached.")) {
        return 181;
    }
    orderedBehaviors.Start(behaviorWorld);
    orderedBehaviors.Update(0.25f);
    orderedBehaviors.Stop();
    if (!Check(orderedBehaviorEvents == std::vector<int>({11, 21, 12, 22, 23, 13}),
               "Runtime Behaviors did not preserve component execution order.")) {
        return 182;
    }

    World triggerWorld;
    const EntityId triggerActor = triggerWorld.CreateEntity("Trigger Actor");
    const EntityId triggerZone = triggerWorld.CreateEntity("Trigger Zone");
    triggerWorld.Find(triggerActor)->characterController =
        CharacterControllerComponent{};
    triggerWorld.Find(triggerActor)->boxCollider = BoxColliderComponent{};
    triggerWorld.Find(triggerZone)->boxCollider = BoxColliderComponent{};
    triggerWorld.Find(triggerZone)->boxCollider->isTrigger = true;
    TriggerEventCounts actorTriggerEvents{};
    TriggerEventCounts zoneTriggerEvents{};
    BehaviorSystem triggerBehaviors;
    TriggerSystem triggers;
    if (!Check(triggerBehaviors.Attach(
                   triggerActor, std::make_unique<TriggerBehavior>(actorTriggerEvents)) &&
                   triggerBehaviors.Attach(
                       triggerZone, std::make_unique<TriggerBehavior>(zoneTriggerEvents)),
               "Trigger test Behaviors could not be attached.")) {
        return 144;
    }
    triggerBehaviors.Start(triggerWorld);
    triggers.Update(triggerWorld, triggerBehaviors);
    triggers.Update(triggerWorld, triggerBehaviors);
    triggerWorld.Find(triggerActor)->transform.position.x = 3.0f;
    triggers.Update(triggerWorld, triggerBehaviors);
    triggerWorld.Find(triggerActor)->transform.position.x = 0.0f;
    triggers.Update(triggerWorld, triggerBehaviors);
    triggerWorld.DestroyEntity(triggerZone);
    triggers.Update(triggerWorld, triggerBehaviors);
    if (!Check(triggers.ActivePairCount() == 0u &&
                   actorTriggerEvents.enter == 2 && actorTriggerEvents.stay == 1 &&
                   actorTriggerEvents.exit == 2 &&
                   actorTriggerEvents.lastOther == triggerZone &&
                   zoneTriggerEvents.enter == 2 && zoneTriggerEvents.stay == 1 &&
                   zoneTriggerEvents.exit == 1 &&
                   zoneTriggerEvents.lastOther == triggerActor,
               "Trigger Enter, Stay, Exit, or destroyed-entity handling is invalid.")) {
        return 145;
    }
    triggers.Clear();
    triggerBehaviors.Clear();

    if (!Check(RotationRoundTrips({25.0f, -40.0f, 70.0f}) &&
                   RotationRoundTrips({120.0f, 215.0f, -150.0f}) &&
                   RotationRoundTrips({89.999f, 35.0f, -20.0f}),
               "Editor Euler rotation conversion changed the rotation matrix.")) {
        return 124;
    }
    Model blendedModel{};
    BoneInfo blendedBone{};
    blendedBone.name = "Root";
    DirectX::XMStoreFloat4x4(&blendedBone.offsetMatrix, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&blendedBone.localBindMatrix, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&blendedBone.parentAdjustmentMatrix,
                             DirectX::XMMatrixIdentity());
    blendedModel.bones.push_back(blendedBone);
    AnimationClip idleClip{};
    idleClip.duration = 1.0f;
    idleClip.nodeAnimations["Root"].translate.keyframes.push_back(
        {0.0f, {0.0f, 0.0f, 0.0f}});
    AnimationClip runClip{};
    runClip.duration = 1.0f;
    runClip.nodeAnimations["Root"].translate.keyframes.push_back(
        {0.0f, {10.0f, 0.0f, 0.0f}});
    blendedModel.animations.emplace("Idle", std::move(idleClip));
    blendedModel.animations.emplace("Run", std::move(runClip));
    Animator::Play(blendedModel, "Idle", true);
    Animator::Update(blendedModel, 0.0f);
    Animator::CrossFade(blendedModel, "Run", 1.0f, true);
    Animator::Update(blendedModel, 0.5f);
    const bool halfwayBlended = blendedModel.skeletonSpaceMatrices.size() == 1u &&
                                std::abs(blendedModel.skeletonSpaceMatrices[0]._41 - 5.0f) <
                                    0.001f;
    Animator::Update(blendedModel, 0.5f);
    if (!Check(halfwayBlended && blendedModel.blendSourceAnimation.empty() &&
                   blendedModel.skeletonSpaceMatrices.size() == 1u &&
                   std::abs(blendedModel.skeletonSpaceMatrices[0]._41 - 10.0f) < 0.001f,
               "Animator CrossFade did not blend or finish the skeleton pose.")) {
        return 167;
    }
    blendedModel.animations["Run"].rootNodeName = "Root";
    blendedModel.lockRootAnimationPosition = true;
    Animator::Play(blendedModel, "Run", true);
    Animator::Update(blendedModel, 0.0f);
    if (!Check(blendedModel.skeletonSpaceMatrices.size() == 1u &&
                   std::abs(blendedModel.skeletonSpaceMatrices[0]._41) < 0.001f,
               "Animator root position lock did not preserve the bind position.")) {
        return 179;
    }
    Model rootAnimatedModel{};
    AnimationClip rootAnimatedClip{};
    rootAnimatedClip.duration = 1.0f;
    rootAnimatedClip.rootNodeName = "Root";
    rootAnimatedClip.nodeAnimations["Root"].translate.keyframes = {
        {0.0f, {2.0f, 0.0f, 0.0f}},
        {1.0f, {8.0f, 0.0f, 0.0f}},
    };
    rootAnimatedModel.animations.emplace("Move", std::move(rootAnimatedClip));
    rootAnimatedModel.lockRootAnimationPosition = true;
    Animator::Play(rootAnimatedModel, "Move", false);
    Animator::Update(rootAnimatedModel, 0.5f);
    if (!Check(rootAnimatedModel.hasRootAnimation &&
                   std::abs(rootAnimatedModel.rootAnimationMatrix._41) < 0.001f,
               "Animator root position lock did not align a non-skinned root to its Entity.")) {
        return 180;
    }
    const auto boxPrimitive =
        ModelPrimitiveFactory::BuildBox(0u, Material{}, 1.0f, 2.0f, 1.0f);
    const auto cylinderPrimitive =
        ModelPrimitiveFactory::BuildCylinder(0u, Material{}, 16u, 0.5f, 0.5f, 2.0f);
    const auto planePrimitive = ModelPrimitiveFactory::BuildPlane(0u, Material{});
    const auto spherePrimitive =
        ModelPrimitiveFactory::BuildSphere(0u, Material{}, 16u, 8u, 0.5f);
    if (!Check(boxPrimitive && IsVerticallyCentered(*boxPrimitive),
               "Box primitive is not centered on its origin.")) {
        return 116;
    }
    if (!Check(cylinderPrimitive && IsVerticallyCentered(*cylinderPrimitive),
               "Cylinder primitive is not centered on its origin.")) {
        return 117;
    }
    if (!Check(planePrimitive && boxPrimitive && spherePrimitive && cylinderPrimitive &&
                   HasOutwardWinding(*planePrimitive) &&
                   HasOutwardWinding(*boxPrimitive) &&
                   HasOutwardWinding(*spherePrimitive) &&
                   HasOutwardWinding(*cylinderPrimitive),
               "Primitive front-face winding is inconsistent.")) {
        return 123;
    }

    World source;
    const EntityId root = source.CreateEntity("Root");
    const EntityId child = source.CreateEntity("Child");
    if (!Check(root.IsValid() && child.IsValid() && root != child, "Entity ids are invalid.")) {
        return 1;
    }

    EntityId parsed{};
    if (!Check(EntityId::TryParse(root.ToString(), parsed) && parsed == root,
               "Entity id text round-trip failed.")) {
        return 2;
    }
    if (!Check(source.SetParent(child, root), "Valid parenting failed.") ||
        !Check(!source.SetParent(root, child), "Hierarchy cycle was accepted.")) {
        return 3;
    }

    WorldEntity* childEntity = source.Find(child);
    if (!Check(childEntity != nullptr, "Child entity was not found.")) {
        return 4;
    }
    childEntity->transform.position = {1.0f, 2.0f, 3.0f};
    childEntity->transform.rotationDegrees = {10.0f, 20.0f, 30.0f};
    childEntity->layer = 2u;
    childEntity->meshRenderer = MeshRendererComponent{};
    childEntity->meshRenderer->primitive = MeshPrimitive::Sphere;
    childEntity->materialOverride = MaterialOverrideComponent{};
    childEntity->materialOverride->baseColor = {0.25f, 0.5f, 0.75f, 0.8f};
    childEntity->materialOverride->metallic = 0.7f;
    childEntity->materialOverride->roughness = 0.2f;
    childEntity->materialOverride->baseColorTexturePath = "asset://textures/test.png";
    childEntity->materialOverride->normalTexturePath = "asset://textures/test_normal.png";
    childEntity->materialOverride->normalStrength = 1.5f;
    childEntity->materialOverride->roughnessTexturePath = "asset://textures/test_orm.png";
    childEntity->materialOverride->metallicTexturePath = "asset://textures/test_orm.png";
    childEntity->materialOverride->pbrTexturePacking =
        MaterialPbrTexturePacking::OcclusionRoughnessMetallic;
    childEntity->materialOverride->blendMode = MaterialSurfaceBlendMode::Transparent;
    childEntity->materialOverride->alphaCutoff = 0.35f;
    childEntity->materialOverride->cullMode = MaterialSurfaceCullMode::None;
    childEntity->materialOverride->depthWrite = false;
    childEntity->light = LightComponent{};
    childEntity->light->type = LightType::Point;
    childEntity->light->intensity = 2.0f;
    childEntity->audioSource = AudioSourceComponent{};
    childEntity->audioSource->clipPath = "asset://Audio/test.wav";
    childEntity->audioSource->loop = true;
    childEntity->audioSource->volume = 0.6f;
    childEntity->audioSource->pitch = 1.25f;
    childEntity->audioSource->spatial = true;
    childEntity->audioSource->minDistance = 2.0f;
    childEntity->audioSource->maxDistance = 30.0f;
    childEntity->animator = AnimatorComponent{};
    childEntity->animator->clip = "Run";
    childEntity->animator->loop = false;
    childEntity->animator->speed = 1.5f;
    childEntity->animator->lockRootPosition = false;
    if (!Check(source.PlayAnimation(child, "Run", false) &&
                   childEntity->animator->runtimeCommand ==
                       AnimatorComponent::RuntimeCommand::Play &&
                   childEntity->animator->runtimeRequestedClip == "Run" &&
                   !childEntity->animator->runtimeLoop &&
                   !source.IsAnimationPlaying(child) && !source.IsAnimationFinished(child) &&
                   source.CrossFadeAnimation(child, "Idle", 0.25f) &&
                   childEntity->animator->runtimeCommand ==
                       AnimatorComponent::RuntimeCommand::CrossFade &&
                   childEntity->animator->runtimeRequestedClip == "Idle" &&
                   childEntity->animator->runtimeLoop &&
                   childEntity->animator->runtimeFadeDuration == 0.25f &&
                   !source.CrossFadeAnimation(child, "Idle", -1.0f) &&
                   source.StopAnimation(child) &&
                   childEntity->animator->runtimeCommand ==
                       AnimatorComponent::RuntimeCommand::Stop &&
                   !source.PlayAnimation(root, "Run") && !source.StopAnimation(root),
               "World Animator playback commands are invalid.")) {
        return 166;
    }
    childEntity->animator->runtimeClip = "Attack";
    childEntity->animator->runtimeNormalizedTime = 0.6f;
    childEntity->animator->runtimeTransitioning = true;
    if (!Check(source.GetCurrentAnimation(child) == "Attack" &&
                   source.GetAnimationNormalizedTime(child) == 0.6f &&
                   source.IsAnimationTransitioning(child) &&
                   source.GetCurrentAnimation(root).empty() &&
                   source.GetAnimationNormalizedTime(root) == 0.0f &&
                   !source.IsAnimationTransitioning(root),
               "World Animator playback state queries are invalid.")) {
        return 168;
    }
    if (!Check(source.PlayAudioSource(child) &&
                   childEntity->audioSource->runtimeCommand ==
                       AudioSourceComponent::RuntimeCommand::Play &&
                   source.PlayAudioSourceOneShot(child) &&
                   source.PlayAudioSourceOneShot(child) &&
                   childEntity->audioSource->pendingOneShots == 2u &&
                   !source.IsAudioSourcePlaying(child) && source.StopAudioSource(child) &&
                   childEntity->audioSource->runtimeCommand ==
                       AudioSourceComponent::RuntimeCommand::Stop &&
                   childEntity->audioSource->pendingOneShots == 0u &&
                   !source.PlayAudioSource(root) && !source.PlayAudioSourceOneShot(root),
               "World AudioSource playback commands are invalid.")) {
        return 159;
    }
    childEntity->scripts.push_back(
        {true, "Rotator", "asset://Scripts/Rotator.cpp"});
    childEntity->scripts[0].properties = {
        {"Speed", ScriptPropertyType::Float, 3.5f, {}},
        {"Target", ScriptPropertyType::Entity, 0.0f, root},
    };
    ScriptPropertyValue serializedBoolean{};
    serializedBoolean.name = "Aggressive";
    serializedBoolean.type = ScriptPropertyType::Boolean;
    serializedBoolean.booleanValue = true;
    childEntity->scripts[0].properties.push_back(serializedBoolean);
    ScriptPropertyValue serializedInteger{};
    serializedInteger.name = "Lives";
    serializedInteger.type = ScriptPropertyType::Integer;
    serializedInteger.integerValue = 7;
    childEntity->scripts[0].properties.push_back(serializedInteger);
    ScriptPropertyValue serializedVector{};
    serializedVector.name = "Offset";
    serializedVector.type = ScriptPropertyType::Vector3;
    serializedVector.vector3Value = {4.0f, 5.0f, 6.0f};
    childEntity->scripts[0].properties.push_back(serializedVector);
    ScriptPropertyValue serializedString{};
    serializedString.name = "Animation";
    serializedString.type = ScriptPropertyType::String;
    serializedString.stringValue = "Run";
    childEntity->scripts[0].properties.push_back(serializedString);
    ScriptPropertyValue serializedClip{};
    serializedClip.name = "Clip";
    serializedClip.type = ScriptPropertyType::AnimationClip;
    serializedClip.stringValue = "Attack";
    childEntity->scripts[0].properties.push_back(serializedClip);
    childEntity->scripts.push_back(
        {false, "FirstPersonController",
         "asset://Scripts/FirstPersonController.cpp"});
    childEntity->scripts.emplace_back();
    childEntity->boxCollider = BoxColliderComponent{};
    childEntity->boxCollider->center = {0.25f, 0.5f, -0.25f};
    childEntity->boxCollider->size = {1.0f, 2.0f, 3.0f};
    childEntity->characterController = CharacterControllerComponent{};
    childEntity->characterController->center = {0.0f, 1.0f, 0.0f};
    childEntity->characterController->radius = 0.4f;
    childEntity->characterController->height = 1.8f;
    if (WorldEntity* rootEntity = source.Find(root)) {
        rootEntity->transform.position = {4.0f, 0.0f, 0.0f};
        rootEntity->camera = CameraComponent{};
        rootEntity->camera->primary = true;
        rootEntity->camera->fieldOfViewDegrees = 60.0f;
        rootEntity->audioListener = AudioListenerComponent{};
    }

    DirectX::XMFLOAT4X4 childWorld{};
    if (!Check(source.TryGetWorldMatrix(child, childWorld) &&
                   std::abs(childWorld._41 - 5.0f) < 0.001f &&
                   std::abs(childWorld._42 - 2.0f) < 0.001f,
               "Parent and child transforms were not composed.")) {
        return 5;
    }
    OBB childCollider{};
    if (!Check(TryBuildWorldBoxCollider(source, child, childCollider) &&
                   std::abs(childCollider.size.x - 1.0f) < 0.001f &&
                   std::abs(childCollider.size.y - 2.0f) < 0.001f &&
                   std::abs(childCollider.size.z - 3.0f) < 0.001f,
               "World BoxCollider did not follow the entity transform.")) {
        return 130;
    }
    OBB overlappingCollider = childCollider;
    overlappingCollider.center.x += 0.25f;
    if (!Check(CollisionUtil::CheckOBB(childCollider, overlappingCollider),
               "Overlapping World BoxColliders were not detected.")) {
        return 131;
    }
    overlappingCollider.center.x += 100.0f;
    if (!Check(!CollisionUtil::CheckOBB(childCollider, overlappingCollider),
               "Separated World BoxColliders reported a collision.")) {
        return 132;
    }

    World movementWorld;
    const EntityId mover = movementWorld.CreateEntity("Mover");
    const EntityId wall = movementWorld.CreateEntity("Wall");
    movementWorld.Find(mover)->characterController = CharacterControllerComponent{};
    movementWorld.Find(wall)->boxCollider = BoxColliderComponent{};
    movementWorld.Find(wall)->transform.position.x = 1.0f;
    const CharacterMoveResult blockedMovement =
        MoveCharacterController(movementWorld, mover, {2.0f, 0.0f, 0.5f});
    if (!Check(blockedMovement.appliedMotion.x >= 0.0f &&
                   blockedMovement.appliedMotion.x < 0.1f &&
                   std::abs(blockedMovement.appliedMotion.z - 0.5f) < 0.001f &&
                   (static_cast<uint8_t>(blockedMovement.flags) &
                    static_cast<uint8_t>(CharacterCollisionFlags::Sides)) != 0u &&
                   movementWorld.Find(mover)->transform.position.x < 0.1f &&
                   std::abs(movementWorld.Find(mover)->transform.position.z - 0.5f) < 0.001f,
               "CharacterController did not stop at a solid BoxCollider and slide.")) {
        return 134;
    }
    movementWorld.Find(wall)->boxCollider->isTrigger = true;
    const float positionBeforeTrigger = movementWorld.Find(mover)->transform.position.x;
    const CharacterMoveResult triggerMovement =
        MoveCharacterController(movementWorld, mover, {2.0f, 0.0f, 0.0f});
    if (!Check(std::abs(triggerMovement.appliedMotion.x - 2.0f) < 0.001f &&
                   std::abs(movementWorld.Find(mover)->transform.position.x -
                            (positionBeforeTrigger + 2.0f)) < 0.001f,
               "Trigger BoxCollider incorrectly blocked movement.")) {
        return 135;
    }

    World layerFilteredWorld;
    PhysicsSettings layerSettings = PhysicsSettings::Defaults();
    layerSettings.layerNames[1] = "Player";
    layerSettings.layerNames[2] = "Environment";
    layerSettings.SetLayersCollide(1u, 2u, false);
    layerFilteredWorld.SetPhysicsSettings(layerSettings);
    const EntityId filteredMover = layerFilteredWorld.CreateEntity("Filtered Mover");
    const EntityId filteredWall = layerFilteredWorld.CreateEntity("Filtered Wall");
    layerFilteredWorld.Find(filteredMover)->layer = 1u;
    layerFilteredWorld.Find(filteredMover)->characterController =
        CharacterControllerComponent{};
    layerFilteredWorld.Find(filteredMover)->boxCollider = BoxColliderComponent{};
    layerFilteredWorld.Find(filteredWall)->layer = 2u;
    layerFilteredWorld.Find(filteredWall)->boxCollider = BoxColliderComponent{};
    layerFilteredWorld.Find(filteredWall)->transform.position.x = 1.0f;
    const CharacterMoveResult filteredMovement =
        MoveCharacterController(layerFilteredWorld, filteredMover, {2.0f, 0.0f, 0.0f});
    layerFilteredWorld.Find(filteredWall)->boxCollider->isTrigger = true;
    TriggerSystem filteredTriggers;
    BehaviorSystem filteredTriggerBehaviors;
    filteredTriggerBehaviors.Start(layerFilteredWorld);
    filteredTriggers.Update(layerFilteredWorld, filteredTriggerBehaviors);
    if (!Check(std::abs(filteredMovement.appliedMotion.x - 2.0f) < 0.001f &&
                   filteredMovement.flags == CharacterCollisionFlags::None &&
                   !CheckCharacterControllerBoxOverlap(layerFilteredWorld, filteredMover,
                                                       filteredWall) &&
                   filteredTriggers.ActivePairCount() == 0u,
               "Physics Layer matrix did not filter movement or Trigger overlap.")) {
        return 150;
    }

    World groundWorld;
    const EntityId groundedController = groundWorld.CreateEntity("Grounded Controller");
    const EntityId floor = groundWorld.CreateEntity("Floor");
    groundWorld.Find(groundedController)->characterController =
        CharacterControllerComponent{};
    groundWorld.Find(groundedController)->transform.position.y = 1.5f;
    groundWorld.Find(floor)->boxCollider = BoxColliderComponent{};
    groundWorld.Find(floor)->boxCollider->size = {10.0f, 1.0f, 10.0f};
    const CharacterMoveResult downwardMovement =
        MoveCharacterController(groundWorld, groundedController, {0.0f, -1.0f, 0.0f});
    if (!Check(downwardMovement.appliedMotion.y > -0.1f &&
                   (static_cast<uint8_t>(downwardMovement.flags) &
                    static_cast<uint8_t>(CharacterCollisionFlags::Below)) != 0u,
               "CharacterController did not report a collision below.")) {
        return 138;
    }

    groundWorld.Find(floor)->active = false;
    const CharacterMoveResult inactiveFloorMovement =
        MoveCharacterController(groundWorld, groundedController, {0.0f, -1.0f, 0.0f});
    if (!Check(inactiveFloorMovement.appliedMotion.y < -0.9f &&
                   inactiveFloorMovement.flags == CharacterCollisionFlags::None,
               "An inactive Entity still blocked CharacterController movement.")) {
        return 156;
    }

    source.Find(root)->active = false;
    if (!Check(source.Find(child)->active && !source.IsActiveInHierarchy(child),
               "A child of an inactive Entity remained active in the hierarchy.")) {
        return 157;
    }

    const std::string serialized = WorldSerializer::Serialize(source);
    World restored;
    std::string error;
    if (!Check(WorldSerializer::Deserialize(serialized, restored, &error), error.c_str())) {
        return 6;
    }
    const WorldEntity* restoredChild = restored.Find(child);
    const bool hasRestoredScript =
        restoredChild != nullptr && !restoredChild->scripts.empty();
    const ScriptPropertyValue* restoredSpeed =
        hasRestoredScript ? FindStoredScriptProperty(restoredChild->scripts[0], "Speed")
                          : nullptr;
    const ScriptPropertyValue* restoredTarget =
        hasRestoredScript ? FindStoredScriptProperty(restoredChild->scripts[0], "Target")
                          : nullptr;
    const ScriptPropertyValue* restoredBoolean = hasRestoredScript
        ? FindStoredScriptProperty(restoredChild->scripts[0], "Aggressive") : nullptr;
    const ScriptPropertyValue* restoredInteger = hasRestoredScript
        ? FindStoredScriptProperty(restoredChild->scripts[0], "Lives") : nullptr;
    const ScriptPropertyValue* restoredVector = hasRestoredScript
        ? FindStoredScriptProperty(restoredChild->scripts[0], "Offset") : nullptr;
    const ScriptPropertyValue* restoredString = hasRestoredScript
        ? FindStoredScriptProperty(restoredChild->scripts[0], "Animation") : nullptr;
    const ScriptPropertyValue* restoredClip = hasRestoredScript
        ? FindStoredScriptProperty(restoredChild->scripts[0], "Clip") : nullptr;
    if (!Check(restored.Entities().size() == 2u && restoredChild != nullptr &&
                   restored.Find(root) != nullptr && !restored.Find(root)->active &&
                   restoredChild->active && !restored.IsActiveInHierarchy(child) &&
                   restoredChild->parent == root && restoredChild->layer == 2u &&
                   restoredChild->transform.position.x == 1.0f &&
                   restoredChild->transform.rotationDegrees.z == 30.0f &&
                   restoredChild->meshRenderer &&
                   restoredChild->meshRenderer->primitive == MeshPrimitive::Sphere &&
                   restoredChild->materialOverride &&
                   restoredChild->materialOverride->baseColor.z == 0.75f &&
                   restoredChild->materialOverride->baseColor.w == 0.8f &&
                   restoredChild->materialOverride->metallic == 0.7f &&
                   restoredChild->materialOverride->roughness == 0.2f &&
                   restoredChild->materialOverride->baseColorTexturePath ==
                       "asset://textures/test.png" &&
                   restoredChild->materialOverride->normalTexturePath ==
                       "asset://textures/test_normal.png" &&
                   restoredChild->materialOverride->normalStrength == 1.5f &&
                   restoredChild->materialOverride->roughnessTexturePath ==
                       "asset://textures/test_orm.png" &&
                   restoredChild->materialOverride->metallicTexturePath ==
                       "asset://textures/test_orm.png" &&
                   restoredChild->materialOverride->pbrTexturePacking ==
                       MaterialPbrTexturePacking::OcclusionRoughnessMetallic &&
                   restoredChild->materialOverride->blendMode ==
                       MaterialSurfaceBlendMode::Transparent &&
                   restoredChild->materialOverride->alphaCutoff == 0.35f &&
                   restoredChild->materialOverride->cullMode ==
                       MaterialSurfaceCullMode::None &&
                   !restoredChild->materialOverride->depthWrite &&
                   restoredChild->light && restoredChild->light->type == LightType::Point &&
                   restoredChild->light->intensity == 2.0f &&
                   restoredChild->audioSource && restoredChild->audioSource->enabled &&
                   restoredChild->audioSource->clipPath == "asset://Audio/test.wav" &&
                   restoredChild->audioSource->playOnAwake &&
                   restoredChild->audioSource->loop &&
                   restoredChild->audioSource->volume == 0.6f &&
                   restoredChild->audioSource->pitch == 1.25f &&
                   restoredChild->audioSource->spatial &&
                   restoredChild->audioSource->minDistance == 2.0f &&
                   restoredChild->audioSource->maxDistance == 30.0f &&
                   restoredChild->animator && restoredChild->animator->enabled &&
                   restoredChild->animator->clip == "Run" &&
                   restoredChild->animator->playOnAwake && !restoredChild->animator->loop &&
                   restoredChild->animator->speed == 1.5f &&
                   !restoredChild->animator->lockRootPosition &&
                   restoredChild->animator->runtimeCommand ==
                       AnimatorComponent::RuntimeCommand::None &&
                   restoredChild->animator->runtimeRequestedClip.empty() &&
                   restoredChild->animator->runtimeClip.empty() &&
                   restoredChild->animator->runtimeFadeDuration == 0.0f &&
                   !restoredChild->animator->runtimePlaying &&
                   !restoredChild->animator->runtimeFinished &&
                   restoredChild->animator->runtimeTime == 0.0f &&
                   restoredChild->animator->runtimeDuration == 0.0f &&
                   restoredChild->animator->runtimeNormalizedTime == 0.0f &&
                   !restoredChild->animator->runtimeTransitioning &&
                   restoredChild->animator->runtimeTransitionProgress == 0.0f &&
                   restoredChild->scripts.size() == 3u &&
                   restoredChild->scripts[0].enabled &&
                   restoredChild->scripts[0].type == "Rotator" &&
                   restoredChild->scripts[0].scriptAssetPath ==
                       "asset://Scripts/Rotator.cpp" &&
                   restoredChild->scripts[0].properties.size() == 7u &&
                   restoredSpeed != nullptr && restoredSpeed->type == ScriptPropertyType::Float &&
                   restoredSpeed->floatValue == 3.5f && restoredTarget != nullptr &&
                   restoredTarget->type == ScriptPropertyType::Entity &&
                   restoredTarget->entityValue == root && restoredBoolean != nullptr &&
                   restoredBoolean->type == ScriptPropertyType::Boolean &&
                   restoredBoolean->booleanValue && restoredInteger != nullptr &&
                   restoredInteger->type == ScriptPropertyType::Integer &&
                   restoredInteger->integerValue == 7 && restoredVector != nullptr &&
                   restoredVector->type == ScriptPropertyType::Vector3 &&
                   restoredVector->vector3Value.x == 4.0f &&
                   restoredVector->vector3Value.y == 5.0f &&
                   restoredVector->vector3Value.z == 6.0f && restoredString != nullptr &&
                   restoredString->type == ScriptPropertyType::String &&
                   restoredString->stringValue == "Run" && restoredClip != nullptr &&
                   restoredClip->type == ScriptPropertyType::AnimationClip &&
                   restoredClip->stringValue == "Attack" &&
                   !restoredChild->scripts[1].enabled &&
                   restoredChild->scripts[1].type == "FirstPersonController" &&
                   restoredChild->scripts[2].type.empty() &&
                   restoredChild->scripts[2].scriptAssetPath.empty() &&
                   restoredChild->boxCollider && !restoredChild->boxCollider->isTrigger &&
                   restoredChild->boxCollider->center.y == 0.5f &&
                   restoredChild->boxCollider->size.z == 3.0f &&
                   restoredChild->characterController &&
                   restoredChild->characterController->center.y == 1.0f &&
                   restoredChild->characterController->radius == 0.4f &&
                   restoredChild->characterController->height == 1.8f &&
                   restored.Find(root)->camera && restored.Find(root)->camera->primary &&
                   restored.Find(root)->audioListener &&
                   restored.Find(root)->audioListener->enabled &&
                   restored.Find(root)->camera->fieldOfViewDegrees == 60.0f,
               "World JSON round-trip changed entity data.")) {
        return 7;
    }

    World instantiatedWorld;
    const EntityId instanceParent = instantiatedWorld.CreateEntity("Instance Parent");
    ScriptPropertyValue externalReference{};
    externalReference.name = "External Target";
    externalReference.type = ScriptPropertyType::Entity;
    externalReference.entityValue = EntityId::New();
    restored.Find(child)->scripts[0].properties.push_back(externalReference);
    std::vector<EntityId> instanceRoots;
    if (!Check(instantiatedWorld.InstantiateEntityHierarchies(
                   restored, instanceParent, instanceRoots, &error) &&
                   instanceRoots.size() == 1u,
               error.empty() ? "Entity hierarchy template could not be instantiated."
                             : error.c_str())) {
        return 153;
    }
    const WorldEntity* instanceRoot = instantiatedWorld.Find(instanceRoots.front());
    const std::vector<EntityId> instanceChildren =
        instantiatedWorld.GetChildren(instanceRoots.front());
    const WorldEntity* instanceChild = instanceChildren.size() == 1u
                                           ? instantiatedWorld.Find(instanceChildren.front())
                                           : nullptr;
    const ScriptPropertyValue* instanceTarget =
        instanceChild != nullptr && !instanceChild->scripts.empty()
            ? FindStoredScriptProperty(instanceChild->scripts[0], "Target")
            : nullptr;
    const ScriptPropertyValue* instanceExternalTarget =
        instanceChild != nullptr && !instanceChild->scripts.empty()
            ? FindStoredScriptProperty(instanceChild->scripts[0], "External Target")
            : nullptr;
    if (!Check(instanceRoot != nullptr && instanceRoot->id != root &&
                   instanceRoot->parent == instanceParent && instanceRoot->camera &&
                   !instanceRoot->camera->primary && instanceRoot->audioListener &&
                   instanceChild != nullptr &&
                   instanceChild->id != child && instanceChild->parent == instanceRoot->id &&
                   instanceChild->meshRenderer && instanceChild->materialOverride &&
                   instanceChild->light && instanceChild->audioSource && instanceChild->animator &&
                   instanceChild->animator->clip == "Run" &&
                   !instanceChild->animator->lockRootPosition &&
                   instanceChild->animator->runtimeCommand ==
                       AnimatorComponent::RuntimeCommand::None &&
                   instanceChild->boxCollider &&
                   instanceChild->characterController && instanceChild->scripts.size() == 3u &&
                   instanceTarget != nullptr &&
                   instanceTarget->entityValue == instanceRoot->id &&
                   instanceExternalTarget != nullptr &&
                   !instanceExternalTarget->entityValue.IsValid(),
               "Entity hierarchy instantiation lost components or internal references.")) {
        return 154;
    }
    World emptyTemplate;
    const size_t instanceCountBeforeFailure = instantiatedWorld.Entities().size();
    if (!Check(!instantiatedWorld.InstantiateEntityHierarchies(
                   emptyTemplate, {}, instanceRoots, &error) &&
                   instantiatedWorld.Entities().size() == instanceCountBeforeFailure &&
                   !instantiatedWorld.InstantiateEntityHierarchies(
                       restored, EntityId::New(), instanceRoots, &error) &&
                   instantiatedWorld.Entities().size() == instanceCountBeforeFailure,
               "Invalid entity hierarchy instantiation modified the destination World.")) {
        return 155;
    }

    const EntityId duplicateRoot = source.DuplicateEntityHierarchy(root);
    const WorldEntity* duplicateRootEntity = source.Find(duplicateRoot);
    const std::vector<EntityId> duplicateChildren = source.GetChildren(duplicateRoot);
    const WorldEntity* duplicateChild =
        duplicateChildren.size() == 1u ? source.Find(duplicateChildren.front()) : nullptr;
    if (!Check(duplicateRoot.IsValid() && duplicateRoot != root && duplicateRootEntity != nullptr &&
                   duplicateRootEntity->name == "Root Copy" && !duplicateRootEntity->parent.IsValid() &&
                   duplicateChild != nullptr && duplicateChild->id != child &&
                   duplicateChild->name == "Child" && duplicateChild->meshRenderer &&
                   duplicateChild->meshRenderer->primitive == MeshPrimitive::Sphere &&
                   duplicateChild->materialOverride &&
                   duplicateChild->materialOverride->metallic == 0.7f &&
                   duplicateChild->materialOverride->baseColorTexturePath ==
                       "asset://textures/test.png" &&
                   duplicateChild->materialOverride->normalTexturePath ==
                       "asset://textures/test_normal.png" &&
                   duplicateChild->materialOverride->normalStrength == 1.5f &&
                   duplicateChild->materialOverride->roughnessTexturePath ==
                       "asset://textures/test_orm.png" &&
                   duplicateChild->materialOverride->pbrTexturePacking ==
                       MaterialPbrTexturePacking::OcclusionRoughnessMetallic &&
                   duplicateChild->materialOverride->blendMode ==
                       MaterialSurfaceBlendMode::Transparent &&
                   duplicateChild->materialOverride->cullMode ==
                       MaterialSurfaceCullMode::None &&
                   duplicateChild->light && duplicateChild->light->type == LightType::Point &&
                   duplicateChild->animator && duplicateChild->animator->clip == "Run" &&
                   !duplicateChild->animator->lockRootPosition &&
                   duplicateChild->animator->runtimeCommand ==
                       AnimatorComponent::RuntimeCommand::None &&
                   duplicateChild->animator->runtimeRequestedClip.empty() &&
                   duplicateChild->animator->runtimeClip.empty() &&
                   duplicateChild->animator->runtimeFadeDuration == 0.0f &&
                   duplicateChild->animator->runtimeTime == 0.0f &&
                   duplicateChild->animator->runtimeDuration == 0.0f &&
                   duplicateChild->animator->runtimeNormalizedTime == 0.0f &&
                   !duplicateChild->animator->runtimeTransitioning &&
                   duplicateChild->animator->runtimeTransitionProgress == 0.0f &&
                   duplicateChild->scripts.size() == 3u &&
                   duplicateChild->scripts[0].type == "Rotator" &&
                   duplicateChild->scripts[0].scriptAssetPath ==
                       "asset://Scripts/Rotator.cpp" &&
                   duplicateChild->scripts[0].properties.size() == 7u &&
                   duplicateChild->scripts[0].properties[0].floatValue == 3.5f &&
                   duplicateChild->scripts[0].properties[1].entityValue == duplicateRoot &&
                   duplicateChild->scripts[0].properties[2].booleanValue &&
                   duplicateChild->scripts[0].properties[3].integerValue == 7 &&
                   duplicateChild->scripts[0].properties[4].vector3Value.z == 6.0f &&
                   duplicateChild->scripts[0].properties[5].stringValue == "Run" &&
                   duplicateChild->scripts[0].properties[6].type ==
                       ScriptPropertyType::AnimationClip &&
                   duplicateChild->scripts[0].properties[6].stringValue == "Attack" &&
                   duplicateChild->scripts[1].type == "FirstPersonController" &&
                   duplicateChild->scripts[2].type.empty() &&
                   duplicateChild->boxCollider &&
                   duplicateChild->boxCollider->center.x == 0.25f &&
                   duplicateChild->boxCollider->size.y == 2.0f &&
                   duplicateChild->characterController &&
                   duplicateChild->characterController->radius == 0.4f &&
                   duplicateChild->characterController->height == 1.8f &&
                   duplicateRootEntity->camera && !duplicateRootEntity->camera->primary &&
                   duplicateRootEntity->audioListener,
               "Hierarchy duplication did not preserve entity data and parenting.")) {
        return 8;
    }
    if (!Check(!source.DuplicateEntityHierarchy({}).IsValid(),
               "An invalid entity hierarchy was duplicated.")) {
        return 9;
    }

    World referenceWorld;
    const EntityId scriptOwner = referenceWorld.CreateEntity("Script Owner");
    const EntityId referencedTarget = referenceWorld.CreateEntity("Referenced Target");
    referenceWorld.Find(scriptOwner)->scripts.push_back(
        {true, "Configurable", "asset://Scripts/Configurable.cpp",
         {{"Target", ScriptPropertyType::Entity, 0.0f, referencedTarget}}});
    if (!Check(referenceWorld.DestroyEntity(referencedTarget) &&
                   !referenceWorld.Find(scriptOwner)
                        ->scripts[0]
                        .properties[0]
                        .entityValue.IsValid(),
               "Destroyed Entity remained assigned to a Script property.")) {
        return 148;
    }

    const std::filesystem::path testPath =
        std::filesystem::temp_directory_path() /
        ("likeengine-world-" + root.ToString() + ".likescene");
    World fileRestored;
    if (!Check(WorldSerializer::Save(source, testPath, &error), error.c_str()) ||
        !Check(WorldSerializer::Load(testPath, fileRestored, &error), error.c_str()) ||
        !Check(fileRestored.Find(child) != nullptr, "Scene file round-trip lost an entity.")) {
        std::error_code cleanupError;
        std::filesystem::remove(testPath, cleanupError);
        return 10;
    }
    std::error_code cleanupError;
    std::filesystem::remove(testPath, cleanupError);

    std::vector<WorldEntity> invalidEntities(1u);
    invalidEntities[0].id = EntityId::New();
    invalidEntities[0].parent = EntityId::New();
    if (!Check(!restored.ReplaceEntities(std::move(invalidEntities), &error),
               "Missing hierarchy parent was accepted.")) {
        return 11;
    }
    if (!Check(restored.Entities().size() == 2u,
               "Failed replacement modified the existing world.")) {
        return 12;
    }
    std::vector<WorldEntity> invalidLayerEntities(1u);
    invalidLayerEntities[0].id = EntityId::New();
    invalidLayerEntities[0].layer = 32u;
    if (!Check(!restored.ReplaceEntities(std::move(invalidLayerEntities), &error),
               "Invalid Entity Layer was accepted.")) {
        return 151;
    }
    std::vector<WorldEntity> invalidColliderEntities(1u);
    invalidColliderEntities[0].id = EntityId::New();
    invalidColliderEntities[0].boxCollider = BoxColliderComponent{};
    invalidColliderEntities[0].boxCollider->size.y = 0.0f;
    if (!Check(!restored.ReplaceEntities(std::move(invalidColliderEntities), &error),
               "Invalid direct BoxCollider replacement was accepted.")) {
        return 133;
    }
    std::vector<WorldEntity> invalidControllerEntities(1u);
    invalidControllerEntities[0].id = EntityId::New();
    invalidControllerEntities[0].characterController = CharacterControllerComponent{};
    invalidControllerEntities[0].characterController->height = 0.5f;
    if (!Check(!restored.ReplaceEntities(std::move(invalidControllerEntities), &error),
               "Invalid direct CharacterController replacement was accepted.")) {
        return 136;
    }

    if (!Check(restored.DestroyEntity(root) && restored.Empty(),
               "Recursive hierarchy deletion failed.")) {
        return 13;
    }

    const std::string invalidRenderer =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"MeshRenderer":{"enabled":true,"source":"Primitive","primitive":99,"model":""}}}]})";
    World rejected;
    if (!Check(!WorldSerializer::Deserialize(invalidRenderer, rejected, &error),
               "Invalid MeshRenderer data was accepted.")) {
        return 14;
    }
    const std::string invalidCamera =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Camera","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"Camera":{"enabled":true,"primary":true,"projection":"Perspective","fieldOfView":180,"orthographicHeight":10,"nearClip":1,"farClip":0.5}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidCamera, rejected, &error),
               "Invalid Camera data was accepted.")) {
        return 114;
    }
    const std::string invalidLight =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Light","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"Light":{"enabled":true,"type":"Spot","color":[1,1,1],"intensity":1,"range":10,"innerAngle":60,"outerAngle":30}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidLight, rejected, &error),
               "Invalid Light data was accepted.")) {
        return 115;
    }
    const std::string invalidAudioListener =
        R"({"version":1,"entities":[{"id":"00000000-0000-0001-0000-000000000001","parent":null,"name":"Bad Audio Listener","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"AudioListener":{"enabled":1}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidAudioListener, rejected, &error),
               "Invalid AudioListener data was accepted.")) {
        return 160;
    }
    const std::string legacyAudioSource =
        R"({"version":1,"entities":[{"id":"00000000-0000-0001-0000-000000000001","parent":null,"name":"Legacy Audio Source","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"AudioSource":{"enabled":true,"clip":"","playOnAwake":true,"loop":false,"volume":1,"spatial":false,"minDistance":1,"maxDistance":50}}}]})";
    World legacyAudioSourceWorld;
    if (!Check(WorldSerializer::Deserialize(legacyAudioSource, legacyAudioSourceWorld, &error) &&
                   legacyAudioSourceWorld.Entities().front().audioSource &&
                   legacyAudioSourceWorld.Entities().front().audioSource->pitch == 1.0f,
               "Legacy AudioSource data did not receive the default Pitch.")) {
        return 161;
    }
    const std::string invalidAudioSourcePitch =
        R"({"version":1,"entities":[{"id":"00000000-0000-0001-0000-000000000001","parent":null,"name":"Bad Audio Source","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"AudioSource":{"enabled":true,"clip":"","playOnAwake":true,"loop":false,"volume":1,"pitch":0,"spatial":false,"minDistance":1,"maxDistance":50}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidAudioSourcePitch, rejected, &error),
               "Invalid AudioSource Pitch was accepted.")) {
        return 162;
    }
    const std::string legacyBehavior =
        R"({"version":1,"entities":[{"id":"00000000-0000-0001-0000-000000000001","parent":null,"name":"Legacy Behavior","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"Behavior":{"enabled":true,"type":"Rotator"}}}]})";
    World legacyBehaviorWorld;
    if (!Check(WorldSerializer::Deserialize(legacyBehavior, legacyBehaviorWorld, &error) &&
                   legacyBehaviorWorld.Entities().size() == 1u &&
                   legacyBehaviorWorld.Entities().front().scripts.size() == 1u &&
                   legacyBehaviorWorld.Entities().front().scripts.front().type == "Rotator",
               "Legacy Behavior data was not migrated to a Script component.")) {
        return 127;
    }
    const std::string invalidBehavior =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Behavior","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"Behavior":{"enabled":true,"type":""}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidBehavior, rejected, &error),
               "Invalid Behavior data was accepted.")) {
        return 128;
    }
    const std::string invalidScriptProperty =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Script Property","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"Scripts":[{"enabled":true,"type":"Configurable","script":"asset://Scripts/Configurable.cpp","properties":{"Speed":{"type":"Number","value":2.5}}}]}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidScriptProperty, rejected, &error),
               "Invalid Script property data was accepted.")) {
        return 149;
    }
    const std::string invalidBooleanProperty =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Boolean","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"Scripts":[{"enabled":true,"type":"Configurable","properties":{"Aggressive":{"type":"Boolean","value":1}}}]}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidBooleanProperty, rejected, &error),
               "Invalid Script Boolean property was accepted.")) {
        return 150;
    }
    const std::string invalidIntegerProperty =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Integer","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"Scripts":[{"enabled":true,"type":"Configurable","properties":{"Lives":{"type":"Integer","value":1.5}}}]}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidIntegerProperty, rejected, &error),
               "Invalid Script Integer property was accepted.")) {
        return 151;
    }
    const std::string invalidVectorProperty =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Vector","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"Scripts":[{"enabled":true,"type":"Configurable","properties":{"Offset":{"type":"Vector3","value":[1,2]}}}]}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidVectorProperty, rejected, &error),
               "Invalid Script Vector3 property was accepted.")) {
        return 152;
    }
    const std::string invalidBoxCollider =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Collider","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"BoxCollider":{"enabled":true,"center":[0,0,0],"size":[1,0,1],"isTrigger":false}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidBoxCollider, rejected, &error),
               "Invalid BoxCollider data was accepted.")) {
        return 129;
    }
    const std::string invalidCharacterController =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Controller","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"CharacterController":{"enabled":true,"center":[0,0,0],"radius":1,"height":1,"slopeLimit":45,"stepOffset":0.3,"skinWidth":0.05,"minMoveDistance":0}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidCharacterController, rejected, &error),
               "Invalid CharacterController data was accepted.")) {
        return 137;
    }
    const std::string invalidMaterial =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Material","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"MaterialOverride":{"enabled":true,"baseColor":[1,1,1,2],"metallic":-1,"roughness":0.5}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidMaterial, rejected, &error),
               "Invalid MaterialOverride data was accepted.")) {
        return 118;
    }
    const std::string invalidNormalMaterial =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Normal","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"MaterialOverride":{"enabled":true,"baseColor":[1,1,1,1],"metallic":0,"roughness":0.5,"normalTexture":"asset://normal.png","normalStrength":-1}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidNormalMaterial, rejected, &error),
               "Invalid Normal texture settings were accepted.")) {
        return 120;
    }
    const std::string invalidPbrPacking =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Packing","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"MaterialOverride":{"enabled":true,"baseColor":[1,1,1,1],"metallic":0,"roughness":0.5,"pbrTexturePacking":"Unknown"}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidPbrPacking, rejected, &error),
               "Invalid PBR texture packing was accepted.")) {
        return 121;
    }
    const std::string invalidMaterialSurface =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Surface","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"MaterialOverride":{"enabled":true,"baseColor":[1,1,1,1],"metallic":0,"roughness":0.5,"blendMode":"Additive","alphaCutoff":2,"cullMode":"Sideways","depthWrite":true}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidMaterialSurface, rejected, &error),
               "Invalid Material surface settings were accepted.")) {
        return 122;
    }
    const std::filesystem::path projectAssets =
        std::filesystem::temp_directory_path() / L"engine-external-project" / L"assets";
    AssetManager::SetProjectAssetRoot(projectAssets);
    const std::filesystem::path resolvedAsset =
        AssetManager::ResolvePathStrict(L"asset://models/brain_stem/BrainStem.glb");
    if (!Check(resolvedAsset ==
                   (projectAssets / L"models" / L"brain_stem" / L"BrainStem.glb")
                       .lexically_normal(),
               "asset:// URI did not resolve against the project asset root.")) {
        return 15;
    }
    if (!Check(AssetManager::ResolvePathStrict(L"asset://../outside.glb").empty(),
               "Project asset traversal was accepted.")) {
        return 16;
    }
    AssetManager::SetEngineResourceRoot(projectAssets / L"engine-resources");
    if (!Check(AssetManager::ResolvePath(L"engine://../outside.hlsl").empty(),
               "Engine resource traversal was accepted.")) {
        return 17;
    }

    const std::filesystem::path projectDirectory =
        std::filesystem::temp_directory_path() / ("engine-project-" + root.ToString());
    std::error_code projectFilesystemError;
    std::filesystem::remove_all(projectDirectory, projectFilesystemError);
    projectFilesystemError.clear();
    if (!Check(std::filesystem::create_directory(projectDirectory, projectFilesystemError) &&
                   !projectFilesystemError,
               "Project test directory could not be created.")) {
        return 18;
    }
    ProjectDescriptor createdProject;
    if (!Check(ProjectDescriptor::Create(projectDirectory, "Created Project", createdProject,
                                         error),
               error.c_str()) ||
        !Check(createdProject.name == "Created Project" &&
                   createdProject.assetRoot == projectDirectory / L"assets" &&
                   createdProject.sceneRoot == projectDirectory / L"scenes" &&
                   std::filesystem::is_regular_file(createdProject.manifestPath) &&
                   std::filesystem::is_regular_file(projectDirectory / L".gitignore") &&
                   std::filesystem::is_directory(createdProject.assetRoot) &&
                   std::filesystem::is_directory(createdProject.sceneRoot),
               "Created project structure is invalid.")) {
        std::filesystem::remove_all(projectDirectory, projectFilesystemError);
        return 19;
    }
    ProjectDescriptor duplicateProject;
    if (!Check(!ProjectDescriptor::Create(projectDirectory, "Duplicate", duplicateProject, error),
               "Project creation accepted a non-empty directory.")) {
        std::filesystem::remove_all(projectDirectory, projectFilesystemError);
        return 20;
    }
    std::ifstream ignoreStream(projectDirectory / L".gitignore");
    const std::string ignoreContents{std::istreambuf_iterator<char>(ignoreStream),
                                     std::istreambuf_iterator<char>()};
    ignoreStream.close();
    const std::filesystem::path scriptDirectory =
        createdProject.assetRoot / L"Scripts";
    std::filesystem::create_directories(scriptDirectory, projectFilesystemError);
    {
        std::ofstream scriptHeader(scriptDirectory / L"FingerprintTest.h",
                                   std::ios::trunc);
        scriptHeader << "// first version\n";
    }
    uint64_t firstScriptFingerprint = 0u;
    uint64_t repeatedScriptFingerprint = 0u;
    uint64_t changedScriptFingerprint = 0u;
    std::string fingerprintError;
    const bool initialFingerprintValid =
        !projectFilesystemError &&
        ScriptBuildService::GetSourceFingerprint(
            createdProject.root, firstScriptFingerprint, fingerprintError) &&
        ScriptBuildService::GetSourceFingerprint(
            createdProject.root, repeatedScriptFingerprint, fingerprintError);
    {
        std::ofstream scriptHeader(scriptDirectory / L"FingerprintTest.h",
                                   std::ios::trunc);
        scriptHeader << "// second version\n";
    }
    if (!Check(ignoreContents == "/library/\n" && initialFingerprintValid &&
                   firstScriptFingerprint == repeatedScriptFingerprint &&
                   ScriptBuildService::GetSourceFingerprint(
                       createdProject.root, changedScriptFingerprint, fingerprintError) &&
                   changedScriptFingerprint != firstScriptFingerprint,
               fingerprintError.empty() ?
                   "Project Script fingerprint or library ignore path is invalid." :
                   fingerprintError.c_str())) {
        std::filesystem::remove_all(projectDirectory, projectFilesystemError);
        return 142;
    }
    if (!Check(WorldSerializer::Save(source, createdProject.startupScene, &error), error.c_str())) {
        std::filesystem::remove_all(projectDirectory, projectFilesystemError);
        return 21;
    }
    const std::filesystem::path recentSettings = projectDirectory / L"settings" / L"recent.json";
    RecentScenesStore recentScenes(recentSettings, createdProject.sceneRoot);
    if (!Check(recentScenes.Save({createdProject.startupScene,
                                  projectDirectory.parent_path() / L"outside.likescene"}),
               "Recent scene settings could not be saved.")) {
        std::filesystem::remove_all(projectDirectory, projectFilesystemError);
        return 22;
    }
    const std::vector<std::filesystem::path> restoredScenes = recentScenes.Load();
    if (!Check(restoredScenes.size() == 1u &&
                   restoredScenes.front() ==
                       std::filesystem::weakly_canonical(createdProject.startupScene),
               "Recent scenes were not safely restored.")) {
        std::filesystem::remove_all(projectDirectory, projectFilesystemError);
        return 23;
    }
    const std::filesystem::path physicsSettingsPath =
        projectDirectory / L"settings" / L"physics.json";
    PhysicsSettingsStore physicsStore(physicsSettingsPath);
    PhysicsSettings physicsSettings{};
    std::string physicsSettingsError;
    const bool defaultPhysicsSettingsLoaded =
        physicsStore.Load(physicsSettings, physicsSettingsError);
    physicsSettings.layerNames[1] = "Player";
    physicsSettings.layerNames[2] = "Enemy";
    physicsSettings.SetLayersCollide(1u, 2u, false);
    const bool physicsSettingsSaved =
        defaultPhysicsSettingsLoaded && physicsStore.Save(physicsSettings, physicsSettingsError);
    PhysicsSettings restoredPhysicsSettings{};
    if (!Check(physicsSettingsSaved &&
                   physicsStore.Load(restoredPhysicsSettings, physicsSettingsError) &&
                   restoredPhysicsSettings.layerNames[0] == "Default" &&
                   restoredPhysicsSettings.layerNames[1] == "Player" &&
                   restoredPhysicsSettings.layerNames[2] == "Enemy" &&
                   !restoredPhysicsSettings.LayersCollide(1u, 2u) &&
                   !restoredPhysicsSettings.LayersCollide(2u, 1u) &&
                   restoredPhysicsSettings.LayersCollide(0u, 1u) &&
                   std::filesystem::is_regular_file(physicsSettingsPath),
               physicsSettingsError.empty() ?
                   "Physics settings were not safely persisted." :
                   physicsSettingsError.c_str())) {
        std::filesystem::remove_all(projectDirectory, projectFilesystemError);
        return 143;
    }
    std::filesystem::remove_all(projectDirectory, projectFilesystemError);
    if (!Check(!projectFilesystemError, "Project test directory cleanup failed.")) {
        return 24;
    }
    World ordered;
    const EntityId first = ordered.CreateEntity("First");
    const EntityId second = ordered.CreateEntity("Second");
    const EntityId third = ordered.CreateEntity("Third");
    if (!Check(ordered.MoveEntityBefore(third, first) &&
                   ordered.GetRootEntities() == std::vector<EntityId>{third, first, second},
               "Moving a root entity before its sibling failed.")) {
        return 25;
    }
    if (!Check(ordered.MoveEntityAfter(third, second) &&
                   ordered.GetRootEntities() == std::vector<EntityId>{first, second, third},
               "Moving a root entity after its sibling failed.")) {
        return 26;
    }
    const EntityId firstChild = ordered.CreateEntity("First Child");
    const EntityId secondChild = ordered.CreateEntity("Second Child");
    if (!Check(ordered.SetParent(firstChild, first) && ordered.SetParent(secondChild, first) &&
                   ordered.MoveEntityBefore(secondChild, firstChild) &&
                   ordered.GetChildren(first) ==
                       std::vector<EntityId>{secondChild, firstChild},
               "Moving a child entity before its sibling failed.")) {
        return 27;
    }
    if (!Check(!ordered.MoveEntityBefore(firstChild, second),
               "Entity ordering accepted a sibling from another parent.")) {
        return 28;
    }
    const EntityId fourth = ordered.CreateEntity("Fourth");
    ordered.MoveEntityBefore(first, fourth);
    ordered.MoveEntityBefore(second, fourth);
    if (!Check(ordered.GetRootEntities() ==
                   std::vector<EntityId>{third, first, second, fourth},
               "Moving multiple siblings before a target did not preserve their order.")) {
        return 29;
    }
    ordered.MoveEntityAfter(second, third);
    ordered.MoveEntityAfter(first, third);
    if (!Check(ordered.GetRootEntities() ==
                   std::vector<EntityId>{third, first, second, fourth},
               "Moving multiple siblings after a target did not preserve their order.")) {
        return 30;
    }

    const std::filesystem::path importDirectory =
        std::filesystem::temp_directory_path() / ("asset-import-" + root.ToString());
    std::error_code importFilesystemError;
    std::filesystem::remove_all(importDirectory, importFilesystemError);
    importFilesystemError.clear();
    std::filesystem::create_directories(importDirectory / "gltf/data", importFilesystemError);
    std::filesystem::create_directories(importDirectory / "gltf/textures",
                                        importFilesystemError);
    std::filesystem::create_directories(importDirectory / "obj/materials",
                                        importFilesystemError);
    std::filesystem::create_directories(importDirectory / "obj/textures",
                                        importFilesystemError);
    const auto writeFile = [](const std::filesystem::path& path, std::string_view contents) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return static_cast<bool>(stream);
    };
    const bool importFilesCreated = !importFilesystemError &&
        writeFile(importDirectory / "gltf/model.gltf",
                  R"({"asset":{"version":"2.0"},"buffers":[{"uri":"data/model.bin"}],"images":[{"uri":"textures/albedo.png"}]})") &&
        writeFile(importDirectory / "gltf/data/model.bin", "mesh-data") &&
        writeFile(importDirectory / "gltf/textures/albedo.png", "image-data") &&
        writeFile(importDirectory / "obj/model.obj", "mtllib materials/model.mtl\n") &&
        writeFile(importDirectory / "obj/materials/model.mtl",
                  "map_Kd -s 1 1 1 ../textures/diffuse\\ image.png\n"
                  "bump ../textures/normal.png\n") &&
        writeFile(importDirectory / "obj/textures/diffuse image.png", "diffuse") &&
        writeFile(importDirectory / "obj/textures/normal.png", "normal") &&
        writeFile(importDirectory / "preview.wav", "audio-data");
    if (!Check(importFilesCreated, "Asset import test files could not be created.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 31;
    }

    std::vector<AssetImport::File> importPlan;
    std::string importError;
    if (!Check(AssetImport::IsTextureFile(importDirectory / "gltf/textures/albedo.png") &&
                   AssetImport::BuildPlan(
                       {importDirectory / "gltf/textures/albedo.png"}, importPlan,
                       importError) &&
                   importPlan.size() == 1u,
               "Standalone texture import plan failed.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 119;
    }
    if (!Check(AssetImport::IsAudioFile(importDirectory / "preview.wav") &&
                   AssetImport::BuildPlan({importDirectory / "preview.wav"}, importPlan,
                                          importError) &&
                   importPlan.size() == 1u,
               "Standalone audio import plan failed.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 158;
    }
    const bool gltfPlanBuilt = AssetImport::BuildPlan(
        {importDirectory / "gltf/model.gltf"}, importPlan, importError);
    if (!Check(gltfPlanBuilt, importError.c_str()) ||
        !Check(importPlan.size() == 3u,
               "glTF import did not collect its buffer and image dependencies.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 32;
    }
    const bool objPlanBuilt = AssetImport::BuildPlan(
        {importDirectory / "obj/model.obj"}, importPlan, importError);
    if (!Check(objPlanBuilt, importError.c_str()) ||
        !Check(importPlan.size() == 4u,
               "OBJ import did not collect its MTL and texture dependencies.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 33;
    }
    std::filesystem::remove(importDirectory / "obj/textures/normal.png",
                            importFilesystemError);
    if (!Check(!AssetImport::BuildPlan({importDirectory / "obj/model.obj"}, importPlan,
                                       importError) &&
                   importError.find("Missing OBJ dependency") != std::string::npos,
               "OBJ import accepted a missing texture dependency.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 34;
    }
    if (!Check(writeFile(importDirectory / "outside.png", "outside") &&
                   writeFile(importDirectory / "obj/materials/model.mtl",
                             "map_Kd ../../outside.png\n") &&
                   !AssetImport::BuildPlan({importDirectory / "obj/model.obj"}, importPlan,
                                           importError) &&
                   importError.find("escapes its source folder") != std::string::npos,
               "OBJ import accepted a dependency outside the OBJ folder.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 35;
    }
    if (!Check(AssetImport::HaveEqualContents(importDirectory / "gltf/data/model.bin",
                                              importDirectory / "gltf/data/model.bin") &&
                   !AssetImport::HaveEqualContents(importDirectory / "gltf/data/model.bin",
                                                   importDirectory / "gltf/textures/albedo.png"),
               "Asset import content comparison is incorrect.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 36;
    }
    std::filesystem::remove_all(importDirectory, importFilesystemError);
    if (!Check(!importFilesystemError, "Asset import test cleanup failed.")) {
        return 37;
    }
    return 0;
}
