#pragma once

#include "world/World.h"

#include <string>
#include <vector>

namespace WorldEntityValidation {
bool PrepareAndValidate(std::vector<WorldEntity>& entities, std::string& error);
}
