#pragma once

#include "util/JsonValue.h"

#include <functional>
#include <string>
#include <vector>

namespace microide::workspace {

// Save participant: Lua callback on buffer save, can modify text or return edit.
struct SaveParticipantSpec {
  std::string id;
  std::string plugin_id;
  // Lua function ref (as string id) stored in plugin context.
};

// Registry for save participants (Lua-driven).
class SaveParticipantRegistry {
 public:
  SaveParticipantRegistry();
  ~SaveParticipantRegistry();

  void Register(const SaveParticipantSpec& spec);
  const std::vector<SaveParticipantSpec>& Specs() const { return specs_; }

 private:
  std::vector<SaveParticipantSpec> specs_;
};

}  // namespace microide::workspace
