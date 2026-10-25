#pragma once

#include <cstdint>

namespace microide::workspace {

// Semantic severity of a status-bar segment or a plugin status item, derived
// where the value is known so the render path never re-parses display text (or a
// contributed "tone" string) to pick a colour. One vocabulary for both: they
// were two value-for-value copies, each with its own tone-to-colour switch.
enum class StatusTone : std::uint8_t {
  Default = 0,
  Info,
  Warning,
  Error,
};

}  // namespace microide::workspace
