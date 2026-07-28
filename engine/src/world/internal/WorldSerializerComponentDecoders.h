#pragma once

#include "WorldSerializerJson.h"

namespace WorldSerializerDecoding {
bool DecodeRenderingComponents(const WorldSerializerJson::Json& encoded,
                               WorldEntity& entity, std::string* error);
bool DecodeRuntimeComponents(const WorldSerializerJson::Json& encoded,
                             WorldEntity& entity, std::string* error);
bool DecodeUiComponents(const WorldSerializerJson::Json& encoded,
                        WorldEntity& entity, std::string* error);
bool DecodeScriptsComponent(const WorldSerializerJson::Json& components,
                            WorldEntity& entity, std::string* error);
bool DecodePhysicsComponents(const WorldSerializerJson::Json& encoded,
                             WorldEntity& entity, std::string* error);
} // namespace WorldSerializerDecoding
