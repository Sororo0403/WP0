#pragma once

#include "runtime/BehaviorRegistry.h"

#include <cstddef>
#include <cstdint>

class Input;

inline constexpr uint32_t kScriptModuleApiVersion = 2u;
inline constexpr char kScriptModuleVersionExport[] =
    "LikeEngineScriptModuleApiVersion";
inline constexpr char kScriptModuleTypesExport[] = "LikeEngineGetScriptTypes";

using ScriptBehaviorFactory = Behavior* (*)(Input* input);

struct ScriptTypeRegistration {
    const char* type = nullptr;
    const char* sourceAsset = nullptr;
    ScriptBehaviorFactory create = nullptr;
    BehaviorRequirements requirements{};
};

using GetScriptModuleApiVersion = uint32_t (*)();
using GetScriptTypeRegistrations = const ScriptTypeRegistration* (*)(size_t* count);
