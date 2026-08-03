#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <string>

namespace microide::workspace {

enum class StatusBarSegmentId : std::uint8_t {
  Project = 0,
  Branch,
  Language,
  Indent,
  Encoding,
  LineColumn,
  Problems,
  Lsp,
  LayoutMode,
  Count,
};

// Semantic severity/state of a segment, derived where the value is known
// (StatusBarModelService) so the render path never re-parses display text to
// pick a color.
enum class StatusBarSegmentTone : std::uint8_t {
  Default = 0,
  Info,
  Warning,
  Error,
};

struct StatusBarSegmentValue {
  std::string text;
  std::string tooltip;
  // Command run when the segment is clicked, empty for a read-only segment.
  // `clickable` is derived from it: the two used to be independent, and every
  // segment shipped with clickable=false and no command at all, so the bar showed
  // imperative tooltips ("Go to Line", "Open Problems") for controls that did
  // nothing. Keeping the flag derived means an actionable-looking segment and an
  // actual action cannot drift apart again.
  std::string command;
  std::string command_arg;
  bool visible = false;
  StatusBarSegmentTone tone = StatusBarSegmentTone::Default;

  bool clickable() const { return !command.empty(); }
};

class StatusBarService {
 public:
  StatusBarService() = default;

  void SetSegment(StatusBarSegmentId id, StatusBarSegmentValue value) {
    StatusBarSegmentValue& slot = segments_[static_cast<std::size_t>(id)];
    // Only the painted aspects mark the bar dirty. Tooltip text and clickability
    // are hover/click state, not pixels, so a change to either must not schedule
    // a repaint.
    if (slot.text != value.text || slot.visible != value.visible || slot.tone != value.tone) {
      painted_state_changed_ = true;
    }
    slot = std::move(value);
  }
  const StatusBarSegmentValue& Segment(StatusBarSegmentId id) const {
    return segments_[static_cast<std::size_t>(id)];
  }

  // True once since the last call if any segment's *painted* value changed.
  //
  // The status bar is the one shell surface no Request*Redraw helper covers: its
  // content is derived from state that lives elsewhere (caret position, language,
  // indent, encoding, git, LSP, diagnostics), so the event that changes it asks
  // for the editor — or the sidebar, or nothing — to be repainted and the strip
  // keeps its old pixels. On a partial-redraw frame that meant moving the caret
  // never updated "Ln 1, Col 1", and opening a second file left the first one's
  // language and indent on screen. Refreshing the model already runs once per
  // frame; this is how the frame learns it has to repaint the strip.
  bool TakePaintedStateChanged() {
    const bool changed = painted_state_changed_;
    painted_state_changed_ = false;
    return changed;
  }

  const std::array<StatusBarSegmentValue, static_cast<std::size_t>(StatusBarSegmentId::Count)>&
  Snapshot() const {
    return segments_;
  }

 private:
  std::array<StatusBarSegmentValue, static_cast<std::size_t>(StatusBarSegmentId::Count)>
      segments_{};
  bool painted_state_changed_ = false;
};

}  // namespace microide::workspace
