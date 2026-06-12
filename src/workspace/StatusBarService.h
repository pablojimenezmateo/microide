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
  bool clickable = false;
  bool visible = false;
  StatusBarSegmentTone tone = StatusBarSegmentTone::Default;
};

class StatusBarService {
 public:
  StatusBarService() = default;

  void SetSegment(StatusBarSegmentId id, StatusBarSegmentValue value) {
    segments_[static_cast<std::size_t>(id)] = std::move(value);
  }
  const StatusBarSegmentValue& Segment(StatusBarSegmentId id) const {
    return segments_[static_cast<std::size_t>(id)];
  }

  const std::array<StatusBarSegmentValue, static_cast<std::size_t>(StatusBarSegmentId::Count)>&
  Snapshot() const {
    return segments_;
  }

 private:
  std::array<StatusBarSegmentValue, static_cast<std::size_t>(StatusBarSegmentId::Count)>
      segments_{};
};

}  // namespace microide::workspace
