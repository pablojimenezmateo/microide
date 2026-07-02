#pragma once

#include "util/JsonValue.h"
#include "workspace/ProviderRegistry.h"

#include <functional>
#include <string>

namespace microide::workspace {

// Save participant: Lua callback on buffer save, can modify text or return edit.
struct SaveParticipantSpec {
  std::string id;
  std::string plugin_id;
  // Lua function ref (as string id) stored in plugin context.
};

using SaveParticipantRegistry = ProviderRegistry<SaveParticipantSpec>;

}  // namespace microide::workspace
