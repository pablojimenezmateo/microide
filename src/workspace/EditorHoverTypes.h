#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>
#include <string>

#include "editor/DiagnosticsStore.h"
#include "plugin/PluginHost.h"

namespace microide::workspace {

// Debug hover-to-inspect popup payload (Phase 5): the evaluated value + its type
// for the identifier under the cursor while a session is Stopped. Strings are
// prebuilt by DebugService (off the render path).
struct DebugHoverValue {
  std::string value;
  std::string type;
  bool operator==(const DebugHoverValue&) const = default;
};

// What the pointer rests on in an editor pane that has a hover card: a line's
// inline blame, a diagnostic, a plugin hover or a debug value.
struct EditorHoverTarget {
  enum class Kind {
    Blame,
    Diagnostic,
    Plugin,
    DebugValue,
  };

  Kind kind = Kind::Blame;
  SDL_FRect anchor_rect{};
  std::size_t blame_line_index = 0;
  std::optional<editor::PublishedDiagnostic> diagnostic;
  std::optional<plugin::PluginHost::HoverResult> plugin_hover;
  std::optional<DebugHoverValue> debug_value;

  bool operator==(const EditorHoverTarget& other) const {
    return kind == other.kind && anchor_rect.x == other.anchor_rect.x &&
           anchor_rect.y == other.anchor_rect.y && anchor_rect.w == other.anchor_rect.w &&
           anchor_rect.h == other.anchor_rect.h &&
           blame_line_index == other.blame_line_index && diagnostic == other.diagnostic &&
           plugin_hover == other.plugin_hover && debug_value == other.debug_value;
  }
};

// The hover card for a target, laid out: where it goes, what it shows and where
// its button is. Frame prep lays it out once per frame (PreparedFrameState) and
// paint draws that one.
struct EditorHoverPopupLayout {
  EditorHoverTarget::Kind kind = EditorHoverTarget::Kind::Blame;
  SDL_FRect anchor_rect{};
  SDL_FRect rect{};
  std::size_t blame_line_index = 0;
  std::optional<editor::PublishedDiagnostic> diagnostic;
  std::optional<plugin::PluginHost::HoverResult> plugin_hover;
  std::optional<DebugHoverValue> debug_value;
  std::optional<SDL_FRect> primary_action_rect;
};

}  // namespace microide::workspace
