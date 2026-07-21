#pragma once

#include "collision/OBB.h"
#include "world/EntityId.h"

class World;

[[nodiscard]] bool TryBuildWorldBoxCollider(const World& world, EntityId entity,
                                            OBB& result);
