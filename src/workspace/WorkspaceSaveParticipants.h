#pragma once

#include "util/JsonValue.h"
#include "workspace/ProviderRegistry.h"

#include <functional>
#include <string>

namespace microide::workspace {

struct SaveParticipantSpec {
  std::string id;
  std::string plugin_id;
};

using SaveParticipantRegistry = ProviderRegistry<SaveParticipantSpec>;

}
